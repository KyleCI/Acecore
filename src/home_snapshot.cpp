#include "home_snapshot.h"

#include <stdio.h>
#include <string.h>

namespace powercell {
namespace {

struct Buf {
  char* data;
  size_t used;
  size_t cap;
  bool ok;
  bool comma;

  void add(const char* text) {
    if (!ok || !text) return;
    const size_t n = strlen(text);
    if (used + n + 1 > cap) {
      ok = false;
      return;
    }
    memcpy(data + used, text, n);
    used += n;
    data[used] = 0;
  }

  void addf(const char* fmt, double value) {
    char tmp[48];
    snprintf(tmp, sizeof tmp, fmt, value);
    add(tmp);
  }

  void sep() {
    if (comma) add(",");
    comma = true;
  }

  void key(const char* name) {
    sep();
    add("\"");
    add(name);
    add("\":");
  }

  void null_value() { add("null"); }

  void boolean(bool value) { add(value ? "true" : "false"); }

  void number(double value) { addf("%.4f", value); }

  void quoted(const char* value) {
    add("\"");
    add(value ? value : "");
    add("\"");
  }

  void key_null(const char* name) {
    key(name);
    null_value();
  }

  void key_bool(const char* name, bool value) {
    key(name);
    boolean(value);
  }

  void key_num(const char* name, bool present, double value) {
    key(name);
    if (present) number(value);
    else null_value();
  }

  void key_str_or_null(const char* name, const char* value) {
    key(name);
    if (value && value[0]) quoted(value);
    else null_value();
  }

