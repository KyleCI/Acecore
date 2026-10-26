#pragma once
#include <stdint.h>

namespace pc {
// Values mirror the STM32 firmware (system_control.c / fault_manager.h).
enum class SysState : uint8_t { Unknown = 0, Idle = 1, Grid = 2, Inverter = 3, Fault = 4,
  Reserve = 5 };  // ESP-side code for legacy "SYS : RESERVE" (framed sys 5 too, agreed with hardware; plus fault bit 20)
enum class SwitchPos : uint8_t { Unknown = 0, Inverter = 1, Grid = 2 };
enum class ChargeState : uint8_t { Unknown = 0, Off = 1, Idle = 2, Wait = 3, Bulk = 4, Absorb = 5, Float = 6, Fault = 7 };

// Fault bits (fault_manager.h)
// fault_bits is u32 on the wire (status v1) and everywhere on the ESP32; bits
// above 7 are named by the STM32 side as they are added.
enum FaultBits : uint32_t {
  FAULT_OVERCURRENT = 0x01, FAULT_SHORT = 0x02, FAULT_DC_OV = 0x04, FAULT_DC_UV = 0x08,
  FAULT_OUT_OV = 0x10, FAULT_THERMAL = 0x20, FAULT_NTC_OPEN = 0x40, FAULT_NO_FEEDBACK = 0x80,
};

enum TransferFlags : uint8_t { TRANSFER_ACTIVE = 0x01, TRANSFER_GRID_SYNCED = 0x02, TRANSFER_VOUT_CALIBRATED = 0x04 };

// One coherent snapshot of what the STM32 reported. Fields the STM32 did not
// send in the latest record have their has_* flag cleared.
struct Stm32Status {
  SysState sys = SysState::Unknown;
  SwitchPos sw = SwitchPos::Unknown;
  ChargeState chr = ChargeState::Unknown;
  float bat_v = 0;      bool has_bat = false;
  uint8_t bat_pct = 0;  bool has_bat_pct = false;
  float grid_v = 0;     bool has_grid = false;
  float temp_c = 0;     bool has_temp = false;
  float vout = 0;       bool has_vout = false;
  float iout = 0;       bool has_iout = false;
  uint32_t fault = 0;   // latched fault bits (u32); 0 when none reported
  // Legacy "RSV:0/1" line (after GRID:) or SYS : RESERVE; framed fault_bits bit 20 (status, not a fault).: the STM32 low-battery reserve latch (inverter off at 47.5 V for 60 s;
  // released at Vbat >= 52.0 V held 60 s from any charge source, or a power cycle).
  bool rsv_latched = false; bool has_rsv = false;
  // Framed status v1 only (legacy text leaves has_ext = false):
  bool has_ext = false;
  uint8_t restart_count = 0;
  uint8_t transfer_flags = 0;  // TRANSFER_* bits
  float chg_current = 0;       // A
  float mod_index = 0;         // 0..6.5535
  uint32_t uptime_s = 0;
  uint32_t seq = 0;     // increments on every completed record
};

const char *sysStateName(SysState s);
const char *switchName(SwitchPos s);
const char *chargeName(ChargeState s);
}  // namespace pc
