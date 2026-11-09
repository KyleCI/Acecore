#pragma once

// The factory chooses the image. Zero means this image has no rated system.
// 5000 and 3500 are the only values that arm overload. Usable watts are
// derived from those two and are not a second setting.
#ifndef POWERCELL_FACTORY_RATED_ACTIVE_W
#define POWERCELL_FACTORY_RATED_ACTIVE_W 0
#endif

#ifndef POWERCELL_FACTORY_VARIANT
#define POWERCELL_FACTORY_VARIANT "unrated"
#endif

#if POWERCELL_FACTORY_RATED_ACTIVE_W != 0 && POWERCELL_FACTORY_RATED_ACTIVE_W != 5000 && \
    POWERCELL_FACTORY_RATED_ACTIVE_W != 3500
#error POWERCELL_FACTORY_RATED_ACTIVE_W must be 0, 5000, or 3500
#endif
