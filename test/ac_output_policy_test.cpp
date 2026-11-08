#include "ac_output_policy.h"

#include <stdio.h>

using namespace powercell;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static AcWatts sample(float watts, uint32_t at, bool valid = true, bool coverage = false) {
  AcWatts s;
  s.valid = valid;
  s.coverage_verified = coverage;
  s.watts = watts;
  s.receivedAtMs = at;
  return s;
}

static void selectedPzemWarnsWithoutShutdown() {
  AcOutputPolicy p;
  p.setCeiling(500, AcMode::WarnAndShutdown);
  p.update(sample(800, 0), 0);
  CHECK(!p.warning());
  p.update(sample(800, 3000), 3000);
  CHECK(!p.warning());
  p.update(sample(800, 6000), 6000);
  CHECK(p.warning());
  CHECK(!p.shutdownLatched());
  CHECK(!p.takeShutdownEdge());
  CHECK(!p.shutdownOperational());
  CHECK(!p.turnsOutputOn());
  CHECK(!AcOutputPolicy::kDrivesOutput);
}

static void staleIsNotZeroAndDoesNotShutdown() {
  AcOutputPolicy p;
  p.setCeiling(500, AcMode::WarnAndShutdown);
  p.setGates(true, true);
  p.update(sample(50, 0, false), 10000);
  CHECK(p.coverageUnavailable());
  CHECK(!p.warning());
  CHECK(!p.shutdownLatched());
  p.update(sample(9000, 0, true), 5000);
  CHECK(p.coverageUnavailable());
  CHECK(!p.shutdownLatched());
}

static void coverageAndPolarityCanRequestShutdownWithoutTurningOn() {
  AcOutputPolicy p;
  p.setCeiling(500, AcMode::WarnAndShutdown);
  p.setGates(true, true);
  p.update(sample(800, 0, true, true), 0);
  p.update(sample(800, 3000, true, true), 3000);
  p.update(sample(800, 11000, true, true), 11000);
  CHECK(p.shutdownLatched());
  CHECK(p.takeShutdownEdge());
  CHECK(!p.takeShutdownEdge());
  CHECK(!p.turnsOutputOn());
  p.update(sample(100, 11000, false, true), 20000);
  CHECK(p.shutdownLatched());
  const uint32_t under = 21000;
  p.update(sample(100, under, true, true), under);
  p.update(sample(100, under + AcOutputPolicy::ReleaseHoldMs, true, true),
           under + AcOutputPolicy::ReleaseHoldMs);
  CHECK(!p.shutdownLatched());
  CHECK(!p.turnsOutputOn());
}

int main() {
  selectedPzemWarnsWithoutShutdown();
  staleIsNotZeroAndDoesNotShutdown();
  coverageAndPolarityCanRequestShutdownWithoutTurningOn();
  if (g_failures) {
    fprintf(stderr, "%d ac output checks failed\n", g_failures);
    return 1;
  }
  printf("ac output checks passed\n");
  return 0;
}
