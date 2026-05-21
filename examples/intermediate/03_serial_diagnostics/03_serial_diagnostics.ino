/*
 * APA-Dose Example — 03: Serial Diagnostics  [INTERMEDIATE]
 *
 * Interactive serial interface for monitoring and controlling a pH pump.
 * Useful during installation, calibration, and fault-finding.
 * Open Serial Monitor at 115200 baud, line ending: Newline.
 *
 * Commands:
 *   s              print full system status
 *   a              acknowledge alarm
 *   m <ms>         manual dose — e.g. "m 5000" runs pump for 5 s (max 5 min; longer is clamped, not rejected)
 *   p <ms>         prime at full speed — e.g. "p 10000"
 *   p <ms> <pwm>   prime at specific PWM — e.g. "p 10000 100" (gentle fill)
 *   sp <val>       set pH setpoint — e.g. "sp 7.4"
 *   b  <val>       set proportional band — e.g. "b 1.0"
 *
 * Status is printed automatically every 30 seconds.
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <APADOSE.h>

const uint8_t PIN_PH_PUMP      = 9;
const uint8_t PIN_FILTER_RELAY = 2;
const uint8_t PIN_ALARM_LED    = 13;

ApaDose phPump(PIN_PH_PUMP);

float getpH()         { return 7.2; /* replace with phSensor.getPH() */ }
bool  filterRunning() { return digitalRead(PIN_FILTER_RELAY) == HIGH; }

// --- Callbacks ---
void onAlarm(ApaDoseAlarm type, const char* msg) {
  Serial.println();
  Serial.print("[ALARM] ");
  Serial.println(msg);

  bool needsAck = (type == ALARM_WRONG_DIRECTION ||
                   type == ALARM_INEFFECTIVE     ||
                   type == ALARM_DAILY_LIMIT);
  Serial.println(needsAck ? "  Type 'a' to acknowledge."
                          : "  Will clear automatically when sensor recovers.");
}

void onAlarmCleared(ApaDoseAlarm type, const char* msg) {
  Serial.println("[ALARM CLEARED] Dosing resumed.");
}

void onStatus(const char* msg) {
  Serial.print("[STATUS] ");
  Serial.println(msg);
}

// --- Status print ---
void printStatus() {
  char buf[APA_DOSE_STATUS_BUFFER_SIZE];  // 96 bytes — guaranteed to fit worst-case output
  phPump.getSystemStatus(buf, sizeof(buf));

  Serial.println(F("--- System Status ---"));
  Serial.println(buf);

  Serial.print(F("  Daily doses : "));
  Serial.print(phPump.getDailyDoseCount());
  if (phPump.getMaxDailyDoses() > 0) {
    Serial.print(F(" / "));
    Serial.print(phPump.getMaxDailyDoses());
  }
  Serial.println();

  Serial.print(F("  Failed attempts : "));
  Serial.println(phPump.getFailedAttempts());

  // --- Dose delivery health ---
  // getDoseEffectiveness() returns 0–100: the last dose as a % of the EMA learned baseline.
  // 100 = at or above normal; below setEfficiencyThreshold() (default 20) = ALARM_INEFFECTIVE.
  // Returns 100 during warm-up (first 3 auto proportional doses) — check hasDoseHistory() first.
  if (phPump.hasDoseHistory()) {
    Serial.print(F("  Delivery EMA    : "));
    Serial.print(phPump.getDoseEffectiveness());
    Serial.println(F("%  (100=normal; <20 fires alarm)"));
    Serial.print(F("  Before / After  : "));
    Serial.print(phPump.getLastDoseSensorBefore(), 2);
    Serial.print(F(" -> "));
    Serial.println(phPump.getLastDoseSensorAfter(), 2);
  } else {
    Serial.println(F("  Delivery EMA    : warming up (<3 doses completed)"));
  }

  Serial.print(F("  Config valid    : "));
  Serial.println(phPump.isConfigurationValid() ? F("YES") : F("NO"));

  Serial.print(F("  Startup blackout: "));
  Serial.println(phPump.isInStartupBlackout() ? F("ACTIVE") : F("DONE"));

  if (phPump.isAlarmActive()) {
    Serial.print(F("  ALARM           : "));
    Serial.println(phPump.getAlarmMessage());

    bool needsAck = (phPump.getCurrentAlarm() == ALARM_WRONG_DIRECTION ||
                     phPump.getCurrentAlarm() == ALARM_INEFFECTIVE     ||
                     phPump.getCurrentAlarm() == ALARM_DAILY_LIMIT);
    Serial.println(needsAck ? F("  -> Type 'a' to acknowledge.")
                            : F("  -> Auto-clears when sensor recovers."));
  }
  Serial.println(F("---------------------"));
}

