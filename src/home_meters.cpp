#include "home_meters.h"

#include "ac_output_settings.h"
#include "bms.h"
#include "display_identity.h"
#include "factory_rating.h"
#include "mppt.h"
#include "provisioning.h"

#ifdef POWERCELL_HW_V011
#include "hw_v011.h"
#endif

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

void collect_home_meters(powercell::HomeMeters* out) {
  if (!out) return;
  powercell::HomeMeters meters;
  meters.output_on = deviceState;

  static char serial[16];
  serial[0] = 0;
  Preferences prefs;
  if (prefs.begin("pc_ident", true)) {
    const String stored = prefs.getString("serial", "");
    snprintf(serial, sizeof serial, "%s", stored.c_str());
    prefs.end();
  }
  meters.device_id = factory_serial_valid(serial) ? serial : nullptr;

  const int usable = powercell::SystemRating::usableFor(POWERCELL_FACTORY_RATED_ACTIVE_W);
  if (usable == powercell::SystemRating::kUsable5kW) {
    meters.has_usable = true;
    meters.usable_kw = 4.5;
    meters.powercell_model = "PC5K";
  } else if (usable == powercell::SystemRating::kUsable35kW) {
    meters.has_usable = true;
    meters.usable_kw = 3.15;
    meters.powercell_model = nullptr;
  }

#ifdef POWERCELL_HW_V011
  HwV011Ac ac;
  meters.meter_present = true;
  if (hwV011AcSample(&ac) && ac.valid) {
    meters.ac_valid = true;
    meters.volts = ac.volts;
    meters.amps = ac.amps;
    meters.active_w = ac.active_w;
    meters.has_lifetime_wh = true;
    meters.lifetime_wh = ac.lifetime_wh;
  }
#endif

  JKPacketData bms = {};
  const uint32_t now = millis();
  const bool got = getBmsData(bms);
  if (powercell::bms_sample_fresh(got && bms.valid, bms.receivedAtMs, now)) {
    meters.bms_valid = true;
    meters.soc = bms.SOC;
    meters.bat_v = bms.packVoltage;
    meters.bat_a = bms.packCurrent;
    meters.bat_w = bms.packPower;
    meters.remain_ah = bms.remainCapacity;
    meters.full_ah = bms.fullCapacity;
    meters.cycles = static_cast<int>(bms.cycleCount);
    meters.soh = bms.SOH;
    meters.bat_temp = bms.tempBat1;
    if (bms.batStatus == 1) meters.bat_status = "charging";
    else if (bms.batStatus == 2) meters.bat_status = "discharging";
    else meters.bat_status = "standby";
  }

  MpptData pv;
  if (mppt_get_data(pv)) {
    meters.pv_valid = true;
    meters.pv_v = pv.v_bus_V;
    meters.pv_a = pv.current_A;
    meters.pv_w = pv.power_W;
  }
  *out = meters;
}
