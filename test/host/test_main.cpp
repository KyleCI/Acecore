// Host unit tests for lib/pc_core. Build and run: make -C test/host
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "crc.h"
#include "identity.h"
#include "mdns_core.h"
#include "ota_identity.h"
#include "jk_bms.h"
#include "power_policy.h"
#include "ota_gate.h"
#include "heap_guard.h"
#include "local_auth.h"
#include "ring_queue.h"
#include "sha256.h"
#include "onb_core.h"
#include "stm32_frame.h"
#include "solar_energy.h"
#include "stm32_legacy_parser.h"
#include "events_core.h"
#include "lora_socket_proto.h"
#include "../../smart-socket-firmware/src/trip_lockout.h"
#include "payment_lock.h"
#include "hw_output.h"
#include "ac_output_policy.h"
#include "lan_ota.h"
#include "ota_errors.h"
#include <string>
#include <algorithm>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))

using namespace pc;

static void test_crc() {
  const uint8_t s[] = "123456789";
  CHECK(crc16_ccitt(s, 9) == 0x29B1);
  CHECK(crc32_ieee(s, 9) == 0xCBF43926u);
  CHECK(crc16_modbus(s, 9) == 0x4B37);
  CHECK(crc32_ieee(s + 4, 5, crc32_ieee(s, 4)) == 0xCBF43926u);  // streaming
}

static void test_frame_roundtrip() {
  uint8_t pl[] = {1, 2, 3, 0xAA, 0x55};
  uint8_t buf[300];
  size_t n = frame::encode(frame::SET_MODE, 7, pl, sizeof pl, buf, sizeof buf);
  CHECK(n == sizeof pl + frame::kOverhead);
  frame::Decoder d; frame::Frame f; int got = 0;
  // garbage + frame
  uint8_t junk[] = {0x00, 0xAA, 0x13, 0xFF};
  uint32_t t = 0;
  for (uint8_t b : junk) d.feed(b, t, f);
  t += 100;  // idle gap resets the parser
  for (size_t i = 0; i < n; ++i) got += d.feed(buf[i], t++, f);
  CHECK(got == 1);
  CHECK(f.type == frame::SET_MODE && f.seq == 7 && f.len == 5 && memcmp(f.payload, pl, 5) == 0);
  // corrupt CRC
  buf[n - 1] ^= 1; got = 0;
  for (size_t i = 0; i < n; ++i) got += d.feed(buf[i], t++, f);
  CHECK(got == 0 && d.crcErrors() == 1);
  // empty payload
  n = frame::encode(frame::GET_STATUS, 1, nullptr, 0, buf, sizeof buf);
  got = 0; for (size_t i = 0; i < n; ++i) got += d.feed(buf[i], t++, f);
  CHECK(n == 7 && got == 1 && f.len == 0);
  // inter-byte timeout drops a half frame then recovers
  n = frame::encode(frame::GET_VERSION, 2, pl, 3, buf, sizeof buf);
  got = 0; for (size_t i = 0; i < 4; ++i) got += d.feed(buf[i], t++, f);
  t += 50; for (size_t i = 0; i < n; ++i) got += d.feed(buf[i], t++, f);
  CHECK(got == 1 && d.timeouts() >= 1);
  // max payload
  uint8_t big[255]; for (int i = 0; i < 255; ++i) big[i] = (uint8_t)i;
  n = frame::encode(0x43, 9, big, 255, buf, sizeof buf);
  got = 0; for (size_t i = 0; i < n; ++i) got += d.feed(buf[i], t++, f);
  CHECK(n == 262 && got == 1 && f.len == 255 && f.payload[254] == 254);
  CHECK(frame::encode(0x01, 0, big, 255, buf, 100) == 0);  // too small buffer
  // wrong version rejected
  uint32_t verr = d.versionErrors();
  n = frame::encode(frame::GET_STATUS, 3, nullptr, 0, buf, sizeof buf); buf[2] = 2;
  got = 0; for (size_t i = 0; i < n; ++i) got += d.feed(buf[i], t++, f);
  CHECK(got == 0 && d.versionErrors() == verr + 1);
}

static void test_status_codec() {
  Stm32Status s; s.sys = SysState::Inverter; s.sw = SwitchPos::Inverter; s.chr = ChargeState::Off;
  s.fault = 0x00010221u; s.bat_v = 12.84f; s.has_bat = true; s.bat_pct = 57; s.has_bat_pct = true;
  s.grid_v = 0; s.has_grid = true; s.temp_c = -3.5f; s.has_temp = true; s.vout = 229.9f; s.iout = 1.23f; s.has_vout = s.has_iout = true;
  s.restart_count = 3; s.transfer_flags = TRANSFER_ACTIVE | TRANSFER_VOUT_CALIBRATED; s.chg_current = 4.56f; s.mod_index = 0.8123f;
  uint8_t p[40]; CHECK(frame::kStatusLen == 30);
  CHECK(frame::encodeStatus(s, 0x01020304u, p, sizeof p) == frame::kStatusLen);
  // exact agreed byte offsets (LE)
  CHECK(p[0] == 3 && p[1] == 1 && p[2] == 1 && p[3] == 3);
  CHECK(p[4] == 0x21 && p[5] == 0x02 && p[6] == 0x01 && p[7] == 0x00);
  CHECK(frame::getU16(p + 8) == 12840 && p[10] == 57 && p[11] == 0x05);
  CHECK(frame::getU16(p + 12) == 0 && (int16_t)frame::getU16(p + 14) == -35);
  CHECK(frame::getU16(p + 16) == 2299 && frame::getU16(p + 18) == 123 && frame::getU16(p + 20) == 456);
  CHECK(frame::getU16(p + 22) == 8123 && frame::getU32(p + 24) == 0x01020304u && frame::getU16(p + 28) == 0x0F);
  Stm32Status o; CHECK(frame::decodeStatus(p, frame::kStatusLen, o));
  CHECK(o.sys == SysState::Inverter && o.fault == 0x00010221u && o.bat_pct == 57 && o.has_ext);
  CHECK(o.restart_count == 3 && o.transfer_flags == 0x05 && o.uptime_s == 0x01020304u);
  NEAR(o.bat_v, 12.84, 0.001); NEAR(o.temp_c, -3.5, 0.01); NEAR(o.vout, 229.9, 0.05); NEAR(o.iout, 1.23, 0.005);
  NEAR(o.chg_current, 4.56, 0.005); NEAR(o.mod_index, 0.8123, 0.0001);
  CHECK(!frame::decodeStatus(p, 29, o));
  CHECK(frame::decodeStatus(p, 34, o));  // appended future fields tolerated
  // valid flags clear individually; 0xFF pct = unknown
  frame::putU16(p + 28, frame::VALID_GRID); p[10] = 0xFF;
  CHECK(frame::decodeStatus(p, 30, o) && !o.has_bat && o.has_grid && !o.has_temp && !o.has_vout && !o.has_bat_pct);
  CHECK(frame::encodeStatus(s, 0, p, 29) == 0);
}

static void feedStr(LegacyTextParser &p, const char *s, Stm32Status &out, int &published) {
  for (; *s; ++s) published += p.feed((uint8_t)*s, out);
}

static void test_legacy_parser() {
  LegacyTextParser p; Stm32Status out; int pub = 0;
  feedStr(p, "boot banner\r\n--- Status ---\r\nSYS : INVERTER\r\nSW  : INVERTER\r\nBAT : 12.80V  55%\r\nGRID: 0.0V\r\n"
             "RAW_INST: 12  RAW_RMS: 3.4\r\nTEMP: 31.2C\r\nCHR : --\r\nVOUT: 230.1V\r\nIOUT: 1.25A\r\nMOD : 0.80\r\n", out, pub);
  CHECK(pub == 0);
  feedStr(p, "--- Status ---\n", out, pub);  // next record publishes the previous one
  CHECK(pub == 1);
  CHECK(out.sys == SysState::Inverter && out.sw == SwitchPos::Inverter && out.chr == ChargeState::Off);
  NEAR(out.bat_v, 12.80, 0.001); CHECK(out.has_bat_pct && out.bat_pct == 55);
  NEAR(out.grid_v, 0, 0.001); NEAR(out.temp_c, 31.2, 0.01); NEAR(out.vout, 230.1, 0.01); NEAR(out.iout, 1.25, 0.001);
  CHECK(out.fault == 0);
  // fault record
  feedStr(p, ">>> FAULT: 0x21 <<<\nSYS : FAULT\nSW  : GRID\nBAT : 10.10V  2%\nGRID: 231.0V\nCHR : BULK\n", out, pub);
  CHECK(p.flushIdle(out));
  CHECK(out.sys == SysState::Fault && out.fault == 0x21 && out.chr == ChargeState::Bulk && out.sw == SwitchPos::Grid);
  // fault mask wider than 8 bits survives the legacy parser (u32 everywhere)
  feedStr(p, "--- Status ---\n>>> FAULT: 0x10200 <<<\nSYS : FAULT\n--- Status ---\n", out, pub);
  CHECK(out.sys == SysState::Fault && out.fault == 0x10200u);
  CHECK(!out.has_vout);
  // overlong line is dropped, not buffered forever
  pub = 0;
  for (int i = 0; i < 5000; ++i) pub += p.feed('X', out);
  p.feed('\n', out);
  CHECK(pub == 0 && p.overflows() == 1);
  feedStr(p, "SYS : IDLE\nBAT : 13.1V 90%\n", out, pub);
  CHECK(p.flushIdle(out) && out.sys == SysState::Idle && out.bat_pct == 90);
  CHECK(!p.flushIdle(out));  // nothing pending
}

static void test_ring() {
  RingQueue<int, 4> q;
  for (int i = 0; i < 6; ++i) q.push(i);
  CHECK(q.size() == 4 && q.dropped() == 2 && *q.peek(0) == 2 && *q.peek(3) == 5);
  CHECK(q.popN(2) == 2 && *q.peek() == 4);
  q.push(6); q.push(7);
  CHECK(q.removeIf([](int v) { return v % 2 == 0; }) == 2);
  CHECK(q.size() == 2 && *q.peek(0) == 5 && *q.peek(1) == 7);
  int v; CHECK(q.pop(&v) && v == 5);
}

static void test_identity() {
  uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAE, 0x9C, 0x1A};
  char c[18], h[13], g[16], s[24], p1[11], p2[11], e[64];
  macColon(mac, c); macHex(mac, h); gatewayName(mac, g); apSsid(mac, s);
  CHECK(!strcmp(c, "24:6F:28:AE:9C:1A") && !strcmp(h, "246F28AE9C1A"));
  CHECK(!strcmp(g, "ACE-AE9C1A") && !strcmp(s, "Powercell-AE9C1A"));
  apPassword(mac, nullptr, 0, p1); CHECK(strlen(p1) == 10);
  uint8_t mac2[6] = {0x24, 0x6F, 0x28, 0xAE, 0x9C, 0x1B};
  apPassword(mac2, nullptr, 0, p2); CHECK(strcmp(p1, p2) != 0);
  const uint8_t salt[] = {1, 2, 3};
  apPassword(mac, salt, 3, p2); CHECK(strcmp(p1, p2) != 0);
  urlEncode(c, e, sizeof e); CHECK(!strcmp(e, "24%3A6F%3A28%3AAE%3A9C%3A1A"));
}

static void test_ota_hardware_model() {
  using namespace pc;
  const char* models[] = {"pc-dsp011-5kw", "pc-dsp011-35kw", "pc-stm01-5kw", "pc-stm01-35kw"};
  char ids[4][128];
  char legacy[96];
  CHECK(otaFormatIdentity(legacy, sizeof legacy, "collector", "esp32dev", "2.5.14"));
  for (int i = 0; i < 4; ++i) {
    CHECK(otaFormatIdentity(ids[i], sizeof ids[i], "collector", models[i], "2.5.14"));
    CHECK(otaModelAccepts(ids[i], strlen(ids[i]), models[i], "2.5.13", "2.5.14", models[i]));
    CHECK(!otaModelAccepts(ids[i], strlen(ids[i]), models[i], "2.5.13", "2.5.14", nullptr));
    CHECK(!otaModelAccepts(ids[i], strlen(ids[i]), models[i], "2.5.13", "2.5.14", "esp32dev"));
    CHECK(!otaModelAccepts(legacy, strlen(legacy), models[i], "2.5.13", "2.5.14", models[i]));
    CHECK(!otaModelAccepts(ids[i], strlen(ids[i]), models[i], "2.5.13", "2.5.13", models[i]));
  }
  for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) if (i != j) {
    CHECK(!otaModelAccepts(ids[i], strlen(ids[i]), models[j], "2.5.13", "2.5.14", models[i]));
    CHECK(!otaModelAccepts(ids[i], strlen(ids[i]), models[j], "2.5.13", "2.5.14", models[j]));
  }
}

static void test_local_auth() {
  const char *tok = "0123456789abcdef0123456789abcdef0123456789abcdef";  // 48 hex, backend format
  CHECK(validLocalToken(tok));
  CHECK(!validLocalToken("short"));
  CHECK(!validLocalToken("0123456789abcdef0123456789abcdef:123"));
  char out[64]; size_t n = 0;
  CHECK(base64Decode("cG93ZXJjZWxsOmFiYw==", 20, out, sizeof out, &n) && n == 13 && !strcmp(out, "powercell:abc"));
  CHECK(base64Decode("cG93ZXJjZWxsOmFiYw", 18, out, sizeof out, &n) && !strcmp(out, "powercell:abc"));
  CHECK(!base64Decode("cG9$", 4, out, sizeof out, &n));
  char u[24], pw[140];
  CHECK(parseBasic("Basic cG93ZXJjZWxsOmFiYw==", u, sizeof u, pw, sizeof pw) && !strcmp(u, "powercell") && !strcmp(pw, "abc"));
  CHECK(parseBasic("basic   cG93ZXJjZWxsOmFiYw==", u, sizeof u, pw, sizeof pw));
  CHECK(!parseBasic("Bearer cG93ZXJjZWxsOmFiYw==", u, sizeof u, pw, sizeof pw));
  CHECK(!parseBasic("Basic bm9jb2xvbg==", u, sizeof u, pw, sizeof pw));  // "nocolon"
  // base64("powercell:" + tok)
  const char *good = "Basic cG93ZXJjZWxsOjAxMjM0NTY3ODlhYmNkZWYwMTIzNDU2Nzg5YWJjZGVmMDEyMzQ1Njc4OWFiY2RlZg==";
  CHECK(basicMatches(good, tok));
  CHECK(!basicMatches(good, "0123456789abcdef0123456789abcdef0123456789abcdee"));
  // base64("admin:" + tok): wrong user
  CHECK(!basicMatches("Basic YWRtaW46MDEyMzQ1Njc4OWFiY2RlZjAxMjM0NTY3ODlhYmNkZWYwMTIzNDU2Nzg5YWJjZGVm", tok));
  CHECK(!basicMatches(good, ""));   // no token provisioned: nothing authenticates
  CHECK(!basicMatches(nullptr, tok));
  CHECK(ctEquals("abc", 3, "abc", 3) && !ctEquals("abc", 3, "abd", 3) && !ctEquals("ab", 2, "abc", 3));
}

static void test_device_ids() {
  char c[64];
  canonicalId("aa:bb:cc:dd:ee:ff", c, sizeof c); CHECK(!strcmp(c, "AABBCCDDEEFF"));
  canonicalId("AA-BB-CC-DD-EE-FF", c, sizeof c); CHECK(!strcmp(c, "AABBCCDDEEFF"));
  canonicalId(" ACE4447 ", c, sizeof c); CHECK(!strcmp(c, "ACE4447"));
  canonicalId("aa:bb-cc:dd:ee:ff", c, sizeof c); CHECK(!strcmp(c, "aa:bb-cc:dd:ee:ff"));  // mixed separators: not a MAC
  CHECK(sameDeviceId("aa:bb:cc:dd:ee:ff", "AABBCCDDEEFF"));
  CHECK(sameDeviceId("aa-bb-cc-dd-ee-ff", "AA:BB:CC:DD:EE:FF"));
  CHECK(sameDeviceId("ace4447", "ACE4447"));
  CHECK(!sameDeviceId("ACE4447", "AABBCCDDEEFF"));
  CHECK(!sameDeviceId("", ""));
}

