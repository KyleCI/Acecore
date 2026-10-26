#include "prefs_safe.h"
#include "stm32_link.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "config.h"
#include "log.h"
#include "pins.h"
#include "stm32_legacy_parser.h"

namespace stm32link {
namespace {
using pc::Stm32Status;
namespace fr = pc::frame;

HardwareSerial &uart = Serial1;
Protocol g_proto = Protocol::LegacyText;
SemaphoreHandle_t g_stateMx = nullptr;   // protects g_status / g_stats
SemaphoreHandle_t g_cmdMx = nullptr;     // one command in flight
QueueHandle_t g_respQ = nullptr;         // response frames for the waiting command
Stm32Status g_status;
bool g_have = false;
uint32_t g_statusAt = 0;
Stats g_stats;
uint8_t g_seq = 0;
volatile int g_waitType = -1;            // response type the command is waiting for
volatile uint8_t g_waitSeq = 0;

pc::LegacyTextParser g_text;
uint32_t g_replySeenSeq = 0;
char g_reply[pc::LegacyTextParser::kMaxLine + 1] = {};
volatile uint32_t g_replySeq = 0;     // published copy (under g_stateMx)
String g_ver;
fr::Decoder g_dec;

void publish(const Stm32Status &s) {
  xSemaphoreTake(g_stateMx, portMAX_DELAY);
  Stm32Status prev = g_status;
  bool had = g_have;
  g_status = s;
  g_have = true;
  g_statusAt = millis();
  ++g_stats.records;
  xSemaphoreGive(g_stateMx);
  if (!had || prev.sys != s.sys) LOGI("STM32", "STATE", "sys=%s sw=%s", pc::sysStateName(s.sys), pc::switchName(s.sw));
  if (s.fault && (!had || prev.fault != s.fault)) LOGW("STM32", "FAULT", "bits=0x%08lX", (unsigned long)s.fault);
}

void onFrame(const fr::Frame &f) {
  if (f.type == fr::STATUS_PUSH) {
    Stm32Status s;
    if (fr::decodeStatus(f.payload, f.len, s)) publish(s);
  } else if (f.type == (fr::GET_STATUS | fr::kResponseBit) && f.len > 1 && f.payload[0] == fr::ST_OK) {
    Stm32Status s;  // response payload: STATUS byte, then the status block
    if (fr::decodeStatus(f.payload + 1, f.len - 1, s)) publish(s);
  }
  if (g_waitType >= 0 && f.type == g_waitType && f.seq == g_waitSeq) xQueueOverwrite(g_respQ, &f);
}

void rxTask(void *) {
  uint32_t lastByte = millis();
  for (;;) {
    int avail = uart.available();
    if (avail <= 0) {
      if (g_proto == Protocol::LegacyText && millis() - lastByte > 300) {
        Stm32Status s;
        if (g_text.flushIdle(s)) publish(s);
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    uint32_t now = millis();
    lastByte = now;
    while (avail-- > 0) {
      int c = uart.read();
      if (c < 0) break;
      ++g_stats.bytes;
      if (g_proto == Protocol::LegacyText) {
        Stm32Status s;
        if (g_text.feed(static_cast<uint8_t>(c), s)) publish(s);
        if (g_text.replySeq() != g_replySeenSeq) {
          g_replySeenSeq = g_text.replySeq();
          xSemaphoreTake(g_stateMx, portMAX_DELAY);
          strlcpy(g_reply, g_text.reply(), sizeof g_reply);
          ++g_replySeq;
          xSemaphoreGive(g_stateMx);
        }
      } else {
        static fr::Frame f;  // 260 B, keep it off the task stack
        if (g_dec.feed(static_cast<uint8_t>(c), now, f)) onFrame(f);
      }
    }
    g_stats.crcErrors = g_dec.crcErrors();
    g_stats.versionErrors = g_dec.versionErrors();
    g_stats.timeouts = g_dec.timeouts();
    g_stats.lineOverflows = g_text.overflows();
  }
}

void pollTask(void *) {
  // Framed mode: if no status push arrived for a while, poll for one.
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(STM32_POLL_MS));
    uint32_t age;
    Stm32Status s;
    if (status(s, &age) && age < STM32_POLL_MS + 200) continue;
    fr::Frame resp;
    transact(fr::GET_STATUS, nullptr, 0, resp, STM32_CMD_TIMEOUT_MS, 1);
  }
}
}  // namespace

const char *resultName(Result r) {
  switch (r) { case Result::Ok: return "ok"; case Result::Nack: return "nack"; case Result::Timeout: return "timeout";
    case Result::NotSupported: return "not_supported"; case Result::Busy: return "busy"; case Result::BadState: return "bad_state";
    default: return "bad_response"; }
}

void begin() {
  g_stateMx = xSemaphoreCreateMutex();
  g_cmdMx = xSemaphoreCreateMutex();
  g_respQ = xQueueCreate(1, sizeof(fr::Frame));
  Preferences p;
  int proto = STM32_LINK_PROTOCOL;
  if (prefsOpenRO(p, "pc_cfg")) { proto = p.getUChar("stm32_proto", STM32_LINK_PROTOCOL); p.end(); }
  g_proto = proto == 1 ? Protocol::Framed : Protocol::LegacyText;
  uint32_t baud = g_proto == Protocol::Framed ? STM32_FRAMED_BAUD : STM32_LEGACY_BAUD;
  uart.setRxBufferSize(1024);
  uart.begin(baud, SERIAL_8N1, PIN_STM32_RX, PIN_STM32_TX);  // single begin; no flow control
#if FEATURE_STM32_NRST
  digitalWrite(PIN_STM32_NRST, HIGH);
  pinMode(PIN_STM32_NRST, OUTPUT_OPEN_DRAIN);  // released (high-Z through open drain)
#endif
  xTaskCreatePinnedToCore(rxTask, "stm32_rx", 4096, nullptr, 3, nullptr, 1);
  if (g_proto == Protocol::Framed) xTaskCreatePinnedToCore(pollTask, "stm32_poll", 3072, nullptr, 1, nullptr, 1);
  LOGI("STM32", "LINK", "protocol=%s baud=%lu rx=%d tx=%d", g_proto == Protocol::Framed ? "framed" : "legacy_text",
       (unsigned long)baud, PIN_STM32_RX, PIN_STM32_TX);
}

Protocol protocol() { return g_proto; }

bool status(Stm32Status &out, uint32_t *ageMs) {
  if (!g_stateMx) return false;
  xSemaphoreTake(g_stateMx, portMAX_DELAY);
  bool have = g_have;
  out = g_status;
  if (ageMs) *ageMs = millis() - g_statusAt;
  xSemaphoreGive(g_stateMx);
  return have;
}

bool online() { Stm32Status s; uint32_t age; return status(s, &age) && age < STM32_STALE_MS; }

Stats stats() {
  Stats s;
  if (!g_stateMx) return s;
  xSemaphoreTake(g_stateMx, portMAX_DELAY); s = g_stats; xSemaphoreGive(g_stateMx);
  return s;
}

Result transact(uint8_t type, const uint8_t *payload, size_t len, fr::Frame &resp, uint32_t timeoutMs, uint8_t retries) {
  if (g_proto != Protocol::Framed) return Result::NotSupported;
  if (xSemaphoreTake(g_cmdMx, pdMS_TO_TICKS(timeoutMs * (retries + 1))) != pdTRUE) return Result::Busy;
  static uint8_t buf[fr::kMaxPayload + fr::kOverhead];
  Result r = Result::Timeout;
  uint8_t seq = ++g_seq;  // same seq on retries so the STM32 can drop duplicates
  size_t n = fr::encode(type, seq, payload, len, buf, sizeof buf);
  if (!n) { xSemaphoreGive(g_cmdMx); return Result::BadResponse; }
  xQueueReset(g_respQ);
  g_waitSeq = seq;
  g_waitType = type | fr::kResponseBit;
  for (uint8_t attempt = 0; attempt <= retries; ++attempt) {
    uart.write(buf, n);
    if (xQueueReceive(g_respQ, &resp, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
      r = (resp.len >= 1 && resp.payload[0] == fr::ST_OK) ? Result::Ok : Result::Nack;
      break;
    }
  }
  g_waitType = -1;
  if (r == Result::Timeout) { xSemaphoreTake(g_stateMx, portMAX_DELAY); ++g_stats.cmdTimeouts; xSemaphoreGive(g_stateMx); }
  xSemaphoreGive(g_cmdMx);
  return r;
}

Result setMode(fr::Mode mode, uint8_t *nack) {
  uint8_t pl[1] = {static_cast<uint8_t>(mode)};
  fr::Frame resp;
  // The STM32 ACKs only once the mode is actually applied (relay/transfer done),
  // so allow a longer wait. It NACKs with ST_BAD_STATE while a fault is latched:
  // that is reported as Result::BadState (clear with FAULT_RESET first).
  Result r = transact(fr::SET_MODE, pl, 1, resp, STM32_SET_MODE_TIMEOUT_MS, 1);
  uint8_t code = (r == Result::Nack && resp.len) ? resp.payload[0] : fr::ST_UNKNOWN;
  if (r == Result::Nack && code == fr::ST_BAD_STATE) r = Result::BadState;
  if (r != Result::Ok && r != Result::Timeout && nack) *nack = code;
  if (r == Result::Ok) { fr::Frame st; transact(fr::GET_STATUS, nullptr, 0, st, STM32_CMD_TIMEOUT_MS, 0); }
  LOGI("STM32", "SET_MODE", "mode=%u result=%s code=0x%02X", mode, resultName(r), code);
  return r;
}

Result faultReset() {
  fr::Frame resp;
  Result r = transact(fr::FAULT_RESET, nullptr, 0, resp);
  LOGI("STM32", "FAULT_RESET", "result=%s", resultName(r));
  return r;
}

Result getVersion(uint32_t &fw, uint8_t &proto) {
  fr::Frame resp;
  Result r = transact(fr::GET_VERSION, nullptr, 0, resp);
  if (r != Result::Ok) return r;
  if (resp.len < 6) return Result::BadResponse;  // status, u8 proto, u32 fw_version
  proto = resp.payload[1];
  fw = fr::getU32(resp.payload + 2);
  return r;
}

Result enterBootloader() {
  Stm32Status s;
  uint32_t age;
  if (!status(s, &age) || age > STM32_STALE_MS || s.sys != pc::SysState::Idle) return Result::BadState;
  uint8_t pl[4];
  fr::putU32(pl, fr::kBootloaderMagic);
  fr::Frame resp;
  Result r = transact(fr::ENTER_BOOTLOADER, pl, 4, resp);
  LOGW("STM32", "ENTER_BOOTLOADER", "result=%s", resultName(r));
  return r;
}

bool pulseReset() {
#if FEATURE_STM32_NRST
  digitalWrite(PIN_STM32_NRST, LOW);
  delay(10);
  digitalWrite(PIN_STM32_NRST, HIGH);  // open drain: releases the line
  LOGW("STM32", "NRST", "pulsed");
  return true;
#else
  return false;
#endif
}

Result textQuery(const char *cmd, const char *replyPrefix, char *out, size_t n, uint32_t timeoutMs) {
  if (out && n) out[0] = 0;
  if (g_proto != Protocol::LegacyText) return Result::NotSupported;
  if (!cmd || strlen(cmd) > 46) return Result::BadResponse;
  // Read-only queries only: calibration writes are a station decision, never automatic.
  if (strcmp(cmd, "VER?") && strcmp(cmd, "CAL?")) return Result::NotSupported;
  if (!g_cmdMx || xSemaphoreTake(g_cmdMx, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return Result::Busy;
  xSemaphoreTake(g_stateMx, portMAX_DELAY);
  uint32_t seq0 = g_replySeq;
  xSemaphoreGive(g_stateMx);
  uart.write(reinterpret_cast<const uint8_t *>(cmd), strlen(cmd));
  uart.write('\n');
  Result r = Result::Timeout;
  size_t pl = strlen(replyPrefix);
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(10));
    char line[sizeof g_reply] = {};
    bool fresh = false;
    xSemaphoreTake(g_stateMx, portMAX_DELAY);
    if (g_replySeq != seq0) { seq0 = g_replySeq; strlcpy(line, g_reply, sizeof line); fresh = true; }
    xSemaphoreGive(g_stateMx);
    if (!fresh) continue;
    if (!strncmp(line, "ERR", 3)) { if (out) strlcpy(out, line, n); r = Result::Nack; break; }
    if (!strncmp(line, replyPrefix, pl)) { if (out) strlcpy(out, line, n); r = Result::Ok; break; }
  }
  if (r == Result::Timeout) ++g_stats.cmdTimeouts;
  if (r == Result::Ok && !strcmp(cmd, "VER?") && out) g_ver = String(out + 4);
  xSemaphoreGive(g_cmdMx);
  return r;
}

bool parseCal(const char *s, CalInfo &o) {
  o = CalInfo();
  if (!s || strncmp(s, "OK CAL", 6)) return false;
  auto num = [s](const char *k, float &v) { const char *p = strstr(s, k); if (!p) return false; v = strtof(p + strlen(k), nullptr); return true; };
  auto flag = [s](const char *k, bool &v) { const char *p = strstr(s, k); if (!p) return false; p += strlen(k); if (*p != '0' && *p != '1') return false; v = *p == '1'; return true; };
  o.valid = num(" scale=", o.scale) && num(" default=", o.def) && flag(" stored=", o.stored) && flag(" active=", o.active) && flag(" pending=", o.pending);
  return o.valid;
}

String lastVersion() { return g_ver; }
}  // namespace stm32link
