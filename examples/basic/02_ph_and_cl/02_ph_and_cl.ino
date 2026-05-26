/*
 * APA-Dose Example — 02: pH + Chlorine Pumps  [BASIC]
 *
 * Two independent ApaDose instances sharing one filtration interlock.
 * Each pump manages its own sensor, setpoint, and alarm state.
 *
 *   phPump — pH- (acid), target pH 7.4
 *   clPump — chlorine/ORP, target ORP 700 mV
 *
 * This example also demonstrates shock / super-chlorination:
 *   Press PIN_SHOCK_BUTTON to trigger a standard weekly shock.
 *   The library doses clPump at full power until ORP reaches SHOCK_ORP_STANDARD
 *   (750 mV) or 4 hours elapse, then resumes normal proportional control.
 *   phPump is held during shock and resumes automatically when shock ends.
 *
 * Dosing type and direction are passed directly to begin() —
 * no separate setDosingType() call needed.
 *
 * Hardware:
 *   PIN_PH_PUMP       D9    pH pump MOSFET
 *   PIN_CL_PUMP       D10   chlorine pump MOSFET
 *   PIN_FILTER_RELAY  D2    filter running signal (HIGH = running)
 *   PIN_ALARM_LED     D13   shared alarm LED (on if either pump has alarm)
 *   PIN_ACK_BUTTON    D3    ACK button (INPUT_PULLUP)
 *   PIN_SHOCK_BUTTON  D4    Shock trigger button (INPUT_PULLUP)
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <APADOSE.h>

const uint8_t PIN_PH_PUMP      = 9;
const uint8_t PIN_CL_PUMP      = 10;
const uint8_t PIN_FILTER_RELAY = 2;
const uint8_t PIN_ALARM_LED    = 13;
const uint8_t PIN_ACK_BUTTON   = 3;
const uint8_t PIN_SHOCK_BUTTON = 4;  // dedicated shock trigger

// Each ApaDose instance needs a unique EEPROM start address, spaced sizeof(ConfigData) = 22 bytes apart.
// Without unique addresses, both pumps share the same 22 bytes — each boot one pump overwrites the
// other's saved setpoint, proportional band, and direction, causing erratic behaviour.
// Default address is 192. Add one instance per 22-byte block:
//   phPump → 192  (default, APA_DOSE_EEPROM_ADDRESS)
//   clPump → 214  (192 + 22)
//   3rd pump → 236  (192 + 44)   APA_DOSE_EEPROM_ADDRESS + 2*sizeof(ConfigData)
//   4th pump → 258  (192 + 66)   APA_DOSE_EEPROM_ADDRESS + 3*sizeof(ConfigData)
ApaDose phPump(PIN_PH_PUMP);                                                // EEPROM 192
ApaDose clPump(PIN_CL_PUMP, APA_DOSE_EEPROM_ADDRESS + sizeof(ConfigData)); // EEPROM 212

float getpH()  { return 7.2; /* replace with phSensor.getPH()   */ }
float getORP() { return 620; /* replace with orpSensor.getORP() */ }

bool filterRunning() {
  return digitalRead(PIN_FILTER_RELAY) == HIGH;
}

// Both pumps can share a single callback — the message identifies which pump
// triggered it. For separate handling, register different callbacks per pump.
void onAlarm(ApaDoseAlarm type, const char* msg) {
  Serial.print("[ALARM] ");
  Serial.println(msg);
}

void onAlarmCleared(ApaDoseAlarm type, const char* msg) {
  Serial.println("[ALARM CLEARED]");
}

void onStatus(const char* msg) {
  Serial.print("[STATUS] ");
  Serial.println(msg);
}

