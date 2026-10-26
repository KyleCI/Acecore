#include <Arduino.h>
#include <EmonLib.h>
#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <PowercellOTA.h>
#include <Preferences.h>
#include "websocket_ca.h"
#include "trip_lockout.h"
#include "powercell_lora_profile.h"

// AcePlug v0.1 pins (firmware-proven, Caleb + HW 2026-10-26). CAD is pending.
// Does not include collector pins.h or config.h.
// Not v0.2 (RELAY 27, CS 32, RST 26, DIO0 34, BL0942 UART 16/17).
// Not the GPIO14/4/2/33/22/21 map in LORA_SOCKET_PROTOCOL.md.
// Current is EmonLib on ADC IO35. BL0942 is v0.2 only.
// RF matches include/powercell_lora_profile.h (433 MHz, sync 0xF1, SF9).

namespace {
constexpr int LED_PIN          = 4;   // v0.1, firmware-proven
constexpr int RELAY_PIN        = 15;  // v0.1 SOCKET_RELAY. Not v0.2 IO27.
constexpr int CURRENT_PIN      = 35;  // v0.1 EmonLib ADC. Not BL0942.
constexpr int LORA_CS_PIN      = 5;   // v0.1
constexpr int LORA_RESET_PIN   = 25;  // v0.1
constexpr int LORA_DIO0_PIN    = 26;  // v0.1
static_assert(RELAY_PIN == 15 && RELAY_PIN != 27, "v0.1 relay is IO15");
static_assert(LORA_CS_PIN == 5 && LORA_RESET_PIN == 25 && LORA_DIO0_PIN == 26, "v0.1 LoRa pins");
static_assert(CURRENT_PIN == 35 && LED_PIN == 4, "v0.1 current and LED");
// Lockout reset: ESP32 module BOOT button (IO0, active low). Hold 3 s.
// Not a confirmed PCB net. Change this if v0.1 has a dedicated button.
constexpr int RESET_BUTTON_PIN = 0;
constexpr unsigned long RESET_HOLD_MS = 3000;
constexpr long    LORA_FREQUENCY         = POWERCELL_LORA_FREQUENCY_HZ;
constexpr long    LORA_BANDWIDTH         = POWERCELL_LORA_BW_HZ;
constexpr int     LORA_SF                = POWERCELL_LORA_SF;
constexpr int     LORA_CR                = POWERCELL_LORA_CR;
constexpr int     LORA_PREAMBLE          = POWERCELL_LORA_PREAMBLE;
constexpr int     LORA_TX_POWER          = POWERCELL_LORA_TX_POWER_DBM;
constexpr long    LORA_SPI_HZ            = POWERCELL_LORA_SPI_HZ;
constexpr uint8_t LORA_SYNC_WORD         = POWERCELL_LORA_SYNC_WORD;
constexpr int     LORA_SCK_PIN           = 18;
constexpr int     LORA_MISO_PIN          = 19;
constexpr int     LORA_MOSI_PIN          = 23;
constexpr float   CURRENT_CALIBRATION    = 5.1F;
// Fitted contact is 10 A. Software warn 8 A, software trip 10 A (clear warn 7 A).
// Not a 20 A contact and not a 20 A SKU. Was OVERCURRENT_AMPS = 50 A, then 20/16.
constexpr float   OVERCURRENT_AMPS       = 10.0F;
constexpr float   WARN_AMPS              = 8.0F;
constexpr float   WARN_CLEAR_AMPS        = 7.0F;
constexpr unsigned long TELEMETRY_BASE_INTERVAL_MS = 4000;
constexpr unsigned long COMMAND_ACK_DELAY_MS        = 60;

EnergyMonitor currentMonitor;
SemaphoreHandle_t relayMutex;

String deviceId;
bool   relayOn     = false;
bool   overcurrent = false;
uint8_t userLimitDa = 100;  // SET_LIMIT deciamps 10..100 (1.0..10.0 A). Trip is min(limit, 10 A).
inline float tripAmps() { return userLimitDa / 10.0F < OVERCURRENT_AMPS ? userLimitDa / 10.0F : OVERCURRENT_AMPS; }
inline float warnAmps() { return tripAmps() == OVERCURRENT_AMPS ? WARN_AMPS : tripAmps() * 0.8F; }
inline float warnClearAmps() { return warnAmps() - 1.0F; }
bool   currentHigh = false;  // >= WARN_AMPS, hysteresis to WARN_CLEAR_AMPS
float  lastCurrent = 0.0F;
// Acecore 2.2: 3 trips within a rolling 5 min latch the relay off (NVS "lockout").
TripLockout lockout;

unsigned long lastSendTime = 0;
unsigned long OVC_trip     = 0;
uint32_t receivedPackets = 0, matchedCommands = 0;
String seenCommandIds;
String acknowledgement;

bool pollCommands();

bool saveLockout() {
    Preferences p;
    if (!p.begin("lockout", false)) return false;
    bool ok = p.putBool("latched", lockout.latched()) == 1;
    p.putFloat("amps", lockout.lastTripAmps());
    p.end();
    return ok;
}

// ── Relay ─────────────────────────────────────────────────────────────────
void applyRelay(bool on) {
    if (on && (firmwareOTA.busy() || lockout.latched())) return;
    relayOn = on;
    digitalWrite(RELAY_PIN, on ? HIGH : LOW);
    digitalWrite(LED_PIN,   on ? HIGH : LOW);
}

void sendTelemetry(const char *reason = nullptr) {
    if (LoRa.beginPacket() != 1) {
        Serial.println("[LoRa] beginPacket failed");
        return;
    }
    LoRa.print(deviceId);
    LoRa.print(",SOCKET CURRENT:");
    LoRa.print(lastCurrent, 3);
    LoRa.print(",SOCKET RELAY:");
    LoRa.print(relayOn ? 1 : 0);
    LoRa.print(",FW:");
    LoRa.print(PowercellOTA::version());
    if (acknowledgement.length()) { LoRa.print(",ACK:"); LoRa.print(acknowledgement); }
    LoRa.print(",LIM:"); LoRa.print(userLimitDa);
    if (lockout.latched()) {
        LoRa.print(",ALERT:LOCKOUT,TRIP CURRENT:");
        LoRa.print(lockout.lastTripAmps(), 3);
    } else if (overcurrent) LoRa.print(",ALERT:OVERCURRENT");
    else if (currentHigh) LoRa.print(",WARN:CURRENT_HIGH");
    const int sent = LoRa.endPacket();
    LoRa.idle(); // Return to standby cleanly so parsePacket arms RX cleanly
    Serial.printf("[LoRa] Sent=%d current=%.3fA relay=%s%s%s\n",
                  sent, lastCurrent, relayOn ? "ON" : "OFF",
                  reason ? " reason=" : "", reason ? reason : "");
}

// ── Command handler ───────────────────────────────────────────────────────
bool handleCommand(const String &packet) {
    // Acecore 2.2: removed a debug toggleRelay() here; it flipped the relay pin on
    // every received packet, including packets addressed to other sockets.
    int comma = packet.indexOf(',');
    if (comma <= 0) return false;

    String target = packet.substring(0, comma);
    String action = packet.substring(comma + 1);
    target.trim();
    action.trim();

    Serial.printf("[LoRa] Received: target='%s' action='%s' myId='%s'\n",
                  target.c_str(), action.c_str(), deviceId.c_str());

    if (!target.equalsIgnoreCase(deviceId)) return false;  // not for us

    bool requestedOn = relayOn;
    bool resetLockout = false;
    String commandId;
    int newLimit = 0;
    if (action.startsWith("SET_LIMIT,")) {
        // SET_LIMIT,<deciamps>,<cmdId>: clamped to 1.0..10.0 A. The 10 A trip does not move.
        String rest = action.substring(10);
        int c2 = rest.indexOf(',');
        if (c2 <= 0) return false;
        newLimit = rest.substring(0, c2).toInt();
        if (newLimit <= 0) return false;
        newLimit = newLimit < 10 ? 10 : newLimit > 100 ? 100 : newLimit;
        commandId = rest.substring(c2 + 1);
    } else if (action.startsWith("RESET_LOCKOUT,")) {
        resetLockout = true;
        requestedOn = false;
        commandId = action.substring(14);
    } else if (action.startsWith("TOGGLE,")) {
        requestedOn = !relayOn;
        commandId = action.substring(7);
    } else if (action.startsWith("RELAY_ON,")) {
        requestedOn = true;
        commandId = action.substring(9);
    } else if (action.startsWith("RELAY_OFF,")) {
        requestedOn = false;
        commandId = action.substring(10);
    } else if (action.startsWith("ON,")) {
        requestedOn = true;
        commandId = action.substring(3);
    } else if (action.startsWith("OFF,")) {
        requestedOn = false;
        commandId = action.substring(4);
    } else {
        Serial.printf("[LoRa] Unknown action: '%s'\n", action.c_str());
        return false;
    }

    commandId.trim();
    if (commandId.isEmpty() || commandId.length() > 47) return false;
    for (size_t i = 0; i < commandId.length(); ++i)
        if (!isalnum(static_cast<unsigned char>(commandId[i])) && commandId[i] != '-') return false;

    ++matchedCommands;
    acknowledgement = commandId;
    if (seenCommandIds.indexOf("|" + commandId + "|") >= 0) {
        Serial.println("[LoRa] Duplicate command; acknowledging without switching");
        delay(COMMAND_ACK_DELAY_MS);
        sendTelemetry("duplicate_ack");
        acknowledgement = "";
        lastSendTime = millis();
        return true;
    }
    if (xSemaphoreTake(relayMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        acknowledgement = "";
        return false;
    }

    // Persist before switching so a reboot cannot repeat a recent command.
    String nextIds = seenCommandIds + "|" + commandId + "|";
    while (nextIds.length() > 1600) {
        int end = nextIds.indexOf('|', 1);
        nextIds.remove(0, end + 1);
    }
    Preferences dedupe;
    bool saved = dedupe.begin("relay_commands", false);
    if (saved) { saved = dedupe.putString("ids", nextIds) > 0; dedupe.end(); }
    if (!saved) {
        xSemaphoreGive(relayMutex);
        acknowledgement = "";
        Serial.println("[LoRa] Toggle not applied: cannot save command ID");
        return false;
    }
    seenCommandIds = nextIds;
    if (newLimit) {
        Preferences lim;
        bool ok = lim.begin("sock_limit", false);
        if (ok) { ok = lim.putUChar("da", static_cast<uint8_t>(newLimit)) == 1; lim.end(); }
        if (ok) { userLimitDa = static_cast<uint8_t>(newLimit); currentHigh = false; }
        xSemaphoreGive(relayMutex);
        Serial.printf("[LIMIT] %.1f A (saved=%d)\n", newLimit / 10.0F, ok);
        delay(COMMAND_ACK_DELAY_MS);
        sendTelemetry("limit_set");  // LIM: shows the applied value (unchanged if the save failed)
        acknowledgement = "";
        lastSendTime = millis();
        return true;
    }
    if (resetLockout) {
        // Clears the latch and the trip history. The relay stays off; the user switches it on.
        lockout.reset();
        overcurrent = false;
        OVC_trip = 0;
        const bool saved = saveLockout();
        xSemaphoreGive(relayMutex);
        Serial.printf("[LOCKOUT] Reset by LoRa command (saved=%d)\n", saved);
        delay(COMMAND_ACK_DELAY_MS);
        sendTelemetry("lockout_reset");
        acknowledgement = "";
        lastSendTime = millis();
        return true;
    }
    if (requestedOn && (overcurrent || lockout.latched() || firmwareOTA.busy())) {
        xSemaphoreGive(relayMutex);
        Serial.println("[LoRa] RELAY_ON rejected — overcurrent, lockout or OTA active");
        delay(COMMAND_ACK_DELAY_MS);
        sendTelemetry("command_rejected");
        acknowledgement = "";
        lastSendTime = millis();
        return true;
    }

    if (requestedOn) {
        Preferences safety;
        if (safety.begin("ota_safety", false)) {
            safety.putBool("hold_off", false);
            safety.end();
        }
    }

    applyRelay(requestedOn);
    xSemaphoreGive(relayMutex);
    Serial.printf("[LoRa] Relay -> %s\n", requestedOn ? "ON" : "OFF");

    delay(COMMAND_ACK_DELAY_MS);
    sendTelemetry("command_ack");
    acknowledgement = "";
    lastSendTime = millis(); // reset telemetry timer so we don't double-send
    return true;
}

// ── Polled receive — called every loop() ─────────────────────────────────
bool pollCommands() {
    int packetSize = LoRa.parsePacket();
    if (packetSize <= 0) return false;

    String packet;
    packet.reserve(packetSize);
    while (LoRa.available()) {
        packet += static_cast<char>(LoRa.read());
    }
    const int rssi = LoRa.packetRssi();
    const float snr = LoRa.packetSnr();
    ++receivedPackets;
    packet.trim();
    Serial.printf("[LoRa] RX %d B rssi=%d snr=%.1f: '%s'\n", packetSize, rssi, snr, packet.c_str());
    return handleCommand(packet);
}

bool deferTelemetryForReceive() {
    return pollCommands();
}

void reportReceiverHealth() {
    static unsigned long lastReport = 0;
    if (millis() - lastReport < 5000) return;
    lastReport = millis();
    Serial.printf("[LoRa RX] id=%s packets=%lu commands=%lu relay=%d\n",
                  deviceId.c_str(), receivedPackets, matchedCommands, relayOn ? 1 : 0);
}

} // namespace

// ── Setup ─────────────────────────────────────────────────────────────────
void setup() {
    // IO15 is a strapping pin (MTDO) and the v0.1 relay. Hold the coil off
    // before any other subsystem so boot does not energize it.
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);
    Serial.begin(115200);

