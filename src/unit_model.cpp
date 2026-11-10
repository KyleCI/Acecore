#include "unit_model.h"
#include <Preferences.h>
#include "prefs_safe.h"

namespace unitmodel {
namespace { const pc::live::Model *g = nullptr; }

void begin() {
  Preferences p;
  if (prefsOpenRO(p, "pc_cfg")) { String c = p.getString("model", ""); p.end(); g = pc::live::findModel(c.c_str()); }
}
const pc::live::Model *get() { return g; }
bool set(const String &code) {
  String v = code; v.trim();
  bool clear = !v.length() || v.equalsIgnoreCase("none") || v.equalsIgnoreCase("unset");
  const pc::live::Model *m = clear ? nullptr : pc::live::findModel(v.c_str());
  if (!clear && !m) return false;
  Preferences p;
  if (!p.begin("pc_cfg", false)) return false;
  bool ok = clear ? (p.remove("model") || !p.isKey("model")) : p.putString("model", m->code) > 0;
  p.end();
  if (ok) g = m;
  return ok;
}
}  // namespace unitmodel