bool lastAckState   = HIGH;
bool lastShockState = HIGH;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_ALARM_LED,    OUTPUT);
  pinMode(PIN_ACK_BUTTON,   INPUT_PULLUP);
  pinMode(PIN_SHOCK_BUTTON, INPUT_PULLUP);
  pinMode(PIN_FILTER_RELAY, INPUT);

  // --- pH pump setup ---
  phPump.setPumpRange(65, 255);  // 65 = PWM start threshold — measure for YOUR pump
  // Solenoid valve: use setPumpRange(255, 255) — PWM is fixed, pulse duration carries proportionality.
  phPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  // PH_MINUS = acid (lowers pH); use PH_PLUS for a base pump (raises pH).
  // Normal on first install — returns false when no valid config exists yet or EEPROM is corrupt.
  if (!phPump.begin(getpH, filterRunning, DOSE_PH, PH_MINUS, 20, 6))
    Serial.println("[INFO] phPump: no saved config — defaults loaded.");

  // --- Chlorine pump setup ---
  clPump.setPumpRange(65, 255);  // measure start threshold for this pump separately
  // Solenoid valve: use setPumpRange(255, 255).
  clPump.setCallbacks(onAlarm, onAlarmCleared, onStatus);
  if (!clPump.begin(getORP, filterRunning, DOSE_CL, CL_PLUS, 20, 12))
    Serial.println("[INFO] clPump: no saved config — defaults loaded.");

  // --- Pool size scaling (call AFTER begin) ---
  // The library is calibrated for a 20 m³ reference pool.
  // If your pool is 30 m³ or smaller: leave this commented out — defaults work fine.
  // If your pool is larger than ~30 m³: uncomment and set your volume. Without it,
  // dose pulses are too short for the larger water volume and pH/ORP will never converge.
  // This setting is shared across all pump instances and survives factoryReset().
  // ApaDose::setPoolVolume(35);  // uncomment and set to YOUR pool volume in m³ (10–90)

  // --- Dead-band (call AFTER begin, optional) ---
  // Suppresses dosing when error is small — reduces pump cycles when pool is near setpoint.
  // 10 % means: pH ±0.10 entry / ±0.05 exit; ORP ±10 mV / ±5 mV. Cleared by factoryReset().
  // ApaDose::setDeadbandPct(10);  // uncomment to enable; 0–20 % of proportional band

  // --- pH-first priority + cross-settle coupling (call AFTER both begin() calls) ---
  // Option J: suspends CL dosing when pH > 7.6 — above that level chlorine is mostly wasted
  //           (chlorine efficiency drops sharply in alkaline water, so dosing more is pointless
  //           until pH drops back into range).
  // Option A: holds CL dosing for N minutes after each pH dose. Without this, adding acid
  //           temporarily lowers ORP, which makes the CL pump dose immediately — raising ORP
  //           which pushes pH back up, causing the pH pump to dose again. The two pumps end up
  //           fighting each other in a slow see-saw. The cross-settle hold breaks that loop.
  // Both disabled by default. Uncomment to enable (requires both pump instances).
  // clPump.setPhPump(&phPump);          // register the link — activates Option J automatically
  // clPump.setCrossSettleMinutes(15);   // Option A: hold CL 15 min after pH doses (0 = off)

  // --- Inter-pump lockout (always active, no configuration needed) ---
  // After either pump doses, ALL pump instances wait 90 seconds before the next dose.
  // This prevents acid and chlorine being injected back-to-back at the same pipe inlet —
  // they can react to produce chlorine gas. If your second pump seems slow after the first
  // one runs, this is the reason — it is working as intended.

  // --- Dynamic OFA / dOFA (always on — zero config needed) ---
  // dOFA independently tracks each pump's proportional run time and learns what is normal
  // for THIS pool. ALARM_OFA fires when today's proportional run exceeds 2× the baseline.
  // Both pumps have separate dOFA baselines — pH pump and CL pump learn independently.
  //
  // Warm-up: baseline seeds at midnight of the first qualifying day — typically day 2 per pump.
  // isDOFALearning() returns true until then. During that first day the pool is protected
  // by other safety systems (ALARM_INEFFECTIVE, ALARM_WRONG_DIRECTION, ALARM_SAFETY_BAND).
  // getDOFAPct()    returns today's proportional run as % of the learned baseline.
  //   Returns 0 until the first qualifying day — normal on day 1, not a fault.
  //
  // First install: leave the resetDOFA() lines below commented out.
  //   dOFA starts learning automatically from the very first proportional dose.
  // Spring opening: uncomment ONCE on the first startup after a shutdown of several weeks.
  //   dOFA then re-learns for the current season. Comment out again after that one boot.
  //
  // phPump.resetDOFA();  // spring opening — uncomment once, then comment out again
  // clPump.resetDOFA();  // spring opening — uncomment once, then comment out again

  // --- Tank level estimation (optional, no hardware required) ---
  // The library tracks cumulative chemical consumption (mL) and computes:
  //   getTankRemainingPct()   — 0–100 % of tank remaining (255 = disabled)
  //   getTankDaysUntilEmpty() — rolling 7-day estimate (255 = < 1 day of data)
  // ALARM_TANK_EMPTY fires when the estimated consumed volume equals the configured capacity.
  // Acknowledge the alarm after refilling — this resets the consumed counter to zero.
  //
  // Default tank capacity is 20 L. Omit these lines if your tanks are 20 L.
  // Both tanks are independent — set each pump's capacity separately.
  phPump.setTankCapacity(20);  // acid tank, litres — adjust to your actual tank size (1–65 L)
  clPump.setTankCapacity(20);  // chlorine tank
  //
  // Accuracy depends on setPumpFlowRate() matching your pump. Default is 450 mL/min.
  // Days-until-empty prediction becomes available after the first full 24 h of operation.
  //
  // If you also connect a physical float switch, register it with setTankEmptyCallback()
  // and leave setTankCapacity() calls above in place — the HW sensor fires ALARM_TANK_EMPTY,
  // while the percentage and days-until-empty display continue working from estimation.

  Serial.println(F("Ready. Press SHOCK button to trigger shock dosing."));
  Serial.println(F("SHOCK button requires: filter running, pH 7.0-7.6, ORP below target."));
}

