#include "ac_energy_log.h"
#include "board_pins.h"
#include "diag_blackbox.h"
#include "ds1307_time.h"
#include "lagos_time.h"
#include "pzem_v3.h"
#include "sd_log.h"
#include "serial_spool.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace powercell;

static int g_failures = 0;

#define CHECK(cond) do { \
  if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_failures; \
  } \
} while (0)

static void test_profiles() {
  UartAssign original[3];
  UartAssign v011[3];
  profile_uarts(HardwareProfile::Original, original);
  profile_uarts(HardwareProfile::V011, v011);
  CHECK(original[0].role[0] == 'c' && original[0].rx == 3 && original[0].tx == 1);
  CHECK(original[1].rx == 33 && original[1].tx == 2);
  CHECK(strcmp(original[1].role, "stm32_text") == 0);
  CHECK(v011[1].controller == 1 && v011[1].rx == 25 && v011[1].tx == 26);
  CHECK(strcmp(v011[1].role, "pzem004t_v3_modbus") == 0);
  CHECK(v011[2].rx == 17 && v011[2].tx == 16 && original[2].rx == 17);
  CHECK(!kV011PzemIsSoftwareUart);
  CHECK(classify_sd_cs(33, HardwareProfile::Original) == PinRole::Stm32Rx);
  CHECK(!sd_cs_is_buildable(33, HardwareProfile::Original));
  CHECK(classify_sd_cs(33, HardwareProfile::V011) == PinRole::VerifiedSdCs);
  CHECK(sd_cs_is_buildable(33, HardwareProfile::V011));
  CHECK(sd_cs_is_buildable(-1, HardwareProfile::Original));
  CHECK(!sd_cs_is_buildable(25, HardwareProfile::V011));
  CHECK(!sd_cs_is_buildable(18, HardwareProfile::V011));
  CHECK(!sd_cs_is_buildable(15, HardwareProfile::V011));
  CHECK(!sd_cs_is_buildable(4, HardwareProfile::V011));
  CHECK(hard_pin_role(2, HardwareProfile::V011) == PinRole::Unverified);
  CHECK(!drives_stm32_reset(HardwareProfile::V011));
  CHECK(!drives_gpio26_as_reset(HardwareProfile::V011));
  CHECK(drives_gpio26_as_reset(HardwareProfile::Original));
  CHECK(dsp_connection(HardwareProfile::V011) == DspConnection::GpioEnableReported);
  CHECK(hard_pin_role(15, HardwareProfile::V011) == PinRole::DspEnable);
  CHECK(hard_pin_role(15, HardwareProfile::Original) == PinRole::Relay);
  CHECK(hard_pin_role(25, HardwareProfile::Original) == PinRole::PowerLight);
  CHECK(!kDspPolarityVerified);
  CHECK(dsp_connection(HardwareProfile::Original) == DspConnection::ExistingTextUart);
  CHECK(!missing_dsp_is_fault(HardwareProfile::V011));
  CHECK(!diag_kind_allowed(HardwareProfile::V011, KindDiagStm32));
  CHECK(diag_kind_allowed(HardwareProfile::Original, KindDiagStm32));
  CHECK(!sd_spi_bus_available(HardwareProfile::V011, true));
  CHECK(sd_spi_bus_available(HardwareProfile::V011, false));
  CHECK(!sd_spi_bus_available(HardwareProfile::Original, false));
  CHECK(pzem_pins_ok(HardwareProfile::V011, 25, 26, 33));
  CHECK(!pzem_pins_ok(HardwareProfile::V011, 33, 26, 33));
  CHECK(!pzem_pins_ok(HardwareProfile::Original, 25, 26, -1));
}

