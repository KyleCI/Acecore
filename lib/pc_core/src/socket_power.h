#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
// 2.5.36 (Caleb 18 Nov 2026): smart-socket power as P = V x I on the LoRa socket path
// (collector side only: LoRa packet format and socket firmware unchanged).
// V is the collector's own AC output voltage (DSP: PZEM-004T v3, STM: STM32 vout), I is the
// socket's RMS current (ZMCT103C CT on the socket). V x I is APPARENT power in VA. It equals
// real watts only for resistive loads (kettle, heater, bulb). For motors, fridges, fans,
// chargers and other electronics it OVERSTATES the real watts (true W = VA x power factor).
// The JSON labels it honestly: apparent_power_va + power_quantity "apparent_va";
// current_usage_watts carries the same VA number only because the app/backend read that key.
// Warnings and trips stay current based (amps), never VA: see socket_rules.h / the socket.
namespace pc {
namespace sockpower {
constexpr float kNoiseFloorA = 0.20f;   // 2.5.37 (Caleb 18 Nov 17:02 WAT): was 0.10. CT idle reading. Below it the power is 0 VA; "current" is still sent as measured
constexpr uint32_t kVoltageFreshMs = 10000;  // collector voltage sample older than this -> voltage and power null
constexpr float kMinPlausibleV = 0.0f;
constexpr float kMaxPlausibleV = 300.0f;
constexpr const char *kVoltageSource = "collector_output";
constexpr const char *kQuantity = "apparent_va";

// Socket current for the power figure: below the noise floor counts as 0 A.
// Negative or non-finite readings are invalid (returns -1).
inline float effectiveCurrent(float amps) {
  if (!isfinite(amps) || amps < 0) return -1.0f;
  return amps < kNoiseFloorA ? 0.0f : amps;
}
inline bool plausibleVoltage(float v) { return isfinite(v) && v >= kMinPlausibleV && v <= kMaxPlausibleV; }

struct Result {
  bool hasVoltage = false;  // collector voltage fresh and plausible
  float voltage = 0;
  bool hasPower = false;    // both voltage and socket current usable
  float apparentVa = 0;
};
// socketFresh: the socket's last reading is current (online). vFresh: the collector's
// output-voltage sample is inside its freshness window. Anything stale or implausible -> null.
inline bool voltageFresh(bool have, uint32_t ageMs) { return have && ageMs <= kVoltageFreshMs; }
inline Result compute(bool socketFresh, float socketAmps, bool vFresh, float volts) {
  Result r;
  if (vFresh && plausibleVoltage(volts)) { r.hasVoltage = true; r.voltage = volts; }
  float a = effectiveCurrent(socketAmps);
  if (r.hasVoltage && socketFresh && a >= 0) {
    r.hasPower = true;
    r.apparentVa = roundf(volts * a * 10.0f) / 10.0f;  // 0.1 VA resolution
  }
  return r;
}

// Writes the per-socket power keys. Obj needs o["key"] = float / const char * / nullptr
// (ArduinoJson JsonObject on the device, a fake object in the host tests).
template <class Obj>
void powerFieldsTo(Obj o, const Result &r) {
  if (r.hasVoltage) o["voltage"] = r.voltage; else o["voltage"] = nullptr;
  o["voltage_source"] = kVoltageSource;
  if (r.hasPower) {
    o["apparent_power_va"] = r.apparentVa;
    o["current_usage_watts"] = r.apparentVa;  // VA, not real watts: the app/backend key name
  } else {
    o["apparent_power_va"] = nullptr;
    o["current_usage_watts"] = nullptr;
  }
  o["power_quantity"] = kQuantity;
}

// Keys kept in the compact top-level "sockets" copy of the local /data snapshot (web_local.cpp)
// when the 4 KB snapshot has to drop metrics.sockets. 2.5.36 adds the power keys.
constexpr const char *kCompactSnapshotKeys[] = {
    "deviceId", "online", "relayState", "desired", "pending", "cmd_id", "command_status",
    "name", "configured", "current", "last_seen_age_ms", "lockout", "rssi", "available_to_pair",
    "current_usage_watts", "apparent_power_va", "power_quantity", "voltage", "voltage_source"};
constexpr size_t kCompactSnapshotKeyCount = sizeof(kCompactSnapshotKeys) / sizeof(kCompactSnapshotKeys[0]);
}  // namespace sockpower
}  // namespace pc
