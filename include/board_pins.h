#pragma once

// Two hardware profiles share this tree.
//
// Original (default env esp32dev), unchanged:
//   UART0 console TX1/RX3, UART1 STM32 text RX33/TX2, UART2 JK-BMS RX17/TX16,
//   LoRa SPI SCK18/MISO19/MOSI23 CS4, LoRa reset GPIO26, power light GPIO25,
//   collector relay GPIO15.
//
// Hardware v0.1.1, stated by Caleb for this build:
//   No STM32 and no LoRa. The collector reads sensors and reports them.
//   UART0 console stays. UART2 BMS stays. UART1, freed from the STM32 link,
//   is the PZEM hardware UART, default RX25/TX26, 9600 8N1, configurable.
//   GPIO15 is the DSP enable, active low: Caleb, low is on and high is off.
//   That polarity is user-confirmed, not a measured proof of what the pin
//   does to charge current or essential power. It is not a power light and
//   not an AcePlug relay. DS3231 shares I2C SDA21/SCL22.
//   SD SPI SCK18/MISO19/MOSI23, chip select GPIO33.
//   GPIO2 (old STM32 TX) and GPIO4 (old LoRa CS) are released and unassigned.
//   Nothing here is a DSP-internal protocol.

#include <stdint.h>

namespace powercell {

enum class HardwareProfile : uint8_t { Original = 0, V011 = 1 };

constexpr int kUart0Tx = 1;
constexpr int kUart0Rx = 3;
constexpr int kStm32Rx = 33;
constexpr int kStm32Tx = 2;
constexpr int kBmsRx = 17;
constexpr int kBmsTx = 16;
constexpr int kPzemRx = 25;
constexpr int kPzemTx = 26;
constexpr int kI2cSda = 21;
constexpr int kI2cScl = 22;
constexpr int kDspEnableV011 = 15;
constexpr bool kDspActiveLow = true;
constexpr bool kDspPolarityUserConfirmed = true;
constexpr bool kPhysicalShutdownVerified = false;

// v0.1.1 only. 0 is on, 1 is off. The original relay stays active high.
inline int v011_dsp_level(bool logical_on) { return logical_on ? 0 : 1; }
inline int original_relay_level(bool logical_on) { return logical_on ? 1 : 0; }

enum class BootAction : uint8_t { SetOutputLatch = 0, EnableOutput = 1 };
struct BootStep {
  BootAction action;
  int level;
};
// Latch the off level before the pin becomes an output. GPIO15 is also a
// strap sampled by the boot ROM; this does not make that reset window quiet.
inline void v011_dsp_boot(BootStep out[2]) {
  out[0] = {BootAction::SetOutputLatch, v011_dsp_level(false)};
  out[1] = {BootAction::EnableOutput, v011_dsp_level(false)};
}

// v0.1.1 battery latch drives AC-output off only. Original relay auto-off stays off.
// Recovery never requests on. Charging and BMS FET registers are not written.
inline bool battery_ac_output_auto_off(HardwareProfile profile) {
  return profile == HardwareProfile::V011;
}
inline int battery_cutoff_pin_level(HardwareProfile profile, bool hold_off) {
  if (!hold_off || profile != HardwareProfile::V011) return -1;
  return v011_dsp_level(false);
}
inline bool battery_recovery_drives_on() { return false; }
inline bool battery_cutoff_writes_bms_fets() { return false; }
inline bool battery_cutoff_stops_charging() { return false; }
inline bool payment_lock_actuator_present() { return false; }
constexpr int kPowerLightOriginal = 25;
constexpr int kRelayOriginal = 15;
constexpr int kStatusRed = 13;
constexpr int kStatusYellow = 12;
constexpr int kStatusGreen = 27;
constexpr int kStatusAmber = 32;
constexpr int kLoraCs = 4;
constexpr int kLoraRst = 26;
constexpr int kStrapCs = 5;
constexpr int kSpiSck = 18;
constexpr int kSpiMiso = 19;
constexpr int kSpiMosi = 23;
constexpr int kSdCsV011 = 33;
constexpr int kAds1115Address = 0x48;
constexpr int kDs3231Address = 0x68;
constexpr int kPzemUartController = 1;
constexpr int kBmsUartController = 2;
constexpr int kConsoleUartController = 0;
constexpr bool kV011PzemIsSoftwareUart = false;

enum class PinRole : uint8_t {
  Disabled = 0,
  VerifiedSdCs,
  Unverified,
  Uart0,
  Stm32Rx,
  Stm32Tx,
  BmsUart,
  PzemUart,
  I2c,
  PowerLight,
  DspEnable,
  Relay,
  StatusLed,
  Strap,
  ModuleFlash,
  SpiSignal,
  LoraCs,
  LoraReset,
  InputOnly,
};

enum class DspConnection : uint8_t { NotFitted = 0, ExistingTextUart = 1, GpioEnableReported = 2 };

struct UartAssign {
  int controller;
  int rx;
  int tx;
  int baud;
  const char* role;
};

inline HardwareProfile build_profile() {
#ifdef POWERCELL_HW_V011
  return HardwareProfile::V011;
#else
  return HardwareProfile::Original;
#endif
}

inline DspConnection dsp_connection(HardwareProfile profile) {
  if (profile == HardwareProfile::Original) return DspConnection::ExistingTextUart;
  return DspConnection::GpioEnableReported;
}

// A missing DSP connector is not a collector fault. v0.1.1 has no DSP link.
// The original profile already has its own text parser; this flag must not
// invent an extra "STM32 absent" alarm.
inline bool missing_dsp_is_fault(HardwareProfile) { return false; }

inline bool diag_kind_records_dsp(uint8_t kind) {
  return kind == 13 || kind == 14;  // KindDiagStm32, KindDiagStm32Bad
}

inline bool diag_kind_allowed(HardwareProfile profile, uint8_t kind) {
  if (dsp_connection(profile) != DspConnection::ExistingTextUart && diag_kind_records_dsp(kind)) {
    return false;
  }
  return true;
}

inline void profile_uarts(HardwareProfile profile, UartAssign out[3]) {
  out[0] = {kConsoleUartController, kUart0Rx, kUart0Tx, 115200, "console"};
  out[2] = {kBmsUartController, kBmsRx, kBmsTx, 115200, "jk_bms"};
  if (profile == HardwareProfile::V011) {
    out[1] = {kPzemUartController, kPzemRx, kPzemTx, 9600, "pzem004t_v3_modbus"};
  } else {
    out[1] = {kPzemUartController, kStm32Rx, kStm32Tx, 9600, "stm32_text"};
  }
}

inline bool drives_stm32_reset(HardwareProfile) { return false; }

inline bool drives_gpio26_as_reset(HardwareProfile profile) {
  return profile == HardwareProfile::Original;
}

inline PinRole hard_pin_role(int pin, HardwareProfile profile) {
  if (pin < 0) return PinRole::Disabled;
  if (pin >= 34 && pin <= 39) return PinRole::InputOnly;
  if (pin >= 6 && pin <= 11) return PinRole::ModuleFlash;
  if (pin == 0 || pin == kStrapCs) return PinRole::Strap;
  if (pin == kUart0Tx || pin == kUart0Rx) return PinRole::Uart0;
  if (pin == kBmsRx || pin == kBmsTx) return PinRole::BmsUart;
  if (pin == kI2cSda || pin == kI2cScl) return PinRole::I2c;
  if (pin == kStatusYellow) return PinRole::Strap;
  if (pin == kStatusRed || pin == kStatusGreen || pin == kStatusAmber) return PinRole::StatusLed;
  if (pin == kSpiSck || pin == kSpiMiso || pin == kSpiMosi) return PinRole::SpiSignal;
  if (profile == HardwareProfile::V011) {
    if (pin == kSdCsV011) return PinRole::VerifiedSdCs;
    if (pin == kPzemRx || pin == kPzemTx) return PinRole::PzemUart;
    if (pin == kDspEnableV011) return PinRole::DspEnable;
    if (pin == kStm32Tx || pin == kLoraCs) return PinRole::Unverified;
    return PinRole::Unverified;
  }
  if (pin == kStm32Rx) return PinRole::Stm32Rx;
  if (pin == kStm32Tx) return PinRole::Stm32Tx;
  if (pin == kPowerLightOriginal) return PinRole::PowerLight;
  if (pin == kLoraRst) return PinRole::LoraReset;
  if (pin == kRelayOriginal) return PinRole::Relay;
  if (pin == kLoraCs) return PinRole::LoraCs;
  return PinRole::Unverified;
}

inline PinRole classify_sd_cs(int pin, HardwareProfile profile) {
  if (pin < 0) return PinRole::Disabled;
  return hard_pin_role(pin, profile);
}

inline bool sd_cs_is_buildable(int pin, HardwareProfile profile) {
  const PinRole role = classify_sd_cs(pin, profile);
  return role == PinRole::Disabled || role == PinRole::VerifiedSdCs;
}

inline int profile_sd_cs(HardwareProfile profile) {
  return profile == HardwareProfile::V011 ? kSdCsV011 : -1;
}

inline bool sd_spi_bus_available(HardwareProfile profile, bool lora_spi_started) {
  return profile == HardwareProfile::V011 && !lora_spi_started;
}

inline const char* pin_role_name(PinRole role) {
  switch (role) {
    case PinRole::Disabled: return "disabled";
    case PinRole::VerifiedSdCs: return "verified_sd_cs";
    case PinRole::Unverified: return "unverified";
    case PinRole::Uart0: return "uart0_console";
    case PinRole::Stm32Rx: return "stm32_uart_rx";
    case PinRole::Stm32Tx: return "stm32_uart_tx";
    case PinRole::BmsUart: return "bms_uart";
    case PinRole::PzemUart: return "pzem_hardware_uart";
    case PinRole::I2c: return "i2c_ads_ds3231";
    case PinRole::PowerLight: return "power_light";
    case PinRole::DspEnable: return "dsp_enable_reported";
    case PinRole::Relay: return "collector_relay";
    case PinRole::StatusLed: return "status_led";
    case PinRole::Strap: return "strap";
    case PinRole::ModuleFlash: return "module_flash";
    case PinRole::SpiSignal: return "spi_clock_or_data";
    case PinRole::LoraCs: return "lora_cs";
    case PinRole::LoraReset: return "lora_reset";
    case PinRole::InputOnly: return "input_only";
  }
  return "unknown";
}

inline bool pzem_pins_ok(HardwareProfile profile, int rx, int tx, int sd_cs) {
  if (profile != HardwareProfile::V011) return false;
  if (rx == tx || rx < 0 || tx < 0) return false;
  if (rx == sd_cs || tx == sd_cs) return false;
  const PinRole rx_role = hard_pin_role(rx, profile);
  const PinRole tx_role = hard_pin_role(tx, profile);
  return rx_role == PinRole::PzemUart && tx_role == PinRole::PzemUart;
}

}  // namespace powercell
