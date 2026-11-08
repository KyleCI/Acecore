#pragma once

class AsyncWebServer;

// Register once before server.begin(); service from loop(), even without Wi-Fi.
void initOfflineBatteryNotifications(AsyncWebServer& server);
void serviceOfflineBatteryNotifications();
// False only while the discharge latch is held or its saved state could not be read.
// This is the collector's own ON command. It is not proof a DSP or BMS FET opened.
bool batteryOutputOnAllowed();