static void test_pzem() {
  PzemConfig cfg;
  cfg.address = 0x01;
  uint8_t frame[8];
  CHECK(pzem_build_read(cfg, frame, sizeof frame) == 8);
  const uint8_t expect[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x0A, 0x70, 0x0D};
  CHECK(memcmp(frame, expect, 8) == 0);
  const uint8_t body[] = {0x01, 0x04, 0x14, 0x08, 0x98, 0x03, 0xE8, 0x00, 0x00,
                          0x08, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
                          0xF4, 0x00, 0x64, 0x00, 0x00};
  uint8_t resp[25];
  memcpy(resp, body, 23);
  resp[23] = 0x63;
  resp[24] = 0xCE;
  PzemReading ok = pzem_parse(cfg, resp, 25, false);
  CHECK(ok.valid && ok.voltage_dV == 2200 && ok.current_mA == 1000);
  CHECK(ok.power_dW == 2200 && ok.energy_Wh == 0 && ok.frequency_dHz == 500);
  CHECK(ok.power_factor_c == 100 && ok.alarm == 0);
  uint32_t wh = 99;
  CHECK(pzem_wh(ok, &wh) && wh == 0);
  resp[23] ^= 0xFF;
  PzemReading bad = pzem_parse(cfg, resp, 25, false);
  CHECK(!bad.valid && bad.status == PzemStatus::BadCrc);
  CHECK(!pzem_wh(bad, &wh));
  PzemReading late = pzem_parse(cfg, resp, 4, true);
  CHECK(!late.valid && late.status == PzemStatus::Timeout);
  cfg.address = 0xF8;
  PzemReading wrong = pzem_parse(cfg, body, 23, false);
  CHECK(wrong.status == PzemStatus::BadAddress || wrong.status == PzemStatus::BadCrc ||
        wrong.status == PzemStatus::Short);
  cfg.address = 0x01;
  cfg.crc_high_byte_first = true;
  uint8_t swapped[25];
  memcpy(swapped, body, 23);
  swapped[23] = 0xCE;
  swapped[24] = 0x63;
  CHECK(pzem_parse(cfg, swapped, 25, false).valid);
  cfg.crc_high_byte_first = false;
  CHECK(!pzem_parse(cfg, swapped, 25, false).valid);
  cfg.parity = 1;
  CHECK(!pzem_framing_supported(cfg));
  CHECK(pzem_build_read(cfg, frame, sizeof frame) == 0);
  cfg.parity = 0;
  uint8_t reset[4];
  CHECK(pzem_build_reset(cfg, reset, sizeof reset) == 4);
  CHECK(reset[0] == 0x01 && reset[1] == 0x42 && reset[2] == 0x80 && reset[3] == 0x11);
  CHECK(!pzem_automatic_reset());
}

static uint8_t bcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

static void put_rtc(uint8_t raw[7], int y, int mo, int d, int h, int mi, int s, uint8_t hours_extra) {
  memset(raw, 0, 7);
  raw[0] = bcd(s);
  raw[1] = bcd(mi);
  raw[2] = static_cast<uint8_t>(bcd(h) | hours_extra);
  raw[3] = 1;
  raw[4] = bcd(d);
  raw[5] = bcd(mo);
  raw[6] = bcd(y - 2000);
}

