#pragma once

#include "home_snapshot.h"

// Fills the meters the mounted GET /data serializer publishes.
// Host tests build HomeMeters directly and do not call this.
void collect_home_meters(powercell::HomeMeters* out);
