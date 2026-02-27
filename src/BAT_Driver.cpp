#include "BAT_Driver.h"
#include <math.h>

#define CHARGING_VOLTAGE 4.0f
#define MAX_BAT_VOLTAGE 3.99f
#define MIN_BAT_VOLTAGE 3.0f
#define VOLTAGE_FILTER_ALPHA 0.25f
#define DISCHARGE_COMP_SECONDS 20.0f
#define MAX_DISCHARGE_RATE_V_PER_S 0.020f

typedef struct {
  float voltage;
  int percent;
} battery_curve_point_t;

static const battery_curve_point_t kBatteryCurve[] = {
    {4.00f, 100},
    {3.95f, 95},
    {3.90f, 90},
    {3.85f, 80},
    {3.80f, 70},
    {3.75f, 60},
    {3.70f, 50},
    {3.65f, 40},
    {3.60f, 30},
    {3.50f, 20},
    {3.40f, 10},
    {3.30f, 5},
    {3.20f, 2},
    {3.10f, 1},
    {3.00f, 0},
};

static int map_voltage_to_percent(float v) {
  const int points = sizeof(kBatteryCurve) / sizeof(kBatteryCurve[0]);

  if (v >= kBatteryCurve[0].voltage) return 100;
  if (v <= kBatteryCurve[points - 1].voltage) return 0;

  for (int i = 0; i < points - 1; i++) {
    const float vHigh = kBatteryCurve[i].voltage;
    const float vLow = kBatteryCurve[i + 1].voltage;
    if (v <= vHigh && v >= vLow) {
      const float span = vHigh - vLow;
      if (span <= 0.0f) return kBatteryCurve[i].percent;
      const float t = (v - vLow) / span;
      const float p = kBatteryCurve[i + 1].percent +
                      t * (kBatteryCurve[i].percent - kBatteryCurve[i + 1].percent);
      int percent = (int)lroundf(p);
      if (percent < 0) percent = 0;
      if (percent > 100) percent = 100;
      return percent;
    }
  }

  return 0;
}

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
  static float filteredVoltage = 0.0f;
  static float previousFilteredVoltage = 0.0f;
  static bool hasFilterState = false;
  static unsigned long previousSampleMs = 0;
  long now = millis();

  if (lastRead + 5000 < now) {
    lastRead = now;

    int voltsInt = analogReadMilliVolts(BAT_ADC_PIN);  // millivolts
    float measuredVoltage = (float)(voltsInt * 3.0f / 1000.0f) / Measurement_offset;
    voltage = measuredVoltage;

    isCharging = voltage > CHARGING_VOLTAGE;

    if (!hasFilterState) {
      filteredVoltage = measuredVoltage;
      previousFilteredVoltage = measuredVoltage;
      previousSampleMs = now;
      hasFilterState = true;
    } else {
      filteredVoltage = (VOLTAGE_FILTER_ALPHA * measuredVoltage) +
                        ((1.0f - VOLTAGE_FILTER_ALPHA) * filteredVoltage);
    }

    float compensatedVoltage = filteredVoltage;
    unsigned long dtMs = (previousSampleMs > 0) ? (now - previousSampleMs) : 0;
    if (!isCharging && dtMs > 0) {
      const float dtSec = (float)dtMs / 1000.0f;
      float dischargeRate = (previousFilteredVoltage - filteredVoltage) / dtSec;  // V/s
      if (dischargeRate < 0.0f) dischargeRate = 0.0f;
      if (dischargeRate > MAX_DISCHARGE_RATE_V_PER_S) {
        dischargeRate = MAX_DISCHARGE_RATE_V_PER_S;
      }

      // Compensate load sag by projecting the open-circuit voltage slightly upward.
      compensatedVoltage += dischargeRate * DISCHARGE_COMP_SECONDS;
    }

    if (compensatedVoltage > MAX_BAT_VOLTAGE) compensatedVoltage = MAX_BAT_VOLTAGE;
    if (compensatedVoltage < MIN_BAT_VOLTAGE) compensatedVoltage = MIN_BAT_VOLTAGE;

    chargePercentage = map_voltage_to_percent(compensatedVoltage);

    previousFilteredVoltage = filteredVoltage;
    previousSampleMs = now;

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