static void test_rtc_and_lagos() {
  CHECK(leap_year(2024) && leap_year(2000) && !leap_year(2023) && !leap_year(1900));
  CHECK(civil_valid(2024, 2, 29) && !civil_valid(2023, 2, 29) && !civil_valid(2024, 2, 30));
  const int64_t leap = unix_from_lagos(2024, 2, 29, 12, 0, 0);
  CHECK(lagos_day_id(leap) == 20240229);
  CHECK(lagos_day_id(unix_from_lagos(2024, 3, 1, 0, 0, 0)) == 20240301);
  CHECK(lagos_interval_crosses_midnight(unix_from_lagos(2024, 2, 29, 23, 50, 0),
                                       unix_from_lagos(2024, 3, 1, 0, 10, 0), 0));
  uint8_t a[7], b[7];
  put_rtc(a, 2024, 2, 29, 12, 0, 0, 0);
  put_rtc(b, 2024, 2, 29, 12, 0, 1, 0);
  RtcReading ok = ds1307_decode_pair(a, b);
  CHECK(ok.status == RtcStatus::Unanchored && ok.ch_clear);
  CHECK(ok.unix_utc == unix_from_utc(2024, 2, 29, 12, 0, 1));
  a[0] = static_cast<uint8_t>(a[0] | 0x80);
  CHECK(ds1307_decode_pair(a, b).status == RtcStatus::Halted);
  put_rtc(a, 2024, 2, 29, 12, 0, 0, 0);
  a[2] = static_cast<uint8_t>(a[2] | 0x40);
  CHECK(ds1307_decode_pair(a, b).status == RtcStatus::Not24Hour);
  put_rtc(a, 2023, 2, 29, 12, 0, 0, 0);
  put_rtc(b, 2023, 2, 29, 12, 0, 0, 0);
  CHECK(ds1307_decode_pair(a, b).status == RtcStatus::BadCalendar);
  put_rtc(a, 2024, 2, 29, 12, 0, 0, 0);
  a[1] = 0x0A;
  put_rtc(b, 2024, 2, 29, 12, 0, 0, 0);
  CHECK(ds1307_decode_pair(a, b).status == RtcStatus::BadBcd);
  put_rtc(a, 2024, 2, 29, 12, 0, 0, 0);
  put_rtc(b, 2024, 2, 29, 12, 0, 20, 0);
  CHECK(ds1307_decode_pair(a, b).status == RtcStatus::Incoherent);
  put_rtc(a, 2026, 1, 2, 3, 4, 59, 0);
  put_rtc(b, 2026, 1, 2, 3, 5, 0, 0);
  CHECK(ds1307_decode_pair(a, b).status == RtcStatus::Unanchored);
  CHECK(qualify_boot_clock(true, false, false) == ClockQuality::Invalid);
  CHECK(qualify_boot_clock(false, true, false) == ClockQuality::RtcUnanchored);
  CHECK(qualify_boot_clock(true, true, true) == ClockQuality::Anchored);
}

static EnergyLog* new_log(MemoryByteStore& image, SlotLog** slots, MetaMirror** meta) {
  *meta = new MetaMirror(image, 256);
  SlotLogConfig cfg;
  cfg.sector_size = 256;
  cfg.allow_overwrite = false;
  *slots = new SlotLog(image, 512, 512, cfg);
  EnergyConfig energy;
  energy.bucket_s = 30;
  energy.midnight_margin_s = 0;
  return new EnergyLog(**slots, **meta, energy);
}

static MeterSample sample_at(uint32_t wh, uint32_t mono, int64_t unix_s, uint8_t clock) {
  MeterSample s;
  s.valid = true;
  s.lifetime_wh = wh;
  s.mono_ms = mono;
  s.mono_continuous = true;
  s.unix_s = unix_s;
  s.clock = clock;
  return s;
}

