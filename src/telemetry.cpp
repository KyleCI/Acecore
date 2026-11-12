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
#include "dsp011_pzem.h"
#include "stm_energy.h"
#include "grid_sense.h"
#include "dsp_thermal.h"
#include "pins.h"
#include "stm32_link.h"
#include "factory_id.h"
#include "live.h"
#include "unit_model.h"

namespace telemetry {
#if POWERCELL_DSP011
void dspGridPublish(JsonObject o, const char *voltKey, const char *availKey);
#endif
namespace {
#if POWERCELL_DSP011
void dspGridKeys(JsonObject o);
#endif
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
#if POWERCELL_DSP011
  if (fresh && s.has_vout && s.has_iout) inv["output_power_quantity"] = "apparent_va";
  else inv["output_power_quantity"] = nullptr;
  inv["stm32_online"] = false;
  acEnergyTodayToJson(inv, "energy_today_kwh");
#else
  stmEnergyToJson(inv, "energy_today_kwh", false);
#endif
  num("grid_voltage", s.has_grid, s.grid_v);
#if POWERCELL_DSP011
  dspGridKeys(inv);
#endif
#if POWERCELL_DSP011
  dspThermalKeys(inv, "temperature");  // system NTC, not an STM32 reading
#else
  num("temperature", s.has_temp, s.temp_c);
#endif
  num("battery_voltage", s.has_bat, s.bat_v);  // STM32 PA3 (cross-check)
  if (!fresh) return;
  inv["system"] = pc::sysStateName(s.sys);
  inv["switch"] = pc::switchName(s.sw);
  inv["charge_state"] = pc::chargeName(s.chr);
#if !POWERCELL_DSP011
  if (s.has_grid) inv["grid_available"] = s.grid_v >= 160.0f;
#endif
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

#if POWERCELL_DSP011
void dspGridKeys(JsonObject o) { dspGridPublish(o, "grid_voltage", "grid_available"); }
#endif

void solarBlock(JsonObject o) {
  solar::Reading r = solar::read();
  o["online"] = r.ok;
  o["valid"] = r.ok;                       // 2.5.4: false = no fresh ADS1115 reading (values null)
  o["i2c_failing"] = solar::failing();
  if (!r.ok) { o["voltage"] = nullptr; o["current"] = nullptr; o["power"] = nullptr; return; }
  o["voltage"] = r.voltage; o["current"] = r.current; o["power"] = r.power;
}

void solarEnergyKeys(JsonObject o) {  // 2.5.8 PV only. DSP AC energy is not written here.
  double et = 0, eT = 0;
  if (solar::energy(et, eT)) { o["energy_today_kwh"] = et; o["energy_total_kwh"] = eT; }
  else { o["energy_today_kwh"] = nullptr; o["energy_total_kwh"] = nullptr; }
}
}  // namespace

#if POWERCELL_DSP011
void dspGridPublish(JsonObject o, const char *voltKey, const char *availKey) {
  DspGridReading g;
  o["grid_v_calibrated"] = false;
  o["grid_v_source"] = "adc_gpio39_k357";
  // Below the 170/160 window the published volts are null, not the GPIO39
  // crosstalk reading. Raw ADC stays on grid_sense_raw_v and in SELFTEST.
  if (dspGridRead(&g) && g.valid) {
    if (g.present) o[voltKey] = g.vrms;
    else o[voltKey] = nullptr;
    o[availKey] = g.present;
    o["grid_v_clipped"] = g.clipped;
    o["grid_sense_raw_v"] = g.raw_vrms;
  } else {
    o[voltKey] = nullptr;
    o[availKey] = nullptr;
    o["grid_v_clipped"] = nullptr;
    o["grid_sense_raw_v"] = nullptr;
  }
}
#endif

#if POWERCELL_DSP011
// System NTC (GPIO36) and fan (GPIO25). Ported from Dubem's dspic collector_2.
// The DSP board has no STM32 temperature, so the installed app's
// inverter_temperature / inverter.temperature keys carry this NTC, as in
// Dubem's metrics.cpp and websocket_commands.cpp. tempKey may be null.
void dspThermalKeys(JsonObject o, const char *tempKey) {
  DspThermalReading t;
  const bool ok = dspThermalRead(&t);
  const bool running = t.sampled;
  if (ok) o["system_temperature_c"] = t.temp_c; else o["system_temperature_c"] = nullptr;
  if (tempKey) {
    if (ok) o[tempKey] = t.temp_c; else o[tempKey] = nullptr;
  }
  const bool faulted = powercell::thermal::sensorFaulted(t.sampled, t.fault, t.age_ms);
  if (running) { o["temp_sensor_fault"] = faulted; o["fan_on"] = t.fan_on; }
  else { o["temp_sensor_fault"] = nullptr; o["fan_on"] = nullptr; }
  const bool cut = powerguard::overTempLatched();
  o["over_temp_cutoff"] = cut;
  // One alert name for the app: the cutoff wins over a sensor fault.
  if (cut) o["temp_alert"] = "over_temperature";
  else if (faulted) o["temp_alert"] = "temp_sensor_fault";
  else o["temp_alert"] = nullptr;
}

void dspThermalBlock(JsonObject o) {
  DspThermalReading t;
  const bool ok = dspThermalRead(&t);
  o["sensor"] = "ntc10k_b3950_2k7_gpio33";
  o["ntc_pin"] = PIN_NTC;
  o["fan_pin"] = PIN_FAN;
  o["valid"] = ok;
  if (ok) o["temperature_c"] = t.temp_c; else o["temperature_c"] = nullptr;
  const bool faulted = powercell::thermal::sensorFaulted(t.sampled, t.fault, t.age_ms);
  const bool cut = powerguard::overTempLatched();
  o["over_temp_cutoff"] = cut;
  o["on_blocked"] = powerguard::overTempBlocksOn();
  if (cut) o["alert"] = "over_temperature";
  else if (faulted) o["alert"] = "temp_sensor_fault";
  else o["alert"] = nullptr;
  o["cutoff_c"] = powercell::thermal::kCutoffC;
  o["on_allowed_below_c"] = powercell::thermal::kOnAllowedBelowC;
  if (t.sampled) {
    o["fault"] = faulted; o["fan_on"] = t.fan_on; o["ntc_mv"] = roundf(t.mv); o["age_ms"] = t.age_ms;
  } else {
    o["fault"] = nullptr; o["fan_on"] = nullptr; o["ntc_mv"] = nullptr; o["age_ms"] = nullptr;
  }
  o["fan_on_c"] = powercell::thermal::kFanOnC;
  o["fan_off_c"] = powercell::thermal::kFanOffC;
}
#endif

void stmEnergyToJson(JsonObject o, const char *kwhKey, bool includeLifetime) {
#if !POWERCELL_DSP011
  double kwh = 0;
  if (kwhKey && stmEnergyTodayKwh(&kwh)) {
    o[kwhKey] = kwh;
    o["energy_today_source"] = "stm32_estimated_pf080";
  } else if (kwhKey) {
    o[kwhKey] = nullptr;
    o["energy_today_source"] = nullptr;
  }
  if (includeLifetime) {
    o["estimated_lifetime_wh"] = stmEnergyLifetimeWh();
    o["estimated_lifetime_source"] = "stm32_estimated_pf080";
  }
#else
  (void)o;
  (void)kwhKey;
  (void)includeLifetime;
#endif
}

void acEnergyTodayToJson(JsonObject o, const char *kwhKey) {
#if POWERCELL_DSP011
  double ac = 0;
  if (kwhKey && hwAcEnergyTodayKwh(&ac)) {
    o[kwhKey] = ac;
    o["energy_today_source"] = "pzem004t_v3_active";
  } else {
    if (kwhKey) o[kwhKey] = nullptr;
    o["energy_today_source"] = nullptr;
  }
#else
  (void)o;
  (void)kwhKey;
#endif
}

// 2.5.8: GET /data + SSE root "solar", the block the installed app's solar tile reads
// (FirmwareSnapshot._solar: solar.online / voltage / current / power).
void appSolarToJson(JsonObject o) {
  solar::Reading r = solar::read();
  pc::live::SolarView pv = pc::live::solarView(r.ok, r.voltage, r.current);
  o["online"] = pv.valid;
  o["valid"] = pv.valid;
  o["i2c_failing"] = solar::failing();
  if (pv.valid) {
    o["voltage"] = pv.voltage; o["current"] = pv.current;
    o["power"] = pc::live::appPowerKw(pv.powerW);  // kW: see live.h appPowerKw
    o["power_w"] = pv.powerW;
  } else { o["voltage"] = nullptr; o["current"] = nullptr; o["power"] = nullptr; o["power_w"] = nullptr; }
  solarEnergyKeys(o);
}

void modelToJson(JsonObject o) {
  const pc::live::Model *m = unitmodel::get();
  if (m) { o["model"] = m->code; o["rated_kva"] = m->ratedKva; o["usable_kw"] = m->usableKw; o["rated_kw"] = m->usableKw; }
  else { o["model"] = nullptr; o["rated_kva"] = nullptr; o["usable_kw"] = nullptr; o["rated_kw"] = nullptr; }
}

void liveToJson(JsonObject o) {
  static pc::live::Runtime rt;
  static portMUX_TYPE rtMux = portMUX_INITIALIZER_UNLOCKED;
  pc::live::Inputs in;
  pc::Stm32Status s; uint32_t age = 0;
  in.stmFresh = stm32link::status(s, &age) && age < STM32_STALE_MS;
  if (in.stmFresh) {
    in.sys = s.sys; in.sw = s.sw; in.chr = s.chr; in.faultActive = pc::ev::faultActive(s.fault);
    in.hasVout = s.has_vout; in.vout = s.vout; in.hasIout = s.has_iout; in.iout = s.iout;
  }
  pc::jk::Data d;
  in.bmsOn = bms::read(d);
  if (in.bmsOn) { in.bmsCurrentA = d.currentMa / 1000.0f; in.bmsPowerW = d.powerMw / 1000.0f; }
  solar::Reading sr = solar::read();
  in.solarOk = sr.ok; in.solarW = sr.power;
#if POWERCELL_DSP011
  in.sysAssumed = true;  // stm32link::status() sets sys = Inverter from any PZEM sample
  {
    DspGridReading g;
    if (dspGridRead(&g) && g.valid) { in.gridKnown = true; in.gridPresent = g.present; }  // debounced, not raw volts
  }
#else
  if (in.stmFresh && s.has_grid) { in.gridKnown = true; in.gridPresent = s.grid_v >= 160.0f; }  // same rule as grid_available
#endif

  o["source"] = pc::live::sourceName(pc::live::source(in));
  o["status"] = pc::live::statusName(pc::live::status(in));
  auto num = [&](const char *k, bool ok, float v) { if (ok) o[k] = v; else o[k] = nullptr; };
  num("output_voltage", in.stmFresh && s.has_vout, s.vout);
  num("output_current", in.stmFresh && s.has_iout, s.iout);
  float w = 0;
  pc::live::PowerSrc ps = pc::live::outputPowerW(in, w);
  num("output_power_w", ps != pc::live::PowerSrc::None, w);
  if (const char *n = pc::live::powerSrcName(ps)) o["output_power_source"] = n; else o["output_power_source"] = nullptr;
#if POWERCELL_DSP011
  float pz_v = 0, pz_a = 0, pz_w = 0;
  uint32_t pz_wh = 0, pz_age = 0;
  if (hwPzemSample(&pz_v, &pz_a, &pz_w, &pz_wh, &pz_age)) {
    w = pz_w;
    o["output_power_w"] = pz_w;
    o["output_power_source"] = "pzem004t_v3_active";
    o["lifetime_wh"] = pz_wh;
  } else {
    o["output_power_w"] = nullptr;
    o["output_power_source"] = nullptr;
    o["lifetime_wh"] = nullptr;
  }
  dspGridPublish(o, "grid_voltage", "grid_available");
#endif
  float pct = 0;
  num("load_pct", ps != pc::live::PowerSrc::None && pc::live::loadPct(w, unitmodel::get(), pct), pct);
#if POWERCELL_DSP011
  dspThermalKeys(o, "inverter_temperature");
#else
  num("inverter_temperature", in.stmFresh && s.has_temp, s.temp_c);
#endif
  {  // 2.5.7 PV in the live block (cloud telemetry.live, /v2/status live, /data+SSE metrics)
    pc::live::SolarView pv = pc::live::solarView(sr.ok, sr.voltage, sr.current);
    o["solar_valid"] = pv.valid;
    num("solar_voltage", pv.valid, pv.voltage);
    num("solar_current", pv.valid, pv.current);
    num("solar_power_w", pv.valid, pv.powerW);
    double et = 0, eT = 0; bool he = solar::energy(et, eT);  // 2.5.8 PV only
    num("solar_energy_today_kwh", he, (float)et);
    num("solar_energy_total_kwh", he, (float)eT);
#if POWERCELL_DSP011
    acEnergyTodayToJson(o, "load_energy_today_kwh");
#else
    stmEnergyToJson(o, "load_energy_today_kwh", true);
#endif
  }
  modelToJson(o);
  num("battery_current_a", in.bmsOn, in.bmsCurrentA);
  num("battery_power_w", in.bmsOn, in.bmsPowerW);
  num("remaining_ah", in.bmsOn, d.remainMah / 1000.0f);
  num("full_capacity_ah", in.bmsOn, d.fullMah / 1000.0f);
  if (in.bmsOn) { o["cycle_count"] = d.cycles; o["soh"] = d.soh; } else { o["cycle_count"] = nullptr; o["soh"] = nullptr; }
  float tte = 0, ttf = 0; bool hasE = false, hasF = false;
  portENTER_CRITICAL(&rtMux);
  if (in.bmsOn) {
    rt.update(in.bmsCurrentA, d.remainMah / 1000.0f, d.fullMah / 1000.0f, millis());
    hasE = rt.toEmptyMin(tte); hasF = rt.toFullMin(ttf);
  } else rt.reset();
  portEXIT_CRITICAL(&rtMux);
  num("time_to_empty_min", hasE, roundf(tte));
  num("time_to_full_min", hasF, roundf(ttf));
}

void buildTelemetry(JsonDocument &doc) {
  {  // 2.2 top-level fields, every cycle, null when unknown
    battery::Reading b = battery::read();
    if (b.valid) doc["soc"] = b.soc; else doc["soc"] = nullptr;
#if POWERCELL_DSP011
    DspGridReading grid;
    if (dspGridRead(&grid) && grid.valid) doc["grid_available"] = grid.present;
    else doc["grid_available"] = nullptr;
#else
    pc::Stm32Status s; uint32_t age = 0;
    if (stm32link::status(s, &age) && age < STM32_STALE_MS && s.has_grid) doc["grid_available"] = s.grid_v >= 160.0f;
    else doc["grid_available"] = nullptr;
#endif
    events::activeToJson(doc["active_events"].to<JsonArray>());
  }
  batteryBlock(doc["battery"].to<JsonObject>());
  inverterBlock(doc["inverter"].to<JsonObject>());
#if POWERCELL_DSP011
  dspThermalBlock(doc["thermal"].to<JsonObject>());
  acEnergyTodayToJson(doc.as<JsonObject>(), "energy_today_kwh");
#else
  stmEnergyToJson(doc.as<JsonObject>(), "energy_today_kwh", false);
#endif
  { JsonObject so = doc["solar"].to<JsonObject>(); solarBlock(so); solarEnergyKeys(so); }
  liveToJson(doc["live"].to<JsonObject>());  // 2.5.4
  JsonArray sockets = doc["sockets"].to<JsonArray>();
  populateDeviceArray(sockets);
  socketcfg::decorate(sockets);
  populateDiscoveredSockets(sockets);
  JsonObject sys = doc["system"].to<JsonObject>();
  sys["uptime_s"] = millis() / 1000;
  sys["free_heap"] = ESP.getFreeHeap();
  sys["heap_min"] = heapguard::heapMin();          // lowest free heap since boot
  sys["heap_largest"] = heapguard::heapLargest();  // largest free block now (TLS gate: heap_guard.h)
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
#if POWERCELL_DSP011
  doc["stm32_online"] = false;
  if (fresh && s.has_vout && s.has_iout) doc["output_power_quantity"] = "apparent_va";
#endif
  num("inverterLoad", fresh && s.has_vout && s.has_iout, s.vout * s.iout);
  num("inverterVoltage", fresh && s.has_vout, s.vout);
  num("inverterCurrent", fresh && s.has_iout, s.iout);
#if POWERCELL_DSP011
  dspThermalKeys(doc.as<JsonObject>(), "inverter_temperature");
#else
  num("inverter_temperature", fresh && s.has_temp, s.temp_c);
#endif
#if POWERCELL_DSP011
  {
    dspGridPublish(doc.as<JsonObject>(), "gridVoltage", "grid_available");
  }
#else
  num("gridVoltage", fresh && s.has_grid, s.grid_v);
#endif
  if (fresh) { doc["inverter_mode"] = pc::sysStateName(s.sys); doc["fault_code"] = s.fault; }
  solar::Reading sr = solar::read();
  num("solarVoltage", sr.ok, sr.voltage);
  num("solarCurrent", sr.ok, sr.current);
  num("solarPower", sr.ok, sr.power);
  doc["solar_valid"] = sr.ok;  // 2.5.4
  liveToJson(doc.as<JsonObject>());  // 2.5.4 flat app keys (source, status, output_power_w, model, TTE ...)
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
