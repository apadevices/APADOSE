/*
 * APA-Dose Example — 05: Multi-Pump System with RTC  [ADVANCED]
 *
 * Four independent pumps sharing one filtration interlock:
 *
 *   phPump   — pH- acid pump        sensor-driven, proportional dosing
 *   clPump   — chlorine / ORP pump  sensor-driven, proportional dosing
 *   flocPump — flocculant           no sensor, manual schedule, max 1/day
 *   algiPump — algaecide            no sensor, manual schedule, max 1/day
 *
 * RTC (DS3231) provides:
 *   - Daily dose counter reset at midnight for all pumps
 *   - Dosing window 08:00–20:00 for pH and CL (no night dosing)
 *   - Floc schedule: Tuesday and Friday at 09:00
 *   - Algi schedule: Monday at 09:00
 *
 * Power-cycle note: without RTC-backed rest-period memory, a reboot during
 * a rest period will allow dosing again after the startup blackout expires.
 * Set blackoutMinutes >= the longest rest period (20 min) to cover this.
 *
 * Hardware:
 *   PIN_PH_PUMP       D9
 *   PIN_CL_PUMP       D10
 *   PIN_FLOC_PUMP     D5
 *   PIN_ALGI_PUMP     D6
 *   PIN_FILTER_RELAY  D2
 *   PIN_ALARM_LED     D13
 *   PIN_ACK_BUTTON    D3
 *   DS3231 RTC        SDA/SCL (I²C)
 *
 * Library: RTClib by Adafruit
 *   Install via Library Manager.
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <APADOSE.h>
#include <RTClib.h>

const uint8_t PIN_PH_PUMP      = 9;
const uint8_t PIN_CL_PUMP      = 10;
const uint8_t PIN_FLOC_PUMP    = 5;
const uint8_t PIN_ALGI_PUMP    = 6;
const uint8_t PIN_FILTER_RELAY = 2;
const uint8_t PIN_ALARM_LED    = 13;
const uint8_t PIN_ACK_BUTTON   = 3;

// Each ApaDose instance requires a unique EEPROM base address, spaced by
// sizeof(ConfigData). Without unique addresses all pumps would share the same
// 14 bytes and corrupt each other's saved configuration on every boot.
ApaDose    phPump  (PIN_PH_PUMP,   APA_DOSE_EEPROM_ADDRESS);                           // EEPROM 192
ApaDose    clPump  (PIN_CL_PUMP,   APA_DOSE_EEPROM_ADDRESS +     sizeof(ConfigData));  // EEPROM 206
ApaDose    flocPump(PIN_FLOC_PUMP, APA_DOSE_EEPROM_ADDRESS + 2 * sizeof(ConfigData));  // EEPROM 220
ApaDose    algiPump(PIN_ALGI_PUMP, APA_DOSE_EEPROM_ADDRESS + 3 * sizeof(ConfigData));  // EEPROM 234
RTC_DS3231 rtc;

// --- Sensor bridges ---
float getpH()  { return 7.2; /* replace with phSensor.getPH()   */ }
float getORP() { return 640; /* replace with orpSensor.getORP() */ }

bool filterRunning() {
  return digitalRead(PIN_FILTER_RELAY) == HIGH;
}

ApaDoseTime getRTC() {
  DateTime now = rtc.now();
  return { now.hour(), now.minute(), now.second(),
           now.day(), now.month(), (uint16_t)now.year() };
}

// --- Alarm callbacks ---
void onAlarm(ApaDoseAlarm type, const char* msg) {
  Serial.print("[ALARM] ");
  Serial.println(msg);
  digitalWrite(PIN_ALARM_LED, HIGH);
}

void onAlarmCleared(ApaDoseAlarm type, const char* msg) {
  // Only clear the LED when all pumps are alarm-free
  if (!phPump.isAlarmActive()   && !clPump.isAlarmActive() &&
      !flocPump.isAlarmActive() && !algiPump.isAlarmActive()) {
    digitalWrite(PIN_ALARM_LED, LOW);
  }
  Serial.println("[ALARM CLEARED]");
}

void onStatus(const char* msg) {
  Serial.print("[STATUS] ");
  Serial.println(msg);
}

