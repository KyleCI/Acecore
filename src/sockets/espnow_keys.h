#pragma once
#include <stdint.h>
// ESP-NOW link keys, loaded from NVS (settings "espnow_keys") at boot.
// They must match the keys provisioned into the smart sockets.
extern uint8_t ESPNOW_PMK[16];
extern uint8_t ESPNOW_LMK[16];
extern uint8_t ESPNOW_AUTH_KEY[16];