// --- Serial command handler ---
void handleSerial() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  if (line == F("s")) {
    printStatus();

  } else if (line == F("a")) {
    phPump.acknowledgeAlarm();
    Serial.println(F("[ACK] Done."));

  } else if (line.startsWith(F("m "))) {
    unsigned long ms = line.substring(2).toInt();
    if (phPump.triggerManualDose(ms)) {
      Serial.print(F("[MANUAL] Dose started for ")); Serial.print(ms); Serial.println(F(" ms."));
    } else {
      Serial.println(F("[MANUAL] Rejected — alarm active, dosing in progress, or daily limit reached."));
    }

  } else if (line.startsWith(F("p "))) {
    String        args  = line.substring(2);
    int           space = args.indexOf(' ');
    unsigned long ms    = args.toInt();
    uint8_t       pwm   = (space > 0) ? (uint8_t)args.substring(space + 1).toInt() : 0;
    if (phPump.triggerPrime(ms, pwm)) {
      Serial.print(F("[PRIME] Started — ")); Serial.print(ms);
      Serial.print(F(" ms, PWM=")); Serial.println(pwm == 0 ? 255 : pwm);
    } else {
      Serial.println(F("[PRIME] Rejected — dosing or priming already active."));
    }

  } else if (line.startsWith(F("sp "))) {
    float val = line.substring(3).toFloat();
    if (phPump.setProbeSetpoint(val)) {
      Serial.print(F("[SETPOINT] Updated to ")); Serial.println(val, 2);
    } else {
      Serial.println(F("[SETPOINT] Rejected — out of range or dosing active."));
    }

  } else if (line.startsWith(F("b "))) {
    float val = line.substring(2).toFloat();
    if (phPump.setProportionalBand(val)) {
      Serial.print(F("[BAND] Updated to ")); Serial.println(val, 2);
    } else {
      Serial.println(F("[BAND] Rejected — out of range or dosing active."));
    }

  } else {
    Serial.println(F("Commands: s | a | m <ms> | p <ms> [pwm] | sp <val> | b <val>"));
  }
}

unsigned long lastStatusPrint = 0;
bool          lastButtonState  = HIGH;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_ALARM_LED,    OUTPUT);
  pinMode(PIN_FILTER_RELAY, INPUT);

  phPump.setPumpRange(65, 255);
  phPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  // PH_MINUS = acid (lowers pH); use PH_PLUS for a base pump (raises pH)
  phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20, 6);

  // --- Pool size scaling (call AFTER begin) ---
  // The library is calibrated for a 20 m³ reference pool.
  // Pools above ~30 m³ need this — without it, pulses are too short and the
  // pump will never converge to setpoint. Set once; survives factoryReset().
  // ApaDose::setPoolVolume(35);  // uncomment and set to YOUR pool volume in m³ (10–90)

  // --- Dose efficiency threshold (call AFTER begin, optional) ---
  // The library tracks delivery health via an EMA of normalised sensor shift per ms of pump run.
  // When a dose achieves less than this % of the learned baseline, ALARM_INEFFECTIVE fires.
  // Default is 20 (active out of the box after 3 warm-up doses). Pass 0 to disable the alarm.
  // phPump.setEfficiencyThreshold(20);  // default — shown here for clarity

  ApaDose::printLibraryInfo();
  Serial.println(F("Commands: s | a | m <ms> | p <ms> [pwm] | sp <val> | b <val>"));
  printStatus();
}

void loop() {
  phPump.update();

  digitalWrite(PIN_ALARM_LED, phPump.isAlarmActive() ? HIGH : LOW);

  handleSerial();

  if (millis() - lastStatusPrint >= 30000UL) {
    printStatus();
    lastStatusPrint = millis();
  }
}
