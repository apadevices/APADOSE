/*
 * APA Expert Example — 07: APAPHX2 + APA-Dose  [Serial Diagnostics]
 *
 * Full production pool controller combining:
 *   APAPHX2_ADS1115  — dual ADS1115-based pH + ORP sensors, non-blocking
 *   APA-Dose         — proportional dosing for pH- and chlorine pumps
 *   DS18B20          — water temperature, feeds pH compensation
 *   DS3231 RTC       — dosing window 08:00–20:00, daily counter reset
 *
 * Integration pattern:
 *   APAPHX2 runs its own non-blocking state machine via updateReading().
 *   Each completed cycle updates a cached value (cachedpH / cachedORP).
 *   APA-Dose sensor callbacks return the cached value — the two libraries
 *   never block each other.
 *
 * Serial commands (115200 baud, Newline line ending):
 *   s              full system status
 *   a              acknowledge alarm (first active pump)
 *   mph <ms>       manual dose — pH pump  (max 5 min / MAX_MANUAL_DOSE_MS; longer is clamped)
 *   mcl <ms>       manual dose — CL pump  (max 5 min / MAX_MANUAL_DOSE_MS; longer is clamped)
 *   pph <ms> [pwm] prime pH pump
 *   pcl <ms> [pwm] prime CL pump
 *   sp <val>       set pH setpoint      e.g. sp 7.4
 *   orp <val>      set ORP setpoint     e.g. orp 700
 *   b  <val>       set pH band          e.g. b 1.0
 *   bc <val>       set ORP band         e.g. bc 100
 *   t  <val>       override water temperature  e.g. t 24.5
 *
 * Required libraries (install via Library Manager):
 *   APAPHX2_ADS1115   https://github.com/apadevices/APAPHX2_ADS1115
 *   RTClib            by Adafruit
 *   OneWire           by Paul Stoffregen
 *   DallasTemperature by Miles Burton
 *
 * Hardware:
 *   PIN_PH_PUMP       D9    pH pump MOSFET
 *   PIN_CL_PUMP       D10   chlorine pump MOSFET
 *   PIN_FILTER_RELAY  D2    filter running signal (HIGH = running)
 *   PIN_ALARM_LED     D13   alarm LED
 *   PIN_ACK_BUTTON    D3    ACK button (INPUT_PULLUP)
 *   PIN_DS18B20       D4    DS18B20 temperature probe (4.7kΩ pull-up to 5V)
 *   APAPHX2 pH board  I²C   address 0x49
 *   APAPHX2 ORP board I²C   address 0x48
 *   DS3231 RTC        I²C
 *
 * APAPHX2 power note:
 *   MCU/logic side: 5V from Arduino.
 *   Analog side: 12V DC external supply on CN2.
 *   Grounds are galvanically isolated — do NOT connect them.
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <Wire.h>
#include <APADOSE.h>
#include <APAPHX2_ADS1115.h>
#include <RTClib.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// --- Pin assignments ---
const uint8_t PIN_PH_PUMP      = 9;
const uint8_t PIN_CL_PUMP      = 10;
const uint8_t PIN_FILTER_RELAY = 2;
const uint8_t PIN_ALARM_LED    = 13;
const uint8_t PIN_ACK_BUTTON   = 3;
const uint8_t PIN_DS18B20      = 4;

// --- APAPHX2 sensors ---
// pH: I²C 0x49, Gain 2 (±2.048V).  ORP: I²C 0x48, Gain 1 (±4.096V).
// begin(false) skips Wire.begin() — we call Wire.begin() once in setup().
ADS1115_PHX_PH phSensor(0x49);
ADS1115_PHX_RX rxSensor(0x48);

// 15 samples × 50 ms = 750 ms per cycle, rolling average over 5 cycles
PHXConfig phCfg = { 15, 50, 5 };
PHXConfig rxCfg = { 15, 50, 5 };

// Cached sensor values — updated by APAPHX2, consumed by APA-Dose callbacks
float cachedpH  = 7.0f;
float cachedORP = 700.0f;

// --- Temperature ---
OneWire           oneWire(PIN_DS18B20);
DallasTemperature tempSensor(&oneWire);
float             waterTemp        = 25.0f;
unsigned long     lastTempRequest  = 0;
bool              tempPending      = false;

// --- RTC ---
RTC_DS3231 rtc;

// --- APA-Dose instances ---
// Each instance must have a unique EEPROM base address, spaced by sizeof(ConfigData).
ApaDose phPump(PIN_PH_PUMP);                                                // EEPROM 192 (default)
ApaDose clPump(PIN_CL_PUMP, APA_DOSE_EEPROM_ADDRESS + sizeof(ConfigData)); // EEPROM 206

// --- APA-Dose sensor callbacks — return cached APAPHX2 values ---
float getpH()  { return cachedpH; }
float getORP() { return cachedORP; }

bool filterRunning() {
  return digitalRead(PIN_FILTER_RELAY) == HIGH;
}

ApaDoseTime getRTC() {
  DateTime now = rtc.now();
  return { now.hour(), now.minute(), now.second(),
           now.day(), now.month(), (uint16_t)now.year() };
}

// --- APAPHX2 message callback (calibration progress → Serial) ---
void onSensorMessage(const __FlashStringHelper* msg) {
  Serial.print(F("[SENSOR] "));
  Serial.println(msg);
}

// --- APA-Dose alarm callbacks ---
void onPhAlarm(ApaDoseAlarm type, const char* msg) {
  Serial.print(F("[pH ALARM] ")); Serial.println(msg);
  digitalWrite(PIN_ALARM_LED, HIGH);
}
void onPhAlarmCleared(ApaDoseAlarm type, const char* msg) {
  if (!clPump.isAlarmActive()) digitalWrite(PIN_ALARM_LED, LOW);
  Serial.println(F("[pH ALARM CLEARED]"));
}
void onClAlarm(ApaDoseAlarm type, const char* msg) {
  Serial.print(F("[CL ALARM] ")); Serial.println(msg);
  digitalWrite(PIN_ALARM_LED, HIGH);
}
void onClAlarmCleared(ApaDoseAlarm type, const char* msg) {
  if (!phPump.isAlarmActive()) digitalWrite(PIN_ALARM_LED, LOW);
  Serial.println(F("[CL ALARM CLEARED]"));
}
void onStatus(const char* msg) {
  Serial.print(F("[STATUS] ")); Serial.println(msg);
}

// -----------------------------------------------------------------------
// Non-blocking sensor management
// -----------------------------------------------------------------------

void updateSensors() {
  // Advance APAPHX2 state machines — both call updateReading() every loop()
  phSensor.updateReading();
  rxSensor.updateReading();

  // pH: capture result and restart cycle
  if (phSensor.isReadingComplete()) {
    if (phSensor.getLastError() == PHXError::NONE) {
      cachedpH = phSensor.getLastReading();
    }
  }
  if (phSensor.getState() == PHXState::IDLE) {
    phSensor.startReading(phCfg);
  }

  // ORP: capture result and restart cycle
  if (rxSensor.isReadingComplete()) {
    if (rxSensor.getLastError() == PHXError::NONE) {
      cachedORP = rxSensor.getLastReading();
    }
  }
  if (rxSensor.getState() == PHXState::IDLE) {
    rxSensor.startReading(rxCfg);
  }
}

// Non-blocking DS18B20: request every 60 s, read 1 s later
void updateTemperature() {
  if (!tempPending && (millis() - lastTempRequest >= 60000UL)) {
    tempSensor.setWaitForConversion(false);
    tempSensor.requestTemperatures();
    lastTempRequest = millis();
    tempPending     = true;
  }
  // DS18B20 12-bit conversion max 750 ms — read after 1 s
  if (tempPending && (millis() - lastTempRequest >= 1000UL)) {
    float t = tempSensor.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED_C && t >= 0.0f && t <= 50.0f) {
      waterTemp = t;
      phSensor.setTemperature(waterTemp);  // feed pH temperature compensation
    }
    tempPending = false;
  }
}

// -----------------------------------------------------------------------
// Serial diagnostics
// -----------------------------------------------------------------------

bool requiresAck(ApaDoseAlarm type) {
  return type == ALARM_WRONG_DIRECTION ||
         type == ALARM_INEFFECTIVE     ||
         type == ALARM_DAILY_LIMIT;
}

void printStatus() {
  Serial.println(F("======= System Status ======="));

  // Sensor readings
  Serial.print(F("  pH      : ")); Serial.print(cachedpH, 3);
  if (!phSensor.isRollingAverageReady()) Serial.print(F("  [warming up]"));
  if (!phSensor.isCalibrated())          Serial.print(F("  [UNCALIBRATED]"));
  Serial.println();

  Serial.print(F("  ORP     : ")); Serial.print(cachedORP, 1); Serial.println(F(" mV"));
  if (!rxSensor.isCalibrated()) { Serial.println(F("            [UNCALIBRATED]")); }

  Serial.print(F("  Temp    : ")); Serial.print(waterTemp, 1); Serial.println(F(" °C"));
  Serial.print(F("  Filter  : ")); Serial.println(filterRunning() ? F("RUNNING") : F("STOPPED"));

  // pH pump
  char buf[APA_DOSE_STATUS_BUFFER_SIZE];  // 96 bytes — guaranteed to fit worst-case output
  phPump.getSystemStatus(buf, sizeof(buf));
  Serial.print(F("  [pH pump] ")); Serial.println(buf);
  Serial.print(F("    doses today: ")); Serial.print(phPump.getDailyDoseCount());
  if (phPump.getMaxDailyDoses() > 0) { Serial.print(F("/")); Serial.print(phPump.getMaxDailyDoses()); }
  Serial.print(F("  failed: ")); Serial.println(phPump.getFailedAttempts());
  if (phPump.isAlarmActive()) {
    Serial.print(F("    ALARM: ")); Serial.println(phPump.getAlarmMessage());
    Serial.println(requiresAck(phPump.getCurrentAlarm())
                   ? F("    -> Type 'a' to acknowledge.")
                   : F("    -> Auto-clears when sensor recovers."));
  }

  // CL pump
  clPump.getSystemStatus(buf, sizeof(buf));
  Serial.print(F("  [CL pump] ")); Serial.println(buf);
  Serial.print(F("    doses today: ")); Serial.print(clPump.getDailyDoseCount());
  if (clPump.getMaxDailyDoses() > 0) { Serial.print(F("/")); Serial.print(clPump.getMaxDailyDoses()); }
  Serial.print(F("  failed: ")); Serial.println(clPump.getFailedAttempts());
  if (clPump.isAlarmActive()) {
    Serial.print(F("    ALARM: ")); Serial.println(clPump.getAlarmMessage());
    Serial.println(requiresAck(clPump.getCurrentAlarm())
                   ? F("    -> Type 'a' to acknowledge.")
                   : F("    -> Auto-clears when sensor recovers."));
  }

  Serial.println(F("============================="));
}

void handleSerial() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  if (line == F("s")) {
    printStatus();

  } else if (line == F("a")) {
    if      (phPump.isAlarmActive()) { phPump.acknowledgeAlarm(); Serial.println(F("[ACK] pH alarm acknowledged.")); }
    else if (clPump.isAlarmActive()) { clPump.acknowledgeAlarm(); Serial.println(F("[ACK] CL alarm acknowledged.")); }
    else Serial.println(F("[ACK] No active alarm."));

  } else if (line.startsWith(F("mph "))) {
    unsigned long ms = line.substring(4).toInt();
    Serial.println(phPump.triggerManualDose(ms) ? F("[pH MANUAL] Started.") : F("[pH MANUAL] Rejected."));

  } else if (line.startsWith(F("mcl "))) {
    unsigned long ms = line.substring(4).toInt();
    Serial.println(clPump.triggerManualDose(ms) ? F("[CL MANUAL] Started.") : F("[CL MANUAL] Rejected."));

  } else if (line.startsWith(F("pph "))) {
    String args = line.substring(4);
    int    sp   = args.indexOf(' ');
    unsigned long ms  = args.toInt();
    uint8_t       pwm = (sp > 0) ? (uint8_t)args.substring(sp + 1).toInt() : 0;
    Serial.println(phPump.triggerPrime(ms, pwm) ? F("[pH PRIME] Started.") : F("[pH PRIME] Rejected."));

  } else if (line.startsWith(F("pcl "))) {
    String args = line.substring(4);
    int    sp   = args.indexOf(' ');
    unsigned long ms  = args.toInt();
    uint8_t       pwm = (sp > 0) ? (uint8_t)args.substring(sp + 1).toInt() : 0;
    Serial.println(clPump.triggerPrime(ms, pwm) ? F("[CL PRIME] Started.") : F("[CL PRIME] Rejected."));

  } else if (line.startsWith(F("sp "))) {
    float val = line.substring(3).toFloat();
    Serial.println(phPump.setProbeSetpoint(val) ? F("[SP] pH setpoint updated.") : F("[SP] Rejected."));

  } else if (line.startsWith(F("orp "))) {
    float val = line.substring(4).toFloat();
    Serial.println(clPump.setProbeSetpoint(val) ? F("[ORP] Setpoint updated.") : F("[ORP] Rejected."));

  } else if (line.startsWith(F("b "))) {
    float val = line.substring(2).toFloat();
    Serial.println(phPump.setProportionalBand(val) ? F("[BAND] pH band updated.") : F("[BAND] Rejected."));

  } else if (line.startsWith(F("bc "))) {
    float val = line.substring(3).toFloat();
    Serial.println(clPump.setProportionalBand(val) ? F("[BAND] ORP band updated.") : F("[BAND] Rejected."));

  } else if (line.startsWith(F("t "))) {
    float val = line.substring(2).toFloat();
    if (val >= 0.0f && val <= 50.0f) {
      waterTemp = val;
      phSensor.setTemperature(waterTemp);
      Serial.print(F("[TEMP] Set to ")); Serial.print(val, 1); Serial.println(F(" °C"));
    } else {
      Serial.println(F("[TEMP] Rejected — valid range 0–50 °C."));
    }

  } else {
    Serial.println(F("Commands: s | a | mph/mcl <ms> | pph/pcl <ms> [pwm] | sp/orp/b/bc <val> | t <val>"));
  }
}

// -----------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------

bool          lastButtonState = HIGH;
unsigned long lastStatusPrint = 0;

void setup() {
  Serial.begin(115200);
  Wire.begin();

  pinMode(PIN_ALARM_LED,    OUTPUT);
  pinMode(PIN_ACK_BUTTON,   INPUT_PULLUP);
  pinMode(PIN_FILTER_RELAY, INPUT);
  digitalWrite(PIN_ALARM_LED, LOW);

  // --- APAPHX2 sensors ---
  // begin(false): Wire already started above
  phSensor.begin(false);
  phSensor.setMessageCallback(onSensorMessage);
  phSensor.setRollingAverage(5);
  phSensor.enableTemperatureCompensation(true);
  phSensor.setTemperature(waterTemp);

  rxSensor.begin(false);
  rxSensor.setMessageCallback(onSensorMessage);
  rxSensor.setRollingAverage(5);

  if (!phSensor.isCalibrated()) Serial.println(F("[WARN] pH sensor not calibrated — run calibration sketch first."));
  if (!rxSensor.isCalibrated()) Serial.println(F("[WARN] ORP sensor not calibrated — run calibration sketch first."));

  // --- Temperature sensor ---
  tempSensor.begin();
  if (tempSensor.getDeviceCount() == 0) Serial.println(F("[WARN] DS18B20 not found — using default 25 °C."));

  // --- RTC ---
  if (!rtc.begin()) Serial.println(F("[WARN] DS3231 not found — dosing window disabled."));

  // --- APA-Dose pumps ---
  // Callbacks MUST be registered before begin() to receive startup messages
  phPump.setPumpRange(65, 255);        // 65 = PWM start threshold — measure for YOUR pump
  // phPump.setPumpFlowRate(450.0);    // optional: measured mL/min at max PWM — enables getDailyVolumeMl()
  phPump.setDosingType(DOSE_PH_MINUS); // acid — doses when pH is above setpoint
  // pH control is one-directional — never run DOSE_PH_PLUS and DOSE_PH_MINUS on the same pool.
  phPump.setRTCCallback(getRTC);
  phPump.setDosingWindow(8, 20);
  phPump.setCallbacks(onPhAlarm, onPhAlarmCleared, onStatus);
  phPump.begin(getpH, filterRunning, 20, 6);

  clPump.setPumpRange(65, 255);        // measure start threshold for this pump separately
  // clPump.setPumpFlowRate(450.0);    // optional: measured mL/min at max PWM — enables getDailyVolumeMl()
  // setDosingType(DOSE_CL) before begin() is critical on first boot:
  // it ensures ORP defaults (setpoint 700, band 100) are written to EEPROM
  // rather than pH defaults. On subsequent boots EEPROM is restored correctly.
  clPump.setDosingType(DOSE_CL);
  clPump.setRTCCallback(getRTC);
  clPump.setDosingWindow(8, 20);
  clPump.setCallbacks(onClAlarm, onClAlarmCleared, onStatus);
  clPump.begin(getORP, filterRunning, 20, 12);

  ApaDose::printLibraryInfo();
  Serial.println(F("Commands: s | a | mph/mcl <ms> | pph/pcl <ms> [pwm] | sp/orp/b/bc <val> | t <val>"));
  printStatus();
}

// -----------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------

void loop() {
  // APAPHX2 state machines — must run every loop() iteration
  updateSensors();
  updateTemperature();

  // APA-Dose state machines
  phPump.update();
  clPump.update();

  // Alarm LED follows real alarm state at all times
  bool anyAlarm = phPump.isAlarmActive() || clPump.isAlarmActive();
  digitalWrite(PIN_ALARM_LED, anyAlarm ? HIGH : LOW);

  // ACK button
  bool buttonState = digitalRead(PIN_ACK_BUTTON);
  if (buttonState == LOW && lastButtonState == HIGH) {
    if      (phPump.isAlarmActive()) phPump.acknowledgeAlarm();
    else if (clPump.isAlarmActive()) clPump.acknowledgeAlarm();
    Serial.println(F("[ACK] Acknowledged."));
  }
  lastButtonState = buttonState;

  handleSerial();

  if (millis() - lastStatusPrint >= 30000UL) {
    printStatus();
    lastStatusPrint = millis();
  }
}