void loop() {
  phPump.update();
  clPump.update();

  // LED on if either pump is alarming
  bool anyAlarm = phPump.isAlarmActive() || clPump.isAlarmActive();
  digitalWrite(PIN_ALARM_LED, anyAlarm ? HIGH : LOW);

  // --- ACK button: short press acknowledges first active alarm ---
  bool ackState = digitalRead(PIN_ACK_BUTTON);
  if (ackState == LOW && lastAckState == HIGH) {
    if      (phPump.isAlarmActive()) phPump.acknowledgeAlarm();
    else if (clPump.isAlarmActive()) clPump.acknowledgeAlarm();
    Serial.println("[ACK] Acknowledged.");
  }
  lastAckState = ackState;

  // --- SHOCK button: trigger hobbyist shock on rising edge ---
  bool shockBtn = digitalRead(PIN_SHOCK_BUTTON);
  if (shockBtn == LOW && lastShockState == HIGH) {
    // SHOCK_ORP_STANDARD = 750 mV (weekly maintenance target).
    // getpH() supplies the current pH — must be 7.0–7.6 for shock to start.
    // Returns false if any entry guard fails (see Serial output for reason via status callback).
    if (clPump.triggerShock(SHOCK_ORP_STANDARD, getpH())) {
      Serial.println(F("[SHOCK] Shock started — dosing at full power until ORP 750 mV."));
      Serial.println(F("[SHOCK] phPump held. Will resume automatically when shock ends."));
    } else {
      Serial.println(F("[SHOCK] Shock rejected — check: filter on? pH 7.0-7.6? ORP below target? No active alarm?"));
    }
  }
  lastShockState = shockBtn;

  // --- Print shock status once per minute while active ---
  static unsigned long lastShockPrint = 0;
  if (clPump.isShockActive() && millis() - lastShockPrint >= 60000UL) {
    lastShockPrint = millis();
    unsigned long rem = clPump.getShockRemainingSeconds();
    Serial.print(F("[SHOCK] Active — ORP: "));
    Serial.print((int)getORP());
    Serial.print(F(" mV, "));
    Serial.print(rem / 60);
    Serial.println(F(" min remaining to ceiling."));
  }

  // --- Print tank level status once every 10 minutes ---
  // getTankRemainingPct()   0–100 = % remaining; 255 = disabled (setTankCapacity not called)
  // getTankDaysUntilEmpty() 0–254 = estimate; 255 = not enough data yet (< 1 day of dosing)
  static unsigned long lastTankPrint = 0;
  if (millis() - lastTankPrint >= 600000UL) {
    lastTankPrint = millis();

    uint8_t phPct  = phPump.getTankRemainingPct();
    uint8_t phDays = phPump.getTankDaysUntilEmpty();
    Serial.print(F("[TANK] pH acid:  "));
    if (phPct == 255) {
      Serial.println(F("disabled"));
    } else {
      Serial.print(phPct); Serial.print(F("% remaining"));
      if (phDays < 255) { Serial.print(F(", ~")); Serial.print(phDays); Serial.print(F(" days")); }
      Serial.println();
    }

    uint8_t clPct  = clPump.getTankRemainingPct();
    uint8_t clDays = clPump.getTankDaysUntilEmpty();
    Serial.print(F("[TANK] Chlorine: "));
    if (clPct == 255) {
      Serial.println(F("disabled"));
    } else {
      Serial.print(clPct); Serial.print(F("% remaining"));
      if (clDays < 255) { Serial.print(F(", ~")); Serial.print(clDays); Serial.print(F(" days")); }
      Serial.println();
    }
  }
}
