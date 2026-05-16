/*
 * APA-Dose Example — 01: Single pH Pump  [BASIC]
 *
 * One pH- (acid) pump keeping pool pH at 7.4.
 * Demonstrates the minimum recommended wiring for a production install:
 *   - Filtration interlock (no dosing without circulation)
 *   - 20-minute startup blackout (prevents double-dose after power cycle)
 *   - Alarm LED driven by continuous polling
 *   - ACK button for latching alarms
 *   - All callbacks registered BEFORE begin()
 *
 * Hardware:
 *   PIN_PH_PUMP       D9    MOSFET gate → peristaltic pump
 *   PIN_FILTER_RELAY  D2    Filter running signal (HIGH = running)
 *   PIN_ALARM_LED     D13   Alarm indicator LED
 *   PIN_ACK_BUTTON    D3    Momentary button, normally open (INPUT_PULLUP)
 *
 * Replace getpH() with your pH sensor library call.
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <APADOSE.h>

const uint8_t PIN_PH_PUMP      = 9;
const uint8_t PIN_FILTER_RELAY = 2;
const uint8_t PIN_ALARM_LED    = 13;
const uint8_t PIN_ACK_BUTTON   = 3;

ApaDose phPump(PIN_PH_PUMP);

// --- Bridge your pH sensor library here ---
float getpH() {
  // return phSensor.getPH();
  return 7.2;  // placeholder
}

// If the filter stays off for more than FILTER_OFF_ALARM_MS (30 min), a
// status message fires once via onStatus. Dosing resumes automatically when
// the filter turns back on. No manual intervention needed.
bool filterRunning() {
  return digitalRead(PIN_FILTER_RELAY) == HIGH;
}

// --- Callbacks ---
// onAlarmTriggered fires once the moment the alarm is raised.
// Use it for immediate notification: first log line, buzzer start.
void onAlarm(ApaDoseAlarm type, const char* msg) {
  Serial.print("[ALARM] ");
  Serial.println(msg);
}

// onAlarmCleared fires when the alarm is resolved.
void onAlarmCleared(ApaDoseAlarm type, const char* msg) {
  Serial.println("[ALARM CLEARED] Dosing resumed.");
}

void onStatus(const char* msg) {
  Serial.print("[STATUS] ");
  Serial.println(msg);
}

bool lastButtonState = HIGH;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_ALARM_LED,    OUTPUT);
  pinMode(PIN_ACK_BUTTON,   INPUT_PULLUP);
  pinMode(PIN_FILTER_RELAY, INPUT);

  phPump.setPumpRange(65, 255);        // 65 = PWM where YOUR pump starts spinning — measure it
  // Solenoid valve: use setPumpRange(255, 255) — PWM is fixed at 255 (fully open),
  // proportionality comes from pulse duration only (time-proportional mode).
  phPump.setDosingType(DOSE_PH_MINUS); // acid — doses when pH is above setpoint
  // pH control is one-directional — never run DOSE_PH_PLUS and DOSE_PH_MINUS on the same pool.

  // Register callbacks BEFORE begin() so startup messages are not missed
  phPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);

  // Sensor callback + filter interlock + 20 min blackout + max 6 doses/day
  // Returns false when EEPROM was corrupt — defaults are loaded automatically, safe to continue.
  if (!phPump.begin(getpH, filterRunning, 20, 6))
    Serial.println("[WARN] EEPROM invalid — defaults loaded.");
}

void loop() {
  phPump.update();  // non-blocking — call every iteration

  // Drive alarm LED by polling, not only callback.
  // Polling guarantees the LED stays in sync even if the callback was missed.
  digitalWrite(PIN_ALARM_LED, phPump.isAlarmActive() ? HIGH : LOW);

  // ACK button clears latching alarms: WRONG_DIRECTION, INEFFECTIVE, DAILY_LIMIT.
  // ALARM_SAFETY_BAND clears automatically — no button needed.
  bool buttonState = digitalRead(PIN_ACK_BUTTON);
  if (buttonState == LOW && lastButtonState == HIGH) {
    phPump.acknowledgeAlarm();
    Serial.println("[ACK] Alarm acknowledged.");
  }
  lastButtonState = buttonState;
}
