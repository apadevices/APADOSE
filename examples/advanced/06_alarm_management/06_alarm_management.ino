/*
 * APA-Dose Example — 06: Alarm Management  [ADVANCED]
 *
 * Reference implementation for complete alarm handling in a pH + CL system.
 * Use this as the template for any production pool controller.
 *
 * Demonstrates all three layers of alarm interaction:
 *
 *   1. CALLBACK (push) — onAlarmTriggered fires once the moment an alarm
 *      is raised. Use it for immediate response: start buzzer, first log line.
 *      onAlarmCleared fires when the alarm resolves.
 *
 *   2. POLLING (pull) — isAlarmActive() queried every loop(). Drives LED and
 *      buzzer continuously. Correct even if the callback fired before
 *      peripherals were initialised, or was missed for any other reason.
 *
 *   3. ACKNOWLEDGMENT — physical button wired to acknowledgeAlarm().
 *      Short press: ACK first active alarm.
 *      Long press (2 s): ACK all alarms at once (useful after power events).
 *
 * Alarm behaviour summary:
 *   ALARM_WRONG_DIRECTION  — requires ACK button
 *   ALARM_INEFFECTIVE      — requires ACK button
 *   ALARM_DAILY_LIMIT      — requires ACK button
 *   ALARM_SAFETY_BAND      — auto-clears when sensor returns to safe range
 *   ALARM_INVALID_PARAM    — never latches; silent rejection only
 *
 * Hardware:
 *   PIN_PH_PUMP       D9
 *   PIN_CL_PUMP       D10
 *   PIN_FILTER_RELAY  D2
 *   PIN_ALARM_LED     D13   on while any alarm is active
 *   PIN_BUZZER        D8    active-high buzzer
 *   PIN_ACK_BUTTON    D3    INPUT_PULLUP, active LOW
 *                           short press = ack first alarm
 *                           long press  = ack all alarms
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <APADOSE.h>

const uint8_t  PIN_PH_PUMP       = 9;
const uint8_t  PIN_CL_PUMP       = 10;
const uint8_t  PIN_FILTER_RELAY  = 2;
const uint8_t  PIN_ALARM_LED     = 13;
const uint8_t  PIN_BUZZER        = 8;
const uint8_t  PIN_ACK_BUTTON    = 3;
const uint16_t LONG_PRESS_MS     = 2000;

// Each ApaDose instance must have a unique EEPROM base address, spaced by
// sizeof(ConfigData). Without unique addresses both pumps would overwrite
// the same bytes and corrupt each other's saved configuration on every boot.
ApaDose phPump(PIN_PH_PUMP);                                                // EEPROM 192 (default)
ApaDose clPump(PIN_CL_PUMP, APA_DOSE_EEPROM_ADDRESS + sizeof(ConfigData)); // EEPROM 206

float getpH()         { return 7.2; /* replace */ }
float getORP()        { return 640; /* replace */ }
bool  filterRunning() { return digitalRead(PIN_FILTER_RELAY) == HIGH; }

// Returns true for alarms that latch until the user presses ACK.
bool requiresAck(ApaDoseAlarm type) {
  return type == ALARM_WRONG_DIRECTION ||
         type == ALARM_INEFFECTIVE     ||
         type == ALARM_DAILY_LIMIT;
}

void logAlarm(const char* pumpName, ApaDoseAlarm type, const char* msg) {
  Serial.print(F("[t="));
  Serial.print(millis() / 1000);
  Serial.print(F("s]["));
  Serial.print(pumpName);
  Serial.print(F("] ALARM: "));
  Serial.println(msg);
  Serial.println(requiresAck(type)
                 ? F("  -> Press ACK button to resume dosing.")
                 : F("  -> Will clear automatically when sensor recovers."));
}

// --- pH alarm callbacks ---
void onPhAlarm(ApaDoseAlarm type, const char* msg) {
  logAlarm("pH", type, msg);
  digitalWrite(PIN_BUZZER, HIGH);  // immediate — callback fires synchronously
}

void onPhAlarmCleared(ApaDoseAlarm type, const char* msg) {
  Serial.print(F("[t="));
  Serial.print(millis() / 1000);
  Serial.println(F("s][pH] Alarm cleared."));
  // Silence buzzer only when CL is also alarm-free
  if (!clPump.isAlarmActive()) digitalWrite(PIN_BUZZER, LOW);
}

// --- CL alarm callbacks ---
void onClAlarm(ApaDoseAlarm type, const char* msg) {
  logAlarm("CL", type, msg);
  digitalWrite(PIN_BUZZER, HIGH);
}

void onClAlarmCleared(ApaDoseAlarm type, const char* msg) {
  Serial.print(F("[t="));
  Serial.print(millis() / 1000);
  Serial.println(F("s][CL] Alarm cleared."));
  if (!phPump.isAlarmActive()) digitalWrite(PIN_BUZZER, LOW);
}

