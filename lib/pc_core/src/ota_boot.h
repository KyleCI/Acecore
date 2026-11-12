#pragma once
// 2.5.22: two-phase cloud OTA ("OTA on boot"). Host-testable: no Arduino types.
//
// Why: on ACE-000001 (2.5.20, 12 Nov 2026) the download handshake failed in mbedTLS with
// -0x7F00 (SSL_ALLOC_FAILED) 235 ms after the GET started, right after the WS was closed.
// Once the WS has been up for a while, the 8-bit DRAM heap is fragmented by what was
// allocated around the WS session (OTA worker stack, mDNS, JSON/String churn), so the two
// 16717 B record buffers no longer fit, although the old log said largest=40948. That
// 40948 B block is the IRAM heap (MALLOC_CAP_32BIT only, see heap_guard.h), which mbedTLS
// cannot use. The early-boot heap is the one the WS handshake always succeeds on, so the
// download is moved there:
//  1. An offer that passes every check, but finds no TLS room after the WS is released,
//     is stored in NVS (version, path, sha256, size, attempt count) and the unit reboots.
//  2. setup() runs the job right after Wi-Fi starts, before the web server, ESP-NOW and
//     the WS (and so before mDNS), on the same TLS client, CA bundle and checks.
//  3. Success: the job is cleared and the unit boots the new image (normal rollback rules).
//     Failure: last_status/last_error/last_error_detail as before, then normal boot; the
//     server re-offers and the job is staged again with the same attempt count.
//  4. The count is incremented in NVS before each attempt, so a crash or power cut also
//     counts. After kMaxTries attempts of the same sha256 the job is abandoned: that
//     sha256 is not staged again (a different sha256, a successful update or the console
//     OTA_RETRY command clears it), so a bad image or a dead link cannot boot-loop.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace pc {
namespace bootota {

constexpr uint8_t kMaxTries = 3;
constexpr uint32_t kWifiWaitMs = 40000;   // station got-IP after Wi-Fi start
constexpr uint32_t kClockWaitMs = 30000;  // SNTP for the signed request (same budget as the WS)
// install() caps: 15 s connect + 45 s handshake + 30 s headers, retried once on a 401
// time skew, + 180 s body. setup() gives up waiting (and reboots) after this.
constexpr uint32_t kRunCapMs = 360000;

// At boot, for a job found in NVS.
enum class Boot : uint8_t {
  None,     // no job
  Run,      // run it now (attempt tries+1)
  Abandon,  // kMaxTries attempts already started: give up on this sha256
  Discard,  // invalid, already running that version, or the running image is unconfirmed
};
inline Boot atBoot(bool set, bool valid, bool sameAsRunning, bool imageUnconfirmed, uint8_t tries) {
  if (!set) return Boot::None;
  if (!valid || sameAsRunning || imageUnconfirmed) return Boot::Discard;
  if (tries >= kMaxTries) return Boot::Abandon;
  return Boot::Run;
}

// When an offer arrives. Checked before the WS is released, so an abandoned image does
// not pause the WS (or turn the output off) every check period.
inline bool offerAbandoned(const char *offerSha, const char *abandonedSha) {
  return offerSha && abandonedSha && *abandonedSha && strcasecmp(offerSha, abandonedSha) == 0;
}

// After the WS is released: install now only if the strict TLS heap gate passes.
enum class Route : uint8_t { InPlace, Stage };
inline Route route(bool heapFitsNow) { return heapFitsNow ? Route::InPlace : Route::Stage; }

// Attempt count carried into a (re)staged job: kept for the same sha256, reset for another.
inline uint8_t stageTries(const char *prevSha, uint8_t prevTries, const char *newSha) {
  if (!prevSha || !newSha || !*prevSha || strcasecmp(prevSha, newSha) != 0) return 0;
  return prevTries;
}
// Staging this sha256 again would exceed the cap: abandon instead of rebooting.
inline bool stageWouldExceed(uint8_t carriedTries) { return carriedTries >= kMaxTries; }

// mbedTLS 2.28 allocation failures (the handshake ran out of heap, not a bad server/cert).
// High-level: SSL -0x7F00, X509 -0x2880, PK -0x3F80, ECP -0x4D80, CIPHER -0x6180,
// MD -0x5180, PEM -0x1180, DHM -0x3400. Low-level (may be added to a high-level code):
// MPI -0x0010, ASN1 -0x006A. -0x2700 (X509 verify failed) is not counted: it is also a
// genuine certificate failure.
inline bool isTlsMemoryError(int e) {
  if (e >= 0) return false;
  const unsigned a = static_cast<unsigned>(-e), hi = a & ~0x7Fu, lo = a & 0x7Fu;
  switch (hi) {
    case 0x7F00: case 0x2880: case 0x3F80: case 0x4D80: case 0x6180: case 0x5180: case 0x1180: case 0x3400: return true;
    default: break;
  }
  return lo == 0x10 || lo == 0x6A;
}
// An in-place download that failed only for lack of TLS heap is retried on the boot path.
inline bool inPlaceFallback(bool connectFailed, int tlsErr) { return connectFailed && isTlsMemoryError(tlsErr); }

// " heap free=<8-bit free> largest=<8-bit largest> min=<8-bit min since boot>" appended to
// a failure detail (bounded; never overflows `out`).
inline void appendHeap(char *out, size_t cap, uint32_t freeB, uint32_t largest, uint32_t minB) {
  if (!out || !cap) return;
  size_t n = strnlen(out, cap);
  if (n + 1 >= cap) return;
  snprintf(out + n, cap - n, " heap free=%lu largest=%lu min=%lu", (unsigned long)freeB, (unsigned long)largest, (unsigned long)minB);
}
// " boot=<attempt>/<kMaxTries>[ abandoned]" appended to a boot-path failure detail.
inline void appendBoot(char *out, size_t cap, uint8_t attempt, bool abandoned) {
  if (!out || !cap) return;
  size_t n = strnlen(out, cap);
  if (n + 1 >= cap) return;
  snprintf(out + n, cap - n, " boot=%u/%u%s", (unsigned)attempt, (unsigned)kMaxTries, abandoned ? " abandoned" : "");
}

// Job fields as stored in NVS. valid() repeats the offer checks that do not need the
// device (the URL/host check and the model are re-done by the caller).
struct Job {
  char version[33] = {0};
  char path[192] = {0};
  char sha[65] = {0};
  uint32_t size = 0;
  uint8_t tries = 0;
};
inline bool hex64(const char *s) {
  if (!s || strlen(s) != 64) return false;
  for (const char *p = s; *p; ++p) {
    char c = *p;
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}
inline bool valid(const Job &j, uint32_t slotSize) {
  size_t vl = strnlen(j.version, sizeof j.version);
  if (vl == 0 || vl > 32) return false;
  if (!hex64(j.sha)) return false;
  if (j.path[0] != '/' || strnlen(j.path, sizeof j.path) >= sizeof j.path) return false;
  if (j.size && slotSize && j.size > slotSize) return false;
  return true;
}

// ---- 2.5.24: one offer check for every staging path ----
// ACE-000001 (2.5.22, 12 Nov 2026): ota_available 2.5.23 arrived with heap_largest=14836, so
// the 16384 B "ota" worker stack could not be allocated; service() dropped the offer without
// a log every check period and the boot path never ran. The main loop now stages such an
// offer itself. handleOffer() (worker) and the main-loop path use this same rule set, in the
// order handleOffer() always applied it.
enum class Offer : uint8_t { Ok, NoVersion, VersionTooLong, SameVersion, BadSha, BadPath, TooBig, Abandoned };
// version: offered; running: POWERCELL_FIRMWARE_VERSION; sha: lower-cased offer sha256;
// pathOk: pc::otaDownloadPath accepted the url (or the own-host fallback); size: offered size
// (0 = unknown); slotSize: next OTA partition size (0 = unknown, size not checked).
inline Offer checkOffer(const char *version, const char *running, const char *sha, bool pathOk, uint32_t size, uint32_t slotSize,
                        const char *abandonedSha) {
  const size_t vl = version ? strlen(version) : 0;
  if (vl == 0) return Offer::NoVersion;
  if (vl > 32) return Offer::VersionTooLong;
  if (running && strcmp(version, running) == 0) return Offer::SameVersion;
  if (!hex64(sha)) return Offer::BadSha;
  if (!pathOk) return Offer::BadPath;
  if (size && slotSize && size > slotSize) return Offer::TooBig;
  if (offerAbandoned(sha, abandonedSha)) return Offer::Abandoned;
  return Offer::Ok;
}
inline const char *offerName(Offer o) {
  switch (o) {
    case Offer::Ok: return "ok";
    case Offer::NoVersion: return "no_version";
    case Offer::VersionTooLong: return "version_too_long";
    case Offer::SameVersion: return "same_version";
    case Offer::BadSha: return "sha256_invalid";
    case Offer::BadPath: return "url_invalid";
    case Offer::TooBig: return "size_exceeds_slot";
    case Offer::Abandoned: return "abandoned";
  }
  return "?";
}

// The OTA worker task could not be created (no heap block for its stack).
//  WS offer that passes checkOffer: stage it to the boot path from the main loop (the in-place
//  gate needs a 35 KB block anyway, so a heap that cannot give the stack cannot give TLS).
//  Anything else (HTTPS check, rejected offer): log and drop; the next check period retries.
enum class TaskFail : uint8_t { Stage, Drop };
inline TaskFail onTaskCreateFailed(bool wsOffer, Offer verdict) {
  return (wsOffer && verdict == Offer::Ok) ? TaskFail::Stage : TaskFail::Drop;
}
// last_error_detail of a job staged because the worker stack did not fit:
// "in_place_gate=task_stack stack=<bytes>" + appendHeap().
inline void taskStackDetail(char *out, size_t cap, uint32_t stack, uint32_t freeB, uint32_t largest, uint32_t minB) {
  if (!out || !cap) return;
  snprintf(out, cap, "in_place_gate=task_stack stack=%lu", (unsigned long)stack);
  appendHeap(out, cap, freeB, largest, minB);
}

}  // namespace bootota
}  // namespace pc
