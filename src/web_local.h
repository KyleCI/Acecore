#pragma once
// Local HTTP API on port 80 (LAN and setup AP), contract of
// official-powercell-app PR #3 (docs/HARDWARE_API.md "Local device web server").
//
// Credential: HTTP Basic, user "powercell", password = local_access_token that
// the backend generates at provisioning and delivers to the unit (WS
// set_local_token, USB "SET local_tok", or the setup AP /provision body).
// The per-unit label token (admin_tok) is a setup credential only: it is
// accepted as Basic powercell:<label token> from clients on the setup AP, and
// only for GET /, POST /login and POST /provision.
//
//   POST /login       {"username":"powercell","token":"..."} and/or Basic -> 200 | 401
//   GET  /            page (setup page when unauthenticated on the AP or when no token exists)
//   GET  /data        JSON snapshot incl. notifications[]
//   GET  /events      SSE: ready, heartbeat, state, notification
//   POST /state       {"state":"on"|"off"}
//   POST /relay       {"socket_id":"...","state":"on"|"off"}
//   POST /provision   {"ssid","pass"|"password","local_token"?}  (Wi-Fi setup)
//   POST /register    {"socket_id"|"id"}     pair a smart socket
//   POST /unregister  {"socket_id"|"id"}     forget a smart socket
//   GET  /api/health  public, no secrets
//   GET  /metrics, /api/state, POST /api/inverter/{mode,fault_reset}  (token)
// POST-only routes answer 405 to any other method; missing/wrong credentials 401.
namespace web {
void begin();
void tokenChanged();  // call after the local token is changed outside the web server
void service();  // runs deferred STM32 commands and the SSE/notification pump
}
