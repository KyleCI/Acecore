#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sha256.h"
// Smart-socket ESP-NOW wire format. Byte-for-byte the collector 1.0.4 / socket 1.0.1 "v1"
// packet (reference acecore-collector-now 2.zip, include/espnow_protocol.h): 44 bytes,
// HMAC-SHA256 (first 16 bytes) over everything before the signature, keyed with the
// fleet AUTH key. Unicast frames are also ESP-NOW encrypted (PMK/LMK). Keys are never in
// source: the collector reads them from NVS (SET espnow_keys), the socket from its own NVS.
//
// 2.5.36 adds two OPTIONAL packet types inside the same v1 frame (version byte stays 1):
//   Status (9)  socket -> collector: warn / lockout / tripped flags, last trip current,
//               user limit, socket firmware version and the last Control acknowledged.
//   Control (10) collector -> socket: reset_lockout or set_limit.
// Older firmware rejects unknown types in decode(), so an old collector ignores Status and
// an old socket ignores Control (the collector then reports the control as unsupported).
namespace pc {
namespace espnow {
enum Type : uint8_t {
  Beacon = 1, Discover = 2, Telemetry = 3, Command = 4, Pair = 5, PairAck = 6, Forget = 7, ForgetAck = 8,
  Status = 9, Control = 10,
};
constexpr uint8_t kMaxType = Control;
// Telemetry flags (v1): bit0 command rejected, bit1 overcurrent, bit2 restart reported,
// bits 4..7 restart reason. Discover flags: 0x04 pairing supported, 0x08 already paired.
constexpr uint8_t TelemetryRejected = 0x01;
constexpr uint8_t TelemetryOvercurrent = 0x02;
constexpr uint8_t RestartReported = 0x04;
constexpr uint8_t PairingSupported = 0x04;  // Discover packets only
constexpr uint8_t AlreadyPaired = 0x08;     // Discover packets only
// Status flags (2.5.36).
constexpr uint8_t StatusWarn = 0x01;      // current >= warn level (8 A at the 10 A limit)
constexpr uint8_t StatusLockout = 0x02;   // 3 trips in 5 min latched the relay off
constexpr uint8_t StatusTripped = 0x04;   // relay tripped, auto reclose pending
constexpr uint8_t StatusHasLimit = 0x08;  // info carries the user limit
// Control flags (2.5.36): exactly one of these.
constexpr uint8_t ControlResetLockout = 0x10;
constexpr uint8_t ControlSetLimit = 0x20;  // current = limit in amps (1..10)

inline uint8_t restartFlags(uint8_t reason) { return RestartReported | ((reason & 0x0f) << 4); }
inline int restartReason(uint8_t flags) { return flags & RestartReported ? (flags >> 4) : -1; }
constexpr uint32_t Magic = 0x5043454e;
constexpr uint8_t Broadcast[6] = {255, 255, 255, 255, 255, 255};

struct __attribute__((packed)) Packet {
  uint32_t magic = Magic;
  uint8_t version = 1;
  uint8_t type = 0;
  uint8_t relay = 0;
  uint8_t flags = 0;
  uint32_t session = 0;
  uint32_t sequence = 0;
  float current = 0;
  uint32_t ackSession = 0;   // Status: packed info (limit + firmware version), see packInfo()
  uint32_t ackSequence = 0;  // Status: last Control sequence handled (0 = none)
  uint8_t signature[16] = {};
};
static_assert(sizeof(Packet) == 44, "Wire packet must match on both devices");

inline void sign(Packet &p, const uint8_t key[16]) {
  uint8_t d[32];
  pc::hmacSha256(key, 16, &p, offsetof(Packet, signature), d);
  memcpy(p.signature, d, sizeof p.signature);
}
inline bool verify(const Packet &p, const uint8_t key[16]) {
  uint8_t d[32];
  pc::hmacSha256(key, 16, &p, offsetof(Packet, signature), d);
  uint8_t diff = 0;
  for (unsigned i = 0; i < sizeof p.signature; ++i) diff |= p.signature[i] ^ d[i];
  return diff == 0;
}
// Same acceptance rules as the 1.0.4 parser, plus the two 2.5.36 types.
inline bool decode(const uint8_t *bytes, int length, Packet &p, const uint8_t key[16]) {
  if (!bytes || length != (int)sizeof(Packet)) return false;
  memcpy(&p, bytes, sizeof p);
  return p.magic == Magic && p.version == 1 && p.type >= Beacon && p.type <= kMaxType && p.session != 0 &&
         p.relay <= 1 && isfinite(p.current) && p.current >= 0 && verify(p, key);
}
inline bool newer(uint32_t sequence, uint32_t previous) { return int32_t(sequence - previous) > 0; }
inline bool acceptsPair(bool paired, bool fromOwner, bool fromCandidate, uint32_t packetSession, uint32_t ownerSession,
                        uint32_t candidateSession, uint32_t echoedSocketSession, uint32_t socketSession, uint32_t beaconAge) {
  return echoedSocketSession == socketSession &&
         (paired ? fromOwner && packetSession == ownerSession
                 : fromCandidate && packetSession == candidateSession && beaconAge <= 5000);
}
inline bool acceptsForget(bool paired, bool fromOwner, bool fromReleasedOwner, uint32_t packetSession,
                          uint32_t ownerSession, uint32_t sequence, uint32_t lastCommand) {
  return paired ? fromOwner && packetSession == ownerSession && newer(sequence, lastCommand) : fromReleasedOwner;
}
inline bool macFromId(const char *id, uint8_t *mac) {
  if (!id || strlen(id) != 12) return false;
  for (unsigned i = 0; i < 6; ++i) {
    unsigned value = 0;
    for (unsigned n = 0; n < 2; ++n) {
      char c = id[2 * i + n];
      unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
      if (digit > 15) return false;
      value = value * 16 + digit;
    }
    mac[i] = value;
  }
  return !(mac[0] & 1);  // never a group (broadcast/multicast) address
}
inline void idFromMac(const uint8_t *mac, char out[13]) {
  snprintf(out, 13, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Status info word: [limit deciamps 8][fw major 8][fw minor 8][fw patch 8].
inline uint32_t packInfo(uint8_t limitDa, uint8_t major, uint8_t minor, uint8_t patch) {
  return (uint32_t(limitDa) << 24) | (uint32_t(major) << 16) | (uint32_t(minor) << 8) | patch;
}
inline uint8_t infoLimitDa(uint32_t info) { return uint8_t(info >> 24); }
// "1.1.0" style version; false (and 0.0.0) if it does not parse.
inline bool parseVersion(const char *s, uint8_t &maj, uint8_t &min, uint8_t &pat) {
  unsigned a = 0, b = 0, c = 0; char tail = 0;
  maj = min = pat = 0;
  if (!s || sscanf(s, "%u.%u.%u%c", &a, &b, &c, &tail) != 3 || a > 255 || b > 255 || c > 255) return false;
  maj = uint8_t(a); min = uint8_t(b); pat = uint8_t(c);
  return true;
}
inline void infoVersion(uint32_t info, char out[12]) {
  snprintf(out, 12, "%u.%u.%u", unsigned((info >> 16) & 0xff), unsigned((info >> 8) & 0xff), unsigned(info & 0xff));
}
// Set-limit amps carried in Control.current: 1..10 A, sent and stored as deciamps 10..100.
inline uint8_t clampLimitDa(int da) { return uint8_t(da < 10 ? 10 : da > 100 ? 100 : da); }
inline uint8_t limitDaFromAmps(float amps) {
  if (!isfinite(amps)) return 0;
  return clampLimitDa(int(lroundf(amps * 10.0f)));
}

// Encrypted-peer budget. ESP-IDF allows CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM = 7 encrypted
// peers (the broadcast peer is unencrypted and does not count). Every paired socket must stay an
// encrypted peer (its telemetry is encrypted unicast), so at most 6 sockets are paired and one
// slot is kept for pairing / forgetting.
constexpr unsigned kMaxEncryptedPeers = 7;
constexpr unsigned kMaxPairedSockets = kMaxEncryptedPeers - 1;
struct PeerEntry {
  bool present = false;      // currently an encrypted ESP-NOW peer
  bool registered = false;   // paired socket: never evicted
  bool lifecycle = false;    // pairing or forgetting in progress: never evicted
  uint32_t lastContact = 0;  // millis of the last authenticated packet
};
// Index of the peer to delete before adding a new one, -1 if there is room, -2 if every
// slot is held by a paired or pairing/forgetting socket (the new peer must not be added).
inline int pickPeerEviction(const PeerEntry *e, size_t n, uint32_t nowMs, unsigned cap = kMaxEncryptedPeers) {
  unsigned used = 0;
  for (size_t i = 0; i < n; ++i) used += e[i].present;
  if (used < cap) return -1;
  int victim = -2; uint32_t oldestAge = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!e[i].present || e[i].registered || e[i].lifecycle) continue;
    uint32_t age = nowMs - e[i].lastContact;
    if (victim < 0 || age > oldestAge) { victim = int(i); oldestAge = age; }
  }
  return victim;
}
}  // namespace espnow
}  // namespace pc
