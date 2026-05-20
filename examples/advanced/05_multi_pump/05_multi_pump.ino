/*
 * APA-Dose Example — 05: Multi-Pump System with RTC  [ADVANCED]
 *
 * Four independent pumps sharing one filtration interlock and one external stop:
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
 * Shock / super-chlorination:
 *   Press PIN_SHOCK_BUTTON to trigger a pro-mode shock:
 *   clPump doses at full power until ORP reaches 770 mV or 3 hours elapse.
 *   pH must be 7.0–7.6. All other pumps are held and resume automatically.
 *   With RTC registered, the 24 h post-shock cooldown is wall-clock based and
 *   survives power cycles — triggerShock() will block for the full cooldown window.
 *
 * External stop — two independent reasons, combined into one callback:
 *   PIN_MAINT_SWITCH  — panel toggle: pool maintainer vacuuming the floor
 *   backwashRunning   — set by the backwash state machine during filter cleaning
 *
 *   While either condition is true, all four dosing pumps are blocked instantly.
 *   After both conditions clear, a mandatory 5-minute settling time applies
 *   before any new dose is allowed.
 *   Priming is always available regardless of external stop state.
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
 *   PIN_MAINT_SWITCH  D7    panel toggle (INPUT_PULLUP, LOW = maintenance ON)
 *   PIN_ALARM_LED     D13
 *   PIN_ACK_BUTTON    D3
 *   PIN_SHOCK_BUTTON  D4    shock trigger (INPUT_PULLUP)
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
const uint8_t PIN_MAINT_SWITCH = 7;   // panel toggle — INPUT_PULLUP, LOW = maintenance active
const uint8_t PIN_ALARM_LED    = 13;
const uint8_t PIN_ACK_BUTTON   = 3;
const uint8_t PIN_SHOCK_BUTTON = 4;   // shock trigger — INPUT_PULLUP

// Each ApaDose instance requires a unique EEPROM base address, spaced by
// sizeof(ConfigData). Without unique addresses all pumps would share the same
// 20 bytes and corrupt each other's saved configuration on every boot.
ApaDose    phPump  (PIN_PH_PUMP,   APA_DOSE_EEPROM_ADDRESS);                           // EEPROM 192
ApaDose    clPump  (PIN_CL_PUMP,   APA_DOSE_EEPROM_ADDRESS +     sizeof(ConfigData));  // EEPROM 212
ApaDose    flocPump(PIN_FLOC_PUMP, APA_DOSE_EEPROM_ADDRESS + 2 * sizeof(ConfigData));  // EEPROM 232
ApaDose    algiPump(PIN_ALGI_PUMP, APA_DOSE_EEPROM_ADDRESS + 3 * sizeof(ConfigData));  // EEPROM 252
RTC_DS3231 rtc;

// --- Sensor bridges ---
float getpH()  { return 7.2; /* replace with phSensor.getPH()   */ }
float getORP() { return 640; /* replace with orpSensor.getORP() */ }

bool filterRunning() {
  return digitalRead(PIN_FILTER_RELAY) == HIGH;
}

// --- External stop ---
bool backwashRunning = false;

