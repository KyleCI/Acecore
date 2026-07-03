// ============================================================
//  ble.cpp
// ============================================================
#include "ble.h"

// ─── BLE Handles ─────────────────────────────────────────────────────────────
BLEServer         *pServer               = nullptr;
BLEService        *pService              = nullptr;
BLECharacteristic *pCharacteristic       = nullptr;
BLEService        *pUartService          = nullptr;
BLECharacteristic *pUartTxCharacteristic = nullptr;
BLECharacteristic *pUartRxCharacteristic = nullptr;



bool   bleInitialized = false;
bool   deviceConnected = false;
String receivedData    = "";
bool   shouldRestart   = false;
bool   relayPulse      = false;
unsigned long relayPulseStart = 0;
unsigned long update          = millis();

// ─── Helpers ─────────────────────────────────────────────────────────────────
String getMacAddress(esp_mac_type_t interface)
{
  unsigned char mac[6] = {0};
  char buf[18];
  if (esp_read_mac(mac, interface) == ESP_OK)
    sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

void uartSend(const String &msg)
{
  if (pUartTxCharacteristic && pServer->getConnectedCount() > 0)
  {
    pUartTxCharacteristic->setValue(msg.c_str());
    pUartTxCharacteristic->notify();
  }
}

// ─── BLE Callbacks ───────────────────────────────────────────────────────────
class MyCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic) override
  {
    String val = String(pCharacteristic->getValue().c_str());
    for (char c : val)
      receivedData += c;
  }
};

class UartCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic) override
  {
    std::string val = pCharacteristic->getValue();
    if (!val.empty())
    {
      String cmd = String(val.c_str());
      cmd.trim();

      Serial.print("[BLE CMD] ");
      Serial.println(cmd);

      if (cmd == "Switch")
      {
        digitalWrite(RELAY_PIN, HIGH);
        delay(300);
        digitalWrite(RELAY_PIN, LOW);
        uartSend("Switched!\n");
      }
      else if (cmd == "Status")
      {
        String status = "OUT: " + String(analogRead(OUT_VOLT)) +
                        " IN: "  + String(analogRead(IN_VOLT))  +
                        " BAT: " + String(analogRead(BAT))      + "\n";
        uartSend(status);
      }
      else
      {
        uartSend("Unknown command: " + cmd + "\n");
      }
    }
  }
};

class MyServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer) override
  {
    deviceConnected = true;
    String mac = getMacAddress(ESP_MAC_BT);
    pCharacteristic->setValue(mac.c_str());
    pCharacteristic->notify();
  }

  void onDisconnect(BLEServer *pServer) override
  {
    deviceConnected = false;
    BLEDevice::startAdvertising();
  }
};

// ─── BLE Setup ───────────────────────────────────────────────────────────────
void handleBLE()
{
  if (!bleInitialized)
  {
    BLEDevice::init("PowerCell Prime 1A");
    bleInitialized = true;
  }

  if (pServer)
  {
    BLEDevice::stopAdvertising();
    pServer->removeService(pService);
    pServer->removeService(pUartService);
  }
  else
  {
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
  }

  pService        = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ    |
      BLECharacteristic::PROPERTY_WRITE   |
      BLECharacteristic::PROPERTY_NOTIFY  |
      BLECharacteristic::PROPERTY_INDICATE);
  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();

  pUartService          = pServer->createService(UART_SERVICE_UUID);
  pUartTxCharacteristic = pUartService->createCharacteristic(
      UART_TX_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_NOTIFY);
  pUartTxCharacteristic->addDescriptor(new BLE2902());
  pUartRxCharacteristic = pUartService->createCharacteristic(
      UART_RX_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_NR);
  pUartRxCharacteristic->setCallbacks(new UartCallbacks());
  pUartService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->addServiceUUID(UART_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x00);
  BLEDevice::startAdvertising();
}