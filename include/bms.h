#pragma once
#include <housekeeping.h>

// ── Frame constants ───────────────────────────────────────────────────────────
#define FRAME_HEADER_LEN   4      // 55 AA EB 90
#define FRAME_TOTAL_LEN    300    // fixed per JK-PB spec
#define FRAME_TYPE_DYNAMIC 0x02   // f[4] = 0x02 → DYNAMIC (0x1620)
#define FRAME_TYPE_SETUP   0x01   // f[4] = 0x01 → SETUP   (0x161E)
#define SCAN_TIMEOUT_MS    5000   // give up scanning if no header seen in 5s



// ── BMS Data Struct ───────────────────────────────────────────────────────────
typedef struct {
    float    cellVoltage[32];       
    uint8_t  cellCount;
    float    cellMaxV;
    float    cellMinV;
    float    cellDiff;
    float    avgCellV;
    uint8_t  maxCell;
    uint8_t  minCell;
    float    packVoltage;               //Voltage of the entire battery pack
    float    packCurrent;              //Current of the entire battery pack
    float    packPower;               //Power of the entire battery pack(Calculated from pack Voltage x pack Current)
    float    tempMOS;
    float    tempBat1;
    float    tempBat2;
    uint8_t  SOC;                     // Battery Percentage
    uint8_t  SOH;                    //Battery Health
    uint8_t  batStatus;             //Charging/Discharging/Idle
    float    remainCapacity;
    float    fullCapacity;
    uint32_t cycleCount;
    bool     chargeMOS;
    bool     dischargeMOS;             
    bool     balanceMOS;
    uint32_t alarmBits; 
    bool     valid;
    uint32_t receivedAtMs;             // millis() when this valid frame was published
} JKPacketData;



// ── Frame buffer — exactly header + 300 bytes ─────────────────────────────────
static uint8_t  frameBuf[FRAME_TOTAL_LEN];

extern JKPacketData bmsData;


void rs485Init();
bool scanForHeader();
bool readBytes(uint16_t offset, uint16_t count);
bool parseDynamic(const uint8_t* f, uint16_t fLen, JKPacketData* out);
void printBmsData(const JKPacketData* s);
static void readAndParse();
void startBmsTask();
bool getBmsData(JKPacketData &out);
