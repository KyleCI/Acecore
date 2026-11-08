#include "load_ac_energy_port.h"

#include "i2c_bus_lock.h"
#include "inverter.h"
#include "journal_files.h"
#include "load_ac_energy.h"
#include "load_ac_energy_upload.h"
#include "original_v01_profile.h"

// Operator opt-in is this exact model id. The shipped image leaves it empty.
#ifndef POWERCELL_REPORTED_VI_OPTIN
#define POWERCELL_REPORTED_VI_OPTIN ""
#endif

#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <Wire.h>
#include <string.h>
#include <time.h>

namespace {
constexpr const char *kJournalPath = "/load_ac_energy.jrnl";
constexpr const char *kJournalTmp = "/load_ac_energy.jrnl.tmp";
constexpr const char *kJournalBak = "/load_ac_energy.jrnl.bak";

class SpiffsDirectory : public loadac::JournalDirectory {
 public:
  bool exists(const char *path) const override { return path && SPIFFS.exists(path); }

  uint32_t file_size(const char *path) const override {
    if (!path || !SPIFFS.exists(path)) return 0;
    File in = SPIFFS.open(path, "r");
    if (!in) return 0;
    uint32_t n = (uint32_t)in.size();
    in.close();
    return n;
  }

  bool remove_file(const char *path) override {
    if (!path || !SPIFFS.exists(path)) return true;
    return SPIFFS.remove(path);
  }

  bool rename_file(const char *from, const char *to) override {
    if (!from || !to) return false;
    return SPIFFS.rename(from, to);
  }

  bool write_all(const char *path, const uint8_t *src, uint32_t n) override {
    if (!path || !src) return false;
    File out = SPIFFS.open(path, "w");
    if (!out) return false;
    size_t wrote = out.write(src, n);
    out.flush();
    out.close();
    return wrote == n;
  }

  bool read_at(const char *path, uint32_t offset, uint8_t *dst, uint32_t n) const override {
    if (!path || !dst) return false;
    File in = SPIFFS.open(path, "r");
    if (!in || !in.seek(offset, SeekSet)) {
      if (in) in.close();
      return false;
    }
    bool ok = in.read(dst, n) == (int)n;
    in.close();
    return ok;
  }
};

class SpiffsStore : public loadac::ByteStore {
 public:
  SpiffsStore() { memset(live_, 0, sizeof(live_)); }

  bool load_existing() { return loadac::journal_load(files_, kPaths, live_, sizeof(live_)); }

  bool read(uint32_t offset, uint8_t *dst, uint32_t n) override {
    if (!dst || offset + n > sizeof(live_)) return false;
    memcpy(dst, live_ + offset, n);
    return true;
  }

  bool write(uint32_t offset, const uint8_t *src, uint32_t n) override {
    if (!src || offset + n > sizeof(live_)) return false;
    memcpy(live_ + offset, src, n);
    return true;
  }

  uint32_t size() const override { return sizeof(live_); }

  // Host fault tests call journal_commit on an in-memory directory. This
  // adapter only compiles the same steps onto SPIFFS; it is not a power-cut test.
  bool persist() override {
    if (loadac::journal_commit(files_, kPaths, live_, sizeof(live_))) return true;
    loadac::journal_load(files_, kPaths, live_, sizeof(live_));
    return false;
  }

 private:
  SpiffsDirectory files_;
  uint8_t live_[loadac::kStoreBytes];
  static const loadac::JournalPaths kPaths;
};

const loadac::JournalPaths SpiffsStore::kPaths = {kJournalPath, kJournalBak, kJournalTmp};

SpiffsStore store;
loadac::Meter meter;
loadac::UploadSession upload;
char upload_frame[loadac::kUploadFrameMax];
char upload_diag[48];
loadac::I2cBackoff rtc_backoff;
bool rtc_wire_ready = false;
bool rtc_absent_logged = false;
bool journal_ready = false;
char device_id[16] = "unknown";

void copy_id(const String &mac) {
  size_t j = 0;
  for (size_t i = 0; i < mac.length() && j + 1 < sizeof(device_id); ++i) {
    char c = mac[i];
    if (c == ':') continue;
    if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
    device_id[j++] = c;
  }
  device_id[j] = 0;
  if (!device_id[0]) memcpy(device_id, "unknown", 8);
}

bool read_ds1307(uint8_t reg[7]) {
  Wire.beginTransmission(ORIGINAL_V01_DS1307_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)ORIGINAL_V01_DS1307_ADDR, 7) != 7) return false;
  for (int i = 0; i < 7; ++i) {
    if (!Wire.available()) return false;
    reg[i] = (uint8_t)Wire.read();
  }
  return true;
}

void ensure_wire() {
  if (rtc_wire_ready) return;
  Wire.begin(ORIGINAL_V01_I2C_SDA, ORIGINAL_V01_I2C_SCL);
  Wire.setClock(ORIGINAL_V01_I2C_HZ);
  rtc_wire_ready = true;
}

