#include "hw_v011.h"

#ifdef POWERCELL_HW_V011

#include "ac_energy_log.h"
#include "ac_output_policy.h"
#include "board_pins.h"
#include "diag_blackbox.h"
#include "ds3231_time.h"
#include "lagos_time.h"
#include "i2c_bus.h"
#include "pzem_v3.h"
#include "sd_log.h"
#include "serial_spool.h"

#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <time.h>
#include <string.h>

namespace {
constexpr uint32_t kFlashSector = 4096;
constexpr uint32_t kSdBlocks = 1024;  // 512 KiB preallocated file, not the whole card
#ifndef PZEM_ADDRESS
#define PZEM_ADDRESS 0xF8
#endif

class PartitionStore : public powercell::ByteStore {
 public:
  explicit PartitionStore(const esp_partition_t* part) : part_(part) {}
  uint32_t size() const override { return part_ ? part_->size : 0; }
  bool read(uint32_t offset, uint8_t* dst, uint32_t len) const override {
    return part_ && esp_partition_read(part_, offset, dst, len) == ESP_OK;
  }
  bool write(uint32_t offset, const uint8_t* src, uint32_t len) override {
    return part_ && esp_partition_write(part_, offset, src, len) == ESP_OK;
  }
  bool erase(uint32_t offset, uint32_t len) override {
    return part_ && esp_partition_erase_range(part_, offset, len) == ESP_OK;
  }

 private:
  const esp_partition_t* part_;
};

class FileCard : public powercell::BlockDevice {
 public:
  bool begin() {
    SPI.begin(powercell::kSpiSck, powercell::kSpiMiso, powercell::kSpiMosi, POWERCELL_SD_CS);
    if (!SD.begin(POWERCELL_SD_CS, SPI, 1000000)) return false;
    file_ = SD.open("/blackbox.bin", "r+");
    if (!file_) file_ = SD.open("/blackbox.bin", "w+");
    if (!file_) return false;
    const uint32_t bytes = kSdBlocks * powercell::kSdBlockBytes;
    if (file_.size() < bytes) {
      uint8_t blank[256];
      memset(blank, 0xFF, sizeof blank);
      file_.seek(file_.size());
      while (file_.size() < bytes) {
        const uint32_t left = bytes - file_.size();
        const size_t n = left > sizeof blank ? sizeof blank : left;
        if (file_.write(blank, n) != n) {
          ready_ = false;
          return false;
        }
        vTaskDelay(1);
      }
      file_.flush();
    }
    ready_ = true;
    return true;
  }
  uint32_t block_count() const override { return ready_ ? kSdBlocks : 0; }
  bool present() const override { return ready_; }
  bool read(uint32_t block, uint8_t* dst) override {
    if (!ready_ || !file_.seek(block * powercell::kSdBlockBytes)) return false;
    return file_.read(dst, powercell::kSdBlockBytes) == powercell::kSdBlockBytes;
  }
  bool write(uint32_t block, const uint8_t* src) override {
    if (!ready_ || !file_.seek(block * powercell::kSdBlockBytes)) {
      ready_ = false;
      return false;
    }
    if (file_.write(src, powercell::kSdBlockBytes) != powercell::kSdBlockBytes) {
      ready_ = false;
      return false;
    }
    return true;
  }
  bool flush() override {
    if (!ready_) return false;
    file_.flush();
    return true;
  }

