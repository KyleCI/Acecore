#pragma once
// v13: one shared 4 KB JSON serialization buffer (WS frames + LAN snapshot), replacing
// the v12 static 8 KB WS buffer and the separate 4 KB snapshot buffer.
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
namespace jscratch {
constexpr size_t kSize = 4096;
char *buf();                       // valid only between take() and give()
bool take(uint32_t ms);
void give();
}
