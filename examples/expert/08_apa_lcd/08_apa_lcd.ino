/*
 * APA Expert Example — 08: APAPHX2 + APA-Dose  [LCD Controller]
 *
 * Full production pool controller with 20×4 LCD display combining:
 *   APAPHX2_ADS1115  — dual ADS1115-based pH + ORP sensors, non-blocking
 *   APA-Dose         — proportional dosing for pH- and chlorine pumps
 *   DS18B20          — water temperature with pH compensation
 *   DS3231 RTC       — dosing window 08:00–20:00, daily counter reset
 *
 * LCD layout (20 × 4):
 *
 *   Normal display:
 *   ┌────────────────────┐
 *   │ pH: 7.23  SP:7.4   │  row 0
 *   │ ORP: 680mV SP:700  │  row 1
 *   │ 24.5°C  pH:3 CL:8  │  row 2  (temp, daily dose counts)
 *   │ System normal      │  row 3
 *   └────────────────────┘
 *
 *   Alarm display (row 3):
 *   │ pH! Press ACK      │  latching alarm — button required
 *   │ CL! Auto-recover.. │  ALARM_SAFETY_BAND — clears automatically
 *
 *   Sensor warming up (row 3):
 *   │ Sensors warming... │  rolling average not yet full
 *
 *   APAPHX2 calibration messages go to row 3 temporarily.
 *
 * ACK button:
 *   Short press  — acknowledge first active latching alarm
 *   Long press (2 s) — acknowledge all alarms
 *
 * Required libraries (install via Library Manager):
 *   APAPHX2_ADS1115       https://github.com/apadevices/APAPHX2_ADS1115
 *   LiquidCrystal_I2C     by Frank de Brabander
 *   RTClib                by Adafruit
 *   OneWire               by Paul Stoffregen
 *   DallasTemperature     by Miles Burton
 *
 * Hardware:
 *   PIN_PH_PUMP       D9    pH pump MOSFET
 *   PIN_CL_PUMP       D10   chlorine pump MOSFET
 *   PIN_FILTER_RELAY  D2    filter running signal (HIGH = running)
 *   PIN_BUZZER        D8    active-high buzzer
 *   PIN_ACK_BUTTON    D3    ACK button (INPUT_PULLUP)
 *   PIN_DS18B20       D4    DS18B20 temperature probe (4.7kΩ pull-up)
 *   APAPHX2 pH board  I²C   address 0x49
 *   APAPHX2 ORP board I²C   address 0x48
 *   DS3231 RTC        I²C
 *   LCD 20×4 I²C      I²C   address 0x27
 *
 * APAPHX2 power:
 *   Logic side: 5V from Arduino (P1 connector).
 *   Analog side: 12V DC external supply (CN2 connector).
 *   Grounds galvanically isolated — do NOT connect them.
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <Wire.h>
#include <APADOSE.h>
#include <APAPHX2_ADS1115.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// --- Pin assignments ---
const uint8_t PIN_PH_PUMP      = 9;
const uint8_t PIN_CL_PUMP      = 10;
const uint8_t PIN_FILTER_RELAY = 2;
const uint8_t PIN_BUZZER       = 8;
const uint8_t PIN_ACK_BUTTON   = 3;
const uint8_t PIN_DS18B20      = 4;

// --- APAPHX2 sensors ---
ADS1115_PHX_PH phSensor(0x49);
ADS1115_PHX_RX rxSensor(0x48);

PHXConfig phCfg = { 15, 50, 5 };
PHXConfig rxCfg = { 15, 50, 5 };

float cachedpH  = 7.0f;
float cachedORP = 700.0f;

// --- Display + peripherals ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
RTC_DS3231        rtc;
OneWire           oneWire(PIN_DS18B20);
DallasTemperature tempSensor(&oneWire);

// --- Temperature ---
float         waterTemp       = 25.0f;
unsigned long lastTempRequest = 0;
bool          tempPending     = false;

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
// LCD row 3 — shared between alarm state, sensor messages, and status
// -----------------------------------------------------------------------

// APAPHX2 calibration messages displayed on row 3 for 10 s
char          lcdRow3Override[21] = "";
unsigned long lcdRow3OverrideUntil = 0;

void setLCDRow3Override(const char* msg) {
  strncpy(lcdRow3Override, msg, 20);
  lcdRow3Override[20]       = '\0';
  lcdRow3OverrideUntil = millis() + 10000UL;
}

// APAPHX2 messages (≤20 chars, F-string) → LCD row 3
void onSensorMessage(const __FlashStringHelper* msg) {
  char buf[21];
  strncpy_P(buf, (PGM_P)msg, 20);
  buf[20] = '\0';
  setLCDRow3Override(buf);
}

// -----------------------------------------------------------------------
// Alarm callbacks
// -----------------------------------------------------------------------

void onAlarm(ApaDoseAlarm type, const char* msg) {
  digitalWrite(PIN_BUZZER, HIGH);  // immediate via callback
}

void onAlarmCleared(ApaDoseAlarm type, const char* msg) {
  if (!phPump.isAlarmActive() && !clPump.isAlarmActive()) {
    digitalWrite(PIN_BUZZER, LOW);
  }
}

void onStatus(const char* msg) { /* LCD shows live state — no serial needed here */ }