 private:
  File file_;
  bool ready_ = false;
};

struct Live {
  bool valid = false;
  char reason[24] = "no_reading";
  uint32_t wh = 0;
  uint32_t power_dW = 0;
  uint32_t power_at_ms = 0;
  uint32_t voltage_dV = 0;
  uint32_t current_mA = 0;
  uint32_t epoch = 1;
  uint8_t clock = 0;
  bool sd = false;
  bool flash = false;
  uint32_t dropped = 0;
  uint32_t diag_retained = 0;
  uint32_t allocated_wh = 0;
  uint32_t unallocated_wh = 0;
  bool store_full = false;
};

SemaphoreHandle_t live_mu;
SemaphoreHandle_t queue_mu;
SemaphoreHandle_t energy_mu;
powercell::DiagQueue* queue = nullptr;
Live live;
PartitionStore* energy_store = nullptr;
PartitionStore* diag_store = nullptr;
powercell::MetaMirror* meta = nullptr;
powercell::SlotLog* energy_slots = nullptr;
powercell::SlotLog* diag_slots = nullptr;
powercell::EnergyLog* energy = nullptr;
FileCard* card = nullptr;
powercell::SdLog* sd = nullptr;
powercell::UploadCursor upload;
bool export_suppress = false;
uint32_t queue_drops = 0;
bool rtc_seen = false;
int64_t rtc_unix = 0;
bool measurement_gap = false;

const char* pzem_reason(powercell::PzemStatus status) {
  switch (status) {
    case powercell::PzemStatus::Ok: return "ok";
    case powercell::PzemStatus::Timeout: return "timeout";
    case powercell::PzemStatus::BadCrc: return "bad_crc";
    case powercell::PzemStatus::BadAddress: return "bad_address";
    case powercell::PzemStatus::Exception: return "exception";
    case powercell::PzemStatus::UnsupportedFraming: return "bad_framing";
    case powercell::PzemStatus::Short: return "short";
  }
  return "missing";
}

void publish_live(const Live& next) {
  if (live_mu && xSemaphoreTake(live_mu, pdMS_TO_TICKS(20)) == pdTRUE) {
    live = next;
    xSemaphoreGive(live_mu);
  }
}

bool enqueue(const powercell::SlotRecord& record) {
  if (export_suppress || !queue || !queue_mu) return false;
  if (xSemaphoreTake(queue_mu, 0) != pdTRUE) {
    ++queue_drops;
    return false;
  }
  const bool ok = queue->try_push(record);
  if (!ok) ++queue_drops;
  xSemaphoreGive(queue_mu);
  return ok;
}

void note_text(uint8_t kind, const char* text) {
  powercell::SlotRecord rec;
  rec.kind = kind;
  rec.mono_ms = millis();
  rec.recv_unix = time(nullptr) >= 1700000000 ? time(nullptr) : 0;
  rec.clock_quality = rec.recv_unix ? static_cast<uint8_t>(powercell::ClockQuality::Anchored) : 0;
  const size_t n = text ? strlen(text) : 0;
  rec.len = static_cast<uint8_t>(n > 63 ? 63 : n);
  if (n) memcpy(rec.payload, text, rec.len);
  if (n > 63) rec.flags = powercell::FlagTruncatedText;
  enqueue(rec);
}

enum class RtcIo { Ok, Nack, Short, Busy };

RtcIo read_ds3231(uint8_t frame[16]) {
  if (!i2c_bus_lock(20)) return RtcIo::Busy;
  Wire.setClock(100000);
  Wire.beginTransmission(powercell::kDs3231Address);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) {
    i2c_bus_unlock();
    return RtcIo::Nack;
  }
  const uint8_t got = Wire.requestFrom(powercell::kDs3231Address,
                                       static_cast<uint8_t>(powercell::kDs3231FrameBytes));
  if (got != powercell::kDs3231FrameBytes) {
    i2c_bus_unlock();
    return RtcIo::Short;
  }
  for (size_t i = 0; i < powercell::kDs3231FrameBytes; ++i) frame[i] = static_cast<uint8_t>(Wire.read());
  i2c_bus_unlock();
  return RtcIo::Ok;
}

bool write_regs(uint8_t reg, const uint8_t* data, size_t len) {
  Wire.beginTransmission(powercell::kDs3231Address);
  Wire.write(reg);
  Wire.write(data, len);
  return Wire.endTransmission(true) == 0;
}

