// 2.5.35 JBD-image host tests: lib/pc_core and the JBD driver built with POWERCELL_BMS_JBD=1,
// the way the six product images are. Build and run: make -C test/host (target jbd_host_tests).
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "events_core.h"
#include "jbd_bms.h"
#include "jk_bms.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

#if !POWERCELL_BMS_JBD
#error "build with -DPOWERCELL_BMS_JBD=1"
#endif

int main() {
  using namespace powercell::jbd;
  using namespace pc::ev;
  // JBD-only protections are named in JBD images.
  CHECK(!strcmp(pc::jk::alarmName(pc::jk::kAlarmJbdDischargeUndertemp), "discharge_undertemp"));
  CHECK(!strcmp(pc::jk::alarmName(pc::jk::kAlarmJbdAfeError), "afe_ic_error"));
  CHECK(!strcmp(pc::jk::alarmName(pc::jk::kAlarmJbdMosLock), "mos_software_lock"));
  CHECK(pc::jk::alarmName(27) == nullptr && pc::jk::alarmName(31) == nullptr);
  CHECK(!strcmp(pc::jk::alarmName(8), "charge_overtemp") && !strcmp(pc::jk::alarmName(15), "discharge_overtemp"));

  // Every JBD protection bit raises exactly one bms_N event with the shared map.
  struct Want { uint16_t prot; const char *id; Type type; Sev sev; };
  const Want w[] = {
      {kProtCellOv, "bms_4", Type::BmsFault, Sev::Critical},     {kProtCellUv, "bms_11", Type::BatteryLow, Sev::Critical},
      {kProtPackOv, "bms_5", Type::BmsFault, Sev::Critical},     {kProtPackUv, "bms_12", Type::BatteryLow, Sev::Critical},
      {kProtChgOt, "bms_8", Type::BmsOverTemp, Sev::Critical},   {kProtChgUt, "bms_9", Type::BmsFault, Sev::Warning},
      {kProtDsgOt, "bms_15", Type::BmsOverTemp, Sev::Critical},  {kProtDsgUt, "bms_24", Type::BmsFault, Sev::Warning},
      {kProtChgOc, "bms_6", Type::Overcurrent, Sev::Critical},   {kProtDsgOc, "bms_13", Type::Overcurrent, Sev::Critical},
      {kProtShort, "bms_14", Type::Overcurrent, Sev::Critical},  {kProtAfe, "bms_25", Type::BmsFault, Sev::Critical},
      {kProtMosLock, "bms_26", Type::BmsFault, Sev::Warning},
  };
  for (const Want &x : w) {
    Engine e; Inputs in; in.bmsOnline = true; in.bmsAlarms = toJkAlarms(x.prot);
    e.update(in, 0, 1);
    Event ev[8]; size_t k = e.drain(ev, 8);
    CHECK(k == 1);
    if (k == 1) {
      CHECK(!strcmp(ev[0].id, x.id) && ev[0].type == x.type && ev[0].severity == x.sev && ev[0].active);
      if (strcmp(ev[0].id, x.id)) printf("  got %s want %s\n", ev[0].id, x.id);
    }
    // Clears after the shared 5 s hold once the BMS drops the bit.
    in.bmsAlarms = 0; e.update(in, 1000, 2); CHECK(e.drain(ev, 8) == 0);
    e.update(in, 6100, 3); k = e.drain(ev, 8);
    CHECK(k == 1 && !ev[0].active);
  }
  // BMS offline: no alarm events at all.
  { Engine e; Inputs in; in.bmsOnline = false; in.bmsAlarms = toJkAlarms(0x1FFF); e.update(in, 0, 1); Event ev[8]; CHECK(e.drain(ev, 8) == 0); }

  printf("jbd image tests: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