// Returns true when ANY condition makes dosing unsafe or pointless.
// The library calls this every update() cycle; any registered pump stops
// immediately when this returns true and waits 5 minutes after it clears.
bool dosingBlocked() {
  bool maintenance = (digitalRead(PIN_MAINT_SWITCH) == LOW);  // active LOW
  return maintenance || backwashRunning;
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
uint8_t lastFlocDay = 255;
uint8_t lastAlgiDay = 255;

void handleScheduledDosing() {
  if (!filterRunning()) return;  // never dose without filtration

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

bool lastAckState   = HIGH;
bool lastShockState = HIGH;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_ALARM_LED,    OUTPUT);
  pinMode(PIN_ACK_BUTTON,   INPUT_PULLUP);
  pinMode(PIN_SHOCK_BUTTON, INPUT_PULLUP);
  pinMode(PIN_FILTER_RELAY, INPUT);
  pinMode(PIN_MAINT_SWITCH, INPUT_PULLUP);  // LOW = maintenance mode active
  digitalWrite(PIN_ALARM_LED, LOW);

  if (!rtc.begin()) {
    Serial.println("[WARN] RTC not found — dosing window, schedules, and shock RTC cooldown disabled.");
  }

  // pH pump — acid, sensor-driven
  phPump.setPumpRange(65, 255);
  phPump.setRTCCallback(getRTC);
  phPump.setDosingWindow(8, 20);
  phPump.setExternalStopCallback(dosingBlocked);
  phPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20, 6);
  phPump.enableAdaptivePB(5);

  // Chlorine pump — ORP-driven, also capable of shock dosing
  clPump.setPumpRange(65, 255);
  clPump.setRTCCallback(getRTC);      // enables wall-clock post-shock cooldown that survives power cycles
  clPump.setDosingWindow(8, 20);
  clPump.setExternalStopCallback(dosingBlocked);
  clPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  clPump.begin(getORP, filterRunning, DOSE_CL, CL_PLUS, 20, 12);

  // Flocculant — sensor-less, schedule-driven
  flocPump.setPumpRange(65, 255);
  flocPump.setExternalStopCallback(dosingBlocked);
  flocPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  flocPump.begin(nullptr, filterRunning, DOSE_PH, PH_PLUS, 0, 1);

  // Algaecide — sensor-less, schedule-driven
  algiPump.setPumpRange(65, 255);
  algiPump.setExternalStopCallback(dosingBlocked);
  algiPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  algiPump.begin(nullptr, filterRunning, DOSE_PH, PH_PLUS, 0, 1);

  Serial.println(F("Ready. Press SHOCK button to trigger pro shock (770 mV, 3 h max, 24 h cooldown)."));
}

void loop() {
  phPump.update();
  clPump.update();
  flocPump.update();
  algiPump.update();

  handleScheduledDosing();

  // --- ACK button — walks pumps in priority order, clears first active alarm ---
  bool ackState = digitalRead(PIN_ACK_BUTTON);
  if (ackState == LOW && lastAckState == HIGH) {
    if      (phPump.isAlarmActive())   phPump.acknowledgeAlarm();
    else if (clPump.isAlarmActive())   clPump.acknowledgeAlarm();
    else if (flocPump.isAlarmActive()) flocPump.acknowledgeAlarm();
    else if (algiPump.isAlarmActive()) algiPump.acknowledgeAlarm();
    Serial.println("[ACK] Acknowledged.");
  }
  lastAckState = ackState;

  // --- SHOCK button — pro API: 770 mV target, 3 h max, 24 h cooldown (RTC-backed) ---
  bool shockBtn = digitalRead(PIN_SHOCK_BUTTON);
  if (shockBtn == LOW && lastShockState == HIGH) {
    // Pro overload: triggerShock(targetORP, maxDurationHours, currentPH, cooldownHours)
    // maxDurationHours clamped to SHOCK_MAX_DURATION_HOURS (4 h) if higher value is passed.
    // cooldownHours clamped to SHOCK_COOLDOWN_MAX_HOURS (48 h) if higher value is passed.
    // With RTC registered, cooldown is wall-clock based — survives power cycles.
    // Returns false if any entry guard fails; reason arrives via status callback.
    if (clPump.triggerShock(770, 3, getpH(), 24)) {
      Serial.println(F("[SHOCK] Shock started — target 770 mV, max 3 h, 24 h cooldown."));
      Serial.println(F("[SHOCK] phPump, flocPump, algiPump held. Will resume automatically."));
    } else {
      Serial.println(F("[SHOCK] Shock rejected — check: filter on? pH 7.0-7.6? ORP below 770? No alarm? Cooldown elapsed?"));
    }
  }
  lastShockState = shockBtn;

  // --- Print shock status every minute while active ---
  static unsigned long lastShockPrint = 0;
  if (clPump.isShockActive() && millis() - lastShockPrint >= 60000UL) {
    lastShockPrint = millis();
    unsigned long rem = clPump.getShockRemainingSeconds();
    Serial.print(F("[SHOCK] Active — ORP: "));
    Serial.print((int)getORP());
    Serial.print(F(" mV, "));
    Serial.print(rem / 60);
    Serial.print(F(" min to ceiling | phPump held: "));
    Serial.println(phPump.isDosingActive() ? F("dosing") : F("yes"));
  }
}
