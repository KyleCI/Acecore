#pragma once
#include <housekeeping.h>



extern int currentLED;
extern const int PWM_CHANNEL;
extern const int PWM_FREQ;
extern const int PWM_RES;
extern const int STEP_INTERVAL;

void initLights();
void lightUpStatus();
bool inputPresent();
int updateLEDColor();
void breatheLED(int pin);