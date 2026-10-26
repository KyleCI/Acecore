#include "heapguard.h"
#include <esp_heap_caps.h>
#include "log.h"

namespace heapguard {
namespace {
volatile uint32_t g_largestMin = UINT32_MAX;
}
bool tlsAllowed(pc::TlsBackoff &b, const char *who) {
  uint32_t largest = ESP.getMaxAllocHeap();
  if (largest < g_largestMin) g_largestMin = largest;
  switch (b.check(millis(), largest)) {
    case pc::TlsBackoff::Go: return true;
    case pc::TlsBackoff::WaitLog:
      LOGW("MEM", "HEAP_LOW", "%s TLS deferred: largest=%lu < %lu free=%lu min=%lu; retry in %lu s", who,
           (unsigned long)largest, (unsigned long)pc::kTlsMinBlock, (unsigned long)ESP.getFreeHeap(),
           (unsigned long)ESP.getMinFreeHeap(), (unsigned long)(b.delayMs() / 1000));
      return false;
    default: return false;
  }
}
void sample() {
  uint32_t largest = ESP.getMaxAllocHeap();
  if (largest < g_largestMin) g_largestMin = largest;
}
uint32_t heapMin() { return ESP.getMinFreeHeap(); }
uint32_t heapLargest() { return ESP.getMaxAllocHeap(); }
uint32_t heapLargestMin() { return g_largestMin == UINT32_MAX ? ESP.getMaxAllocHeap() : g_largestMin; }
}  // namespace heapguard
