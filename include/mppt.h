// mppt.h — ADS1115 Voltage + Current Sense
// PowerCell MPPT — PlatformIO / ESP32
//
// Channels:
//   A0/A1 (differential): 2 mΩ shunt  — current sense
//   A2    (single-ended): 200 kΩ / 3.3 kΩ voltage divider — up to ~200 V → 3.3 V
//
// FreeRTOS is built into the ESP32 Arduino framework; no extra library needed.
// Results are published to a shared MpptData struct protected by a mutex.

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ─── I2C pins ────────────────────────────────────────────────────────────────
// Default ESP32 I2C pins. Change if your board routes them differently.
constexpr int MPPT_I2C_SDA = 21;
constexpr int MPPT_I2C_SCL = 22;

// ─── Calibration constants ────────────────────────────────────────────────────
// Measure your actual resistors with a multimeter before adjusting.

constexpr float MPPT_DIVIDER_R_TOP  = 200000.0f;   // 200 kΩ (top arm)
constexpr float MPPT_DIVIDER_R_BOT  =   3300.0f;   // 3.3 kΩ (bottom arm)
constexpr float MPPT_DIVIDER_RATIO  = (MPPT_DIVIDER_R_TOP + MPPT_DIVIDER_R_BOT)
                                       / MPPT_DIVIDER_R_BOT;

// Software trim: if V_bus still reads high/low after resistor correction,
// adjust until Serial output matches your multimeter at a known voltage.
// e.g. code reads 100 V but meter reads 95 V → set 0.95f
constexpr float MPPT_VOLTAGE_CAL    = 1.0f;        // ← tune this last

constexpr float MPPT_SHUNT_OHMS     = 0.002f;      // 2 mΩ = 0.002 Ω

// ─── ADS1115 gain & LSB ───────────────────────────────────────────────────────
// A0/A1 diff  (shunt): max across 2 mΩ @ 100 A = 200 mV → GAIN_SIXTEEN (±0.256 V)
// A2 single   (divider): max ≈ 3.3 V               → GAIN_ONE     (±4.096 V)

constexpr adsGain_t MPPT_CURRENT_GAIN = GAIN_SIXTEEN;  // ±0.256 V → ~7.8125 µV/bit
constexpr adsGain_t MPPT_VOLTAGE_GAIN = GAIN_ONE;       // ±4.096 V → 125 µV/bit

constexpr float MPPT_LSB_SIXTEEN    = 0.256f  / 32768.0f;
constexpr float MPPT_LSB_ONE        = 4.096f  / 32768.0f;

// ─── Task tuning ──────────────────────────────────────────────────────────────
constexpr uint8_t     MPPT_SAMPLES         = 16;    // averages per reading
constexpr uint32_t    MPPT_TASK_PERIOD_MS  = 500;   // publish interval
constexpr uint32_t    MPPT_TASK_STACK      = 4096;  // bytes (ESP32 uses bytes, not words)
constexpr UBaseType_t MPPT_TASK_PRIO       = 1;     // low priority; raise if needed
constexpr int         MPPT_TASK_CORE       = 1;     // pin to APP_CPU (core 1); core 0 = WiFi/BT

// ─── Shared result type ───────────────────────────────────────────────────────

struct MpptData {
    float    v_adc_V;     // raw ADC voltage (use this to verify divider ratio)
    float    v_bus_V;     // reconstructed PV / bus voltage
    float    current_A;   // charge / load current
    float    power_W;     // computed power
    uint32_t timestamp;   // millis() at time of reading
};

// ─── Public API ───────────────────────────────────────────────────────────────

/**
 * @brief  Initialise I2C, ADS1115, mutex, and pin the sense task to core 1.
 *         Call once from setup() — no scheduler start needed on ESP32.
 * @return true if ADS1115 was found; false on I2C failure.
 */
bool mppt_init();

/**
 * @brief  Copy the latest MpptData snapshot in a thread-safe manner.
 * @param  out  Destination struct written atomically under the mutex.
 * @return true if a valid reading has been published at least once.
 */
bool mppt_get_data(MpptData &out);
static void print_task(void *pvParameters);