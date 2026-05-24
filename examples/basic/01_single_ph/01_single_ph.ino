/*
 * APA-Dose Example — 01: Single pH Pump  [BASIC]
 *
 * One pH- (acid) pump keeping pool pH at 7.4.
 * Demonstrates the minimum recommended wiring for a production install:
 *   - Filtration interlock (no dosing without circulation)
 *   - 20-minute startup blackout (prevents double-dose after power cycle)
 *   - Tank empty sensor (ALARM_TANK_EMPTY — latching, blocks dosing and priming)
 *   - Alarm LED driven by continuous polling
 *   - ACK button for latching alarms
 *   - All callbacks registered BEFORE begin()
 *
 * Hardware:
 *   PIN_PH_PUMP       D9    MOSFET gate → peristaltic pump
 *   PIN_FILTER_RELAY  D2    Filter running signal (HIGH = running)
 *   PIN_TANK_SENSOR   D4    Float switch: GND one leg, D4 the other (INPUT_PULLUP)
 *                           Float up (tank full)  → switch open  → pin HIGH
 *                           Float down (tank empty) → switch closed → pin LOW
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
const uint8_t PIN_TANK_SENSOR  = 4;
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
bool filterRunning() { return digitalRead(PIN_FILTER_RELAY) == HIGH; }

// Float up (tank full) → switch open → pin HIGH → returns false.
// Float down (tank empty) → switch closed → pin LOW → returns true → ALARM_TANK_EMPTY.
// Why: running a peristaltic pump dry (no liquid) wears out the rollers fast and can
// permanently damage the pump head within hours. The library blocks dosing the moment
// this returns true and fires ALARM_TANK_EMPTY so you know to refill.
bool tankEmpty() { return digitalRead(PIN_TANK_SENSOR) == LOW; }

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
  pinMode(PIN_TANK_SENSOR,  INPUT_PULLUP);

  phPump.setPumpRange(65, 255);  // 65 = PWM where YOUR pump starts spinning — measure it
  // Solenoid valve: use setPumpRange(255, 255) — PWM is fixed at 255 (fully open),
  // proportionality comes from pulse duration only (time-proportional mode).

  // Register callbacks BEFORE begin() so startup messages are not missed
  phPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  phPump.setTankEmptyCallback(tankEmpty);

  // Sensor + filter + type + direction + 20 min blackout + max 6 doses/day
  // PH_MINUS = acid pump (lowers pH); use PH_PLUS for a base pump (raises pH).
  // Returns false on first install (blank EEPROM) or after EEPROM corruption — safe to ignore.
  // Defaults loaded: setpoint 7.4, proportional band 1.0 pH. Dosing starts immediately with
  // these values; use setSetpoint() / setProportionalBand() in setup() to override if needed.
  if (!phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20, 6))
    Serial.println("[INFO] No saved config — defaults loaded (SP 7.4, PB 1.0).");

  // --- Pool size scaling (call AFTER begin) ---
  // The library is calibrated for a 20 m³ reference pool.
  // If your pool is 30 m³ or smaller: leave this commented out — defaults work fine.
  // If your pool is larger than ~30 m³: uncomment and set your volume. Without it,
  // dose pulses are too short for the larger water volume and pH will never converge.
  // This setting is shared across all pump instances and survives factoryReset().
  // ApaDose::setPoolVolume(35);  // uncomment and set to YOUR pool volume in m³ (10–90)

  // --- Dose efficiency threshold (call AFTER begin, optional) ---
  // After 3 warm-up doses the library learns how much each dose normally shifts the sensor.
  // If a later dose achieves less than this % of that learned baseline, ALARM_INEFFECTIVE fires.
  // Typical cause: empty chemical tank, blocked tube, or pump head failure.
  // Default is 20 % (active automatically after warm-up). Pass 0 to disable.
  // phPump.setEfficiencyThreshold(20);  // default — shown here for clarity
}

void loop() {
  phPump.update();  // non-blocking — call every iteration

  // Drive alarm LED by polling, not only callback.
  // Polling guarantees the LED stays in sync even if the callback was missed.
  digitalWrite(PIN_ALARM_LED, phPump.isAlarmActive() ? HIGH : LOW);

  // ACK button clears latching alarms: WRONG_DIRECTION, INEFFECTIVE, TANK_EMPTY.
  // ALARM_DAILY_LIMIT auto-clears at midnight when the daily counter resets — no ACK needed.
  // ALARM_INEFFECTIVE can fire two ways: sensor showed no meaningful response on 3
  // consecutive doses (cold-start), or the last dose fell below the EMA delivery
  // baseline (default 20% threshold — see setEfficiencyThreshold() in setup above).
  // Call getDoseEffectiveness() to read the last dose as a % of the learned baseline.
  // ALARM_SAFETY_BAND clears automatically — no button needed.
  bool buttonState = digitalRead(PIN_ACK_BUTTON);
  if (buttonState == LOW && lastButtonState == HIGH) {
    phPump.acknowledgeAlarm();
    Serial.println("[ACK] Alarm acknowledged.");
  }
  lastButtonState = buttonState;
}
