#pragma once
#include <stdint.h>
namespace pc {
// What to do with an inverter on/off request. The ESP drives no relay; the
// STM32 owns it. Without a live STM32 an ON request is refused at once
// (stm32_timeout) instead of waiting on retries, and PWR_KEY (IO15) stays LOW.
// OFF is always safe: IO15 LOW, plus set_mode OFF when the STM32 is there.
enum class PowerAction : uint8_t { RefuseOta, RefuseStm32Offline, SendSetMode, DrivePwrKey, ReleasePwrKey };
inline PowerAction powerAction(bool on, bool otaBusy, bool stm32Online, bool framed) {
  if (on && otaBusy) return PowerAction::RefuseOta;
  if (!stm32Online) return on ? PowerAction::RefuseStm32Offline : PowerAction::ReleasePwrKey;
  if (framed) return PowerAction::SendSetMode;
  return on ? PowerAction::DrivePwrKey : PowerAction::ReleasePwrKey;
}
}  // namespace pc