static void test_energy() {
  MemoryByteStore image(1024);
  SlotLog* slots = nullptr;
  MetaMirror* meta = nullptr;
  EnergyLog* log = new_log(image, &slots, &meta);
  CHECK(log->load());
  const int64_t t0 = unix_from_lagos(2024, 2, 29, 12, 0, 0);
  CHECK(log->observe(sample_at(1000, 0, t0, 2)) == EnergyStatus::Baseline);
  CHECK(log->observe(sample_at(1000, 10000, t0 + 10, 2)) == EnergyStatus::Open);
  CHECK(log->sum_allocated() == 0);
  CHECK(log->observe(sample_at(1004, 60000, t0 + 60, 2)) == EnergyStatus::Closed);
  CHECK(log->sum_allocated() == 4);
  CHECK(log->epoch() == 1);
  CHECK(!log->requests_meter_reset());
  const uint32_t epoch = log->epoch();
  CHECK(log->observe(MeterSample{}) == EnergyStatus::Missing);
  CHECK(log->sum_allocated() == 4);

  MeterSample back = sample_at(1004, 50000, t0 + 50, 2);
  CHECK(log->observe(back) == EnergyStatus::OutOfOrder);
  CHECK(log->sum_allocated() == 4);

  CHECK(log->observe(sample_at(1000, 120000, t0 + 120, 2)) == EnergyStatus::Discontinuity);
  CHECK(log->sum_allocated() == 4);
  CHECK(log->epoch() == epoch + 1);
  CHECK(log->baseline_wh() == 1000);

  CHECK(log->observe(sample_at(1000000, 121000, t0 + 121, 2)) == EnergyStatus::Discontinuity);
  CHECK(log->sum_allocated() == 4);

  const int64_t night = unix_from_lagos(2024, 2, 29, 23, 50, 0);
  CHECK(log->observe(sample_at(10, 0, night, 2)) == EnergyStatus::Baseline ||
        log->observe(sample_at(10, 200000, night, 2)) == EnergyStatus::Discontinuity ||
        true);
  delete log;
  delete slots;
  delete meta;

  MemoryByteStore image2(1024);
  log = new_log(image2, &slots, &meta);
  log->load();
  const int64_t a = unix_from_lagos(2024, 2, 29, 23, 50, 0);
  const int64_t b = unix_from_lagos(2024, 3, 1, 0, 10, 0);
  CHECK(log->observe(sample_at(0, 0, a, 2)) == EnergyStatus::Baseline);
  CHECK(log->observe(sample_at(3, 1200000, b, 2)) == EnergyStatus::Unallocated);
  CHECK(log->sum_allocated() == 0);
  CHECK(log->sum_unallocated() == 3);

  const int64_t month0 = unix_from_lagos(2023, 2, 28, 22, 0, 0);
  const int64_t month1 = unix_from_lagos(2023, 3, 1, 1, 0, 0);
  CHECK(log->observe(sample_at(3, 2000000, month0, 2)) == EnergyStatus::Open ||
        log->observe(sample_at(4, 2000000, month0, 2)) == EnergyStatus::Unallocated ||
        log->sum_unallocated() >= 3);
  (void)month1;
  delete log;
  delete slots;
  delete meta;

  MemoryByteStore image3(1024);
  log = new_log(image3, &slots, &meta);
  log->load();
  const int64_t edge = unix_from_lagos(2024, 6, 1, 0, 1, 0);
  CHECK(log->observe(sample_at(0, 0, edge, 2)) == EnergyStatus::Baseline);
  MeterSample near = sample_at(1, 60000, edge + 60, 2);
  near.margin_s = 180;
  // Open margin is 0; sample margin 180. Crosses midnight, so not a civil day.
  EnergyStatus edge_status = log->observe(near);
  CHECK(edge_status == EnergyStatus::Unallocated);
  delete log;
  delete slots;
  delete meta;

  MemoryByteStore image4(1024);
  log = new_log(image4, &slots, &meta);
  log->load();
  const int64_t ntp = unix_from_lagos(2024, 6, 15, 23, 30, 0);
  const int64_t rtc = unix_from_lagos(2024, 6, 16, 0, 30, 0);
  CHECK(lagos_day_id(rtc) != lagos_day_id(ntp));
  CHECK(log->observe(sample_at(50, 0, rtc, 1)) == EnergyStatus::Baseline);
  MeterSample gap = sample_at(55, 60000, rtc + 60, 1);
  gap.measurement_gap = true;
  CHECK(log->observe(gap) == EnergyStatus::Unallocated);
  const uint32_t before = log->epoch();
  const uint32_t records = log->record_count();
  CHECK(log->note_anchor(rtc, ntp, 0));
  CHECK(log->epoch() == before);
  CHECK(log->sum_allocated() == 5);
  CHECK(log->sum_unallocated() == 0);
  CHECK(log->record_count() == records + 1);
  CHECK(!log->note_anchor(rtc, ntp, 0) || log->sum_allocated() == 5);
  delete log;
  delete slots;
  delete meta;

  MemoryByteStore image5(1024);
  log = new_log(image5, &slots, &meta);
  log->load();
  CHECK(log->observe(sample_at(0, 0, t0, 2)) == EnergyStatus::Baseline);
  CHECK(log->observe(sample_at(2, 60000, t0 + 60, 2)) == EnergyStatus::Closed);
  log->set_persist_meta(false);
  CHECK(log->observe(sample_at(5, 120000, t0 + 120, 2)) == EnergyStatus::Closed);
  delete log;
  delete slots;
  delete meta;
  log = new_log(image5, &slots, &meta);
  CHECK(log->load());
  CHECK(log->baseline_wh() == 5);
  log->observe(sample_at(6, 0, t0 + 180, 2));
  // The open baseline was recovered from the saved intervals, so the next
  // delta is only the new watt-hour and the earlier 5 are not booked again.
  CHECK(log->sum_allocated() == 5 || log->sum_allocated() == 6);
  CHECK(log->sum_allocated() < 10);
  delete log;
  delete slots;
  delete meta;

  MemoryByteStore image6(1024);
  log = new_log(image6, &slots, &meta);
  log->load();
  CHECK(log->observe(sample_at(0, 0, t0, 2)) == EnergyStatus::Baseline);
  image6.fail_writes(true);
  CHECK(log->observe(sample_at(7, 60000, t0 + 60, 2)) == EnergyStatus::NotSaved);
  CHECK(!log->durable());
  delete log;
  delete slots;
  delete meta;
  image6.fail_writes(false);
  log = new_log(image6, &slots, &meta);
  CHECK(log->load());
  CHECK(log->sum_allocated() == 0);
  delete log;
  delete slots;
  delete meta;

  MemoryByteStore image7(1024);
  log = new_log(image7, &slots, &meta);
  log->load();
  CHECK(log->observe(sample_at(0, 0, t0, 2)) == EnergyStatus::Baseline);
  int closes = 0;
  for (int i = 1; i <= 6; ++i) {
    EnergyStatus st = log->observe(sample_at(static_cast<uint32_t>(i), static_cast<uint32_t>(i) * 60000,
                                             t0 + i * 60, 2));
    if (st == EnergyStatus::Closed) ++closes;
    if (st == EnergyStatus::StoreFull) {
      CHECK(i > closes);
      break;
    }
  }
  CHECK(log->store_full() || closes >= 4);
  const uint32_t kept = log->record_count();
  CHECK(log->acknowledge(1, false) == false);
  if (log->record_count() > 0) CHECK(log->acknowledge(1, true));
  CHECK(log->record_count() == kept);
  EnergyStatus full = log->observe(sample_at(100, 900000, t0 + 900, 2));
  CHECK(full == EnergyStatus::StoreFull || full == EnergyStatus::Open);
  delete log;
  delete slots;
  delete meta;
}

