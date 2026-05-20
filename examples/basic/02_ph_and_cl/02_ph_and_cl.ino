/*
 * APA-Dose Example — 02: pH + Chlorine Pumps  [BASIC]
 *
 * Two independent ApaDose instances sharing one filtration interlock.
 * Each pump manages its own sensor, setpoint, and alarm state.
 *
 *   phPump — pH- (acid), target pH 7.4
 *   clPump — chlorine/ORP, target ORP 700 mV
 *
 * This example also demonstrates shock / super-chlorination:
 *   Press PIN_SHOCK_BUTTON to trigger a standard weekly shock.
 *   The library doses clPump at full power until ORP reaches SHOCK_ORP_STANDARD
 *   (750 mV) or 4 hours elapse, then resumes normal proportional control.
 *   phPump is held during shock and resumes automatically when shock ends.
 *
 * Dosing type and direction are passed directly to begin() —
 * no separate setDosingType() call needed.
 *
 * Hardware:
 *   PIN_PH_PUMP       D9    pH pump MOSFET
 *   PIN_CL_PUMP       D10   chlorine pump MOSFET
 *   PIN_FILTER_RELAY  D2    filter running signal (HIGH = running)
 *   PIN_ALARM_LED     D13   shared alarm LED (on if either pump has alarm)
 *   PIN_ACK_BUTTON    D3    ACK button (INPUT_PULLUP)
 *   PIN_SHOCK_BUTTON  D4    Shock trigger button (INPUT_PULLUP)
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <APADOSE.h>

const uint8_t PIN_PH_PUMP      = 9;
const uint8_t PIN_CL_PUMP      = 10;
const uint8_t PIN_FILTER_RELAY = 2;
const uint8_t PIN_ALARM_LED    = 13;
const uint8_t PIN_ACK_BUTTON   = 3;
const uint8_t PIN_SHOCK_BUTTON = 4;  // dedicated shock trigger

// Each ApaDose instance must have a unique EEPROM base address, spaced by
// sizeof(ConfigData). Without unique addresses both pumps would overwrite
// the same bytes and corrupt each other's saved configuration on every boot.
ApaDose phPump(PIN_PH_PUMP);                                                // EEPROM 192 (default)
ApaDose clPump(PIN_CL_PUMP, APA_DOSE_EEPROM_ADDRESS + sizeof(ConfigData)); // EEPROM 212

float getpH()  { return 7.2; /* replace with phSensor.getPH()   */ }
float getORP() { return 620; /* replace with orpSensor.getORP() */ }

bool filterRunning() {
  return digitalRead(PIN_FILTER_RELAY) == HIGH;
}

// Both pumps can share a single callback — the message identifies which pump
// triggered it. For separate handling, register different callbacks per pump.
void onAlarm(ApaDoseAlarm type, const char* msg) {
  Serial.print("[ALARM] ");
  Serial.println(msg);
}

void onAlarmCleared(ApaDoseAlarm type, const char* msg) {
  Serial.println("[ALARM CLEARED]");
}

void onStatus(const char* msg) {
  Serial.print("[STATUS] ");
  Serial.println(msg);
}

bool lastAckState   = HIGH;
bool lastShockState = HIGH;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_ALARM_LED,    OUTPUT);
  pinMode(PIN_ACK_BUTTON,   INPUT_PULLUP);
  pinMode(PIN_SHOCK_BUTTON, INPUT_PULLUP);
  pinMode(PIN_FILTER_RELAY, INPUT);

  // --- pH pump setup ---
  phPump.setPumpRange(65, 255);  // 65 = PWM start threshold — measure for YOUR pump
  // Solenoid valve: use setPumpRange(255, 255) — PWM is fixed, pulse duration carries proportionality.
  phPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  // PH_MINUS = acid (lowers pH); use PH_PLUS for a base pump (raises pH).
  // Normal on first install — returns false when no valid config exists yet or EEPROM is corrupt.
  if (!phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20, 6))
    Serial.println("[INFO] phPump: no saved config — defaults loaded.");

  // --- Chlorine pump setup ---
  clPump.setPumpRange(65, 255);  // measure start threshold for this pump separately
  // Solenoid valve: use setPumpRange(255, 255).
  clPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  if (!clPump.begin(getORP, filterRunning, DOSE_CL, CL_PLUS, 20, 12))
    Serial.println("[INFO] clPump: no saved config — defaults loaded.");

  Serial.println(F("Ready. Press SHOCK button to trigger shock dosing."));
  Serial.println(F("SHOCK button requires: filter running, pH 7.0-7.6, ORP below target."));
}

void loop() {
  phPump.update();
  clPump.update();

  // LED on if either pump is alarming
  bool anyAlarm = phPump.isAlarmActive() || clPump.isAlarmActive();
  digitalWrite(PIN_ALARM_LED, anyAlarm ? HIGH : LOW);

  // --- ACK button: short press acknowledges first active alarm ---
  bool ackState = digitalRead(PIN_ACK_BUTTON);
  if (ackState == LOW && lastAckState == HIGH) {
    if      (phPump.isAlarmActive()) phPump.acknowledgeAlarm();
    else if (clPump.isAlarmActive()) clPump.acknowledgeAlarm();
    Serial.println("[ACK] Acknowledged.");
  }
  lastAckState = ackState;

  // --- SHOCK button: trigger hobbyist shock on rising edge ---
  bool shockBtn = digitalRead(PIN_SHOCK_BUTTON);
  if (shockBtn == LOW && lastShockState == HIGH) {
    // SHOCK_ORP_STANDARD = 750 mV (weekly maintenance target).
    // getpH() supplies the current pH — must be 7.0–7.6 for shock to start.
    // Returns false if any entry guard fails (see Serial output for reason via status callback).
    if (clPump.triggerShock(SHOCK_ORP_STANDARD, getpH())) {
      Serial.println(F("[SHOCK] Shock started — dosing at full power until ORP 750 mV."));
      Serial.println(F("[SHOCK] phPump held. Will resume automatically when shock ends."));
    } else {
      Serial.println(F("[SHOCK] Shock rejected — check: filter on? pH 7.0-7.6? ORP below target? No active alarm?"));
    }
  }
  lastShockState = shockBtn;

  // --- Print shock status once per minute while active ---
  static unsigned long lastShockPrint = 0;
  if (clPump.isShockActive() && millis() - lastShockPrint >= 60000UL) {
    lastShockPrint = millis();
    unsigned long rem = clPump.getShockRemainingSeconds();
    Serial.print(F("[SHOCK] Active — ORP: "));
    Serial.print((int)getORP());
    Serial.print(F(" mV, "));
    Serial.print(rem / 60);
    Serial.println(F(" min remaining to ceiling."));
  }
}