// -----------------------------------------------------------------------
// Non-blocking sensor updates
// -----------------------------------------------------------------------

void updateSensors() {
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
  if (!tempPending && (millis() - lastTempRequest >= 60000UL)) {
    tempSensor.setWaitForConversion(false);
    tempSensor.requestTemperatures();
    lastTempRequest = millis();
    tempPending     = true;
  }
  if (tempPending && (millis() - lastTempRequest >= 1000UL)) {
    float t = tempSensor.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED_C && t >= 0.0f && t <= 50.0f) {
      waterTemp = t;
      phSensor.setTemperature(waterTemp);
    }
    tempPending = false;
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

  // Row 0 — pH reading + setpoint + status indicator
  bool phAlarm = phPump.isAlarmActive();
  snprintf(row, sizeof(row), "pH:%-5.2f  SP:%.1f %s",
           cachedpH,
           phPump.getCurrentSetpoint(),
           phAlarm              ? "[!]"
           : phPump.isDosingActive() ? "[D]"
                                      : "[OK]");
  lcd.setCursor(0, 0);
  lcd.print(row);

  // Row 1 — ORP reading + setpoint + status indicator
  bool clAlarm = clPump.isAlarmActive();
  snprintf(row, sizeof(row), "ORP:%-5.0fmV SP:%.0f%s",
           cachedORP,
           clPump.getCurrentSetpoint(),
           clAlarm              ? "[!]"
           : clPump.isDosingActive() ? "[D]"
                                      : " OK");
  lcd.setCursor(0, 1);
  lcd.print(row);

  // Row 2 — temperature + daily dose counters
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

  // Row 3 — priority: override message > alarm > warming > normal
  lcd.setCursor(0, 3);

  if (millis() < lcdRow3OverrideUntil) {
    // APAPHX2 calibration or other override message
    char padded[21];
    snprintf(padded, sizeof(padded), "%-20s", lcdRow3Override);
    lcd.print(padded);

  } else if (phAlarm) {
    bool ack = requiresAck(phPump.getCurrentAlarm());
    lcd.print(ack ? "pH! Press ACK       "
                  : "pH! Auto-recovering ");

  } else if (clAlarm) {
    bool ack = requiresAck(clPump.getCurrentAlarm());
    lcd.print(ack ? "CL! Press ACK       "
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
// Button handling — short press / long press
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
// Setup
// -----------------------------------------------------------------------

unsigned long lastLCDUpdate = 0;

void setup() {
  Wire.begin();

  pinMode(PIN_FILTER_RELAY, INPUT);
  pinMode(PIN_BUZZER,       OUTPUT);
  pinMode(PIN_ACK_BUTTON,   INPUT_PULLUP);
  digitalWrite(PIN_BUZZER,  LOW);

  // LCD — show splash while the rest of setup runs
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print(F("APA Pool Controller "));
  lcd.setCursor(0, 1); lcd.print(F("Initialising...     "));

  // APAPHX2 sensors
  phSensor.begin(false);
  phSensor.setMessageCallback(onSensorMessage);
  phSensor.setRollingAverage(5);
  phSensor.enableTemperatureCompensation(true);
  phSensor.setTemperature(waterTemp);

  rxSensor.begin(false);
  rxSensor.setMessageCallback(onSensorMessage);
  rxSensor.setRollingAverage(5);

  // Warn on LCD if sensors are not calibrated
  if (!phSensor.isCalibrated()) {
    lcd.setCursor(0, 2); lcd.print(F("WARN:pH uncalibrated"));
    delay(2000);
  }
  if (!rxSensor.isCalibrated()) {
    lcd.setCursor(0, 2); lcd.print(F("WARN:ORP uncalibratd"));
    delay(2000);
  }

  // Temperature sensor
  tempSensor.begin();

  // RTC
  if (!rtc.begin()) {
    lcd.setCursor(0, 2); lcd.print(F("WARN: RTC not found "));
    delay(2000);
  }

  // APA-Dose — callbacks before begin()
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

// -----------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------

void loop() {
  updateSensors();
  updateTemperature();

  phPump.update();
  clPump.update();

  // Buzzer: polled as safety net — correct even if callback was missed
  bool anyAlarm = phPump.isAlarmActive() || clPump.isAlarmActive();
  digitalWrite(PIN_BUZZER, anyAlarm ? HIGH : LOW);

  handleButton();

  if (millis() - lastLCDUpdate >= 5000UL) {
    updateLCD();
    lastLCDUpdate = millis();
  }
}
