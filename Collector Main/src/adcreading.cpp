#include "adcreadings.h"
#include <algorithm>
#include <Arduino.h>


PZEM004Tv30 pzem(PZEM_SERIAL, PZEM_RX_PIN, PZEM_TX_PIN);

// ─── Lead Acid 12V Discharge Curve ───────────────────────────────────────────
static const float voltageTable[][2] = {
  { 42.0f,  0  },
  { 44.0f,  5  },
  { 46.0f,  10 },
  { 47.0f,  20 },
  { 48.0f,  30 },
  { 49.0f,  40 },
  { 50.0f,  50 },
  { 51.0f,  60 },
  { 52.0f,  70 },
  { 53.0f,  80 },
  { 54.0f,  90 },
  { 55.0f,  95 },
  { 56.0f, 100 },
};
static const int TABLE_SIZE = sizeof(voltageTable) / sizeof(voltageTable[0]);

// ─── EMA State ────────────────────────────────────────────────────────────────
static float emaBat     = 0.0f;
static float emaSolar   = 0.0f;
static float emaCurrent = 0.0f;

// ─── Raw ADC (oversampled) ────────────────────────────────────────────────────
static float readADC(int pin)
{
  long sum = 0;
  for (int i = 0; i < 16; i++)
    sum += analogRead(pin);
  return (sum / 16.0f) * (ADC_VREF / ADC_RES);
}

// ─── Median Filter ────────────────────────────────────────────────────────────
static float medianADC(int pin, int samples = 9)
{
  float buf[9];
  for (int i = 0; i < samples; i++)
  {
    buf[i] = analogRead(pin);
    delay(1);
  }
  std::sort(buf, buf + samples);
  return buf[samples / 2] * (ADC_VREF / ADC_RES);
}

// ─── Stable Read (median → EMA) ───────────────────────────────────────────────
static float stableRead(int pin, float &ema)
{
  float raw = medianADC(pin);
  ema = EMA_ALPHA * raw + (1.0f - EMA_ALPHA) * ema;
  return ema;
}

// ─── Voltage → Percentage ─────────────────────────────────────────────────────
static float voltageToPercent(float voltage)
{
  if (voltage <= voltageTable[0][0])             return 0.0f;
  if (voltage >= voltageTable[TABLE_SIZE-1][0])  return 100.0f;

  for (int i = 1; i < TABLE_SIZE; i++)
  {
    if (voltage <= voltageTable[i][0])
    {
      float t = (voltage - voltageTable[i-1][0]) /
                (voltageTable[i][0] - voltageTable[i-1][0]);
      return voltageTable[i-1][1] + t * (voltageTable[i][1] - voltageTable[i-1][1]);
    }
  }
  return 100.0f;
}

// ─── Public API ───────────────────────────────────────────────────────────────
void updateADC()
{
  stableRead(BAT,     emaBat);
  stableRead(IN_VOLT, emaSolar);
  stableRead(CSENSE,  emaCurrent);
}

float readBatVoltage()
{
  return (emaBat / DIVIDER)* CAL_FACTOR;;
}

float readSolarVoltage()
{
  return (emaSolar / DIVIDER)* CAL_FACTOR;;
}

float readCurrent()
{
  return emaCurrent;
}

float getBatteryPercent()
{
  // No Serial print here: this is called from the sensor task, the LED
  // status update, and every web request. Logging belongs at the sample
  // site (sensor task / loop), not in a hot getter.
  float voltage = readBatVoltage();
  return voltageToPercent(voltage);
}



OutputReadings readOutput() {
    OutputReadings r = {0, 0, 0, 0, 0, 0};  // default all to 0

    float v = pzem.voltage();
    float i = pzem.current();
    float p = pzem.power();
    float e = pzem.energy();
    float f = pzem.frequency();
    float pf = pzem.pf();

    r.voltage   = isnan(v)  ? 0.0f : v;
    r.current   = isnan(i)  ? 0.0f : i;
    r.power     = isnan(p)  ? 0.0f : p;
    r.energy    = isnan(e)  ? 0.0f : e;
    r.frequency = isnan(f)  ? 0.0f : f;
    r.pf        = isnan(pf) ? 0.0f : pf;

    return r;
}

// usage

