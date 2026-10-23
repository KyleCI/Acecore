/* This file contains main functionality for BMS communication
    Reads Data from UART2, checks for the start of a valid frame(0x55, 0xAA, 0xEB, 0x90)
    Parses the data into the struct JKPacketData */

#include "pinDefines.h"
#include "bms.h"
#include <energy.h>
#include <math.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

JKPacketData bmsData = {0};

// --- Mutex protecting bmsData for cross-task access ---
static SemaphoreHandle_t bmsMutex = NULL;

// ── Little-endian readers ─────────────────────────────────────────────────────
static inline uint16_t le16u(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline int16_t le16s(const uint8_t* p) {
    return (int16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t le32u(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
static inline int32_t le32s(const uint8_t* p) {
    return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

// ── Scan UART for 55 AA EB 90 header ─────────────────────────────────────────
bool scanForHeader() {
    uint8_t match[4] = {0x55, 0xAA, 0xEB, 0x90};
    uint8_t idx = 0;

    uint32_t start = millis();

    while (millis() - start < SCAN_TIMEOUT_MS) {
        if (!JK_UART.available()) {
            vTaskDelay(pdMS_TO_TICKS(2));   // yield instead of busy-spinning
            continue;
        }

        uint8_t b = (uint8_t)JK_UART.read();

        if (b == match[idx]) {
            idx++;
            if (idx == 4) {
                frameBuf[0] = 0x55;
                frameBuf[1] = 0xAA;
                frameBuf[2] = 0xEB;
                frameBuf[3] = 0x90;
                return true;
            }
        } else {
            idx = (b == match[0]) ? 1 : 0;
        }
    }

    Serial.println("[Scan] Timeout — no 55 AA EB 90 found.");
    return false;
}

// ── Read exactly N bytes into frameBuf starting at offset ─────────────────────
bool readBytes(uint16_t offset, uint16_t count) {
    uint32_t start   = millis();
    uint16_t rxCount = 0;

    while (rxCount < count) {
        if (millis() - start > 2000) {
            Serial.printf("[RX] Timeout after %d/%d bytes.\n", rxCount, count);
            return false;
        }
        if (JK_UART.available()) {
            frameBuf[offset + rxCount] = (uint8_t)JK_UART.read();
            rxCount++;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));   // yield instead of busy-spinning
        }
    }
    return true;
}

// ── Parse DYNAMIC frame (0x1620) ──────────────────────────────────────────────
bool parseDynamic(const uint8_t* f, uint16_t fLen, JKPacketData* out) {
    if (f == nullptr || out == nullptr || fLen != FRAME_TOTAL_LEN) return false;
    if (f[0] != 0x55 || f[1] != 0xAA || f[2] != 0xEB || f[3] != 0x90 ||
        f[4] != FRAME_TYPE_DYNAMIC) return false;

    memset(out, 0, sizeof(JKPacketData));

    out->cellMinV  = 9.9f;
    out->cellMaxV  = 0.0f;
    out->cellCount = 0;

    for (int i = 0; i < 32; i++) {
        uint16_t raw = le16u(&f[6 + i * 2]);
        out->cellVoltage[i] = raw * 0.001f;
        if (raw > 500) {
            out->cellCount = i + 1;
            if (out->cellVoltage[i] > out->cellMaxV) out->cellMaxV = out->cellVoltage[i];
            if (out->cellVoltage[i] < out->cellMinV) out->cellMinV = out->cellVoltage[i];
        }
    }
    out->cellDiff = (out->cellMaxV - out->cellMinV) * 1000.0f;

    out->avgCellV = le16u(&f[74])  * 0.001f;
    out->maxCell  = f[78];
    out->minCell  = f[79];

    out->tempMOS     = le16s(&f[144]) * 0.1f;
    out->packVoltage = le32u(&f[150]) * 0.001f;
    out->packPower   = le32s(&f[154]) * 0.001f;
    out->packCurrent = le32s(&f[158]) * 0.001f;
    out->tempBat1    = le16s(&f[162]) * 0.1f;
    out->tempBat2    = le16s(&f[164]) * 0.1f;
    out->alarmBits   = le32u(&f[166]);
    out->batStatus   = f[172];
    out->SOC         = f[173];

    out->remainCapacity = le32s(&f[174]) * 0.001f;
    out->fullCapacity   = le32u(&f[178]) * 0.001f;
    out->cycleCount     = le32u(&f[182]);
    out->SOH            = f[190];

    out->chargeMOS    = f[198];
    out->dischargeMOS = f[199];
    out->balanceMOS   = f[200];

    // A zero SOC is legitimate only in a plausible, complete BMS frame.
    out->valid = out->SOC <= 100 && out->cellCount > 0 &&
                 isfinite(out->packVoltage) && out->packVoltage > 0.0f;
    return out->valid;
}

// ── Print ─────────────────────────────────────────────────────────────────────
void printBmsData(const JKPacketData* s) {
    if (!s->valid) { Serial.println("[BMS] Invalid."); return; }

    Serial.println("\n[BMS] ── Cell Voltages ─────────────────────────────");
    for (int i = 0; i < s->cellCount; i++) {
        Serial.printf("  Cell %02d : %.3f V%s\n",
                      i + 1, s->cellVoltage[i],
                      (i == s->maxCell) ? " <- MAX" :
                      (i == s->minCell) ? " <- MIN" : "");
    }
    Serial.printf("  Max   : %.3f V  (Cell %d)\n", s->cellMaxV, s->maxCell + 1);
    Serial.printf("  Min   : %.3f V  (Cell %d)\n", s->cellMinV, s->minCell + 1);
    Serial.printf("  Delta : %.1f mV\n",            s->cellDiff);
    Serial.printf("  Avg   : %.3f V\n",             s->avgCellV);

    Serial.println("[BMS] ── Pack ───────────────────────────────────────");
    Serial.printf("  Voltage : %.3f V\n",  s->packVoltage);
    Serial.printf("  Current : %.3f A\n",  s->packCurrent);
    Serial.printf("  Power   : %.1f W\n",  s->packPower);

    Serial.println("[BMS] ── Temperatures ──────────────────────────────");
    Serial.printf("  MOS   : %.1f C\n",  s->tempMOS);
    Serial.printf("  Bat 1 : %.1f C\n",  s->tempBat1);
    Serial.printf("  Bat 2 : %.1f C\n",  s->tempBat2);

    Serial.println("[BMS] ── State ──────────────────────────────────────");
    const char* sta[] = {"Standby", "Charging", "Discharging"};
    Serial.printf("  Status : %s\n",      s->batStatus < 3 ? sta[s->batStatus] : "?");
    Serial.printf("  SOC    : %u %%\n",   s->SOC);
    Serial.printf("  SOH    : %u %%\n",   s->SOH);
    Serial.printf("  Remain : %.2f Ah\n", s->remainCapacity);
    Serial.printf("  Full   : %.2f Ah\n", s->fullCapacity);
    Serial.printf("  Cycles : %u\n",      s->cycleCount);

    Serial.println("[BMS] ── FET ────────────────────────────────────────");
    Serial.printf("  Charge    : %s\n", s->chargeMOS    ? "ON" : "OFF");
    Serial.printf("  Discharge : %s\n", s->dischargeMOS ? "ON" : "OFF");
    Serial.printf("  Balance   : %s\n", s->balanceMOS   ? "ON" : "OFF");

    if (s->alarmBits)
        Serial.printf("[BMS] !! ALARM: 0x%08X\n", s->alarmBits);

    Serial.println("[BMS] ─────────────────────────────────────────────\n");
}

// ── Passive scan and parse (called from within the task, not the main loop) ──
static void readAndParse() {
    JKPacketData localData;

    if (!scanForHeader()) return;
    if (!readBytes(4, FRAME_TOTAL_LEN - 4)) return;

    uint8_t frameType = frameBuf[4];

    if (frameType == FRAME_TYPE_DYNAMIC) {
    Serial.printf("[Frame] DYNAMIC (0x1620) — %d bytes\n", FRAME_TOTAL_LEN);
    if (parseDynamic(frameBuf, FRAME_TOTAL_LEN, &localData)) {
        if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            localData.receivedAtMs = millis();
            bmsData = localData;
            xSemaphoreGive(bmsMutex);
        }

        // ── Energy tracking (discharge only, 3s frame interval) ──────────
        energy_update(localData.packVoltage, localData.packCurrent, 3.0f);
        energy_save_if_due();
        // ─────────────────────────────────────────────────────────────────

    } else {
        Serial.println("[Parser] DYNAMIC parse failed.");
    }

    } else if (frameType == FRAME_TYPE_SETUP) {
        Serial.println("[Frame] SETUP (0x161E) — skipping.");

    } else {
        Serial.printf("[Frame] Unknown type: 0x%02X — discarding.\n", frameType);
    }
}

// ── Background task ────────────────────────────────────────────────────────
static void bmsTask(void *pvParameters) {
    for (;;) {
        readAndParse();
        vTaskDelay(pdMS_TO_TICKS(1));   // brief yield between scan attempts
    }
}

// ── Call once from setup(), after JK_UART.begin(...) ─────────────────────────
void startBmsTask() {
    bmsMutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(
        bmsTask,
        "bmsTask",
        4096,     // stack — parseDynamic's 32-entry cell array lives on it briefly via localData
        NULL,
        1,        // priority — same tier as other sensor tasks, below AsyncTCP/WiFi
        NULL,
        1         // core 1, leave core 0 for WiFi/BLE
    );
}

// ── Thread-safe snapshot for other tasks (e.g. telemetry/web server) ─────────
bool getBmsData(JKPacketData &out) {
    if (bmsMutex == nullptr) return false;
    bool ok = false;
    if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        out = bmsData;
        ok = out.valid;
        xSemaphoreGive(bmsMutex);
    }
    return ok;
}
