#ifndef ADCREADINGS_H
#define ADCREADINGS_H
#include <Arduino.h>
#include <algorithm>
#include <PZEM004Tv30.h>


#define PZEM_RX_PIN 17
#define PZEM_TX_PIN 16
#define PZEM_SERIAL Serial2

extern PZEM004Tv30 pzem;

#define ADC_RES     4095.0f
#define ADC_VREF    3.3f
#define R1          220000.0f
#define R2          12000.0f
#define DIVIDER     (R2 / (R1 + R2)) 
#define CAL_FACTOR 1.065f

#define EMA_ALPHA 0.1f
extern const int OUT_VOLT;
extern const int IN_VOLT;
extern const int BAT;
extern const int CSENSE;

struct OutputReadings {
    float voltage;
    float current;
    float power;
    float energy;
    float frequency;
    float pf;
};


void  updateADC();          // call every loop()
float readBatVoltage();     // volts
float readSolarVoltage();   // volts
float readCurrent();        // volts (scale to amps with your sense resistor)
float getBatteryPercent();  // 0-100
OutputReadings readOutput();

#endif