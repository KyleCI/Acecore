#include "bms.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"
#if !POWERCELL_BMS_JKPB
#include "jbd_bms.h"
#endif
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

#if !POWERCELL_BMS_JKPB
// JBD / Xiaoxiang poller (2.5.35 product BMS). Request 0x03 basic info, then 0x04 cells,
// every JBD_POLL_MS. A failed or refused read publishes nothing: the last good reading
// ages out after JK_BMS_STALE_MS (bms_online false, fields null), the same as the JK path.
void publishJbd(const powercell::jbd::Reading& r) {
  xSemaphoreTake(mx, portMAX_DELAY);
  pc::jk::Data prev = latest;
  const bool prevFresh = have && millis() - at < JK_BMS_STALE_MS;
  xSemaphoreGive(mx);
  pc::jk::Data d;
  powercell::jbd::toJkData(r, prevFresh ? &prev : nullptr, &d);
  xSemaphoreTake(mx, portMAX_DELAY);
  latest = d;
  have = true;
  at = millis();
  ++st.ok;
  xSemaphoreGive(mx);
}

enum class Rx : uint8_t { Ok, Timeout, Bad, Checksum, Status, Paused };

Rx readReg(uint8_t reg, uint8_t* buf, size_t cap, const uint8_t** data, uint8_t* dlen) {
  uint8_t req[7];
  powercell::jbd::buildRead(reg, req);
  while (uart.available()) uart.read();
  uart.write(req, sizeof req);
  uart.flush();
  const uint32_t start = millis();
  auto rx = [&](uint8_t& b) -> bool {
    while (millis() - start < 300) {
      if (paused) return false;
      if (uart.available()) { b = static_cast<uint8_t>(uart.read()); return true; }
      vTaskDelay(1);
    }
    return false;
  };
  uint8_t b = 0;
  do { if (!rx(b)) return paused ? Rx::Paused : Rx::Timeout; } while (b != powercell::jbd::kStart);
  if (cap < 7) return Rx::Bad;
  buf[0] = b;
  for (int i = 1; i < 4; ++i) if (!rx(buf[i])) return paused ? Rx::Paused : Rx::Timeout;
  const uint8_t n = buf[3];
  if (static_cast<size_t>(n) + 7 > cap) return Rx::Bad;
  for (size_t i = 4; i < static_cast<size_t>(n) + 7; ++i) if (!rx(buf[i])) return paused ? Rx::Paused : Rx::Timeout;
  xSemaphoreTake(mx, portMAX_DELAY);
  ++st.frames;
  lastFrameAt = millis();
  xSemaphoreGive(mx);
  switch (powercell::jbd::classifyFrame(buf, static_cast<size_t>(n) + 7, reg, data, dlen)) {
    case powercell::jbd::Frame::Ok: return Rx::Ok;
    case powercell::jbd::Frame::ChecksumError: return Rx::Checksum;
    case powercell::jbd::Frame::StatusError: return Rx::Status;
    default: return Rx::Bad;
  }
}

void count(Rx r) {
  if (r == Rx::Ok || r == Rx::Paused) return;
  xSemaphoreTake(mx, portMAX_DELAY);
  if (r == Rx::Timeout) ++st.timeouts;
  else if (r == Rx::Checksum) ++st.checksumErrors;
  else ++st.badFrames;  // bad frame, BMS status error, or an implausible reading
  const uint32_t bad = st.checksumErrors + st.badFrames, to = st.timeouts;
  xSemaphoreGive(mx);
  if (r == Rx::Timeout) { if (to % 30 == 1) LOGW("BMS", "JBD", "no reply (timeouts=%lu)", (unsigned long)to); }
  else if (bad % 20 == 1) LOGW("BMS", "JBD", "frame refused (bad=%lu)", (unsigned long)bad);
}

void task(void*) {
  uint8_t buf[96];
  for (;;) {
    if (paused) { idle = true; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    idle = false;
    const uint8_t* data = nullptr;
    uint8_t dlen = 0;
    powercell::jbd::Reading reading;
    Rx r = readReg(powercell::jbd::kRegBasic, buf, sizeof buf, &data, &dlen);
    if (r == Rx::Ok && !powercell::jbd::parseBasic(data, dlen, &reading)) r = Rx::Bad;
    count(r);
    if (r == Rx::Ok) {
      vTaskDelay(pdMS_TO_TICKS(50));  // JBD needs a short gap between requests
      Rx c = readReg(powercell::jbd::kRegCells, buf, sizeof buf, &data, &dlen);
      if (c == Rx::Ok && !powercell::jbd::parseCells(data, dlen, &reading)) c = Rx::Bad;
      count(c);
      publishJbd(reading);
    }
    vTaskDelay(pdMS_TO_TICKS(JBD_POLL_MS));
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
#if !POWERCELL_BMS_JKPB
  uart.begin(PIN_BMS_BAUD, SERIAL_8N1, PIN_BMS_RX, PIN_BMS_TX);
  xTaskCreatePinnedToCore(task, "bms", 4096, nullptr, 1, nullptr, 1);
#if POWERCELL_DSP011
  LOGI("BMS", "INIT", "JBD poll %lu 8N1 rx=%d tx=%d swap=%d", (unsigned long)PIN_BMS_BAUD, PIN_BMS_RX, PIN_BMS_TX, DSP_BMS_UART_SWAP);
#else
  LOGI("BMS", "INIT", "JBD poll %lu 8N1 rx=%d tx=%d", (unsigned long)PIN_BMS_BAUD, PIN_BMS_RX, PIN_BMS_TX);
#endif
#else
  uart.begin(JK_BMS_BAUD, SERIAL_8N1, PIN_BMS_RX, PIN_BMS_TX);
  xTaskCreatePinnedToCore(task, "bms", 4096, nullptr, 1, nullptr, 1);
#if POWERCELL_DSP011
  LOGI("BMS", "INIT", "JK-PB frames %lu 8N1 rx=%d tx=%d swap=%d", (unsigned long)JK_BMS_BAUD, PIN_BMS_RX, PIN_BMS_TX, DSP_BMS_UART_SWAP);
#else
  LOGI("BMS", "INIT", "JK-PB frames %lu 8N1 rx=%d tx=%d", (unsigned long)JK_BMS_BAUD, PIN_BMS_RX, PIN_BMS_TX);
#endif
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
#if !POWERCELL_BMS_JKPB
  // The JBD only talks when asked: send one 0x03 basic-info request (read-only) first.
  while (uart.available()) uart.read();
  uint8_t req[7];
  powercell::jbd::buildRead(powercell::jbd::kRegBasic, req);
  uart.write(req, sizeof req);
#endif
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
