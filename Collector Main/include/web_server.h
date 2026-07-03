// #pragma once

// // ============================================================
// //  web_server.h  — HTTP/SSE server, JSON builder, and
// //                  dashboard HTML declarations.
// // ============================================================

// #include <ESPAsyncWebServer.h>

// // ── Shared server instance (defined in web_server.cpp) ───────────────────────
// extern AsyncWebServer server;

// // ── JSON state builder ────────────────────────────────────────────────────────
// String buildJSON();

// // ── HTTP route handlers ───────────────────────────────────────────────────────
// void handleRoot(AsyncWebServerRequest *request);
// void handleData(AsyncWebServerRequest *request);
// void handleEvents(AsyncWebServerRequest *request);
// void handleApprove(AsyncWebServerRequest *request);
// void handleReject(AsyncWebServerRequest *request);
// void handleRelay(AsyncWebServerRequest *request);
