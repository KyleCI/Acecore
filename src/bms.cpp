#include "bms.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"
#include "jbd_bms.h"
#include "log.h"
#include "pins.h"

namespace bms {
namespace {
HardwareSerial &uart = Serial2;
SemaphoreHandle_t mx = nullptr;
pc::jk::Data latest;
bool have = false;
uint32_t at = 0, lastFrameAt = 0;
Stats st;
pc::jk::Assembler asmb;
volatile bool paused = false, idle = false;

#if POWERCELL_DSP011
void publishJbd(const powercell::jbd::Reading& r) {
  pc::jk::Data d;
  d.cellCount = r.cellCount;
  for (uint8_t i = 0; i < r.cellCount && i < pc::jk::kMaxCells; ++i)
    d.cellMv[i] = static_cast<uint16_t>(r.cellVoltage[i] * 1000.f + 0.5f);
  d.cellAvgMv = static_cast<uint16_t>(r.avgCellV * 1000.f + 0.5f);
  d.cellDiffMv = static_cast<uint16_t>(r.cellDiffMv + 0.5f);
  d.cellMax = static_cast<uint8_t>(r.maxCell + 1);
  d.cellMin = static_cast<uint8_t>(r.minCell + 1);
  d.packMv = static_cast<uint32_t>(r.packVoltage * 1000.f + 0.5f);
  d.currentMa = static_cast<int32_t>(r.packCurrent * 1000.f);
  d.powerMw = static_cast<int32_t>(r.packPower * 1000.f);
  if (r.hasTemp1) d.tempC[0] = r.tempBat1;
  if (r.hasTemp2) d.tempC[1] = r.tempBat2;
  d.alarms = r.alarmBits;
  d.status = r.batStatus;
  d.soc = r.soc;
  d.soh = r.soh;
  d.remainMah = static_cast<int32_t>(r.remainAh * 1000.f);
  d.fullMah = static_cast<uint32_t>(r.fullAh * 1000.f);
  d.cycles = r.cycles;
  d.chargeMos = r.chargeMos;
  d.dischargeMos = r.dischargeMos;
  d.balancing = r.balancing;
  xSemaphoreTake(mx, portMAX_DELAY);
  latest = d;
  have = true;
  at = millis();
  ++st.ok;
  ++st.frames;
  xSemaphoreGive(mx);
}

bool readReg(uint8_t reg, uint8_t* buf, size_t cap, const uint8_t** data, uint8_t* dlen) {
  uint8_t req[7];
  powercell::jbd::buildRead(reg, req);
  while (uart.available()) uart.read();
  uart.write(req, sizeof req);
  const uint32_t start = millis();
  auto rx = [&](uint8_t& b) -> bool {
    while (millis() - start < 300) {
      if (uart.available()) { b = static_cast<uint8_t>(uart.read()); return true; }
      vTaskDelay(1);
    }
    return false;
  };
  uint8_t b = 0;
  do { if (!rx(b)) return false; } while (b != powercell::jbd::kStart);
  if (cap < 7) return false;
  buf[0] = b;
  for (int i = 1; i < 4; ++i) if (!rx(buf[i])) return false;
  const uint8_t n = buf[3];
  if (static_cast<size_t>(n) + 7 > cap) return false;
  for (size_t i = 4; i < static_cast<size_t>(n) + 7; ++i) if (!rx(buf[i])) return false;
  return powercell::jbd::checkFrame(buf, static_cast<size_t>(n) + 7, reg, data, dlen);
}

void task(void*) {
  uint8_t buf[88];
  for (;;) {
    const uint8_t* data = nullptr;
    uint8_t dlen = 0;
    powercell::jbd::Reading reading;
    bool ok = readReg(powercell::jbd::kRegBasic, buf, sizeof buf, &data, &dlen) &&
              powercell::jbd::parseBasic(data, dlen, &reading);
    if (ok) {
      if (readReg(powercell::jbd::kRegCells, buf, sizeof buf, &data, &dlen))
        powercell::jbd::parseCells(data, dlen, &reading);
      publishJbd(reading);
    } else {
      xSemaphoreTake(mx, portMAX_DELAY);
      ++st.timeouts;
      xSemaphoreGive(mx);
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}
#else
void task(void *) {
  for (;;) {
    if (paused) { idle = true; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    idle = false;
    while (!paused && uart.available()) {
      if (!asmb.feed(uint8_t(uart.read()), millis())) continue;
      pc::jk::Data d;
      pc::jk::Parse r = pc::jk::parseDynamic(asmb.buf(), pc::jk::kFrameLen, d);
      xSemaphoreTake(mx, portMAX_DELAY);
      ++st.frames;
      lastFrameAt = millis();
      st.timeouts = asmb.timeouts();
      if (r == pc::jk::Parse::Ok) { latest = d; have = true; at = lastFrameAt; ++st.ok; }
      else if (r == pc::jk::Parse::NotDynamic) ++st.setupFrames;
      else if (r == pc::jk::Parse::ChecksumError) ++st.checksumErrors;
      else ++st.badFrames;
      uint32_t bad = st.checksumErrors + st.badFrames;
      xSemaphoreGive(mx);
      if (r == pc::jk::Parse::ChecksumError || r == pc::jk::Parse::BadFrame)
        if (bad % 20 == 1) LOGW("BMS", "FRAME", "%s (bad=%lu)", pc::jk::parseName(r), (unsigned long)bad);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
#endif
}  // namespace

void begin() {
  mx = xSemaphoreCreateMutex();
  uart.setRxBufferSize(1024);
#if POWERCELL_DSP011
  uart.begin(PIN_BMS_BAUD, SERIAL_8N1, PIN_BMS_RX, PIN_BMS_TX);
  xTaskCreatePinnedToCore(task, "bms", 4096, nullptr, 1, nullptr, 1);
  LOGI("BMS", "INIT", "JBD poll %lu 8N1 rx=%d tx=%d swap=%d", (unsigned long)PIN_BMS_BAUD, PIN_BMS_RX, PIN_BMS_TX, DSP_BMS_UART_SWAP);
#else
  uart.begin(JK_BMS_BAUD, SERIAL_8N1, PIN_BMS_RX, PIN_BMS_TX);
  xTaskCreatePinnedToCore(task, "bms", 4096, nullptr, 1, nullptr, 1);
  LOGI("BMS", "INIT", "JK-PB frames %lu 8N1 rx=%d tx=%d", (unsigned long)JK_BMS_BAUD, PIN_BMS_RX, PIN_BMS_TX);
#endif
}

bool sample(pc::jk::Data &out, uint32_t *ageMs, uint32_t maxAgeMs) {
  if (!mx) return false;
  xSemaphoreTake(mx, portMAX_DELAY);
  uint32_t age = millis() - at;
  bool ok = have && age <= maxAgeMs;
  if (ok) out = latest;
  xSemaphoreGive(mx);
  if (ok && ageMs) *ageMs = age;
  return ok;
}

bool read(pc::jk::Data &out, uint32_t *ageMs) {
  if (!mx) return false;
  xSemaphoreTake(mx, portMAX_DELAY);
  uint32_t age = millis() - at;
  bool ok = have && age < JK_BMS_STALE_MS;
  if (ok) out = latest;
  xSemaphoreGive(mx);
  if (ok && ageMs) *ageMs = age;
  return ok;
}

// TEMPORARY debug: pause the parser task and hex-dump up to n raw bytes from Serial2.
size_t rawDump(Print &out, size_t n, uint32_t timeoutMs) {
  paused = true;
  uint32_t t0 = millis();
  while (!idle && millis() - t0 < 100) delay(1);
  size_t got = 0;
  t0 = millis();
  while (got < n && millis() - t0 < timeoutMs) {
    if (!uart.available()) { delay(1); continue; }
    out.printf("%02X%c", uart.read(), (++got % 16) ? ' ' : '\n');
  }
  if (got % 16) out.println();
  paused = false;
  return got;
}

bool online() { pc::jk::Data d; return read(d); }

Stats stats() {
  Stats s;
  if (!mx) return s;
  xSemaphoreTake(mx, portMAX_DELAY); s = st; xSemaphoreGive(mx);
  return s;
}
}  // namespace bms
