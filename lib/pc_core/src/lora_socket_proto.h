#pragma once
#include <stddef.h>
#include <stdint.h>
// Smart-socket LoRa wire format (Dubem smart-socket-firmware, 433 MHz, sync 0xF1,
// SF9/BW125k/CR4-5/preamble 12/CRC on). Plain ASCII, comma separated:
//   socket -> gateway: "<ID12>,SOCKET CURRENT:<A>,SOCKET RELAY:<0|1>,FW:<ver>[,ACK:<cmdId>][,ALERT:OVERCURRENT|,ALERT:LOCKOUT,TRIP CURRENT:<A>][,WARN:CURRENT_HIGH][,LIM:<deciamps>]"
//   gateway -> socket: "<ID12>,RELAY_ON,<cmdId>" | "<ID12>,RELAY_OFF,<cmdId>" | "<ID12>,RESET_LOCKOUT,<cmdId>" | "<ID12>,SET_LIMIT,<deciamps>,<cmdId>"
// SET_LIMIT (socket config, Oct 22 2026): user limit 10..200 dA (1..20 A), clamped by the socket,
// stored in socket NVS and reported back as LIM. The 20 A hard trip is fixed; user-limit trips count
// toward the lockout.
// ALERT:OVERCURRENT = socket tripped its relay (>= 20 A). WARN:CURRENT_HIGH =
// >= 16 A, cleared below 15 A (proposed in 2.2; older sockets never send it).
// ALERT:LOCKOUT (2.2, Oct 22 2026) = 3 trips within a rolling 5 min latched the relay
// off (persisted in socket NVS); TRIP CURRENT is the last trip current in A. Cleared
// only by RESET_LOCKOUT (acked with ACK:<cmdId> and no ALERT:LOCKOUT) or the socket button.
namespace pc {
namespace lora {
constexpr float kTripAmps = 20.0f, kWarnAmps = 16.0f, kWarnClearAmps = 15.0f, kTripClearAmps = 18.0f;
struct SocketPacket {
  char id[13] = {};
  bool hasCurrent = false, hasRelay = false;
  float current = 0;
  int relay = 0;
  char fw[33] = {};
  char ack[48] = {};
  bool overcurrent = false, warning = false;
  bool lockout = false, hasTripCurrent = false;
  bool hasLimit = false; uint8_t limitDa = 0;  // LIM:<deciamps>, newer sockets only
  float tripCurrent = 0;  // last trip current (A), sent with ALERT:LOCKOUT
};
// True only for a well-formed telemetry packet (12-hex id, finite current >= 0, relay 0/1).
bool parseSocketPacket(const char *s, size_t n, SocketPacket &out);
bool validCommandId(const char *id);  // 1..47 chars of [A-Za-z0-9-], what the socket accepts
// Returns length written (0 if it does not fit or the inputs are invalid).
size_t buildRelayCommand(const char *id12, bool on, const char *cmdId, char *buf, size_t cap);
size_t buildResetLockoutCommand(const char *id12, const char *cmdId, char *buf, size_t cap);
size_t buildSetLimitCommand(const char *id12, uint8_t deciamps, const char *cmdId, char *buf, size_t cap);  // clamps 10..200
}  // namespace lora
}  // namespace pc
