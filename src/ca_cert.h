#pragma once
#include <Arduino.h>
// TLS trust anchors for api.acecore.tech (ISRG roots, from collector 1.0.4).
// Used by both the WebSocket and the OTA client. Review before the roots expire.
extern const char POWERCELL_CA_CERT[];
