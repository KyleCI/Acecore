#include <housekeeping.h>
#include "power_units.h"
#ifdef POWERCELL_HW_V011
#include "hw_v011.h"
#endif

// ── TODO: wire these to your actual inverter/PZEM reading code ──────────────
static float getInverterEnergy() { return 0.0f; }      // e.g. from PZEM
static float getInverterTemperature() { return 0.0f; } // e.g. from an NTC/ADC
static bool getDeviceState() { return true; }          // relay/on-off state

// ── Last-known-good battery values ───────────────────────────────────────────
// Held over whenever getBmsData() has no fresh frame, so the dashboard never
// drops to null/0 just because a UART read hasn't landed yet.
static float lastGoodBatteryVoltage = 0.0f;
static float lastGoodBatteryPercentage = 0.0f;
static float lastGoodBatteryTemperature = 0.0f;
static bool haveEverSeenBmsData = false;

void registerMetricsRoute(AsyncWebServer &server)
{
    server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest *request)
              {
        JsonDocument doc;   // ArduinoJson v7 -- no manual capacity needed

        // ── Battery section -- sourced from the BMS task ─────────────────────
        JKPacketData bms;
#ifndef POWERCELL_HW_V011
        InverterSnapshot snap = getInverterStatus();
#endif
        MpptData mppt;
        mppt_get_data(mppt);
        float outputLoad = 0.0f;

        if (getBmsData(bms)) {
            lastGoodBatteryVoltage    = bms.packVoltage;
            lastGoodBatteryPercentage = bms.SOC;
            lastGoodBatteryTemperature = bms.tempBat1;
            haveEverSeenBmsData       = true;
            // outputLoad                = bms.packPower;
        }
        // else: fall through and reuse the last cached values below.
        // outputLoad stays 0.0f on a stale read since there's no "last good"
        // power figure requested -- say the word if you want that cached too.

        if (haveEverSeenBmsData) {
            doc["battery_voltage"]    = lastGoodBatteryVoltage;
            doc["battery_percentage"] = lastGoodBatteryPercentage;
            doc["battery_temperature"] = lastGoodBatteryTemperature;
        } else {
            // No BMS frame has ever arrived (e.g. just booted) -- nothing
            // "good" to hold onto yet, so this is the only case still null.
            doc["battery_voltage"]    = nullptr;
            doc["battery_percentage"] = nullptr;
            doc["battery_temperature"] = nullptr;
        }
        // ── Device / powercell section ───────────────────────────────────────
        doc["device_state"]          = getDeviceState();
        doc["inverter_energy"]       = energy_get_wh();
#ifdef POWERCELL_HW_V011
        hwV011AppendTelemetry(doc);
#else
        powercell::OutputSample sample;
        const bool seen = snap.vout != 0.f || snap.iout != 0.f;
        sample.voltage_present = seen;
        sample.volts = snap.vout;
        sample.current_present = seen;
        sample.amps = snap.iout;
        sample.active_present = false;
        const powercell::NormalizedOutput normalized = powercell::normalize_output(sample);
        if (normalized.have_apparent) {
            doc["inverterLoad"] = normalized.apparent_va;
            doc["output_power"] = normalized.apparent_va;
            doc["output_power_quantity"] = "apparent_va";
        } else {
            doc["inverterLoad"] = nullptr;
        }
        doc["output_power_w"] = nullptr;
        doc["inverter_temperature"]  = snap.tempC;
        doc["inverterVoltage"] = snap.vout;
        doc["gridVoltage"] = snap.gridVoltage;
#endif

        // Solar Panel Section
        doc["solarVoltage"] = mppt.v_bus_V;
        doc["solarCurrent"] = mppt.current_A;
        if (powercell::power_publishable(mppt.power_W)) {
            doc["solarPower"] = mppt.power_W;
            doc["solar_power_w"] = mppt.power_W;
        } else {
            doc["solarPower"] = nullptr;
            doc["solar_power_w"] = nullptr;
        }
        
        // ── Smart Sockets section -- one entry per registered device ─────────
        JsonArray sockets = doc["sockets"].to<JsonArray>();
        populateDeviceArray(sockets);

        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response); });
}