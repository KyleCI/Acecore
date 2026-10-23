// /* This file contains source code for the Smart Socket
// Indicator LED shows socket state,
// ZMCT103C current sensor reads current,
// Toggles Relay off/on
// Communicate with collector via LoRa,
// Data sent to the Collector to be rendered on the frontend*/
// #include <housekeeping.h>
// #include <LoRa.h>
// #include <EmonLib.h>
// #include <WiFi.h>

// // Device identity — the ESP32's own MAC address, colons stripped (e.g.
// // "246F28AE9C1A"). Unique per chip, so the same firmware image works on
// // every socket — no more per-unit build_flags needed. Stripping colons
// // keeps it under NVS's 15-character key limit (a full MAC is 17 chars)
// // and saves a few bytes of LoRa airtime on every packet.
// String DEVICE_ID;

// EnergyMonitor emon1;
// double Irms;        //Current in amperes
// bool relayState = true;
// unsigned long lastSendTime = 0;
// const unsigned long SEND_INTERVAL = 2000;

// // Was previously declared *inside* loop(), which reset it to 0 every
// // iteration — the "cleared" check below would then be true almost always,
// // not just 15s after an actual trip. Must persist across iterations.
// static unsigned long OVC_trip = 0;

// // Calibration constant: (CT turns ratio / burden resistor).
// // Adjust after calibrating against a known load.
// const float CURRENT_CALIBRATION = 5.1;

// void setup()
// {
//     Serial.begin(115200);
//     pinMode(SOCKET_LED, OUTPUT);
//     pinMode(SOCKET_RELAY, OUTPUT);
//     digitalWrite(SOCKET_RELAY, HIGH);
//     digitalWrite(SOCKET_LED, HIGH);

//     // Read the MAC once, then turn WiFi off — this device only talks over LoRa.
//     WiFi.mode(WIFI_STA);
//     DEVICE_ID = WiFi.macAddress();
//     DEVICE_ID.replace(":", "");
//     WiFi.mode(WIFI_OFF);
//     Serial.print("Device ID (MAC): ");
//     Serial.println(DEVICE_ID);

//     LoRa.setPins(SOCKET_CS, SOCKET_RESET, SOCKET_DIO0);
//     while (!LoRa.begin(433E6))
//     {
//         Serial.println("Trying to Initialize LoRa");
//         delay(500);
//     }
//     // Change sync word (0xF1) to match the receiver LoRa ranges from 0-0xFF
//     LoRa.setSyncWord(0xF1);
//     Serial.println("LoRa Initializing Successful!");

//     emon1.current(SOCKET_CURRENT, CURRENT_CALIBRATION); // Current: input pin, calibration
// }

// void loop()
// {
//     // ---- 1. Send telemetry (current + relay state) periodically ----
//     unsigned long now = millis();
//     Irms = emon1.calcIrms(1480); // 1480 samples ~ good for 50Hz mains

//     if (OVC_trip != 0 && millis() - OVC_trip >= 15000) {
//         Serial.println("Overcurrent condition cleared. Relay can be turned on again.");
//         LoRa.beginPacket();
//         OVC_trip = 0;
//         digitalWrite(SOCKET_RELAY, HIGH);
//         digitalWrite(SOCKET_LED, HIGH);
//         relayState = true;
//         LoRa.print(DEVICE_ID);
//         LoRa.print(",Power Restored:");
//         LoRa.endPacket();
//     }

//     if (now - lastSendTime >= SEND_INTERVAL)
//     {
//         lastSendTime = now;
//         Irms = emon1.calcIrms(1480); // 1480 samples ~ good for 50Hz mains
//         LoRa.beginPacket();
//         LoRa.print(DEVICE_ID);
//         LoRa.print(",SOCKET CURRENT:");
//         LoRa.print(Irms, 3);
//         LoRa.print(",SOCKET RELAY:");
//         LoRa.print(relayState ? 1 : 0);
//         Serial.print("Sent -> Current: ");
//         Serial.print(Irms, 3);
//         Serial.print(" A, Relay: ");
//         Serial.println(relayState ? "ON" : "OFF");
//         if (Irms > SOCKET_OVERCURRENT_THRESHOLD) {
//             OVC_trip = millis();
//             Serial.println("Overcurrent detected! Turning off relay.");
//             digitalWrite(SOCKET_RELAY, LOW);
//             digitalWrite(SOCKET_LED, LOW);
//             relayState = false;
//             LoRa.print(",ALERT:OVERCURRENT");
//         }
//         LoRa.endPacket();
//     }

//     // ---- 2. Listen for incoming commands from the Collector ----
//     int packetSize = LoRa.parsePacket();
//     if (packetSize)
//     {
//         String command = "";
//         while (LoRa.available())
//         {
//             command += (char)LoRa.read();
//         }
//         command.trim();

//         // Commands are addressed as "<deviceId>,RELAY_ON" / "<deviceId>,RELAY_OFF"
//         int commaIdx = command.indexOf(',');
//         String targetId = (commaIdx == -1) ? "" : command.substring(0, commaIdx);
//         String action   = (commaIdx == -1) ? command : command.substring(commaIdx + 1);

//         if (targetId == DEVICE_ID)
//         {
//             Serial.print("Received command: ");
//             Serial.println(action);

//             if (action.equalsIgnoreCase("RELAY_OFF"))
//             {
//                 digitalWrite(SOCKET_RELAY, LOW);
//                 digitalWrite(SOCKET_LED, LOW);
//                 relayState = false;
//                 Serial.println("Relay turned OFF by remote command.");
//             }
//             else if (action.equalsIgnoreCase("RELAY_ON"))
//             {
//                 digitalWrite(SOCKET_RELAY, HIGH);
//                 digitalWrite(SOCKET_LED, HIGH);
//                 relayState = true;
//                 Serial.println("Relay turned ON by remote command.");
//             }
//         }
//         // else: command addressed to a different socket, ignore
//     }
// }