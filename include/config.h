#pragma once
// Build-time configuration. Override any of these with -D in platformio.ini.
// Nothing secret lives here: credentials are provisioned into NVS at the
// factory (see README "Provisioning").

#ifndef POWERCELL_FIRMWARE_VERSION
#define POWERCELL_FIRMWARE_VERSION "2.5.0"
#endif
#ifndef POWERCELL_FIRMWARE_KIND
#define POWERCELL_FIRMWARE_KIND "collector"
#endif
#ifndef POWERCELL_HARDWARE_MODEL
#define POWERCELL_HARDWARE_MODEL "esp32dev"  // must match the backend OTA model string
#endif

// ---- Backend ---------------------------------------------------------------
#ifndef POWERCELL_WS_HOST
#define POWERCELL_WS_HOST "api.acecore.tech"
#endif
#ifndef POWERCELL_WS_PORT
#define POWERCELL_WS_PORT 443
#endif
#ifndef POWERCELL_WS_TLS
#define POWERCELL_WS_TLS 1
#endif
#ifndef POWERCELL_WS_PATH
#define POWERCELL_WS_PATH "/ws/device"
#endif
#ifndef POWERCELL_OTA_BASE_URL_DEFAULT
#define POWERCELL_OTA_BASE_URL_DEFAULT "https://api.acecore.tech"
#endif

// ---- Dev build guard ---------------------------------------------------------
// Cleartext ws:// and http:// OTA exist only for the bench env esp32dev_dev
// (POWERCELL_DEV_BUILD=1, POWERCELL_DEV_INSECURE=1). Every other env is TLS-only.
#ifndef POWERCELL_DEV_BUILD
#define POWERCELL_DEV_BUILD 0
#endif
#ifndef POWERCELL_DEV_INSECURE
#define POWERCELL_DEV_INSECURE 0
#endif
#if POWERCELL_DEV_INSECURE && !POWERCELL_DEV_BUILD
#error "POWERCELL_DEV_INSECURE is only allowed in the esp32dev_dev env"
#endif
#if !POWERCELL_WS_TLS && !POWERCELL_DEV_INSECURE
#error "Cleartext WebSocket (POWERCELL_WS_TLS=0) needs the dev env (POWERCELL_DEV_INSECURE=1)"
#endif
#define POWERCELL_OTA_ALLOW_HTTP POWERCELL_DEV_INSECURE

// ---- Feature flags ---------------------------------------------------------
// Fitted on every board (hardware VP, Dubem reference): JK-BMS on UART2,
// ADS1115 PV sensing on I2C, SX1278 LoRa. These are required parts; if one
// does not answer, the firmware logs it and reports its fields as null.
#ifndef FEATURE_LORA              // SX127x 433 MHz on the LoRa header (CS IO4, RST IO26, DIO0 not used)
#define FEATURE_LORA 1
#endif
#ifndef FEATURE_ESPNOW_SOCKETS    // legacy smart-socket path over ESP-NOW; disabled in 2.2 (sockets use LoRa)
#define FEATURE_ESPNOW_SOCKETS 0
#endif
#ifndef FEATURE_SD_PROBE          // optional SD card on CS2 (IO14), detected at boot
#define FEATURE_SD_PROBE 1
#endif
#ifndef FEATURE_BAT_SENSE_ADC     // IO34 BAT_SENSE (copy of STM32 PA3): last-resort pack voltage
#define FEATURE_BAT_SENSE_ADC 1
#endif
#ifndef FEATURE_STM32_NRST        // IO26 -> 1k -> STM32 NRST bodge. Not possible while LoRa uses IO26.
#define FEATURE_STM32_NRST 0
#endif
// Status LED mapping. The Dubem/1.0.x code drives RED=IO13, YELLOW=IO12; the
// schematic labels IO12 "R" and IO13 "Y". Keep the old-code mapping (1) until
// Caleb confirms on a board; set 0 for the schematic labels.
#ifndef LED_MAP_OLD_CODE
#define LED_MAP_OLD_CODE 1
#endif

