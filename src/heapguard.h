#pragma once
#include <Arduino.h>
#include <heap_guard.h>  // pc::TlsBackoff, pc::tlsHeapOk (lib/pc_core)

// Heap watermarks and the pre-TLS memory guard (see lib/pc_core/src/heap_guard.h).
namespace heapguard {
// true = enough heap for a TLS handshake now (2.5.20 rule, 8-bit DRAM numbers since 2.5.22: free >= 50 KB and the two
// mbedTLS record buffers fit, probed for real when the largest block alone is not
// conclusive). Otherwise backs off (5 s doubling to 60 s) and logs HEAP_LOW once per
// backoff step. `b` is per caller. Call it only right before a new TLS connect.
bool tlsAllowed(pc::TlsBackoff &b, const char *who);
// Allocates and frees the two record buffers with the caps mbedTLS uses. true = both fit.
bool probeTlsBuffers();
// 2.5.22: every number below is the 8-bit internal DRAM heap (MALLOC_CAP_INTERNAL |
// MALLOC_CAP_8BIT), the heap mbedTLS, Strings and JSON allocate from. ESP.getFreeHeap()
// and ESP.getMaxAllocHeap() also count the 32-bit-only IRAM heap (~41 KB on ESP32).
uint32_t freeNow();          // free DRAM now
uint32_t largestNow();       // largest free DRAM block now
uint32_t iramFree();         // free IRAM-only heap (32-bit access; informational)
// Strict check before a download TLS session (no backoff, logs nothing): free >= 50 KB and
// both record buffers fit (probed for real when the largest block alone is not conclusive).
bool tlsFitsNow(pc::TlsHeap *why = nullptr);
void sample();               // update the watermarks (main loop, every pass)
uint32_t heapMin();          // lowest free DRAM since boot
uint32_t heapLargest();      // largest free DRAM block now
uint32_t heapLargestMin();   // lowest "largest free DRAM block" seen since boot
}  // namespace heapguard
