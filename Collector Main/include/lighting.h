#pragma once
#include <Arduino.h>
#include "adcreadings.h"

extern const int RELAY_ON;
extern const int redLED;
extern const int greenLED;
extern const int whiteLED;
extern int currentLED;
extern const int PWM_CHANNEL;
extern const int PWM_FREQ;
extern const int PWM_RES;
extern const int STEP_INTERVAL;

bool inputPresent();
int updateLEDColor();
void breatheLED(int pin);