    Preferences dedupe;
    if (dedupe.begin("relay_commands", true)) {
        seenCommandIds = dedupe.getString("ids", "");
        dedupe.end();
    }
    relayMutex = xSemaphoreCreateMutex();
    if (!relayMutex) {
        pinMode(RELAY_PIN, OUTPUT);
        digitalWrite(RELAY_PIN, LOW);
        ESP.restart();
    }

    pinMode(LED_PIN,   OUTPUT);
    pinMode(RELAY_PIN, OUTPUT);
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

    Preferences lk;
    if (lk.begin("lockout", true)) {
        lockout.restore(lk.getBool("latched", false), lk.getFloat("amps", 0.0F));
        lk.end();
    }
    Preferences lim;
    if (lim.begin("sock_limit", true)) {
        uint8_t v = lim.getUChar("da", 100);
        userLimitDa = v < 10 ? 10 : v > 100 ? 100 : v;
        lim.end();
    }
    if (lockout.latched()) Serial.printf("[LOCKOUT] Latched from NVS (last trip %.2f A); relay stays off\n", lockout.lastTripAmps());

    // Restore relay state across OTA reboot
    Preferences safety;
    safety.begin("ota_safety", true);
    bool holdOff = safety.getBool("hold_off", false);
    safety.end();
    if (lockout.latched()) applyRelay(false);  // latched: off regardless of the OTA hold flag
    else applyRelay(!holdOff);