static void test_ota_url() {
  char p[200];
  const char *base = "https://api-dev.acecore.tech";
  CHECK(otaDownloadPath(base, "https://api-dev.acecore.tech/api/ota/download?device_id=AABBCCDDEEFF", p, sizeof p) &&
        !strcmp(p, "/api/ota/download?device_id=AABBCCDDEEFF"));
  CHECK(otaDownloadPath("https://api-dev.acecore.tech/", "https://API-DEV.acecore.tech/api/firmware.bin", p, sizeof p) && !strcmp(p, "/api/firmware.bin"));
  CHECK(otaDownloadPath(base, "/api/firmware/download?device_id=ACE4447", p, sizeof p));
  CHECK(!otaDownloadPath(base, "https://evil.example/api/ota/download", p, sizeof p));
  CHECK(!otaDownloadPath(base, "https://api-dev.acecore.tech.evil.example/api/ota/download", p, sizeof p));
  CHECK(!otaDownloadPath(base, "https://api-dev.acecore.tech@evil.example/api/ota/download", p, sizeof p));
  CHECK(!otaDownloadPath(base, "http://api-dev.acecore.tech/api/ota/download", p, sizeof p));
  CHECK(!otaDownloadPath(base, "https://api-dev.acecore.tech/api/ota/downloadX", p, sizeof p));
  CHECK(!otaDownloadPath(base, "https://api-dev.acecore.tech/api/../secret", p, sizeof p));
  CHECK(!otaDownloadPath(base, "/api/users", p, sizeof p));
  CHECK(isSha256Hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK(!isSha256Hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b85"));
  CHECK(!isSha256Hex(nullptr));
}

static void test_power_policy() {
  // Bare board / STM32 silent: ON is refused at once, OFF only releases PWR_KEY.
  CHECK(powerAction(true, false, false, false) == PowerAction::RefuseStm32Offline);
  CHECK(powerAction(true, false, false, true) == PowerAction::RefuseStm32Offline);
  CHECK(powerAction(false, false, false, true) == PowerAction::ReleasePwrKey);
  CHECK(powerAction(true, true, true, true) == PowerAction::RefuseOta);
  CHECK(powerAction(false, true, true, true) == PowerAction::SendSetMode);
  CHECK(powerAction(true, false, true, true) == PowerAction::SendSetMode);
  CHECK(powerAction(true, false, true, false) == PowerAction::DrivePwrKey);
  CHECK(powerAction(false, false, true, false) == PowerAction::ReleasePwrKey);
}

static void test_ota_url_dev_http() {
  char p[200];
  const char *base = "http://138.68.154.128";
  CHECK(otaDownloadPath(base, "http://138.68.154.128/api/ota/download?device_id=AABBCCDDEEFF", p, sizeof p) &&
        !strcmp(p, "/api/ota/download?device_id=AABBCCDDEEFF"));
  CHECK(!otaDownloadPath(base, "http://138.68.154.1280/api/ota/download", p, sizeof p));
  CHECK(!otaDownloadPath(base, "https://evil.example/api/ota/download", p, sizeof p));
  CHECK(!otaDownloadPath("https://api.acecore.tech", "http://api.acecore.tech/api/ota/download", p, sizeof p));
}

static void test_ota_gate() {
  OtaGate g(60000);
  // A check alone never pauses the WebSocket (the bench failure: WS dropped 35 s after boot).
  CHECK(g.start() && g.phase() == OtaGate::Checking && !g.wsPaused() && g.busy());
  CHECK(!g.start());
  g.finish();
  CHECK(!g.busy() && !g.wsPaused());
  // Update found: wait for the loop's grant; refused while a socket command is in flight.
  CHECK(g.start());
  g.requestInstall(1000);
  CHECK(g.waiting() && !g.wsPaused() && !g.mayInstall(2000));
  g.grant(false);
  CHECK(g.waiting() && !g.wsPaused());
  g.grant(true);
  CHECK(g.wsPaused() && g.mayInstall(3000));
  g.finish();
  CHECK(!g.wsPaused() && !g.busy());
  // Never granted: the worker gives up after the timeout and the socket was never paused.
  CHECK(g.start());
  g.requestInstall(0);
  CHECK(!g.mayInstall(60001) && !g.waiting() && !g.wsPaused());
  g.grant(true);  // late grant after the give-up is ignored
  CHECK(!g.wsPaused());
  g.finish();
  CHECK(!g.busy());
}

static void test_battery_policy() {
  // Caleb 8 Nov 2026: low 25, critical 15 (one-time shutdown warning), shutdown 10, restart 15.
  CHECK(BatteryAlertPolicy::kLow == 25 && BatteryAlertPolicy::kCritical == 15 && BatteryAlertPolicy::kShutdown == 10 &&
        BatteryAlertPolicy::kRestart == 15);
  BatteryAlertPolicy p;
  CHECK(p.lowPercent() == 25 && p.criticalPercent() == 15);
  CHECK(p.update(true, 80, 0) == 0 && p.level() == BatteryLevel::None);
  CHECK(p.update(true, 26, 500) == 0 && p.level() == BatteryLevel::None);
  CHECK(p.update(true, 25, 1000) == BatteryAlertPolicy::BatteryChanged && p.level() == BatteryLevel::Low);
  CHECK(p.update(true, 27, 2000) == 0 && p.level() == BatteryLevel::Low);     // hysteresis 3
  CHECK(p.update(true, 16, 2500) == 0 && p.level() == BatteryLevel::Low);
  CHECK(p.update(true, 15, 3000) == BatteryAlertPolicy::BatteryChanged && p.level() == BatteryLevel::Critical);
  // No 5 % level any more: 10 % and below stay Critical (the 10 % shutdown is the battery_critical event).
  CHECK(p.update(true, 10, 4000) == 0 && p.level() == BatteryLevel::Critical);
  CHECK(p.update(true, 4, 5000) == 0 && p.level() == BatteryLevel::Critical);
  // One-time: a wobble across 15/16 % does not repeat it.
  CHECK(p.update(true, 16, 6000) == 0 && p.update(true, 15, 7000) == 0 && p.update(true, 17, 8000) == 0);
  // Latch held (restart blocked until 15 %): stays Critical even at 30 %.
  CHECK(p.update(true, 30, 9000, true) == 0 && p.update(true, 30, 200000, true) == 0 && p.level() == BatteryLevel::Critical);
  // Released: Critical clears only after > 18 % held 60 s.
  CHECK(p.update(true, 30, 210000) == 0);
  CHECK(p.update(true, 30, 269999) == 0 && p.level() == BatteryLevel::Critical);
  CHECK(p.update(true, 30, 270000) == BatteryAlertPolicy::BatteryChanged && p.level() == BatteryLevel::None);
  // Leaving at 20 % goes to Low, not None.
  BatteryAlertPolicy q;
  q.update(true, 14, 0);
  CHECK(q.level() == BatteryLevel::Critical);
  q.update(true, 20, 1000);
  CHECK(q.update(true, 20, 61000) == BatteryAlertPolicy::BatteryChanged && q.level() == BatteryLevel::Low);
  // A dip back to <= 18 % restarts the 60 s hold.
  BatteryAlertPolicy h; h.update(true, 14, 0); h.update(true, 20, 1000); h.update(true, 18, 30000); h.update(true, 20, 40000);
  CHECK(h.update(true, 20, 61000) == 0 && h.update(true, 20, 100000) == BatteryAlertPolicy::BatteryChanged);
  // Unavailable after 15 s of invalid readings.
  CHECK(p.update(false, 0, 280000) == 0 && p.available());
  CHECK(p.update(false, 0, 280000 + BatteryAlertPolicy::kUnavailableAfterMs) == BatteryAlertPolicy::AvailabilityChanged && !p.available());
  CHECK(p.update(true, 50, 300000) == BatteryAlertPolicy::AvailabilityChanged && p.available());
  CHECK(!strcmp(batteryLevelType(BatteryLevel::Critical), "battery_critical"));
  CHECK(!strcmp(batteryLevelType(BatteryLevel::Low), "battery_low"));
  CHECK(!strcmp(batteryLevelType(BatteryLevel::None), "battery_low"));
  // Saved Manual values (/v1/battery-settings low 40, warning 20) drive the levels.
  BatteryAlertPolicy m;
  CHECK(m.setThresholds(40, 20) && m.lowPercent() == 40 && m.criticalPercent() == 20);
  CHECK(m.update(true, 40, 0) == BatteryAlertPolicy::BatteryChanged && m.level() == BatteryLevel::Low);
  CHECK(m.update(true, 20, 1000) == BatteryAlertPolicy::BatteryChanged && m.level() == BatteryLevel::Critical);
  // Invalid pairs fall back to 25/15.
  CHECK(!m.setThresholds(15, 20) && m.lowPercent() == 25 && m.criticalPercent() == 15);
  CHECK(!m.setThresholds(30, 10) && !m.setThresholds(96, 20) && !m.setThresholds(30, 81) && !m.setThresholds(NAN, 15));
  CHECK(m.setThresholds(12, 11) && !BatteryAlertPolicy::validThresholds(11, 11));
}

// ---- JK-BMS (JK-PB 300-byte frames) --------------------------------------------
// Assembled byte by byte from the public JK-PB/JK02 32S dynamic-frame layout
// (github.com/syssi/esphome-jk-bms, jk_bms_ble / jk_rs485_bms) and the offsets in
// the Dubem collector parser, for a 16S LiFePO4 pack: cells about 3.31 V,
// 52.950 V, -12.5 A, SOC 76 %, 42 cycles, cell undervoltage alarm. Replace with
// a bench capture from the fitted BMS once available.
static const uint8_t kJkDynamic[] = {
    0x55, 0xAA, 0xEB, 0x90, 0x02, 0x7C, 0xF0, 0x0C, 0xEC, 0x0C, 0xF3, 0x0C, 0xE5, 0x0C, 0xEE, 0x0C,
    0xED, 0x0C, 0xEA, 0x0C, 0xF1, 0x0C, 0xEF, 0x0C, 0xE8, 0x0C, 0xEB, 0x0C, 0xF0, 0x0C, 0xE9, 0x0C,
    0xEE, 0x0C, 0xEC, 0x0C, 0xED, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xED, 0x0C, 0x0E, 0x00, 0x02, 0x03,
    0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00,
    0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x38, 0x01, 0x00, 0x00, 0x00, 0x00, 0xD6, 0xCE, 0x00, 0x00, 0x8D, 0xE6, 0xF5, 0xFF, 0x2C, 0xCF,
    0xFF, 0xFF, 0x13, 0x01, 0xE0, 0xFF, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x02, 0x4C, 0xE0, 0x28,
    0x01, 0x00, 0xA0, 0x86, 0x01, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x94};

static void test_jk_bms() {
  using namespace pc::jk;
  Data d;
  CHECK(parseDynamic(kJkDynamic, sizeof kJkDynamic, d) == Parse::Ok);
  CHECK(d.cellCount == 16 && d.cellMv[0] == 3312 && d.cellMv[15] == 3309 && d.cellDiffMv == 14);
  CHECK(d.cellAvgMv == 3309 && d.cellMax == 3 && d.cellMin == 4);
  CHECK(d.packMv == 52950 && d.currentMa == -12500 && d.powerMw == -661875);
  NEAR(d.mosTempC, 31.2, 0.01); NEAR(d.tempC[0], 27.5, 0.01); NEAR(d.tempC[1], -3.2, 0.01);
  CHECK(d.soc == 76 && d.soh == 100 && d.cycles == 42 && d.status == 2);
  CHECK(d.remainMah == 76000 && d.fullMah == 100000);
  CHECK(d.chargeMos && d.dischargeMos && !d.balancing);
  CHECK(d.alarms == (1u << 11) && !strcmp(alarmName(11), "cell_undervoltage") && alarmName(24) == nullptr);
  // Failures never touch the output
  Data e; e.soc = 99;
  uint8_t f[sizeof kJkDynamic]; memcpy(f, kJkDynamic, sizeof f);
  f[160] ^= 0x01;
  CHECK(parseDynamic(f, sizeof f, e) == Parse::ChecksumError);
  f[160] ^= 0x01; f[4] = kTypeSetup; f[299] = frameChecksum(f);
  CHECK(parseDynamic(f, sizeof f, e) == Parse::NotDynamic);
  f[4] = kTypeDynamic; f[173] = 101; f[299] = frameChecksum(f);
  CHECK(parseDynamic(f, sizeof f, e) == Parse::BadFrame);  // implausible SOC
  CHECK(parseDynamic(kJkDynamic, 299, e) == Parse::BadFrame);
  CHECK(e.soc == 99);
  // Assembler: junk, a false header start, a frame; then a stalled frame is dropped
  Assembler a; uint32_t t = 0; int got = 0;
  const uint8_t junk[] = {0x00, 0x55, 0xAA, 0x13, 0x55, 0x55};
  for (uint8_t b : junk) got += a.feed(b, t);
  for (size_t i = 0; i < sizeof kJkDynamic; ++i) got += a.feed(kJkDynamic[i], t);
  CHECK(got == 1 && memcmp(a.buf(), kJkDynamic, sizeof kJkDynamic) == 0);
  got = 0;
  for (size_t i = 0; i < 150; ++i) got += a.feed(kJkDynamic[i], t);
  t += 500;
  for (size_t i = 0; i < sizeof kJkDynamic; ++i) got += a.feed(kJkDynamic[i], t);
  CHECK(got == 1 && a.timeouts() == 1 && parseDynamic(a.buf(), kFrameLen, e) == Parse::Ok && e.soc == 76);

  // A reconnect/boot frame can carry SOC 0 while remaining capacity is still ~96%.
  // That byte is not a measurement: it must not publish, so it cannot latch or release.
  uint8_t glitch[sizeof kJkDynamic];
  memcpy(glitch, kJkDynamic, sizeof glitch);
  glitch[173] = 0;
  glitch[174] = 0x00; glitch[175] = 0x77; glitch[176] = 0x01; glitch[177] = 0x00;  // 96000 mAh
  glitch[178] = 0xA0; glitch[179] = 0x86; glitch[180] = 0x01; glitch[181] = 0x00;  // 100000 mAh
  glitch[299] = frameChecksum(glitch);
  Data kept;
  kept.soc = 96;
  CHECK(parseDynamic(glitch, sizeof glitch, kept) == Parse::BadFrame);
  CHECK(kept.soc == 96);
  // SOC 0 with no capacity total is incomplete, not a real empty pack.
  memcpy(glitch, kJkDynamic, sizeof glitch);
  glitch[173] = 0;
  glitch[174] = glitch[175] = glitch[176] = glitch[177] = 0;
  glitch[178] = glitch[179] = glitch[180] = glitch[181] = 0;
  glitch[299] = frameChecksum(glitch);
  CHECK(parseDynamic(glitch, sizeof glitch, kept) == Parse::BadFrame);
  CHECK(kept.soc == 96);
  // A consistent empty pack still parses, and a consistent 96% still parses.
  memcpy(glitch, kJkDynamic, sizeof glitch);
  glitch[173] = 0;
  glitch[174] = glitch[175] = glitch[176] = glitch[177] = 0;
  glitch[178] = 0xA0; glitch[179] = 0x86; glitch[180] = 0x01; glitch[181] = 0x00;
  glitch[299] = frameChecksum(glitch);
  CHECK(parseDynamic(glitch, sizeof glitch, kept) == Parse::Ok && kept.soc == 0);
  memcpy(glitch, kJkDynamic, sizeof glitch);
  glitch[173] = 96;
  glitch[174] = 0x00; glitch[175] = 0x77; glitch[176] = 0x01; glitch[177] = 0x00;
  glitch[178] = 0xA0; glitch[179] = 0x86; glitch[180] = 0x01; glitch[181] = 0x00;
  glitch[299] = frameChecksum(glitch);
  CHECK(parseDynamic(glitch, sizeof glitch, kept) == Parse::Ok && kept.soc == 96 && kept.remainMah == 96000);
  // A high SOC byte with an empty remaining count must not release a latch.
  memcpy(glitch, kJkDynamic, sizeof glitch);
  glitch[173] = 96;
  glitch[174] = glitch[175] = glitch[176] = glitch[177] = 0;
  glitch[178] = 0xA0; glitch[179] = 0x86; glitch[180] = 0x01; glitch[181] = 0x00;
  glitch[299] = frameChecksum(glitch);
  kept.soc = 11;
  CHECK(parseDynamic(glitch, sizeof glitch, kept) == Parse::BadFrame);
  CHECK(kept.soc == 11);
}

static bool hexIs(const uint8_t *d, size_t n, const char *want) { char h[129]; toHex(d, n, h); return !strcmp(h, want); }

static void test_sha256() {
  uint8_t d[32];
  sha256("abc", 3, d);
  CHECK(hexIs(d, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  sha256(m, strlen(m), d);
  CHECK(hexIs(d, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  Sha256 s; for (int i = 0; i < 1000; ++i) s.update("aaaaaaaaaa", 10); s.finish(d);  // incremental, 10000 x 'a'
  uint8_t e[32]; static char big[10000]; memset(big, 'a', sizeof big); sha256(big, sizeof big, e);
  CHECK(!memcmp(d, e, 32));
  hmacSha256((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, d);  // RFC 4231 case 2
  CHECK(hexIs(d, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
  uint8_t k131[131]; memset(k131, 0xaa, sizeof k131);                       // RFC 4231 case 6 (long key)
  const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
  hmacSha256(k131, sizeof k131, m6, strlen(m6), d);
  CHECK(hexIs(d, 32, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
  pbkdf2Sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, d, 32);
  CHECK(hexIs(d, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
  pbkdf2Sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 2, d, 32);
  CHECK(hexIs(d, 32, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
  pbkdf2Sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096, d, 32);
  CHECK(hexIs(d, 32, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
  uint8_t b[3]; CHECK(fromHex("0aFf10", b, 3) && b[1] == 0xff && !fromHex("0aFg10", b, 3) && !fromHex("0a", b, 3));
}

static void test_passcode() {
  using namespace pc::onb;
  CHECK(validPasscode("482019") && !validPasscode("111111") && !validPasscode("48201") && !validPasscode("4820193"));
  CHECK(!validPasscode("48a019") && !validPasscode("") && !validPasscode(nullptr) && !validPasscode("48201937"));
  uint8_t salt[16]; for (int i = 0; i < 16; ++i) salt[i] = uint8_t(i);
  Verifier v; makeVerifier("482019", salt, v);
  CHECK(hexIs(v.dk, 32, "797d49640973c107a34a17a5545b7ac43226f7ee84faa9127292a257851fa277"));  // python hashlib
  CHECK(checkPasscode("482019", v) && !checkPasscode("482018", v) && !checkPasscode("", v));
  char fp[33]; fingerprint("dev-secret-0123456789", v, fp);
  CHECK(!strcmp(fp, "924a69d94c86528bc6a97a4bfbd29ff2"));
  char hx[97]; verifierHex(v, hx); Verifier w;
  CHECK(strlen(hx) == 96 && verifierFromHex(hx, w) && sameVerifier(v, w) && !verifierFromHex("00", w));
  hx[5] = 'z'; CHECK(!verifierFromHex(hx, w));
  // Random generators: format and edge cases
  uint8_t r[24] = {0}; char p[7];
  passcodeFromRandom(r, p); CHECK(validPasscode(p) && strlen(p) == 6);                  // 000000 -> fixed
  for (int i = 0; i < 8; ++i) r[i] = 0xff;
  passcodeFromRandom(r, p); CHECK(validPasscode(p));
  char ap[13]; apPasswordFromRandom(r, ap); CHECK(strlen(ap) == 12 && !strpbrk(ap, "0O1lI"));
  char cc[9]; uint8_t c5[5] = {0x00, 0x44, 0x32, 0x14, 0xc7}; claimCodeFromRandom(c5, cc);
  CHECK(!strcmp(cc, "ABCDEFGH"));  // base32 of 00 44 32 14 c7
  char n[9]; CHECK(normalizeClaimCode("abcd-efgh", n) && !strcmp(n, "ABCDEFGH") && !normalizeClaimCode("ABCDEFG1", n) && !normalizeClaimCode("ABCDEFGHA", n));
  char h[65]; claimHash("AABBCCDDEEFF", "ABCDEFGH", h);
  CHECK(!strcmp(h, "5b2736ccef0fa883082042178aa65d915e3b867d19e3c0de31eec55785e685b6"));
  char tok[49]; tokenFromRandom(r, tok); CHECK(strlen(tok) == 48);
}

static void test_limiter() {
  using namespace pc::onb;
  Limiter l; uint32_t t = 1000, ra = 0;
  for (int i = 0; i < 4; ++i) { l.fail(t); CHECK(!l.locked(t)); }
  l.fail(t); CHECK(l.locked(t, &ra) && ra == 30 && !l.locked(t + 30000));
  l.fail(t + 30000); CHECK(l.locked(t + 30000, &ra) && ra == 60);
  l.fail(t + 90000); CHECK(l.locked(t + 90000, &ra) && ra == 120);
  CHECK(Limiter::lockSeconds(4) == 0 && Limiter::lockSeconds(9) == 480 && Limiter::lockSeconds(10) == 900 && Limiter::lockSeconds(40) == 900);
  l.success(); CHECK(!l.locked(t + 90000) && l.fails == 0);
  Limiter w; w.fails = 4; w.fail(0xFFFFF000u); CHECK(w.locked(0xFFFFF000u) && w.locked(0x00000100u) && !w.locked(0x00010000u));  // millis wrap
}

static void test_pairs_sessions() {
  using namespace pc::onb;
  PairStore ps; ps.clear();
  CHECK(ps.issue("phoneA", "Pixel", "tokA", 100) >= 0 && ps.count() == 1);
  CHECK(ps.find("tokA") >= 0 && ps.find("tokB") < 0 && ps.find("") < 0);
  int a = ps.issue("phoneA", "Pixel", "tokA2", 200);  // re-issue replaces the token, keeps created
  CHECK(ps.count() == 1 && ps.find("tokA") < 0 && ps.find("tokA2") == a && ps.p[a].created == 100 && ps.p[a].lastSeen == 200);
  char tok[16];
  for (int i = 0; i < 8; ++i) { snprintf(tok, sizeof tok, "t%d", i); char id[16]; snprintf(id, sizeof id, "p%d", i); ps.issue(id, "", tok, 300 + i); }
  CHECK(ps.count() == 8 && ps.find("tokA2") < 0 && ps.find("t0") >= 0 && ps.find("t7") >= 0);  // oldest (phoneA) evicted
  CHECK(ps.remove("p3") && !ps.remove("p3") && ps.count() == 7 && ps.find("t3") < 0);
  int keep = ps.find("t5"); ps.keepOnly(keep);
  CHECK(ps.count() == 1 && ps.find("t5") == keep);
  char longId[60]; memset(longId, 'x', 59); longId[59] = 0;
  int s = ps.issue(longId, longId, "tl", 1); CHECK(strlen(ps.p[s].id) == kPhoneIdMax && strlen(ps.p[s].name) == kPhoneNameMax);
  ps.clear(); CHECK(ps.count() == 0);
  Sessions ss; uint32_t now = 5000;
  ss.issue("s1", now); CHECK(ss.valid("s1", now) && !ss.valid("s2", now) && !ss.valid("s1", now + kSessionTtlMs));
  for (int i = 0; i < 4; ++i) { snprintf(tok, sizeof tok, "x%d", i); ss.issue(tok, now + 10 + i); }
  CHECK(!ss.valid("s1", now + 20) && ss.valid("x0", now + 20) && ss.valid("x3", now + 20));  // oldest replaced
  ss.issue("sp", now + 30, "phoneZ"); int si = ss.find("sp", now + 31);
  CHECK(si >= 0 && !strcmp(ss.s[si].phone, "phoneZ") && ss.find("nope", now) < 0);
  ss.clear(); CHECK(!ss.valid("x3", now + 20));
}

static void test_wifi_ap_policy() {
  using namespace pc::onb;
  CHECK(!strcmp(wifiFailReason(15, false), "wrong_password") && !strcmp(wifiFailReason(204, false), "wrong_password"));
  CHECK(!strcmp(wifiFailReason(202, false), "wrong_password") && !strcmp(wifiFailReason(201, false), "not_found"));
  CHECK(!strcmp(wifiFailReason(201, true), "5ghz_only") && wifiFailReason(8, false) == nullptr);
  ApPolicy a; a.begin(0);
  CHECK(a.update(false, false, false, 0));                       // unconfigured: hotspot on
  ApPolicy b; b.begin(0);
  CHECK(!b.update(true, false, false, 299999) && b.update(true, false, false, 300000));  // 5 min fallback
  CHECK(b.update(true, false, false, 400000));                   // stays on while STA is down
  CHECK(b.update(true, true, false, 500000) && b.update(true, true, false, 619999) && !b.update(true, true, false, 620000));  // 120 s grace
  CHECK(!b.update(true, false, false, 700000) && !b.update(true, false, false, 999999) && b.update(true, false, false, 1000000));
  ApPolicy c; c.begin(0); c.update(true, true, false, 10);       // online, hotspot off
  CHECK(!c.update(true, true, false, 200000));
  c.force(200000); CHECK(c.update(true, true, false, 200001) && c.update(true, true, false, 799999) && !c.update(true, true, false, 800000));  // AP ON
  CHECK(c.update(true, true, true, 800001));                     // testing keeps it up
  c.restartGrace(900000); CHECK(c.update(true, true, false, 900001) && !c.update(true, true, false, 1020000));  // wifi/save grace
  CHECK(!strcmp(ApPolicy::stateName(false, false, false, true), "unconfigured") && !strcmp(ApPolicy::stateName(true, true, true, true), "testing"));
  CHECK(!strcmp(ApPolicy::stateName(true, true, false, false), "online") && !strcmp(ApPolicy::stateName(true, false, false, true), "fallback"));
  CHECK(!strcmp(ApPolicy::stateName(true, false, false, false), "connecting"));
  CHECK(!strcmp(ApPolicy::stateName(true, true, false, true), "grace"));
  // v14 hotspot-only mode
  { NetMode m = NetMode::Unset;
    CHECK(parseNetMode("hotspot_only", m) && m == NetMode::HotspotOnly);
    CHECK(parseNetMode("station", m) && m == NetMode::Station);
    CHECK(!parseNetMode("unset", m) && !parseNetMode("", m) && !parseNetMode(nullptr, m) && !parseNetMode("HOTSPOT_ONLY", m));
    CHECK(netModeFromStored("") == NetMode::Unset && netModeFromStored("junk") == NetMode::Unset && netModeFromStored("hotspot_only") == NetMode::HotspotOnly);
    CHECK(!strcmp(netModeName(NetMode::Unset), "unset") && !strcmp(netModeName(NetMode::Station), "station") && !strcmp(netModeName(NetMode::HotspotOnly), "hotspot_only"));
    CHECK(!strcmp(ApPolicy::stateName(false, false, false, true, true), "hotspot_only"));  // no 'unconfigured'
    CHECK(!strcmp(ApPolicy::stateName(true, false, true, true, true), "testing"));
    ApPolicy h; h.begin(0);
    CHECK(h.update(false, false, false, 1000, true));
    CHECK(h.update(true, true, false, 10000, true));
    CHECK(h.update(true, true, false, 10000 + 3600000, true));  // never times out (no 120 s off)
    // hotspot_only -> station: the existing grace then AP-off
    h.restartGrace(4000000); CHECK(h.update(true, true, false, 4060000, false)); CHECK(!h.update(true, true, false, 4130000, false));
  }
  { ApPolicy g; g.begin(0); CHECK(g.update(false, false, false, 1000));  // unprovisioned: AP on
    g.restartGrace(10000); CHECK(g.update(true, true, false, 60000));      // +50 s after save: still grace, no fallback
    CHECK(!g.update(true, true, false, 130000)); CHECK(!g.update(true, true, false, 200000)); }
  ApPolicy none; none.fallbackMs = 30000; none.graceMs = 120000; none.begin(0);
  CHECK(none.update(false, false, false, 0));  // no credentials: hotspot immediately
  ApPolicy loss; loss.fallbackMs = 30000; loss.graceMs = 120000; loss.begin(0);
  CHECK(!loss.update(true, false, false, 29999) && loss.update(true, false, false, 30000));
  CHECK(loss.update(true, true, false, 30000) && loss.update(true, true, false, 149999));
  CHECK(!loss.update(true, true, false, 150000));  // 120 s after the station returns
  // STM32 and DSP product images share kApFallbackMs / kApGraceMs / kWifiRetryMs.
  CHECK(kApFallbackMs == 30000 && kApGraceMs == 120000 && kWifiRetryMs == 5000);
  ApPolicy boot; boot.fallbackMs = kApFallbackMs; boot.graceMs = kApGraceMs; boot.begin(0);
  CHECK(boot.update(false, false, false, 0));                              // boot, no credentials
  ApPolicy stm; stm.fallbackMs = kApFallbackMs; stm.graceMs = kApGraceMs; stm.begin(0);
  CHECK(!stm.update(true, false, false, kApFallbackMs - 1));
  CHECK(stm.update(true, false, false, kApFallbackMs));                    // loss
  CHECK(stm.update(true, true, false, kApFallbackMs));                     // station returns, grace starts
  CHECK(stm.update(true, true, false, kApFallbackMs + kApGraceMs - 1));
  CHECK(!stm.update(true, true, false, kApFallbackMs + kApGraceMs));       // grace ends
  const uint32_t dropped = kApFallbackMs + kApGraceMs + 1000;
  CHECK(!stm.update(true, false, false, dropped));                         // flap: timer restarts
  CHECK(!stm.update(true, false, false, dropped + kApFallbackMs - 1));
  CHECK(stm.update(true, false, false, dropped + kApFallbackMs));
  ApPolicy setup; setup.fallbackMs = kApFallbackMs; setup.graceMs = kApGraceMs; setup.begin(0);
  CHECK(setup.update(true, false, true, 0));                               // onboarding test, before the loss timer
  CHECK(setup.update(true, true, true, 1000));
}

static void test_ws_rules() {
  using namespace pc::onb;
  const char *rid = "5d0f8a6e-2c1b-4e7f-9a3d-1b2c3d4e5f60";
  CHECK(setPasscodeError(rid, true, 1000, 2000, "482019") == nullptr);
  CHECK(setPasscodeError(rid, true, 0, 2000, "482019") == nullptr);   // clock not set: expiry skipped
  CHECK(!strcmp(setPasscodeError(rid, false, 1000, 2000, "482019"), "insecure_transport"));
  CHECK(!strcmp(setPasscodeError(rid, true, 2000, 2000, "482019"), "expired"));
  CHECK(!strcmp(setPasscodeError(rid, true, 1000, 2000, "48201937"), "invalid_passcode"));
  CHECK(!strcmp(setPasscodeError("", true, 1000, 2000, "482019"), "invalid_request_id"));
  char longRid[49]; memset(longRid, 'a', 48); longRid[48] = 0;
  CHECK(!strcmp(setPasscodeError(longRid, true, 1000, 2000, "482019"), "invalid_request_id"));
  CHECK(validUnclaimReason("owner_request") && validUnclaimReason("transfer") && validUnclaimReason("support") && !validUnclaimReason("x") && !validUnclaimReason(nullptr));
  CHECK(unclaimClearsPairs("transfer") && unclaimClearsPairs("support") && !unclaimClearsPairs("owner_request"));
  CHECK(validOwnerId("usr_0123456789abcdef"));  // PR #6 format: usr_ + 16 hex
  CHECK(validOwnerId("usr_8f3k2m") && !validOwnerId("") && !validOwnerId("a b") && !validOwnerId("a\"b"));
}

static void test_legacy_event_lines() {
  LegacyTextParser p; Stm32Status out; int pub = 0;
  feedStr(p, ">>> FAULT: 0x01 <<<\nFAULT: Permanent fault - manual reset required\nFAULT: Max restart attempts (3) reached\n"
             "--- Status ---\nSYS : FAULT\nCHR : IDLE\n", out, pub);
  CHECK(p.flushIdle(out));
  CHECK(out.fault == (0x01u | ev::kStatusPermanent | ev::kStatusRestartsExhausted) && ev::faultActive(out.fault));
  feedStr(p, "CHR: Battery confirmed full\nWARN: High current 19.5A\nGRID: Lost (120.0V)\n--- Status ---\nSYS : GRID\nCHR : FLOAT\nGRID: 230.0V\n", out, pub);
  CHECK(p.flushIdle(out));
  CHECK(out.fault == ev::kStatusChargerFull && !ev::faultActive(out.fault));  // latches cleared when SYS left FAULT
  NEAR(out.grid_v, 230.0, 0.01);  // "GRID: Lost (120.0V)" is an event line, not the reading
  feedStr(p, "--- Status ---\nSYS : GRID\nCHR : BULK\n", out, pub);
  CHECK(p.flushIdle(out) && out.fault == 0);
}

static int countType(const ev::Event *e, size_t n, ev::Type t, bool active) {
  int c = 0; for (size_t i = 0; i < n; ++i) c += e[i].type == t && e[i].active == active; return c;
}

static void test_events_engine() {
  using namespace ev;
  Engine g; Inputs in; ev::Event e[24]; size_t n;
  in.stmOnline = true; in.framed = false; in.sys = SysState::Inverter; in.hasIout = true; in.iout = 19.5f;
  in.hasGrid = true; in.gridV = 230; in.hasTemp = true; in.temp = 40; in.socValid = true; in.soc = 60;
  g.update(in, 0, 1000); CHECK(g.drain(e, 24) == 0);           // overload needs 2 s
  g.update(in, 1500, 1001); CHECK(g.drain(e, 24) == 0);
  g.update(in, 2100, 1002); n = g.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::Overload && e[0].active && e[0].severity == Sev::Warning && !strcmp(e[0].source, "esp"));
  in.iout = 18.0f; g.update(in, 3000, 1003); g.update(in, 9000, 1009); CHECK(g.drain(e, 24) == 0);  // hysteresis holds above 17 A
  in.iout = 10.0f; g.update(in, 10000, 1010); g.update(in, 14000, 1014); CHECK(g.drain(e, 24) == 0);
  g.update(in, 15100, 1015); n = g.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::Overload && !e[0].active && e[0].hasValue && e[0].value == 10.0f);
  // framed link without bit16 (every STM32 today): the ESP 19 A rule still runs
  Engine f; in.framed = true; in.iout = 25; f.update(in, 0, 1); f.update(in, 2100, 2); n = f.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::Overload && e[0].active && !strcmp(e[0].source, "esp"));
  // once the STM32 sets bit16, bit16 drives overload
  in.fault = kStatusOverloadWarn; f.update(in, 5100, 3); n = f.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::Overload && e[0].active && !strcmp(e[0].source, "stm32") && !strcmp(e[0].code, "stm32_warn_19a"));
  // permanent and non-permanent faults
  in.fault = 0x01 | 0x08; f.update(in, 6000, 4); n = f.drain(e, 24);
  CHECK(countType(e, n, Type::Overload, false) == 1);
  bool permOc = false, nonPermUv = false;
  for (size_t i = 0; i < n; ++i) {
    if (!strcmp(e[i].id, "stm32_0x01")) permOc = e[i].permanent && e[i].type == Type::Overcurrent && e[i].severity == Sev::Critical;
    if (!strcmp(e[i].id, "stm32_0x08")) nonPermUv = !e[i].permanent && e[i].type == Type::BatteryLow;
  }
  CHECK(permOc && nonPermUv);
  in.fault = 0x08 | kStatusPermanent | kStatusRestartsExhausted; f.update(in, 7000, 5); n = f.drain(e, 24);
  bool upgraded = false, exhausted = false;
  for (size_t i = 0; i < n; ++i) {
    if (!strcmp(e[i].id, "stm32_0x08")) upgraded = e[i].active && e[i].permanent;  // bit17 latch marks every active fault
    if (!strcmp(e[i].id, "stm32_restarts_exhausted")) exhausted = e[i].active && e[i].permanent && e[i].type == Type::InverterFault;
  }
  CHECK(upgraded && exhausted);
  // STM32 offline: conditions held, no clears
  in.stmOnline = false; in.fault = 0; f.update(in, 8000, 6); CHECK(f.drain(e, 24) == 0);
  // grid down with debounce + restore hysteresis
  Engine gr; Inputs gi; gi.stmOnline = true; gi.hasGrid = true; gi.gridV = 100;
  gr.update(gi, 0, 1); gr.update(gi, 3100, 2); n = gr.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::GridDown && e[0].active);
  gi.gridV = 165; gr.update(gi, 4000, 3); gr.update(gi, 8000, 4); CHECK(gr.drain(e, 24) == 0);
  gi.gridV = 225; gr.update(gi, 9000, 5); gr.update(gi, 12100, 6); n = gr.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::GridDown && !e[0].active);
  // battery full from bit19, battery low from SoC policy with severity change
  Engine b; Inputs bi; bi.stmOnline = true; bi.fault = kStatusChargerFull; bi.socValid = true; bi.soc = 98;
  b.update(bi, 0, 1); b.update(bi, 10100, 2); n = b.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::BatteryFull && e[0].severity == Sev::Info && !strcmp(e[0].code, "charger_full"));
  bi.fault = 0; bi.soc = 25; b.update(bi, 11000, 3); b.update(bi, 21100, 4); n = b.drain(e, 24);
  CHECK(countType(e, n, Type::BatteryFull, false) == 1 && countType(e, n, Type::BatteryLow, true) == 1);
  for (size_t i = 0; i < n; ++i) if (e[i].type == Type::BatteryLow) CHECK(!strcmp(e[i].code, "battery_low") && e[i].severity == Sev::Warning);
  bi.soc = 15; b.update(bi, 22000, 5); n = b.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::BatteryLow && e[0].severity == Sev::Critical && !strcmp(e[0].code, "battery_critical"));
  bi.soc = 5; b.update(bi, 23000, 6); CHECK(b.drain(e, 24) == 0);  // no shutdown_imminent level any more
  // Saved Manual thresholds (low 40, warning 20) feed battery_soc.
  Engine sv; Inputs svi; svi.socValid = true; svi.soc = 35; svi.batLowPct = 40; svi.batCritPct = 20;
  sv.update(svi, 0, 1); n = sv.drain(e, 24);
  CHECK(n == 1 && !strcmp(e[0].id, "battery_soc") && !strcmp(e[0].code, "battery_low"));
  svi.soc = 20; sv.update(svi, 1000, 2); n = sv.drain(e, 24);
  CHECK(n == 1 && !strcmp(e[0].code, "battery_critical"));
  Engine dv; Inputs di; di.socValid = true; di.soc = 35; dv.update(di, 0, 1); CHECK(dv.drain(e, 24) == 0);  // default 25: none
  // BMS alarms
  Engine m; Inputs mi; mi.bmsOnline = true; mi.bmsAlarms = (1u << 13) | (1u << 21);
  m.update(mi, 0, 1); n = m.drain(e, 24);
  CHECK(countType(e, n, Type::Overcurrent, true) == 1 && countType(e, n, Type::OverTemp, true) == 1);
  mi.bmsAlarms = 0; m.update(mi, 1000, 2); CHECK(m.drain(e, 24) == 0); m.update(mi, 6100, 3); CHECK(m.drain(e, 24) == 2);
  // sockets: 8 A warning, 10 A critical, socket trip flag, relay results
  Engine s; Inputs si; si.nSockets = 1; strcpy(si.sockets[0].id, "A1B2C3D4E5F6"); si.sockets[0].online = true;
  si.sockets[0].current = 8.5f; s.update(si, 0, 1); CHECK(s.drain(e, 24) == 0);
  s.update(si, 1100, 2); n = s.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::Overcurrent && e[0].severity == Sev::Warning && !strcmp(e[0].socket, "A1B2C3D4E5F6") && !strcmp(e[0].code, "socket_warn_8a"));
  si.sockets[0].current = 10.2f; s.update(si, 1200, 3); n = s.drain(e, 24);
  CHECK(n == 1 && e[0].active && e[0].severity == Sev::Critical);
  si.sockets[0].current = 0; si.sockets[0].alertOvercurrent = true; s.update(si, 1300, 4); n = s.drain(e, 24);
  CHECK(n == 1 && !strcmp(e[0].code, "socket_tripped_10a"));
  si.sockets[0].alertOvercurrent = false; s.update(si, 2000, 5); s.update(si, 7100, 6); n = s.drain(e, 24);
  CHECK(n == 1 && !e[0].active);
  strcpy(si.sockets[0].cmd, "timeout"); s.update(si, 8000, 7); n = s.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::RelayFailed && e[0].active && !strcmp(e[0].code, "hardware_timeout"));
  CHECK(s.activeCount() == 1 && s.active(0)->type == Type::RelayFailed);
  strcpy(si.sockets[0].cmd, "confirmed"); s.update(si, 9000, 8); n = s.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::RelayFailed && !e[0].active && s.activeCount() == 0);
  CHECK(!strcmp(typeName(Type::InverterFault), "inverter_fault") && !strcmp(sevName(Sev::Critical), "critical"));
}

static void test_lora_proto() {
  using namespace lora;
  SocketPacket k; const char *a = "a1b2c3d4e5f6,SOCKET CURRENT:20.512,SOCKET RELAY:0,FW:1.1.0,ACK:c1-7,ALERT:OVERCURRENT";
  CHECK(parseSocketPacket(a, strlen(a), k));
  CHECK(!strcmp(k.id, "A1B2C3D4E5F6") && fabs(k.current - 20.512) < 1e-3 && k.relay == 0 && !strcmp(k.fw, "1.1.0") && !strcmp(k.ack, "c1-7") && k.overcurrent && !k.warning);
  const char *w = "A1B2C3D4E5F6,SOCKET CURRENT:16.2,SOCKET RELAY:1,FW:1.1.0,WARN:CURRENT_HIGH";
  CHECK(parseSocketPacket(w, strlen(w), k) && k.warning && !k.overcurrent && k.relay == 1 && !k.ack[0]);
  const char *bad[] = {"A1B2C3D4E5F,SOCKET CURRENT:1,SOCKET RELAY:1", "A1B2C3D4E5F6,SOCKET CURRENT:nan,SOCKET RELAY:1",
                       "A1B2C3D4E5F6,SOCKET CURRENT:1,SOCKET RELAY:2", "A1B2C3D4E5F6,Power Restored:", "A1B2C3D4E5F6,RELAY_ON,abc"};
  for (auto b : bad) CHECK(!parseSocketPacket(b, strlen(b), k));
  char buf[80];
  CHECK(buildRelayCommand("A1B2C3D4E5F6", true, "c1a2-9", buf, sizeof buf) && !strcmp(buf, "A1B2C3D4E5F6,RELAY_ON,c1a2-9"));
  CHECK(!buildRelayCommand("A1B2C3D4E5F6", false, "bad id", buf, sizeof buf) && !buildRelayCommand("XYZ", false, "c1", buf, sizeof buf));
  CHECK(!buildRelayCommand("A1B2C3D4E5F6", false, "c1", buf, 10));
  CHECK(kTripAmps == ev::kSocketTripA && kWarnAmps == ev::kSocketWarnA);
  // 2.2 lockout field + RESET_LOCKOUT command
  const char *l = "A1B2C3D4E5F6,SOCKET CURRENT:0.000,SOCKET RELAY:0,FW:1.2.0,ACK:g1-3,ALERT:LOCKOUT,TRIP CURRENT:21.304";
  CHECK(parseSocketPacket(l, strlen(l), k) && k.lockout && !k.overcurrent && k.hasTripCurrent && fabs(k.tripCurrent - 21.304) < 1e-3 && !strcmp(k.ack, "g1-3"));
  CHECK(parseSocketPacket(a, strlen(a), k) && !k.lockout && !k.hasTripCurrent);
  const char *lb = "A1B2C3D4E5F6,SOCKET CURRENT:0,SOCKET RELAY:0,ALERT:LOCKOUT,TRIP CURRENT:x";
  CHECK(parseSocketPacket(lb, strlen(lb), k) && k.lockout && !k.hasTripCurrent);
  CHECK(buildResetLockoutCommand("A1B2C3D4E5F6", "g1-4", buf, sizeof buf) && !strcmp(buf, "A1B2C3D4E5F6,RESET_LOCKOUT,g1-4"));
  CHECK(!buildResetLockoutCommand("A1B2C3D4E5F6", "bad id", buf, sizeof buf));
}

static void test_trip_lockout() {
  TripLockout t;
  CHECK(!t.onTrip(1000, 20.5f) && !t.onTrip(60000, 21.0f) && !t.latched());
  CHECK(t.onTrip(200000, 22.0f) && t.latched() && fabsf(t.lastTripAmps() - 22.0f) < 1e-6);
  CHECK(!t.onTrip(210000, 23.0f) && t.latched());  // already latched
  t.reset(); CHECK(!t.latched() && t.recentTrips(210000) == 0);
  // rolling window: 3 trips spread over > 5 min do not latch, the next close one does
  CHECK(!t.onTrip(0, 20) && !t.onTrip(200000, 20) && !t.onTrip(301000, 20) && !t.latched());
  CHECK(t.onTrip(400000, 20) && t.latched());  // 200000, 301000, 400000 within 5 min
  t.reset();
  CHECK(!t.onTrip(0xFFFFF000u, 20) && !t.onTrip(0x00001000u, 20) && t.onTrip(0x00002000u, 20));  // millis wrap
  TripLockout r; r.restore(true, 20.7f); CHECK(r.latched() && fabsf(r.lastTripAmps() - 20.7f) < 1e-6);
}

static void test_lockout_event() {
  using namespace ev;
  Engine s; Inputs si; si.nSockets = 1; strcpy(si.sockets[0].id, "A1B2C3D4E5F6"); si.sockets[0].online = true;
  Event e[24];
  si.sockets[0].lockout = true; si.sockets[0].lockoutA = 21.3f; s.update(si, 0, 100); size_t n = s.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::ApplianceLockout && e[0].active && e[0].severity == Sev::Critical && e[0].hasValue &&
        fabsf(e[0].value - 21.3f) < 1e-4 && !strcmp(e[0].code, "socket_lockout_3trips") && !strcmp(e[0].socket, "A1B2C3D4E5F6") &&
        !strcmp(e[0].source, "socket") && e[0].permanent && !strcmp(typeName(e[0].type), "appliance_lockout"));
  si.sockets[0].online = false; si.sockets[0].lockout = false; s.update(si, 1000, 101); CHECK(s.drain(e, 24) == 0);  // offline: held
  si.sockets[0].online = true; s.update(si, 2000, 102); n = s.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::ApplianceLockout && !e[0].active && fabsf(e[0].value - 21.3f) < 1e-4);
}


#include "soc_guard.h"
static void test_soc_guard() {
  using namespace pc::soc;
  CHECK(!shed(Priority::Heavy, false, true, 41) && shed(Priority::Heavy, false, true, 40));
  CHECK(shed(Priority::Heavy, true, true, 49) && !shed(Priority::Heavy, true, true, 50));
  CHECK(!shed(Priority::Essential, false, true, 21) && shed(Priority::Essential, false, true, 20));
  CHECK(shed(Priority::Essential, true, true, 29) && !shed(Priority::Essential, true, true, 30));
  CHECK(shed(Priority::Heavy, true, false, 90) && !shed(Priority::Heavy, false, false, 0));  // invalid: hold
  CriticalGuard g; GuardIn in; in.socValid = true; in.soc = 11; in.bmsOnline = true;
  CHECK(g.update(in, 0) == Change::None && !g.latched());
  in.soc = 10; CHECK(g.update(in, 1000) == Change::Latched && g.latched());
  CHECK(kRestoreSoc == 15); in.soc = kRestoreSoc; in.bmsCurrentA = 1.5f; CHECK(g.update(in, 2000) == Change::None);
  CHECK(g.update(in, 61000) == Change::None);
  in.bmsCurrentA = 0.5f; CHECK(g.update(in, 61500) == Change::None);  // dip resets the hold
  in.bmsCurrentA = 2; CHECK(g.update(in, 62000) == Change::None); CHECK(g.update(in, 121999) == Change::None);
  CHECK(g.update(in, 122000) == Change::Released && !g.latched());
  // below the restore level charging is not enough
  CriticalGuard h; h.restore(true); in.soc = kRestoreSoc - 1; h.update(in, 0); CHECK(h.update(in, 100000) == Change::None && h.latched());
  // BMS offline: STM32 charge state counts
  CriticalGuard k; k.restore(true); GuardIn j; j.socValid = true; j.soc = 30; j.stmCharging = true;
  k.update(j, 0); CHECK(k.update(j, 60000) == Change::Released);
  // 2.5.3: stale NVS latch + healthy idle BMS (99 %, 0 A) releases after 60 s
  CriticalGuard m; m.restore(true); GuardIn b; b.socValid = true; b.soc = 99; b.bmsOnline = true; b.bmsSocValid = true; b.bmsCurrentA = 0;
  CHECK(m.update(b, 5000) == Change::None); CHECK(m.update(b, 64999) == Change::None && m.latched());
  CHECK(m.update(b, 65000) == Change::Released && !m.latched());
  // no oscillation: stays released at 99 %, relatches only at <= 10 %
  CHECK(m.update(b, 70000) == Change::None && !m.latched());
  b.soc = 12; CHECK(m.update(b, 71000) == Change::None && !m.latched());
  b.soc = 10; CHECK(m.update(b, 72000) == Change::Latched);
  b.soc = 14; CHECK(m.update(b, 200000) == Change::None && m.latched());
  // BMS frame invalid mid-hold restarts the hold
  CriticalGuard r; r.restore(true); b.soc = 50; r.update(b, 0); b.bmsSocValid = false; r.update(b, 30000);
  b.bmsSocValid = true; CHECK(r.update(b, 31000) == Change::None); CHECK(r.update(b, 90999) == Change::None); CHECK(r.update(b, 91000) == Change::Released);
  // SoC 12 % idle on BMS: stays latched
  CriticalGuard n; n.restore(true); b.soc = 12; n.update(b, 0); CHECK(n.update(b, 600000) == Change::None && n.latched());
  // no BMS (STM32 voltage SoC 90 %, idle): stays latched; charging releases
  CriticalGuard q; q.restore(true); GuardIn v; v.socValid = true; v.soc = 90;
  q.update(v, 0); CHECK(q.update(v, 600000) == Change::None && q.latched());
  v.bmsOnline = true; q.update(v, 600001); CHECK(q.update(v, 700000) == Change::None && q.latched());  // BMS online but soc not from BMS
  v.bmsOnline = false; v.stmCharging = true; q.update(v, 700001); CHECK(q.update(v, 760001) == Change::Released);
  { // 2.5.5 power-on gate
    OnGateIn o; o.bmsSocValid = true; o.bmsSoc = 11; o.stmReserve = true; o.espLatched = true; o.vbatValid = true; o.vbat = 49;
    CHECK(onGate(o) == OnGate::RefuseSoc);   // 2.5.13: latched, restart blocked until 15 %
    o.bmsSoc = 14.9f; CHECK(onGate(o) == OnGate::RefuseSoc);
    o.bmsSoc = 15; CHECK(onGate(o) == OnGate::Allow);
    o.espLatched = false; o.bmsSoc = 11; CHECK(onGate(o) == OnGate::Allow);  // not latched: > 10 %
    o.espLatched = true;
    o.bmsSoc = 10; CHECK(onGate(o) == OnGate::RefuseSoc);
    o.bmsSoc = 5; o.vbat = 53; CHECK(onGate(o) == OnGate::RefuseSoc);
    o.bmsSoc = 50; o.vbat = 47; CHECK(onGate(o) == OnGate::Allow);
    OnGateIn v; v.vbatValid = true; v.vbat = 48.0f; v.stmReserve = true; CHECK(onGate(v) == OnGate::Allow);
    v.vbat = 47.9f; CHECK(onGate(v) == OnGate::RefuseVoltage);
    OnGateIn n; CHECK(onGate(n) == OnGate::Allow);
    n.espLatched = true; CHECK(onGate(n) == OnGate::RefuseLatched);
    n.espLatched = false; n.stmReserve = true; CHECK(onGate(n) == OnGate::RefuseLatched);
  }
}
static void test_rsv_parse() {
  LegacyTextParser p; Stm32Status out; int pub = 0;
  feedStr(p, "--- Status ---\nSYS : GRID\nBAT : 47.40V  3%\nGRID: 231.0V\nRSV:1\nCHR : BULK\n--- Status ---\n", out, pub);
  CHECK(out.has_rsv && out.rsv_latched && out.sys == SysState::Grid && out.chr == ChargeState::Bulk && out.fault == 0);
  feedStr(p, "SYS : RESERVE\nGRID: 0.0V\nRSV: 1\n--- Status ---\n", out, pub);
  CHECK(out.sys == SysState::Reserve && out.rsv_latched && !strcmp(sysStateName(out.sys), "RESERVE") && out.fault == 0);
  feedStr(p, "SYS : INVERTER\nGRID: 0.0V\nRSV:0\n--- Status ---\n", out, pub);
  CHECK(out.has_rsv && !out.rsv_latched);
  feedStr(p, "SYS : INVERTER\n--- Status ---\n", out, pub);
  CHECK(!out.has_rsv && !out.rsv_latched);
  Stm32Status f; f.sys = SysState::Grid; f.fault = 1u << 20; f.has_ext = true; uint8_t buf[64];
  size_t n = pc::frame::encodeStatus(f, 0, buf, sizeof buf); Stm32Status g; CHECK(n && pc::frame::decodeStatus(buf, n, g) && g.rsv_latched && (g.fault & 0xFF) == 0);
  Stm32Status f5; f5.sys = SysState::Reserve; f5.has_ext = true; n = pc::frame::encodeStatus(f5, 0, buf, sizeof buf);
  CHECK(n && pc::frame::decodeStatus(buf, n, g) && g.sys == SysState::Reserve && g.rsv_latched && g.fault == 0);
}
static void test_critical_events() {
  using namespace ev;
  Engine s; Inputs in; in.stmOnline = true; in.socValid = true; in.soc = 9; Event e[24];
  in.critLatched = true; s.update(in, 0, 100); size_t n = s.drain(e, 24); bool found = false;
  for (size_t i = 0; i < n; ++i) if (e[i].type == Type::BatteryCritical) {
    found = true; CHECK(e[i].active && e[i].severity == Sev::Critical && !strcmp(e[i].source, "esp") && !strcmp(e[i].code, "soc_10pct") &&
                        !strcmp(e[i].releaseRequires, "charging") && !strcmp(typeName(e[i].type), "battery_critical")); }
  CHECK(found);
  in.stmReserve = true; s.update(in, 1000, 101); n = s.drain(e, 24); found = false;
  for (size_t i = 0; i < n; ++i) if (!strcmp(e[i].code, "stm32_reserve_latch")) {
    found = true; CHECK(e[i].active && !strcmp(e[i].source, "stm32") && !strcmp(e[i].releaseRequires, "charge_to_52v")); }
  CHECK(found);
  // ESP restore met while the STM32 is still latched: no output_restored yet
  in.soc = 30; in.critLatched = false; s.update(in, 2000, 102); n = s.drain(e, 24);
  for (size_t i = 0; i < n; ++i) CHECK(e[i].type != Type::OutputRestored);
  { int c = 0; for (size_t i = 0; i < n; ++i) if (e[i].type == Type::BatteryCritical && !e[i].active && !strcmp(e[i].source, "esp")) ++c; CHECK(c == 1); }
  in.stmReserve = false; s.update(in, 3000, 103); n = s.drain(e, 24);
  int restored = 0, cleared = 1;
  for (size_t i = 0; i < n; ++i) { if (e[i].type == Type::OutputRestored) { ++restored; CHECK(e[i].active && e[i].severity == Sev::Info);
      if (!strcmp(e[i].source, "esp")) CHECK(!strcmp(e[i].code, "soc15_charging")); }
    if (e[i].type == Type::BatteryCritical && !e[i].active) ++cleared; }
  CHECK(restored == 1 && cleared == 2);
  for (size_t i = 0; i < n; ++i) if (e[i].type == Type::OutputRestored) CHECK(!strcmp(e[i].code, "stm32_reserve_released"));
  // ESP-only latch: restored right away with soc15_charging
  in.critLatched = true; s.update(in, 4000, 104); s.drain(e, 24);
  in.critLatched = false; s.update(in, 5000, 105); n = s.drain(e, 24); restored = 0;
  for (size_t i = 0; i < n; ++i) if (e[i].type == Type::OutputRestored) { ++restored; CHECK(!strcmp(e[i].code, "soc15_charging") && !strcmp(e[i].source, "esp")); }
  CHECK(restored == 1);
}


#include "time_policy.h"
static void test_time_policy() {
  using namespace pc::clk;
  const int64_t T = 1792000000;
  CHECK(judge(T, 0, Source::None, false) == Verdict::Accept);
  CHECK(judge(T, T - 5000, Source::Nvs, false) == Verdict::Accept);
  CHECK(judge(T, T - 30, Source::Nvs, false) == Verdict::Accept);    // NVS restore counts as unsynced
  CHECK(judge(T, T - 60, Source::Phone, false) == Verdict::InSync);
  CHECK(judge(T, T + 61, Source::Phone, false) == Verdict::Accept);
  CHECK(judge(T, T - 3600, Source::Ntp, true) == Verdict::NtpAuthoritative);
  CHECK(judge(T, T - 3600, Source::Ntp, false) == Verdict::Accept);  // NTP stale (offline > 2 h)
  CHECK(judge(T, T - 10, Source::Ntp, false) == Verdict::InSync);
  CHECK(judge(1600000000, 0, Source::None, false) == Verdict::Invalid);
  CHECK(judge(T, T - 30, Source::Rtc, false) == Verdict::InSync);   // coherent RTC is a clock, not a signing source
  CHECK(judge(T, T + 61, Source::Rtc, false) == Verdict::Accept);
  CHECK(validTz(60) && validTz(-720) && validTz(840) && !validTz(841) && kTzDefault == 60);
  CHECK(!strcmp(verdictName(Verdict::NtpAuthoritative), "ntp_authoritative"));
  CHECK(!strcmp(sourceName(Source::Rtc), "rtc"));
}

#include "ds3231_time.h"
#include "lagos_time.h"
#include <string.h>
static uint8_t rtc_bcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
static void put_rtc(uint8_t frame[16], int y, int mo, int d, int h, int mi, int s, uint8_t hours_extra,
                     uint8_t month_extra, uint8_t status) {
  memset(frame, 0, 16);
  frame[0] = rtc_bcd(s);
  frame[1] = rtc_bcd(mi);
  frame[2] = static_cast<uint8_t>(rtc_bcd(h) | hours_extra);
  frame[3] = 1;
  frame[4] = rtc_bcd(d);
  frame[5] = static_cast<uint8_t>(rtc_bcd(mo) | month_extra);
  frame[6] = rtc_bcd(y % 100);
  frame[0x0E] = powercell::kDs3231ControlRun;
  frame[0x0F] = status;
}
#include "battery_alert_policy.h"
#include "ac_energy_today.h"
#include "stm_energy.h"
#include "jbd_bms.h"
#include "grid_sense.h"
#include "dsp_thermal.h"
#include "pzem_stats.h"
#include "pzem_route.h"
#include "dsp_status_led.h"
#include "hw_output.h"
#include "config.h"
#include "pins.h"
static powercell::BatteryReading soc_at(float percent, uint32_t at, bool bms_own = true) {
  return {true, percent, 51.2f, at, false, bms_own};
}
#include "sd_log.h"
#include "serial_spool.h"
#include "ac_energy_log.h"
#include "byte_store.h"
#include "ac_output_settings.h"
#include "ac_output_policy.h"
static void test_factory_rating() {
  using namespace powercell;
  SystemRating five(5000, "pc-dsp011-5kw");
  CHECK(five.armed() && five.usableW() == 4500 && !strcmp(five.state().variant, "pc-dsp011-5kw"));
  five.observeStore(true, 3500);
  CHECK(!five.armed() && !strcmp(five.reason(), "rating_mismatch"));
  SystemRating mid(3500, "pc-dsp011-35kw");
  CHECK(mid.armed() && mid.usableW() == 3150);
  mid.observeStore(false, 5000);
  CHECK(mid.armed());
  SystemRating none(0, "unrated");
  CHECK(!none.armed() && none.usableW() == 0);
  none.observeStore(true, 5000);
  CHECK(!none.armed());

  AcOutputSettings ceiling;
  auto too_high = ceiling.set(SettingsAuth::Ok, 1, true, 4501, true, 1, 4500, 0, true);
  CHECK(!too_high.applied && too_high.reason == SettingsReason::CeilingAboveUsable);
  auto ok = ceiling.set(SettingsAuth::Ok, 1, true, 4000, true, 1, 4500, 0, true);
  CHECK(ok.applied && ok.readback.ceiling_w == 4000);
  auto unset = ceiling.set(SettingsAuth::Ok, 1, true, 100, true, 1, 0, ok.readback.applied_revision, true);
  CHECK(!unset.applied && unset.reason == SettingsReason::UsableUnset);

  AcOutputPolicy policy;
  policy.setUsable(4500);
  AcWatts sample;
  sample.valid = true;
  sample.watts = 4500;
  sample.receivedAtMs = 10000;
  policy.update(sample, 10000);
  sample.receivedAtMs = 14000;
  policy.update(sample, 14000);
  CHECK(!policy.shutdownLatched());
  sample.watts = 4501;
  sample.receivedAtMs = 14000;
  policy.update(sample, 14000);
  sample.receivedAtMs = 22000;
  policy.update(sample, 22000);
  CHECK(policy.shutdownLatched() && !policy.turnsOutputOn());
}

static void test_sd_energy() {
  using namespace powercell;
  CHECK(profile_sd_cs(HardwareProfile::V011) == 5);
  CHECK(profile_sd_cs(HardwareProfile::Original) == 14);
  CHECK(profile_sd_cs(HardwareProfile::V011) != 25 && profile_sd_cs(HardwareProfile::V011) != 26);
  CHECK(!diag_kind_allowed(HardwareProfile::V011, KindDiagStm32));
  CHECK(diag_kind_allowed(HardwareProfile::Original, KindDiagStm32));

  FakeCard card(8);
  SdLog log(card, true);
  CHECK(log.recover());
  SlotRecord rec;
  rec.kind = KindDiagConsole;
  memcpy(rec.payload, "hello", 5);
  rec.len = 5;
  CHECK(log.append(rec) == SdStatus::Ok);
  CHECK(log.flush());
  SlotRecord got;
  CHECK(log.read(1, &got) && memcmp(got.payload, "hello", 5) == 0);

  console_spool().ingest("SET api_key supersecret\n", 24);
  char recent[200];
  CHECK(console_spool().export_recent(recent, sizeof recent) > 0);
  CHECK(strstr(recent, "supersecret") == nullptr);

  MemoryByteStore store(16384);
  MetaMirror mirror(store, 4096);
  SlotLogConfig cfg;
  cfg.sector_size = 4096;
  cfg.allow_overwrite = false;
  SlotLog slots(store, 8192, 8192, cfg);
  CHECK(slots.recover());
  EnergyLog energy(slots, mirror, EnergyConfig());
  energy.load();
  MeterSample sample;
  sample.valid = true;
  sample.lifetime_wh = 1000;
  sample.mono_ms = 1000;
  sample.mono_continuous = true;
  sample.unix_s = 1700000100;
  sample.clock = static_cast<uint8_t>(ClockQuality::RtcUnanchored);
  sample.margin_s = 180;
  CHECK(energy.observe(sample) == EnergyStatus::Baseline);
  CHECK(!energy.requests_meter_reset());
  sample.lifetime_wh = 1004;
  sample.mono_ms = 1000 + 900000;
  sample.unix_s += 900;
  CHECK(energy.observe(sample) != EnergyStatus::Missing);
  CHECK(!energy.requests_meter_reset());
}

static void test_battery_cutoff() {
  using P = powercell::BatteryAlertPolicy;
  CHECK(!P::kPhysicalAutoOffEnabled);
  CHECK(!powercell::battery_cutoff_writes_bms_fets() && !powercell::battery_cutoff_stops_charging());
  CHECK(!powercell::battery_recovery_drives_on());
  CHECK(powercell::battery_cutoff_pin_level(powercell::HardwareProfile::V011, true) == 0);
  CHECK(powercell::battery_cutoff_pin_level(powercell::HardwareProfile::Original, true) == -1);
  CHECK(!powercell::battery_ac_output_auto_off(powercell::HardwareProfile::Original));
  CHECK(powercell::battery_ac_output_auto_off(powercell::HardwareProfile::V011));

  P open;
  open.update(soc_at(10.1f, 1), 1);
  CHECK(!open.outputLatched() && open.outputOnAllowed());
  CHECK(open.update(soc_at(10, 2), 2) & P::BatteryChanged);
  CHECK(open.outputLatched() && !open.outputOnAllowed());
  CHECK(open.latchDirty());
  open.ackLatchSave(true);
  CHECK(open.latchDurable() && !open.latchDirty());

  P stale(0);
  powercell::BatteryReading old = soc_at(5, 0);
  CHECK(stale.update(old, 16000) == P::AvailabilityChanged);
  CHECK(!stale.outputLatched());
  stale.update(soc_at(8, 17000), 17000);
  CHECK(stale.outputLatched());
  powercell::BatteryReading held = soc_at(80, 18000);
  stale.update(held, 18000 + P::FreshnessMs + 1);
  CHECK(stale.outputLatched() && !stale.outputOnAllowed());

  P recover;
  recover.update(soc_at(9, 1000), 1000);
  const uint32_t start = 5000;
  recover.update(soc_at(100, start), start);
  CHECK(recover.outputLatched() && !recover.outputOnAllowed());
  recover.update(soc_at(100, start + P::RecoveryHoldMs), start + P::RecoveryHoldMs);
  CHECK(!recover.outputLatched() && recover.outputOnAllowed());

  P other;
  other.update(soc_at(9, 1000), 1000);
  const uint32_t done = 2000 + P::RecoveryHoldMs;
  other.update(soc_at(100, 2000, false), 2000);
  other.update(soc_at(100, done, false), done);
  CHECK(other.outputLatched() && !other.outputOnAllowed());

  P persisted(50);
  persisted.loadLatch(true, true);
  CHECK(persisted.outputLatched() && !persisted.outputOnAllowed());
  CHECK(persisted.update(soc_at(12, 60), 60) == 0 || persisted.outputLatched());

  P failed(50);
  failed.loadLatch(false, false);
  CHECK(!failed.outputOnAllowed() && !failed.outputLatched());
  failed.update(soc_at(80, 100), 100);
  CHECK(!failed.outputOnAllowed());
  failed.update(soc_at(80, 100 + P::RecoveryHoldMs), 100 + P::RecoveryHoldMs);
  CHECK(failed.outputOnAllowed() && !failed.outputLatched());
  // A failed NVS read clears only in RAM today. The clear must be saved, or the
  // next boot (and the websocket session that follows it) refuses ON again.
  CHECK(failed.latchDirty());

  // Reconnect: restored latch, then stale/invalid samples, then idle 96% for 60s.
  // Invalid and stale SoC must not count as 0% and must not count as a pass.
  P again(10);
  again.loadLatch(true, true);
  powercell::BatteryReading junk = {false, 0, 0, 0, false, false};
  again.update(junk, 1000);
  powercell::BatteryReading stale96 = soc_at(96, 1000);
  again.update(stale96, 1000 + P::FreshnessMs + 1);
  CHECK(again.outputLatched() && !again.outputOnAllowed());
  const uint32_t back = 5000;
  again.update(soc_at(96, back), back);
  CHECK(again.outputLatched() && !again.outputOnAllowed());
  again.update(soc_at(96, back + P::RecoveryHoldMs - 1), back + P::RecoveryHoldMs - 1);
  CHECK(again.outputLatched());
  again.update(soc_at(96, back + P::RecoveryHoldMs), back + P::RecoveryHoldMs);
  CHECK(!again.outputLatched() && again.outputOnAllowed());
  CHECK(!powercell::battery_recovery_drives_on());
  CHECK(again.latchDirty());

  using S = P::LatchStore;
  CHECK(P::classifyLatchStore(false, true, false, false) == S::Absent);
  CHECK(P::classifyLatchStore(false, false, false, false) == S::Unreadable);
  CHECK(P::classifyLatchStore(true, false, false, false) == S::Clear);
  CHECK(P::classifyLatchStore(true, false, true, false) == S::Clear);
  CHECK(P::classifyLatchStore(true, false, true, true) == S::Held);
  P absent;
  absent.applyLatchStore(S::Absent);
  CHECK(absent.outputOnAllowed() && !absent.outputLatched());
  absent.update(soc_at(96, 1000), 1000);
  CHECK(absent.outputOnAllowed() && !absent.outputLatched());
  CHECK(!absent.latchDirty());
  P saved;
  saved.applyLatchStore(S::Held);
  CHECK(saved.outputLatched() && !saved.outputOnAllowed());
  saved.update(soc_at(96, 2000), 2000);
  saved.update(soc_at(96, 2000 + P::RecoveryHoldMs), 2000 + P::RecoveryHoldMs);
  CHECK(!saved.outputLatched() && saved.outputOnAllowed() && saved.latchDirty());
}

static void test_ac_energy_today() {
  using namespace powercell;
  AcEnergyToday t;
  TodayInput in;
  in.valid = false;
  in.lifetime_wh = 0;
  in.clock = TodayClock::Anchored;
  in.day_id = 100;
  TodayOutput o = t.observe(in);
  CHECK(!o.known && !o.persist && !t.known());

  in.valid = true;
  in.lifetime_wh = 5000;
  in.clock = TodayClock::Untrusted;
  in.day_id = 0;
  in.mono_ms = 1000;
  in.mono_continuous = true;
  o = t.observe(in);
  CHECK(!o.known && !o.persist && !t.baselineValid());

  // Offline, no trusted clock: first valid PZEM sample starts a session baseline.
  in.clock = TodayClock::Session;
  in.day_id = kSessionDayId;
  o = t.observe(in);
  CHECK(o.known && o.persist && o.kwh == 0);
  CHECK(t.baselineWh() == 5000 && t.dayId() == kSessionDayId && t.known());

  in.lifetime_wh = 6350;  // 1.35 kWh since the first sample (Caleb's 1350 Wh case)
  in.mono_ms = 1000 + 3600 * 1000;
  o = t.observe(in);
  CHECK(o.known && !o.persist);
  NEAR(o.kwh, 1.35, 1e-9);

  // A trusted clock later in the same run adopts the session; energy stays.
  in.clock = TodayClock::Anchored;
  in.day_id = 100;
  in.lifetime_wh = 6350;
  in.mono_ms = 1000 + 3600 * 1000 + 1000;
  o = t.observe(in);
  CHECK(o.known && o.persist);
  NEAR(o.kwh, 1.35, 1e-9);
  CHECK(t.baselineWh() == 5000 && t.dayId() == 100);

  AcEnergyToday fresh;
  in.clock = TodayClock::Anchored;
  in.day_id = 100;
  in.lifetime_wh = 5000;
  in.mono_ms = 1000;
  o = fresh.observe(in);
  CHECK(o.known && o.persist && o.kwh == 0);
  CHECK(fresh.baselineWh() == 5000 && fresh.dayId() == 100);
  t = fresh;

  in.lifetime_wh = 6500;
  in.mono_ms = 1000 + 3600 * 1000;
  o = t.observe(in);
  CHECK(o.known && !o.persist);
  NEAR(o.kwh, 1.5, 1e-9);

  in.valid = false;
  o = t.observe(in);
  CHECK(!o.known && !o.persist);
  CHECK(t.baselineWh() == 5000 && t.dayId() == 100);

  in.valid = true;
  in.lifetime_wh = 6500;
  o = t.observe(in);
  CHECK(o.known);
  NEAR(o.kwh, 1.5, 1e-9);

  AcEnergyToday booted;
  booted.restore(true, 100, 5000, false);
  in.lifetime_wh = 8000;
  in.mono_continuous = false;
  o = booted.observe(in);
  CHECK(o.known && !o.persist);
  NEAR(o.kwh, 3.0, 1e-9);

  in.mono_continuous = true;
  in.mono_ms = 5000;
  in.lifetime_wh = 8000;
  booted.observe(in);
  in.mono_ms = 6000;
  in.lifetime_wh = 8000 + 100;
  o = booted.observe(in);
  CHECK(!o.known && o.persist && booted.dayUnknown());

  in.lifetime_wh = 8100;
  in.mono_ms = 7000;
  o = booted.observe(in);
  CHECK(!o.known && !o.persist);

  in.clock = TodayClock::Rtc;
  in.day_id = 101;
  in.lifetime_wh = 9000;
  in.mono_ms = 8000;
  o = booted.observe(in);
  CHECK(o.known && o.persist && o.kwh == 0);
  CHECK(!booted.dayUnknown() && booted.baselineWh() == 9000 && booted.dayId() == 101);

  in.day_id = 0;
  in.clock = TodayClock::Untrusted;
  in.lifetime_wh = 9500;
  o = booted.observe(in);
  CHECK(o.known && !o.persist);  // keep the established day if the clock drops
  NEAR(o.kwh, 0.0, 1e-9);
  CHECK(booted.dayId() == 101 && booted.baselineWh() == 9000);

  in.clock = TodayClock::Session;
  in.day_id = kSessionDayId;
  in.lifetime_wh = 9500;
  in.mono_ms = 8000 + 3600 * 1000;  // 500 Wh over an hour is a plausible step
  o = booted.observe(in);
  CHECK(o.known);
  NEAR(o.kwh, 0.5, 1e-9);
  CHECK(booted.dayId() == 101 && booted.baselineWh() == 9000);
}

static void test_v011_gpio() {
  using namespace powercell;
  CHECK(DSP_PWR_ACTIVE_LEVEL == 1);
  CHECK(v011_dsp_level(true) == 1 && v011_dsp_level(false) == 0);
  CHECK(original_relay_level(true) == 1 && original_relay_level(false) == 0);
  CHECK(!kPhysicalShutdownVerified && kDspPolarityUserConfirmed);
  CHECK(!battery_recovery_drives_on());
  BootStep steps[2];
  v011_dsp_boot(steps);
  CHECK(steps[0].action == BootAction::SetOutputLatch && steps[0].level == 0);
  CHECK(steps[1].action == BootAction::EnableOutput && steps[1].level == 0);
  CHECK(battery_cutoff_pin_level(HardwareProfile::V011, true) == 0);
  CHECK(battery_cutoff_pin_level(HardwareProfile::V011, false) == -1);
  CHECK(battery_cutoff_pin_level(HardwareProfile::Original, true) == -1);
  CHECK(commanded_pin_level(HardwareProfile::Original, true) == 1);
  CHECK(commanded_pin_level(HardwareProfile::Original, false) == 0);
  CHECK(commanded_pin_level(HardwareProfile::V011, true) == 1);
  const OffOrigin offs[] = {OffOrigin::BootLatch, OffOrigin::OtaSafe, OffOrigin::BatteryCutoff,
                            OffOrigin::Overload, OffOrigin::PaymentLock, OffOrigin::RemoteOff,
                            OffOrigin::FactoryReset};
  for (OffOrigin origin : offs) {
    CHECK(off_origin_level(HardwareProfile::V011, origin) == 0);
    CHECK(off_origin_level(HardwareProfile::Original, origin) == 0);
  }
}

// Model of one ESP32 pad: IO_MUX function, hold latch, GPIO-matrix level and enable.
struct PadSim {
  bool gpio_func = false;  // GPIO15 resets into MTDO (JTAG)
  bool held = false;
  bool out_en = false;
  int reg = 0;
  int pad = -1;  // -1 = not driven by the GPIO matrix (floating / JTAG)
  void settle() {
    if (held) return;  // frozen
    pad = (gpio_func && out_en) ? reg : -1;
  }
  void apply(const powercell::PadStep& st) {
    using powercell::PadOp;
    switch (st.op) {
      case PadOp::HoldRelease: held = false; break;
      case PadOp::SelectGpio: gpio_func = true; break;
      case PadOp::PullsOff: break;
      case PadOp::SetLevel: reg = st.level; break;
      case PadOp::OutputEnable: out_en = true; break;
      case PadOp::HoldEnable: settle(); held = true; return;
    }
    settle();
  }
  void run(bool on) {
    powercell::PadStep steps[powercell::kDspPadSteps];
    const size_t n = powercell::v011_dsp_pad_steps(on, steps);
    for (size_t i = 0; i < n; ++i) apply(steps[i]);
  }
};

static void test_dsp_pad_steps() {
  using namespace powercell;
  // The cbf48bd sequence: level + direction + hold, no IO_MUX select. The pad never follows.
  PadSim old;
  old.apply({PadOp::SetLevel, 1});
  old.apply({PadOp::OutputEnable, -1});
  CHECK(old.pad == -1);
  // Fixed sequence: boot latch OFF, then ON, then OFF, then ON again.
  PadSim p;
  p.run(false);
  CHECK(p.gpio_func && p.pad == 0 && p.held);
  p.run(true);
  CHECK(p.pad == 1 && !p.held);  // the hold is released, so ON reaches the pad
  p.run(false);
  CHECK(p.pad == 0 && p.held);
  p.run(true);
  CHECK(p.pad == 1);
  // SelectGpio comes before the driver is enabled, and the level is set first (no high glitch on off).
  PadStep st[kDspPadSteps];
  size_t n = v011_dsp_pad_steps(false, st);
  CHECK(n == 7 && st[0].op == PadOp::HoldRelease && st[1].op == PadOp::SetLevel && st[1].level == 0);
  size_t sel = 99, en = 99;
  for (size_t i = 0; i < n; ++i) { if (st[i].op == PadOp::SelectGpio) sel = i; if (st[i].op == PadOp::OutputEnable) en = i; }
  CHECK(sel < en && st[n - 1].op == PadOp::HoldEnable);
  n = v011_dsp_pad_steps(true, st);
  CHECK(n == 6 && st[n - 1].op == PadOp::SetLevel && st[n - 1].level == 1);
  for (size_t i = 0; i < n; ++i) CHECK(st[i].op != PadOp::HoldEnable);
}

static void test_pzem_route() {
  using namespace powercell;
  PzemRouteRegs ok;
  ok.driver = true;
  ok.rx_in_sel = (1u << 7) | 35;   // U1RXD from the matrix, GPIO35
  ok.tx_out_sel = 17 | (1u << 10); // U1TXD on GPIO32 (OEN bit ignored)
  ok.rx_iomux = (2u << 12) | (1u << 9);
  ok.tx_iomux = (2u << 12);
  CHECK(pzem_route_check(ok, 35) == kRouteOk);
  char n[96];
  CHECK(strcmp(pzem_route_names(0, n, sizeof n), "ok") == 0);
  PzemRouteRegs r = ok; r.driver = false;
  CHECK(pzem_route_check(r, 35) == kRouteNoDriver);
  r = ok; r.rx_in_sel = 0x30;      // reset value: constant low, not the matrix
  CHECK(pzem_route_check(r, 35) == kRouteRxNotMatrix);
  r = ok; r.rx_in_sel = (1u << 7) | 32;  // RX swapped onto 32
  CHECK(pzem_route_check(r, 35) == kRouteRxWrongPin);
  r = ok; r.tx_out_sel = 256;      // plain GPIO output (pinMode / digitalWrite took GPIO32)
  CHECK(pzem_route_check(r, 35) == kRouteTxWrongSig);
  r = ok; r.tx_out_sel = 71;       // LEDC HS ch0 on GPIO32
  CHECK(pzem_route_check(r, 35) == kRouteTxWrongSig);
  r = ok; r.rx_iomux = (2u << 12); // input buffer off
  CHECK(pzem_route_check(r, 35) == kRouteRxNoIe);
  r = ok; r.rx_iomux = (0u << 12) | (1u << 9);
  CHECK(pzem_route_check(r, 35) == kRouteRxMux);
  r = ok; r.tx_iomux = (3u << 12);
  CHECK(pzem_route_check(r, 35) == kRouteTxMux);
  r = ok; r.rx_rtc = true; r.tx_rtc = true;  // ADC / RTC IO took both pads
  const uint16_t f = pzem_route_check(r, 35);
  CHECK(f == (kRouteRxRtc | kRouteTxRtc));
  CHECK(strcmp(pzem_route_names(f, n, sizeof n), "rx_rtc,tx_rtc") == 0);
  char tiny[8];
  pzem_route_names(f, tiny, sizeof tiny);
  CHECK(strlen(tiny) < sizeof tiny);
  // Request bytes as Dubem's readAll(): F8 04 00 00 00 0A + CRC low, high.
  PzemConfig cfg;
  uint8_t req[8];
  CHECK(pzem_build_read(cfg, req, sizeof req) == 8);
  const uint8_t dubem[6] = {0xF8, 0x04, 0x00, 0x00, 0x00, 0x0A};
  CHECK(memcmp(req, dubem, 6) == 0);
  const uint16_t c = pzem_crc(dubem, 6);
  CHECK(req[6] == (c & 0xFF) && req[7] == (c >> 8));
  char hex[64];
  CHECK(pzem_hex(req, 8, hex, sizeof hex) == 23);
  CHECK(strncmp(hex, "F8 04 00 00 00 0A ", 18) == 0);
  CHECK(pzem_hex(req, 8, hex, 8) < 8 && strlen(hex) < 8);
  CHECK(pzem_hex(req, 0, hex, sizeof hex) == 0 && hex[0] == 0);
}

static void test_dsp_status_led() {
  using namespace powercell;
  CHECK(kLedPwmFreqHz == 5000 && kLedPwmBits == 8 && kLedBreatheMs == 4000 && kLedBmsStaleMs == 15000);
  // Colors (Dubem lighting.cpp): >=70 green, 40..69 blue, <40 red, no/stale BMS red.
  CHECK(led_color(true, 100) == StatusColor::Green);
  CHECK(led_color(true, 70) == StatusColor::Green);
  CHECK(led_color(true, 69) == StatusColor::Blue);
  CHECK(led_color(true, 40) == StatusColor::Blue);
  CHECK(led_color(true, 39) == StatusColor::Red);
  CHECK(led_color(true, 0) == StatusColor::Red);
  CHECK(led_color(false, 100) == StatusColor::Red);
  // Breathe: 0 -> 255 -> 0 over 4 s.
  CHECK(led_breathe(0) == 0 && led_breathe(2000) == 255 && led_breathe(4000) == 0);
  CHECK(led_breathe(1000) == 127 && led_breathe(3000) == 127 && led_breathe(6000) == 255);
  // Solid without grid, breathing with grid; one LED only (color carries it).
  StatusLed a = dsp_status_led(true, 80, false, 1000);
  CHECK(a.color == StatusColor::Green && a.duty == 255);
  StatusLed b = dsp_status_led(true, 50, true, 1000);
  CHECK(b.color == StatusColor::Blue && b.duty == 127);
  StatusLed c = dsp_status_led(false, 90, true, 2000);
  CHECK(c.color == StatusColor::Red && c.duty == 255);
  StatusLed e = dsp_status_led(false, 90, false, 0);
  CHECK(e.color == StatusColor::Red && e.duty == 255);
}

static bool read_text_file(const char *path, char *buf, size_t cap);
static void test_front_light_follows() {
  using namespace powercell;
  CHECK(kDspFrontLightV011 == 27);
  CHECK(v011_front_light_level(true) == 1 && v011_front_light_level(false) == 0);  // Dubem: HIGH = ON
  CHECK(v011_front_light_level(false) == v011_dsp_level(false) && v011_front_light_level(true) == v011_dsp_level(true));  // same as GPIO15
  // Every DSP pad write (boot latch, all off paths via latchOutputOff/drive, and on) also drives the light.
  char src[16384];
  const char *paths[] = {"../../src/gateway.cpp", "../src/gateway.cpp", "src/gateway.cpp"};
  bool got = false;
  for (const char *path : paths) if (read_text_file(path, src, sizeof src)) { got = true; break; }
  CHECK(got);
  if (!got) return;
  const char *latch = strstr(src, "void latchDspEnableOff()");
  const char *drv = strstr(src, "void drive(bool on)");
  CHECK(latch && drv);
  if (!latch || !drv) return;
  const char *latchEnd = strstr(latch, "\n}");
  const char *drvEnd = strstr(drv, "\n}");
  const char *a = strstr(latch, "frontLight(false);");
  const char *b = strstr(drv, "frontLight(on);");
  CHECK(a && a < latchEnd);
  CHECK(b && b < drvEnd);
  char leds[8192];
  const char *lpaths[] = {"../../src/leds.cpp", "../src/leds.cpp", "src/leds.cpp"};
  got = false;
  for (const char *path : lpaths) if (read_text_file(path, leds, sizeof leds)) { got = true; break; }
  CHECK(got && !strstr(leds, "PIN_FRONT_LIGHT}"));  // leds no longer forces the light low
}

static void test_pzem_stats() {
  using namespace powercell;
  CHECK(pzem_classify(25, PzemStatus::Ok, true) == PzemPollOutcome::Ok);
  CHECK(pzem_classify(0, PzemStatus::Timeout, false) == PzemPollOutcome::NoReply);
  CHECK(pzem_classify(7, PzemStatus::Short, false) == PzemPollOutcome::Short);
  CHECK(pzem_classify(25, PzemStatus::BadCrc, false) == PzemPollOutcome::Bad);
  CHECK(pzem_classify(25, PzemStatus::Ok, false) == PzemPollOutcome::Bad);  // sentinel power
  PzemStats s;
  CHECK(s.record(PzemPollOutcome::NoReply, 1000) == PzemStats::Log::Warn);  // first failure logs at once
  for (uint32_t t = 2000; t < 31000; t += 1000) CHECK(s.record(PzemPollOutcome::NoReply, t) == PzemStats::Log::None);
  CHECK(s.record(PzemPollOutcome::Short, 31000) == PzemStats::Log::Warn);  // 30 s later
  CHECK(s.no_reply == 30 && s.short_frames == 1 && s.ok == 0 && s.consecutive_fail == 31 && !s.ever_ok);
  CHECK(s.record(PzemPollOutcome::Ok, 32000) == PzemStats::Log::Recovered);
  CHECK(s.record(PzemPollOutcome::Ok, 33000) == PzemStats::Log::None);
  CHECK(s.ok == 2 && s.consecutive_fail == 0 && s.ever_ok && s.last_ok_ms == 33000 && s.polls == 33);
  CHECK(s.record(PzemPollOutcome::Bad, 34000) == PzemStats::Log::Warn);  // new episode logs at once
  CHECK(s.bad_frames == 1);
  CHECK(!strcmp(pzem_outcome_name(PzemPollOutcome::NoReply), "no_reply"));
}

static void test_ds3231() {
  using namespace powercell;
  uint8_t a[16], b[16];
  put_rtc(a, 2024, 2, 29, 12, 0, 0, 0, 0, 0);
  put_rtc(b, 2024, 2, 29, 12, 0, 1, 0, 0, 0);
  RtcReading ok = ds3231_decode_pair(a, b);
  CHECK(ok.status == RtcStatus::Unanchored && !ok.oscillator_stopped && ok.battery_oscillator_enabled);
  CHECK(ok.unix_utc == unix_from_utc(2024, 2, 29, 12, 0, 1));
  a[0x0F] = 0x80;
  CHECK(ds3231_decode_pair(a, b).status == RtcStatus::OscillatorStopped);
  CHECK(ds3231_decode_pair(a, b).unix_utc == 0);
  put_rtc(a, 2024, 2, 29, 12, 0, 0, 0, 0, 0);
  a[0] = static_cast<uint8_t>(a[0] | 0x80);
  CHECK(ds3231_decode_pair(a, b).status == RtcStatus::BadRegister);
  put_rtc(a, 2023, 2, 29, 12, 0, 0, 0, 0, 0);
  put_rtc(b, 2023, 2, 29, 12, 0, 0, 0, 0, 0);
  CHECK(ds3231_decode_pair(a, b).status == RtcStatus::BadCalendar);
  put_rtc(a, 2024, 2, 29, 12, 0, 0, 0, 0, 0);
  put_rtc(b, 2024, 2, 29, 12, 0, 20, 0, 0, 0);
  CHECK(ds3231_decode_pair(a, b).status == RtcStatus::Incoherent);
  uint8_t encoded[7], control = 0, status = 0;
  const int64_t stamp = unix_from_utc(2026, 11, 8, 20, 45, 0);
  CHECK(ds3231_encode(stamp, encoded, &control, &status));
  CHECK(control == kDs3231ControlRun && status == kDs3231StatusClear && (encoded[2] & 0x40) == 0);
  uint8_t programmed[16];
  memset(programmed, 0, sizeof programmed);
  memcpy(programmed, encoded, 7);
  programmed[0x0E] = control;
  programmed[0x0F] = status;
  CHECK(ds3231_readback_matches(ds3231_decode(programmed), stamp));
  programmed[0x0F] = 0x80;
  CHECK(!ds3231_readback_matches(ds3231_decode(programmed), stamp));
  CHECK(!ds3231_encode(0, encoded, &control, &status));
  CHECK(qualify_boot_clock(true, false, false) == ClockQuality::Invalid);
  CHECK(qualify_boot_clock(false, true, false) == ClockQuality::RtcUnanchored);
  CHECK(qualify_boot_clock(true, true, true) == ClockQuality::Anchored);
}

#include "socket_rules.h"
#include "lora_socket_proto.h"
static void test_socket_rules() {
  using namespace pc::sock;
  uint16_t m; char b[6];
  CHECK(parseHHMM("09:00", m) && m == 540 && parseHHMM("23:59", m) && m == 1439);
  CHECK(!parseHHMM("24:00", m) && !parseHHMM("9:00", m) && !parseHHMM("09:60", m) && !parseHHMM("", m));
  fmtHHMM(1260, b); CHECK(!strcmp(b, "21:00"));
  Mode md; Prio pr;
  CHECK(parseMode("day_night", md) && md == Mode::DayNight && parseMode("ai", md) && md == Mode::Ai && !parseMode("auto", md));
  CHECK(parsePrio("essential", pr) && pr == Prio::Essential && !parsePrio("x", pr));
  uint8_t da; bool cl;
  CHECK(limitDa(8.0f, da, &cl) && da == 80 && !cl);
  CHECK(limitDa(13.0f, da, &cl) && da == 100 && cl);
  CHECK(limitDa(25.0f, da, &cl) && da == 100 && cl);
  CHECK(limitDa(0.5f, da, &cl) && da == 10 && cl);
  CHECK(!limitDa(0.0f, da, &cl) && !limitDa(-3.0f, da, &cl));
  CHECK(validShed(40, 50) && !validShed(50, 50) && !validShed(-1, 10) && !validShed(10, 101));
  CHECK(overrideMs(0, 0, false, 1800) == 1800000u);
  CHECK(overrideMs(1000 + 600, 1000, true, 1800) == 600000u);
  CHECK(overrideMs(1000 + 9000, 1000, true, 1800) == 1800000u);
  CHECK(overrideMs(900, 1000, true, 1800) == 0u);
  Sched w; w.mask = 0x7F; w.action = 0; w.start = 1320; w.end = 360;  // 22:00-06:00 wraps
  CHECK(inWindow(w, 1400, 3) && inWindow(w, 100, 3) && !inWindow(w, 400, 3) && !inWindow(w, 700, 3));
  w.mask = 1 << 2; w.start = 600; w.end = 660;
  CHECK(inWindow(w, 610, 2) && !inWindow(w, 610, 3));

  Config c; State st; In in; in.socValid = true; in.soc = 80; in.clockSynced = true; in.minutes = 600; in.wday = 2; in.nowMs = 1000;
  CHECK(sizeof(Config) <= 96 && c.limitDa == 100 && c.prio == Prio::Heavy);
  Decision d = decide(c, st, in); CHECK(d.want == -1);  // manual, no schedules
  c.nSched = 1; c.sched[0] = w; c.sched[0].action = 1;
  d = decide(c, st, in); CHECK(d.want == 1 && d.reason == Reason::Schedule);
  in.clockSynced = false; d = decide(c, st, in); CHECK(d.want == -1);  // unsynced: schedules paused
  in.clockSynced = true;
  // SoC shed with hysteresis (heavy 40/50)
  in.soc = 39; d = decide(c, st, in); CHECK(d.want == 0 && d.reason == Reason::SocHeavy && st.shed);
  in.soc = 45; d = decide(c, st, in); CHECK(d.want == 0 && st.shed);
  in.soc = 50; d = decide(c, st, in); CHECK(d.want == 1 && !st.shed);
  c.prio = Prio::Essential; in.soc = 39; d = decide(c, st, in); CHECK(d.want == 1);
  in.soc = 19; d = decide(c, st, in); CHECK(d.want == 0 && d.reason == Reason::SocEssential);
  in.soc = 80; d = decide(c, st, in); st.shed = false;
  // grid up: no SoC shedding, and an active shed ends when the grid returns
  in.gridDown = false; in.soc = 10; c.prio = Prio::Heavy; d = decide(c, st, in); CHECK(d.reason != Reason::SocHeavy && !st.shed);
  in.gridDown = true; d = decide(c, st, in); CHECK(d.want == 0 && d.reason == Reason::SocHeavy && st.shed);
  in.gridDown = false; d = decide(c, st, in); CHECK(!st.shed && d.reason != Reason::SocHeavy);
  in.latched = true; d = decide(c, st, in); CHECK(d.want == 0 && d.reason == Reason::SocHeavy); in.latched = false;  // critical latch still applies
  in.gridDown = true; in.soc = 80; c.prio = Prio::Essential; d = decide(c, st, in);
  // overload sheds heavy only
  in.overload = true; d = decide(c, st, in); CHECK(d.reason != Reason::InverterOverload);
  c.prio = Prio::Heavy; d = decide(c, st, in); CHECK(d.want == 0 && d.reason == Reason::InverterOverload);
  in.overload = false;
  // safety beats everything, latched beats override
  in.safety = true; d = decide(c, st, in); CHECK(d.want == 0 && d.reason == Reason::SafetyTrip); in.safety = false;
  // AI override applies only in ai mode, until it expires, then fallback to manual
  c.mode = c.lastUserMode = Mode::Ai;
  st.ov.set = true; st.ov.on = false; st.ov.untilMs = 5000;
  d = decide(c, st, in); CHECK(d.want == 0 && d.reason == Reason::CloudOverride && st.fb == Fallback::None && effectiveMode(c, st) == Mode::Ai);
  in.latched = true; d = decide(c, st, in); CHECK(d.want == 0 && d.reason != Reason::CloudOverride); in.latched = false;
  in.nowMs = 6000; d = decide(c, st, in);
  CHECK(!st.ov.set && st.ovExpired && st.fb == Fallback::OverrideExpired && effectiveMode(c, st) == Mode::Manual && d.reason == Reason::Schedule);
  st.ov.set = true; st.ov.untilMs = 9000; st.ovExpired = false; in.cloudUp = false;
  d = decide(c, st, in); CHECK(st.fb == Fallback::CloudOffline && !st.ov.set && d.reason != Reason::CloudOverride);
  in.cloudUp = true; d = decide(c, st, in); CHECK(st.fb == Fallback::None && effectiveMode(c, st) == Mode::Ai);
  // weather override ignored on essential sockets
  c.mode = c.lastUserMode = Mode::Weather; c.prio = Prio::Essential; st.ov.set = true; st.ov.weather = true; st.ov.on = false; st.ov.untilMs = 20000;
  d = decide(c, st, in); CHECK(d.reason != Reason::CloudOverride);
  // DayNight: heavy off between night_start and day_start
  Config n; n.mode = n.lastUserMode = Mode::DayNight; State s2;
  in.minutes = 1300; d = decide(n, s2, in); CHECK(d.want == 0 && d.reason == Reason::DayNight);
  in.minutes = 300; d = decide(n, s2, in); CHECK(d.want == 0);
  in.minutes = 600; d = decide(n, s2, in); CHECK(d.want == 1 && d.reason == Reason::DayNight);
  CHECK(!strcmp(reasonName(Reason::SocHeavy), "soc_heavy") && !strcmp(fallbackName(Fallback::CloudOffline), "cloud_offline") && !fallbackName(Fallback::None));
  // LoRa SET_LIMIT + LIM
  using namespace pc::lora;
  char buf[64]; SocketPacket k;
  CHECK(buildSetLimitCommand("A1B2C3D4E5F6", 80, "g1-9", buf, sizeof buf) && !strcmp(buf, "A1B2C3D4E5F6,SET_LIMIT,80,g1-9"));
  CHECK(buildSetLimitCommand("A1B2C3D4E5F6", 250, "g1-9", buf, sizeof buf) && !strcmp(buf, "A1B2C3D4E5F6,SET_LIMIT,100,g1-9"));
  const char *p1 = "A1B2C3D4E5F6,SOCKET CURRENT:1.0,SOCKET RELAY:1,ACK:g1-9,LIM:80";
  CHECK(parseSocketPacket(p1, strlen(p1), k) && k.hasLimit && k.limitDa == 80);
  const char *p2 = "A1B2C3D4E5F6,SOCKET CURRENT:1.0,SOCKET RELAY:1,LIM:999";
  CHECK(parseSocketPacket(p2, strlen(p2), k) && !k.hasLimit);
}

static void test_heap_guard() {
  using namespace pc;
  // 2.5.20 gate: record buffer arithmetic (mbedTLS 2.28, 16384 content, light poisoning)
  CHECK(kTlsRecordBuf == 13 + 16384 + 16 + 48 + 256);
  CHECK(kTlsRecordAlloc == 16736 && kTlsPairBlock == 35520 && kTlsMinBlock == 18432 && kTlsMinFree == 51200);
  // ACE-000001 on 2.5.18 (12 Nov 2026): largest=40948 free=72064 was refused by the old
  // 40960 rule; it holds both record buffers, so it now goes without a probe.
  CHECK(tlsHeapVerdict(40948, 72064) == TlsHeap::Ok);
  CHECK(tlsHeapVerdict(kTlsPairBlock, kTlsMinFree) == TlsHeap::Ok);
  CHECK(tlsHeapVerdict(kTlsPairBlock - 1, 80000) == TlsHeap::Probe);
  CHECK(tlsHeapVerdict(kTlsMinBlock, 80000) == TlsHeap::Probe);
  CHECK(tlsHeapVerdict(kTlsMinBlock - 1, 80000) == TlsHeap::LowBlock);
  CHECK(tlsHeapVerdict(60000, kTlsMinFree - 1) == TlsHeap::LowFree);  // v10: ~44 KB left -> -9984
  CHECK(tlsHeapVerdict(44000, 44000) == TlsHeap::LowFree);
  CHECK(!strcmp(tlsHeapName(TlsHeap::LowBlock), "low_block") && !strcmp(tlsHeapName(TlsHeap::Probe), "probe"));
  {
    int probes = 0;
    auto yes = [&] { ++probes; return true; };
    auto no = [&] { ++probes; return false; };
    TlsHeap why = TlsHeap::Ok;
    CHECK(tlsHeapOk(40948, 72064, no, &why) && why == TlsHeap::Ok && probes == 0);       // no probe needed
    CHECK(tlsHeapOk(30000, 72064, yes, &why) && why == TlsHeap::Probe && probes == 1);   // two blocks fit
    CHECK(!tlsHeapOk(30000, 72064, no, &why) && why == TlsHeap::Probe && probes == 2);   // probe failed
    CHECK(!tlsHeapOk(17000, 72064, yes, &why) && why == TlsHeap::LowBlock && probes == 2);  // never probes
    CHECK(!tlsHeapOk(60000, 50000, yes, &why) && why == TlsHeap::LowFree && probes == 2);
  }
  TlsBackoff b;
  CHECK(b.due(0) && b.step(0, true) == TlsBackoff::Go);
  CHECK(b.step(100, false) == TlsBackoff::WaitLog && b.delayMs() == 5000);
  CHECK(!b.due(200) && b.step(200, true) == TlsBackoff::Wait);  // still in backoff even if heap recovered
  CHECK(b.due(5100) && b.step(5100, false) == TlsBackoff::WaitLog && b.delayMs() == 10000);
  CHECK(b.step(15100, false) == TlsBackoff::WaitLog && b.delayMs() == 20000);
  CHECK(b.step(35100, false) == TlsBackoff::WaitLog && b.delayMs() == 40000);
  CHECK(b.step(75100, false) == TlsBackoff::WaitLog && b.delayMs() == 60000);
  CHECK(b.step(135100, false) == TlsBackoff::WaitLog && b.delayMs() == 60000);  // capped
  CHECK(b.step(195100, true) == TlsBackoff::Go && b.delayMs() == 0);
  CHECK(b.step(195200, false) == TlsBackoff::WaitLog && b.delayMs() == 5000);  // reset after success
  // mDNS waits for the first WS TLS connect, at most 60 s after the IP, only with a backend
  CHECK(pc::mdns::waitForTls(true, false, 0) && pc::mdns::waitForTls(true, false, 59999));
  CHECK(!pc::mdns::waitForTls(true, false, 60000));
  CHECK(!pc::mdns::waitForTls(true, true, 0) && !pc::mdns::waitForTls(false, false, 0));
  OtaTransport t;
  CHECK(t.support() == OtaTransport::Unknown && t.useWs(true) && !t.useWs(false));
  t.sent(1000);
  CHECK(!t.timedOut(20999) && t.timedOut(21000) && !t.timedOut(30000));
  CHECK(!t.reply());                                         // late reply after timeout ignored
  t.sent(40000); CHECK(t.reply() && t.support() == OtaTransport::Yes && !t.waiting() && t.seenYes());
  // v11: after ota_ws=1 this boot, never HTTPS: WS down or unsettled -> Wait, settled -> Ws
  OtaTransport a;
  a.wsConnected(); a.helloAck(true);
  CHECK(a.plan(true, 29999) == OtaTransport::Wait && a.plan(true, 30000) == OtaTransport::Ws);
  CHECK(a.plan(false, 0) == OtaTransport::Wait);             // bench 368883: WS down at check time
  a.sent(100); a.wsDown(); CHECK(!a.waiting());
  a.sent(200); CHECK(a.timedOut(20200) && a.plan(true, 40000) == OtaTransport::Ws);  // timeout: retry over WS
  for (int i = 0; i < 5; ++i) { a.wsConnected(); a.helloAck(false); }
  CHECK(!a.httpsAllowed() && a.plan(false, 0) == OtaTransport::Wait);  // sticky for the boot
  // never seen ota_ws=1: HTTPS only after 3 consecutive WS cycles without it
  OtaTransport u;
  CHECK(u.plan(false, 0) == OtaTransport::Wait);             // WS never up: wait, no HTTPS
  u.wsConnected(); CHECK(u.plan(true, 40000) == OtaTransport::Ws);   // unknown: try WS
  u.wsConnected(); u.helloAck(false); CHECK(!u.httpsAllowed());
  u.wsConnected(); CHECK(u.httpsAllowed() && u.plan(true, 40000) == OtaTransport::Https && !u.useWs(true));
  OtaTransport r; r.wsConnected(); r.wsConnected(); r.helloAck(true); r.wsConnected(); r.wsConnected(); r.wsConnected();
  CHECK(!r.httpsAllowed());                                  // a yes resets and sticks
}

static void test_legacy_replies() {
  LegacyTextParser p; Stm32Status out; int pub = 0;
  feedStr(p, "--- Status ---\nSYS : GRID\nVER PC-INV-2.1.0 Oct 20 2026 10:00:00\nBAT : 12.80V  55%\n", out, pub);
  CHECK(p.replySeq() == 1 && !strncmp(p.reply(), "VER PC-INV-2.1.0", 16));
  feedStr(p, "OK CAL scale=1.0100 default=1.0000 stored=1 active=1 pending=0\nERR UNKNOWN\n--- Status ---\n", out, pub);
  CHECK(pub == 1 && out.sys == SysState::Grid && p.replySeq() == 3 && !strcmp(p.reply(), "ERR UNKNOWN"));
  CHECK(!LegacyTextParser::isReplyLine("OKAY") && !LegacyTextParser::isReplyLine("VOUT: 1V") && LegacyTextParser::isReplyLine("OK"));
}


static void test_payment_lock() {
  using namespace pc::paylock;
  using namespace pc;
  char msg[160];
  // Message layout matches DeviceHmacAuth::paymentLockSignature / hq_control_station_test.php
  size_t n = message("cmd-1", true, "installment_default", 1730000000u, msg, sizeof msg);
  CHECK(n == strlen("cmd-1\n1\ninstallment_default\n1730000000"));
  CHECK(!memcmp(msg, "cmd-1\n1\ninstallment_default\n1730000000", n));
  n = message("cmd-1", false, "cleared", 9u, msg, sizeof msg);
  CHECK(n == strlen("cmd-1\n0\ncleared\n9") && !memcmp(msg, "cmd-1\n0\ncleared\n9", n));
  // HMAC vector from backend test: hash_hmac('sha256', "cmd-1\n0\ncleared\n9", secret)
  // Use a fixed 32-byte secret of 0x11..
  uint8_t secret[32]; memset(secret, 0x11, sizeof secret);
  uint8_t mac[32]; char hex[65];
  n = message("cmd-1", false, "cleared", 9u, msg, sizeof msg);
  hmacSha256(secret, 32, msg, n, mac);
  toHex(mac, 32, hex);
  CHECK(strlen(hex) == 64);
  // Cross-check: message for lock with empty reason
  n = message("abc", true, "", 1u, msg, sizeof msg);
  CHECK(n == strlen("abc\n1\n\n1") && !memcmp(msg, "abc\n1\n\n1", n));
  CHECK(message("", true, "r", 1, msg, sizeof msg) == 0);
  CHECK(message("id", true, "r", 0, msg, sizeof msg) == 0);
  CHECK(message("id", true, "r", 1, msg, 4) == 0);  // too small
  // Judge: accept / duplicate / stale / invalid
  CHECK(judge(100, 50, false) == Verdict::Apply);
  CHECK(judge(100, 100, false) == Verdict::Stale);
  CHECK(judge(99, 100, false) == Verdict::Stale);
  CHECK(judge(100, 50, true) == Verdict::Duplicate);
  CHECK(judge(0, 0, false) == Verdict::Invalid);
  CHECK(controlBlocked(true) && !controlBlocked(false));
  CHECK(!strcmp(setStateBlock(true, true, true, true), "payment_locked"));
  CHECK(!strcmp(setStateBlock(true, false, false, true), "payment_locked"));
  CHECK(!strcmp(setStateBlock(false, true, true, true), "battery_critical_latched"));
  CHECK(!strcmp(setStateBlock(false, true, true, false), "battery_critical_latched"));
  CHECK(!strcmp(setStateBlock(false, true, false, true), "output_overload_latched"));
  CHECK(setStateBlock(false, false, true, true) == nullptr);
  CHECK(setStateBlock(false, true, false, false) == nullptr);
}


#include "live.h"
static void feedText(pc::LegacyTextParser &p, const char *txt, pc::Stm32Status &out, int &pubs) {
  for (const char *c = txt; *c; ++c) if (p.feed((uint8_t)*c, out)) ++pubs;
}
static void test_live() {
  using namespace pc; using namespace pc::live;
  // STM32 2.x inverting record (3-phase print, system_control.c Telemetry_Print)
  LegacyTextParser p; Stm32Status s; int pubs = 0;
  feedText(p, "--- Status ---\r\nSYS : INVERTER\r\nSW  : INVERTER\r\nBAT : 52.96V  99%\r\nGRID: 0.0V\r\nRSV:0\r\nRAW_INST: 12  RAW_RMS: 3.1\r\n"
              "TEMP: 31.5C\r\nCHR : --\r\nVOUT: 229.8V\r\nIOUT: 1.40A\r\nIRAW_INST: 2010  IRAW_AVG: 1998.2\r\nMOD : 0.8123\r\nSOFT: DONE\r\n--------------\r\n"
              "--- Status ---\r\n", s, pubs);
  CHECK(pubs == 1 && s.sys == SysState::Inverter && s.has_vout && s.has_iout && s.has_temp);
  NEAR(s.vout, 229.8, 0.01); NEAR(s.iout, 1.40, 0.001); NEAR(s.temp_c, 31.5, 0.01);
  Inputs in; in.stmFresh = true; in.sys = s.sys; in.sw = s.sw; in.chr = s.chr; in.hasVout = true; in.hasIout = true; in.vout = s.vout; in.iout = s.iout;
  in.bmsOn = true; in.bmsCurrentA = -7; in.bmsPowerW = -370;
  CHECK(source(in) == Source::Battery && status(in) == Status::Inverting);
  float w = 0; CHECK(outputPowerW(in, w) == PowerSrc::Stm32); NEAR(w, 321.72, 0.1);
  in.hasIout = false; CHECK(outputPowerW(in, w) == PowerSrc::BmsEstimate); NEAR(w, 333, 0.1);
  in.solarOk = true; in.solarW = 400; in.bmsCurrentA = 1; CHECK(source(in) == Source::Solar);
  in.solarOk = false;
  in.sys = SysState::Grid; in.sw = SwitchPos::Grid; in.chr = ChargeState::Bulk; in.bmsCurrentA = 10;
  CHECK(source(in) == Source::Grid && status(in) == Status::Charging && outputPowerW(in, w) == PowerSrc::None);
  in.chr = ChargeState::Idle; in.bmsCurrentA = 0; CHECK(status(in) == Status::Idle);
  in.sys = SysState::Idle; in.sw = SwitchPos::Inverter; CHECK(status(in) == Status::Standby && source(in) == Source::None);
  CHECK(outputPowerW(in, w) == PowerSrc::Off && w == 0);
  in.sys = SysState::Fault; CHECK(status(in) == Status::Fault);
  in.sys = SysState::Inverter; in.faultActive = true; CHECK(status(in) == Status::Fault);
  in.stmFresh = false; CHECK(status(in) == Status::Unknown && source(in) == Source::None);
  CHECK(!strcmp(statusName(Status::Inverting), "inverting") && !strcmp(sourceName(Source::Battery), "battery"));
  // models
  const Model *m = findModel("PC5K"); CHECK(m && m->ratedKva == 5.0f && m->usableKw == 4.5f);
  CHECK(findModel("5kva") == m && findModel(" pc5k ") == m && findModel("PC5KVA") == m);
  CHECK(findModel("PC10K") && findModel("PC10K")->usableKw == 9.0f && findModel("PC3K")->usableKw == 2.7f);
  const Model *m35 = findModel("PC3_5K");
  CHECK(m35 && m35->ratedKva == 3.5f && m35->usableKw == 3.15f);
  CHECK(findModel("3.5K") == m35 && findModel("PC3.5K") == m35 && findModel("3.5kva") == m35);
  CHECK(findModel("35K") == m35 && findModel("PC35K") == m35 && findModel(" pc3_5k ") == m35);
  CHECK(!findModel("") && !findModel("PC7K") && !findModel(nullptr));
  float pct = 0; CHECK(loadPct(320, m, pct)); NEAR(pct, 7.111, 0.01); CHECK(!loadPct(320, nullptr, pct));
  // runtime
  Runtime r; float mins = 0;
  r.update(-10, 200, 280, 0); CHECK(r.toEmptyMin(mins)); NEAR(mins, 1200, 0.1); CHECK(!r.toFullMin(mins));
  r.update(-20, 200, 280, 60000);  // halfway to -20 after one tau
  NEAR(r.smoothedA(), -15, 0.01);
  r.update(0.1f, 200, 280, 61000); CHECK(r.toEmptyMin(mins));  // still smoothed discharge
  r.update(20, 200, 280, 62000); CHECK(r.toFullMin(mins)); NEAR(mins, 240, 0.1); CHECK(!r.toEmptyMin(mins));
  Runtime idle; idle.update(0.1f, 200, 280, 0); CHECK(!idle.toEmptyMin(mins) && !idle.toFullMin(mins));
  Runtime tiny; tiny.update(-0.31f, 280, 280, 0); CHECK(tiny.toEmptyMin(mins) && mins == Runtime::kMaxMin);
  // backoff
  Backoff b; CHECK(b.due(0));
  CHECK(!b.fail(0) && !b.fail(10) && b.due(20));
  CHECK(b.fail(20) && b.failed() && b.delayMs() == 1000 && !b.due(500) && b.due(1020));
  CHECK(!b.fail(1020) && b.delayMs() == 2000);
  for (int i = 0; i < 10; ++i) b.fail(5000 + i);
  CHECK(b.delayMs() == 30000);
  CHECK(b.ok() && !b.failed() && b.due(0) && !b.ok());
}

static void test_xfer4() {  // 2.5.6
  CHECK(parseRsvReply("RSV:OK") == RsvReply::Ok);
  CHECK(parseRsvReply("RSV:OK\r\n") == RsvReply::Ok);
  CHECK(parseRsvReply("RSV:ERR,VBAT") == RsvReply::ErrVbat);
  CHECK(parseRsvReply("RSV:ERR,FAULT") == RsvReply::ErrFault);
  CHECK(parseRsvReply("RSV:ERR,BUSY") == RsvReply::ErrBusy);
  CHECK(parseRsvReply("RSV:ERR,XYZ") == RsvReply::ErrOther);
  CHECK(parseRsvReply("RSV:1") == RsvReply::Invalid && parseRsvReply(nullptr) == RsvReply::Invalid);
  LegacyTextParser p; Stm32Status out; int pub = 0;
  feedStr(p, "--- Status ---\r\nSYS : RESERVE\r\nBAT : 49.10V\r\nTEMP: 34.6C  NTC_RAW: 2213\r\nRSV:1\r\n", out, pub);
  uint32_t r0 = p.replySeq();
  feedStr(p, "RSV:ERR,VBAT\r\n", out, pub);
  CHECK(p.replySeq() == r0 + 1 && !strcmp(p.reply(), "RSV:ERR,VBAT"));
  feedStr(p, "RSV:OK\r\n", out, pub);
  CHECK(p.replySeq() == r0 + 2 && parseRsvReply(p.reply()) == RsvReply::Ok);
  feedStr(p, "--- Status ---\r\n", out, pub);
  CHECK(pub == 1 && out.has_temp && out.has_ntc_raw && out.ntc_raw == 2213 && out.has_rsv && out.rsv_latched);
  NEAR(out.temp_c, 34.6, 0.01);
  feedStr(p, "SYS : IDLE\r\nTEMP: 30.0C\r\nRSV:0\r\n--- Status ---\r\n", out, pub);
  CHECK(pub == 2 && out.has_temp && !out.has_ntc_raw && !out.rsv_latched);
}

static void test_solar_view() {  // 2.5.7
  using namespace pc::live;
  SolarView v = solarView(true, 40.0f, 2.5f);
  CHECK(v.valid); CHECK(v.powerW > 99.99f && v.powerW < 100.01f);
  CHECK(!solarView(false, 40.0f, 2.5f).valid);
  CHECK(!solarView(true, NAN, 1.0f).valid);
  CHECK(!solarView(true, 40.0f, INFINITY).valid);
  CHECK(solarView(false, 40.0f, 2.5f).powerW == 0);
  char b[160];
  formatSolarLine(b, sizeof b, v, 18, false, 230);
  CHECK(strcmp(b, "SOLAR valid=1 V=40.00 A=2.500 W=100.0 i2c_errors=18 failing=0 age_ms=230") == 0);
  formatSolarLine(b, sizeof b, SolarView{}, 3, true, UINT32_MAX);
  CHECK(strcmp(b, "SOLAR valid=0 V=null A=null W=null i2c_errors=3 failing=1 age_ms=never") == 0);
  char small[8]; formatSolarLine(small, sizeof small, v, 0, false, 0); CHECK(strlen(small) == 7);
}

static void test_solar_energy() {  // 2.5.8
  using namespace pc::solar;
  // local day: Lagos +60 min; 2026-11-04 23:30 UTC is 2026-11-05 00:30 local
  int64_t e = 1793835000LL;  // 2026-11-04T23:30:00Z
  CHECK(localDay(e, true, 60) == localDay(e, true, 0) + 1);
  CHECK(localDay(e, false, 60) == kNoDay);
  CHECK(localDay(1000, true, 0) == kNoDay);
  Energy en;
  // 1000 W for 1 h in 500 ms steps = 1000 Wh
  uint32_t t = 0;
  for (int i = 0; i <= 7200; ++i) { en.update(true, 1000.0f, t, 100); t += 500; }
  CHECK(fabs(en.totalWh() - 1000.0) < 0.01); CHECK(fabs(en.todayWh() - 1000.0) < 0.01);
  // gap > 10 s not integrated; negative power ignored; invalid breaks the chain
  en.update(true, 1000.0f, t + 20000, 100);
  CHECK(fabs(en.totalWh() - 1000.0) < 0.01);
  en.update(true, -50.0f, t + 20500, 100); CHECK(fabs(en.totalWh() - 1000.0) < 0.01);
  en.update(false, 0, t + 21000, 100); en.update(true, 1000.0f, t + 21500, 100);
  CHECK(fabs(en.totalWh() - 1000.0) < 0.01);
  // save policy: 10 Wh after 5 min; 10 Wh before 5 min does not
  Energy s;
  s.update(true, 3600.0f, 0, 100);
  s.update(true, 3600.0f, 10000, 100);  // 10 Wh in 10 s
  CHECK(fabs(s.totalWh() - 10.0) < 0.01);
  CHECK(!s.saveDue(10000));
  CHECK(s.saveDue(kSaveMinMs));
  s.markSaved(kSaveMinMs);
  CHECK(!s.saveDue(kSaveMinMs));
  // 1 Wh after 15 min saves; 0.2 Wh after 15 min does not
  uint32_t t1 = kSaveMinMs + 1000;
  s.update(true, 3600.0f, t1, 100);
  s.update(true, 3600.0f, t1 + 1000, 100);  // +1 Wh
  CHECK(fabs(s.totalWh() - 11.0) < 0.01);
  CHECK(!s.saveDue(t1 + 1000));
  CHECK(s.saveDue(kSaveMinMs + kSaveIdleMs));
  Energy trickle;
  trickle.update(true, 720.0f, 0, 100);
  trickle.update(true, 720.0f, 1000, 100);  // 0.2 Wh in 1 s
  CHECK(fabs(trickle.totalWh() - 0.2) < 0.01);
  CHECK(!trickle.saveDue(kSaveIdleMs));
  CHECK(!trickle.saveDue(kSaveIdleMs + kSaveMinMs));
  // day rollover: today -> 0, total kept, save due even with no unsaved Wh
  Energy enDay;
  uint32_t tDay = 0;
  for (int i = 0; i <= 7200; ++i) { enDay.update(true, 1000.0f, tDay, 100); tDay += 500; }
  enDay.markSaved(tDay);
  double tot = enDay.totalWh();
  enDay.update(true, 0.0f, tDay + 500, 101);
  CHECK(enDay.todayWh() == 0); CHECK(fabs(enDay.totalWh() - tot) < 1e-9); CHECK(enDay.saveDue(tDay + 500)); CHECK(enDay.day() == 101);
  // unknown day keeps counting on the last day, no reset
  Energy b; b.restore(5000, 300, 101);
  b.update(true, 0, 0, kNoDay); CHECK(b.todayWh() == 300); CHECK(b.day() == 101);
  b.update(true, 0, 500, 101); CHECK(b.todayWh() == 300);   // same day after clock arrives: kept
  b.update(true, 0, 1000, 102); CHECK(b.todayWh() == 0);    // next day: reset
  CHECK(b.totalWh() == 5000);
  // never-saved day + clock arrives: adopt, no reset
  Energy c; c.update(true, 3600.0f, 0, kNoDay); c.update(true, 3600.0f, 1000, kNoDay);
  CHECK(fabs(c.todayWh() - 1.0) < 1e-9); c.update(true, 0, 1500, 50); CHECK(fabs(c.todayWh() - 1.0) < 1e-9);
  // restore sanitises
  Energy d; d.restore(NAN, -3, 7); CHECK(d.totalWh() == 0 && d.todayWh() == 0);
  Energy f; f.restore(10, 20, 7); CHECK(f.totalWh() == 20);
}

static powercell::StmEstimateSample stm_at(float volts, float amps, uint32_t mono, int64_t unix_s, int32_t day) {
  powercell::StmEstimateSample s;
  s.valid = true;
  s.volts = volts;
  s.amps = amps;
  s.mono_ms = mono;
  s.unix_s = unix_s;
  s.day_id = day;
  return s;
}

static void test_stm_estimate() {
  using namespace powercell;
  const int64_t utc_midnight = unix_from_utc(2026, 11, 10, 0, 0, 0);
  const int64_t lagos_midnight = utc_midnight - static_cast<int64_t>(kLagosOffsetMin) * 60;
  CHECK(kLagosOffsetMin == 60);
  CHECK(pc::clk::kTzDefault == 60);
  CHECK(pc::solar::localDay(lagos_midnight, true, kLagosOffsetMin) ==
        pc::solar::localDay(lagos_midnight - 1, true, kLagosOffsetMin) + 1);

  CHECK(StmEnergyEstimate::plausible(230, 10));
  CHECK(StmEnergyEstimate::plausible(0, 0));
  CHECK(!StmEnergyEstimate::plausible(400, 1));
  CHECK(!StmEnergyEstimate::plausible(230, -1));
  CHECK(!StmEnergyEstimate::plausible(230, 40));
  CHECK(!StmEnergyEstimate::plausible(10, 5));
  CHECK(!StmEnergyEstimate::plausible(NAN, 1));
  NEAR(StmEnergyEstimate::activeWatts(230, 10), 1840.0, 1e-4);

  StmEnergyEstimate e;
  const double watts = 1840.0;
  e.observe(stm_at(230, 10, 1000, 1700000000, 100));
  if (e.publish().persist) e.ackPersist(1000);
  for (int i = 1; i <= 10; ++i) e.observe(stm_at(230, 10, 1000 + static_cast<uint32_t>(i) * 1000, 1700000000 + i, 100));
  NEAR(e.workingTodayWh(), watts * 10.0 / 3600.0, 1e-4);
  NEAR(e.workingLifetimeWh(), watts * 10.0 / 3600.0, 1e-4);
  CHECK(e.publish().lifetime_wh <= e.workingLifetimeWh() + 1e-9);

  const double before_save = e.publish().lifetime_wh;
  bool saved = false;
  for (int i = 11; i <= 920; ++i) {
    const auto view = e.observe(stm_at(230, 10, 1000 + static_cast<uint32_t>(i) * 1000, 1700000000 + i, 100));
    if (view.persist) {
      e.ackPersist(1000 + static_cast<uint32_t>(i) * 1000);
      saved = true;
      break;
    }
  }
  CHECK(saved);
  CHECK(e.publish().lifetime_wh + 1e-6 >= before_save);
  CHECK(e.publish().today_known);
  NEAR(e.publish().lifetime_wh, e.workingLifetimeWh(), 1e-6);
  const double published = e.publish().lifetime_wh;
  e.observe(stm_at(230, 10, 1000 + 921u * 1000, 1700000000 + 921, 100));
  CHECK(e.publish().lifetime_wh + 1e-9 >= published);

  const double life_before_midnight = e.workingLifetimeWh();
  const auto rolled = e.observe(stm_at(230, 10, 1000 + 922u * 1000, 1700000000 + 922, 101));
  NEAR(e.workingTodayWh(), watts / 3600.0, 1e-4);
  NEAR(e.workingLifetimeWh(), life_before_midnight + watts / 3600.0, 1e-4);
  CHECK(rolled.persist);
  e.ackPersist(1000 + 922u * 1000);
  const double saved_today = e.workingTodayWh();
  const double saved_life = e.workingLifetimeWh();

  StmEnergyEstimate booted;
  booted.restore(101, saved_today, saved_life);
  StmEstimateSample quiet = stm_at(0, 0, 50, 1700001000, 101);
  quiet.valid = false;
  const auto restored = booted.observe(quiet);
  CHECK(restored.today_known);
  NEAR(restored.lifetime_wh, saved_life, 1e-6);
  NEAR(restored.today_kwh * 1000.0, saved_today, 1e-4);
  booted.observe(stm_at(230, 10, 5000, 1700001000, 101));
  booted.observe(stm_at(230, 10, 6000, 1700001001, 101));
  CHECK(booted.workingLifetimeWh() > saved_life);
  CHECK(booted.publish().lifetime_wh + 1e-9 >= saved_life);

  StmEnergyEstimate jump;
  jump.observe(stm_at(230, 10, 1000, 1700000000, 100));
  jump.observe(stm_at(230, 10, 2000, 1700000001, 100));
  const double jumped = jump.workingLifetimeWh();
  jump.observe(stm_at(230, 10, 3000, 1700000001 + 1000, 100));
  NEAR(jump.workingLifetimeWh(), jumped, 1e-6);

  StmEnergyEstimate gap;
  gap.observe(stm_at(230, 10, 1000, 1700000000, 100));
  gap.observe(stm_at(230, 10, 2000, 1700000001, 100));
  const double gapped = gap.workingLifetimeWh();
  gap.observe(stm_at(230, 10, 2000 + kStmMaxGapMs + 1, 1700000001 + 12, 100));
  NEAR(gap.workingLifetimeWh(), gapped, 1e-6);

  StmEnergyEstimate backward;
  backward.observe(stm_at(230, 10, 5000, 1700000010, 100));
  backward.observe(stm_at(230, 10, 6000, 1700000011, 100));
  const double backed = backward.workingLifetimeWh();
  backward.observe(stm_at(230, 10, 5500, 1700000012, 100));
  NEAR(backward.workingLifetimeWh(), backed, 1e-6);
  backward.observe(stm_at(230, 10, 7000, 1700000011 - 30, 100));
  NEAR(backward.workingLifetimeWh(), backed, 1e-6);

  StmEnergyEstimate bad;
  bad.observe(stm_at(230, 10, 1000, 1700000000, 100));
  bad.observe(stm_at(230, 10, 2000, 1700000001, 100));
  const double kept = bad.workingLifetimeWh();
  const double kept_today = bad.workingTodayWh();
  bad.observe(stm_at(400, 1, 3000, 1700000002, 100));
  bad.observe(stm_at(230, -1, 4000, 1700000003, 100));
  bad.observe(stm_at(230, 40, 5000, 1700000004, 100));
  bad.observe(stm_at(10, 5, 6000, 1700000005, 100));
  StmEstimateSample missing = stm_at(0, 0, 7000, 1700000006, 100);
  missing.valid = false;
  bad.observe(missing);
  NEAR(bad.workingLifetimeWh(), kept, 1e-6);
  NEAR(bad.workingTodayWh(), kept_today, 1e-6);
  CHECK(kept_today > 0);
}

static void test_grid_sense() {
  using namespace powercell::grid;
  const float k = kScale;
  const int n = kSampleCount;
  float wave[kSampleCount];
  const float offset_v = 1.20f;  // not 1.65 V
  const float peak_v = (230.f / k) * sqrtf(2.f);
  for (int i = 0; i < n; ++i) {
    const float pin = offset_v + peak_v * sinf(2.f * 3.14159265f * static_cast<float>(i) / kSamplesPerCycle);
    wave[i] = pin * 1000.f;
  }
  const WaveResult mains = fromPinMillivolts(wave, n, k);
  CHECK(mains.ok && !mains.clipped);
  NEAR(mains.ac_ref_v, offset_v, 0.02);
  NEAR(mains.vrms, 230.0, 0.5);

  float zero[kSampleCount];
  for (int i = 0; i < n; ++i) zero[i] = 1800.f;
  const WaveResult dead = fromPinMillivolts(zero, n, k);
  CHECK(dead.ok && !dead.clipped);
  NEAR(dead.ac_ref_v, 1.8, 0.001);
  NEAR(dead.vrms, 0.0, 0.001);

  float biased[8] = {900, 1100, 1300, 1500, 1700, 1900, 2100, 2300};
  NEAR(meanMv(biased, 8) / 1000.f, 1.6, 0.001);
  const WaveResult mid = fromPinMillivolts(biased, 8, k);
  CHECK(mid.ok);
  NEAR(mid.ac_ref_v, 1.6, 0.001);

  wave[3] = 2500.f;
  CHECK(fromPinMillivolts(wave, n, k).clipped);
  wave[3] = offset_v * 1000.f;
  wave[4] = 100.f;
  CHECK(fromPinMillivolts(wave, n, k).clipped);
  CHECK(!anyClipped(zero, n));

  CHECK(!presentHysteresis(false, 165.f));
  CHECK(presentHysteresis(false, 170.f));
  CHECK(presentHysteresis(true, 165.f));
  CHECK(presentHysteresis(true, 160.f));
  CHECK(!presentHysteresis(true, 159.f));

  // Bench crosstalk: inverter ON, no mains. GPIO39 saw 36-88 V. Must not be grid.
  const bool inverting = true;
  CHECK(!senseIsGrid(false, 36.f, inverting));
  CHECK(!senseIsGrid(false, 88.f, inverting));
  CHECK(!senseIsGrid(false, 159.f, inverting));
  CHECK(senseIsGrid(false, 230.f, inverting));
  CHECK(senseIsGrid(true, 170.f, inverting));
  CHECK(!senseIsGrid(true, 36.f, inverting));
  CHECK(!senseIsGrid(true, 88.f, inverting));
  NEAR(publishedVoltage(false, 36.f), 0.0, 0.001);
  NEAR(publishedVoltage(false, 88.f), 0.0, 0.001);
  NEAR(publishedVoltage(true, 230.f), 230.0, 0.001);

  PresentDebounce d;
  CHECK(!d.update(false, 0));
  CHECK(!d.update(true, 0));                   // start of enter window
  CHECK(!d.update(true, kPresentDebounceMs - 1));
  CHECK(d.update(true, kPresentDebounceMs));   // held 2 s at 230 V class
  CHECK(d.update(false, kPresentDebounceMs));  // start of exit window, still present
  CHECK(d.update(false, kPresentDebounceMs + kPresentDebounceMs - 1));
  CHECK(!d.update(false, kPresentDebounceMs + kPresentDebounceMs));
  // A brief spike must not enter.
  PresentDebounce spike;
  CHECK(!spike.update(true, 1000));
  CHECK(!spike.update(false, 1500));
  CHECK(!spike.update(true, 2000));
  CHECK(!spike.update(true, 2000 + kPresentDebounceMs - 1));
  CHECK(spike.update(true, 2000 + kPresentDebounceMs));
}

static void test_jbd() {
  using namespace powercell::jbd;
  uint8_t req[7];
  buildRead(kRegBasic, req);
  CHECK(req[0] == 0xDD && req[1] == 0xA5 && req[2] == 0x03 && req[3] == 0x00);
  CHECK(req[4] == 0xFF && req[5] == 0xFD && req[6] == 0x77);
  buildRead(kRegCells, req);
  CHECK(req[2] == 0x04 && req[4] == 0xFF && req[5] == 0xFC && req[6] == 0x77);

  uint8_t basic[23] = {};
  basic[0] = 0x14; basic[1] = 0x50;             // 52.00 V
  basic[4] = 0x25; basic[5] = 0x80;             // 96.00 Ah
  basic[6] = 0x27; basic[7] = 0x10;             // 100.00 Ah
  basic[8] = 0x00; basic[9] = 0x0A;             // 10 cycles
  basic[19] = 96;                               // SOC
  basic[20] = 0x03;                             // charge + discharge MOS
  basic[21] = 16;
  uint8_t frame[40];
  size_t n = wrapResponse(kRegBasic, 0, basic, 23, frame, sizeof frame);
  const uint8_t* data = nullptr;
  uint8_t dlen = 0;
  CHECK(n == 30 && checkFrame(frame, n, kRegBasic, &data, &dlen) && dlen == 23);
  Reading r;
  CHECK(parseBasic(data, dlen, &r) && r.valid && r.soc == 96);
  NEAR(r.packVoltage, 52.0, 0.01);
  NEAR(r.remainAh, 96.0, 0.01);
  CHECK(r.chargeMos && r.dischargeMos && r.cellCount == 16 && r.batStatus == 0);

  frame[4 + 23] ^= 0x01;
  CHECK(!checkFrame(frame, n, kRegBasic, &data, &dlen));
  CHECK(!checkFrame(frame, 4, kRegBasic, &data, &dlen));
  CHECK(!checkFrame(frame, n - 1, kRegBasic, &data, &dlen));

  uint8_t badSoc[23] = {};
  memcpy(badSoc, basic, 23);
  badSoc[19] = 0;
  n = wrapResponse(kRegBasic, 0, badSoc, 23, frame, sizeof frame);
  CHECK(checkFrame(frame, n, kRegBasic, &data, &dlen));
  Reading kept;
  kept.soc = 55;
  CHECK(!parseBasic(data, dlen, &kept) && kept.soc == 55);

  uint8_t cells[4] = {0x0C, 0xE4, 0x0C, 0x80};  // 3300 mV, 3200 mV
  n = wrapResponse(kRegCells, 0, cells, 4, frame, sizeof frame);
  CHECK(checkFrame(frame, n, kRegCells, &data, &dlen));
  CHECK(parseCells(data, dlen, &r));
  NEAR(r.cellVoltage[0], 3.3, 0.001);
  NEAR(r.cellVoltage[1], 3.2, 0.001);
  NEAR(r.cellDiffMv, 100.0, 0.2);
  CHECK(r.cellCount == 2 && r.maxCell == 0 && r.minCell == 1);
  CHECK(!parseCells(data, 3, &r));
}

static bool read_text_file(const char *path, char *buf, size_t cap) {
  FILE *f = fopen(path, "r");
  if (!f) return false;
  size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = 0;
  return n > 0;
}

static bool flag_in_env(const char *text, const char *env, const char *flag) {
  char tag[80];
  snprintf(tag, sizeof tag, "[env:%s]", env);
  const char *start = strstr(text, tag);
  if (!start) return false;
  const char *next = strstr(start + 1, "[env:");
  size_t n = next ? (size_t)(next - start) : strlen(start);
  char block[4096];
  if (n >= sizeof block) n = sizeof block - 1;
  memcpy(block, start, n);
  block[n] = 0;
  return strstr(block, flag) != nullptr;
}


static float ntc_mv_for(float c, bool low_side) {  // independent forward model of Dubem's divider
  const double r = 10000.0 * exp(3950.0 * (1.0 / (c + 273.15) - 1.0 / 298.15));
  return (float)(low_side ? 3300.0 * r / (r + 2700.0) : 3300.0 * 2700.0 / (r + 2700.0));
}

static void test_dsp_thermal() {
  using namespace powercell::thermal;
  float c = -99.f;
  // Dubem default wiring: NTC to 3V3, 2k7 to GND. 25 C -> 701.6 mV.
  CHECK(mvToCelsius(3300.f * 2700.f / 12700.f, &c, false)); NEAR(c, 25.0, 0.05);
  const float pts[] = {0.5f, 10.f, 25.f, 40.f, 65.f, 70.f, 85.f, 100.f, 125.f, 149.f};
  for (float t : pts) {
    c = -99.f; CHECK(mvToCelsius(ntc_mv_for(t, false), &c, false)); NEAR(c, t, 0.05);
    c = -99.f; CHECK(mvToCelsius(ntc_mv_for(t, true), &c, true)); NEAR(c, t, 0.05);
  }
  // Pin volts rise with temperature on the default wiring, fall on the low-side wiring.
  CHECK(ntc_mv_for(70.f, false) > ntc_mv_for(25.f, false));
  CHECK(ntc_mv_for(70.f, true) < ntc_mv_for(25.f, true));
  // Open / short / rails: invalid, *c untouched (never a fabricated value).
  const float bad[] = {0.f, 10.f, 39.9f, 3260.1f, 3300.f, 4000.f, -5.f};
  for (float mv : bad) {
    c = 12.34f;
    CHECK(!mvToCelsius(mv, &c, false)); NEAR(c, 12.34, 1e-6);
    c = 12.34f;
    CHECK(!mvToCelsius(mv, &c, true)); NEAR(c, 12.34, 1e-6);
  }
  c = 12.34f; CHECK(!mvToCelsius(NAN, &c, false)); NEAR(c, 12.34, 1e-6);
  CHECK(!mvToCelsius(INFINITY, &c, false));
  CHECK(!mvToCelsius(700.f, nullptr, false));
  // ESP32 11 dB ADC saturates near 3.1 V: a shorted NTC reads ~3130 mV -> >150 C -> invalid.
  c = 12.34f; CHECK(!mvToCelsius(3130.f, &c, false)); NEAR(c, 12.34, 1e-6);
  // Below 0 C (or a reading that maps there) is out of range.
  CHECK(!mvToCelsius(200.f, &c, false));
  CHECK(!mvToCelsius(ntc_mv_for(-5.f, false), &c, false));
  CHECK(!mvToCelsius(ntc_mv_for(155.f, false), &c, false));
  CHECK(!mvToCelsius(ntc_mv_for(-5.f, true), &c, true));

  // Fan: ON >= 70, OFF <= 60 (Caleb 2026-11-11), hold between, fault forces ON (Dubem fail-safe).
  CHECK(kFanOnC == 70.0f && kFanOffC == 60.0f);
  CHECK(!fanWanted(false, false, 25.f));
  CHECK(!fanWanted(false, false, 69.9f));
  CHECK(fanWanted(false, false, 70.f));
  CHECK(fanWanted(true, false, 67.f));
  CHECK(!fanWanted(false, false, 67.f));
  CHECK(fanWanted(true, false, 65.f));   // was the old OFF point: now still ON
  CHECK(fanWanted(true, false, 60.1f));
  CHECK(!fanWanted(true, false, 60.f));
  CHECK(fanWanted(false, true, 25.f));
  CHECK(fanWanted(false, true, NAN));
  CHECK(fanWanted(false, false, NAN));
  CHECK(fanWanted(true, false, 85.f));   // fan stays on through a cutoff
  // Sweep up then down: one ON edge at 70, one OFF edge at 60.
  bool fan = false; int on_edges = 0, off_edges = 0;
  for (float t = 50.f; t <= 90.f; t += 0.5f) { bool w = fanWanted(fan, false, t); if (w && !fan) { ++on_edges; NEAR(t, 70.0, 0.01); } fan = w; }
  for (float t = 90.f; t >= 50.f; t -= 0.5f) { bool w = fanWanted(fan, false, t); if (!w && fan) { ++off_edges; NEAR(t, 60.0, 0.01); } fan = w; }
  CHECK(on_edges == 1 && off_edges == 1);

  // Sensor health: no fault before the first sample; fault or stale after.
  CHECK(!sensorFaulted(false, false, 0));
  CHECK(!sensorFaulted(false, true, 99999));
  CHECK(!sensorFaulted(true, false, 2000));
  CHECK(sensorFaulted(true, true, 0));
  CHECK(!sensorFaulted(true, false, kStaleMs - 1));
  CHECK(sensorFaulted(true, false, kStaleMs));
}

static void test_dsp_over_temp() {
  using namespace powercell::thermal;
  CHECK(kCutoffC == 80.0f && kOnAllowedBelowC == 70.0f);
  OverTempGuard g;
  CHECK(!g.latched() && !g.blocksOn(true, 25.f) && !g.blocksOn(false, 0.f));
  // Below 80: no latch, ON allowed (70-80 too when not latched).
  CHECK(!g.update(true, 79.9f) && !g.latched());
  CHECK(!g.blocksOn(true, 75.f));
  // Sensor fault alone never latches, even with a stale hot value passed in.
  CHECK(!g.update(false, 95.f) && !g.latched() && !g.dirty());
  CHECK(!g.blocksOn(false, 95.f));
  // 80.0 latches once (edge), marks dirty for NVS.
  CHECK(g.update(true, 80.f));
  CHECK(g.latched() && g.dirty());
  CHECK(!g.update(true, 85.f));            // no second edge
  g.ackSaved(); CHECK(!g.dirty());
  // While latched: hot, 70-80, exactly 70, no reading, fault -> ON refused, latch kept.
  CHECK(!g.acceptOn(true, 85.f) && g.latched());
  CHECK(!g.acceptOn(true, 75.f) && g.latched());
  CHECK(!g.acceptOn(true, 70.f) && g.latched());
  CHECK(!g.acceptOn(false, 25.f) && g.latched());
  CHECK(g.blocksOn(true, 70.f) && g.blocksOn(false, 0.f) && !g.dirty());
  // Cooling alone does not clear the latch (no auto-ON).
  CHECK(!g.update(true, 40.f) && g.latched());
  // User ON at a valid 69.9 C: accepted, latch cleared, dirty for NVS.
  CHECK(!g.blocksOn(true, 69.9f));
  CHECK(g.acceptOn(true, 69.9f) && !g.latched() && g.dirty());
  g.ackSaved();
  // Not latched: ON accepted with no state change; >= 80 refused even before the service pass latches.
  CHECK(g.acceptOn(true, 50.f) && !g.dirty());
  CHECK(g.blocksOn(true, 80.f) && !g.acceptOn(true, 81.f));
  // Reboot with a persisted latch and no reading yet: ON refused until valid < 70.
  OverTempGuard b; b.restore(true);
  CHECK(b.latched() && !b.dirty());
  CHECK(!b.acceptOn(false, 0.f) && b.latched());
  CHECK(b.acceptOn(true, 30.f) && !b.latched());

  // ON gate order: payment lock > battery > overload > over temperature.
  using pc::paylock::setStateBlock;
  CHECK(!strcmp(setStateBlock(false, true, false, false, true), "over_temperature_latched"));
  CHECK(!strcmp(setStateBlock(false, true, true, false, true), "battery_critical_latched"));
  CHECK(!strcmp(setStateBlock(false, true, false, true, true), "output_overload_latched"));
  CHECK(!strcmp(setStateBlock(true, true, false, false, true), "payment_locked"));
  CHECK(setStateBlock(false, false, false, false, true) == nullptr);  // OFF is never blocked
  CHECK(setStateBlock(false, true, false, false) == nullptr);

  // Alerts through the event engine (same path as battery_critical / over_temp).
  using namespace pc::ev;
  Engine e; Inputs in; Event out[24];
  in.dspThermal = true; in.sysTempValid = true; in.sysTemp = 45.f;
  e.update(in, 1000, 0); CHECK(e.drain(out, 24) == 0);
  in.sysTemp = 81.f; in.overTempLatched = true;
  e.update(in, 2000, 0);
  size_t n = e.drain(out, 24);
  CHECK(n == 1 && out[0].type == Type::OverTemp && out[0].active && out[0].severity == Sev::Critical);
  CHECK(n == 1 && !strcmp(out[0].id, "over_temperature") && !strcmp(typeName(out[0].type), "over_temp"));
  CHECK(n == 1 && out[0].hasValue && fabs(out[0].value - 81.0) < 0.01 && !strcmp(out[0].code, "system_ntc_80c_cutoff"));
  CHECK(n == 1 && out[0].releaseRequires && !strcmp(out[0].releaseRequires, "cool_below_70c_then_on"));
  in.sysTemp = 50.f; e.update(in, 3000, 0); CHECK(e.drain(out, 24) == 0);  // still latched: no clear, no repeat
  CHECK(e.activeCount() == 1);
  in.overTempLatched = false; e.update(in, 4000, 0);
  n = e.drain(out, 24);
  CHECK(n == 1 && out[0].type == Type::OverTemp && !out[0].active);

  // temp_sensor_fault: raised once after 4 s of fault, no repeat, cleared after 10 s valid.
  Engine f; Inputs fi; fi.dspThermal = true; fi.sysTempValid = false; fi.sysTempFault = true;
  f.update(fi, 0, 0); CHECK(f.drain(out, 24) == 0);
  f.update(fi, 3999, 0); CHECK(f.drain(out, 24) == 0);
  f.update(fi, 4000, 0);
  n = f.drain(out, 24);
  CHECK(n == 1 && out[0].type == Type::TempSensorFault && out[0].active && out[0].severity == Sev::Warning);
  CHECK(n == 1 && !strcmp(typeName(out[0].type), "temp_sensor_fault") && !strcmp(out[0].id, "temp_sensor_fault"));
  for (uint32_t t = 5000; t < 60000; t += 1000) f.update(fi, t, 0);
  CHECK(f.drain(out, 24) == 0);  // no spam while it stays faulted
  // A fault never raises the cutoff alert.
  for (size_t i = 0; i < f.activeCount(); ++i) CHECK(f.active(i)->type != Type::OverTemp);
  fi.sysTempFault = false; fi.sysTempValid = true; fi.sysTemp = 30.f;
  f.update(fi, 60000, 0); f.update(fi, 69999, 0); CHECK(f.drain(out, 24) == 0);
  f.update(fi, 70000, 0);
  n = f.drain(out, 24);
  CHECK(n == 1 && out[0].type == Type::TempSensorFault && !out[0].active);
  // A brief flicker (< 4 s) does not alert.
  fi.sysTempFault = true; f.update(fi, 71000, 0); fi.sysTempFault = false; f.update(fi, 73000, 0); f.update(fi, 90000, 0);
  CHECK(f.drain(out, 24) == 0);
  // STM images (dspThermal false) never raise either alert.
  Engine s; Inputs si; si.overTempLatched = true; si.sysTempFault = true;
  s.update(si, 0, 0); s.update(si, 20000, 0);
  n = s.drain(out, 24);
  bool any = false; for (size_t i = 0; i < n; ++i) any |= out[i].type == Type::OverTemp || out[i].type == Type::TempSensorFault;
  CHECK(!any);
}

static void test_dsp_bms_select() {
  // Host tests compile like an STM image: JK-PB, DSP_BMS_JKPB left at its default 0.
  CHECK(POWERCELL_DSP011 == 0);
  CHECK(DSP_BMS_JKPB == 0);
  CHECK(POWERCELL_BMS_JKPB == 1);
  CHECK(PIN_BMS_RX == 17 && PIN_BMS_TX == 16 && PIN_BMS_BAUD == 115200);
  char buf[16384];
  const char *paths[] = {"../../platformio.ini", "../platformio.ini", "platformio.ini"};
  bool got = false;
  for (const char *path : paths) {
    if (read_text_file(path, buf, sizeof buf)) { got = true; break; }
  }
  CHECK(got);
  if (!got) return;
  CHECK(flag_in_env(buf, "esp32dev_dsp011_5kw", "DSP_BMS_JKPB=1"));
  CHECK(flag_in_env(buf, "esp32dev_dsp011_35kw", "DSP_BMS_JKPB=1"));  // Dubem working firmware: JK-PB on both
  CHECK(!flag_in_env(buf, "esp32dev_stm01_5kw", "DSP_BMS_JKPB"));
  CHECK(!flag_in_env(buf, "esp32dev_stm01_35kw", "DSP_BMS_JKPB"));
}

static void test_live_grid() {  // grid_available as an input to live source/status
  using namespace pc; using namespace pc::live;
  enum G { kUnknown, kAbsent, kPresent };
  const float amps[3] = {5.0f, -7.0f, 0.0f};  // charging, discharging, idle
  auto mk = [](bool dsp, SysState sys, G g, float a) {
    Inputs in; in.stmFresh = true; in.sys = sys; in.sysAssumed = dsp;
    in.hasVout = in.hasIout = (sys == SysState::Inverter); in.vout = 230; in.iout = 2;
    in.bmsOn = true; in.bmsCurrentA = a; in.bmsPowerW = a * 52;
    in.gridKnown = g != kUnknown; in.gridPresent = g == kPresent;
    return in;
  };
  // Baseline: what source/status were before the grid input existed.
  auto legacy = [](Inputs in) { in.gridKnown = false; in.gridPresent = false; return in; };
  // DSP: sys is the hardcoded Inverter placeholder.
  for (int g = 0; g < 3; ++g) for (float a : amps) {
    Inputs in = mk(true, SysState::Inverter, (G)g, a);
    Source src = source(in); Status st = status(in);
    if (g == kPresent && a > -kChargeA) {
      CHECK(src == Source::Grid);
      CHECK(st == (a > kChargeA ? Status::Charging : Status::Idle));
    } else {
      CHECK(src == source(legacy(in)) && st == status(legacy(in)));
      CHECK(src == Source::Battery && st == Status::Inverting);  // unknown/absent or discharging: unchanged
    }
  }
  {  // DSP grid present, small idle drain inside the +-0.5 A band: idle pass-through
    Inputs in = mk(true, SysState::Inverter, kPresent, -0.3f);
    CHECK(source(in) == Source::Grid && status(in) == Status::Idle);
    in.bmsCurrentA = -0.5f; CHECK(source(in) == Source::Battery && status(in) == Status::Inverting);
  }
  {  // DSP grid present but BMS offline: cannot tell discharge, keep today
    Inputs in = mk(true, SysState::Inverter, kPresent, 0); in.bmsOn = false;
    CHECK(source(in) == Source::Battery && status(in) == Status::Inverting);
  }
  {  // DSP grid present, fault still wins on status
    Inputs in = mk(true, SysState::Inverter, kPresent, 5); in.faultActive = true;
    CHECK(status(in) == Status::Fault && source(in) == Source::Grid);
  }
  {  // DSP grid present + PV active, pack charging: grid (was solar)
    Inputs in = mk(true, SysState::Inverter, kPresent, 3); in.solarOk = true; in.solarW = 500;
    CHECK(source(in) == Source::Grid && status(in) == Status::Charging);
    in.gridPresent = false; CHECK(source(in) == Source::Solar && status(in) == Status::Inverting);
    in.gridKnown = false; CHECK(source(in) == Source::Solar && status(in) == Status::Inverting);
  }
  {  // DSP stale PZEM: unchanged (unknown / none)
    Inputs in = mk(true, SysState::Inverter, kPresent, 5); in.stmFresh = false;
    CHECK(source(in) == Source::None && status(in) == Status::Unknown);
  }
  {  // DSP output power path untouched by the grid input
    Inputs in = mk(true, SysState::Inverter, kPresent, 5); float w = 0;
    CHECK(outputPowerW(in, w) == PowerSrc::Stm32); NEAR(w, 460, 0.1);
  }
  // STM: a real SYS report is authoritative for Inverter/Grid/Idle/Reserve/Fault.
  const SysState sysAll[] = {SysState::Inverter, SysState::Grid, SysState::Idle, SysState::Reserve, SysState::Fault};
  for (SysState sys : sysAll) for (int g = 0; g < 3; ++g) for (float a : amps) {
    Inputs in = mk(false, sys, (G)g, a);
    CHECK(source(in) == source(legacy(in)) && status(in) == status(legacy(in)));
  }
  {  // STM explicit expectations, grid present
    Inputs in = mk(false, SysState::Inverter, kPresent, 5);
    CHECK(source(in) == Source::Battery && status(in) == Status::Inverting);
    in = mk(false, SysState::Grid, kPresent, 5); CHECK(source(in) == Source::Grid && status(in) == Status::Charging);
    in = mk(false, SysState::Grid, kPresent, 0); CHECK(source(in) == Source::Grid && status(in) == Status::Idle);
    in = mk(false, SysState::Idle, kPresent, 0); CHECK(source(in) == Source::None && status(in) == Status::Standby);
  }
  // STM with SYS Unknown (framed sys > 5): grid input applies like DSP.
  for (int g = 0; g < 3; ++g) for (float a : amps) {
    Inputs in = mk(false, SysState::Unknown, (G)g, a);
    if (g == kPresent && a > -kChargeA) {
      CHECK(source(in) == Source::Grid && status(in) == (a > kChargeA ? Status::Charging : Status::Idle));
    } else {
      CHECK(source(in) == Source::None);
      CHECK(status(in) == (a > kChargeA ? Status::Charging : Status::Unknown));
    }
  }
  CHECK(!strcmp(statusName(Status::Idle), "idle") && !strcmp(sourceName(Source::Grid), "grid") &&
        !strcmp(statusName(Status::Charging), "charging"));
}

static void test_app_power_kw() {  // 2.5.8: the app's displayKw(x) = x > 20 ? x/1000 : x
  auto displayKw = [](double x) { return fabs(x) > 20.0 ? x / 1000.0 : x; };
  const float ws[] = {0.0f, 5.0f, 15.0f, 20.0f, 21.0f, 127.9f, 3000.0f, 19999.0f};
  for (float w : ws) CHECK(fabs(displayKw(pc::live::appPowerKw(w)) - w / 1000.0) < 1e-6);
  char b[200];
  pc::live::formatSolarLine2(b, sizeof b, pc::live::solarView(true, 40.0f, 2.5f), 0, false, 10, 1.2345, 12.5);
  CHECK(strcmp(b, "SOLAR valid=1 V=40.00 A=2.500 W=100.0 i2c_errors=0 failing=0 age_ms=10 today_kwh=1.234 total_kwh=12.500") == 0 ||
        strcmp(b, "SOLAR valid=1 V=40.00 A=2.500 W=100.0 i2c_errors=0 failing=0 age_ms=10 today_kwh=1.235 total_kwh=12.500") == 0);
  pc::live::formatSolarLine2(b, sizeof b, pc::live::SolarView{}, 0, false, UINT32_MAX, -1, -1);
  CHECK(strstr(b, "today_kwh=null total_kwh=null") != nullptr);
}

static void test_mdns() {
  using namespace pc::mdns;
  char h[kHostCap];
  const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xDD, 0xEE, 0xFF};
  // Display ids already valid stay exactly as /v2/setup/info reports them.
  CHECK(hostLabel("ACE-000001", h, sizeof h) == 10 && !strcmp(h, "ACE-000001"));
  CHECK(hostLabel("ACE-DDEEFF", h, sizeof h) == 10 && !strcmp(h, "ACE-DDEEFF"));
  // Spaces, dots, underscores, unicode bytes collapse to one '-'; edges trimmed.
  CHECK(hostLabel("My Powercell #2", h, sizeof h) && !strcmp(h, "My-Powercell-2"));
  CHECK(hostLabel("  ace_000001.local ", h, sizeof h) && !strcmp(h, "ace-000001-local"));
  CHECK(hostLabel("--ACE--01--", h, sizeof h) && !strcmp(h, "ACE-01"));
  CHECK(hostLabel("Caf\xc3\xa9 unit", h, sizeof h) && !strcmp(h, "Caf-unit"));
  CHECK(hostLabel("", h, sizeof h) == 0 && h[0] == 0);
  CHECK(hostLabel("___", h, sizeof h) == 0 && h[0] == 0);
  CHECK(hostLabel(nullptr, h, sizeof h) == 0 && h[0] == 0);
  // 63-char label limit, no trailing '-' after the cut.
  char longId[100]; memset(longId, 'A', 99); longId[99] = 0;
  CHECK(hostLabel(longId, h, sizeof h) == 63 && strlen(h) == 63);
  memset(longId, 'B', 62); longId[62] = ' '; longId[63] = 'C'; longId[64] = 0;
  CHECK(hostLabel(longId, h, sizeof h) == 62 && h[61] == 'B');
  // Small buffer cuts safely.
  char tiny[5]; CHECK(hostLabel("ACE-000001", tiny, sizeof tiny) == 3 && !strcmp(tiny, "ACE"));
  // MAC fallback only when nothing usable is left; same format as pc::gatewayName.
  CHECK(hostname("ACE-000001", mac, h, sizeof h) == 10 && !strcmp(h, "ACE-000001"));
  CHECK(hostname("!!!", mac, h, sizeof h) == 10 && !strcmp(h, "ACE-DDEEFF"));
  char gw[16]; gatewayName(mac, gw);
  CHECK(hostname(gw, mac, h, sizeof h) && !strcmp(h, gw));

  // Announcer: start once the station is on the home network, no repeat while steady.
  Announcer a;
  CHECK(a.decide(false, 0, "ACE-000001", 0) == Action::None);   // hotspot / no IP: nothing
  CHECK(a.decide(true, 1, "ACE-000001", 10) == Action::Start);
  a.started(true, "ACE-000001", 1, 10);
  CHECK(a.running && !strcmp(a.host, "ACE-000001"));
  CHECK(a.decide(true, 1, "ACE-000001", 20) == Action::None);
  CHECK(a.decide(true, 1, "ACE-000001", 999999) == Action::None);
  // Reconnect without the loop seeing the drop (new GOT_IP): restart once.
  CHECK(a.decide(true, 2, "ACE-000001", 30) == Action::Restart);
  a.started(true, "ACE-000001", 2, 30);
  CHECK(a.decide(true, 2, "ACE-000001", 40) == Action::None);
  // Rename (gw_name / factory serial): restart under the new name.
  CHECK(a.decide(true, 2, "ACE-000002", 50) == Action::Restart);
  a.started(true, "ACE-000002", 2, 50);
  // Station lost / hotspot-only / Wi-Fi test: stop once, then stay idle.
  CHECK(a.decide(false, 2, "ACE-000002", 60) == Action::Stop);
  a.stopped();
  CHECK(!a.running && a.host[0] == 0);
  CHECK(a.decide(false, 2, "ACE-000002", 70) == Action::None);
  // Back on the network: start again.
  CHECK(a.decide(true, 3, "ACE-000002", 80) == Action::Start);
  // begin failed: wait kRetryMs, unless a new GOT_IP arrives first.
  a.started(false, "ACE-000002", 3, 100);
  CHECK(!a.running && a.failed && a.host[0] == 0);
  CHECK(a.decide(true, 3, "ACE-000002", 100 + kRetryMs - 1) == Action::None);
  CHECK(a.decide(true, 3, "ACE-000002", 100 + kRetryMs) == Action::Start);
  CHECK(a.decide(true, 4, "ACE-000002", 101) == Action::Start);
  // millis() wrap does not block the retry forever.
  Announcer w; w.started(false, "ACE-000001", 1, 0xFFFFFF00u);
  CHECK(w.decide(true, 1, "ACE-000001", 0xFFFFFF00u + 10) == Action::None);
  CHECK(w.decide(true, 1, "ACE-000001", (uint32_t)(0xFFFFFF00u + kRetryMs)) == Action::Start);
  // Empty name never starts; a running announcer stops.
  Announcer e;
  CHECK(e.decide(true, 1, "", 0) == Action::None);
  CHECK(e.decide(true, 1, nullptr, 0) == Action::None);
  e.started(true, "ACE-000001", 1, 0);
  CHECK(e.decide(true, 1, "", 5) == Action::Stop);
}

