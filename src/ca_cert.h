#pragma once
#include <Arduino.h>
// TLS trust anchors (ISRG roots, from collector 1.0.4). Let's Encrypt chains for
// api-dev.acecore.tech (live Upload cloud) and api.acecore.tech (esp32dev_prod).
// Used by both the WebSocket and the OTA client. Review before the roots expire.
extern const char POWERCELL_CA_CERT[];
