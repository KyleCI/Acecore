#pragma once

// The factory chooses the image. Zero means this image has no rated system.
// 5000, 3500 and 1500 are the only values that arm overload. Usable watts are
// derived from those three and are not a second setting.
// 1500 (Powercell Prime 1.5 kVA, 2.5.33) is PROVISIONAL: scaled from 3.5 kVA
// pending the real 1.5 kVA hardware specs.
#ifndef POWERCELL_FACTORY_RATED_ACTIVE_W
#define POWERCELL_FACTORY_RATED_ACTIVE_W 0
#endif

#ifndef POWERCELL_FACTORY_VARIANT
#define POWERCELL_FACTORY_VARIANT "unrated"
#endif

#if POWERCELL_FACTORY_RATED_ACTIVE_W != 0 && POWERCELL_FACTORY_RATED_ACTIVE_W != 5000 && \
    POWERCELL_FACTORY_RATED_ACTIVE_W != 3500 && POWERCELL_FACTORY_RATED_ACTIVE_W != 1500
#error POWERCELL_FACTORY_RATED_ACTIVE_W must be 0, 5000, 3500, or 1500
#endif
