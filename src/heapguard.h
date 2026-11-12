#pragma once
#include <Arduino.h>
#include <heap_guard.h>  // pc::TlsBackoff, pc::tlsHeapOk (lib/pc_core)

// Heap watermarks and the pre-TLS memory guard (see lib/pc_core/src/heap_guard.h).
namespace heapguard {
// true = enough heap for a TLS handshake now (2.5.20 rule: free >= 50 KB and the two
// mbedTLS record buffers fit, probed for real when the largest block alone is not
// conclusive). Otherwise backs off (5 s doubling to 60 s) and logs HEAP_LOW once per
// backoff step. `b` is per caller. Call it only right before a new TLS connect.
bool tlsAllowed(pc::TlsBackoff &b, const char *who);
// Allocates and frees the two record buffers with the caps mbedTLS uses. true = both fit.
bool probeTlsBuffers();
void sample();               // update the watermarks (main loop, every pass)
uint32_t heapMin();          // lowest free heap since boot
uint32_t heapLargest();      // largest free block now
uint32_t heapLargestMin();   // lowest "largest free block" seen since boot
}  // namespace heapguard
