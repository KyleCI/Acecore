#include "lora.h"
#include <SPI.h>
#include <LoRa.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <string.h>
#include <map>

// SPI bus pins for the RFM95W — fall back to ESP32 VSPI defaults if your
// pin defines header doesn't provide them.
#ifndef LORA_SCK
#define LORA_SCK 18
#endif
#ifndef LORA_MISO
#define LORA_MISO 19
#endif
#ifndef LORA_MOSI
#define LORA_MOSI 23
#endif

// Radio settings — same values as the working duplex test. These MUST match
// the Smart Socket firmware exactly. Override in the pin defines if needed.
#ifndef LORA_BANDWIDTH
#define LORA_BANDWIDTH 125E3
#endif
#ifndef LORA_SF
#define LORA_SF 7
#endif
#ifndef LORA_CR
#define LORA_CR 5          // 4/5
#endif
#ifndef LORA_PREAMBLE
#define LORA_PREAMBLE 8
#endif
#ifndef LORA_TX_POWER
#define LORA_TX_POWER 17   // dBm, PA_BOOST (RFM95W has no RFO output)
#endif

// ---------------------------------------------------------------------------
// Globals declared extern in lora.h
// ---------------------------------------------------------------------------
SemaphoreHandle_t dataMutex;
SemaphoreHandle_t loraMutex;
TaskHandle_t loraTaskHandle;

static Preferences registry; // NVS namespace "registry" holds known device IDs as keys

// Latest reading per registered device, keyed by deviceId. Only devices that
// are both registered AND have sent at least one telemetry packet appear
// here — access only while holding dataMutex.
static std::map<String, loraPacket> deviceCache;

// ---------------------------------------------------------------------------
// loraInit — bring up the RFM95W and create the mutexes used elsewhere.
//
// DIO0 is not wired, so -1 is passed to setPins(). Everything in this file
// uses the library's polling path (parsePacket / blocking endPacket), which
// reads the IRQ flags over SPI and doesn't need DIO0.
// LORA_FREQUENCY must be 868E6 or 915E6 for the RFM95W.
// ---------------------------------------------------------------------------
void loraInit() {
    dataMutex = xSemaphoreCreateMutex();
    loraMutex = xSemaphoreCreateMutex();

    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    LoRa.setSPI(SPI);
    LoRa.setPins(LORA_CS, LORA_RST, -1); // DIO0 not connected

    if (!LoRa.begin(LORA_FREQUENCY)) {
        Serial.println("[LoRa] RFM95W init failed - check wiring/frequency");
        return;
    }

    // Same configuration as the duplex test sketch
    LoRa.setSignalBandwidth(LORA_BANDWIDTH);
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setPreambleLength(LORA_PREAMBLE);
    LoRa.setSyncWord(LORA_SYNC_WORD);
    LoRa.setTxPower(LORA_TX_POWER);
    LoRa.enableCrc();
    Serial.println("[LoRa] RFM95W initialized (no DIO0, polling mode)");
}

// ---------------------------------------------------------------------------
// checkRegistered — true if deviceId exists as a key in the "registry" NVS
// namespace.
// ---------------------------------------------------------------------------
bool checkRegistered(String deviceId) {
    registry.begin("registry", true); // read-only
    bool isRegistered = registry.isKey(deviceId.c_str());
    registry.end();
    return isRegistered;
}

// ---------------------------------------------------------------------------
// registerDevice / unregisterDevice — add or remove a device ID from the
// "registry" NVS namespace. unregisterDevice also erases the device's
// cached reading immediately, so the next /metrics poll drops it rather
// than waiting for it to simply go quiet.
// ---------------------------------------------------------------------------
void registerDevice(String deviceId) {
    if (deviceId.length() == 0) {
        Serial.println("[LoRa] registerDevice called with empty deviceId");
        return;
    }
    registry.begin("registry", false); // read-write
    registry.putBool(deviceId.c_str(), true);
    registry.end();
    Serial.printf("[LoRa] Registered device: %s\n", deviceId.c_str());
}

void unregisterDevice(String deviceId) {
    if (deviceId.length() == 0) {
        Serial.println("[LoRa] unregisterDevice called with empty deviceId");
        return;
    }
    registry.begin("registry", false);
    registry.remove(deviceId.c_str());
    registry.end();

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        deviceCache.erase(deviceId);
        xSemaphoreGive(dataMutex);
    }

    Serial.printf("[LoRa] Unregistered device: %s\n", deviceId.c_str());
}

