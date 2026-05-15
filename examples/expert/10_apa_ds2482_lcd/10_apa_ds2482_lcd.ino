/*
 * APA Expert Example — 10: APAPHX2 + APA-Dose + DS2482  [LCD Controller]
 *
 * Full production pool controller with 20×4 LCD display combining:
 *   APAPHX2_ADS1115  — dual ADS1115-based pH + ORP sensors (non-blocking)
 *   APA-Dose         — proportional dosing for pH- and chlorine pumps
 *   DS2482           — DS2482-800 I2C-to-1-Wire bridge → DS18B20 water temp
 *
 * This is example 08 with OneWire/DallasTemperature replaced by DS2482.
 *
 * LCD layout (20 × 4):
 *   ┌────────────────────┐
 *   │ pH: 7.23  SP:7.4   │  row 0 — pH reading, setpoint, status
 *   │ ORP: 680mV SP:700  │  row 1 — ORP reading, setpoint, status
 *   │ 24.5°C  pH:3 CL:8  │  row 2 — water temp, daily dose counts
 *   │ System normal      │  row 3 — alarm / sensor state / APAPHX2 messages
 *   └────────────────────┘
 *
 * Row 3 priority (highest to lowest):
 *   1. APAPHX2 calibration message (shown 10 s then dismissed)
 *   2. Active alarm with ACK guidance
 *   3. Sensors warming up (rolling average not yet full)
 *   4. Filter stopped
 *   5. Startup blackout active
 *   6. System normal
 *
 * Status indicators on rows 0/1:
 *   [OK]  — in range, not dosing
 *   [D]   — pump actively dosing right now
 *   [!]   — alarm active
 *
 * ACK button:
 *   Short press  — acknowledge first active latching alarm
 *   Long press (2 s) — acknowledge all alarms
 *
 * Required libraries:
 *   APAPHX2_ADS1115       https://github.com/apadevices/APAPHX2_ADS1115
 *   DS2482                https://github.com/apadevices/DS2482  (place in /lib)
 *   LiquidCrystal_I2C     by Frank de Brabander
 *   RTClib                by Adafruit
 *
 * Hardware:
 *   PIN_PH_PUMP       D9    pH pump MOSFET
 *   PIN_CL_PUMP       D10   chlorine pump MOSFET
 *   PIN_FILTER_RELAY  D2    filter running signal (HIGH = running)
 *   PIN_BUZZER        D8    active-high buzzer
 *   PIN_ACK_BUTTON    D3    ACK button (INPUT_PULLUP)
 *   APAPHX2 pH board  I²C   address 0x49
 *   APAPHX2 ORP board I²C   address 0x48
 *   DS2482-800        I²C   address 0x18 (default)
 *   DS18B20 probe     DS2482 channel 0, 4.7 kΩ pull-up to 3.3V/5V
 *   DS3231 RTC        I²C
 *   LCD 20×4 I²C      I²C   address 0x27
 *
 * APAPHX2 power note:
 *   Logic side: 5V from Arduino.  Analog side: 12V DC external (CN2).
 *   Grounds galvanically isolated — do NOT connect them.
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <Wire.h>
#include <APADOSE.h>
#include <APAPHX2_ADS1115.h>
#include <DS2482.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>

// --- Pin assignments ---
const uint8_t PIN_PH_PUMP      = 9;
const uint8_t PIN_CL_PUMP      = 10;
const uint8_t PIN_FILTER_RELAY = 2;
const uint8_t PIN_BUZZER       = 8;
const uint8_t PIN_ACK_BUTTON   = 3;

// --- APAPHX2 sensors ---
ADS1115_PHX_PH phSensor(0x49);
ADS1115_PHX_RX rxSensor(0x48);

PHXConfig phCfg = { 15, 50, 5 };
PHXConfig rxCfg = { 15, 50, 5 };

float cachedpH  = 7.0f;
float cachedORP = 700.0f;

// --- DS2482 temperature ---
DS2482        ds2482;
const uint8_t TEMP_CHANNEL    = 0;
float         waterTemp        = 25.0f;
unsigned long lastTempRequest  = 0;
bool          conversionActive = false;

// --- Peripherals ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
RTC_DS3231        rtc;

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

// -----------------------------------------------------------------------
// LCD row 3 override — APAPHX2 messages shown for 10 s
// -----------------------------------------------------------------------

char          lcdRow3Override[21]    = "";
unsigned long lcdRow3OverrideUntil   = 0;

void setLCDRow3Override(const char* msg) {
  strncpy(lcdRow3Override, msg, 20);
  lcdRow3Override[20]    = '\0';
  lcdRow3OverrideUntil   = millis() + 10000UL;
}

void onSensorMessage(const __FlashStringHelper* msg) {
  char buf[21];
  strncpy_P(buf, (PGM_P)msg, 20);
  buf[20] = '\0';
  setLCDRow3Override(buf);
}

// -----------------------------------------------------------------------
// APA-Dose alarm callbacks
// -----------------------------------------------------------------------

void onAlarm(ApaDoseAlarm type, const char* msg) {
  digitalWrite(PIN_BUZZER, HIGH);
}

void onAlarmCleared(ApaDoseAlarm type, const char* msg) {
  if (!phPump.isAlarmActive() && !clPump.isAlarmActive()) {
    digitalWrite(PIN_BUZZER, LOW);
  }
}

void onStatus(const char* msg) { /* LCD shows live state */ }