bool write_ds3231(int64_t unix_utc) {
  uint8_t time_bytes[7], control = 0, status = 0;
  if (!powercell::ds3231_encode(unix_utc, time_bytes, &control, &status)) return false;
  if (!i2c_bus_lock(20)) return false;
  Wire.setClock(100000);
  const bool wrote = write_regs(0x00, time_bytes, 7) && write_regs(0x0E, &control, 1) &&
                     write_regs(0x0F, &status, 1);
  i2c_bus_unlock();
  if (!wrote) return false;
  uint8_t first[16], second[16];
  if (read_ds3231(first) != RtcIo::Ok || read_ds3231(second) != RtcIo::Ok) return false;
  return powercell::ds3231_readback_matches(powercell::ds3231_decode_pair(first, second), unix_utc);
}

void poll_clock_and_meter() {
  uint8_t first[16], second[16];
  const RtcIo io_a = read_ds3231(first);
  const RtcIo io_b = io_a == RtcIo::Ok ? read_ds3231(second) : io_a;
  powercell::RtcReading rtc;
  rtc.status = powercell::RtcStatus::Incoherent;
  if (io_a == RtcIo::Ok && io_b == RtcIo::Ok) rtc = powercell::ds3231_decode_pair(first, second);
  else if (io_a == RtcIo::Nack || io_b == RtcIo::Nack) rtc.status = powercell::RtcStatus::Incoherent;
  else if (io_a == RtcIo::Short || io_b == RtcIo::Short) rtc.status = powercell::RtcStatus::ShortRead;
  const time_t wall = time(nullptr);
  const bool ntp = wall >= 1700000000;
  const bool skew = rtc.status == powercell::RtcStatus::Unanchored &&
                    (rtc.unix_utc > wall + 3 || wall > rtc.unix_utc + 3);
  const bool chip_invalid = rtc.status == powercell::RtcStatus::OscillatorStopped ||
                            rtc.status == powercell::RtcStatus::BadBcd ||
                            rtc.status == powercell::RtcStatus::BadCalendar ||
                            rtc.status == powercell::RtcStatus::BadRegister;
  if (ntp && (skew || chip_invalid)) {
    if (skew && energy) energy->note_anchor(rtc.unix_utc, wall, 180);
    note_text(powercell::KindDiagClock, write_ds3231(wall) ? "rtc_set_from_ntp" : "rtc_set_rejected");
  } else if (rtc.status == powercell::RtcStatus::Unanchored) {
    rtc_seen = true;
    rtc_unix = rtc.unix_utc;
  } else if (!rtc_seen) {
    rtc_seen = true;
    const char* reason = "rtc_invalid";
    if (io_a == RtcIo::Nack || io_b == RtcIo::Nack) reason = "rtc_nack";
    else if (rtc.status == powercell::RtcStatus::ShortRead) reason = "rtc_short";
    else if (rtc.status == powercell::RtcStatus::OscillatorStopped) reason = "rtc_oscillator_stopped";
    note_text(powercell::KindDiagClock, reason);
  }

  powercell::PzemConfig cfg;
  cfg.address = PZEM_ADDRESS;
  cfg.timeout_ms = 200;
  uint8_t request[8];
  const size_t n = powercell::pzem_build_read(cfg, request, sizeof request);
  while (Serial1.available()) Serial1.read();
  if (n) Serial1.write(request, n);
  uint8_t response[32];
  size_t got = 0;
  const uint32_t start = millis();
  while (got < 25 && millis() - start < cfg.timeout_ms) {
    if (Serial1.available()) response[got++] = static_cast<uint8_t>(Serial1.read());
    else vTaskDelay(1);
  }
  const bool timed_out = got < 25;
  const powercell::PzemReading reading = powercell::pzem_parse(cfg, response, got, timed_out);
  powercell::MeterSample sample;
  sample.mono_ms = millis();
  sample.mono_continuous = true;
  sample.measurement_gap = measurement_gap;
  if (ntp) {
    sample.unix_s = wall;
    sample.clock = static_cast<uint8_t>(powercell::ClockQuality::Anchored);
  } else if (rtc.status == powercell::RtcStatus::Unanchored) {
    sample.unix_s = rtc.unix_utc;
    sample.clock = static_cast<uint8_t>(powercell::ClockQuality::RtcUnanchored);
    sample.margin_s = 180;
  }
  uint32_t wh = 0;
  sample.valid = powercell::pzem_wh(reading, &wh);
  if (sample.valid) sample.lifetime_wh = wh;
  measurement_gap = !sample.valid;
  Live next = live;
  if (energy && energy_mu && xSemaphoreTake(energy_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
    energy->observe(sample);
    next.epoch = energy->epoch();
    next.allocated_wh = energy->sum_allocated();
    next.unallocated_wh = energy->sum_unallocated();
    next.store_full = energy->store_full();
    xSemaphoreGive(energy_mu);
  }
  next.valid = sample.valid;
  strncpy(next.reason, sample.valid ? "ok" : pzem_reason(reading.status), sizeof next.reason);
  next.reason[sizeof next.reason - 1] = 0;
  if (sample.valid) {
    next.wh = wh;
    next.power_dW = reading.power_dW;
    next.power_at_ms = millis();
    next.voltage_dV = reading.voltage_dV;
    next.current_mA = reading.current_mA;
  }
  next.clock = sample.clock;
  next.sd = sd && sd->stats().ready;
  next.flash = diag_slots != nullptr;
  next.dropped = queue_drops;
  if (diag_slots) next.diag_retained = diag_slots->stats().retained;
  publish_live(next);
  powercell::AcWatts ceiling;
  ceiling.valid = next.valid;
  ceiling.coverage_verified = false;
  ceiling.watts = next.valid ? next.power_dW / 10.0f : 0.f;
  ceiling.receivedAtMs = next.valid ? next.power_at_ms : 0;
  powercell::acOutputObserve(ceiling, millis());
  static uint32_t last_bad = 0;
  if (!sample.valid && millis() - last_bad > 10000) {
    last_bad = millis();
    note_text(powercell::KindDiagPzemBad, next.reason);
  }
}

void open_storage() {
  const esp_partition_t* energy_part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x9A), "acenergy");
  const esp_partition_t* diag_part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x99), "blackbox");
  if (energy_part && energy_part->size > kFlashSector * 3) {
    energy_store = new PartitionStore(energy_part);
    meta = new powercell::MetaMirror(*energy_store, kFlashSector);
    powercell::SlotLogConfig cfg;
    cfg.sector_size = kFlashSector;
    cfg.allow_overwrite = false;
    energy_slots = new powercell::SlotLog(*energy_store, kFlashSector * 2, energy_part->size - kFlashSector * 2, cfg);
    powercell::EnergyConfig energy_cfg;
    energy = new powercell::EnergyLog(*energy_slots, *meta, energy_cfg);
    energy->load();
  } else {
    note_text(powercell::KindDiagStorage, "energy_partition_missing");
  }
  if (diag_part && diag_part->size > kFlashSector) {
    diag_store = new PartitionStore(diag_part);
    powercell::SlotLogConfig cfg;
    cfg.sector_size = kFlashSector;
    cfg.allow_overwrite = true;
    cfg.preserve_last = 32;
    diag_slots = new powercell::SlotLog(*diag_store, 0, diag_part->size, cfg);
    diag_slots->recover();
  } else {
    note_text(powercell::KindDiagStorage, "diag_partition_missing");
  }
  card = new FileCard();
  if (card->begin()) {
    sd = new powercell::SdLog(*card, true);
    sd->recover();
    note_text(powercell::KindDiagStorage, "sd_ready");
  } else {
    note_text(powercell::KindDiagStorage, "sd_absent");
  }
}

