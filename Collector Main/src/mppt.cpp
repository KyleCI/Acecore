/*
  ESP32 SPI SLAVE - Integer Parser for Arduino MPPT Master
  Implementation file
*/

#include "mppt.h"

// ─── Buffer & Statistic Definitions ─────────────────────────────────────────
uint8_t rxBuf[SPI_FRAME_SIZE];
uint8_t txBuf[SPI_FRAME_SIZE];

uint32_t      frameCount       = 0;
uint32_t      parseErrors      = 0;
unsigned long lastDisplayTime  = 0;
unsigned long lastJsonOutputTime = 0;
String compactJson;


// ─── Main SPI Handler ─────────────────────────────────────────────────────────
// Populates `mppt` in place; returns true only on a fully parsed frame.
bool handleSPI(JsonObject mppt) {
  spi_slave_transaction_t t;
  memset(&t, 0, sizeof(t));
  memset(rxBuf, 0, SPI_FRAME_SIZE);
  t.length    = SPI_FRAME_SIZE * 8;
  t.tx_buffer = txBuf;
  t.rx_buffer = rxBuf;

  esp_err_t ret = spi_slave_transmit(VSPI_HOST, &t, portMAX_DELAY);
  if (ret != ESP_OK) {
    Serial.print(F("[ERROR] SPI transmission failed: "));
    Serial.println(ret);
    return false;
  }

  frameCount++;
  uint16_t len = 0;
  while (len < SPI_FRAME_SIZE && rxBuf[len] != 0) len++;

  if (len == 0) {
    Serial.println(F("[WARN] Received empty frame"));
    buildAck();
    return false;
  }

  char dataString[SPI_FRAME_SIZE + 1];
  memcpy(dataString, rxBuf, len);
  dataString[len] = '\0';
  for (int i = len - 1; i >= 0; i--) {
    if (dataString[i] == '\n' || dataString[i] == '\r') dataString[i] = '\0';
    else break;
  }

  MPPTData mpptData;
  MPPTIntegerData mpptInt;
  if (!parseIntegerCSV(dataString, mpptInt)) {
    parseErrors++;
    Serial.print(F("[PARSE ERROR] Frame #")); Serial.println(frameCount);
#if MPPT_VERBOSE
    Serial.print(F("Received: "));            Serial.println(dataString);
    displayRawData(dataString, len);
#endif
    buildAck();
    return false;
  }

  convertToFloat(mpptInt, mpptData);
#if MPPT_VERBOSE
  displayFormattedTable(mpptData);
  displayRawData(dataString, len);
  displayJSONOutput(mpptData);
#endif

  mppt["Timestamp"]       = mpptData.timestamp;
  mppt["Solar Voltage"]   = mpptData.solarVoltage;
  mppt["Solar Current"]   = mpptData.current;
  mppt["Solar Power"]     = mpptData.power;
  mppt["Mppt Temperature"]     = mpptData.temperature;
  mppt["Battery Voltage"] = mpptData.batteryVoltage;
  mppt["MPP Found"]       = mpptData.mppFound;

  buildAck();
  if (millis() - lastDisplayTime >= 30000) {
    displayStatistics();
    lastDisplayTime = millis();
  }
  return true;
}

// ─── SPI ACK ─────────────────────────────────────────────────────────────────
void buildAck() {
  const char* ack = "OK";
  memset(txBuf, 0, SPI_FRAME_SIZE);
  memcpy(txBuf, ack, strlen(ack));
}

// ─── Parsing ─────────────────────────────────────────────────────────────────
bool parseIntegerCSV(const char* data, MPPTIntegerData& mpptInt) {
  int parsed = sscanf(data, "%lu,%ld,%ld,%ld,%ld,%d,%d,%ld,%ld,%ld,%ld",
    &mpptInt.timestamp,
    &mpptInt.solarVoltage_mV,
    &mpptInt.current_mA,
    &mpptInt.power_mW,
    &mpptInt.temperature_mC,
    &mpptInt.pwmDuty,
    &mpptInt.mppFound,
    &mpptInt.mppVoltage_mV,
    &mpptInt.mppCurrent_mA,
    &mpptInt.mppPower_mW,
    &mpptInt.batteryVoltage_mV
  );
  return (parsed == 11);
}