  void key_usable(const char* name, const HomeMeters& meters) {
    if (!meters.has_usable) return;
    key(name);
    number(meters.usable_kw);
  }
};

bool serial_ok(const char* serial) {
  if (!serial) return false;
  if (strncmp(serial, "ACE-", 4) != 0) return false;
  for (int i = 0; i < 6; ++i) {
    const char c = serial[4 + i];
    if (c < '0' || c > '9') return false;
  }
  return serial[10] == 0;
}

bool apparent(const HomeMeters& meters) { return meters.have_voltage && meters.have_current; }

double apparent_va(const HomeMeters& meters) { return meters.volts * meters.amps; }

void output_block(Buf& buf, const HomeMeters& meters, bool include_apparent) {
  buf.key_num("output_voltage", meters.have_voltage, meters.volts);
  buf.key_num("output_current", meters.have_current, meters.amps);
  buf.key_num("output_power_w", meters.have_active, meters.active_w);
  buf.key_str_or_null("output_power_source", meters.have_active ? meters.active_source : nullptr);
  if (include_apparent && apparent(meters)) {
    buf.key("output_power");
    buf.number(apparent_va(meters));
    buf.key("output_power_quantity");
    buf.quoted("apparent_va");
  }
  buf.key_usable("usable_kw", meters);
  buf.key_str_or_null("powercell_model", meters.powercell_model);
  buf.key_null("inverter_temperature");
  buf.key_num("battery_voltage", meters.bms_valid, meters.bat_v);
  buf.key_num("battery_current_a", meters.bms_valid, meters.bat_a);
  buf.key_num("battery_power_w", meters.bms_valid, meters.bat_w);
  buf.key_num("remaining_ah", meters.bms_valid, meters.remain_ah);
  buf.key_num("full_capacity_ah", meters.bms_valid, meters.full_ah);
  buf.key_num("cycle_count", meters.bms_valid, static_cast<double>(meters.cycles));
  buf.key_num("soh", meters.bms_valid, meters.soh);
  buf.key_bool("solar_valid", meters.pv_valid);
  buf.key_num("solar_voltage", meters.pv_valid, meters.pv_v);
  buf.key_num("solar_current", meters.pv_valid, meters.pv_a);
  buf.key_num("solar_power_w", meters.pv_valid, meters.pv_w);
}

}  // namespace

int home_snapshot_write(const HomeMeters& meters, const char* note_a, const char* note_b,
                        char* out, size_t cap) {
  if (!out || cap < 2) return -1;
  Buf buf{out, 0, cap, true, false};
  out[0] = 0;
  buf.add("{");
  buf.key("device_id");
  if (serial_ok(meters.device_id)) buf.quoted(meters.device_id);
  else buf.null_value();
  buf.key("device_state");
  buf.quoted(meters.output_on ? "On" : "Off");
  buf.key("state");
  buf.quoted(meters.output_on ? "on" : "off");
  buf.key("model");
  buf.quoted("esp32dev");
  buf.key_str_or_null("powercell_model", meters.powercell_model);
  buf.key_usable("usable_kw", meters);
  buf.key_null("inverter_temperature");

  buf.key("last_data");
  buf.add("{");
  buf.comma = false;
  buf.key_num("power", meters.have_active, meters.active_w);
  buf.add("}");

  buf.comma = true;
  buf.key("inverter");
  buf.add("{");
  buf.comma = false;
  buf.key_bool("stm32_online", meters.stm32_online);
  buf.key_num("output_voltage", meters.have_voltage, meters.volts);
  buf.key_num("output_current", meters.have_current, meters.amps);
  if (apparent(meters)) {
    buf.key("output_power");
    buf.number(apparent_va(meters));
    buf.key("output_power_quantity");
    buf.quoted("apparent_va");
  }
  buf.add("}");

  buf.comma = true;
  buf.key("metrics");
  buf.add("{");
  buf.comma = false;
  output_block(buf, meters, false);
  buf.key_num("inverterVoltage", meters.have_voltage, meters.volts);
  buf.key_num("inverterCurrent", meters.have_current, meters.amps);
  buf.key_num("inverterLoad", apparent(meters), apparent_va(meters));
  buf.add("}");

  buf.comma = true;
  buf.key("live");
  buf.add("{");
  buf.comma = false;
  output_block(buf, meters, false);
  buf.add("}");

  buf.comma = true;
  buf.key("battery");
  buf.add("{");
  buf.comma = false;
  buf.key_bool("valid", meters.bms_valid);
  buf.key("status");
  if (meters.bms_valid && meters.bat_status && meters.bat_status[0]) buf.quoted(meters.bat_status);
  else buf.quoted("unavailable");
  buf.key("source");
  buf.quoted(meters.bms_valid ? "jk_bms_uart2" : "unavailable");
  buf.key_num("percentage", meters.bms_valid, meters.soc);
  buf.key_num("soc", meters.bms_valid, meters.soc);
  buf.key_num("voltage", meters.bms_valid, meters.bat_v);
  buf.key_num("current", meters.bms_valid, meters.bat_a);
  buf.key_num("power", meters.bms_valid, meters.bat_w);
  buf.key_num("battery_power_w", meters.bms_valid, meters.bat_w);
  buf.key_num("remaining_ah", meters.bms_valid, meters.remain_ah);
  buf.key_num("full_ah", meters.bms_valid, meters.full_ah);
  buf.key_num("full_capacity_ah", meters.bms_valid, meters.full_ah);
  buf.key_num("cycles", meters.bms_valid, static_cast<double>(meters.cycles));
  buf.key_num("cycle_count", meters.bms_valid, static_cast<double>(meters.cycles));
  buf.key_num("soh", meters.bms_valid, meters.soh);
  buf.key_num("temperature", meters.bms_valid, meters.bat_temp);
  buf.add("}");

  buf.comma = true;
  buf.key("solar");
  buf.add("{");
  buf.comma = false;
  buf.key_bool("valid", meters.pv_valid);
  if (meters.pv_valid) {
    buf.key_num("voltage", true, meters.pv_v);
    buf.key_num("current", true, meters.pv_a);
    buf.key_num("power", true, meters.pv_w);
    buf.key_num("power_w", true, meters.pv_w);
  }
  buf.add("}");

  if (meters.meter_present) {
    buf.comma = true;
    buf.key("ac_consumed");
    buf.add("{");
    buf.comma = false;
    buf.key("schema");
    buf.number(1);
    buf.key("quantity");
    buf.quoted("active_energy_wh");
    buf.key("meter");
    buf.quoted("pzem004t_v3");
    buf.key_bool("valid", meters.ac_valid);
    buf.key_bool("stale_is_zero", false);
    buf.key_num("lifetime_wh", meters.ac_valid && meters.has_lifetime_wh, meters.lifetime_wh);
    buf.key_num("active_power_w", meters.ac_valid, meters.active_w);
    buf.key_num("voltage_v", meters.ac_valid, meters.volts);
    buf.key_num("current_a", meters.ac_valid, meters.amps);
    buf.add("}");
  }

  buf.comma = true;
  buf.key("notifications");
  buf.add("[");
  bool any = false;
  if (note_a && note_a[0]) {
    buf.add(note_a);
    any = true;
  }
  if (note_b && note_b[0]) {
    if (any) buf.add(",");
    buf.add(note_b);
  }
  buf.add("]}");
  if (!buf.ok) {
    out[0] = 0;
    return -1;
  }
  return static_cast<int>(buf.used);
}

}  // namespace powercell
