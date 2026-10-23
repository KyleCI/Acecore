#pragma once

class AsyncWebServer;

// Register once before server.begin(); service from loop(), even without Wi-Fi.
void initOfflineBatteryNotifications(AsyncWebServer& server);
void serviceOfflineBatteryNotifications();
