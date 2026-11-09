#include "home_snapshot.h"

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
  meters.bms_valid = false;
  meters.pv_valid = false;
  meters.has_lifetime_wh = false;
  check(powercell::home_snapshot_write(meters, nullptr, nullptr, body, sizeof body) > 0, "stale write");
  printf("SNAP stale %s\n", body);

  fresh(&meters);
  meters.ac_valid = false;
  meters.has_usable = true;
  meters.usable_kw = 3.15;
  meters.powercell_model = nullptr;
  meters.device_id = "AA:BB:CC:DD:EE:FF";
  check(powercell::home_snapshot_write(meters, nullptr, nullptr, body, sizeof body) > 0, "3.5kW write");
  printf("SNAP kw35 %s\n", body);

  check(powercell::home_snapshot_write(meters, nullptr, nullptr, body, 8) < 0, "short buffer fails");
  if (failures) {
    fprintf(stderr, "%d snapshot checks failed\n", failures);
    return 1;
  }
  return 0;
}