// ---------------------------------------------------------------------------
// handleSerialProvisioning — quick way to register/unregister devices
// without building a UI first. Call this once per loop() iteration on the
// Collector. deviceId is the socket's own MAC with colons stripped (it
// prints this at boot, e.g. "Device ID (MAC): 246F28AE9C1A") — copy that
// into the Serial monitor on the Collector:
//   REGISTER:246F28AE9C1A
//   UNREGISTER:246F28AE9C1A
// ---------------------------------------------------------------------------
void handleSerialProvisioning() {
    if (!Serial.available()) {
        return;
    }

    String line = Serial.readStringUntil('\n');
    line.trim();

    if (line.startsWith("REGISTER:")) {
        registerDevice(line.substring(strlen("REGISTER:")));
    } else if (line.startsWith("UNREGISTER:")) {
        unregisterDevice(line.substring(strlen("UNREGISTER:")));
    }
}

// ---------------------------------------------------------------------------
// parsePacket — decodes the Smart Socket wire format:
//   "<deviceId>,SOCKET CURRENT:<float>,SOCKET RELAY:<0|1>[,ALERT:OVERCURRENT]"
//   "<deviceId>,Power Restored:"
//
// current/relayState are left at -1 (sentinel) when the packet carries no
// telemetry (e.g. a "Power Restored" event), so loraTask can tell an
// event-only message apart from an actual reading.
// ---------------------------------------------------------------------------
loraPacket parsePacket() {
    loraPacket pkt;
    pkt.deviceId = "";
    pkt.current = -1.0f;
    pkt.relayState = -1;
    pkt.rssi = 0;

    String incoming = "";
    while (LoRa.available()) {
        incoming += (char)LoRa.read();
    }
    pkt.rssi = LoRa.packetRssi();

    int firstComma = incoming.indexOf(',');
    if (firstComma == -1) {
        Serial.print("[LoRa] Malformed packet (no device ID): ");
        Serial.println(incoming);
        return pkt;
    }

    pkt.deviceId = incoming.substring(0, firstComma);
    String rest = incoming.substring(firstComma + 1);

    int currentIdx = rest.indexOf("SOCKET CURRENT:");
    if (currentIdx != -1) {
        int valueStart = currentIdx + strlen("SOCKET CURRENT:");
        int valueEnd = rest.indexOf(',', valueStart);
        String currentStr = (valueEnd == -1) ? rest.substring(valueStart) : rest.substring(valueStart, valueEnd);
        pkt.current = currentStr.toFloat();
    }

    int relayIdx = rest.indexOf("SOCKET RELAY:");
    if (relayIdx != -1) {
        int valueStart = relayIdx + strlen("SOCKET RELAY:");
        int valueEnd = rest.indexOf(',', valueStart);
        String relayStr = (valueEnd == -1) ? rest.substring(valueStart) : rest.substring(valueStart, valueEnd);
        pkt.relayState = relayStr.toInt();
    }

    if (rest.indexOf("ALERT:OVERCURRENT") != -1) {
        Serial.printf("[LoRa] OVERCURRENT alert from %s\n", pkt.deviceId.c_str());
    }

    return pkt;
}

// ---------------------------------------------------------------------------
// populateDeviceArray — fills a caller-owned JsonArray with one object per
// registered device's latest reading: {deviceId, current, relayState, rssi}.
// Use this when /metrics already builds a larger document elsewhere (e.g.
// battery/inverter fields) and just needs the sockets nested in as a field.
// ---------------------------------------------------------------------------
void populateDeviceArray(JsonArray &arr) {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (auto const &entry : deviceCache) {
            JsonObject obj = arr.add<JsonObject>();
            obj["deviceId"]   = entry.second.deviceId;
            obj["current"]    = entry.second.current;
            obj["relayState"] = entry.second.relayState;
            obj["rssi"]       = entry.second.rssi;
        }
        xSemaphoreGive(dataMutex);
    }
}

// ---------------------------------------------------------------------------
// buildMetricsJson — standalone top-level array version of the same data,
// for a /metrics route that has nothing else to report.
// ---------------------------------------------------------------------------
String buildMetricsJson() {
    JsonDocument doc; // ArduinoJson v7
    JsonArray arr = doc.to<JsonArray>();
    populateDeviceArray(arr);
    // On a mutex timeout, populateDeviceArray simply leaves arr empty rather
    // than tagging an error onto its shape — callers already treat a shorter
    // list as "device(s) gone", so this degrades safely.

    String output;
    serializeJson(doc, output);
    return output;
}

// ---- diagnostics (remove once working) ----
static uint8_t diagReadReg(uint8_t addr) {
    SPI.beginTransaction(SPISettings(8E6, MSBFIRST, SPI_MODE0));
    digitalWrite(LORA_CS, LOW);
    SPI.transfer(addr & 0x7F);
    uint8_t v = SPI.transfer(0x00);
    digitalWrite(LORA_CS, HIGH);
    SPI.endTransaction();
    return v;
}

