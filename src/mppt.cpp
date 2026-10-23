// mppt.cpp — ADS1115 Voltage + Current Sense (background task)
// PowerCell MPPT — PlatformIO / ESP32

#include "housekeeping.h"


// ─── Module-private state ─────────────────────────────────────────────────────

static Adafruit_ADS1115  s_ads;
static MpptData          s_data        = {};
static bool              s_data_valid  = false;
static SemaphoreHandle_t s_mutex       = nullptr;
static TaskHandle_t      s_task_handle = nullptr;

// ─── Private helpers ──────────────────────────────────────────────────────────

/**
 * Average n differential A0–A1 readings.
 * ADS1115 @ 128 SPS → ~7.8 ms per conversion; vTaskDelay(1) between samples
 * yields the CPU rather than busy-waiting with delayMicroseconds.
 */
static int32_t read_diff01_avg(uint8_t n = MPPT_SAMPLES) {
    int32_t sum = 0;
    for (uint8_t i = 0; i < n; i++) {
        sum += s_ads.readADC_Differential_0_1();
        vTaskDelay(pdMS_TO_TICKS(1));   // yield; ~8 ms per sample anyway
    }
    return sum / n;
}

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "mppt.h"
 
// ─── Print task (core 1, low priority) ───────────────────────────────────────
 
static void print_task(void *pvParameters) {
    (void)pvParameters;
 
    Serial.println("───────────────────────────────────────────────────────────────────────");
    Serial.println("  V_adc (V)  |  V_bus (V)  |  I_shunt (A)  |  Power (W)  |  Age (ms)");
    Serial.println("───────────────────────────────────────────────────────────────────────");
 
    MpptData d;
    for (;;) {
        if (mppt_get_data(d)) {
            uint32_t age = millis() - d.timestamp;
            Serial.printf("  %7.4f V  |  %8.2f V  |  %10.3f A  |  %9.2f W  |  %4u ms\n",
                          d.v_adc_V, d.v_bus_V, d.current_A, d.power_W, age);
        } else {
            Serial.println("  [waiting for first reading…]");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


/**
 * Average n single-ended readings on the given channel.
 */
static int32_t read_single_avg(uint8_t channel, uint8_t n = MPPT_SAMPLES) {
    int32_t sum = 0;
    for (uint8_t i = 0; i < n; i++) {
        sum += s_ads.readADC_SingleEnded(channel);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return sum / n;
}

// ─── FreeRTOS task (pinned to core 1) ────────────────────────────────────────

static void mppt_sense_task(void *pvParameters) {
    (void)pvParameters;

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xPeriod = pdMS_TO_TICKS(MPPT_TASK_PERIOD_MS);

    for (;;) {
        // ── 1. Current — A0/A1 differential ──────────────────────────────
        s_ads.setGain(MPPT_CURRENT_GAIN);
        int32_t raw_i   = read_diff01_avg();
        float   shunt_v = (float)raw_i * MPPT_LSB_SIXTEEN;  // V across shunt
        float   current = shunt_v / MPPT_SHUNT_OHMS;        // I = V / R (A)

        // ── 2. Voltage — A2 single-ended ─────────────────────────────────
        s_ads.setGain(MPPT_VOLTAGE_GAIN);
        int32_t raw_v = read_single_avg(2);
        float   v_adc = (float)raw_v * MPPT_LSB_ONE;        // V at ADS pin
        float   v_bus = v_adc * MPPT_DIVIDER_RATIO * MPPT_VOLTAGE_CAL;

        // ── 3. Power ──────────────────────────────────────────────────────
        float power = v_bus * current;

        // ── 4. Publish under mutex ────────────────────────────────────────
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s_data.v_adc_V   = v_adc;
            s_data.v_bus_V   = v_bus;
            s_data.current_A = current;
            s_data.power_W   = power;
            s_data.timestamp = millis();
            s_data_valid     = true;
            xSemaphoreGive(s_mutex);
        }

        // ── 5. Sleep until next period (no drift) ────────────────────────
        vTaskDelayUntil(&xLastWakeTime, xPeriod);
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

bool mppt_init() {
    // Create mutex first — task may be scheduled before init() returns
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        Serial.println("[MPPT] ERROR: failed to create mutex.");
        return false;
    }

    // Initialise I2C with explicit pins (ESP32 requires this)
    Wire.begin(MPPT_I2C_SDA, MPPT_I2C_SCL);

    if (!s_ads.begin()) {
        Serial.println("[MPPT] ERROR: ADS1115 not found — check I2C wiring.");
        return false;
    }

    s_ads.setDataRate(RATE_ADS1115_128SPS);

    Serial.println("[MPPT] ADS1115 initialised.");
    Serial.printf("[MPPT]   SDA / SCL      : GPIO%d / GPIO%d\n", MPPT_I2C_SDA, MPPT_I2C_SCL);
    Serial.printf("[MPPT]   Divider ratio  : %.4f\n", MPPT_DIVIDER_RATIO);
    Serial.printf("[MPPT]   Voltage CAL    : %.4f\n", MPPT_VOLTAGE_CAL);
    Serial.printf("[MPPT]   Shunt (Ω)      : %.5f\n", MPPT_SHUNT_OHMS);
    Serial.printf("[MPPT]   Task core      : %d\n",   MPPT_TASK_CORE);

    // xTaskCreatePinnedToCore — ESP32-specific; pins task to a fixed core.
    // Core 0 handles WiFi/BT; core 1 is APP_CPU — better for deterministic sensing.
    BaseType_t ret = xTaskCreatePinnedToCore(
        mppt_sense_task,    // task function
        "mppt_sense",       // name (for debugging / uxTaskGetSystemState)
        MPPT_TASK_STACK,    // stack in bytes on ESP32
        nullptr,            // parameters
        MPPT_TASK_PRIO,     // priority
        &s_task_handle,     // handle (optional, kept for future suspend/resume)
        MPPT_TASK_CORE      // core affinity
    );

    if (ret != pdPASS) {
        Serial.println("[MPPT] ERROR: failed to create sense task.");
        return false;
    }

    Serial.println("[MPPT] Sense task started.");
    return true;
}

bool mppt_get_data(MpptData &out) {
    if (s_mutex == nullptr) return false;

    bool valid = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        out   = s_data;
        valid = s_data_valid;
        xSemaphoreGive(s_mutex);
    }
    return valid;
}