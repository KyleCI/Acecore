#pragma once
#include <stddef.h>
#include <stdint.h>
#include "local_auth.h"
#include "stm32_types.h"
// Notification event engine (firmware 2.2). Pure logic, host tested.
// Each condition has a stable id, debounce (raw state must hold for onMs /
// offMs before it changes) and threshold hysteresis. Transitions (start,
// clear, severity change) are queued for the device glue to fan out to WS
// telemetry, the offline queue, LAN /data and the /events SSE stream.
namespace pc {
namespace ev {
enum class Type : uint8_t { Overload, Overcurrent, InverterFault, BmsFault, RelayFailed, OverTemp, BatteryFull, BatteryLow, GridDown, ApplianceLockout, BatteryCritical, OutputRestored, ModeFallback, LoadShed };
enum class Sev : uint8_t { Info, Warning, Critical };
const char *typeName(Type t);
const char *sevName(Sev s);

// STM32 fault_bits (u32). Bits 0..7 are faults; 16..19 are status flags (hardware VP, no VER bump).
constexpr uint32_t kFaultMask = 0xFF;
constexpr uint32_t kStatusOverloadWarn = 1u << 16;      // >= 19 A RMS for 2 s
constexpr uint32_t kStatusPermanent = 1u << 17;         // permanent fault latched
constexpr uint32_t kStatusRestartsExhausted = 1u << 18; // auto-restarts exhausted
constexpr uint32_t kStatusChargerFull = 1u << 19;       // charger confirmed full
constexpr uint32_t kStatusReserve = 1u << 20;           // STM32 reserve latch (47.5 V / 60 s); status, not a fault
constexpr uint32_t kPermanentBits = 0x01 | 0x02 | 0x04 | 0x10 | 0x40;  // need a manual reset
inline bool faultActive(uint32_t bits) { return (bits & kFaultMask) != 0; }

constexpr float kOverloadAmps = 19.0f, kOverloadClearAmps = 17.0f;  // ESP rule: any link without bit16
constexpr float kTempWarnC = 70.0f, kTempWarnClearC = 65.0f;
constexpr float kGridMinV = 160.0f, kGridRestoreV = 170.0f;
constexpr float kSocketTripA = 10.0f, kSocketTripClearA = 9.0f, kSocketWarnA = 8.0f, kSocketWarnClearA = 7.0f;

struct Event {
  Type type = Type::InverterFault;
  bool active = false;
  Sev severity = Sev::Info;
  bool hasValue = false;
  float value = 0;
  char id[32] = {};      // stable condition id, e.g. "stm32_0x01", "socket_oc_A1B2C3D4E5F6"
  char socket[13] = {};  // smart socket id, empty otherwise
  char code[24] = {};
  uint32_t ts = 0;       // unix seconds (0 if the clock is not set)
  const char *source = "esp";  // stm32 | bms | socket | esp
  bool permanent = false;
  const char *releaseRequires = nullptr;  // battery_critical: "charging" (ESP latch) | "charge_to_52v" (STM32 latch)
};

struct SocketIn {
  char id[13] = {};
  bool online = false;
  float current = 0;
  bool alertOvercurrent = false;  // socket tripped (LoRa ALERT:OVERCURRENT)
  bool alertWarning = false;      // socket >= 8 A (LoRa WARN:CURRENT_HIGH)
  bool lockout = false;            // socket latched off after 3 trips in 5 min (ALERT:LOCKOUT)
  float lockoutA = 0;              // last trip current (A)
  char cmd[12] = {};              // last command_status: pending|confirmed|rejected|timeout
};

struct Inputs {
  bool stmOnline = false, framed = false;  // framed = protocol v1 (status flags trusted)
  uint32_t fault = 0;
  SysState sys = SysState::Unknown;
  ChargeState chr = ChargeState::Unknown;
  bool hasIout = false; float iout = 0;
  bool hasTemp = false; float temp = 0;
  bool hasGrid = false; float gridV = 0;
  bool bmsOnline = false; uint32_t bmsAlarms = 0; float bmsCurrentA = 0;
  bool socValid = false; float soc = 0; const char *socSource = "bms";
  // SoC critical latch (ESP, soc_guard.h) and the STM32 reserve latch (legacy RSV:1)
  bool critLatched = false;
  bool stmReserve = false;
  static constexpr size_t kMaxSockets = 10;
  SocketIn sockets[kMaxSockets];
  size_t nSockets = 0;
};

class Engine {
 public:
  static constexpr size_t kConds = 64, kOut = 24;
  void update(const Inputs &in, uint32_t nowMs, uint32_t unixTs);
  // Transitions since the last drain, oldest first.
  size_t drain(Event *out, size_t max);
  size_t activeCount() const;
  const Event *active(size_t i) const;  // i-th active condition (0..activeCount-1)
  uint32_t dropped() const { return dropped_; }
  BatteryLevel batteryLevel() const { return bat_.level(); }

 private:
  struct Cond { bool used = false, active = false, raw = false; uint32_t since = 0; Event ev; };
  struct Spec {
    const char *id; Type type; Sev sev; bool hasValue; float value; const char *code;
    const char *socket; const char *source; bool permanent; uint32_t onMs, offMs;
    const char *releaseRequires;  // omitted in brace init: nullptr
  };
  Cond *find(const char *id, bool create);
  bool isActive(const char *id) const;
  void cond(const Spec &s, bool raw, uint32_t now, uint32_t ts);
  void push(const Event &e);
  Cond c_[kConds];
  Event out_[kOut];
  size_t outHead_ = 0, outN_ = 0;
  uint32_t dropped_ = 0;
  bool bit16Seen_ = false;
  bool restorePending_ = false, lastLatchStm_ = false;
  void oneShot(Type t, Sev sev, const char *id, const char *code, const char *source, bool hasValue, float value, uint32_t ts);  // framed STM32 proved it reports bit16
  BatteryAlertPolicy bat_;
};
}  // namespace ev
}  // namespace pc