void loraDiag(const char *tag) {
    bool locked = loraMutex && xSemaphoreTake(loraMutex, pdMS_TO_TICKS(100)) == pdTRUE;
    uint8_t  op   = diagReadReg(0x01);
    uint32_t frf  = ((uint32_t)diagReadReg(0x06) << 16) |
                    ((uint32_t)diagReadReg(0x07) << 8)  | diagReadReg(0x08);
    uint8_t  sync = diagReadReg(0x39);
    uint8_t  cfg1 = diagReadReg(0x1D);
    uint8_t  cfg2 = diagReadReg(0x1E);
    if (locked) xSemaphoreGive(loraMutex);
    Serial.printf("[LoRa-DIAG %-14s] op=0x%02X freq=%.2fMHz sync=0x%02X cfg1=0x%02X cfg2=0x%02X\n",
                  tag, op, frf * 32.0f / 524288.0f, sync, cfg1, cfg2);
}



// ---------------------------------------------------------------------------
// loraTask — polls the radio, parses incoming packets, drops unregistered
// devices from /metrics (but still logs them to Serial), and caches
// registered telemetry readings per device for buildMetricsJson(). Event-only
// packets (e.g. "Power Restored") are logged but don't touch the cache.
// ---------------------------------------------------------------------------
void loraTask(void *parameter) {
    for (;;) {
        if (xSemaphoreTake(loraMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            int packetSize = LoRa.parsePacket();

            if (packetSize > 0) {
                loraPacket incoming = parsePacket();
                xSemaphoreGive(loraMutex); // release radio before NVS/JSON work

                Serial.printf("[LoRa] From %s | current=%.3fA relay=%d rssi=%d\n",
                              incoming.deviceId.c_str(), incoming.current,
                              incoming.relayState, incoming.rssi);

                if (incoming.deviceId.length() == 0) {
                    // couldn't parse a device ID at all — nothing to do
                } else if (!checkRegistered(incoming.deviceId)) {
                    Serial.printf("[LoRa] Unregistered device '%s' - not forwarded to /metrics\n",
                                  incoming.deviceId.c_str());
                } else if (incoming.current >= 0 && incoming.relayState >= 0) {
                    // Only real telemetry updates the cached reading
                    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        deviceCache[incoming.deviceId] = incoming;
                        xSemaphoreGive(dataMutex);
                    }
                }
            } else {
                xSemaphoreGive(loraMutex);
            }
        }
                // ---- diagnostics (remove once working) ----
        static uint32_t polls = 0, nextBeat = 0;
        polls++;
        if ((int32_t)(millis() - nextBeat) >= 0) {
            nextBeat = millis() + 10000;
            if (xSemaphoreTake(loraMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                // diagHeartbeat(polls);
                xSemaphoreGive(loraMutex);
            }
            polls = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(1)); // tight poll, as in the duplex test
    }
}

// ---------------------------------------------------------------------------
// sendRelayCommand — sends an ON/OFF command addressed to a specific socket.
// Wire format: "<deviceId>,RELAY_ON" / "<deviceId>,RELAY_OFF" — matches the
// Smart Socket transmitter's command listener.
// ---------------------------------------------------------------------------
void sendRelayCommand(String deviceId, bool turnOn) {
    if (deviceId.length() == 0) {
        Serial.println("[LoRa] sendRelayCommand called with empty deviceId");
        return;
    }

    String command = deviceId + "," + (turnOn ? "RELAY_ON" : "RELAY_OFF");

    if (xSemaphoreTake(loraMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        bool sent = false;
        if (LoRa.beginPacket() == 1) {
            LoRa.print(command);
            sent = LoRa.endPacket() == 1; // blocking, polls TX_DONE (no DIO0)
        }
        xSemaphoreGive(loraMutex);        // next parsePacket() re-arms RX

        if (sent) {
            Serial.printf("[LoRa] Sent %s command to %s\n",
                          turnOn ? "ON" : "OFF", deviceId.c_str());
        } else {
            Serial.printf("[LoRa] Failed to send %s command to %s\n",
                          turnOn ? "ON" : "OFF", deviceId.c_str());
        }
    } else {
        Serial.println("[LoRa] Could not acquire radio to send command");
    }
}

void startLoraTask() {
    xTaskCreatePinnedToCore(
        loraTask,          // Task function
        "loraTask",        // Name of the task
        4096,              // Stack size in words
        NULL,              // Task input parameter
        3,                 // Priority of the task
        &loraTaskHandle,   // Task handle
        1                  // Core where the task should run
    );
}