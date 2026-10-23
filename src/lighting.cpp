#include <housekeeping.h>



const int PWM_CHANNEL = 0;
const int PWM_FREQ    = 5000;
const int PWM_RES     = 8;
const int STEP_INTERVAL = 8;  // ms between brightness steps

void initLights(){
  pinMode(RED, OUTPUT);
  pinMode(GREEN, OUTPUT);
  pinMode(YELLOW, OUTPUT);
  pinMode(AMBER, OUTPUT);
  pinMode(LIGHTS, OUTPUT);
}

bool inputPresent()
{
  InverterSnapshot snap = getInverterStatus();
  if (snap.gridVoltage >= 160){
    return true;
  }
  return false;
}

int updateLEDColor()
{
  float percentage = bmsData.SOC;
  if (percentage >= 70)
    return GREEN;
  else if (percentage >= 40)
    return YELLOW;
  return RED;   // covers 0–40% and any out-of-range value
}

void lightUpStatus(){
  digitalWrite(RED, LOW);
  digitalWrite(GREEN, LOW);
  digitalWrite(YELLOW, LOW);
  digitalWrite(AMBER, LOW);
  digitalWrite(updateLEDColor(), HIGH);
}

void breatheLED(int pin) {
    static int     channel    = 0;
    static bool    initialized = false;
    static int     brightness  = 0;
    static int     direction   = 1;
    static unsigned long lastStep = 0;

    if (!initialized) {
        ledcSetup(channel, PWM_FREQ, PWM_RES);
        ledcAttachPin(pin, channel);
        initialized = true;
    }

    if (millis() - lastStep >= STEP_INTERVAL) {   // step every 8ms → full cycle ≈ 4s
        brightness += direction;
        if (brightness >= 255) direction = -1;
        if (brightness <= 0)   direction =  1;
        ledcWrite(channel, brightness);
        lastStep = millis();
    }
}