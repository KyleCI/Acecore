#include "bms.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"
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

void task(void *) {
  for (;;) {
    while (uart.available()) {
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
}  // namespace

void begin() {
  mx = xSemaphoreCreateMutex();
  uart.setRxBufferSize(1024);  // > 3 frames
  uart.begin(JK_BMS_BAUD, SERIAL_8N1, PIN_BMS_RX, PIN_BMS_TX);
  xTaskCreatePinnedToCore(task, "bms", 4096, nullptr, 1, nullptr, 1);
  LOGI("BMS", "INIT", "JK-PB frames %lu 8N1 rx=%d tx=%d", (unsigned long)JK_BMS_BAUD, PIN_BMS_RX, PIN_BMS_TX);
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

bool online() { pc::jk::Data d; return read(d); }

Stats stats() {
  Stats s;
  if (!mx) return s;
  xSemaphoreTake(mx, portMAX_DELAY); s = st; xSemaphoreGive(mx);
  return s;
}
}  // namespace bms