void poll_rtc() {
  uint32_t now = millis();
  if (!loadac::i2c_allow(rtc_backoff, now)) return;
  if (!i2c_bus_try_lock(15)) {
    loadac::i2c_fail(&rtc_backoff, now);
    return;
  }
  ensure_wire();
  Wire.setClock(ORIGINAL_V01_I2C_HZ);
  uint8_t first[7], second[7];
  bool ok = read_ds1307(first) && read_ds1307(second);
  i2c_bus_unlock();
  if (!ok) {
    loadac::i2c_fail(&rtc_backoff, now);
    if (!rtc_absent_logged) {
      rtc_absent_logged = true;
      Serial.println("[RTC] DS1307 not answering on I2C 21/22; clock unanchored. Electrical approval is still pending.");
    }
    return;
  }
  loadac::i2c_ok(&rtc_backoff);
  rtc_backoff.suppress_until_ms = now + 1000;
  rtc_absent_logged = false;
  meter.observe_rtc(first, second, now);
  loadac::RtcReading face;
  if (!loadac::ds1307_pair(first, second, &face) || !face.ok) return;
  if (meter.status().time_quality == loadac::TimeQuality::Anchored) return;
  time_t wall = time(nullptr);
  if (wall >= 1700000000) meter.agree_anchor(face.unix_utc, (int64_t)wall, now, 2);
}
}  // namespace

void note_upload_diag() {
  const char *diag = upload.diagnostic();
  if (!diag || strcmp(diag, upload_diag) == 0) return;
  strncpy(upload_diag, diag, sizeof(upload_diag) - 1);
  upload_diag[sizeof(upload_diag) - 1] = 0;
  Serial.printf("[AC] upload %s\n", upload_diag);
}

void load_ac_energy_begin() {
  memset(&rtc_backoff, 0, sizeof(rtc_backoff));
  memset(upload_diag, 0, sizeof(upload_diag));
  upload.reset(millis());
  copy_id(WiFi.macAddress());
  loadac::Stm32Observation observed = loadac::pinned_main_stm32_observation(millis());
  loadac::UnavailableReason why = loadac::UnavailableReason::SourceUnavailable;
  loadac::qualify_stm32(observed, &why);
  bool media = SPIFFS.begin(false) && store.load_existing();
  journal_ready = media && meter.begin(store, loadac::default_model());
  if (journal_ready) {
    meter.set_device_id(device_id);
    if (POWERCELL_REPORTED_VI_OPTIN[0]) meter.select_reported_vi(POWERCELL_REPORTED_VI_OPTIN);
  }
  meter.set_source_blocked(why);
  Serial.printf("[AC] live estimate blocked (%s). PF assumption %ld millis. Journal upload waits for the commit-backed /ws/device ACK.\n",
                loadac::reason_name(why), (long)loadac::kPfAssumedMillis);
  if (!journal_ready) Serial.println("[AC] journal was not committed to SPIFFS.");
}

void load_ac_energy_poll() {
  if (strcmp(device_id, "unknown") == 0) {
    copy_id(WiFi.macAddress());
    if (journal_ready && strcmp(device_id, "unknown") != 0) meter.set_device_id(device_id);
  }
  poll_rtc();
  if (journal_ready) {
    meter.poll_day(millis());
    if (meter.reported_vi_selected()) meter.note_xfer4_candidate(getStm32LoadCandidate());
  }
}

void load_ac_energy_fill(JsonObject dst, bool include_missing_path) {
  char raw[1600];
  const char *path = "stm32_uart1_rx33_tx2_legacy_text";
  uint32_t n = loadac::status_json(meter.status(), path, include_missing_path, raw, sizeof(raw));
  if (!n) {
    dst["contract"] = loadac::kContract;
    dst["estimate_available"] = false;
    dst["synthetic"] = false;
    dst["bms_dc_used"] = false;
    dst["pf_assumed_millis"] = loadac::kPfAssumedMillis;
    dst["pf_is_assumption"] = true;
    return;
  }
  JsonDocument parsed;
  if (deserializeJson(parsed, raw, n)) return;
  dst.set(parsed.as<JsonObject>());
}

void load_ac_energy_fill_brief(JsonObject dst) { load_ac_energy_fill(dst, false); }

void load_ac_energy_socket_link(bool up) {
  upload.on_link(up, millis());
  note_upload_diag();
}

const char *load_ac_energy_socket_arm() {
  if (!journal_ready) return 0;
  if (!upload.arm(meter, millis(), upload_frame, sizeof(upload_frame))) {
    note_upload_diag();
    return 0;
  }
  note_upload_diag();
  return upload_frame;
}

void load_ac_energy_socket_sent() {
  upload.sent(millis());
  note_upload_diag();
}

void load_ac_energy_socket_send_failed() {
  upload.send_failed(millis());
  note_upload_diag();
}

void load_ac_energy_socket_text(const uint8_t *payload, size_t length) {
  static char text[loadac::kUploadFrameMax];
  if (!journal_ready || !payload || !length || length >= sizeof(text)) return;
  memcpy(text, payload, length);
  text[length] = 0;
  upload.on_text(meter, text, millis());
  note_upload_diag();
}

void load_ac_energy_register(AsyncWebServer &server) {
  server.on("/load_ac_energy", HTTP_GET, [](AsyncWebServerRequest *request) {
    char raw[1600];
    uint32_t n = loadac::status_json(meter.status(), "stm32_uart1_rx33_tx2_legacy_text", true, raw, sizeof(raw));
    if (!n) {
      request->send(500, "application/json", "{\"contract\":\"estimate.load_ac_energy.v1\",\"estimate_available\":false}");
      return;
    }
    request->send(200, "application/json", raw);
  });
}
