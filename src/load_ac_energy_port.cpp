#include "load_ac_energy_port.h"

#include "i2c_bus_lock.h"
#include "load_ac_energy.h"
#include "load_ac_energy_upload.h"
#include "original_v01_profile.h"

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

class SpiffsStore : public loadac::ByteStore {
 public:
  SpiffsStore() { memset(live_, 0, sizeof(live_)); }

  bool load_existing() {
    if (load_file(kJournalPath)) return true;
    if (load_file(kJournalBak)) return true;
    return !SPIFFS.exists(kJournalPath) && !SPIFFS.exists(kJournalBak);
  }

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

  bool persist() override {
    if (!write_verified(kJournalTmp)) {
      reload();
      return false;
    }
    if (SPIFFS.exists(kJournalPath)) {
      SPIFFS.remove(kJournalBak);
      if (!SPIFFS.rename(kJournalPath, kJournalBak)) {
        reload();
        return false;
      }
    }
    if (!SPIFFS.rename(kJournalTmp, kJournalPath) || !matches_file(kJournalPath)) {
      SPIFFS.remove(kJournalPath);
      if (SPIFFS.exists(kJournalBak)) SPIFFS.rename(kJournalBak, kJournalPath);
      reload();
      return false;
    }
    return true;
  }

 private:
  bool write_verified(const char *path) {
    File out = SPIFFS.open(path, "w");
    if (!out) return false;
    size_t n = out.write(live_, sizeof(live_));
    out.flush();
    out.close();
    return n == sizeof(live_) && matches_file(path);
  }

  bool matches_file(const char *path) {
    File in = SPIFFS.open(path, "r");
    if (!in) return false;
    uint8_t chunk[128];
    uint32_t off = 0;
    bool ok = true;
    while (off < sizeof(live_)) {
      uint32_t want = sizeof(live_) - off;
      if (want > sizeof(chunk)) want = sizeof(chunk);
      if (in.read(chunk, want) != (int)want || memcmp(chunk, live_ + off, want) != 0) {
        ok = false;
        break;
      }
      off += want;
    }
    in.close();
    return ok;
  }

  bool load_file(const char *path) {
    if (!SPIFFS.exists(path)) return false;
    File in = SPIFFS.open(path, "r");
    if (!in) return false;
    size_t n = in.read(live_, sizeof(live_));
    in.close();
    return n == sizeof(live_);
  }

  void reload() {
    memset(live_, 0, sizeof(live_));
    if (load_file(kJournalPath)) return;
    memset(live_, 0, sizeof(live_));
    load_file(kJournalBak);
  }

  uint8_t live_[loadac::kStoreBytes];
};

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
  if (journal_ready) meter.set_device_id(device_id);
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
  if (journal_ready) meter.poll_day(millis());
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
