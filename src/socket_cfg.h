#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
// AcePlug socket config glue (rules in pc_core/socket_cfg.h). Commands SET_LIMIT, SET_CONFIG and
// SET_OVERRIDE arrive on the backend WS (actions set_limit|set_config|set_override) and on the LAN
// (POST /v2/socket-config). Config is stored per socket in NVS "sock_cfg" (one blob per socket).
namespace socketcfg {
void begin();
// Engine pass over the paired sockets (powerguard, every 2 s).
void pass(bool socValid, float soc, bool gridDown, bool latched, bool overload);
// Applies one command object ({"type":"SET_...","socket":..}). type/socket may come from the WS
// envelope when the body omits them. Returns nullptr when applied, else an error code.
const char *apply(JsonObjectConst cmd, const char *type, const String &socket, String &socketOut);
void decorate(JsonArray sockets);           // adds the config/state fields to each socket object
void stateJson(const String &id, JsonObject o);
void setPriority(const String &id, bool essential);  // console PRIO fallback
void shedList(JsonArray a);                 // sockets currently off by an automatic rule
}  // namespace socketcfg