static void test_factory_reset_scope() {
  using namespace pc::onb;
  // Customer state goes.
  const char *erased[] = {"pc_onb", "pc_offline", "registry", "forgotten", "sock_cfg", "sock_prio", "ac_out", "bat_set"};
  for (auto ns : erased) CHECK(factoryResetErasesNamespace(ns));
  const char *removed[] = {"wifi_ssid", "wifi_pass", "local_tok", "net_mode", "ap_pass", "label_pc", "gw_name"};
  for (auto k : removed) CHECK(factoryResetRemovesKey(k));
  // Factory identity, rating, calibration, OTA, battery protection and provisioning stay.
  const char *kept[] = {"pc_ident", "pc_cfg", "system_cfg", "pc_ota", "pwr_guard", "battery_alerts", "solar", "clock", "pc_secure", "pc_factory"};
  for (auto ns : kept) CHECK(!factoryResetErasesNamespace(ns));
  const char *keptKeys[] = {"api_key", "ota_url", "admin_tok", "espnow_keys", "locked", "migrated", "serial", "secret", "model", "rated"};
  for (auto k : keptKeys) CHECK(!factoryResetRemovesKey(k));
  CHECK(!factoryResetErasesNamespace(nullptr));
  CHECK(!factoryResetRemovesKey(nullptr));
  CHECK(kFactoryResetNamespaceCount == 8);
  CHECK(kFactoryResetUserKeyCount == 7);
}

