#pragma once
#include <Arduino.h>

// v15 factory identity (NVS namespace "pc_ident"). Survives FACTORY_RESET and the
// account-deactivation reset; only FACTORY_WIPE ALL (bench/rework) clears it.
//  serial    "ACE-000123", written by the factory station (console SERIAL)
//  secret    32 bytes from esp_fill_random on the first factory boot; reported to the
//            station once over serial (console SECRET) and never logged. v17: also the
//            HMAC key for passcode fingerprint (legacy NVS "dev_secret" migrated/cleared).
//  ap_pass   label hotspot password (restored by a factory reset)
//  rst_*     reset_pending flag + event fields, last reset time, recent remote cmd_ids
//  pay_*     HQ payment_lock (v16): locked flag, reason, last issued_at, recent cmd_ids
//            Survives as-bought factory reset so a customer cannot unlock by resetting.
namespace fid {
void begin();                      // called by settings::begin() before the ids are built
String serial();                   // "" when the station has not written one
String serialDigits();             // "000123" or ""
bool validSerial(const String &s); // ACE-NNNNNN
bool setSerial(const String &s);

bool hasSecret();
bool secretReported();
bool takeSecretOnce(char hex[65]); // hex once (unlocked, not yet reported), then false forever
bool setSecretHex(const String &hex64);  // rework/rotate from the station
bool rotateSecret();               // new random secret, reported flag cleared (unlocked only)
String legacySecretHex();          // x-device-secret during migration ("" if no secret)
bool hmac(const uint8_t *msg, size_t n, uint8_t out[32]);  // HMAC-SHA256 keyed with the raw secret

String apPass();                   // "" if none stored yet
bool setApPass(const String &p);

struct ResetInfo { bool pending = false; String source, cmdId; uint32_t at = 0; };
void markReset(const char *source, const String &cmdId, uint32_t resetAt, uint32_t lastAt);
ResetInfo resetInfo();
void clearResetPending();
uint32_t lastResetAt();
bool cmdSeen(const String &cmdId);

// HQ payment_lock (survives FACTORY_RESET / FACTORY_WIPE; cleared by unlock cmd or PAYMENT_UNLOCK)
bool paymentLocked();
String paymentLockReason();
uint32_t lastPaymentLockAt();
bool paymentCmdSeen(const String &cmdId);
// Apply a verified payment_lock: updates locked/reason/last/cmd ring. Returns false on NVS failure.
bool applyPaymentLock(bool locked, const String &reason, const String &cmdId, uint32_t issuedAt);
// Bench-only clear (console PAYMENT_UNLOCK). Does not touch the cmd ring.
bool clearPaymentLock();
}  // namespace fid