// ─── Conversion ───────────────────────────────────────────────────────────────
void convertToFloat(const MPPTIntegerData& mpptInt, MPPTData& mppt) {
  mppt.timestamp      = mpptInt.timestamp;
  mppt.solarVoltage   = mpptInt.solarVoltage_mV   / INT_TO_FLOAT_DIVISOR;
  mppt.current        = mpptInt.current_mA         / INT_TO_FLOAT_DIVISOR;
  mppt.power          = mpptInt.power_mW           / INT_TO_FLOAT_DIVISOR;
  mppt.temperature    = mpptInt.temperature_mC     / INT_TO_FLOAT_DIVISOR;
  mppt.pwmDuty        = mpptInt.pwmDuty;
  mppt.mppFound       = (mpptInt.mppFound == 1);
  mppt.mppVoltage     = mpptInt.mppVoltage_mV      / INT_TO_FLOAT_DIVISOR;
  mppt.mppCurrent     = mpptInt.mppCurrent_mA      / INT_TO_FLOAT_DIVISOR;
  mppt.mppPower       = mpptInt.mppPower_mW        / INT_TO_FLOAT_DIVISOR;
  mppt.batteryVoltage = mpptInt.batteryVoltage_mV  / INT_TO_FLOAT_DIVISOR;
}

// ─── Display: JSON ────────────────────────────────────────────────────────────
void displayJSONOutput(const MPPTData& mppt) {
  StaticJsonDocument<768> doc;

  doc["frame"]         = frameCount;
  doc["timestamp_ms"]  = mppt.timestamp;
  doc["timestamp_sec"] = mppt.timestamp / 1000.0;

  JsonObject ch2 = doc.createNestedObject("channel2");
  ch2["voltage"]      = mppt.solarVoltage;
  ch2["current"]      = mppt.current;
  ch2["power"]        = mppt.power;
  ch2["temperature"]  = mppt.temperature;
  ch2["pwm_duty"]     = mppt.pwmDuty;
  ch2["pwm_percent"]  = (mppt.pwmDuty / 255.0) * 100.0;

  JsonObject mpp = doc.createNestedObject("mpp_tracking");
  mpp["found"] = mppt.mppFound;
  if (mppt.mppFound) {
    mpp["voltage"] = mppt.mppVoltage;
    mpp["current"] = mppt.mppCurrent;
    mpp["power"]   = mppt.mppPower;
  }

  JsonObject battery = doc.createNestedObject("battery");
  battery["voltage"] = mppt.batteryVoltage;

  JsonObject calculated = doc.createNestedObject("calculated");
  if (mppt.solarVoltage > 0 && mppt.current > 0) {
    float theoreticalPower = mppt.solarVoltage * mppt.current;
    if (theoreticalPower > 0) {
      calculated["efficiency_percent"] = (mppt.power / theoreticalPower) * 100.0;
    }
  }

  JsonObject system = doc.createNestedObject("system");
  system["free_heap"]  = ESP.getFreeHeap();
  system["uptime_sec"] = millis() / 1000;

  String jsonOutput;
  serializeJson(doc, jsonOutput);

  Serial.println();
  Serial.println(F("═══════════════════════════════════════════════════════════════"));
  Serial.println(F(" MPPT DATA (JSON FORMAT) "));
  Serial.println(F("═══════════════════════════════════════════════════════════════"));
  Serial.println(jsonOutput);
  Serial.println(F("═══════════════════════════════════════════════════════════════"));
}

