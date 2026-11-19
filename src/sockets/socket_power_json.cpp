// 2.5.36 socket power keys (voltage x current, apparent VA) for populateDeviceArray().
#include "config.h"
#include "sockets.h"
#include "socket_power.h"
#include "../stm32_link.h"

// DSP: stm32link::status() is the PZEM-004T v3 sample on the AC output (hwPzemSample() already
// refuses samples older than 2 s). STM: the STM32 status frame vout. Either way the sample must be
// no older than pc::sockpower::kVoltageFreshMs (10 s) and inside STM32_STALE_MS (6 s).
SocketVoltage socketsCollectorVoltage() {
  SocketVoltage v;
  pc::Stm32Status s; uint32_t age = 0;
  if (stm32link::status(s, &age) && age < STM32_STALE_MS && pc::sockpower::voltageFresh(s.has_vout, age)) { v.fresh = true; v.volts = s.vout; }
  return v;
}

void socketPowerToJson(JsonObject o, bool socketFresh, float socketAmps, const SocketVoltage &v) {
  pc::sockpower::powerFieldsTo(o, pc::sockpower::compute(socketFresh, socketAmps, v.fresh, v.volts));
}