static void test_ac_limit_events() {  // 2.5.13: Manual output power limit warning / shutdown events (DSP)
  using namespace ev;
  powercell::AcOutputPolicy pol; pol.setUsable(4500); pol.setCeiling(3000, powercell::AcMode::WarnAndShutdown);
  Engine g; Event e[24]; size_t n;
  auto step = [&](float w, uint32_t t) {
    powercell::AcWatts s; s.valid = true; s.watts = w; s.receivedAtMs = t; pol.update(s, t);
    Inputs in; in.acLimit = true; in.acWarn = pol.warning(); in.acShutdown = pol.shutdownLatched();
    in.acRatedShutdown = pol.systemOverloadLatched(); in.acWattsValid = true; in.acWatts = w;
    g.update(in, t, t / 1000);
    return g.drain(e, 24);
  };
  uint32_t t = 0;
  for (; t <= 4000; t += 1000) CHECK(step(1000, t) == 0);           // startup grace + normal load
  CHECK(step(3200, 5000) == 0 && step(3200, 6000) == 0 && step(3200, 7000) == 0);  // 3 s hold
  n = step(3200, 8000);
  CHECK(n == 1 && !strcmp(e[0].id, "ac_output_limit") && e[0].active && e[0].type == Type::Overload &&
        e[0].severity == Sev::Warning && !strcmp(e[0].code, "output_limit_w") && e[0].hasValue && e[0].value == 3200.0f &&
        !strcmp(typeName(e[0].type), "overload"));
  for (t = 9000; t < 13000; t += 1000) CHECK(step(3200, t) == 0);   // one-shot while over
  n = step(3200, 13000);                                             // 8 s: warn_and_shutdown latches
  CHECK(n == 1 && !strcmp(e[0].id, "ac_output_shutdown") && e[0].active && e[0].severity == Sev::Critical &&
        !strcmp(e[0].code, "output_limit_off") && !strcmp(e[0].releaseRequires, "load_below_limit_30s"));
  CHECK(step(3200, 14000) == 0);
  // Output off: load 0. Warning clears after the 10 s rearm + 10 s debounce, shutdown after 30 s.
  size_t clrWarn = 0, clrShut = 0; uint32_t warnAt = 0, shutAt = 0;
  for (t = 15000; t <= 60000; t += 1000) {
    n = step(0, t);
    for (size_t i = 0; i < n; ++i) {
      if (!strcmp(e[i].id, "ac_output_limit") && !e[i].active) { ++clrWarn; warnAt = t; }
      if (!strcmp(e[i].id, "ac_output_shutdown") && !e[i].active) { ++clrShut; shutAt = t; }
    }
  }
  CHECK(clrWarn == 1 && warnAt == 35000 && clrShut == 1 && shutAt == 45000);
  // Warn-only: warning event, no shutdown event.
  powercell::AcOutputPolicy wo; wo.setUsable(4500); wo.setCeiling(3000, powercell::AcMode::WarnOnly);
  Engine h; int warns = 0, shuts = 0;
  for (t = 0; t <= 40000; t += 1000) {
    float w = t < 4000 ? 500 : 3500;
    powercell::AcWatts s; s.valid = true; s.watts = w; s.receivedAtMs = t; wo.update(s, t);
    Inputs in; in.acLimit = true; in.acWarn = wo.warning(); in.acShutdown = wo.shutdownLatched(); in.acWattsValid = true; in.acWatts = w;
    h.update(in, t, 0); n = h.drain(e, 24);
    for (size_t i = 0; i < n; ++i) { warns += !strcmp(e[i].id, "ac_output_limit") && e[i].active; shuts += !strcmp(e[i].id, "ac_output_shutdown"); }
  }
  CHECK(warns == 1 && shuts == 0 && !wo.shutdownLatched());
  // Usable rating exceeded (any mode): rated_overload_off.
  powercell::AcOutputPolicy rp; rp.setUsable(4500); rp.setCeiling(0, powercell::AcMode::WarnOnly);
  Engine r; bool rated = false;
  for (t = 0; t <= 20000; t += 1000) {
    float w = t < 4000 ? 500 : 4800;
    powercell::AcWatts s; s.valid = true; s.watts = w; s.receivedAtMs = t; rp.update(s, t);
    Inputs in; in.acLimit = true; in.acWarn = rp.warning(); in.acShutdown = rp.shutdownLatched(); in.acRatedShutdown = rp.systemOverloadLatched();
    r.update(in, t, 0); n = r.drain(e, 24);
    for (size_t i = 0; i < n; ++i) if (!strcmp(e[i].id, "ac_output_shutdown") && e[i].active) rated = !strcmp(e[i].code, "rated_overload_off");
  }
  CHECK(rated);
  // STM images (acLimit false): never raised.
  Engine st; Inputs si; si.acWarn = true; si.acShutdown = true; st.update(si, 0, 0); st.update(si, 20000, 0); CHECK(st.drain(e, 24) == 0);
}