void drain_queue() {
  if (!queue || !queue_mu) return;
  for (int i = 0; i < 4; ++i) {
    powercell::SlotRecord rec;
    if (xSemaphoreTake(queue_mu, pdMS_TO_TICKS(5)) != pdTRUE) return;
    const bool have = queue->pop(&rec);
    xSemaphoreGive(queue_mu);
    if (!have) return;
    powercell::route_diag(powercell::HardwareProfile::V011, sd, diag_slots, rec);
  }
}

void recorder_task(void*) {
  open_storage();
  const esp_reset_reason_t reason = esp_reset_reason();
  char boot[64];
  snprintf(boot, sizeof boot, "boot model=hw-v0.1.1 fw=%s reset=%d", POWERCELL_FIRMWARE_VERSION, static_cast<int>(reason));
  note_text(powercell::KindDiagBoot, boot);
  note_text(powercell::KindDiagReset, boot);
  if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
      reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT) {
    if (diag_slots) diag_slots->latch_incident();
    if (sd) sd->latch_incident(32);
    note_text(powercell::KindDiagIncident, "previous_reset_latched");
  }
  uint32_t last_poll = 0;
  uint32_t last_health = 0;
  for (;;) {
    const uint32_t now = millis();
    if (now - last_poll >= 1000) {
      last_poll = now;
      poll_clock_and_meter();
    }
    drain_queue();
    if (sd) powercell::console_spool().drain(sd, 4);
    if (now - last_health >= 30000) {
      last_health = now;
      char health[64];
      snprintf(health, sizeof health, "sd=%d drops=%lu full=%d", sd && sd->stats().ready ? 1 : 0,
               static_cast<unsigned long>(queue_drops), energy && energy->store_full() ? 1 : 0);
      note_text(powercell::KindDiagStorage, health);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void export_records(Print& out, bool json) {
  (void)json;
  powercell::console_spool().set_suppressed(true);
  export_suppress = true;
  if (sd) powercell::console_spool().drain(sd, 32);
  char body[1800];
  if (sd && sd->stats().ready) {
    powercell::console_spool().export_sd(sd, body, sizeof body);
    out.println(json ? "source=sd_serial" : "source=sd serial");
  } else {
    snprintf(body, sizeof body, "sd_unavailable\n");
    powercell::console_spool().export_recent(body + strlen(body), sizeof body - strlen(body));
    out.println("source=ram_recent");
  }
  out.println(body);
  export_suppress = false;
  powercell::console_spool().set_suppressed(false);
}
}  // namespace

void hwV011Start() {
  live_mu = xSemaphoreCreateMutex();
  queue_mu = xSemaphoreCreateMutex();
  queue = new powercell::DiagQueue(32);
  energy_mu = xSemaphoreCreateMutex();
  upload.set_contract(false);
  xTaskCreatePinnedToCore(recorder_task, "hwv011", 8192, nullptr, 1, nullptr, 1);
}

void hwV011NoteState(bool on) { note_text(powercell::KindDiagState, on ? "state_on" : "state_off"); }

void hwV011NoteBms(bool ok, float volts, float amps, int soc) {
  static uint32_t last = 0;
  if (!ok) {
    hwV011NoteBmsFail("stale");
    return;
  }
  if (millis() - last < 5000) return;
  last = millis();
  char line[64];
  snprintf(line, sizeof line, "v=%.2f a=%.2f soc=%d", volts, amps, soc);
  note_text(powercell::KindDiagBms, line);
}

void hwV011NoteBmsFail(const char* reason) {
  static uint32_t last = 0;
  if (millis() - last < 10000) return;
  last = millis();
  note_text(powercell::KindDiagBmsBad, reason ? reason : "bms_missing");
}

void hwV011NotePv(float volts, float amps, float watts) {
  static uint32_t last = 0;
  if (millis() - last < 5000) return;
  last = millis();
  char line[64];
  snprintf(line, sizeof line, "v=%.2f a=%.3f w=%.1f", volts, amps, watts);
  note_text(powercell::KindDiagPv, line);
}

void hwV011Routes(AsyncWebServer& server) {
  server.on("/diag/export", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasHeader("API-KEY")) {
      req->send(401, "text/plain", "unauthorized");
      return;
    }
    Preferences creds;
    creds.begin("powercell_ota", true);
    String saved = creds.getString("api_key");
    creds.end();
    if (saved.isEmpty() || req->header("API-KEY") != saved) {
      req->send(401, "text/plain", "unauthorized");
      return;
    }
    String body;
    export_suppress = true;
    if (diag_slots) {
      uint32_t shown = 0;
      for (uint32_t i = 0; i < diag_slots->capacity() && shown < 40; ++i) {
        powercell::SlotRecord rec;
        if (!diag_slots->at(i, &rec)) continue;
        char line[160];
        powercell::format_diag_text(rec, line, sizeof line);
        body += line;
        body += '\n';
        ++shown;
      }
    } else {
      body = "storage_unavailable\n";
    }
    export_suppress = false;
    req->send(200, "text/plain", body);
  });
}

