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
                 bool mode_known, int mode, int usable_w, uint32_t base_revision, bool persist_ok);
  const AcState& state() const { return state_; }

 private:
  AcState state_;
};

enum class RatingAvailability : uint8_t {
  Armed = 0,
  Unprovisioned,
  Unknown,
  Mismatch,
  InvalidStore
};

struct RatingState {
  int schema = 1;
  int factory_rated_w = 0;
  int usable_active_w = 0;
  bool store_present = false;
  int stored_rated_w = 0;
  RatingAvailability availability = RatingAvailability::Unprovisioned;
  const char* variant = "unrated";
  bool overload_configured = false;
  bool physical_shutdown_verified = false;
};

// The rated watts are fixed when the object is created from the factory image.
// observeStore records a leftover NVS value and never writes it.
class SystemRating {
 public:
  static constexpr int kRated5kW = 5000;
  static constexpr int kRated35kW = 3500;
  static constexpr int kUsable5kW = 4500;
  static constexpr int kUsable35kW = 3150;
  static int usableFor(int rated_active_w);

  SystemRating(int factory_rated_w, const char* variant);
  void observeStore(bool present, int stored_rated_w);
  bool armed() const { return state_.overload_configured; }
  int usableW() const { return state_.usable_active_w; }
  const char* reason() const;
  const RatingState& state() const { return state_; }

 private:
  void evaluate();
  int factory_rated_w_ = 0;
  const char* variant_ = "unrated";
  bool store_present_ = false;
  int stored_rated_w_ = 0;
  RatingState state_;
};

}  // namespace powercell