static void test_lan_ota() {  // local hotspot OTA (POST /ota), ported from collector 2.5.12
  using namespace pc::lanota;
  CHECK(compareVersion("2.5.12", "2.5.11") > 0); CHECK(compareVersion("2.5.9", "2.5.12") < 0);
  CHECK(compareVersion("2.5.12", "2.5.12") == 0); CHECK(compareVersion("3.0", "2.9.9") > 0); CHECK(compareVersion("2.5", "2.5.0") == 0);
  const char *pre = "POWERCELL_FW|collector|pc-dsp011-5kw|";
  // Other models' tags (3.5 kW DSP, STM, legacy esp32dev, socket) must not be taken for ours.
  std::string img = std::string(5000, '\xAA') + "POWERCELL_FW|collector|pc-dsp011-5kw|" + std::string(1, '\0') + "junk" +
      "POWERCELL_FW|collector|pc-dsp011-35kw|9.9.9|END" + "POWERCELL_FW|collector|pc-stm01-5kw|9.9.8|END" +
      "POWERCELL_FW|collector|esp32dev|9.9.7|END" + "POWERCELL_FW|socket|pc-dsp011-5kw|9.9.6|END" +
      "POWERCELL_FW|collector|pc-dsp011-5kw|2.5.14|END" + std::string(300, 'x');
  for (size_t chunk : {1u, 7u, 64u, 4096u}) {
    TagScanner sc(pre);
    for (size_t i = 0; i < img.size(); i += chunk) sc.feed(reinterpret_cast<const uint8_t *>(img.data()) + i, std::min(chunk, img.size() - i));
    CHECK(sc.found() && sc.version() == "2.5.14");
    CHECK(judge("2.5.13", sc, false) == Verdict::Ok);
    CHECK(judge("2.5.14", sc, false) == Verdict::SameVersion);
    CHECK(judge("2.5.14", sc, true) == Verdict::SameVersion);   // current rule: same version never installs
    CHECK(judge("2.5.16", sc, false) == Verdict::Downgrade);
    CHECK(judge("2.5.16", sc, true) == Verdict::Ok);           // ?force=1 allows an older version
  }
  // A 3.5 kW image on a 5 kW unit: no tag for this model.
  TagScanner other(pre); std::string s1 = "POWERCELL_FW|collector|pc-dsp011-35kw|2.5.14|END";
  other.feed(reinterpret_cast<const uint8_t *>(s1.data()), s1.size());
  CHECK(!other.found() && judge("2.5.13", other, true) == Verdict::NoTag && !strcmp(verdictName(Verdict::NoTag), "identity_or_model_mismatch"));
  TagScanner none(pre); std::string s2 = "POWERCELL_FW|socket|pc-dsp011-5kw|1.0.3|END"; none.feed(reinterpret_cast<const uint8_t *>(s2.data()), s2.size());
  CHECK(!none.found());
  TagScanner bad(pre); std::string s3 = std::string(pre) + "abc|END"; bad.feed(reinterpret_cast<const uint8_t *>(s3.data()), s3.size()); CHECK(!bad.found());
  std::string h(64, 'a'), H(64, 'A'); CHECK(hexEq(h.c_str(), H.c_str())); CHECK(!hexEq(h.c_str(), "ab")); std::string g(64, 'g'); CHECK(!hexEq(g.c_str(), g.c_str()));
  CHECK(!hexEq(nullptr, h.c_str()));
  // Confirm rule: hotspot_only 3 min of local health, otherwise the 30 s cloud rule unchanged.
  CHECK(confirmAfterMs(true, 30000) == 180000 && confirmAfterMs(false, 30000) == 30000);
  CHECK(healthyNow(true, false, 40000) && !healthyNow(true, true, 20000));
  CHECK(healthyNow(false, true, 1000) && !healthyNow(false, false, 100000));
  // The real image, if one was built: its tag is found for its own model only.
  if (FILE *f = fopen("../../.pio/build/esp32dev_dsp011_5kw/firmware.bin", "rb")) {
    TagScanner sr(pre), s35("POWERCELL_FW|collector|pc-dsp011-35kw|"); uint8_t b[1500]; size_t k;
    while ((k = fread(b, 1, sizeof b, f)) > 0) { sr.feed(b, k); s35.feed(b, k); }
    fclose(f);
    CHECK(sr.found() && sr.version() == "2.5.20" && !s35.found());
  }
}