    firmwareOTA.begin(true, WEBSOCKET_CA_CERT, []() {
        if (xSemaphoreTake(relayMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
        Preferences safety;
        bool stored = false;
        if (safety.begin("ota_safety", false)) {
            stored = safety.putBool("hold_off", true) == 1;
            safety.end();
        }
        applyRelay(false);
        xSemaphoreGive(relayMutex);
        return stored;
    });

    // Get MAC as device ID, then kill WiFi — LoRa only
    WiFi.mode(WIFI_STA);
    deviceId = WiFi.macAddress();
    deviceId.replace(":", "");
    WiFi.mode(WIFI_OFF);
    Serial.printf("[Socket] Device ID: %s\n", deviceId.c_str());

    randomSeed(esp_random());
    currentMonitor.current(CURRENT_PIN, CURRENT_CALIBRATION);

    SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
    LoRa.setSPI(SPI);
    // DIO0 is wired on this PCB (GPIO 26) but RX is polled, same as the collector.
    LoRa.setPins(LORA_CS_PIN, LORA_RESET_PIN, LORA_DIO0_PIN);
    while (!LoRa.begin(LORA_FREQUENCY)) {
        Serial.println("[LoRa] Init failed, retrying...");
        delay(500);
    }
    LoRa.setSPIFrequency(LORA_SPI_HZ);
    LoRa.setTxPower(LORA_TX_POWER);
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BANDWIDTH);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setPreambleLength(LORA_PREAMBLE);
    LoRa.setSyncWord(LORA_SYNC_WORD);
    LoRa.enableCrc();
    LoRa.idle(); // standby; pollCommands() arms single RX

    lastSendTime = millis();
    Serial.printf("[LoRa] Smart socket ready freq=%.0fMHz SF%d BW%ld CR4/%d pre%d sync=0x%02X CRC=1 tx=%ddBm\n",
                  LORA_FREQUENCY / 1e6, LORA_SF, LORA_BANDWIDTH, LORA_CR, LORA_PREAMBLE,
                  LORA_SYNC_WORD, LORA_TX_POWER);
}

// ── Loop ──────────────────────────────────────────────────────────────────
void loop() {
    // 1. Check for incoming LoRa commands first (lowest latency)
    pollCommands();
    // reportReceiverHealth();

    // 2. OTA housekeeping
    firmwareOTA.service();

    // 3. Serial OTA config
    static String serialLine;
    while (Serial.available()) {
        char c = static_cast<char>(Serial.read());
        if (c == '\n') { firmwareOTA.configure(serialLine); serialLine = ""; }
        else if (c != '\r') {
            if (serialLine.length() < 512) serialLine += c;
            else serialLine = "";
        }
    }

    // 4. Current sampling + periodic telemetry
    unsigned long now = millis();

    // Socket button: hold 3 s to clear a lockout (relay stays off).
    static unsigned long pressedAt = 0;
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
        if (!pressedAt) pressedAt = now ? now : 1;
        else if (pressedAt != ULONG_MAX && now - pressedAt >= RESET_HOLD_MS) {
            pressedAt = ULONG_MAX;  // once per press
            if (lockout.latched() && xSemaphoreTake(relayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                lockout.reset(); overcurrent = false; OVC_trip = 0;
                const bool saved = saveLockout();
                xSemaphoreGive(relayMutex);
                Serial.printf("[LOCKOUT] Reset by button (saved=%d)\n", saved);
                sendTelemetry("lockout_reset");
                lastSendTime = now;
            }
        }
    } else pressedAt = 0;

    // Overcurrent cleared after 15 s — restore relay and notify collector (not while locked out)
    if (OVC_trip != 0 && !lockout.latched() && now - OVC_trip >= 15000) {
        Serial.println("[OVC] Condition cleared — restoring relay");
        OVC_trip   = 0;
        overcurrent = false;
        applyRelay(true);
        sendTelemetry("power_restored");
        lastSendTime = now;
    }

    // Jitter avoids repeated on-air collisions with retrying collectors
    const unsigned long telemetryInterval = TELEMETRY_BASE_INTERVAL_MS + (esp_random() % 1000);
    if (now - lastSendTime >= telemetryInterval) {
        if (deferTelemetryForReceive()) { delay(2); return; }
        lastCurrent = static_cast<float>(currentMonitor.calcIrms(1480));

        const char *reason = nullptr;
        if (lastCurrent >= tripAmps() && OVC_trip == 0) {
            OVC_trip    = now;
            overcurrent = true;
            applyRelay(false);
            reason = "overcurrent_trip";
            Serial.printf("[OVC] %.2f A >= %.1f A trip (user limit %.1f A): relay tripped\n", lastCurrent, tripAmps(), userLimitDa / 10.0F);
            // User-limit and 10 A trips both count toward the lockout; 15 s auto-reclose otherwise.
            if (lockout.onTrip(now, lastCurrent)) {
                OVC_trip = 0;          // no auto reclose
                overcurrent = false;   // superseded by ALERT:LOCKOUT
                const bool saved = saveLockout();
                reason = "lockout";
                Serial.printf("[LOCKOUT] 3 trips in 5 min, last %.2f A; relay latched off (saved=%d)\n", lastCurrent, saved);
            }
        }
        const bool high = currentHigh ? lastCurrent >= warnClearAmps() : lastCurrent >= warnAmps();
        if (high != currentHigh) { currentHigh = high; if (!reason) reason = high ? "current_high" : "current_normal"; }

        if (reason || !deferTelemetryForReceive()) {  // alert changes go out at once
            sendTelemetry(reason);
            lastSendTime = millis();
        }
    }

    delay(2);
}