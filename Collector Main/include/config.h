#pragma once

// ============================================================
//  config.h  — Compile-time constants, shared message types,
//              packed structs, and network configuration.
//              Must be byte-identical with socket firmware.
// ============================================================

#include <Arduino.h>

// ── Device / buffer limits ───────────────────────────────────────────────────
#define MAX_DEVICES           10
#define MAX_BATCH              4
#define MAX_PENDING            5

// ── Timing ──────────────────────────────────────────────────────────────────
#define RELAY_PENDING_TIMEOUT  5000   // ms — clear pending if no ACK after 5 s
#define OC_DISPLAY_TIMEOUT    10000   // ms — auto-expire OC banner if no new alert
#define HTTP_TIMEOUT_MS        3000

// ── Message types ────────────────────────────────────────────────────────────
#define MSG_TYPE_DISCOVERY    1
#define MSG_TYPE_DATA         2
#define MSG_TYPE_CONTROL      3
#define MSG_TYPE_ACK          4
#define MSG_TYPE_CHANNEL_INFO 5
#define MSG_TYPE_OVERCURRENT  6   // socket → collector: overcurrent alert

// ── Electrical assumption ────────────────────────────────────────────────────
#define ASSUMED_VOLTAGE  220.0f

// ── Network credentials ──────────────────────────────────────────────────────
const char* const routerSSID  = "Acecore_Starlink";
const char* const routerPASS  = "Acecore@2022";


// ── HTTP upload endpoint ─────────────────────────────────────────────────────
const char* const serverURL   = "http://api.acecore.tech/api/update-electrical-data/";
const char* const apiKey      = "13db0a2eda129aa67f8b2c60e175e1fd";

// Device identifier sent in every HTTP upload payload as "device_id".
const char* const deviceName  = "ACE2025";

// ── Shared packed message (must match socket firmware layout exactly) ─────────
#pragma pack(push, 1)
struct DataSample {
    float current;
    float voltage;
    bool  relayState;
};

typedef struct {
    uint8_t    msgType;
    uint16_t   msgID;
    uint8_t    deviceID;
    uint8_t    sampleCount;
    DataSample samples[MAX_BATCH];
    uint8_t    channel;
    uint16_t   slotTime;
    uint8_t    slotIndex;
    uint32_t   timestamp;
    uint8_t    relayCmd;
} SharedMessage;
#pragma pack(pop)
