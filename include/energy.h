// include/energy.h
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load persisted energy from NVS — call once in setup(), after NVS init.
 * Returns the restored value in Wh (0.0 if no prior save exists).
 */
float energy_init(void);

/**
 * Accumulate discharge energy from a BMS sample.
 * Only accumulates when current_a < 0 (discharging).
 * Call from bmsTask after every valid parseDynamic().
 *
 * @param voltage_v   bmsData.packVoltage
 * @param current_a   bmsData.packCurrent  (negative = discharge on JK BMS)
 * @param interval_s  Frame interval in seconds (3.0 for your BMS)
 */
void energy_update(float voltage_v, float current_a, float interval_s);

/**
 * Periodic NVS save — call from bmsTask every N frames.
 * Handles wear-levelling cadence internally.
 */
void energy_save_if_due(void);

/** @return Total discharge energy accumulated in Wh */
float energy_get_wh(void);

/** @return Total discharge energy accumulated in kWh */
float energy_get_kwh(void);

/** @return Instantaneous discharge power in W (0 if not discharging) */
float energy_get_power_w(void);

/**
 * Reset accumulator to zero and erase NVS key.
 * Thread-safe — can be called from any task.
 */
void energy_reset(void);

#ifdef __cplusplus
}
#endif