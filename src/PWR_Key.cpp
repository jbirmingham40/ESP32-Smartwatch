#include "PWR_Key.h"

static uint8_t BAT_State = 0;
static uint8_t Device_State = 0;
static uint16_t Long_Press = 0;

// Display idle-blanking state
static volatile unsigned long lastActivityTime = 0;  // updated from ISR and main loop
static bool displayAwake = true;

// Called from touch ISR and anywhere else user activity is detected.
// Only updates a timestamp — safe to call from an interrupt context.
void PWR_UpdateActivity(void) {
  lastActivityTime = millis();
}

bool PWR_IsDisplayAwake(void) {
  return displayAwake;
}

void Fall_Asleep(void) {
  displayAwake = false;
  Set_Backlight(0);
}

void Restart(void) {

}

void Shutdown(void) {
  digitalWrite(PWR_Control_PIN, LOW);
  Set_Backlight(0);
}

void PWR_Loop(void) {
  unsigned long now = millis();

  // --- Display idle timeout ---
  if (displayAwake) {
    if (now - lastActivityTime > SCREEN_TIMEOUT_MS) {
      Fall_Asleep();
    }
  } else {
    // Wake display when fresh activity arrives (touch ISR bumped lastActivityTime)
    if (now - lastActivityTime < 500) {
      displayAwake = true;
      Set_Backlight(LCD_Backlight);
    }
  }

  // --- Button long-press handling ---
  if (BAT_State) {
    if (!digitalRead(PWR_KEY_Input_PIN)) {
      // Button is held — counts as activity, wake display if asleep
      PWR_UpdateActivity();
      if (BAT_State == 2) {
        Long_Press++;
        if (Long_Press >= Device_Sleep_Time) {
          if (Long_Press >= Device_Sleep_Time && Long_Press < Device_Restart_Time)
            Device_State = 1;
          else if (Long_Press >= Device_Restart_Time && Long_Press < Device_Shutdown_Time)
            Device_State = 2;
          else if (Long_Press >= Device_Shutdown_Time)
            Shutdown();
        }
      }
    } else {
      if (BAT_State == 1)
        BAT_State = 2;
      Long_Press = 0;
    }
  }
}

void PWR_Init(void) {
  pinMode(PWR_KEY_Input_PIN, INPUT);
  pinMode(PWR_Control_PIN, OUTPUT);
  // Always latch power so the device stays on when USB is removed.
  // Shutdown() drives this LOW explicitly when a power-off is intended.
  digitalWrite(PWR_Control_PIN, HIGH);
  if (!digitalRead(PWR_KEY_Input_PIN)) {  // Button is active-low: pressed = LOW
    BAT_State = 1;  // Wait for button release before counting long-press
  } else {
    BAT_State = 2;  // USB/no-button boot: immediately ready for button events
  }
  lastActivityTime = millis();  // Start the idle timer from boot
}