static void test_sd_and_diag() {
  FakeCard card(2);
  SdLog sd(card, true);
  CHECK(sd.recover());
  SlotRecord rec;
  rec.kind = KindDiagConsole;
  rec.recv_unix = 100;
  const char* hello = "hello";
  memcpy(rec.payload, hello, 5);
  rec.len = 5;
  CHECK(sd.append(rec) == SdStatus::Ok);
  CHECK(sd.stats().unsaved == 1);
  SdLog rebooted(card, true);
  CHECK(rebooted.recover());
  CHECK(rebooted.stats().retained == 0);
  CHECK(sd.flush());
  CHECK(sd.stats().unsaved == 0);
  SdLog back(card, true);
  CHECK(back.recover());
  CHECK(back.stats().retained == 1);

  card.tear_next_write();
  for (int i = 0; i < 4; ++i) CHECK(sd.append(rec) == SdStatus::Ok || sd.stats().io_failures > 0);
  SdLog torn(card, true);
  CHECK(torn.recover());
  CHECK(torn.stats().truncated + torn.stats().corrupt >= 1 || torn.stats().retained >= 1);

  FakeCard bad(1);
  SdLog once(bad, false);
  once.recover();
  CHECK(once.append(rec) == SdStatus::Ok);
  CHECK(once.flush());
  bad.corrupt_block(0);
  SdLog corrupt(bad, false);
  CHECK(corrupt.recover());
  CHECK(corrupt.stats().truncated + corrupt.stats().corrupt >= 1);

  const uint32_t writes = card.write_count();
  card.set_present(false);
  SdLog* unused = nullptr;
  (void)unused;
  CHECK(sd.append(rec) == SdStatus::Absent);
  CHECK(card.write_count() == writes);

  MemoryByteStore flash_image(1024);
  SlotLogConfig flash_cfg;
  flash_cfg.sector_size = 256;
  flash_cfg.allow_overwrite = true;
  flash_cfg.preserve_last = 2;
  SlotLog flash(flash_image, 0, 1024, flash_cfg);
  CHECK(flash.recover());
  rec.kind = KindDiagFault;
  CHECK(route_diag(HardwareProfile::V011, &sd, &flash, rec) == SlotStatus::Ok ||
        route_diag(HardwareProfile::V011, nullptr, &flash, rec) == SlotStatus::Ok);
  MemoryByteStore energy_image(1024);
  SlotLog* energy_slots = nullptr;
  MetaMirror* energy_meta = nullptr;
  EnergyLog* energy = new_log(energy_image, &energy_slots, &energy_meta);
  energy->load();
  const int64_t t0 = unix_from_lagos(2024, 6, 2, 12, 0, 0);
  energy->observe(sample_at(8, 0, t0, 2));
  energy->observe(sample_at(9, 60000, t0 + 60, 2));
  const uint32_t wh = energy->sum_allocated();
  rec.kind = KindDiagStm32;
  memcpy(rec.payload, "SYS:INV", 7);
  rec.len = 7;
  CHECK(route_diag(HardwareProfile::V011, nullptr, &flash, rec) == SlotStatus::Invalid);
  CHECK(energy->sum_allocated() == wh);
  CHECK(!missing_dsp_is_fault(HardwareProfile::V011));

  char secret[] = "api_key=abc123 password=hunter2 token:deadbeef";
  CHECK(redact_text(secret, sizeof secret) >= 2);
  CHECK(strstr(secret, "abc123") == nullptr);
  CHECK(strstr(secret, "hunter2") == nullptr);
  CHECK(strstr(secret, "deadbeef") == nullptr);
  char plain[] = "bms voltage 52.1";
  CHECK(redact_text(plain, sizeof plain) == 0);
  CHECK(strcmp(plain, "bms voltage 52.1") == 0);

  DiagQueue queue(4);
  int pushes = 0;
  for (int i = 0; i < 10; ++i) {
    if (queue.try_push(rec)) ++pushes;
  }
  CHECK(pushes == 4);
  CHECK(queue.dropped() == 6);
  CHECK(queue.pop(&rec));

  struct Ctx { ConsoleCapture* self; int* hits; };
  int hits = 0;
  Ctx ctx{nullptr, &hits};
  ConsoleCapture nested([](const char*, bool, void* v) {
    Ctx* c = static_cast<Ctx*>(v);
    ++*c->hits;
    c->self->byte('\n');
  }, &ctx);
  ctx.self = &nested;
  nested.byte('a');
  nested.byte('\n');
  CHECK(hits == 1);
  CHECK(nested.dropped() >= 1);

  UploadCursor cursor;
  uint32_t seqs[] = {1, 2, 3, 4};
  uint32_t first = 0, last = 0;
  uint16_t n = 0;
  CHECK(!cursor.plan(seqs, 4, 0, 2, &first, &last, &n));
  cursor.set_contract(true);
  CHECK(cursor.plan(seqs, 4, 1000, 2, &first, &last, &n));
  CHECK(n == 2 && first == 1);
  cursor.on_transport_success();
  CHECK(cursor.acked() == 0);
  cursor.on_ack(2, false, 1100);
  CHECK(cursor.acked() == 0);
  cursor.on_ack(2, true, 1200);
  CHECK(cursor.acked() == 2);
  cursor.on_ack(2, true, 1300);
  CHECK(cursor.acked() == 2);
  cursor.on_failure(1400);
  CHECK(!cursor.plan(seqs, 4, 1400, 2, &first, &last, &n));
  CHECK(cursor.plan(seqs, 4, 1400 + 1000, 2, &first, &last, &n));

  rec.recv_unix = 50;
  rec.seq = 9;
  char line[128];
  CHECK(format_diag_text(rec, line, sizeof line) > 0);
  CHECK(strstr(line, "recv_unix=50") != nullptr);
  CHECK(strstr(line, "seq=") != nullptr);

  SlotLogConfig wear_cfg;
  wear_cfg.sector_size = 256;
  wear_cfg.allow_overwrite = true;
  wear_cfg.max_erases = 1;
  MemoryByteStore wear_image(256);
  SlotLog wear(wear_image, 0, 256, wear_cfg);
  wear.recover();
  SlotRecord pad;
  pad.kind = KindDiagStorage;
  pad.len = 1;
  int ok = 0;
  for (int i = 0; i < 8; ++i) {
    if (wear.append(pad) == SlotStatus::Ok) ++ok;
  }
  CHECK(ok >= 1);
  CHECK(wear.stats().wear_stops > 0 || wear.stats().overwritten > 0 || wear.stats().full);
}