// -----------------------------------------------------------------------
// Non-blocking sensor updates
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
  if (ds2482.getState() == DS2482State::ERROR) {
    ds2482.reset();
    ds2482.begin();
    conversionActive = false;
    return;
  }

  if (!conversionActive) {
    if (millis() - lastTempRequest >= 60000UL) {
      if (ds2482.startTemperatureConversion(TEMP_CHANNEL)) {
        conversionActive = true;
        lastTempRequest  = millis();
      }
    }
  } else {
    if (ds2482.checkConversionStatus()) {
      float t;
      if (ds2482.readTemperature(TEMP_CHANNEL, &t)) {
        if (t > 0.0f && t < 50.0f) {
          waterTemp = t;
          phSensor.setTemperature(waterTemp);
        }
      }
      conversionActive = false;
    }
    // Safety timeout
    if (millis() - lastTempRequest >= 2000UL) {
      conversionActive = false;
      ds2482.clearState();
    }
  }
}

// -----------------------------------------------------------------------
// LCD display
// -----------------------------------------------------------------------

bool requiresAck(ApaDoseAlarm type) {
  return type == ALARM_WRONG_DIRECTION ||
         type == ALARM_INEFFECTIVE     ||
         type == ALARM_DAILY_LIMIT;
}

void updateLCD() {
  char row[21];

  // Row 0 — pH
  bool phAlarm = phPump.isAlarmActive();
  snprintf(row, sizeof(row), "pH:%-5.2f  SP:%.1f %s",
           cachedpH,
           phPump.getCurrentSetpoint(),
           phAlarm                   ? "[!]"
           : phPump.isDosingActive() ? "[D]"
                                     : "[OK]");
  lcd.setCursor(0, 0);
  lcd.print(row);

  // Row 1 — ORP
  bool clAlarm = clPump.isAlarmActive();
  snprintf(row, sizeof(row), "ORP:%-5.0fmV SP:%.0f%s",
           cachedORP,
           clPump.getCurrentSetpoint(),
           clAlarm                   ? "[!]"
           : clPump.isDosingActive() ? "[D]"
                                     : " OK");
  lcd.setCursor(0, 1);
  lcd.print(row);

  // Row 2 — water temperature + daily dose counters
  if (phPump.getMaxDailyDoses() > 0 && clPump.getMaxDailyDoses() > 0) {
    snprintf(row, sizeof(row), "%-4.1f\xDF""C pH:%d/%d CL:%d/%d",
             waterTemp,
             phPump.getDailyDoseCount(), phPump.getMaxDailyDoses(),
             clPump.getDailyDoseCount(), clPump.getMaxDailyDoses());
  } else {
    snprintf(row, sizeof(row), "%-4.1f\xDF""C  pH:%-2d  CL:%-2d",
             waterTemp,
             phPump.getDailyDoseCount(),
             clPump.getDailyDoseCount());
  }
  lcd.setCursor(0, 2);
  lcd.print(row);

  // Row 3 — priority stack
  lcd.setCursor(0, 3);

  if (millis() < lcdRow3OverrideUntil) {
    char padded[21];
    snprintf(padded, sizeof(padded), "%-20s", lcdRow3Override);
    lcd.print(padded);

  } else if (phAlarm) {
    lcd.print(requiresAck(phPump.getCurrentAlarm())
              ? "pH! Press ACK       "
              : "pH! Auto-recovering ");

  } else if (clAlarm) {
    lcd.print(requiresAck(clPump.getCurrentAlarm())
              ? "CL! Press ACK       "
              : "CL! Auto-recovering ");

  } else if (!phSensor.isRollingAverageReady() || !rxSensor.isRollingAverageReady()) {
    lcd.print("Sensors warming...  ");

  } else if (!filterRunning()) {
    lcd.print("Filter stopped      ");

  } else if (phPump.isInStartupBlackout() || clPump.isInStartupBlackout()) {
    lcd.print("Startup blackout... ");

  } else {
    lcd.print("System normal       ");
  }
}

