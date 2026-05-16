/*
 * APA-Dose Example — 02: pH + Chlorine Pumps  [BASIC]
 *
 * Two independent ApaDose instances sharing one filtration interlock.
 * Each pump manages its own sensor, setpoint, and alarm state.
 *
 *   phPump — pH- (acid), target pH 7.4
 *   clPump — chlorine/ORP, target ORP 700 mV
 *
 * Key point: setDosingType(DOSE_CL) must be called BEFORE begin() on first
 * boot so the library saves ORP defaults (setpoint 700, band 100) to EEPROM
 * rather than pH defaults. On subsequent boots EEPROM is restored correctly.
 *
 * Hardware:
 *   PIN_PH_PUMP       D9    pH pump MOSFET
 *   PIN_CL_PUMP       D10   chlorine pump MOSFET
 *   PIN_FILTER_RELAY  D2    filter running signal (HIGH = running)
 *   PIN_ALARM_LED     D13   shared alarm LED (on if either pump has alarm)
 *   PIN_ACK_BUTTON    D3    ACK button (INPUT_PULLUP)
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <APADOSE.h>

const uint8_t PIN_PH_PUMP      = 9;
const uint8_t PIN_CL_PUMP      = 10;
const uint8_t PIN_FILTER_RELAY = 2;
const uint8_t PIN_ALARM_LED    = 13;
const uint8_t PIN_ACK_BUTTON   = 3;

// Each ApaDose instance must have a unique EEPROM base address, spaced by
// sizeof(ConfigData). Without unique addresses both pumps would overwrite
// the same bytes and corrupt each other's saved configuration on every boot.
ApaDose phPump(PIN_PH_PUMP);                                                // EEPROM 192 (default)
ApaDose clPump(PIN_CL_PUMP, APA_DOSE_EEPROM_ADDRESS + sizeof(ConfigData)); // EEPROM 206

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

bool lastButtonState = HIGH;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_ALARM_LED,    OUTPUT);
  pinMode(PIN_ACK_BUTTON,   INPUT_PULLUP);
  pinMode(PIN_FILTER_RELAY, INPUT);

  // --- pH pump setup ---
  phPump.setPumpRange(65, 255);        // 65 = PWM start threshold — measure for YOUR pump
  // Solenoid valve: use setPumpRange(255, 255) — PWM is fixed, pulse duration carries proportionality.
  phPump.setDosingType(DOSE_PH_MINUS); // acid — doses when pH is above setpoint
  // pH control is one-directional — never run DOSE_PH_PLUS and DOSE_PH_MINUS on the same pool.
  phPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  if (!phPump.begin(getpH, filterRunning, 20, 6))
    Serial.println("[WARN] phPump EEPROM invalid — defaults loaded.");

  // --- Chlorine pump setup ---
  // setDosingType(DOSE_CL) before begin() is critical on first boot:
  // it ensures ORP defaults (setpoint 700, band 100) are written to EEPROM
  // rather than pH defaults. On subsequent boots EEPROM is restored correctly.
  clPump.setPumpRange(65, 255);        // measure start threshold for this pump separately
  // Same note: for a solenoid valve use setPumpRange(255, 255).
  clPump.setDosingType(DOSE_CL);
  clPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  if (!clPump.begin(getORP, filterRunning, 20, 12))
    Serial.println("[WARN] clPump EEPROM invalid — defaults loaded.");
}

void loop() {
  phPump.update();
  clPump.update();

  // LED on if either pump is alarming
  bool anyAlarm = phPump.isAlarmActive() || clPump.isAlarmActive();
  digitalWrite(PIN_ALARM_LED, anyAlarm ? HIGH : LOW);

  // Short press: ACK first active alarm found
  bool buttonState = digitalRead(PIN_ACK_BUTTON);
  if (buttonState == LOW && lastButtonState == HIGH) {
    if      (phPump.isAlarmActive()) phPump.acknowledgeAlarm();
    else if (clPump.isAlarmActive()) clPump.acknowledgeAlarm();
    Serial.println("[ACK] Acknowledged.");
  }
  lastButtonState = buttonState;
}