// --- Scheduled dosing for sensor-less pumps ---
// Tracked per-day to prevent double-dosing after a reboot on the same day.
uint8_t lastFlocDay = 255;
uint8_t lastAlgiDay = 255;

void handleScheduledDosing() {
  if (!filterRunning()) return;  // never dose without filtration
  // triggerManualDose() accepts up to MAX_MANUAL_DOSE_MS (5 min); longer values are clamped silently.

  DateTime now = rtc.now();
  uint8_t  dow = now.dayOfTheWeek();  // 0=Sun 1=Mon 2=Tue ... 6=Sat

  // Floc: Tuesday (2) and Friday (5) after 09:00, once per day
  if (now.hour() >= 9 &&
      (dow == 2 || dow == 5) &&
      now.day() != lastFlocDay) {
    if (flocPump.triggerManualDose(30000UL)) {  // 30 s
      lastFlocDay = now.day();
      Serial.println("[FLOC] Scheduled dose triggered.");
    }
  }

  // Algi: Monday (1) after 09:00, once per day
  if (now.hour() >= 9 &&
      dow == 1 &&
      now.day() != lastAlgiDay) {
    if (algiPump.triggerManualDose(20000UL)) {  // 20 s
      lastAlgiDay = now.day();
      Serial.println("[ALGI] Scheduled dose triggered.");
    }
  }
}

bool lastButtonState = HIGH;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_ALARM_LED,    OUTPUT);
  pinMode(PIN_ACK_BUTTON,   INPUT_PULLUP);
  pinMode(PIN_FILTER_RELAY, INPUT);
  digitalWrite(PIN_ALARM_LED, LOW);

  if (!rtc.begin()) {
    Serial.println("[WARN] RTC not found — dosing window and schedules disabled.");
  }

  // pH pump — acid, sensor-driven
  phPump.setPumpRange(65, 255);        // 65 = PWM start threshold — measure for YOUR pump
  // phPump.setPumpFlowRate(450.0);    // optional: measured mL/min at max PWM — enables getDailyVolumeMl()
  phPump.setDosingType(DOSE_PH_MINUS); // acid — doses when pH is above setpoint
  // pH control is one-directional — never run DOSE_PH_PLUS and DOSE_PH_MINUS on the same pool.
  phPump.setRTCCallback(getRTC);
  phPump.setDosingWindow(8, 20);
  phPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  phPump.begin(getpH, filterRunning, 20, 6);

  // Chlorine pump — ORP-driven
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

  // Flocculant — sensor-less pump driven entirely by the schedule in handleScheduledDosing().
  // nullptr sensor: skips proportional dosing; only triggerManualDose() calls are accepted.
  // 0 blackout: sensor-less pumps don't need startup settling time.
  // No setRTCCallback/setDosingWindow: the time check is done in handleScheduledDosing()
  // directly via rtc.now(), so the library's dosing-window feature is not used here.
  flocPump.setPumpRange(65, 255);      // measure start threshold for your flocculant pump
  // flocPump.setPumpFlowRate(450.0);  // optional: measured mL/min at max PWM — enables getDailyVolumeMl()
  flocPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  flocPump.begin(nullptr, filterRunning, 0, 1);

  // Algaecide — same sensor-less pattern as flocPump.
  algiPump.setPumpRange(65, 255);      // measure start threshold for your algaecide pump
  // algiPump.setPumpFlowRate(450.0);  // optional: measured mL/min at max PWM — enables getDailyVolumeMl()
  algiPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  algiPump.begin(nullptr, filterRunning, 0, 1);
}

void loop() {
  phPump.update();
  clPump.update();
  flocPump.update();
  algiPump.update();

  handleScheduledDosing();

  // ACK button — walks pumps in priority order, clears first active alarm
  bool buttonState = digitalRead(PIN_ACK_BUTTON);
  if (buttonState == LOW && lastButtonState == HIGH) {
    if      (phPump.isAlarmActive())   phPump.acknowledgeAlarm();
    else if (clPump.isAlarmActive())   clPump.acknowledgeAlarm();
    else if (flocPump.isAlarmActive()) flocPump.acknowledgeAlarm();
    else if (algiPump.isAlarmActive()) algiPump.acknowledgeAlarm();
    Serial.println("[ACK] Acknowledged.");
  }
  lastButtonState = buttonState;
}
