#pragma once
#include <Arduino.h>
#include "original_v01_profile.h"

/* JK BMS */
#define JK_UART Serial2
#define JK_BAUD 115200

//inverter UART
#define INVERTER_UART Serial1
#define INVERTER_BAUD ORIGINAL_V01_INVERTER_BAUD
#define INVERTER_RX ORIGINAL_V01_INVERTER_RX
#define INVERTER_TX ORIGINAL_V01_INVERTER_TX

/* MPPT SPI */
// #define MOSI 23
// #define MISO 19
// #define SCK 18
#define CS ORIGINAL_V01_MPPT_CS

/* LoRa */
#define LORA_RST ORIGINAL_V01_LORA_RST
#define LORA_CS ORIGINAL_V01_LORA_CS
#define LORA_DIO0 ORIGINAL_V01_LORA_DIO0
#define LORA_FREQUENCY  433E6
#define LORA_SYNC_WORD  0xF1   // must match the Smart Socket transmitters

/* GPIO */
#define RELAY ORIGINAL_V01_RELAY
#define RED ORIGINAL_V01_RED
#define YELLOW ORIGINAL_V01_YELLOW
#define GREEN ORIGINAL_V01_GREEN
#define AMBER ORIGINAL_V01_AMBER
#define LIGHTS ORIGINAL_V01_LIGHTS

/* Smart Socket*/
#define SOCKET_LED 4
#define SOCKET_RELAY 15
#define SOCKET_CURRENT 35
#define SOCKET_CS 5
#define SOCKET_RESET 25
#define SOCKET_DIO0 26
#define SOCKET_OVERCURRENT_THRESHOLD 50
