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
 *   ALARM_INEFFECTIVE      — requires ACK button; two independent trigger paths:
 *                              • EMA path: last dose achieved < efficiency threshold (default 20%)
 *                                of the learned delivery baseline — catches empty tank or pump
 *                                failure after 1–3 bad doses. getDoseEffectiveness() shows the ratio.
 *                              • 3-strike path: sensor shows no meaningful response on 3 consecutive
 *                                doses — cold-start safety net before baseline is established.
 *   ALARM_DAILY_LIMIT      — auto-clears at midnight (RTC) or after 24 h (millis fallback)
 *   ALARM_TANK_EMPTY       — requires ACK button; fires when setTankEmptyCallback() returns true
 *                              at dose-start time; dosing AND priming blocked until resolved
 *   ALARM_OFA              — requires ACK button; fires at 90 % of the daily pump run-time limit
 *                              set by setOFALimit(), OR when dOFA detects today's proportional
 *                              run time exceeds 2× the self-learned baseline (dOFA is always
 *                              active, no setOFALimit() call needed); a 70 %/1.5× warning fires
 *                              first (dosing continues); counter resets on ACK or at midnight
 *   ALARM_SAFETY_BAND      — auto-clears when sensor returns to safe range
 *   ALARM_INVALID_PARAM    — never latches; silent rejection only
 *
 * Hardware:
 *   PIN_PH_PUMP       D9
 *   PIN_CL_PUMP       D10
 *   PIN_FILTER_RELAY  D2
 *   PIN_PH_TANK       D5    Float switch: acid tank (INPUT_PULLUP, LOW = empty)
 *   PIN_CL_TANK       D6    Float switch: chlorine tank (INPUT_PULLUP, LOW = empty)
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
const uint8_t  PIN_PH_TANK       = 5;
const uint8_t  PIN_CL_TANK       = 6;
const uint8_t  PIN_ALARM_LED     = 13;
const uint8_t  PIN_BUZZER        = 8;
const uint8_t  PIN_ACK_BUTTON    = 3;
const uint16_t LONG_PRESS_MS     = 2000;

// Each ApaDose instance must have a unique EEPROM base address, spaced by
// sizeof(ConfigData) = 22 bytes. Without unique addresses both pumps would overwrite
// the same bytes and corrupt each other's saved configuration on every boot.
ApaDose phPump(PIN_PH_PUMP);                                                // EEPROM 192 (default)
ApaDose clPump(PIN_CL_PUMP, APA_DOSE_EEPROM_ADDRESS + sizeof(ConfigData)); // EEPROM 214

float getpH()         { return 7.2; /* replace */ }
float getORP()        { return 640; /* replace */ }
bool  filterRunning() { return digitalRead(PIN_FILTER_RELAY) == HIGH; }
bool  phTankEmpty()   { return digitalRead(PIN_PH_TANK) == LOW; }
bool  clTankEmpty()   { return digitalRead(PIN_CL_TANK) == LOW; }

