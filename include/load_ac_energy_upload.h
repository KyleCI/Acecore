#pragma once

#include "load_ac_energy.h"

#include <stdint.h>

namespace loadac {

// Device-socket upload of journaled estimate.load_ac_energy.v1 records.
//
// The frame is {"type":"energy_estimate","record":<stored bytes>}. The stored
// bytes are not rewritten, rescaled, or given a new identity for retry.
//
// A record leaves the pending journal only after the socket text is the
// commit-backed ACK: status success, ack boolean true, replay boolean,
// contract and stream exact, record_id an integer >= 1, and the same
// device_id, meter_epoch, boot_id, seq, clock_correction_version, and
// simulation as the stored body. The slot is marked retired only when that
// image has been persisted. HTTP success and {"status":"success"} are not ACKs.
//
// kAckWaitMs is the existing device-socket reply bound (the socket notes that
// the device falls back after 20 s). The retry bounds match this firmware's
// websocket reconnect (5 s) and the I2C backoff cap (60 s). They are not a
// sensor calibration.

constexpr uint32_t kAckWaitMs = 20000;
constexpr uint32_t kUploadRetryMinMs = 5000;
constexpr uint32_t kUploadRetryMaxMs = 60000;
constexpr uint32_t kUploadFrameMax = 1900;

class UploadSession {
 public:
  UploadSession();
  void reset(uint32_t now_ms);
  void on_link(bool up, uint32_t now_ms);
  // Copy one wrapped frame for the oldest sendable pending record.
  bool arm(Meter &meter, uint32_t now_ms, char *dst, uint32_t cap);
  void sent(uint32_t now_ms);
  void send_failed(uint32_t now_ms);
  void on_text(Meter &meter, const char *json, uint32_t now_ms);
  bool inflight() const { return phase_ == Phase::Inflight; }
  bool link_up() const { return link_; }
  const char *diagnostic() const { return diagnostic_; }
  uint32_t last_record_id() const { return last_record_id_; }
  const char *armed_body() const { return body_; }

 private:
  enum class Phase : uint8_t { Idle = 0, Armed, Inflight };

  void set_diag(const char *token);
  void backoff(uint32_t now_ms);
  bool ready(uint32_t now_ms) const;

  Phase phase_;
  bool link_;
  uint32_t ready_at_;
  uint32_t sent_ms_;
  uint32_t backoff_ms_;
  uint32_t last_record_id_;
  uint32_t boot_u32_;
  uint64_t seq_u64_;
  uint32_t clock_version_;
  bool simulation_;
  char device_id_[121];
  char meter_epoch_[24];
  char boot_id_[65];
  char seq_[24];
  char diagnostic_[48];
  char body_[kDefaultSlotBytes];
};

}  // namespace loadac
