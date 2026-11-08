#pragma once

#include "battery_settings.h"

namespace powercell {

enum class AcModeSetting : uint8_t { WarnOnly = 0, WarnAndShutdown = 1 };

struct AcState {
  int schema = 1;
  int ceiling_w = 0;
  AcModeSetting mode = AcModeSetting::WarnOnly;
  uint32_t applied_revision = 0;
  bool durable = true;
  bool warn_operational = false;
  bool shutdown_operational = false;
  bool physical_shutdown_verified = false;
};

struct AcDecision {
  bool applied = false;
  bool durable_commit = false;
  bool pending = false;
  SettingsReason reason = SettingsReason::None;
  AcState readback;
};

class AcOutputSettings {
 public:
  static constexpr int kSchema = 1;
  static constexpr int kMinW = 50;
  static constexpr int kMaxW = 20000;

  AcDecision readback() const;
  void load(int ceiling_w, int mode, uint32_t revision);
  AcDecision set(SettingsAuth auth, int schema, bool ceiling_is_int, int ceiling_w,
                 bool mode_known, int mode, uint32_t base_revision, bool persist_ok);
  const AcState& state() const { return state_; }

 private:
  AcState state_;
};

}  // namespace powercell
