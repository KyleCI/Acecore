#include "dsp011_log.h"
#if POWERCELL_DSP011
#include "ac_energy_log.h"
#include "clock.h"
#include "dsp011_pzem.h"
#include "log.h"
#include "periph.h"
#include "pins.h"
#include "sd_log.h"
#include "serial_spool.h"
#include <SD.h>
#include <heap_guard.h>  // pc::kSdMaxFiles, FatFs context size (2.5.29)
#include "heapguard.h"
#include "ff.h"
#include <SPI.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {
constexpr int kSdCs = PIN_DSP_SD_CS;
constexpr uint32_t kSdBlocks = 1024;
constexpr uint32_t kEnergyBytes = 65536;
constexpr uint32_t kSector = 4096;
constexpr uint32_t kHwLogStack = 8192;
static_assert(kSdCs == 5, "DSP SD chip select is GPIO5");
// 2.5.29: the mount context holds max_files FIL objects (4136 B each) for the uptime.
static_assert(sizeof(FIL) == pc::kFatFsFilBytes, "FatFs FIL size (per-file 4096 B cache) as documented in heap_guard.h");
static_assert(kSdCs != PIN_PZEM_RX && kSdCs != PIN_PZEM_TX, "SD chip select is not a PZEM pin");
static_assert(PIN_SPI_SCK == 18 && PIN_SPI_MISO == 19 && PIN_SPI_MOSI == 23, "SD SPI pins");
static_assert(PIN_SD_CS == 14, "original profile SD CS stays GPIO14");

class FileCard : public powercell::BlockDevice {
 public:
  bool begin() {
    file_ = SD.open("/blackbox.bin", "r+");
    if (!file_) file_ = SD.open("/blackbox.bin", "w+");
    if (!file_) return false;
    const uint32_t bytes = kSdBlocks * powercell::kSdBlockBytes;
    if (!preallocate(bytes)) return false;
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
    if (!ready_ || !file_.seek(block * powercell::kSdBlockBytes)) { ready_ = false; return false; }
    if (file_.write(src, powercell::kSdBlockBytes) != powercell::kSdBlockBytes) { ready_ = false; return false; }
    return true;
  }
  bool flush() override { if (!ready_) return false; file_.flush(); return true; }

 private:
  bool preallocate(uint32_t bytes) {
    if (file_.size() >= bytes) return true;
    uint8_t blank[256];
    memset(blank, 0xFF, sizeof blank);
    file_.seek(file_.size());
    while (file_.size() < bytes) {
      const uint32_t left = bytes - file_.size();
      const size_t n = left > sizeof blank ? sizeof blank : left;
      if (file_.write(blank, n) != n) { ready_ = false; return false; }
      vTaskDelay(1);
    }
    file_.flush();
    return true;
  }
  File file_;
  bool ready_ = false;
};

