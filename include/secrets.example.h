#pragma once
// TEMPLATE ONLY. This firmware never compiles secrets in.
// Provision each unit over USB at the factory instead (README "Provisioning"):
//   SET api_key <backend API key for this unit>
//   SET local_tok <local_access_token from provisioning>   (OTA uses api_key as Bearer)
//   SET espnow_keys <96 hex chars: PMK(32) LMK(32) AUTH(32)>
//   SET wifi_ssid <ssid>   /   SET wifi_pass <password>   (optional)
//   INFO   (print the label values: ap_pass, admin_token)
//   LOCK
// If a bench script needs a local copy, copy this file to include/secrets.h
// (gitignored) and keep it out of every zip and share.
#define POWERCELL_API_KEY ""
