#pragma once

class AsyncWebServer;

void manualApiBegin();
void manualRegister(AsyncWebServer& server);
int manualDataStatus(const char* authorization);