class FileNor : public powercell::ByteStore {
 public:
  bool begin() {
    file_ = SD.open("/acenergy.bin", "r+");
    if (!file_) file_ = SD.open("/acenergy.bin", "w+");
    if (!file_) return false;
    uint8_t blank[256];
    memset(blank, 0xFF, sizeof blank);
    file_.seek(file_.size());
    while (file_.size() < kEnergyBytes) {
      const uint32_t left = kEnergyBytes - file_.size();
      const size_t n = left > sizeof blank ? sizeof blank : left;
      if (file_.write(blank, n) != n) return false;
      vTaskDelay(1);
    }
    file_.flush();
    return true;
  }
  uint32_t size() const override { return kEnergyBytes; }
  bool read(uint32_t offset, uint8_t* dst, uint32_t len) const override {
    if (!dst || offset + len < offset || offset + len > kEnergyBytes) return false;
    if (!file_.seek(offset)) return false;
    return file_.read(dst, len) == len;
  }
  bool write(uint32_t offset, const uint8_t* src, uint32_t len) override {
    if (!src || offset + len < offset || offset + len > kEnergyBytes) return false;
    uint8_t buf[128];
    uint32_t done = 0;
    while (done < len) {
      const uint32_t n = len - done > sizeof buf ? sizeof buf : len - done;
      if (!file_.seek(offset + done) || file_.read(buf, n) != n) return false;
      for (uint32_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(buf[i] & src[done + i]);
      if (!file_.seek(offset + done) || file_.write(buf, n) != n) return false;
      done += n;
    }
    return true;
  }
  bool erase(uint32_t offset, uint32_t len) override {
    if (offset + len < offset || offset + len > kEnergyBytes) return false;
    uint8_t blank[128];
    memset(blank, 0xFF, sizeof blank);
    uint32_t done = 0;
    while (done < len) {
      const uint32_t n = len - done > sizeof blank ? sizeof blank : len - done;
      if (!file_.seek(offset + done) || file_.write(blank, n) != n) return false;
      done += n;
    }
    return true;
  }
  bool flush() override { file_.flush(); return true; }

 private:
  mutable File file_;
};

SemaphoreHandle_t io = nullptr;
FileCard* card = nullptr;
FileNor* energyStore = nullptr;
powercell::SdLog* sd = nullptr;
powercell::MetaMirror* meta = nullptr;
powercell::SlotLog* slots = nullptr;
powercell::EnergyLog* energy = nullptr;
bool gap = false;

void lock() { if (io) xSemaphoreTake(io, portMAX_DELAY); }
void unlock() { if (io) xSemaphoreGive(io); }

void observeEnergy() {
  if (!energy) return;
  float volts = 0, amps = 0, watts = 0;
  uint32_t wh = 0, age = 0;
  powercell::MeterSample sample;
  sample.mono_ms = millis();
  sample.mono_continuous = true;
  sample.measurement_gap = gap;
  const bool ok = hwPzemSample(&volts, &amps, &watts, &wh, &age);
  sample.valid = ok;
  if (ok) sample.lifetime_wh = wh;
  gap = !ok;
  const int64_t now = (int64_t)time(nullptr);
  const pc::clk::Source src = clock_::source();
  if (now >= pc::clk::kMinEpoch && (src == pc::clk::Source::Ntp || src == pc::clk::Source::Phone)) {
    sample.unix_s = now;
    sample.clock = static_cast<uint8_t>(powercell::ClockQuality::Anchored);
  } else if (now >= pc::clk::kMinEpoch && src == pc::clk::Source::Rtc) {
    sample.unix_s = now;
    sample.clock = static_cast<uint8_t>(powercell::ClockQuality::RtcUnanchored);
    sample.margin_s = 180;
  }
  energy->observe(sample);
}

void task(void*) {
  bool mounted = false;
  if (periph::spiMutex()) xSemaphoreTake(periph::spiMutex(), portMAX_DELAY);
  const uint32_t t0 = millis();
  // 2.5.29: two files (blackbox.bin + acenergy.bin) instead of the Arduino default 5:
  // the FatFs mount context drops from 26928 to 14520 B of DRAM (ACE-000003: no WS TLS).
  const bool begun = SD.begin(kSdCs, SPI, 1000000, "/sd", pc::kSdMaxFiles);
  const uint32_t mountMs = millis() - t0;
  if (begun) {
    card = new FileCard();
    energyStore = new FileNor();
    if (card->begin()) {
      sd = new powercell::SdLog(*card, true);
      sd->recover();
      mounted = true;
    }
    if (energyStore->begin()) {
      powercell::SlotLogConfig cfg;
      cfg.sector_size = kSector;
      cfg.allow_overwrite = false;
      meta = new powercell::MetaMirror(*energyStore, kSector);
      slots = new powercell::SlotLog(*energyStore, kSector * 2, kEnergyBytes - kSector * 2, cfg);
      slots->recover();
      energy = new powercell::EnergyLog(*slots, *meta, powercell::EnergyConfig());
      energy->load();
    }
    const uint32_t mb = uint32_t(SD.cardSize() / (1024 * 1024));
    // 2.5.30: no usable file -> close both and unmount, so the 14520 B FatFs context is
    // returned (ACE-000003 2.5.29 without a card: mount "succeeded", present=0, held anyway).
    const bool keep = pc::sdKeepMount(mounted, energy != nullptr);
    if (!keep) {
      delete card; card = nullptr;
      delete energyStore; energyStore = nullptr;
      SD.end();
    }
    periph::noteSd(mounted, keep ? mb : 0);
    // 2.5.30: the 2.5.29 "DRAM used" field compared free DRAM at task start with free DRAM after the
    // (up to 60 s) mount, so it counted every other allocation of the boot. The context size
    // is fixed: report it and whether it is still held.
    LOGI("SD", "CARD", "cs=%d mount=1 blackbox=%d energy=%d size_mb=%lu mount_ms=%lu ctx=%lu held=%d free=%lu", kSdCs, (int)mounted,
         (int)(energy != nullptr), (unsigned long)mb, (unsigned long)mountMs, (unsigned long)pc::fatVfsCtxBytes(pc::kSdMaxFiles), (int)keep,
         (unsigned long)heapguard::freeNow());
  } else {
    periph::noteSd(false, 0);
    LOGW("SD", "CARD", "no card on GPIO%d mount=0 mount_ms=%lu held=0 free=%lu", kSdCs, (unsigned long)mountMs, (unsigned long)heapguard::freeNow());
  }
  if (periph::spiMutex()) xSemaphoreGive(periph::spiMutex());
  if (!sd && !energy) {
    // 2.5.30: nothing to drain or observe without a card: end the task and return its
    // 8192 B stack. hwLogExport() still answers from RAM (it checks sd).
    LOGI("SD", "TASK_END", "hwlog stops (no SD log); %lu B stack returned", (unsigned long)kHwLogStack);
    vTaskDelete(nullptr);
    return;
  }

  uint32_t last = 0;
  for (;;) {
    lock();
    if (periph::spiMutex()) xSemaphoreTake(periph::spiMutex(), portMAX_DELAY);
    if (sd) powercell::console_spool().drain(sd, 4);
    if (millis() - last >= 1000) { last = millis(); observeEnergy(); }
    if (periph::spiMutex()) xSemaphoreGive(periph::spiMutex());
    unlock();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
}  // namespace

void hwLogStart() {
  io = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(task, "hwlog", kHwLogStack, nullptr, 1, nullptr, 1);
}

void hwLogExport(bool full) {
  (void)full;
  powercell::console_spool().set_suppressed(true);
  lock();
  if (periph::spiMutex()) xSemaphoreTake(periph::spiMutex(), portMAX_DELAY);
  if (sd) powercell::console_spool().drain(sd, 32);
  char body[1800];
  if (sd && sd->stats().ready) {
    powercell::console_spool().export_sd(sd, body, sizeof body);
    Serial.println("source=sd_serial");
  } else {
    snprintf(body, sizeof body, "sd_unavailable\n");
    powercell::console_spool().export_recent(body + strlen(body), sizeof body - strlen(body));
    Serial.println("source=ram_recent");
  }
  Serial.println(body);
  if (periph::spiMutex()) xSemaphoreGive(periph::spiMutex());
  unlock();
  powercell::console_spool().set_suppressed(false);
}
#endif
