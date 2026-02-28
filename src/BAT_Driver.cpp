#include "BAT_Driver.h"
#include <math.h>

#define CHARGING_VOLTAGE 4.0f
#define MAX_BAT_VOLTAGE 3.99f
#define MIN_BAT_VOLTAGE 3.0f
// Filter alpha 0.20 → ~25-second time constant (5s sample interval / 0.20).
// Faster than 0.10 so load-sag voltage recovers quickly when the display sleeps,
// preventing the "10% lost in 2 minutes" effect.  The wild swings (84→94→87→91)
// were caused by DISCHARGE_COMP_SECONDS=20 amplifying noise 20×, not by alpha;
// with that removed and 8-sample averaging in place, 0.20 is safe.
#define VOLTAGE_FILTER_ALPHA 0.20f
#define ADC_NUM_SAMPLES 8  // Average this many readings to suppress ADC noise

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
  static bool hasFilterState = false;
  long now = millis();

  if (lastRead + 5000 < now) {
    lastRead = now;

    // Average multiple ADC readings to reduce noise, especially after light-sleep
    // wakeup where the first sample is often several tens of mV off.
    long sumMv = 0;
    for (int i = 0; i < ADC_NUM_SAMPLES; i++) {
      sumMv += analogReadMilliVolts(BAT_ADC_PIN);
    }
    float measuredVoltage = (float)(sumMv / ADC_NUM_SAMPLES) * 3.0f / 1000.0f / Measurement_offset;
    voltage = measuredVoltage;

    isCharging = voltage > CHARGING_VOLTAGE;

    if (!hasFilterState) {
      filteredVoltage = measuredVoltage;
      hasFilterState = true;
    } else {
      filteredVoltage = (VOLTAGE_FILTER_ALPHA * measuredVoltage) +
                        ((1.0f - VOLTAGE_FILTER_ALPHA) * filteredVoltage);
    }

    // No discharge-rate compensation — the 20-second projection that was here
    // amplified ADC noise by 20× and caused the percentage to swing wildly
    // (e.g. 84 → 94 → 87 → 91) rather than decreasing smoothly.  The filtered
    // voltage on its own is accurate enough; it may read 1–5% lower under heavy
    // load (display on, WiFi active) due to battery internal-resistance sag, but
    // the faster alpha (0.20) means it recovers quickly once the display sleeps.
    float displayVoltage = filteredVoltage;
    if (displayVoltage > MAX_BAT_VOLTAGE) displayVoltage = MAX_BAT_VOLTAGE;
    if (displayVoltage < MIN_BAT_VOLTAGE) displayVoltage = MIN_BAT_VOLTAGE;

    chargePercentage = map_voltage_to_percent(displayVoltage);
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
