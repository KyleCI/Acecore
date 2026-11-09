#include "home_snapshot.h"
#include "power_units.h"

#include <stdio.h>
#include <string.h>

namespace {

int failures = 0;

void check(bool ok, const char* text) {
  if (ok) return;
  fprintf(stderr, "FAIL %s\n", text);
  ++failures;
}

void fresh(powercell::HomeMeters* m) {
  m->device_id = "ACE-000123";
  m->output_on = true;
  m->have_voltage = true;
  m->have_current = true;
  m->have_active = true;
  m->active_source = "pzem004t_v3_active";
  m->ac_valid = true;
  m->volts = 230;
  m->amps = 0.1;
  m->active_w = 15;
  m->meter_present = true;
  m->has_lifetime_wh = true;
  m->lifetime_wh = 1200;
  m->bms_valid = true;
  m->soc = 40;
  m->bat_v = 51.2;
  m->bat_a = 1.5;
  m->bat_w = 76.8;
  m->remain_ah = 40;
  m->full_ah = 100;
  m->cycles = 12;
  m->soh = 98;
  m->bat_temp = 27.5;
  m->bat_status = "standby";
  m->pv_valid = true;
  m->pv_v = 36;
  m->pv_a = 2;
  m->pv_w = 72;
  m->has_usable = true;
  m->usable_kw = 4.5;
  m->powercell_model = "PC5K";
}

}  // namespace

int main() {
  powercell::OutputSample watts;
  watts.active_present = true;
  watts.active = 15;
  watts.active_scale = powercell::PowerScale::Watts;
  watts.voltage_present = true;
  watts.volts = 230;
  watts.current_present = true;
  watts.amps = 0.1;
  const powercell::NormalizedOutput fifteen = powercell::normalize_output(watts);
  check(fifteen.have_active && fifteen.active_w == 15, "15 W stays 15");
  check(fifteen.have_apparent && fifteen.apparent_va == 23, "apparent stays V*I");
  check(fifteen.active_w != fifteen.apparent_va, "active is not apparent");
  powercell::OutputSample kilowatts = watts;
  kilowatts.active = 0.015;
  kilowatts.active_scale = powercell::PowerScale::Kilowatts;
  const powercell::NormalizedOutput from_kw = powercell::normalize_output(kilowatts);
  check(from_kw.active_w > 14.999 && from_kw.active_w < 15.001, "0.015 kW becomes 15 W");
  powercell::OutputSample stm32;
  stm32.voltage_present = true;
  stm32.volts = 230;
  stm32.current_present = true;
  stm32.amps = 1.5;
  const powercell::NormalizedOutput stm = powercell::normalize_output(stm32);
  check(!stm.have_active && stm.have_apparent && stm.apparent_va == 345, "STM32 has VA and no active watts");
  powercell::OutputSample placeholder = watts;
  placeholder.active = -1;
  check(!powercell::normalize_output(placeholder).have_active, "-1 is not published");
  placeholder.active = 99999;
  check(!powercell::normalize_output(placeholder).have_active, "99999 is not published");
  check(!powercell::power_publishable(-1) && !powercell::power_publishable(99999), "sentinels rejected");
  check(powercell::power_publishable(15), "15 W is a real reading");

  check(powercell::bms_sample_fresh(true, 5000, 20000), "bms 15s inclusive");
  check(!powercell::bms_sample_fresh(true, 4999, 20000), "bms older than 15s");
  check(!powercell::bms_sample_fresh(false, 20000, 20000), "bms invalid frame");
  check(powercell::ac_sample_fresh(true, 1000, 3000), "ac 2s inclusive");
  check(!powercell::ac_sample_fresh(true, 999, 3000), "ac older than 2s");
  check(!powercell::ac_sample_fresh(false, 3000, 3000), "ac invalid sample");

  char body[4096];
  powercell::HomeMeters meters;
  fresh(&meters);
  check(powercell::home_snapshot_write(meters, "{\"id\":\"battery_low\"}", nullptr, body, sizeof body) > 0,
        "fresh write");
  printf("SNAP fresh %s\n", body);

  fresh(&meters);
  meters.ac_valid = false;
  meters.have_voltage = false;
  meters.have_current = false;
  meters.have_active = false;
  meters.bms_valid = false;
  meters.pv_valid = false;
  meters.has_lifetime_wh = false;
  check(powercell::home_snapshot_write(meters, nullptr, nullptr, body, sizeof body) > 0, "stale write");
  printf("SNAP stale %s\n", body);

  fresh(&meters);
  meters.ac_valid = false;
  meters.have_voltage = false;
  meters.have_current = false;
  meters.have_active = false;
  meters.has_usable = true;
  meters.usable_kw = 3.15;
  meters.powercell_model = nullptr;
  meters.device_id = "AA:BB:CC:DD:EE:FF";
  check(powercell::home_snapshot_write(meters, nullptr, nullptr, body, sizeof body) > 0, "3.5kW write");
  printf("SNAP kw35 %s\n", body);

  powercell::HomeMeters converted;
  converted.have_voltage = from_kw.have_voltage;
  converted.have_current = from_kw.have_current;
  converted.have_active = from_kw.have_active;
  converted.volts = from_kw.volts;
  converted.amps = from_kw.amps;
  converted.active_w = from_kw.active_w;
  converted.active_source = "pzem004t_v3_active";
  check(powercell::home_snapshot_write(converted, nullptr, nullptr, body, sizeof body) > 0, "kW input write");
  printf("SNAP from_kw %s\n", body);

  powercell::HomeMeters text;
  text.stm32_online = true;
  text.have_voltage = stm.have_voltage;
  text.have_current = stm.have_current;
  text.have_active = stm.have_active;
  text.volts = stm.volts;
  text.amps = stm.amps;
  text.active_w = stm.active_w;
  check(powercell::home_snapshot_write(text, nullptr, nullptr, body, sizeof body) > 0, "stm32 write");
  printf("SNAP stm32 %s\n", body);

  powercell::HomeMeters bad;
  bad.have_active = true;
  bad.active_w = -1;
  bad.active_source = "pzem004t_v3_active";
  bad.have_voltage = true;
  bad.have_current = true;
  bad.volts = 99999;
  bad.amps = 1;
  bad.bms_valid = true;
  bad.bat_w = 99999;
  bad.pv_valid = true;
  bad.pv_w = -1;
  bad.meter_present = true;
  bad.ac_valid = true;
  check(powercell::home_snapshot_write(bad, nullptr, nullptr, body, sizeof body) > 0, "sentinel write");
  printf("SNAP sentinel %s\n", body);

  check(powercell::home_snapshot_write(meters, nullptr, nullptr, body, 8) < 0, "short buffer fails");
  if (failures) {
    fprintf(stderr, "%d snapshot checks failed\n", failures);
    return 1;
  }
  return 0;
}
