/*
 * APA Expert Example — 09: APAPHX2 + APA-Dose + DS2482  [Serial Diagnostics]
 *
 * Full production pool controller combining all three APA libraries:
 *   APAPHX2_ADS1115  — dual ADS1115-based pH + ORP sensors (non-blocking)
 *   APA-Dose         — proportional dosing for pH- and chlorine pumps
 *   DS2482           — DS2482-800 I2C-to-1-Wire bridge → DS18B20 water temp
 *
 * This is example 07 with OneWire/DallasTemperature replaced by DS2482.
 * All three libraries share the I2C bus and run fully non-blocking.
 *
 * Temperature channel assignment (DS2482-800 has 8 channels):
 *   Channel 0 — water temperature (feeds pH compensation)
 *   Channels 1–7 — available for additional probes (equipment room, inlet, etc.)
 *
 * DS2482 non-blocking pattern:
 *   startTemperatureConversion(ch) → triggers DS18B20 conversion
 *   checkConversionStatus()        → poll until true (conversion ~750 ms)
 *   readTemperature(ch, &val)      → collect result
 *   Conversion requested every 60 s — pool water temperature is slow-moving.
 *
 * Serial commands (115200 baud, Newline line ending):
 *   s              full system status
 *   a              acknowledge alarm (first active pump)
 *   mph <ms>       manual dose — pH pump  (max 5 min / MAX_MANUAL_DOSE_MS; longer is clamped)
 *   mcl <ms>       manual dose — CL pump  (max 5 min / MAX_MANUAL_DOSE_MS; longer is clamped)
 *   pph <ms> [pwm] prime pH pump
 *   pcl <ms> [pwm] prime CL pump
 *   sp  <val>      set pH setpoint      e.g. sp 7.4
 *   orp <val>      set ORP setpoint     e.g. orp 700
 *   b   <val>      set pH band          e.g. b 1.0
 *   bc  <val>      set ORP band         e.g. bc 100
 *   t   <val>      override water temperature  e.g. t 24.5
 *
 * Required libraries:
 *   APAPHX2_ADS1115   https://github.com/apadevices/APAPHX2_ADS1115
 *   DS2482            https://github.com/apadevices/DS2482  (place in /lib)
 *   RTClib            by Adafruit
 *
 * Hardware:
 *   PIN_PH_PUMP       D9    pH pump MOSFET
 *   PIN_CL_PUMP       D10   chlorine pump MOSFET
 *   PIN_FILTER_RELAY  D2    filter running signal (HIGH = running)
 *   PIN_ALARM_LED     D13   alarm LED
 *   PIN_ACK_BUTTON    D3    ACK button (INPUT_PULLUP)
 *   APAPHX2 pH board  I²C   address 0x49
 *   APAPHX2 ORP board I²C   address 0x48
 *   DS2482-800        I²C   address 0x18 (default)
 *   DS18B20 probe     DS2482 channel 0, 4.7 kΩ pull-up to 3.3V/5V
 *   DS3231 RTC        I²C
 *
 * APAPHX2 power note:
 *   Logic side: 5V from Arduino.  Analog side: 12V DC external (CN2).
 *   Grounds are galvanically isolated — do NOT connect them.
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <Wire.h>
#include <APADOSE.h>
#include <APAPHX2_ADS1115.h>
#include <DS2482.h>
#include <RTClib.h>

// --- Pin assignments ---
const uint8_t PIN_PH_PUMP      = 9;
const uint8_t PIN_CL_PUMP      = 10;
const uint8_t PIN_FILTER_RELAY = 2;
const uint8_t PIN_ALARM_LED    = 13;
const uint8_t PIN_ACK_BUTTON   = 3;

// --- APAPHX2 sensors ---
ADS1115_PHX_PH phSensor(0x49);
ADS1115_PHX_RX rxSensor(0x48);

PHXConfig phCfg = { 15, 50, 5 };  // 15 samples × 50 ms, rolling avg 5
PHXConfig rxCfg = { 15, 50, 5 };

float cachedpH  = 7.0f;
float cachedORP = 700.0f;

// --- DS2482 temperature ---
DS2482        ds2482;                     // default I2C address 0x18
const uint8_t TEMP_CHANNEL    = 0;        // DS18B20 on channel 0
float         waterTemp        = 25.0f;   // initial safe default
unsigned long lastTempRequest  = 0;
bool          conversionActive = false;

// --- RTC ---
RTC_DS3231 rtc;

// --- APA-Dose pumps ---
// Each instance must have a unique EEPROM base address, spaced by sizeof(ConfigData).
ApaDose phPump(PIN_PH_PUMP);                                                // EEPROM 192 (default)
ApaDose clPump(PIN_CL_PUMP, APA_DOSE_EEPROM_ADDRESS + sizeof(ConfigData)); // EEPROM 206

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

// --- APAPHX2 message callback ---
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

void updateAPAPHX2() {
  phSensor.updateReading();
  rxSensor.updateReading();

  if (phSensor.isReadingComplete()) {
    if (phSensor.getLastError() == PHXError::NONE)
      cachedpH = phSensor.getLastReading();
  }
  if (phSensor.getState() == PHXState::IDLE) phSensor.startReading(phCfg);

  if (rxSensor.isReadingComplete()) {
    if (rxSensor.getLastError() == PHXError::NONE)
      cachedORP = rxSensor.getLastReading();
  }
  if (rxSensor.getState() == PHXState::IDLE) rxSensor.startReading(rxCfg);
}

void updateTemperature() {
  // Recover from DS2482 hardware error before doing anything else
  if (ds2482.getState() == DS2482State::ERROR) {
    ds2482.reset();
    ds2482.begin();
    conversionActive = false;
    return;
  }

  if (!conversionActive) {
    // Request a new conversion every 60 s
    if (millis() - lastTempRequest >= 60000UL) {
      if (ds2482.startTemperatureConversion(TEMP_CHANNEL)) {
        conversionActive = true;
        lastTempRequest  = millis();
      }
      // If startTemperatureConversion returns false, no sensor on channel —
      // next attempt will try again after the 60 s interval.
    }
  } else {
    // Conversion in progress — check for completion every loop tick
    if (ds2482.checkConversionStatus()) {
      float t;
      if (ds2482.readTemperature(TEMP_CHANNEL, &t)) {
        if (t > 0.0f && t < 50.0f) {
          waterTemp = t;
          phSensor.setTemperature(waterTemp);  // update pH temperature compensation
        }
      }
      conversionActive = false;
    }

    // Safety timeout: if conversion hasn't finished within 2 s, abandon it
    if (millis() - lastTempRequest >= 2000UL) {
      conversionActive = false;
      ds2482.clearState();
    }
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

  Serial.print(F("  pH      : ")); Serial.print(cachedpH, 3);
  if (!phSensor.isRollingAverageReady()) Serial.print(F("  [warming up]"));
  if (!phSensor.isCalibrated())          Serial.print(F("  [UNCALIBRATED]"));
  Serial.println();

  Serial.print(F("  ORP     : ")); Serial.print(cachedORP, 1); Serial.println(F(" mV"));
  if (!rxSensor.isCalibrated()) Serial.println(F("            [UNCALIBRATED]"));

  Serial.print(F("  Temp    : ")); Serial.print(waterTemp, 1);
  Serial.print(F(" °C  (DS2482 ch")); Serial.print(TEMP_CHANNEL); Serial.println(F(")"));
  Serial.print(F("  Filter  : ")); Serial.println(filterRunning() ? F("RUNNING") : F("STOPPED"));

  char buf[APA_DOSE_STATUS_BUFFER_SIZE];  // 96 bytes — guaranteed to fit worst-case output

  phPump.getSystemStatus(buf, sizeof(buf));
  Serial.print(F("  [pH pump] ")); Serial.println(buf);
  Serial.print(F("    doses: ")); Serial.print(phPump.getDailyDoseCount());
  if (phPump.getMaxDailyDoses() > 0) { Serial.print(F("/")); Serial.print(phPump.getMaxDailyDoses()); }
  Serial.print(F("  failed: ")); Serial.println(phPump.getFailedAttempts());
  if (phPump.isAlarmActive()) {
    Serial.print(F("    ALARM: ")); Serial.println(phPump.getAlarmMessage());
    Serial.println(requiresAck(phPump.getCurrentAlarm()) ? F("    -> Type 'a'.") : F("    -> Auto-clears."));
  }

  clPump.getSystemStatus(buf, sizeof(buf));
  Serial.print(F("  [CL pump] ")); Serial.println(buf);
  Serial.print(F("    doses: ")); Serial.print(clPump.getDailyDoseCount());
  if (clPump.getMaxDailyDoses() > 0) { Serial.print(F("/")); Serial.print(clPump.getMaxDailyDoses()); }
  Serial.print(F("  failed: ")); Serial.println(clPump.getFailedAttempts());
  if (clPump.isAlarmActive()) {
    Serial.print(F("    ALARM: ")); Serial.println(clPump.getAlarmMessage());
    Serial.println(requiresAck(clPump.getCurrentAlarm()) ? F("    -> Type 'a'.") : F("    -> Auto-clears."));
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
    Serial.println(phPump.triggerPrime(args.toInt(), sp > 0 ? (uint8_t)args.substring(sp+1).toInt() : 0)
                   ? F("[pH PRIME] Started.") : F("[pH PRIME] Rejected."));

  } else if (line.startsWith(F("pcl "))) {
    String args = line.substring(4);
    int    sp   = args.indexOf(' ');
    Serial.println(clPump.triggerPrime(args.toInt(), sp > 0 ? (uint8_t)args.substring(sp+1).toInt() : 0)
                   ? F("[CL PRIME] Started.") : F("[CL PRIME] Rejected."));

  } else if (line.startsWith(F("sp "))) {
    Serial.println(phPump.setProbeSetpoint(line.substring(3).toFloat())
                   ? F("[SP] Updated.") : F("[SP] Rejected."));

  } else if (line.startsWith(F("orp "))) {
    Serial.println(clPump.setProbeSetpoint(line.substring(4).toFloat())
                   ? F("[ORP] Updated.") : F("[ORP] Rejected."));

  } else if (line.startsWith(F("b "))) {
    Serial.println(phPump.setProportionalBand(line.substring(2).toFloat())
                   ? F("[BAND] pH updated.") : F("[BAND] Rejected."));

  } else if (line.startsWith(F("bc "))) {
    Serial.println(clPump.setProportionalBand(line.substring(3).toFloat())
                   ? F("[BAND] ORP updated.") : F("[BAND] Rejected."));

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
// Setup & loop
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

  // --- DS2482 temperature bridge ---
  if (!ds2482.begin()) {
    Serial.println(F("[WARN] DS2482 not found — temperature reading disabled. Using default 25 °C."));
  }

  // --- APAPHX2 sensors (Wire already started) ---
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

  // --- RTC ---
  if (!rtc.begin()) Serial.println(F("[WARN] DS3231 not found — dosing window disabled."));

  // --- APA-Dose pumps (callbacks before begin) ---
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

void loop() {
  updateAPAPHX2();
  updateTemperature();

  phPump.update();
  clPump.update();

  digitalWrite(PIN_ALARM_LED,
               (phPump.isAlarmActive() || clPump.isAlarmActive()) ? HIGH : LOW);

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
