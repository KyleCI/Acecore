#include <housekeeping.h>

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
        InverterSnapshot snap = getInverterStatus();
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
        doc["inverterLoad"]  = snap.vout * snap.iout;
        doc["inverter_energy"]       = energy_get_wh();
        doc["inverter_temperature"]  = snap.tempC;
        doc["inverterVoltage"] = snap.vout;
        doc["gridVoltage"] = snap.gridVoltage;

        // Solar Panel Section
        doc["solarVoltage"] = mppt.v_bus_V;
        doc["solarCurrent"] = mppt.current_A;
        doc["solarPower"] = mppt.power_W;
        
        // ── Smart Sockets section -- one entry per registered device ─────────
        JsonArray sockets = doc["sockets"].to<JsonArray>();
        populateDeviceArray(sockets);

        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response); });
}