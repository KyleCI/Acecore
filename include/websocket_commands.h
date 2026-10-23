#pragma once
#include <housekeeping.h>

// Initializes the secure WebSocket command channel.
void initWebSocketCommands();

// Must be called frequently from loop().
void serviceWebSocketCommands();

// Returns true only when the WSS connection is established.
bool webSocketCommandsConnected();

// Queue a JSON telemetry message for WebSocket transmission.
bool publishWebSocketTelemetry(const String& json);

// Notify the WebSocket layer that a socket relay has physically changed state.
void notifyWebSocketSocketResult(const String &deviceId, bool relayOn);

// Queue a current gateway/socket state snapshot for the server.
void publishWebSocketState();
