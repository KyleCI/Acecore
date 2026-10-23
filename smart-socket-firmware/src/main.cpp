#include <Arduino.h>
#include <EmonLib.h>
#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <PowercellOTA.h>
#include <Preferences.h>
#include "websocket_ca.h"

namespace {
constexpr int LED_PIN = 4;
constexpr int RELAY_PIN = 15;
constexpr int CURRENT_PIN = 35;
constexpr int LORA_SCK_PIN = 18;
constexpr int LORA_MISO_PIN = 19;
constexpr int LORA_MOSI_PIN = 23;
constexpr int LORA_CS_PIN = 5;
constexpr int LORA_RESET_PIN = 25;
constexpr int LORA_DIO0_PIN = 26;   // still wired, not used (polling mode, like the Collector)
// Radio settings: MUST match the Collector's lora.cpp exactly
constexpr long LORA_FREQUENCY = 915E6;
constexpr long LORA_BANDWIDTH = 125E3;
constexpr int LORA_SF = 7;
constexpr int LORA_CR = 5;           // 4/5
constexpr int LORA_PREAMBLE = 8;
constexpr uint8_t LORA_SYNC_WORD = 0xF1;
constexpr int LORA_TX_POWER = 17;    // dBm, PA_BOOST
constexpr int MAX_PACKET_LEN = 127;
constexpr float CURRENT_CALIBRATION = 5.1F;
constexpr float OVERCURRENT_AMPS = 50.0F;
constexpr unsigned long TELEMETRY_INTERVAL_MS = 5000;
constexpr unsigned long COMMAND_ACK_DELAY_MS = 50;

SemaphoreHandle_t relayMutex;
EnergyMonitor currentMonitor;
String deviceId;
bool relayOn = false;
bool overcurrent = false;
float lastCurrent = 0.0F;
unsigned long nextTelemetryAt = 0;

bool deadlineReached(unsigned long deadline) {
  return static_cast<long>(millis() - deadline) >= 0;
}

void applyRelay(bool on) {
  if (on && firmwareOTA.busy()) return;
  relayOn = on;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void sendTelemetry(const char *reason = nullptr) {
  if (LoRa.beginPacket() != 1) {
    Serial.println("[LoRa] Unable to begin telemetry packet");
    return;
  }
  LoRa.print(deviceId);
  LoRa.print(",SOCKET CURRENT:");
  LoRa.print(lastCurrent, 3);
  LoRa.print(",SOCKET RELAY:");
  LoRa.print(relayOn ? 1 : 0);
  LoRa.print(",FW:");
  LoRa.print(PowercellOTA::version());
  if (overcurrent) LoRa.print(",ALERT:OVERCURRENT");
  const int sent = LoRa.endPacket();   // blocking; next parsePacket() re-arms RX
  Serial.printf("[LoRa] Telemetry sent=%d current=%.3fA relay=%s%s%s\n",
                sent, lastCurrent, relayOn ? "ON" : "OFF",
                reason ? " reason=" : "", reason ? reason : "");
}

void handleCommand(const String &packet) {
  const int comma = packet.indexOf(',');
  if (comma <= 0) return;

  String target = packet.substring(0, comma);
  String action = packet.substring(comma + 1);
  target.trim();
  action.trim();
  if (!target.equalsIgnoreCase(deviceId)) return;

  bool requestedOn;
  if (action.equalsIgnoreCase("RELAY_ON")) {
    requestedOn = true;
  } else if (action.equalsIgnoreCase("RELAY_OFF")) {
    requestedOn = false;
  } else {
    return;
  }

  if (xSemaphoreTake(relayMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  if (requestedOn && (overcurrent || firmwareOTA.busy())) {
    xSemaphoreGive(relayMutex);
    Serial.println("[LoRa] RELAY_ON rejected while overcurrent protection is active");
    delay(COMMAND_ACK_DELAY_MS);
    sendTelemetry("overcurrent_reject");
    return;
  }

  if (requestedOn) {
    Preferences safety;
    if (safety.begin("ota_safety", false)) { safety.putBool("hold_off", false); safety.end(); }
  }
  applyRelay(requestedOn);
  xSemaphoreGive(relayMutex);
  Serial.printf("[LoRa] Command applied: %s\n", requestedOn ? "ON" : "OFF");

  // The collector treats a telemetry frame with the requested relay state as
  // the physical acknowledgement. Send it immediately instead of waiting for
  // the periodic telemetry deadline.
  delay(COMMAND_ACK_DELAY_MS);
  sendTelemetry("command_ack");
  nextTelemetryAt = millis() + TELEMETRY_INTERVAL_MS + random(0, 500);
}

// Polling receive, the same as the Collector and the duplex test
void pollCommands() {
  const int size = LoRa.parsePacket();
  if (size <= 0) return;
  if (size > MAX_PACKET_LEN) {                 // oversize: drain and ignore
    while (LoRa.available()) LoRa.read();
    return;
  }
  String packet;
  packet.reserve(size);
  while (LoRa.available()) packet += static_cast<char>(LoRa.read());
  packet.trim();
  handleCommand(packet);                       // ignores packets not addressed to us
}
}  // namespace

void setup() {
  Serial.begin(115200);
  relayMutex = xSemaphoreCreateMutex();
  if (!relayMutex) { pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, LOW); ESP.restart(); }
  pinMode(LED_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  Preferences safety;
  safety.begin("ota_safety", true);
  bool holdOff = safety.getBool("hold_off", false);
  safety.end();
  applyRelay(!holdOff);
  firmwareOTA.begin(true, WEBSOCKET_CA_CERT, []() {
    if (xSemaphoreTake(relayMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    Preferences safety;
    bool stored = false;
    if (safety.begin("ota_safety", false)) { stored = safety.putBool("hold_off", true) == 1; safety.end(); }
    applyRelay(false);
    xSemaphoreGive(relayMutex);
    return stored;
  });

  WiFi.mode(WIFI_STA);
  deviceId = WiFi.macAddress();
  deviceId.replace(":", "");
  WiFi.mode(WIFI_OFF);

  randomSeed(esp_random());
  currentMonitor.current(CURRENT_PIN, CURRENT_CALIBRATION);
  SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
  LoRa.setSPI(SPI);
  LoRa.setPins(LORA_CS_PIN, LORA_RESET_PIN, -1);   // polling mode, DIO0 unused
  while (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[LoRa] Initialization failed; retrying");
    delay(500);
  }
  // Same configuration as the Collector
  LoRa.setSignalBandwidth(LORA_BANDWIDTH);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setPreambleLength(LORA_PREAMBLE);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.enableCrc();
  nextTelemetryAt = millis() + 1000 + random(0, 500);
  Serial.printf("[LoRa] Smart socket ready id=%s frequency=%.0fMHz sync=0x%02X\n",
                deviceId.c_str(), LORA_FREQUENCY / 1e6, LORA_SYNC_WORD);
}

void loop() {
  // Keep command receive latency low. Current measurement runs only when a
  // telemetry sample is due, rather than blocking every pass through loop().
  pollCommands();
  firmwareOTA.service();
  static String serialLine;
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n') { firmwareOTA.configure(serialLine); serialLine = ""; }
    else if (c != '\r') {
      if (serialLine.length() < 512) serialLine += c;
      else serialLine = "";
    }
  }

  if (deadlineReached(nextTelemetryAt)) {
    lastCurrent = static_cast<float>(currentMonitor.calcIrms(1480));
    if (lastCurrent > OVERCURRENT_AMPS) {
      overcurrent = true;
      applyRelay(false);
    } else if (overcurrent) {
      overcurrent = false;
    }
    sendTelemetry();
    nextTelemetryAt = millis() + TELEMETRY_INTERVAL_MS + random(0, 500);
  }

  delay(1);
}