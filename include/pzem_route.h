#pragma once
// PZEM UART1 pin-routing check (DSP v0.1.1, RX GPIO35 / TX GPIO32). Pure logic,
// host-tested. The firmware reads the ESP32 GPIO matrix, IO_MUX and RTC mux
// registers and passes the raw values here, so a pin taken over after
// Serial1.begin() (ADC, RTC IO, another peripheral, pinMode) is visible and
// can be routed back. Bench readback: PZEM RAW.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace powercell {

constexpr uint32_t kU1RxdInIdx = 17;   // soc/gpio_sig_map.h U1RXD_IN_IDX
constexpr uint32_t kU1TxdOutIdx = 17;  // soc/gpio_sig_map.h U1TXD_OUT_IDX
constexpr uint32_t kPinFuncGpio = 2;   // ESP32 IO_MUX MCU_SEL for the GPIO function

struct PzemRouteRegs {
  bool driver = false;          // uart_is_driver_installed(UART_NUM_1)
  uint32_t rx_in_sel = 0;       // GPIO_FUNC17_IN_SEL_CFG_REG (U1RXD)
  uint32_t tx_out_sel = 0;      // GPIO_FUNC32_OUT_SEL_CFG_REG
  uint32_t rx_iomux = 0;        // IO_MUX reg of GPIO35
  uint32_t tx_iomux = 0;        // IO_MUX reg of GPIO32
  bool rx_rtc = false;          // RTC mux bit set on GPIO35 (pad owned by RTC/ADC)
  bool tx_rtc = false;          // RTC mux bit set on GPIO32
};

enum PzemRouteFault : uint16_t {
  kRouteOk = 0,
  kRouteNoDriver = 1u << 0,
  kRouteRxNotMatrix = 1u << 1,  // U1RXD not taken from the GPIO matrix
  kRouteRxWrongPin = 1u << 2,   // U1RXD reads another GPIO
  kRouteTxWrongSig = 1u << 3,   // GPIO32 does not output U1TXD
  kRouteRxMux = 1u << 4,        // GPIO35 IO_MUX not on the GPIO function
  kRouteRxNoIe = 1u << 5,       // GPIO35 input buffer off
  kRouteTxMux = 1u << 6,        // GPIO32 IO_MUX not on the GPIO function
  kRouteRxRtc = 1u << 7,        // GPIO35 handed to RTC IO (ADC)
  kRouteTxRtc = 1u << 8,        // GPIO32 handed to RTC IO (ADC / touch / 32k xtal)
};

inline uint16_t pzem_route_check(const PzemRouteRegs& r, uint32_t rx_pin) {
  uint16_t f = kRouteOk;
  if (!r.driver) f |= kRouteNoDriver;
  if (!((r.rx_in_sel >> 7) & 1u)) f |= kRouteRxNotMatrix;
  else if ((r.rx_in_sel & 0x3Fu) != rx_pin) f |= kRouteRxWrongPin;
  if ((r.tx_out_sel & 0x1FFu) != kU1TxdOutIdx) f |= kRouteTxWrongSig;
  if (((r.rx_iomux >> 12) & 0x7u) != kPinFuncGpio) f |= kRouteRxMux;
  if (!((r.rx_iomux >> 9) & 1u)) f |= kRouteRxNoIe;
  if (((r.tx_iomux >> 12) & 0x7u) != kPinFuncGpio) f |= kRouteTxMux;
  if (r.rx_rtc) f |= kRouteRxRtc;
  if (r.tx_rtc) f |= kRouteTxRtc;
  return f;
}

// Comma list of fault names into out ("ok" when none). Returns out.
inline const char* pzem_route_names(uint16_t f, char* out, size_t cap) {
  static const char* const kNames[] = {"no_driver", "rx_not_matrix", "rx_wrong_pin", "tx_wrong_sig", "rx_mux",
                                       "rx_no_ie", "tx_mux", "rx_rtc", "tx_rtc"};
  if (!out || !cap) return out;
  out[0] = 0;
  if (!f) { snprintf(out, cap, "ok"); return out; }
  size_t n = 0;
  for (unsigned i = 0; i < sizeof kNames / sizeof kNames[0]; ++i) {
    if (!(f & (1u << i))) continue;
    int w = snprintf(out + n, cap > n ? cap - n : 0, "%s%s", n ? "," : "", kNames[i]);
    if (w < 0) break;
    n += static_cast<size_t>(w);
    if (n >= cap) { out[cap - 1] = 0; break; }
  }
  return out;
}

// "F8 04 14 ..." hex dump, space separated. Returns characters written.
inline size_t pzem_hex(const uint8_t* b, size_t n, char* out, size_t cap) {
  if (!out || !cap) return 0;
  out[0] = 0;
  size_t w = 0;
  for (size_t i = 0; i < n && w + 3 < cap; ++i) {
    snprintf(out + w, cap - w, "%s%02X", i ? " " : "", b[i]);
    w += i ? 3 : 2;
  }
  return w;
}

}  // namespace powercell