// Returns true for alarms that latch until the user presses ACK.
bool requiresAck(ApaDoseAlarm type) {
  return type == ALARM_WRONG_DIRECTION ||
         type == ALARM_INEFFECTIVE     ||
         type == ALARM_TANK_EMPTY      ||
         type == ALARM_OFA;
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

// Print dose delivery health for one pump — call from printAlarmStatus().
// getDoseEffectiveness() returns 0–100: last dose as % of the EMA learned baseline.
// Useful context when ALARM_INEFFECTIVE fires: shows how far delivery has dropped.
void printDelivery(const char* name, ApaDose& pump) {
  Serial.print(F("  ")); Serial.print(name);
  if (pump.hasDoseHistory()) {
    Serial.print(F(" delivery: "));
    Serial.print(pump.getDoseEffectiveness());
    Serial.print(F("%  (before: "));
    Serial.print(pump.getLastDoseSensorBefore(), 2);
    Serial.print(F(" -> after: "));
    Serial.print(pump.getLastDoseSensorAfter(), 2);
    Serial.println(F(")"));
  } else {
    Serial.println(F(" delivery: warming up (<3 doses)"));
  }
}

// Print OFA usage for one pump — only shown when OFA is enabled (pct > 0 or active).
void printOFA(const char* name, ApaDose& pump) {
  uint8_t pct = pump.getOFAPct();
  if (pct == 0) return;  // OFA disabled or no pump run time yet today
  Serial.print(F("  ")); Serial.print(name);
  Serial.print(F(" OFA: ")); Serial.print(pct);
  Serial.println(F("% of today's limit"));
}

// Print dOFA status for one pump.
// During warm-up: shows "learning" — normal until the first qualifying day (typically day 2).
// Once ready: shows today's proportional run as % of the learned baseline.
void printDOFA(const char* name, ApaDose& pump) {
  Serial.print(F("  ")); Serial.print(name);
  if (pump.isDOFALearning()) {
    // Normal on first install or after resetDOFA() — baseline seeds at midnight of day 1.
    // No action needed; pool is protected by other safety alarms during this first day.
    Serial.println(F(" dOFA: warming up — normal, alarm active from day 2"));
  } else {
    Serial.print(F(" dOFA: "));
    Serial.print(pump.getDOFAPct());
    Serial.println(F("% of learned baseline"));
  }
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
  printDelivery("pH ", phPump);
  printOFA("pH ", phPump);
  printDOFA("pH ", phPump);

  if (clPump.isAlarmActive()) {
    Serial.print(F("  CL  ALARM : ")); Serial.println(clPump.getAlarmMessage());
    Serial.println(requiresAck(clPump.getCurrentAlarm())
                   ? F("         -> Press ACK.")
                   : F("         -> Auto-recovering."));
  } else {
    Serial.println(F("  CL  OK"));
  }
  printDelivery("CL ", clPump);
  printOFA("CL ", clPump);
  printDOFA("CL ", clPump);

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
  pinMode(PIN_PH_TANK,      INPUT_PULLUP);
  pinMode(PIN_CL_TANK,      INPUT_PULLUP);
  digitalWrite(PIN_ALARM_LED, LOW);
  digitalWrite(PIN_BUZZER,    LOW);

  // Separate callbacks per pump for clean identification in Serial log
  phPump.setPumpRange(65, 255);        // 65 = PWM start threshold — measure for YOUR pump
  // phPump.setPumpFlowRate(450.0);    // optional: measured mL/min at max PWM — enables getDailyVolumeMl()
  phPump.setCallbacks(onPhAlarm, onPhAlarmCleared, onStatus);
  phPump.setTankEmptyCallback(phTankEmpty);
  // PH_MINUS = acid (lowers pH); use PH_PLUS for a base pump (raises pH)
  phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20, 6);

  clPump.setPumpRange(65, 255);        // measure start threshold for this pump separately
  // clPump.setPumpFlowRate(450.0);    // optional: measured mL/min at max PWM — enables getDailyVolumeMl()
  clPump.setCallbacks(onClAlarm, onClAlarmCleared, onStatus);
  clPump.setTankEmptyCallback(clTankEmpty);
  clPump.begin(getORP, filterRunning, DOSE_CL, CL_PLUS, 20, 12);

  // --- Pool size scaling (call AFTER begin) ---
  // The library is calibrated for a 20 m³ reference pool.
  // Pools above ~30 m³ need this — without it, pulses are too short and the
  // pump will never converge to setpoint. Set once; survives factoryReset().
  // ApaDose::setPoolVolume(35);  // uncomment and set to YOUR pool volume in m³ (10–90)

  // --- Dead-band (call AFTER begin, optional) ---
  // Suppresses dosing when error is small — reduces pump cycles when pool is near setpoint.
  // 10 % means: pH ±0.10 entry / ±0.05 exit; ORP ±10 mV / ±5 mV. Cleared by factoryReset().
  // ApaDose::setDeadbandPct(10);  // uncomment to enable; 0–20 % of proportional band

  // --- pH-first priority + cross-settle coupling (call AFTER both begin() calls) ---
  // Option J: automatically suspends CL dosing when pH > 7.6 — chlorine is ineffective above this.
  // Option A: holds CL for N minutes after a pH dose to let chemistry equilibrate.
  // Both are disabled by default. Uncomment to enable (requires both phPump and clPump instances).
  // clPump.setPhPump(&phPump);          // register the link — activates Option J automatically
  // clPump.setCrossSettleMinutes(15);   // Option A: hold CL 15 min after pH doses (0 = off)

  // --- Dose efficiency threshold (call AFTER begin, optional; applies per pump) ---
  // The library tracks delivery health via an EMA of normalised sensor shift per ms of pump run.
  // ALARM_INEFFECTIVE fires when a dose achieves less than this % of the learned baseline.
  // Default is 20 (active out of the box after 3 warm-up doses). Pass 0 to disable the alarm.
  // phPump.setEfficiencyThreshold(20);  // default — set lower to tolerate more variance
  // clPump.setEfficiencyThreshold(20);  // independent per pump

  // --- Dynamic OFA / dOFA (always on — zero config needed) ---
  // Each pump independently learns its normal proportional run time and fires ALARM_OFA
  // when today exceeds 2× the baseline (warning status at 1.5×). No setup required.
  // Warm-up: baseline ready after the first qualifying day. isDOFALearning() returns true until then.
  // getDOFAPct() shows today's proportional run vs the learned baseline (shown in printDOFA above).
  //
  // Spring opening — call resetDOFA() on each pump after a long shutdown so the library
  // re-learns the current season's chemistry rather than using last year's baseline:
  // phPump.resetDOFA();
  // clPump.resetDOFA();
  //
  // To disable dOFA for a pump entirely (e.g. sensor-less algaecide pump):
  // phPump.disableDOFA();
  //
  // To speed up or slow down learning (default 10 days, range 3–14):
  // phPump.setDOFAAdaptDays(5);
  // clPump.setDOFAAdaptDays(5);

  // --- Over-feed alarm / OFA (optional — leave commented out if you don't need it) ---
  // Prevents a stuck sensor or misconfigured setpoint from running the pump all day.
  // The limit is in pump RUN minutes per day, not wall-clock minutes.
  // How to choose a value: for a 20 m³ pool a well-tuned system rarely needs more than
  // 20–30 min/day of run time. Watch getOFAPct() over the first week — if it consistently
  // hits 80–90 % while water chemistry is good, raise the limit; if it stays under 30 %,
  // the limit is generous. For a 40 m³ pool use the same number — the library doubles
  // it automatically when you call setPoolVolume(40).
  // At 70 % of the limit: a status warning fires (dosing continues, no ACK needed).
  // At 90 %: ALARM_OFA fires, dosing stops. Press ACK to reset the counter immediately
  // and resume dosing. The counter also resets at midnight for unattended systems.
  // getOFAPct() returns today's usage (0–100 %) — useful for a dashboard display.
  // phPump.setOFALimit(30);   // uncomment and set — reference minutes for a 20 m³ pool
  // clPump.setOFALimit(30);   // set independently for each pump

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
