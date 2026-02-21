#include "BAT_Driver.h"

#define CHARGING_VOLTAGE 4.0f
#define MAX_BAT_VOLTAGE 3.95f
#define MIN_BAT_VOLTAGE 3.0f

float voltage = 0.0;
int chargePercentage;
char chargePercentageStr[5] = { 0 };
bool isCharging = false;

void BAT_Init(void) {
  //set the resolution to 12 bits (0-4095)
  analogReadResolution(12);
}

void BAT_Get_Volts(void) {
  static long lastRead = 0;
  long now = millis();

  if (lastRead + 5000 < now) {
    lastRead = now;

    int voltsInt = analogReadMilliVolts(BAT_ADC_PIN);  // millivolts
    voltage = (float)(voltsInt * 3.0 / 1000.0) / Measurement_offset;

    isCharging = voltage > CHARGING_VOLTAGE;

    if (voltage > MAX_BAT_VOLTAGE) {
      chargePercentage = 100;
    } else if (voltage < MIN_BAT_VOLTAGE) {
      chargePercentage = 0;
    } else {
      chargePercentage = ((voltage - MIN_BAT_VOLTAGE) / (MAX_BAT_VOLTAGE - MIN_BAT_VOLTAGE)) * 100;
    }
    sprintf(chargePercentageStr, "%d%%", (int)chargePercentage);
  }
}

bool BAT_Is_Charging(void) {
  return isCharging;
}

const char *BAT_Get_Charge_Percentage(void) {
  return chargePercentageStr;
}

const char *BAT_Get_Charge_Percentage_Str(void) {
  return chargePercentageStr;
}
