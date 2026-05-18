/*
 * APA-Dose Example — 04: LCD Display  [INTERMEDIATE]
 *
 * Live status for pH + chlorine pumps on a 20×4 I²C LCD.
 * Buzzer sounds on alarm; ACK button silences and resumes dosing.
 *
 * LCD layout (20×4):
 *   Row 0:  pH: 7.23  SP:7.4  [OK]
 *   Row 1:  ORP: 680mV SP:700 [OK]
 *   Row 2:  pH doses:3/6  CL:8/12
 *   Row 3:  pH:12m CL:rdy rest  (or alarm text; "System normal" when both eligible)
 *
 * Hardware:
 *   PIN_PH_PUMP       D9
 *   PIN_CL_PUMP       D10
 *   PIN_FILTER_RELAY  D2
 *   PIN_ACK_BUTTON    D3    INPUT_PULLUP, active LOW
 *   PIN_BUZZER        D8    active-high buzzer
 *   I²C LCD           SDA/SCL, address 0x27, 20×4
 *
 * Library: LiquidCrystal_I2C by Frank de Brabander
 *   Install via Library Manager or: https://github.com/johnrickman/LiquidCrystal_I2C
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <APADOSE.h>
#include <LiquidCrystal_I2C.h>

const uint8_t PIN_PH_PUMP      = 9;
const uint8_t PIN_CL_PUMP      = 10;
const uint8_t PIN_FILTER_RELAY = 2;
const uint8_t PIN_ACK_BUTTON   = 3;
const uint8_t PIN_BUZZER       = 8;

// Each ApaDose instance must have a unique EEPROM base address, spaced by
// sizeof(ConfigData). Without unique addresses both pumps would overwrite
// the same bytes and corrupt each other's saved configuration on every boot.
ApaDose           phPump(PIN_PH_PUMP);                                                // EEPROM 192 (default)
ApaDose           clPump(PIN_CL_PUMP, APA_DOSE_EEPROM_ADDRESS + sizeof(ConfigData)); // EEPROM 212
LiquidCrystal_I2C lcd(0x27, 20, 4);

float getpH()         { return 7.2; /* replace */ }
float getORP()        { return 680; /* replace */ }
bool  filterRunning() { return digitalRead(PIN_FILTER_RELAY) == HIGH; }

// Callback drives buzzer immediately — no polling delay
void onAlarm(ApaDoseAlarm type, const char* msg) {
  digitalWrite(PIN_BUZZER, HIGH);
}

void onAlarmCleared(ApaDoseAlarm type, const char* msg) {
  // Only silence buzzer when all alarms are gone
  if (!phPump.isAlarmActive() && !clPump.isAlarmActive()) {
    digitalWrite(PIN_BUZZER, LOW);
  }
}

void onStatus(const char* msg) { /* LCD shows live state — status log not needed here */ }