// -----------------------------------------------------------------------
// Button: short press / long press
// -----------------------------------------------------------------------

const uint16_t LONG_PRESS_MS    = 2000;
bool           lastButtonState  = HIGH;
unsigned long  buttonPressedAt  = 0;
bool           longPressHandled = false;

void handleButton() {
  bool state = digitalRead(PIN_ACK_BUTTON);

  if (state == LOW && lastButtonState == HIGH) {
    buttonPressedAt  = millis();
    longPressHandled = false;
  }

  if (state == LOW && !longPressHandled &&
      (millis() - buttonPressedAt) >= LONG_PRESS_MS) {
    phPump.acknowledgeAlarm();
    clPump.acknowledgeAlarm();
    setLCDRow3Override("All alarms ACK'd");
    longPressHandled = true;
  }

  if (state == HIGH && lastButtonState == LOW && !longPressHandled) {
    if (phPump.isAlarmActive()) {
      phPump.acknowledgeAlarm();
      setLCDRow3Override("pH alarm ACK'd");
    } else if (clPump.isAlarmActive()) {
      clPump.acknowledgeAlarm();
      setLCDRow3Override("CL alarm ACK'd");
    }
  }

  lastButtonState = state;
}

// -----------------------------------------------------------------------
// Setup & loop
// -----------------------------------------------------------------------

unsigned long lastLCDUpdate = 0;

void setup() {
  Wire.begin();

  pinMode(PIN_FILTER_RELAY, INPUT);
  pinMode(PIN_BUZZER,       OUTPUT);
  pinMode(PIN_ACK_BUTTON,   INPUT_PULLUP);
  digitalWrite(PIN_BUZZER,  LOW);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print(F("APA Pool Controller "));
  lcd.setCursor(0, 1); lcd.print(F("Initialising...     "));

  // DS2482
  if (!ds2482.begin()) {
    lcd.setCursor(0, 2); lcd.print(F("WARN:DS2482 missing "));
    delay(2000);
  }

  // APAPHX2
  phSensor.begin(false);
  phSensor.setMessageCallback(onSensorMessage);
  phSensor.setRollingAverage(5);
  phSensor.enableTemperatureCompensation(true);
  phSensor.setTemperature(waterTemp);

  rxSensor.begin(false);
  rxSensor.setMessageCallback(onSensorMessage);
  rxSensor.setRollingAverage(5);

  if (!phSensor.isCalibrated()) {
    lcd.setCursor(0, 2); lcd.print(F("WARN:pH uncalibrated"));
    delay(2000);
  }
  if (!rxSensor.isCalibrated()) {
    lcd.setCursor(0, 2); lcd.print(F("WARN:ORP uncalibratd"));
    delay(2000);
  }

  // RTC
  if (!rtc.begin()) {
    lcd.setCursor(0, 2); lcd.print(F("WARN: RTC not found "));
    delay(2000);
  }

  // APA-Dose (callbacks before begin)
  phPump.setPumpRange(65, 255);        // 65 = PWM start threshold — measure for YOUR pump
  // phPump.setPumpFlowRate(450.0);    // optional: measured mL/min at max PWM — enables getDailyVolumeMl()
  phPump.setDosingType(DOSE_PH_MINUS); // acid — doses when pH is above setpoint
  // pH control is one-directional — never run DOSE_PH_PLUS and DOSE_PH_MINUS on the same pool.
  phPump.setRTCCallback(getRTC);
  phPump.setDosingWindow(8, 20);
  phPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  phPump.begin(getpH, filterRunning, 20, 6);

  clPump.setPumpRange(65, 255);        // measure start threshold for this pump separately
  // clPump.setPumpFlowRate(450.0);    // optional: measured mL/min at max PWM — enables getDailyVolumeMl()
  // setDosingType(DOSE_CL) before begin() is critical on first boot:
  // it ensures ORP defaults (setpoint 700, band 100) are written to EEPROM
  // rather than pH defaults. On subsequent boots EEPROM is restored correctly.
  clPump.setDosingType(DOSE_CL);
  clPump.setRTCCallback(getRTC);
  clPump.setDosingWindow(8, 20);
  clPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  clPump.begin(getORP, filterRunning, 20, 12);

  lcd.clear();
}

void loop() {
  updateAPAPHX2();
  updateTemperature();

  phPump.update();
  clPump.update();

  // Buzzer: polled — correct even if callback was missed
  bool anyAlarm = phPump.isAlarmActive() || clPump.isAlarmActive();
  digitalWrite(PIN_BUZZER, anyAlarm ? HIGH : LOW);

  handleButton();

  if (millis() - lastLCDUpdate >= 5000UL) {
    updateLCD();
    lastLCDUpdate = millis();
  }
}