static void test_stm_save_interval() {  // Caleb 12 Nov 2026: 5 min + 10 Wh, 15 min fallback
  using namespace powercell;
  CHECK(kStmSaveMinMs == 5u * 60u * 1000u && kStmSaveMaxMs == 15u * 60u * 1000u && kStmSaveWh == 10.0 && kStmSaveIdleWh == 0.5);
  // Worst-case loss at rated power: 5 min. 5 kW: 416.7 Wh, 3.5 kW: 291.7 Wh.
  NEAR(5000.0 * kStmSaveMinMs / 3600000.0, 416.67, 0.01);
  NEAR(3500.0 * kStmSaveMinMs / 3600000.0, 291.67, 0.01);
  // Worst-case writes per day: one per 5 min + one day change.
  CHECK(86400000u / kStmSaveMinMs + 1 == 289);
  auto run = [](float volts, float amps, uint32_t seconds, uint32_t *firstSaveS, int *saves) {
    StmEnergyEstimate e; e.restore(100, 0, 1000);  // a saved counter exists
    *firstSaveS = 0; *saves = 0;
    for (uint32_t i = 0; i <= seconds; ++i) {
      const uint32_t ms = 1000 + i * 1000;
      StmEstimateSample s; s.valid = true; s.volts = volts; s.amps = amps; s.mono_ms = ms; s.unix_s = 1700000000 + i; s.day_id = 100;
      const auto v = e.observe(s);
      if (v.persist) { e.ackPersist(ms); if (!*firstSaveS) *firstSaveS = i; ++*saves; }
    }
    return e;
  };
  uint32_t first = 0; int saves = 0;
  // Full load 5 kW (230 V, 27.2 A): 10 Wh after ~8 s, so the 5 min floor sets the pace.
  run(230, 27.2f, 3600, &first, &saves);
  CHECK(first == 299 && saves == 12);  // 5 min from boot (ms 0)  // 12 writes/hour = 288/day
  // 1 kW: the same 5 min.
  run(230, 5.44f, 3600, &first, &saves);
  CHECK(first == 299 && saves == 12);  // 5 min from boot (ms 0)
  // 50 W (230 V, 0.272 A): 10 Wh takes 12 min, so the save is at 12 min, not 5.
  run(230, 0.272f, 3600, &first, &saves);
  CHECK(first >= 719 && first <= 721 && saves == 5);
  // 3 W trickle: 0.5 Wh after 10 min, the 15 min fallback flushes it.
  {
    StmEnergyEstimate e; e.restore(100, 0, 1000); int n = 0; uint32_t at = 0;
    for (uint32_t i = 0; i <= 3600; ++i) {
      const uint32_t ms = 1000 + i * 1000;
      StmEstimateSample s; s.valid = true; s.volts = 230; s.amps = 0.0163f; s.mono_ms = ms; s.unix_s = 1700000000 + i; s.day_id = 100;
      if (e.observe(s).persist) { e.ackPersist(ms); ++n; if (!at) at = i; }
    }
    CHECK(at == 899 && n == 4);  // 3 W: about 0.75 Wh at 15 min from boot
  }
  // No energy: no write, ever.
  run(230, 0, 3600, &first, &saves);
  CHECK(saves == 0);
  // A day change still saves at once.
  {
    StmEnergyEstimate e; e.restore(100, 5, 1000);
    StmEstimateSample s; s.valid = true; s.volts = 230; s.amps = 1; s.mono_ms = 1000; s.unix_s = 1700000000; s.day_id = 100;
    CHECK(!e.observe(s).persist);
    s.mono_ms = 2000; s.unix_s += 1; s.day_id = 101;
    CHECK(e.observe(s).persist);
  }
}