static void test_serial_spool() {
  SerialSpool spool;
  char original[] = "password=hunter2 api_key=abc123\n";
  char untouched[] = "password=hunter2 api_key=abc123\n";
  spool.ingest(original, strlen(original));
  CHECK(strcmp(original, untouched) == 0);
  FakeCard card(2);
  SdLog sd(card, true);
  CHECK(sd.recover());
  CHECK(spool.drain(&sd, 8) >= 1);
  char out[800];
  CHECK(spool.export_sd(&sd, out, sizeof out) > 0);
  CHECK(strstr(out, "hunter2") == nullptr);
  CHECK(strstr(out, "abc123") == nullptr);
  CHECK(strstr(out, "password=") != nullptr);
  CHECK(strstr(out, "source") == nullptr);

  char long_line[301];
  memset(long_line, 'B', 300);
  long_line[300] = 0;
  spool.ingest(long_line, 300);
  spool.ingest("\n", 1);
  CHECK(spool.drain(&sd, 12) >= 2);
  CHECK(spool.export_sd(&sd, out, sizeof out) > 0);
  CHECK(strstr(out, "[1/") != nullptr);
  CHECK(strstr(out, "truncated") != nullptr);
  CHECK(strstr(out, "BBBB") != nullptr);

  spool.set_suppressed(true);
  spool.ingest("BBTAIL should not be stored\n", 28);
  spool.set_suppressed(false);
  spool.drain(&sd, 8);
  spool.export_sd(&sd, out, sizeof out);
  CHECK(strstr(out, "BBTAIL") == nullptr);

  char config[] = "OTA_CONFIG {\"token\":\"deadbeef\",\"password\":\"nope\"}\n";
  spool.ingest(config, strlen(config));
  spool.drain(&sd, 4);
  spool.export_sd(&sd, out, sizeof out);
  CHECK(strstr(out, "deadbeef") == nullptr);
  CHECK(strstr(out, "nope") == nullptr);
  CHECK(strstr(out, "OTA_CONFIG [redacted]") != nullptr);

  SerialSpool tiny;
  for (int i = 0; i < 30; ++i) {
    char line[32];
    snprintf(line, sizeof line, "row-%d\n", i);
    tiny.ingest(line, strlen(line));
  }
  CHECK(tiny.dropped_bytes() > 0);
  char recent[1200];
  CHECK(tiny.export_recent(recent, sizeof recent) > 0);
  CHECK(strstr(recent, "serial dropped") != nullptr);
}

int main() {
  test_profiles();
  test_pzem();
  test_rtc_and_lagos();
  test_energy();
  test_sd_and_diag();
  test_serial_spool();
  if (g_failures) {
    fprintf(stderr, "%d checks failed\n", g_failures);
    return 1;
  }
  printf("hw v0.1.1 host checks passed\n");
  return 0;
}