// --- LCD update ---
void updateLCD() {
  char row[21];

  // Row 0 — pH
  snprintf(row, sizeof(row), "pH:%-5.2f SP:%.1f %s",
           phPump.getProbeValue(),
           phPump.getCurrentSetpoint(),
           phPump.isAlarmActive() ? "[!]" : "[OK]");
  lcd.setCursor(0, 0);
  lcd.print(row);

  // Row 1 — ORP / chlorine
  snprintf(row, sizeof(row), "ORP:%-4.0fmV SP:%.0f%s",
           clPump.getProbeValue(),
           clPump.getCurrentSetpoint(),
           clPump.isAlarmActive() ? " [!]" : " [OK]");
  lcd.setCursor(0, 1);
  lcd.print(row);

  // Row 2 — dose counters
  if (phPump.getMaxDailyDoses() > 0 && clPump.getMaxDailyDoses() > 0) {
    snprintf(row, sizeof(row), "pH:%d/%d doses  CL:%d/%d  ",
             phPump.getDailyDoseCount(), phPump.getMaxDailyDoses(),
             clPump.getDailyDoseCount(), clPump.getMaxDailyDoses());
  } else {
    snprintf(row, sizeof(row), "pH doses:%-3d CL:%-3d     ",
             phPump.getDailyDoseCount(), clPump.getDailyDoseCount());
  }
  lcd.setCursor(0, 2);
  lcd.print(row);

  // Row 3 — alarm message or normal state
  lcd.setCursor(0, 3);
  if (phPump.isAlarmActive()) {
    bool needsAck = (phPump.getCurrentAlarm() == ALARM_WRONG_DIRECTION ||
                     phPump.getCurrentAlarm() == ALARM_INEFFECTIVE     ||
                     phPump.getCurrentAlarm() == ALARM_DAILY_LIMIT);
    lcd.print(needsAck ? "pH! Press ACK       "
                       : "pH! Auto-recovering ");
  } else if (clPump.isAlarmActive()) {
    bool needsAck = (clPump.getCurrentAlarm() == ALARM_WRONG_DIRECTION ||
                     clPump.getCurrentAlarm() == ALARM_INEFFECTIVE     ||
                     clPump.getCurrentAlarm() == ALARM_DAILY_LIMIT);
    lcd.print(needsAck ? "CL! Press ACK       "
                       : "CL! Auto-recovering ");
  } else if (phPump.isInStartupBlackout() || clPump.isInStartupBlackout()) {
    lcd.print("Startup blackout... ");
  } else {
    unsigned long phRest = phPump.getSecondsUntilNextDose() / 60;
    unsigned long clRest = clPump.getSecondsUntilNextDose() / 60;
    if (phRest > 0 || clRest > 0) {
      char phStr[5], clStr[5];
      if (phRest > 0) snprintf(phStr, sizeof(phStr), "%um", (unsigned)phRest);
      else            strcpy(phStr, "rdy");
      if (clRest > 0) snprintf(clStr, sizeof(clStr), "%um", (unsigned)clRest);
      else            strcpy(clStr, "rdy");
      snprintf(row, sizeof(row), "pH:%-3s CL:%-3s rest  ", phStr, clStr);
    } else {
      snprintf(row, sizeof(row), "%-20s", "System normal");
    }
    lcd.print(row);
  }
}

bool          lastButtonState = HIGH;
unsigned long lastLCDUpdate   = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_FILTER_RELAY, INPUT);
  pinMode(PIN_ACK_BUTTON,   INPUT_PULLUP);
  pinMode(PIN_BUZZER,       OUTPUT);
  digitalWrite(PIN_BUZZER,  LOW);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("APA-Dose starting...");

  phPump.setPumpRange(65, 255);        // 65 = PWM start threshold — measure for YOUR pump
  phPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  // PH_MINUS = acid (lowers pH); use PH_PLUS for a base pump (raises pH)
  phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20, 6);

  clPump.setPumpRange(65, 255);        // measure start threshold for this pump separately
  clPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  clPump.begin(getORP, filterRunning, DOSE_CL, CL_PLUS, 20, 12);

  lcd.clear();
}

void loop() {
  phPump.update();
  clPump.update();

  // Buzzer also polled — catches alarms that fired before callbacks registered
  bool anyAlarm = phPump.isAlarmActive() || clPump.isAlarmActive();
  digitalWrite(PIN_BUZZER, anyAlarm ? HIGH : LOW);

  // ACK button — clears first active latching alarm
  bool buttonState = digitalRead(PIN_ACK_BUTTON);
  if (buttonState == LOW && lastButtonState == HIGH) {
    if      (phPump.isAlarmActive()) phPump.acknowledgeAlarm();
    else if (clPump.isAlarmActive()) clPump.acknowledgeAlarm();
  }
  lastButtonState = buttonState;

  // LCD refresh every 5 s — avoids flicker, fast enough for pool monitoring
  if (millis() - lastLCDUpdate >= 5000UL) {
    updateLCD();
    lastLCDUpdate = millis();
  }
}
