#pragma once 
#include <Arduino.h>

/* JK BMS */
#define JK_UART Serial2
#define JK_BAUD 115200

//inverter UART
#define INVERTER_UART Serial1
#define INVERTER_BAUD 9600
#define INVERTER_RX 33
#define INVERTER_TX 2

/* MPPT SPI */
// #define MOSI 23
// #define MISO 19
// #define SCK 18
#define CS 5

/* LoRa */
#define LORA_RST 26
#define LORA_CS 4
#define LORA_DIO0 2
#define LORA_FREQUENCY  433E6
#define LORA_SYNC_WORD  0xF1   // must match the Smart Socket transmitters

/* GPIO */
#define RELAY 15
#define RED 13
#define YELLOW 12
#define GREEN 27
#define AMBER 32
#define LIGHTS 25

/* Smart Socket*/
#define SOCKET_LED 4
#define SOCKET_RELAY 15
#define SOCKET_CURRENT 35
#define SOCKET_CS 5
#define SOCKET_RESET 25
#define SOCKET_DIO0 26
#define SOCKET_OVERCURRENT_THRESHOLD 50
