#pragma once
#include <Arduino.h>
#include <heap_guard.h>  // pc::TlsBackoff (lib/pc_core)

// Heap watermarks and the pre-TLS memory guard (see lib/pc_core/src/heap_guard.h).
namespace heapguard {
// true = enough contiguous heap for a TLS handshake now. Otherwise backs off
// (5 s doubling to 60 s) and logs HEAP_LOW once per backoff step. `b` is per caller.
bool tlsAllowed(pc::TlsBackoff &b, const char *who);
void sample();               // update the watermarks (main loop, every pass)
uint32_t heapMin();          // lowest free heap since boot
uint32_t heapLargest();      // largest free block now
uint32_t heapLargestMin();   // lowest "largest free block" seen since boot
}  // namespace heapguard
