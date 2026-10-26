#include "socket_cfg.h"
#include "power_guard.h"
#include "telemetry.h"
#include "heapguard.h"
#include <WiFi.h>
#include "backend_ws.h"
#include "battery.h"
#include "config.h"
#include "events.h"
#include "events_core.h"
#include "bms.h"
#include "gateway.h"
#include "offline_queue.h"
#include "ota.h"
#include "periph.h"
#include "settings.h"
#include "sockets/sockets.h"
#include "solar.h"
#include "stm32_link.h"
#include "factory_id.h"

namespace telemetry {
namespace {
void faultList(JsonArray arr, uint32_t bits) {
  static const char *names[8] = {"overcurrent", "short_circuit", "dc_overvoltage", "dc_undervoltage",
                                 "output_overvoltage", "thermal", "ntc_open", "no_feedback"};
  for (int i = 0; i < 32; ++i) {
    if (!(bits & (1u << i))) continue;
    if (i < 8) arr.add(names[i]);
    else if (i >= 16 && i <= 19) { static const char *st[4] = {"overload_warning", "permanent_fault", "restarts_exhausted", "charger_full"}; arr.add(st[i - 16]); }
    else { char n[12]; snprintf(n, sizeof n, "fault_bit%d", i); arr.add(n); }
  }
}

// Everything here comes from the STM32 (owner of the inverter, relay, AC,
// grid, load current, NTC, charger and faults). No AC meter on the ESP side.
void inverterBlock(JsonObject inv) {
  pc::Stm32Status s;
  uint32_t age = 0;
  bool have = stm32link::status(s, &age);
  bool fresh = have && age < STM32_STALE_MS;
  inv["stm32_online"] = fresh;
  inv["requested_on"] = gateway::state();
  if (have) inv["stm32_age_ms"] = age;
  auto num = [&](const char *k, bool ok, float v) { if (fresh && ok) inv[k] = v; else inv[k] = nullptr; };
  num("output_voltage", s.has_vout, s.vout);
  num("output_current", s.has_iout, s.iout);  // AC load current (CT)
  num("output_power", s.has_vout && s.has_iout, s.vout * s.iout);  // apparent, V x I
  num("grid_voltage", s.has_grid, s.grid_v);
  num("temperature", s.has_temp, s.temp_c);
  num("battery_voltage", s.has_bat, s.bat_v);  // STM32 PA3 (cross-check)
  if (!fresh) return;
  inv["system"] = pc::sysStateName(s.sys);
  inv["switch"] = pc::switchName(s.sw);
  inv["charge_state"] = pc::chargeName(s.chr);
  if (s.has_grid) inv["grid_available"] = s.grid_v >= 160.0f;
  inv["fault_code"] = s.fault;
  inv["fault_active"] = pc::ev::faultActive(s.fault);  // bits 16-19 are status flags, not faults
  faultList(inv["faults"].to<JsonArray>(), s.fault);
  if (s.has_ext) {  // framed status v1 only
    inv["restart_count"] = s.restart_count;
    inv["charge_current"] = s.chg_current;
    inv["modulation_index"] = s.mod_index;
    inv["stm32_uptime_s"] = s.uptime_s;
    inv["transfer_active"] = (s.transfer_flags & pc::TRANSFER_ACTIVE) != 0;
    inv["grid_phase_synced"] = (s.transfer_flags & pc::TRANSFER_GRID_SYNCED) != 0;
    inv["vout_calibrated"] = (s.transfer_flags & pc::TRANSFER_VOUT_CALIBRATED) != 0;
  }
}

void bmsAlarms(JsonArray arr, uint32_t bits) {
  for (int i = 0; i < 32; ++i)
    if (bits & (1u << i)) {
      if (const char *n = pc::jk::alarmName(i)) arr.add(n);
      else { char b[12]; snprintf(b, sizeof b, "alarm_bit%d", i); arr.add(b); }
    }
}

// Battery: BMS owns SoC, cells, current, temps, cycles, alarms. Shown voltage is
// the BMS pack voltage; the STM32 PA3 value is kept as stm32_voltage.
void batteryBlock(JsonObject b) {
  battery::Reading r = battery::read();
  if (r.valid) { b["voltage"] = r.voltage; b["soc"] = r.soc; }
  else { b["voltage"] = nullptr; b["soc"] = nullptr; }
  b["source"] = r.source;
  if (r.hasStm32V) b["stm32_voltage"] = r.stm32V; else b["stm32_voltage"] = nullptr;
  b["voltage_mismatch"] = r.mismatch;
  pc::jk::Data d;
  uint32_t age = 0;
  bool on = bms::read(d, &age);
  b["bms_online"] = on;
  if (!on) {
    for (const char *k : {"current", "power", "temperature", "mos_temperature", "cycles", "soh", "remaining_ah",
                          "full_ah", "cell_min_v", "cell_max_v", "cell_diff_mv", "status"})
      b[k] = nullptr;
    b["cells"].to<JsonArray>();
    b["alarms"].to<JsonArray>();
    return;
  }
  b["bms_age_ms"] = age;
  b["current"] = d.currentMa / 1000.0f;
  b["power"] = d.powerMw / 1000.0f;
  b["temperature"] = d.tempC[0];
  JsonArray t = b["temperatures"].to<JsonArray>(); t.add(d.tempC[0]); t.add(d.tempC[1]);
  b["mos_temperature"] = d.mosTempC;
  b["cycles"] = d.cycles;
  b["soh"] = d.soh;
  b["remaining_ah"] = d.remainMah / 1000.0f;
  b["full_ah"] = d.fullMah / 1000.0f;
  static const char *st[] = {"standby", "charging", "discharging"};
  b["status"] = d.status < 3 ? st[d.status] : "unknown";
  b["charge_mos"] = d.chargeMos; b["discharge_mos"] = d.dischargeMos; b["balancing"] = d.balancing;
  JsonArray c = b["cells"].to<JsonArray>();
  for (int i = 0; i < d.cellCount; ++i) c.add(d.cellMv[i] / 1000.0f);
  uint16_t lo = 0xFFFF, hi = 0;
  for (int i = 0; i < d.cellCount; ++i) if (d.cellMv[i] > 500) { if (d.cellMv[i] < lo) lo = d.cellMv[i]; if (d.cellMv[i] > hi) hi = d.cellMv[i]; }
  b["cell_min_v"] = lo / 1000.0f;
  b["cell_max_v"] = hi / 1000.0f;
  b["cell_diff_mv"] = d.cellDiffMv;
  b["alarm_bits"] = d.alarms;
  bmsAlarms(b["alarms"].to<JsonArray>(), d.alarms);
}

void solarBlock(JsonObject o) {
  solar::Reading r = solar::read();
  o["online"] = r.ok;
  if (!r.ok) { o["voltage"] = nullptr; o["current"] = nullptr; o["power"] = nullptr; return; }
  o["voltage"] = r.voltage; o["current"] = r.current; o["power"] = r.power;
}
}  // namespace

void buildTelemetry(JsonDocument &doc) {
  {  // 2.2 top-level fields, every cycle, null when unknown
    battery::Reading b = battery::read();
    if (b.valid) doc["soc"] = b.soc; else doc["soc"] = nullptr;
    pc::Stm32Status s; uint32_t age = 0;
    if (stm32link::status(s, &age) && age < STM32_STALE_MS && s.has_grid) doc["grid_available"] = s.grid_v >= 160.0f;
    else doc["grid_available"] = nullptr;
    events::activeToJson(doc["active_events"].to<JsonArray>());
  }
  batteryBlock(doc["battery"].to<JsonObject>());
  inverterBlock(doc["inverter"].to<JsonObject>());
  solarBlock(doc["solar"].to<JsonObject>());
  JsonArray sockets = doc["sockets"].to<JsonArray>();
  populateDeviceArray(sockets);
  socketcfg::decorate(sockets);
  populateDiscoveredSockets(sockets);
  JsonObject sys = doc["system"].to<JsonObject>();
  sys["uptime_s"] = millis() / 1000;
  sys["free_heap"] = ESP.getFreeHeap();
  sys["heap_min"] = heapguard::heapMin();          // lowest free heap since boot
  sys["heap_largest"] = heapguard::heapLargest();  // largest free block now (TLS needs >= 45 KB)
  sys["heap_largest_min"] = heapguard::heapLargestMin();
  sys["wifi_rssi"] = WiFi.RSSI();
  sys["offline_pending"] = offlineq::pending();
  periph::toJson(sys["peripherals"].to<JsonObject>());
  powerguard::toJson(doc["power_guard"].to<JsonObject>());
}

void buildSnapshot(JsonDocument &doc) {
  const auto &id = settings::id();
  doc["gateway_name"] = id.gatewayName;
  doc["device_name"] = id.gatewayName;
  doc["online"] = true;
  JsonObject gw = doc["gateway"].to<JsonObject>();
  gw["reported_state"] = gateway::state() ? "on" : "off";
  gw["status"] = gateway::state() ? "on" : "off";
  gw["payment_locked"] = fid::paymentLocked();
  gw["payment_lock_reason"] = fid::paymentLocked() ? fid::paymentLockReason() : "";
  JsonArray sockets = doc["sockets"].to<JsonArray>();
  populateDeviceArray(sockets);
  socketcfg::decorate(sockets);
  populateDiscoveredSockets(sockets);
}

void buildMetrics(JsonDocument &doc) {
  battery::Reading b = battery::read();
  auto num = [&](const char *k, bool ok, float v) { if (ok) doc[k] = v; else doc[k] = nullptr; };
  num("battery_voltage", b.valid, b.voltage);
  if (b.valid) doc["battery_percentage"] = b.soc; else doc["battery_percentage"] = nullptr;
  doc["battery_source"] = b.source;
  num("battery_voltage_stm32", b.hasStm32V, b.stm32V);
  pc::jk::Data d;
  bool bmsOn = bms::read(d);
  doc["bms_online"] = bmsOn;
  num("battery_current", bmsOn, d.currentMa / 1000.0f);
  num("battery_power", bmsOn, d.powerMw / 1000.0f);
  num("battery_temperature", bmsOn, d.tempC[0]);
  num("bms_mos_temperature", bmsOn, d.mosTempC);
  if (bmsOn) { doc["battery_cycles"] = d.cycles; doc["bms_alarm_bits"] = d.alarms; }
  else { doc["battery_cycles"] = nullptr; doc["bms_alarm_bits"] = nullptr; }
  doc["device_state"] = gateway::state();  // inverter requested on (PWR_KEY / set_mode)
  doc["payment_locked"] = fid::paymentLocked();
  doc["payment_lock_reason"] = fid::paymentLocked() ? fid::paymentLockReason() : "";
  pc::Stm32Status s;
  uint32_t age = 0;
  bool fresh = stm32link::status(s, &age) && age < STM32_STALE_MS;
  doc["stm32_online"] = fresh;
  num("inverterLoad", fresh && s.has_vout && s.has_iout, s.vout * s.iout);
  num("inverterVoltage", fresh && s.has_vout, s.vout);
  num("inverterCurrent", fresh && s.has_iout, s.iout);
  num("inverter_temperature", fresh && s.has_temp, s.temp_c);
  num("gridVoltage", fresh && s.has_grid, s.grid_v);
  if (fresh) { doc["inverter_mode"] = pc::sysStateName(s.sys); doc["fault_code"] = s.fault; }
  solar::Reading sr = solar::read();
  num("solarVoltage", sr.ok, sr.voltage);
  num("solarCurrent", sr.ok, sr.current);
  num("solarPower", sr.ok, sr.power);
  doc["radio_ready"] = socketsRadioReady();
  doc["lora_ready"] = periph::loraPresent();
  doc["sd_card"] = periph::sdPresent();
  doc["socket_transport"] = socketTransport();
  doc["cloud_connected"] = backend::connected();
  doc["firmware_version"] = ota::version();
  doc["device_id"] = settings::id().deviceId;
  JsonArray sockets = doc["sockets"].to<JsonArray>();
  populateDeviceArray(sockets);
  socketcfg::decorate(sockets);
}
}  // namespace telemetry