bool hwV011ConsumeSerial(const String& line) {
  if (line.startsWith("BBEXPORT") || line.startsWith("BBTAIL")) {
    export_records(Serial, line.startsWith("BBEXPORT"));
    return true;
  }
  return false;
}

void hwV011AppendTelemetry(JsonDocument& doc) {
  Live snap;
  if (live_mu && xSemaphoreTake(live_mu, pdMS_TO_TICKS(10)) == pdTRUE) {
    snap = live;
    xSemaphoreGive(live_mu);
  }
  JsonObject ac = doc["ac_consumed"].to<JsonObject>();
  ac["schema"] = 1;
  ac["quantity"] = "active_energy_wh";
  ac["meter"] = "pzem004t_v3";
  ac["ct_a"] = 100;
  ac["uart"] = "uart1_rx25_tx26";
  ac["boundary"] = "intended_common_ac_load_unverified";
  ac["ceiling_source"] = "pzem004t_v3_active_power";
  ac["freshness_ms"] = 2000;
  ac["total_output_coverage_verified"] = false;
  ac["stale_is_zero"] = false;
  ac["valid"] = snap.valid;
  ac["reason"] = snap.reason;
  if (snap.valid) {
    ac["lifetime_wh"] = snap.wh;
    ac["active_power_w"] = snap.power_dW / 10.0;
    ac["active_power_age_ms"] = millis() - snap.power_at_ms;
    ac["voltage_v"] = snap.voltage_dV / 10.0;
    ac["current_a"] = snap.current_mA / 1000.0;
  } else {
    ac["lifetime_wh"] = nullptr;
    ac["active_power_w"] = nullptr;
  }
  ac["counter_epoch"] = snap.epoch;
  ac["clock_quality"] = snap.clock;
  ac["retained_allocated_wh"] = snap.allocated_wh;
  ac["retained_unallocated_wh"] = snap.unallocated_wh;
  ac["store_full"] = snap.store_full;
  JsonObject dsp = doc["dsp"].to<JsonObject>();
  dsp["present"] = false;
  dsp["protocol"] = "none";
  JsonObject diag = doc["diag"].to<JsonObject>();
  diag["schema"] = 1;
  diag["upload"] = "pending_server_contract";
  diag["sd"] = snap.sd ? "ready" : "absent_or_failed";
  diag["flash_fallback"] = snap.flash;
  diag["dropped"] = snap.dropped;
  diag["retained"] = snap.diag_retained;
}

bool hwV011HandleAck(JsonDocument& doc) {
  const String type = doc["type"] | "";
  const bool energy_ack = type == "ac_energy_ack";
  const bool diag_ack = type == "diag_blackbox_ack";
  if (!energy_ack && !diag_ack) return false;
  const bool durable = doc["durable_commit"] | false;
  if (!durable) return true;
  if (energy_ack && energy && energy_mu && xSemaphoreTake(energy_mu, pdMS_TO_TICKS(20)) == pdTRUE) {
    const uint32_t seq = doc["seq"] | 0;
    if (seq) energy->acknowledge(seq, true);
    xSemaphoreGive(energy_mu);
  }
  if (diag_ack) {
    upload.set_contract(true);
    const uint32_t seq = doc["seq"] | 0;
    upload.on_ack(seq, true, millis());
  }
  return true;
}

#endif
