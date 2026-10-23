// src/energy.cpp
#include "energy.h"
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ── NVS config ────────────────────────────────────────────────────────────────
static const char* NVS_NAMESPACE  = "powercell";
static const char* NVS_KEY_WHR    = "energy_wh";

// ── Save cadence — every 20 frames × 3s = ~60s ───────────────────────────────
static constexpr uint8_t SAVE_EVERY_N_FRAMES = 20;

// ── State ─────────────────────────────────────────────────────────────────────
static float              s_energy_wh   = 0.0f;
static float              s_power_w     = 0.0f;
static uint8_t            s_frame_count = 0;
static SemaphoreHandle_t  s_mutex       = NULL;
static Preferences        s_prefs;

// ── Internal helpers ──────────────────────────────────────────────────────────
static void nvs_save(void) {
    s_prefs.begin(NVS_NAMESPACE, false);        // false = read/write
    s_prefs.putFloat(NVS_KEY_WHR, s_energy_wh);
    s_prefs.end();
    Serial.printf("[Energy] NVS saved: %.4f Wh\n", s_energy_wh);
}

static void nvs_erase(void) {
    s_prefs.begin(NVS_NAMESPACE, false);
    s_prefs.remove(NVS_KEY_WHR);
    s_prefs.end();
    Serial.println("[Energy] NVS key erased.");
}

// ── Public API ────────────────────────────────────────────────────────────────

float energy_init(void) {
    s_mutex = xSemaphoreCreateMutex();

    s_prefs.begin(NVS_NAMESPACE, true);         // true = read-only
    s_energy_wh = s_prefs.getFloat(NVS_KEY_WHR, 0.0f);
    s_prefs.end();

    Serial.printf("[Energy] Restored from NVS: %.4f Wh (%.6f kWh)\n",
                  s_energy_wh, s_energy_wh / 1000.0f);
    return s_energy_wh;
}

void energy_update(float voltage_v, float current_a, float interval_s) {
    // JK BMS: negative current = discharge
    if (current_a >= 0.0f) {
        s_power_w = 0.0f;   // not discharging — clear instantaneous power
        return;
    }

    float discharge_a = -current_a;                         // make positive
    s_power_w         = voltage_v * discharge_a;            // W
    float delta_wh    = s_power_w * (interval_s / 3600.0f); // Wh

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_energy_wh += delta_wh;
        s_frame_count++;
        xSemaphoreGive(s_mutex);
    }
}

void energy_save_if_due(void) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        bool due = (s_frame_count >= SAVE_EVERY_N_FRAMES);
        if (due) s_frame_count = 0;
        xSemaphoreGive(s_mutex);

        if (due) nvs_save();
    }
}

float energy_get_wh(void) {
    float val = 0.0f;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        val = s_energy_wh;
        xSemaphoreGive(s_mutex);
    }
    return val;
}

float energy_get_kwh(void)     { return energy_get_wh() / 1000.0f; }

float energy_get_power_w(void) {
    float val = 0.0f;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        val = s_power_w;
        xSemaphoreGive(s_mutex);
    }
    return val;
}

void energy_reset(void) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_energy_wh   = 0.0f;
        s_power_w     = 0.0f;
        s_frame_count = 0;
        xSemaphoreGive(s_mutex);
    }
    nvs_erase();
    Serial.println("[Energy] Accumulator reset to zero.");
}