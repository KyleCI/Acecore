#pragma once
// USB serial console (115200) for factory provisioning and bench work.
// Commands (one per line):
//   INFO                         identity, provisioning state; label secrets until LOCK
//   SET <key> <value>            api_key | device_secret | local_tok | ota_url | ap_pass | admin_tok | gw_name |
//                                wifi_ssid | wifi_pass | espnow_keys | stm32_proto (0|1)
//   OTA_CONFIG {json}            1.0.x factory format (base_url, api_key, local_token, ssid, password; "token" ignored)
//   (local_tok = backend local_access_token; allowed after LOCK, applies immediately)
//   LOCK                         refuse further secret changes over USB
//   FACTORY_RESET                wipe secrets, Wi-Fi, and queue (needs typing twice)
//   REGISTER:<id> | UNREGISTER:<id> | RELAY:<id>:ON|OFF | SOCKETS
//   STM32 STATUS | STM32 MODE <off|grid|inverter|auto> | STM32 FAULT_RESET | STM32 VERSION
//   REBOOT
namespace console {
void service();
}