static void test_ota_errors() {  // 2.5.18: specific OTA failure codes and the hello OTA host
  using namespace pc::otaerr;
  char b[48];
  const size_t slot = 0x1E0000;
  // Before any flash work.
  CHECK(classifyResponse(-1, -1, slot, 0) == Fail::ConnectFailed);      // DNS / TCP / TLS handshake
  CHECK(classifyResponse(-5, -1, slot, 0) == Fail::ConnectFailed);
  CHECK(classifyResponse(-11, -1, slot, 0) == Fail::Timeout);           // no response headers
  CHECK(classifyResponse(401, 30, slot, 0) == Fail::Http);
  CHECK(classifyResponse(404, -1, slot, 0) == Fail::Http);
  CHECK(classifyResponse(200, -1, slot, 0) == Fail::SizeMismatch);      // chunked / no Content-Length
  CHECK(classifyResponse(200, 100, slot, 0) == Fail::SizeMismatch);
  CHECK(classifyResponse(200, 1380144, slot, 1380000) == Fail::SizeMismatch);
  CHECK(classifyResponse(200, slot + 1, slot, 0) == Fail::TooLarge);
  CHECK(classifyResponse(200, slot + 1, slot, slot + 1) == Fail::TooLarge);
  CHECK(classifyResponse(200, 1380144, slot, 1380144) == Fail::None);
  CHECK(classifyResponse(200, 1380144, slot, 0) == Fail::None);
  CHECK(classifyResponse(200, slot, slot, 0) == Fail::None);
  // Strings reported in last_error.
  code(Fail::Http, 401, b, sizeof b); CHECK(!strcmp(b, "download_http_401"));
  code(Fail::Http, 503, b, sizeof b); CHECK(!strcmp(b, "download_http_503"));
  code(Fail::ConnectFailed, -1, b, sizeof b); CHECK(!strcmp(b, "download_connect_failed"));
  const struct { Fail f; const char *s; } names[] = {
    {Fail::NoSlot, "download_no_slot"}, {Fail::UrlInvalid, "download_url_invalid"}, {Fail::Timeout, "download_timeout"},
    {Fail::SizeMismatch, "download_size_mismatch"}, {Fail::TooLarge, "download_too_large"},
    {Fail::FlashBeginFailed, "download_flash_begin_failed"}, {Fail::Stalled, "download_stalled"},
    {Fail::ReadError, "download_read_error"}, {Fail::VerifyMagic, "verify_magic"}, {Fail::FlashWriteFailed, "flash_write_failed"},
    {Fail::VerifySize, "verify_size"}, {Fail::VerifyIdentity, "verify_identity"}, {Fail::VerifySha, "verify_sha"},
    {Fail::FinalizeFailed, "finalize_failed"}};
  for (const auto &n : names) { code(n.f, 0, b, sizeof b); CHECK(!strcmp(b, n.s)); CHECK(strlen(b) < 32); }
  code(Fail::None, 0, b, sizeof b); CHECK(b[0] == 0);
  { char tiny[8]; code(Fail::VerifyIdentity, 0, tiny, sizeof tiny); CHECK(strlen(tiny) == 7); }  // truncates, never overflows
  // The legacy catch-all is gone.
  for (const auto &n : names) CHECK(strcmp(name(n.f), "download_or_verification_failed") != 0);
  // After the body: loop errors win, then size, identity, sha.
  CHECK(classifyVerify(Fail::Stalled, 10, 100, false, false) == Fail::Stalled);
  CHECK(classifyVerify(Fail::VerifyMagic, 0, 100, false, false) == Fail::VerifyMagic);
  CHECK(classifyVerify(Fail::None, 99, 100, true, true) == Fail::VerifySize);
  CHECK(classifyVerify(Fail::None, 100, 100, false, true) == Fail::VerifyIdentity);
  CHECK(classifyVerify(Fail::None, 100, 100, true, false) == Fail::VerifySha);
  CHECK(classifyVerify(Fail::None, 100, 100, true, true) == Fail::None);
  // Hello ota_host: scheme://host[:port], nothing else.
  char h[96];
  CHECK(hostOf("https://api-dev.acecore.tech", h, sizeof h) && !strcmp(h, "https://api-dev.acecore.tech"));
  CHECK(hostOf("https://api-dev.acecore.tech/", h, sizeof h) && !strcmp(h, "https://api-dev.acecore.tech"));
  CHECK(hostOf("HTTPS://API.AceCore.Tech:8443/api/ota?key=secret#x", h, sizeof h) && !strcmp(h, "https://api.acecore.tech:8443"));
  CHECK(hostOf("https://user:pw@api.acecore.tech/x", h, sizeof h) && !strcmp(h, "https://api.acecore.tech"));
  CHECK(strstr(h, "pw") == nullptr && strstr(h, "user") == nullptr);
  CHECK(hostOf("https://api.acecore.tech?token=abc", h, sizeof h) && !strcmp(h, "https://api.acecore.tech"));
  CHECK(!hostOf("api.acecore.tech", h, sizeof h) && h[0] == 0);
  CHECK(!hostOf("https://", h, sizeof h));
  CHECK(!hostOf("https:///path", h, sizeof h));
  CHECK(!hostOf("https://bad host", h, sizeof h));
  CHECK(!hostOf(nullptr, h, sizeof h));
  { char s8[8]; CHECK(!hostOf("https://api-dev.acecore.tech", s8, sizeof s8) && s8[0] == 0); }
  // ota_url_override
  CHECK(!isOverride("https://api-dev.acecore.tech", "https://api-dev.acecore.tech"));
  CHECK(!isOverride("https://API-dev.acecore.tech/", "https://api-dev.acecore.tech"));
  CHECK(isOverride("https://api.acecore.tech", "https://api-dev.acecore.tech"));
  CHECK(isOverride("https://api-dev.acecore.tech:8443", "https://api-dev.acecore.tech"));
  CHECK(isOverride("https://api-dev.acecore.tech/v2", "https://api-dev.acecore.tech"));
}

