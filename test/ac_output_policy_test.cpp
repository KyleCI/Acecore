#include "ac_output_policy.h"

#include <stdio.h>

using namespace powercell;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static AcWatts sample(float watts, uint32_t at, bool valid = true) {
  AcWatts s;
  s.valid = valid;
  s.coverage_verified = false;
  s.watts = watts;
  s.receivedAtMs = at;
  return s;
}

static void holds(AcOutputPolicy* p, int watts, uint32_t at) {
  p->update(sample(static_cast<float>(watts), at), at);
}

static void tripsAt(int usable) {
  AcOutputPolicy p;
  p.setUsable(usable);
  holds(&p, usable, 0);
  CHECK(!p.shutdownLatched());
  holds(&p, usable + 1, 0);
  CHECK(!p.shutdownLatched());
  holds(&p, usable + 1, 3000);
  CHECK(!p.shutdownLatched());
  holds(&p, usable + 1, 3000 + AcOutputPolicy::ShutdownHoldMs - 1);
  CHECK(!p.shutdownLatched());
  holds(&p, usable + 1, 3000 + AcOutputPolicy::ShutdownHoldMs);
  CHECK(p.shutdownLatched());
  CHECK(p.systemOverloadLatched());
  CHECK(p.takeShutdownEdge());
  CHECK(!p.turnsOutputOn());
  CHECK(!AcOutputPolicy::kDrivesOutput);
  CHECK(!AcOutputPolicy::kCoverageVerified);
  CHECK(p.unavailableReason() != nullptr);
  holds(&p, 0, 20000);
  CHECK(p.shutdownLatched());
  const uint32_t under = 21000;
  holds(&p, usable, under);
  holds(&p, usable, under + AcOutputPolicy::ReleaseHoldMs);
  CHECK(!p.shutdownLatched());
  CHECK(!p.turnsOutputOn());
}

static void missingRatingDoesNotTrip() {
  AcOutputPolicy p;
  p.update(sample(9000, 0), 0);
  p.update(sample(9000, 20000), 20000);
  CHECK(!p.shutdownLatched());
  CHECK(p.coverageUnavailable());
}

static void staleDoesNotTripOrRelease() {
  AcOutputPolicy p;
  p.setUsable(4500);
  p.update(sample(9000, 0, false), 10000);
  CHECK(!p.shutdownLatched());
  holds(&p, 9000, 0);
  holds(&p, 9000, 3000);
  holds(&p, 9000, 3000 + AcOutputPolicy::ShutdownHoldMs);
  CHECK(p.shutdownLatched());
  p.update(sample(100, 0, false), 40000);
  CHECK(p.shutdownLatched());
}

static void lowerCeilingCannotRaiseOverload() {
  AcOutputPolicy p;
  p.setUsable(4500);
  p.setCeiling(500, AcMode::WarnOnly);
  holds(&p, 800, 0);
  holds(&p, 800, 3000);
  holds(&p, 800, 6000);
  CHECK(p.warning());
  CHECK(!p.shutdownLatched());
  p.setCeiling(500, AcMode::WarnAndShutdown);
  holds(&p, 800, 6000 + AcOutputPolicy::ShutdownHoldMs);
  CHECK(p.shutdownLatched());
  CHECK(!p.systemOverloadLatched());
  CHECK(!p.turnsOutputOn());
}

int main() {
  tripsAt(4500);
  tripsAt(3150);
  missingRatingDoesNotTrip();
  staleDoesNotTripOrRelease();
  lowerCeilingCannotRaiseOverload();
  if (g_failures) {
    fprintf(stderr, "%d ac output checks failed\n", g_failures);
    return 1;
  }
  printf("ac output checks passed\n");
  return 0;
}
