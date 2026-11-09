#include "factory_station.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
  CHECK(factory_serial_valid("ACE-000123"));
  CHECK(!factory_serial_valid("ACE-DDEEFF"));
  CHECK(!display_serial_match("aa:bb:cc:dd:ee:ff", "ACE-000123"));
  CHECK(display_serial_match("ace-000123", "ACE-000123"));
  char ap[24];
  label_hotspot("ACE-000123", ap);
  CHECK(strcmp(ap, "Powercell-000123") == 0);

  powercell::FactoryStation station;
  station.usable_w = 4500;
  uint8_t secret[32];
  for (int i = 0; i < 32; ++i) secret[i] = (uint8_t)(i + 1);
  station.set_secret(secret);
  char reply[512];
  station.console("SERIAL ACE-000123", reply, sizeof reply);
  CHECK(strcmp(reply, "OK serial set") == 0);
  station.console("SET model PC3K", reply, sizeof reply);
  CHECK(strstr(reply, "ERR"));
  station.console("SET model PC5K", reply, sizeof reply);
  CHECK(strstr(reply, "OK model=PC5K"));
  station.console("SECRET", reply, sizeof reply);
  CHECK(strncmp(reply, "SECRET ", 7) == 0);
  station.console("SECRET", reply, sizeof reply);
  CHECK(strstr(reply, "ERR already reported"));
  station.console("LOCK", reply, sizeof reply);
  CHECK(strcmp(reply, "OK locked") == 0);
  station.console("SERIAL ACE-000124", reply, sizeof reply);
  CHECK(strstr(reply, "ERR locked"));
  CHECK(strcmp(station.serial, "ACE-000123") == 0);

  char msg[160];
  size_t n = pc::paylock::message("cmd1", true, "hq", 1700001000, msg, sizeof msg);
  uint8_t mac[32];
  pc::hmacSha256(secret, 32, msg, n, mac);
  char sig[65];
  static const char* hexd = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    sig[i * 2] = hexd[mac[i] >> 4];
    sig[i * 2 + 1] = hexd[mac[i] & 15];
  }
  sig[64] = 0;
  CHECK(station.apply_payment("cmd1", true, "hq", 1700001000, sig));
  CHECK(station.payment_locked);
  station.customer_reset();
  CHECK(station.payment_locked);
  CHECK(strcmp(station.serial, "ACE-000123") == 0);
  CHECK(!station.apply_payment("cmd1", false, "hq", 1700002000, "00"));

  powercell::WifiSession wifi;
  CHECK(wifi.start("Home", "correcthorsebattery", "ab12cd34"));
  CHECK(!wifi.save(reply, sizeof reply));
  wifi.succeed("192.168.1.40");
  CHECK(wifi.save(reply, sizeof reply));
  CHECK(strcmp(reply, "192.168.1.40") == 0);
  CHECK(strcmp(wifi.saved_ssid, "Home") == 0);

  powercell::AcPublication measured = powercell::publish_pzem(true, 230.0f, 2.0f, 400.0f, 12.5f);
  CHECK(measured.publish_active && measured.active_w == 400.0f && !measured.grid_voltage && !measured.pv);
  powercell::AcPublication missing = powercell::publish_pzem(false, 0, 0, 0, 0);
  CHECK(!missing.publish_active && !missing.apparent_power);

  char body[256], path[80];
  powercell::ota_check_json("AABBCCDDEEFF", "1.0.1", "collector", "hw-v0.1.1", true, "idle", "", body, sizeof body);
  CHECK(strstr(body, "\"device_id\":\"AABBCCDDEEFF\""));
  CHECK(strstr(body, "\"firmware_version\":\"1.0.1\""));
  CHECK(!strstr(body, "ACE-000123"));
  powercell::ota_download_path("AABBCCDDEEFF", path, sizeof path);
  CHECK(strcmp(path, "/api/ota/download?device_id=AABBCCDDEEFF") == 0);

  station.usable_w = 3150;
  station.locked = false;
  station.console("SET model PC5K", reply, sizeof reply);
  CHECK(strstr(reply, "ERR"));
  station.console("SET model PC3K", reply, sizeof reply);
  CHECK(strstr(reply, "ERR"));

  if (g_failures) {
    fprintf(stderr, "%d factory contract checks failed\n", g_failures);
    return 1;
  }
  printf("factory contract checks passed\n");
  return 0;
}
