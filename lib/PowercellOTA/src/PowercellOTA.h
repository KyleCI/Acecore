#pragma once
#include <Arduino.h>
#include <atomic>

#ifndef POWERCELL_FIRMWARE_VERSION
#define POWERCELL_FIRMWARE_VERSION "1.0.0"
#endif
#ifndef POWERCELL_FIRMWARE_KIND
#define POWERCELL_FIRMWARE_KIND "collector"
#endif
#define POWERCELL_HARDWARE_MODEL "esp32dev"

// Provision once over USB: OTA_CONFIG {"base_url":"https://api.acecore.tech",
// "token":"<per-device OTA token>","ssid":"...","password":"..."}
// Network credentials are needed for sockets only. Credentials stay in NVS,
// so a release binary contains no per-device secrets.
class PowercellOTA {
 public:
  void begin(bool temporaryWifi, const char *caCertificate, bool (*prepareUpdate)());
  void service(bool hardwareHealthy = true);
  bool configure(const String &line);
  bool busy() const { return updating; }
  static const char *version() { return POWERCELL_FIRMWARE_VERSION; }
 private:
  static void worker(void *arg);
  void check();
  bool exchange(const char *status, const String &error, String *response = nullptr);
  bool install(const String &path, size_t size, const String &sha256, const String &version);
  String baseUrl, token, ssid, password, deviceId, deploymentId, savedStatus, savedError;
  const char *ca = nullptr;
  bool (*prepare)() = nullptr;
  bool temporary = false, pendingBoot = false, healthy = false;
  std::atomic<bool> running{false}, updating{false};
  unsigned long nextCheck = 0, healthySince = 0;
  void saveStatus(const char *status, const String &error = "");
};

extern PowercellOTA firmwareOTA;
