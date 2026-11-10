#include "json_scratch.h"
namespace jscratch {
namespace { char g_buf[kSize]; SemaphoreHandle_t g_mx = nullptr; portMUX_TYPE g_init = portMUX_INITIALIZER_UNLOCKED; }
char *buf() { return g_buf; }
bool take(uint32_t ms) {
  if (!g_mx) { SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex(); portENTER_CRITICAL(&g_init); if (!g_mx) { g_mx = m; m = nullptr; } portEXIT_CRITICAL(&g_init); if (m) vSemaphoreDelete(m); }
  return xSemaphoreTakeRecursive(g_mx, pdMS_TO_TICKS(ms)) == pdTRUE;
}
void give() { xSemaphoreGiveRecursive(g_mx); }
}
