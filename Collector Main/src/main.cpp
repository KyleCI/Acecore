#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_bt_device.h"
#include "provisioning.h"
#include "webinterface.h"
#include "adcreadings.h"
#include "ble.h"
#include "config.h"
#include "device_manager.h"
#include "wifi_manager.h"
#include "espnow_handler.h"
#include "web_server.h"
#include "upload_task.h"
#include "lighting.h"
#include "mppt.h"
#include "sensor_task.h"
#include "spi_task.h"

// ─── Pin Definitions ──────────────────────────────────────────────────────────

#define LIGHTS 25

const int RELAY_PIN = 15;
const int OUT_VOLT = 39;
const int IN_VOLT = 36;
const int BAT = 32;
const int CSENSE = 35;
const int RELAY_ON = 26;

const int redLED = 12;
const int greenLED = 27;
const int whiteLED = 13;
int currentLED;

#define ADC_RES 4095.0f
#define ADC_VREF 3.3f
#define DIVIDER (R2 / (R1 + R2))
#define RELAY_PULSE_MS 10

// ─── Arduino Entry Points ───
void setup()
{
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(OUT_VOLT, INPUT);
  pinMode(IN_VOLT, INPUT);
  pinMode(CSENSE, INPUT);
  pinMode(BAT, INPUT);
  pinMode(LIGHTS, OUTPUT);
  pinMode(redLED, OUTPUT);
  pinMode(greenLED, OUTPUT);  
  pinMode(whiteLED, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LIGHTS, LOW);

  if (!SPIFFS.begin(true))
  {
    Serial.println("[SPIFFS] Mount failed — halting");
    while (1)
      ;
  }

  deviceState = loadDeviceState();
  lastUpdate = millis();

  // ─── Routes ───────────────────────────────────────────────────────────────

  server.on("/ON", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    deviceState = true;
    digitalWrite(RELAY_PIN, HIGH);
    digitalWrite(LIGHTS, HIGH);
    // relayPulse = true;
    // relayPulseStart = millis();
    saveDeviceState();
    req->send(200, "text/plain", "ON"); });

  server.on("/OFF", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    deviceState = false;
    digitalWrite(RELAY_PIN, LOW);
    digitalWrite(LIGHTS, LOW);
    // relayPulse = true;
    // relayPulseStart = millis();
    saveDeviceState();
    req->send(200, "text/plain", "OFF"); });
  server.on("/provision", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    if (req->hasParam("ssid")) {
        // Form submitted — save credentials
        String ssid = req->getParam("ssid")->value();
        String pass = req->hasParam("pass") ? req->getParam("pass")->value() : "";
        saveNVS(ssid, pass);
        req->send(200, "text/plain", "Saved! Rebooting...");
        shouldRestart = true;
    } else {
        // No params — serve the page
        req->send_P(200, "text/html", PROVISION_HTML);
    } });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req)
            { req->send(SPIFFS, "/home.html", "text/html"); });
  server.on("/monitor", HTTP_GET, [](AsyncWebServerRequest *req)
            { req->send(SPIFFS, "/monitor_home.html", "text/html"); });

  server.on("/sensor-data", HTTP_GET, handleSensorData);

  server.on("/state", HTTP_GET, [](AsyncWebServerRequest *req)
            { req->send(200, "text/plain", deviceState ? "ON" : "OFF"); });

  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest *req)
            { req->send(200, "application/json", getSensorData()); });

  // Latest MPPT frame parsed by the background SPI task (fresh < 5 s).
  server.on("/mppt", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    String m = getMpptJson(5000);
    if (m.length()) req->send(200, "application/json", m);
    else            req->send(200, "application/json", "{}"); });

  server.on("/monitor", HTTP_GET, [](AsyncWebServerRequest *req)
            { handleRoot(req); });
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *req)
            { handleData(req); });
  server.on("/approve", HTTP_GET, [](AsyncWebServerRequest *req)
            { handleApprove(req); });
  server.on("/reject", HTTP_GET, [](AsyncWebServerRequest *req)
            { handleReject(req); });
  server.on("/relay", HTTP_GET, [](AsyncWebServerRequest *req)
            { handleRelay(req); });

  server.addHandler(&events); // SSE

  server.onNotFound([](AsyncWebServerRequest *req)
                    {
    String path = req->url();
    if (SPIFFS.exists(path))
    {
      String ct = "text/plain";
      if (path.endsWith(".html")) ct = "text/html";
      else if (path.endsWith(".css"))  ct = "text/css";
      else if (path.endsWith(".js"))   ct = "application/javascript";
      else if (path.endsWith(".png"))  ct = "image/png";
      else if (path.endsWith(".jpg"))  ct = "image/jpeg";
      req->send(SPIFFS, path, ct);
    }
    else
      req->send(404, "text/plain", "Not found"); });

  // ─── Provisioning Server (fallback) ─────────────────────────────────────────

  // ─── WiFi + Server Start ──────────────────────────────────────────────────
  if (connectToSaved())
  {
    server.begin();
    Serial.println("[HTTP] App server started");
  }
  else
  {
    startProvisioning(); // this registers its own / route and calls server.begin()
  }
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);

  // Initialise shared resources
  uploadMutex = xSemaphoreCreateMutex();
  initSensorTask();   // sensor-snapshot mutex
  initSpiTask();      // MPPT-cache mutex
  memset(uploadSlots, 0, sizeof(uploadSlots));
  memset(relayPending, 0, sizeof(relayPending));
  memset(devices, 0, sizeof(devices));
  Serial.println(F("[PKT]  Start=0xAA  End=0x55  Checksum=XOR[1..17]"));
  Serial.println(F("[ACK]  ACK=0x06  NAK=0x15"));
  Serial.println();

  // Configure SPI bus
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = PIN_MOSI;
  buscfg.miso_io_num = PIN_MISO;
  buscfg.sclk_io_num = PIN_SCK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = SPI_FRAME_SIZE;

  // Configure SPI slave interface
  spi_slave_interface_config_t slvcfg = {};
  slvcfg.spics_io_num = PIN_SS;
  slvcfg.flags = 0;
  slvcfg.queue_size = 3;
  slvcfg.mode = 0;
  slvcfg.post_setup_cb = NULL;
  slvcfg.post_trans_cb = NULL;

  // Initialize SPI slave
  esp_err_t ret = spi_slave_initialize(
      VSPI_HOST,
      &buscfg,
      &slvcfg,
      SPI_DMA_CH_AUTO);

  if (ret != ESP_OK)
  {
    Serial.println(F("✗ ERROR: SPI Slave Initialization Failed! ✗"));
    Serial.print(F("Error code: "));
    Serial.println(ret);
    while (1)
    {
      delay(1000);
      Serial.println(F("System halted - check wiring"));
    }
  }

  Serial.println(F("✓ SPI Slave Initialized Successfully ✓"));
  Serial.print(F("  MOSI: GPIO"));
  Serial.print(PIN_MOSI);
  Serial.print(F("  MISO: GPIO"));
  Serial.print(PIN_MISO);
  Serial.print(F("  SCK:  GPIO"));
  Serial.print(PIN_SCK);
  Serial.print(F("  SS:   GPIO"));
  Serial.println(PIN_SS);
  Serial.println();

  buildAck();

  frameCount = 0;
  parseErrors = 0;
  lastDisplayTime = millis();
  lastJsonOutputTime = millis();

  // ─── Background tasks ───────────────────────────────────────────────────────
  // Pinned to core 1 (APP_CPU) so the blocking SPI/PZEM work stays off the
  // WiFi/ESP-NOW core (core 0). The async web server runs on its own task.
  xTaskCreatePinnedToCore(sensorTask, "sensors",  4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(spiTask,    "spi_mppt", 6144, NULL, 1, NULL, 1);
  // Background task: batch-upload device data over HTTP
  xTaskCreate(uploadTask, "http_upload", 8192, NULL, 1, NULL);
}

void loop()
{
  checkWiFi(); // ← initialises ESP-NOW once WiFi is up

  if (shouldRestart)
  {
    delay(500);
    ESP.restart();
  }

  // ─── LED status from the cached battery % (no blocking ADC here) ───
  digitalWrite(redLED, LOW);
  digitalWrite(greenLED, LOW);
  digitalWrite(whiteLED, LOW);

  currentLED = updateLEDColor();
  digitalWrite(currentLED, HIGH);

  // ─── Rate-limited debug print from the cached PZEM snapshot ───
  static unsigned long lastDbg = 0;
  if (millis() - lastDbg >= 1000)
  {
    lastDbg = millis();
    SensorCache s = getSensorCache();
    Serial.printf("Voltage: %.2f V, Current: %.2f A, Power: %.2f W, Energy: %.3f kWh\n",
                  s.output.voltage, s.output.current, s.output.power, s.output.energy);
  }

  vTaskDelay(pdMS_TO_TICKS(10)); // yield — loop is now lightweight
}
