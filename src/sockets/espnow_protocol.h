// Vendored from collector 1.0.4 (include/espnow_protocol.h). Wire format
// unchanged so socket firmware 1.0.1 keeps working; keys now come from NVS.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <mbedtls/md.h>
#include "espnow_keys.h"

namespace SocketLink {
enum Type : uint8_t { Beacon = 1, Discover = 2, Telemetry = 3, Command = 4, Pair = 5, PairAck = 6, Forget = 7, ForgetAck = 8 };
// Keep the v1 packet size: old firmware ignores these optional flag bits.
constexpr uint8_t RestartReported = 0x04;
constexpr uint8_t PairingSupported = 0x04; // Discovery packets only.
constexpr uint8_t AlreadyPaired = 0x08;
inline uint8_t restartFlags(uint8_t reason) { return RestartReported | ((reason & 0x0f) << 4); }
inline int restartReason(uint8_t flags) { return flags & RestartReported ? (flags >> 4) : -1; }
constexpr uint32_t Magic = 0x5043454e;
constexpr uint8_t Broadcast[6] = {255,255,255,255,255,255};
struct __attribute__((packed)) Packet {
  uint32_t magic = Magic;
  uint8_t version = 1;
  uint8_t type = 0;
  uint8_t relay = 0;
  uint8_t flags = 0;
  uint32_t session = 0;
  uint32_t sequence = 0;
  float current = 0;
  uint32_t ackSession = 0;
  uint32_t ackSequence = 0;
  uint8_t signature[16] = {};
};
static_assert(sizeof(Packet) == 44, "Wire packet must match on both devices");
inline bool authenticate(Packet &packet, bool verify) {
  uint8_t digest[32];
  const auto *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md_hmac(info, ESPNOW_AUTH_KEY, sizeof(ESPNOW_AUTH_KEY),
      reinterpret_cast<const uint8_t *>(&packet), offsetof(Packet, signature), digest)) return false;
  if (!verify) { memcpy(packet.signature, digest, sizeof(packet.signature)); return true; }
  uint8_t difference = 0;
  for (unsigned i=0; i<sizeof(packet.signature); ++i) difference |= packet.signature[i] ^ digest[i];
  return difference == 0;
}
inline bool decode(const uint8_t *bytes, int length, Packet &packet) {
  if (!bytes || length != sizeof(Packet)) return false;
  memcpy(&packet, bytes, sizeof(packet));
  return packet.magic == Magic && packet.version == 1 && packet.type >= Beacon
      && packet.type <= ForgetAck && packet.session != 0 && packet.relay <= 1
      && isfinite(packet.current) && packet.current >= 0 && authenticate(packet, true);
}
inline bool newer(uint32_t sequence, uint32_t previous) {
  return int32_t(sequence - previous) > 0;
}
inline bool acceptsPair(bool paired, bool fromOwner, bool fromCandidate,
                        uint32_t packetSession, uint32_t ownerSession, uint32_t candidateSession,
                        uint32_t echoedSocketSession, uint32_t socketSession, uint32_t beaconAge) {
  return echoedSocketSession == socketSession &&
      (paired ? fromOwner && packetSession == ownerSession
              : fromCandidate && packetSession == candidateSession && beaconAge <= 5000);
}
inline bool acceptsForget(bool paired, bool fromOwner, bool fromReleasedOwner,
                          uint32_t packetSession, uint32_t ownerSession,
                          uint32_t sequence, uint32_t lastCommand) {
  return paired ? fromOwner && packetSession == ownerSession && newer(sequence, lastCommand)
                : fromReleasedOwner;
}
inline bool macFromId(const char *id, uint8_t *mac) {
  if (!id || strlen(id) != 12) return false;
  for (unsigned i=0; i<6; ++i) {
    unsigned value = 0;
    for (unsigned n=0; n<2; ++n) {
      char c = id[2*i+n];
      unsigned digit = c >= '0' && c <= '9' ? c-'0' : c >= 'A' && c <= 'F' ? c-'A'+10 : c >= 'a' && c <= 'f' ? c-'a'+10 : 16;
      if (digit > 15) return false;
      value = value*16 + digit;
    }
    mac[i] = value;
  }
  return !(mac[0] & 1);
}
}
