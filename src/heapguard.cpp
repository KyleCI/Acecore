#include "heapguard.h"
#include <esp_heap_caps.h>
#include "log.h"

namespace heapguard {
namespace {
volatile uint32_t g_largestMin = UINT32_MAX;
// mbedTLS allocates with CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC: internal, 8-bit capable.
constexpr uint32_t kCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
}
bool probeTlsBuffers() {
  void *a = heap_caps_malloc(pc::kTlsRecordBuf, kCaps);
  void *b = a ? heap_caps_malloc(pc::kTlsRecordBuf, kCaps) : nullptr;
  const bool ok = a && b;
  if (b) heap_caps_free(b);
  if (a) heap_caps_free(a);
  return ok;
}
uint32_t freeNow() { return heap_caps_get_free_size(kCaps); }
uint32_t largestNow() { return heap_caps_get_largest_free_block(kCaps); }
uint32_t iramFree() {
  const uint32_t all = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), dram = heap_caps_get_free_size(kCaps);
  return all > dram ? all - dram : 0;
}
bool tlsFitsNow(pc::TlsHeap *why) {
  const uint32_t largest = largestNow(), freeB = freeNow();
  if (largest < g_largestMin) g_largestMin = largest;
  return pc::tlsHeapOk(largest, freeB, probeTlsBuffers, why);
}
bool tlsAllowed(pc::TlsBackoff &b, const char *who) {
  if (!b.due(millis())) return false;
  const uint32_t largest = largestNow(), freeB = freeNow();
  if (largest < g_largestMin) g_largestMin = largest;
  pc::TlsHeap why = pc::TlsHeap::Ok;
  const bool ok = pc::tlsHeapOk(largest, freeB, probeTlsBuffers, &why);
  switch (b.step(millis(), ok)) {
    case pc::TlsBackoff::Go:
      if (why == pc::TlsHeap::Probe)
        LOGI("MEM", "TLS_PROBE_OK", "%s: both %lu B record buffers fit (largest=%lu free=%lu)", who, (unsigned long)pc::kTlsRecordBuf,
             (unsigned long)largest, (unsigned long)freeB);
      return true;
    case pc::TlsBackoff::WaitLog:
      LOGW("MEM", "HEAP_LOW", "%s TLS deferred: %s%s largest=%lu free=%lu min=%lu need_free=%lu need_block=%lu/%lu; retry in %lu s", who,
           pc::tlsHeapName(why), why == pc::TlsHeap::Probe ? "_failed" : "", (unsigned long)largest, (unsigned long)freeB,
           (unsigned long)heapMin(), (unsigned long)pc::kTlsMinFree, (unsigned long)pc::kTlsMinBlock,
           (unsigned long)pc::kTlsPairBlock, (unsigned long)(b.delayMs() / 1000));
      return false;
    default: return false;
  }
}
void sample() {
  uint32_t largest = largestNow();
  if (largest < g_largestMin) g_largestMin = largest;
}
uint32_t heapMin() { return heap_caps_get_minimum_free_size(kCaps); }
uint32_t heapLargest() { return largestNow(); }
uint32_t heapLargestMin() { return g_largestMin == UINT32_MAX ? largestNow() : g_largestMin; }
}  // namespace heapguard
