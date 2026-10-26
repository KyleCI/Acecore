// Host unit tests for lib/pc_core. Build and run: make -C test/host
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "crc.h"
#include "identity.h"
#include "jk_bms.h"
#include "power_policy.h"
#include "ota_gate.h"
#include "heap_guard.h"
#include "local_auth.h"
#include "ring_queue.h"
#include "sha256.h"
#include "onb_core.h"
#include "stm32_frame.h"
#include "stm32_legacy_parser.h"
#include "events_core.h"
#include "lora_socket_proto.h"
#include "../../smart-socket-firmware/src/trip_lockout.h"
#include "payment_lock.h"

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

// Live Upload cloud (Caleb 2026-10-26). Default esp32dev expects this host.
// Production api.acecore.tech is esp32dev_prod only, not the default base.
static const char *kLiveOtaBase = "https://api-dev.acecore.tech";

static void test_ota_url() {
  char p[200];
  const char *base = kLiveOtaBase;
  CHECK(otaDownloadPath(base, "https://api-dev.acecore.tech/api/ota/download?device_id=AABBCCDDEEFF", p, sizeof p) &&
        !strcmp(p, "/api/ota/download?device_id=AABBCCDDEEFF"));
  CHECK(otaDownloadPath("https://api-dev.acecore.tech/", "https://API-DEV.acecore.tech/api/firmware.bin", p, sizeof p) && !strcmp(p, "/api/firmware.bin"));
  CHECK(otaDownloadPath(base, "/api/firmware/download?device_id=ACE4447", p, sizeof p));
  CHECK(!otaDownloadPath(base, "https://evil.example/api/ota/download", p, sizeof p));
  CHECK(!otaDownloadPath(base, "https://api.acecore.tech/api/ota/download", p, sizeof p));  // prod host is not the live base
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

static void test_ota_url_prod_opt_in() {
  char p[200];
  const char *base = "https://api.acecore.tech";  // esp32dev_prod only
  CHECK(otaDownloadPath(base, "https://api.acecore.tech/api/ota/download?device_id=AABBCCDDEEFF", p, sizeof p) &&
        !strcmp(p, "/api/ota/download?device_id=AABBCCDDEEFF"));
  CHECK(!otaDownloadPath(base, "https://api-dev.acecore.tech/api/ota/download", p, sizeof p));
  CHECK(!otaDownloadPath(base, "http://api.acecore.tech/api/ota/download", p, sizeof p));
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
  BatteryAlertPolicy p;
  CHECK(p.update(true, 80, 0) == 0 && p.level() == BatteryLevel::None);
  CHECK(p.update(true, 19, 1000) == BatteryAlertPolicy::BatteryChanged && p.level() == BatteryLevel::Low);
  CHECK(p.update(true, 21, 2000) == 0 && p.level() == BatteryLevel::Low);     // hysteresis
  CHECK(p.update(true, 9, 3000) && p.level() == BatteryLevel::Critical);
  CHECK(p.update(true, 4, 4000) && p.level() == BatteryLevel::ShutdownRisk);
  CHECK(p.update(true, 7, 5000) == 0 && p.level() == BatteryLevel::ShutdownRisk);
  CHECK(p.update(true, 9, 6000) && p.level() == BatteryLevel::Critical);
  CHECK(p.update(true, 30, 7000) && p.level() == BatteryLevel::None);
  CHECK(p.update(false, 0, 8000) == 0 && p.available());
  CHECK(p.update(false, 0, 8000 + BatteryAlertPolicy::kUnavailableAfterMs) == BatteryAlertPolicy::AvailabilityChanged && !p.available());
  CHECK(p.update(true, 50, 30000) == BatteryAlertPolicy::AvailabilityChanged && p.available());
  CHECK(!strcmp(batteryLevelType(BatteryLevel::Critical), "battery_critical"));
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
  bi.fault = 0; bi.soc = 15; b.update(bi, 11000, 3); b.update(bi, 21100, 4); n = b.drain(e, 24);
  CHECK(countType(e, n, Type::BatteryFull, false) == 1 && countType(e, n, Type::BatteryLow, true) == 1);
  bi.soc = 8; b.update(bi, 22000, 5); n = b.drain(e, 24);
  CHECK(n == 1 && e[0].type == Type::BatteryLow && e[0].severity == Sev::Critical && !strcmp(e[0].code, "battery_critical"));
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
  CHECK(validTz(60) && validTz(-720) && validTz(840) && !validTz(841) && kTzDefault == 60);
  CHECK(!strcmp(verdictName(Verdict::NtpAuthoritative), "ntp_authoritative"));
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
  const char *p2 = "A1B2C3D4E5F6,SOCKET CURRENT:1.0,SOCKET RELAY:1,LIM:130";
  CHECK(parseSocketPacket(p2, strlen(p2), k) && !k.hasLimit);
}

static void test_heap_guard() {
  using namespace pc;
  TlsBackoff b;
  CHECK(b.check(0, 60000) == TlsBackoff::Go);
  CHECK(b.check(100, 40000) == TlsBackoff::WaitLog && b.delayMs() == 5000);
  CHECK(b.check(200, 90000) == TlsBackoff::Wait);           // still in backoff even if heap recovered
  CHECK(b.check(5100, 40000) == TlsBackoff::WaitLog && b.delayMs() == 10000);
  CHECK(b.check(15100, 40000) == TlsBackoff::WaitLog && b.delayMs() == 20000);
  CHECK(b.check(35100, 40000) == TlsBackoff::WaitLog && b.delayMs() == 40000);
  CHECK(b.check(75100, 40000) == TlsBackoff::WaitLog && b.delayMs() == 60000);
  CHECK(b.check(135100, 40000) == TlsBackoff::WaitLog && b.delayMs() == 60000);  // capped
  CHECK(b.check(195100, 40960) == TlsBackoff::Go && b.delayMs() == 0);
  CHECK(b.check(195200, 40959) == TlsBackoff::WaitLog && b.delayMs() == 5000);  // reset after success
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
}

int main() {
  test_legacy_replies();
  test_time_policy();
  test_soc_guard(); test_rsv_parse(); test_critical_events();
  test_trip_lockout(); test_lockout_event();
  test_crc(); test_frame_roundtrip(); test_status_codec(); test_legacy_parser(); test_ring(); test_jk_bms(); test_identity();
  test_local_auth(); test_device_ids(); test_ota_url(); test_ota_url_prod_opt_in(); test_battery_policy(); test_power_policy(); test_ota_url_dev_http(); test_ota_gate(); test_heap_guard();
  test_sha256(); test_passcode(); test_limiter(); test_pairs_sessions(); test_wifi_ap_policy(); test_ws_rules();
  test_legacy_event_lines(); test_events_engine(); test_lora_proto(); test_socket_rules();
  test_payment_lock();
  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