int main() {
  test_live(); test_live_grid(); test_xfer4(); test_solar_view(); test_solar_energy(); test_app_power_kw();
  test_legacy_replies();
  test_time_policy(); test_ds3231(); test_ac_energy_today(); test_stm_estimate(); test_grid_sense(); test_dsp_thermal(); test_dsp_over_temp(); test_jbd(); test_dsp_bms_select(); test_v011_gpio(); test_dsp_pad_steps(); test_front_light_follows(); test_dsp_status_led(); test_pzem_route(); test_pzem_stats(); test_battery_cutoff(); test_sd_energy(); test_factory_rating();
  test_soc_guard(); test_rsv_parse(); test_critical_events();
  test_trip_lockout(); test_lockout_event();
  test_crc(); test_frame_roundtrip(); test_status_codec(); test_legacy_parser(); test_ring(); test_jk_bms(); test_identity(); test_mdns();
  test_ota_hardware_model();
  test_local_auth(); test_device_ids(); test_ota_url(); test_battery_policy(); test_power_policy(); test_ota_url_dev_http(); test_ota_gate(); test_heap_guard();
  test_sha256(); test_passcode(); test_limiter(); test_pairs_sessions(); test_wifi_ap_policy(); test_factory_reset_scope(); test_ws_rules();
  test_legacy_event_lines(); test_events_engine(); test_lora_proto(); test_socket_rules();
  test_payment_lock(); test_ac_limit_events(); test_lan_ota(); test_stm_save_interval(); test_ota_errors();
  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