// ─── Display: Formatted Table ─────────────────────────────────────────────────
void displayFormattedTable(const MPPTData& mppt) {
  Serial.println();
  Serial.println(F("┌───────────────┬──────────────────────────────────────────┐"));
  Serial.println(F("│ FIELD         │ VALUE                                    │"));
  Serial.println(F("├───────────────┼──────────────────────────────────────────┤"));

  Serial.print(F("│ Timestamp     │ "));
  Serial.print(mppt.timestamp);
  Serial.print(F(" ms ("));
  Serial.print(mppt.timestamp / 1000.0, 1);
  Serial.println(F(" s)                          │"));

  Serial.println(F("├───────────────┼──────────────────────────────────────────┤"));
  Serial.println(F("│ SOLAR CH2     │                                          │"));

  Serial.print(F("│ Voltage       │ "));
  Serial.print(mppt.solarVoltage, 2);
  Serial.println(F(" V                                    │"));

  Serial.print(F("│ Current       │ "));
  Serial.print(mppt.current, 3);
  Serial.println(F(" A                                   │"));

  Serial.print(F("│ Power         │ "));
  Serial.print(mppt.power, 3);
  Serial.println(F(" W                                   │"));

  Serial.print(F("│ Temperature   │ "));
  Serial.print(mppt.temperature, 1);
  Serial.println(F(" °C                                  │"));

  Serial.print(F("│ PWM Duty      │ "));
  Serial.print(mppt.pwmDuty);
  Serial.print(F(" ("));
  Serial.print((mppt.pwmDuty / 255.0) * 100.0, 1);
  Serial.println(F("%)                               │"));

  Serial.println(F("├───────────────┼──────────────────────────────────────────┤"));
  Serial.println(F("│ MPP TRACKING  │                                          │"));

  Serial.print(F("│ MPP Found     │ "));
  Serial.println(mppt.mppFound ? F("YES ✓                                     │")
                               : F("NO  ✗                                     │"));

  if (mppt.mppFound) {
    Serial.print(F("│ MPP Voltage   │ "));
    Serial.print(mppt.mppVoltage, 2);
    Serial.println(F(" V                                    │"));

    Serial.print(F("│ MPP Current   │ "));
    Serial.print(mppt.mppCurrent, 3);
    Serial.println(F(" A                                   │"));

    Serial.print(F("│ MPP Power     │ "));
    Serial.print(mppt.mppPower, 3);
    Serial.println(F(" W                                   │"));
  }

  Serial.println(F("├───────────────┼──────────────────────────────────────────┤"));
  Serial.println(F("│ BATTERY       │                                          │"));

  Serial.print(F("│ Voltage       │ "));
  Serial.print(mppt.batteryVoltage, 2);
  Serial.println(F(" V                                    │"));

  Serial.println(F("└───────────────┴──────────────────────────────────────────┘"));
}

// ─── Display: Raw Data ────────────────────────────────────────────────────────
void displayRawData(const char* data, uint16_t len) {
  Serial.println(F("\n┌────────────── RAW INTEGER DATA ──────────────┐"));
  Serial.print(F("│ "));
  for (uint16_t i = 0; i < len && i < 60; i++) {
    if (data[i] == '\n' || data[i] == '\r') continue;
    Serial.print(data[i]);
  }
  if (len > 60) Serial.print("...");
  for (int i = 0; i < (60 - (int)(len > 60 ? 60 : len)); i++) Serial.print(" ");
  Serial.println(F(" │"));
  Serial.println(F("└──────────────────────────────────────────────┘"));
  Serial.println(F("Note: Values are in mV, mA, mW, m°C (multiplied by 1000)"));
}

// ─── Display: Statistics ──────────────────────────────────────────────────────
void displayStatistics() {
  Serial.println();
  Serial.println(F("┌─────────────────────────────────────────────────────────┐"));
  Serial.println(F("│              SPI STATISTICS SUMMARY                     │"));
  Serial.println(F("├─────────────────────────────────────────────────────────┤"));

  Serial.print(F("│ Total frames received: "));
  Serial.print(frameCount);
  for (int i = 0; i < 40 - (int)String(frameCount).length(); i++) Serial.print(" ");
  Serial.println(F("│"));

  Serial.print(F("│ Parse errors: "));
  Serial.print(parseErrors);
  for (int i = 0; i < 40 - (int)String(parseErrors).length(); i++) Serial.print(" ");
  Serial.println(F("│"));

  float successRate = (frameCount > 0)
    ? ((frameCount - parseErrors) * 100.0f / frameCount)
    : 0.0f;
  Serial.print(F("│ Success rate: "));
  Serial.print(successRate, 1);
  Serial.print(F("%"));
  for (int i = 0; i < 37 - (int)String(successRate, 1).length(); i++) Serial.print(" ");
  Serial.println(F("│"));

  unsigned long uptime = millis() / 1000;
  Serial.print(F("│ Uptime: "));
  Serial.print(uptime / 60);
  Serial.print(F("m "));
  Serial.print(uptime % 60);
  Serial.print(F("s"));
  for (int i = 0; i < 32 - (int)String(uptime).length(); i++) Serial.print(" ");
  Serial.println(F("│"));

  Serial.println(F("└─────────────────────────────────────────────────────────┘"));
}
