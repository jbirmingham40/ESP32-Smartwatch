#include "PWR_Key.h"
#include "BAT_Driver.h"

static uint8_t BAT_State = 0;
static uint8_t Device_State = 0;
static uint16_t Long_Press = 0;
static bool lastButtonPressed = false;

// Display idle-blanking state
static volatile unsigned long lastActivityTime = 0;  // updated from ISR and main loop
static volatile bool wakeRequestFromIsr = false;
static bool displayAwake = true;
static unsigned long displaySleepStartMs = 0;

// Called from touch ISR and anywhere else user activity is detected.
// Only updates a timestamp — safe to call from an interrupt context.
void PWR_UpdateActivity(void) {
  lastActivityTime = millis();
}

void PWR_RequestWakeFromISR(void) {
  wakeRequestFromIsr = true;
}

bool PWR_IsDisplayAwake(void) {
  return displayAwake;
}

unsigned long PWR_GetDisplaySleepMs(void) {
  if (displayAwake) return 0;
  return millis() - displaySleepStartMs;
}

void Fall_Asleep(void) {
  displayAwake = false;
  displaySleepStartMs = millis();
  wakeRequestFromIsr = false;
  // Force panel brightness off without modifying LCD_Backlight.
  // This preserves the user's configured brightness for wake restore.
  ledcWrite(LCD_Backlight_PIN, 0);
  LCD_Sleep(true);
}

void Restart(void) {

}

void Shutdown(void) {
  digitalWrite(PWR_Control_PIN, LOW);
  // Force panel brightness off without modifying LCD_Backlight.
  ledcWrite(LCD_Backlight_PIN, 0);
}

void PWR_Loop(void) {
  unsigned long now = millis();
  static unsigned long lastPowerLog = 0;
  static unsigned long lastLoopTs = 0;
  static unsigned long awakeAccumMs = 0;
  static unsigned long asleepAccumMs = 0;

  wakeRequestFromIsr = false;

  if (lastLoopTs != 0) {
    unsigned long dt = now - lastLoopTs;
    if (displayAwake) awakeAccumMs += dt;
    else asleepAccumMs += dt;
  }
  lastLoopTs = now;

  // --- Display idle timeout ---
  if (displayAwake) {
    if (now - lastActivityTime > SCREEN_TIMEOUT_MS) {
      Fall_Asleep();
    }
  } else {
    // Wake display when fresh activity arrives.
    if (now - lastActivityTime < 500) {
      displayAwake = true;
      displaySleepStartMs = 0;
      LCD_Sleep(false);
      Set_Backlight(LCD_Backlight);
    }
  }

  // --- Button long-press handling ---
  if (BAT_State) {
    bool buttonPressed = !digitalRead(PWR_KEY_Input_PIN);
    if (buttonPressed) {
      // Treat button as activity only on the press edge, and only once the
      // boot-time key release guard has finished (BAT_State == 2).
      // This prevents a noisy/stuck-low key pin from continuously resetting
      // screen idle timeout and causing high battery drain.
      if (BAT_State == 2 && !lastButtonPressed) {
        PWR_UpdateActivity();
      }
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
      lastButtonPressed = true;
    } else {
      if (BAT_State == 1)
        BAT_State = 2;
      Long_Press = 0;
      lastButtonPressed = false;
    }
  }

  if (now - lastPowerLog >= 60000) {
    lastPowerLog = now;
    unsigned long total = awakeAccumMs + asleepAccumMs;
    unsigned long awakePct = (total > 0) ? ((awakeAccumMs * 100UL) / total) : 0;
    Serial.printf("[Power] display=%s idle_ms=%lu backlight=%u key=%d awake_pct=%lu%%\n",
                  displayAwake ? "on" : "off",
                  (unsigned long)(now - lastActivityTime),
                  (unsigned)LCD_Backlight,
                  !digitalRead(PWR_KEY_Input_PIN),
                  awakePct);
    awakeAccumMs = 0;
    asleepAccumMs = 0;
  }
}

void PWR_Init(void) {
  // Active-low key input. Internal pull-up prevents floating/false-low reads
  // that can continuously reset activity and block display sleep.
  pinMode(PWR_KEY_Input_PIN, INPUT_PULLUP);
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