// ---- JK-BMS (JK-PB series, 300-byte 55 AA EB 90 frames) ----------------------
#ifndef JK_BMS_BAUD
#define JK_BMS_BAUD 115200        // 8N1
#endif
#define JK_BMS_STALE_MS 10000     // no valid frame for this long = BMS offline, fields null
#define PACK_V_CROSSCHECK_TOL 1.5f  // V; BMS vs STM32 pack voltage disagreement flag

// ---- LoRa (must match the smart-socket transmitters) -------------------------
#define LORA_FREQUENCY 433E6
#define LORA_SYNC_WORD 0xF1
#define LORA_SF 9
#define LORA_BW 125000
#define LORA_CR 5
#define LORA_PREAMBLE 12
#ifndef FEATURE_LORA_SOCKETS      // smart sockets over LoRa (Dubem protocol); needs FEATURE_LORA
#define FEATURE_LORA_SOCKETS (FEATURE_LORA && !FEATURE_ESPNOW_SOCKETS)
#endif
#if FEATURE_LORA_SOCKETS && !FEATURE_LORA
#error "FEATURE_LORA_SOCKETS requires FEATURE_LORA"
#endif
#if FEATURE_LORA_SOCKETS && FEATURE_ESPNOW_SOCKETS
#error "pick one smart-socket transport"
#endif

// ---- STM32 link ------------------------------------------------------------
// 0 = legacy text lines at 9600 (current STM32 firmware)
// 1 = framed binary protocol at 115200 (agreed v1, once the STM32 side ships it)
#ifndef STM32_LINK_PROTOCOL
#define STM32_LINK_PROTOCOL 0
#endif
#define STM32_LEGACY_BAUD 9600
#define STM32_FRAMED_BAUD 115200
#define STM32_STALE_MS 6000          // status older than this is reported as stale
#define STM32_POLL_MS 1000           // framed mode: poll get_status if no push arrived
#define STM32_CMD_TIMEOUT_MS 200
#ifndef STM32_SET_MODE_TIMEOUT_MS
#define STM32_SET_MODE_TIMEOUT_MS 3000  // STM32 ACKs set_mode only once applied
#endif
#define STM32_CMD_RETRIES 3

// ---- Timings ---------------------------------------------------------------
#define TELEMETRY_PERIOD_MS 5000
#define TELEMETRY_LOW_POWER_MS 60000  // battery critical latched; the 30 s snapshot keeps the backend online window
#define BACKEND_ONLINE_WINDOW_S 45   // PR #6: online = a message within 45 s; telemetry must stay well under it
static_assert(TELEMETRY_PERIOD_MS < BACKEND_ONLINE_WINDOW_S * 1000 / 3, "telemetry interval too long for the backend online window");
#define SNAPSHOT_PERIOD_MS 30000
#define WIFI_CONNECT_TIMEOUT_MS 20000
#ifndef AP_FALLBACK_MIN
#define AP_FALLBACK_MIN 5             // reopen the hotspot after this many minutes without the router (2.1 decision)
#endif
#define WIFI_TEST_TIMEOUT_MS 20000    // /v2/wifi/test
#define NTP_LOG_PERIOD_MS 10000
#define OTA_HEALTHY_MS 30000          // continuous healthy time before an OTA image is marked valid
#define OTA_VERIFY_DEADLINE_MS 300000 // roll back if never healthy within 5 min of first boot
#define OTA_CHECK_PERIOD_MS 300000

// ---- Offline queue ---------------------------------------------------------
#define OFFLINE_QUEUE_CAPACITY 32
#define OFFLINE_BATCH_MAX 8
#define OFFLINE_CHECKPOINT_MS 30000   // routine events reach NVS at most every 30 s

#define WS_PING_MS 15000
#define WS_PONG_TIMEOUT_MS 10000
#define WS_PING_MISSES 2
#define LOOP_SLOW_MS 500              // log a main-loop pass longer than this (ws.loop starvation)
#define WS_MAX_MESSAGE 16384          // matches backend frame limit
