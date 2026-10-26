// Acecore Powercell collector firmware 2.x (ESP32, Arduino).
// See README.md for the architecture, pin map and decisions.
#include "events.h"
#include "clock.h"
#include "power_guard.h"
#include "socket_cfg.h"
#include <Arduino.h>
#include "backend_ws.h"
#include "battery.h"
#include "bms.h"
#include "config.h"
#include "console.h"
#include "gateway.h"
#include "leds.h"
#include "log.h"
#include "net.h"
#include "offline_queue.h"
#include "onboarding.h"
#include "ota.h"
#include "heapguard.h"
#include "periph.h"
#include "settings.h"
#include "sockets/sockets.h"
#include "stm32_link.h"
#include "stm32_updater.h"
#include "web_local.h"

namespace {
// Before an OTA download: make the output safe and persist counters.
bool prepareForUpdate() {
  gateway::set(false, "ota");
  return true;
}
}  // namespace

void setup() {
  gateway::earlyInit();  // IO15 (PWR_KEY, strapping pin) LOW: inverter not requested
  Serial.begin(115200);
  delay(50);
  esp_reset_reason_t why = esp_reset_reason();
  LOGI("BOOT", "START", "fw=%s kind=%s reset_reason=%d free_heap=%lu", POWERCELL_FIRMWARE_VERSION, POWERCELL_FIRMWARE_KIND,
       int(why), (unsigned long)ESP.getFreeHeap());
  if (why == ESP_RST_BROWNOUT) LOGE("BOOT", "BROWNOUT", "supply dipped; check the 5 V rail");
  if (why == ESP_RST_TASK_WDT || why == ESP_RST_INT_WDT || why == ESP_RST_WDT) LOGE("BOOT", "WATCHDOG", "previous run stalled");

  settings::begin();
  clock_::begin();     // tz + last good time from NVS
  onboarding::begin();  // per-unit passcode/claim state (after settings, before net/web)
  gateway::begin();
  leds::begin();
  stm32link::begin();
  battery::begin();
  bms::begin();
  periph::begin();     // SPI bus, ADS1115 solar, LoRa, optional SD
  offlineq::begin();
  events::begin();
  socketsInit();
  socketcfg::begin();
  powerguard::begin();  // after gateway/sockets: a latched shutdown is re-applied from NVS
  net::begin();
  socketsStart();     // ESP-NOW follows the STA/AP channel; retries until the radio is up
  web::begin();
  backend::begin();
  ota::begin(prepareForUpdate, backend::pauseForOta);
}

void loop() {
  static uint32_t worst = 0, lastReport = 0;
  uint32_t t0 = millis();
  backend::appLock();
  console::service();
  net::service();
  offlineq::service(backend::connected());
  gateway::service();
  powerguard::service();
  clock_::service();
  events::service();
  web::service();
  onboarding::service();
  ota::service(backend::healthy());
  stm32update::service();
  leds::service();
  backend::appUnlock();
  heapguard::sample();
  uint32_t dt = millis() - t0;
  if (dt > worst) worst = dt;
  if (dt > LOOP_SLOW_MS) LOGW("LOOP", "SLOW", "pass took %lu ms", (unsigned long)dt);
  if (millis() - lastReport > 60000) { lastReport = millis(); LOGI("LOOP", "STATS", "worst pass %lu ms in the last minute heap_free=%lu heap_min=%lu heap_largest=%lu largest_min=%lu", (unsigned long)worst, (unsigned long)ESP.getFreeHeap(), (unsigned long)heapguard::heapMin(), (unsigned long)heapguard::heapLargest(), (unsigned long)heapguard::heapLargestMin()); worst = 0; }
  delay(2);
}