void onStatus(const char* msg) {
  Serial.print(F("[STATUS] "));
  Serial.println(msg);
}

// --- Alarm status report ---
void printAlarmStatus() {
  Serial.println(F("--- Alarm Status ---"));

  if (phPump.isAlarmActive()) {
    Serial.print(F("  pH  ALARM : ")); Serial.println(phPump.getAlarmMessage());
    Serial.println(requiresAck(phPump.getCurrentAlarm())
                   ? F("         -> Press ACK.")
                   : F("         -> Auto-recovering."));
  } else {
    Serial.println(F("  pH  OK"));
  }

  if (clPump.isAlarmActive()) {
    Serial.print(F("  CL  ALARM : ")); Serial.println(clPump.getAlarmMessage());
    Serial.println(requiresAck(clPump.getCurrentAlarm())
                   ? F("         -> Press ACK.")
                   : F("         -> Auto-recovering."));
  } else {
    Serial.println(F("  CL  OK"));
  }

  Serial.println(F("--------------------"));
}

// --- Button handling: short press vs long press ---
bool          lastButtonState  = HIGH;
unsigned long buttonPressedAt  = 0;
bool          longPressHandled = false;

void handleButton() {
  bool state = digitalRead(PIN_ACK_BUTTON);

  if (state == LOW && lastButtonState == HIGH) {
    buttonPressedAt  = millis();
    longPressHandled = false;
  }

  // Long press threshold reached while still held
  if (state == LOW && !longPressHandled &&
      (millis() - buttonPressedAt) >= LONG_PRESS_MS) {
    phPump.acknowledgeAlarm();
    clPump.acknowledgeAlarm();
    Serial.println(F("[ACK] All alarms acknowledged (long press)."));
    longPressHandled = true;
  }

  // Short press: released before long-press threshold
  if (state == HIGH && lastButtonState == LOW && !longPressHandled) {
    if (phPump.isAlarmActive()) {
      phPump.acknowledgeAlarm();
      Serial.println(F("[ACK] pH alarm acknowledged."));
    } else if (clPump.isAlarmActive()) {
      clPump.acknowledgeAlarm();
      Serial.println(F("[ACK] CL alarm acknowledged."));
    } else {
      Serial.println(F("[ACK] No active alarm."));
    }
  }

  lastButtonState = state;
}

unsigned long lastStatusPrint = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_ALARM_LED,    OUTPUT);
  pinMode(PIN_BUZZER,       OUTPUT);
  pinMode(PIN_ACK_BUTTON,   INPUT_PULLUP);
  pinMode(PIN_FILTER_RELAY, INPUT);
  digitalWrite(PIN_ALARM_LED, LOW);
  digitalWrite(PIN_BUZZER,    LOW);

  // Separate callbacks per pump for clean identification in Serial log
  phPump.setPumpRange(65, 255);        // 65 = PWM start threshold — measure for YOUR pump
  // phPump.setPumpFlowRate(450.0);    // optional: measured mL/min at max PWM — enables getDailyVolumeMl()
  phPump.setDosingType(DOSE_PH_MINUS); // acid — doses when pH is above setpoint
  // pH control is one-directional — never run DOSE_PH_PLUS and DOSE_PH_MINUS on the same pool.
  phPump.setCallbacks(onPhAlarm, onPhAlarmCleared, onStatus);
  phPump.begin(getpH, filterRunning, 20, 6);

  clPump.setPumpRange(65, 255);        // measure start threshold for this pump separately
  // clPump.setPumpFlowRate(450.0);    // optional: measured mL/min at max PWM — enables getDailyVolumeMl()
  // setDosingType(DOSE_CL) before begin() is critical on first boot:
  // it ensures ORP defaults (setpoint 700, band 100) are written to EEPROM
  // rather than pH defaults. On subsequent boots EEPROM is restored correctly.
  clPump.setDosingType(DOSE_CL);
  clPump.setCallbacks(onClAlarm, onClAlarmCleared, onStatus);
  clPump.begin(getORP, filterRunning, 20, 12);

  Serial.println(F("APA-Dose Alarm Management Demo"));
  Serial.println(F("Short press ACK = ack first alarm | Long press (2s) = ack all"));
  printAlarmStatus();
}

void loop() {
  phPump.update();
  clPump.update();

  // POLLING — LED and buzzer reflect real alarm state at all times.
  // This is the safety net: correct even if a callback was missed.
  bool anyAlarm = phPump.isAlarmActive() || clPump.isAlarmActive();
  digitalWrite(PIN_ALARM_LED, anyAlarm ? HIGH : LOW);
  digitalWrite(PIN_BUZZER,    anyAlarm ? HIGH : LOW);

  handleButton();

  // Periodic status every 30 s
  if (millis() - lastStatusPrint >= 30000UL) {
    printAlarmStatus();
    lastStatusPrint = millis();
  }
}
