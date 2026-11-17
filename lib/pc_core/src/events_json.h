#pragma once
#include "events_core.h"
// Event -> JSON fields (2.5.31). Shared by the device (ArduinoJson JsonObject) and the host
// tests (a fake object), so the wire shape is tested. Obj needs o["key"] = value for
// const char *, bool, float, uint32_t and nullptr.
//   id: the stable condition id ("over_temperature", "bms_1", "temp_sensor_fault", ...),
//       so the backend can tell a BMS alarm from the system NTC cutoff (2.5.31).
//   temp_c: over_temp only, the temperature the event carries (same number as value).
namespace pc {
namespace ev {
template <class Obj>
void eventFieldsTo(Obj o, const Event &e) {
  o["id"] = e.id;
  o["type"] = typeName(e.type);
  o["active"] = e.active;
  o["severity"] = sevName(e.severity);
  if (e.hasValue) o["value"] = e.value; else o["value"] = nullptr;
  if (e.type == Type::OverTemp) { if (e.hasValue) o["temp_c"] = e.value; else o["temp_c"] = nullptr; }
  if (e.socket[0]) o["socket"] = e.socket; else o["socket"] = nullptr;
  o["code"] = e.code;
  if (e.type == Type::ModeFallback || e.type == Type::LoadShed) o["reason"] = e.code;
  if (e.ts) o["ts"] = e.ts; else o["ts"] = nullptr;
  o["source"] = e.source;
  if (e.permanent) o["permanent"] = true;
  if (e.releaseRequires && e.active) o["release_requires"] = e.releaseRequires;
}
}  // namespace ev
}  // namespace pc
