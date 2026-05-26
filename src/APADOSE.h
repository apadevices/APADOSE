/*
 * APA-Dose Library
 *
 * A non-blocking, bulletproof dosing system for swimming pool chemical automation
 * Part of the Arduino Pool Automation (APA) product family
 *
 * Features:
 * - Proportional pulse-based dosing with feedback control
 * - Comprehensive alarm system with wrong direction detection
 * - Flexible configuration (pH+, pH- and chlorine/ORP pump support)
 * - EEPROM persistent storage
 * - Hardware-agnostic callback interface
 *
 * Version: 3.17.1
 * Author: kecup@vazac.eu (APA Devices)
 * Date: May 2026
 */

#ifndef APADOSE_H
#define APADOSE_H

#include <Arduino.h>
#include <EEPROM.h>

// Define APA_DOSE_DEBUG (build flag or before this include) to enable per-cycle
// diagnostic messages: sample readings, PWM values, pulse timing, feedback result.
// Disabled by default — saves ~200 bytes flash and eliminates runtime snprintf overhead.
// Enable in platformio.ini:  build_flags = -D APA_DOSE_DEBUG
// #define APA_DOSE_DEBUG

// Library version
#define APA_DOSE_VERSION "3.17.1"
#define APA_DOSE_VERSION_MAJOR 3
#define APA_DOSE_VERSION_MINOR 17
#define APA_DOSE_VERSION_PATCH 1

// pH sensor profile — hardcoded defaults (stored in flash, never copied to SRAM)
constexpr float PH_SETPOINT_MIN        = 6.8f;
constexpr float PH_SETPOINT_MAX        = 7.8f;
constexpr float PH_SETPOINT_DEFAULT    = 7.4f;
constexpr float PH_BAND_MIN            = 0.5f;
constexpr float PH_BAND_MAX            = 2.0f;
constexpr float PH_BAND_DEFAULT        = 1.0f;
constexpr float PH_FEEDBACK_THRESHOLD  = 0.05f; // meaningful pH change after dose
constexpr float PH_SAFETY_HARD_CAP     = 1.0f;  // absolute max pH deviation from setpoint
constexpr float PH_SENSOR_MIN          = 0.0f;   // below this → hardware fault (open/shorted probe)
constexpr float PH_SENSOR_MAX          = 14.0f;  // above this → hardware fault

// ORP / chlorine sensor profile — hardcoded defaults (stored in flash, never copied to SRAM)
constexpr float ORP_SETPOINT_MIN       = 400.0f;
constexpr float ORP_SETPOINT_MAX       = 850.0f;  // safety ceiling: free chlorine becomes harmful above ~850 mV
constexpr float ORP_SETPOINT_DEFAULT   = 700.0f;
constexpr float ORP_BAND_MIN           = 50.0f;
constexpr float ORP_BAND_MAX           = 250.0f;
constexpr float ORP_BAND_DEFAULT       = 100.0f;
constexpr float ORP_FEEDBACK_THRESHOLD = 10.0f; // meaningful ORP change after dose (mV) — 5 mV was inside electrode noise floor
constexpr float ORP_SAFETY_HARD_CAP    = 150.0f; // absolute max ORP deviation from setpoint (mV)
constexpr float ORP_SENSOR_MIN         = -1500.0f; // below this → hardware fault (beyond electrode range)
constexpr float ORP_SENSOR_MAX         =  1500.0f; // above this → hardware fault

constexpr float SAFETY_BAND_MULTIPLIER = 1.5f;   // dynamic safety margin: band × this value

// Maximum duration accepted by triggerManualDose() — protects against accidental over-dosing.
// Requests above this ceiling are clamped and a status message is sent.
constexpr unsigned long MAX_MANUAL_DOSE_MS = 5UL * 60UL * 1000UL;  // 5 minutes

// Minimum gap between any two pump instances completing a dose.
// Prevents back-to-back injection of incompatible chemicals at the same inlet (acid + chlorine).
constexpr unsigned long INTER_PUMP_LOCKOUT_MS = 90000UL;  // 90 s

constexpr uint8_t       BEFORE_SAMPLES   = 2;
constexpr uint8_t       AFTER_SAMPLES    = 3;
constexpr unsigned long SAMPLE_INTERVAL  = 30000UL;

// If the sensor callback returns no valid value for this duration, automatic dosing is
// suspended and ALARM_SENSOR_FAULT fires. Clears automatically on the next valid reading.
constexpr unsigned long SENSOR_STALE_MS  = 30UL * 60UL * 1000UL;

// Continuous invalid readings (out-of-range or NaN/inf) for this duration → ALARM_SENSOR_FAULT.
// Short enough to catch a failed probe quickly; long enough to ignore momentary glitches.
constexpr unsigned long SENSOR_FAULT_MS       =  2UL * 60UL * 1000UL;

// Zone 4 (75–100 % error) base pulse duration — also the anchor for the feedback cap.
constexpr unsigned long ZONE4_PULSE_MS        = 180000UL;
// Feedback escalation ceiling: zone 4 × 5/3 (≈ ×1.67) = 300 s (5 min).
// Escalation path: 180 s → 234 s (×1.3) → 300 s cap (×1.3, capped).
constexpr unsigned long FEEDBACK_PULSE_MAX_MS = ZONE4_PULSE_MS * 5UL / 3UL;

// How long the filtration pump must be continuously off before a status warning fires.
constexpr unsigned long FILTER_OFF_ALARM_MS = 30UL * 60UL * 1000UL;  // 30 minutes

// Over-feed alarm (OFA) — cumulative daily pump run-time thresholds.
// Reference limit set by setOFALimit() is for a 20 m³ pool and scales with setPoolVolume().
constexpr uint8_t OFA_WARNING_PCT = 70;  // status warning fires, dosing continues
constexpr uint8_t OFA_STOP_PCT    = 90;  // ALARM_OFA fires, dosing stops until ACK + midnight reset

// Dynamic OFA (dOFA) — self-learning daily baseline; always on, zero config needed.
// Accumulates proportional-only run time; excludes manual doses, shock, and prime.
// EMA is updated once per day at midnight (or 24 h millis rollover) when at least
// DOFA_MIN_DAILY_SEC of proportional run time was accumulated.
constexpr uint16_t DOFA_MIN_DAILY_SEC    = 60;   // min seconds/day to update EMA (skips idle days)
constexpr uint16_t DOFA_MIN_BASELINE_SEC = 300;  // min learned baseline (5 min) before checks activate
constexpr uint8_t  DOFA_WARN_FACTOR      = 150;  // warning at 1.5× learned baseline
constexpr uint8_t  DOFA_STOP_FACTOR      = 200;  // ALARM_OFA at 2.0× learned baseline

// Over-setpoint protection — mirrors the dead-band zone on the opposite side of the setpoint.
// If the reading stays beyond (setpoint ± deadbandW) on the wrong side for this long, ALARM_OVER_SETPOINT fires.
// Auto-clears the moment the reading returns to the dosing zone — no ACK needed.
constexpr uint32_t OVER_SETPOINT_DELAY_MS = 1800000UL;  // 30 min

// Mandatory settling time after the external stop callback clears.
// Prevents a dose from firing immediately when an operator toggles between filtration
// modes quickly — water may still be diverted or stationary during the transition.
constexpr unsigned long EXTERNAL_STOP_RESUME_MS = 5UL * 60UL * 1000UL;  // 5 minutes

// pH bounds for chlorine operations — applies to shock mode and pH-first priority guard (Option J).
// Below CL_PH_MIN: water too acidic for efficient Cl oxidation.
// Above CL_PH_MAX: Cl mostly in ineffective hypochlorite form; dosing wastes chemical.
constexpr float CL_PH_MIN = 7.0f;
constexpr float CL_PH_MAX = 7.6f;

// Dose efficiency EMA — smoothing factor for the learned delivery baseline.
// ~0.2 gives the last 5 doses strong influence; hardcoded for simplicity and AVR RAM savings.
constexpr float EFFICIENCY_EMA_ALPHA = 0.2f;

// Shock / super-chlorination mode
// ORP target must be within this range — 800 mV is the library's hard safety ceiling.
constexpr float SHOCK_ORP_MIN = 600.0f;
constexpr float SHOCK_ORP_MAX = 800.0f;
// Stop dosing this fraction before the target to compensate for Cl mixing / ORP lag.
constexpr float SHOCK_OVERSHOOT_MARGIN = 0.10f;
// Hard ceiling on active shock dosing duration — silently clamped.
constexpr uint8_t SHOCK_MAX_DURATION_HOURS = 4;
// Post-shock safety band suppression window. Default 24 h; max 48 h.
constexpr uint8_t SHOCK_COOLDOWN_DEFAULT_HOURS = 24;
constexpr uint8_t SHOCK_COOLDOWN_MAX_HOURS     = 48;
// ORP must rise at least this much within SHOCK_RISE_CHECK_MS — confirms chemical delivery.
// The time window is scaled by volumeScale() at point of use in manageShock().
constexpr unsigned long SHOCK_RISE_CHECK_MS = 20UL * 60UL * 1000UL;
constexpr float         SHOCK_RISE_MIN_MV   = 20.0f;

// Named ORP presets — use these instead of raw mV values for clarity.
constexpr uint16_t SHOCK_ORP_MILD       = 700;  // light — post-rain, minor algae risk
constexpr uint16_t SHOCK_ORP_STANDARD   = 750;  // weekly maintenance shock
constexpr uint16_t SHOCK_ORP_AGGRESSIVE = 800;  // heavy algae, after heavy bather load

// EEPROM configuration
// APAPHX2_ADS1115 occupies addresses 128-177 (pH cal + ORP cal).
// APA-Dose starts at 192, leaving a safe gap after the sensor library.
// sizeof(ConfigData) = 25 bytes (version 6+). Per-instance layout:
//   pump 1: 192–216   pump 2: 217–241   pump 3: 242–266   pump 4: 267–291
constexpr uint16_t APA_DOSE_EEPROM_ADDRESS = 192;
constexpr uint16_t APA_DOSE_MAGIC_NUMBER   = 0xABCD;

// Global slot — shared static values stored once, outside per-instance ConfigData.
// 3 bytes immediately before APA_DOSE_EEPROM_ADDRESS: [poolVolume][deadbandPct][0xA5]
// uint16_t matches APA_DOSE_EEPROM_ADDRESS type — avoids truncation on Mega (4KB EEPROM).
constexpr uint16_t APA_GLOBAL_EEPROM_ADDR = APA_DOSE_EEPROM_ADDRESS - 3;
constexpr uint8_t  APA_GLOBAL_VALID_BYTE  = 0xA5;

// Pool volume scaling — reference pool the library was calibrated on.
constexpr uint8_t REFERENCE_VOLUME_M3 = 20;

// Minimum safe buffer size for getSystemStatus().
// Worst-case output: "Sensor:1000.00 SP:900.00 Band:250.00 Type:pH- Dosing:YES Alarm:Dose ineffective"
// = ~80 chars; 96 provides a comfortable margin.
constexpr size_t APA_DOSE_STATUS_BUFFER_SIZE = 96;

// --- Public enumerations ---

// Chemical type installed in the pump
enum ApaDoseType : uint8_t {  // fixed underlying type — sizeof = 1 on all platforms
  DOSE_PH,  // pH pump — direction (raise or lower) set separately via ApaDoseDirection
  DOSE_CL   // Chlorine/oxidant — doses when ORP is below setpoint
};

// Dosing direction for pH pumps — ignored for DOSE_CL; use CL_PLUS alias with chlorine pumps
enum ApaDoseDirection : uint8_t {
  PH_PLUS,  // Base chemical — doses when pH is below setpoint
  PH_MINUS  // Acid chemical — doses when pH is above setpoint
};

// Semantic alias for DOSE_CL pumps — identical to PH_PLUS internally; direction is not used for chlorine
constexpr ApaDoseDirection CL_PLUS = PH_PLUS;

// Alarm conditions reported via callback
enum ApaDoseAlarm {
  ALARM_NONE,
  ALARM_WRONG_DIRECTION,  // sensor value moved opposite to expected (wrong chemical?)
  ALARM_INEFFECTIVE,      // sensor value did not change after multiple attempts (pump/supply issue)
  ALARM_SAFETY_BAND,      // sensor value drifted beyond safety limits
  ALARM_INVALID_PARAM,    // Configuration value rejected (out of allowed range)
  ALARM_DAILY_LIMIT,      // maximum daily dose count reached — auto-clears at midnight / 24 h
  ALARM_SENSOR_FAULT,     // Sensor reading invalid (out of range / NaN) for >2 min, or no reading for >30 min
  ALARM_TANK_EMPTY,       // chemical tank empty — requires refill and acknowledgeAlarm()
  ALARM_OFA,              // cumulative daily pump run time exceeded 90% of setOFALimit() — requires acknowledgeAlarm()
  ALARM_OVER_SETPOINT     // sensor has been on the wrong side of setpoint for >30 min — auto-clears when reading returns to dosing zone
};

// --- Internal structures ---

// Parameters for a single dosing pulse
struct DosingPulse {
  uint8_t       pwmIntensity;   // Pump speed  0-255
  unsigned long pulseDuration;  // Pump run time (ms)
  unsigned long restPeriod;     // Mixing wait after pulse (ms)
};

// Configuration saved to EEPROM.
// __attribute__((packed)) removes compiler padding so the checksum covers
// only real data bytes — no undefined padding bytes included.
// Bump APA_DOSE_CONFIG_VERSION whenever this struct layout changes.
struct __attribute__((packed)) ConfigData {
  uint16_t         magicNumber;      // Detects uninitialised EEPROM
  uint8_t          version;          // Config format version
  float            setpoint;         // User sensor target (pH or ORP mV)
  float            proportionalBand; // Control band width (fixed; user-configured)
  ApaDoseType      dosingType;       // Chemical type: DOSE_PH or DOSE_CL
  ApaDoseDirection phDirection;      // pH direction: PH_PLUS or PH_MINUS (ignored for DOSE_CL)
  uint8_t          nudgePct;         // Adaptive PB: 0 = disabled, 1–25 = nudge rate %
  float            adaptedPB;        // Adaptive PB: current learned value; 0.0 when disabled
  uint16_t         dofaLearnedSec;   // dOFA: EMA learned daily baseline (seconds); 0 = still learning
  uint8_t          tankCapacityL;    // Tank volume in litres (0 = disabled, default 20); max 65 L
  uint16_t         tankConsumedMl;   // Cumulative consumption since last refill/ACK (mL); saved at midnight
  uint16_t         checksum;         // Data integrity validation
};
constexpr uint8_t APA_DOSE_CONFIG_VERSION = 6;  // bumped: tankCapacityL + tankConsumedMl added; old EEPROM falls back to safe defaults

// Feedback phase state machine — replaces three separate bool fields
enum FeedbackPhase : uint8_t {
  FB_IDLE            = 0,
  FB_MEASURING_BEFORE,
  FB_WAITING,
  FB_MEASURING_AFTER
};

// Feedback control state
struct FeedbackState {
  float         valueBeforeDose;
  float         valueAfterDose;
  uint8_t       failedAttempts;        // max 3 before ALARM_INEFFECTIVE
  FeedbackPhase phase;                 // current feedback cycle phase
  float         sampleSum;
  uint8_t       sampleCount;           // max AFTER_SAMPLES = 3
  uint8_t       targetSamples;
  unsigned long nextSampleTime;        // millis() expiry for next sample window
  uint8_t       wrongDirectionCount;   // max 3 before ALARM_WRONG_DIRECTION
};

// Alarm state — boolean flags moved to ApaDose::flags bitfield
struct AlarmState {
  ApaDoseAlarm  currentAlarm;
  char          alarmMessage[20];  // max 19 chars — fits one LCD row
};

// Wall-clock time snapshot passed from external RTC (e.g. DS3231)
struct ApaDoseTime {
  uint8_t  hour;    // 0-23
  uint8_t  minute;  // 0-59
  uint8_t  second;  // 0-59
  uint8_t  day;     // 1-31
  uint8_t  month;   // 1-12
  uint16_t year;    // e.g. 2026
};

// --- Callback types ---
typedef void       (*AlarmCallback)(ApaDoseAlarm alarm, const char* message);
typedef void       (*StatusCallback)(const char* message);
typedef float      (*SensorReadCallback)();    // Return current sensor value (pH or ORP mV)
typedef bool       (*FilterCallback)();        // Return true if filtration pump is running
typedef bool       (*ExternalStopCallback)();  // Return true to block all dosing (except priming)
typedef bool       (*TankEmptyCallback)();     // Return true when chemical tank is empty
typedef ApaDoseTime (*RTCReadCallback)();       // Return current date/time from external RTC

// --- Main class ---

class ApaDose {
private:
  // Hardware
  uint8_t pumpPin;

  // Pump speed range - calibrate to your specific pump
  uint8_t pumpMinPWM;   // PWM where pump actually starts spinning (measure for your pump)
  uint8_t pumpMaxPWM;   // Maximum allowed PWM (usually 255)

  // Configuration
  float            setpoint;
  float            proportionalBand;
  ApaDoseType      dosingType;
  ApaDoseDirection phDirection;

  // Boolean state — 23 flags packed into 3 bytes (vs 23 bytes as individual bools)
  struct {
    bool dosingActive        : 1;
    bool blackoutMessageSent : 1;
    bool dosingWindowEnabled : 1;
    bool manualDoseActive    : 1;
    bool primingActive       : 1;
    bool configurationValid  : 1;
    bool lastDoseDataValid   : 1;
    bool filterOffAlarmSent  : 1;
    bool sensorValueBad      : 1;
    bool alarmActive         : 1;
    bool alarmNeedsAck       : 1;
    bool sensorStaleWarned   : 1;  // set after SENSOR_STALE_MS; cleared on next good read
    bool externalStopSent    : 1;  // rate-limits "ExtStop active" status message
    bool outsideDosingWindow : 1;  // cached result of last window check in update()
    bool shockActive         : 1;  // this instance is running shock mode
    bool shockHoldSent       : 1;  // rate-limits "Held:shock active" on non-shock instances
    bool deadbandSatisfied   : 1;  // set when sensor retreats past exit threshold; cleared on re-entry
    bool phHoldSent          : 1;  // rate-limits "CL held: pH high" status message (Option J)
    bool settleHoldSent      : 1;  // rate-limits "CL held: settling" status message (Option A)
    bool ofaWarningSent      : 1;  // rate-limits OFA 70% warning — reset at midnight
    bool dofaDisabled        : 1;  // disableDOFA() sets this; suppresses all dOFA checks
    bool dofaWarningSent     : 1;  // rate-limits dOFA 150% warning — reset at midnight
    bool overSetpointFired   : 1;  // prevents re-trigger while ALARM_OVER_SETPOINT is active
  } flags;

  // System state
  unsigned long dosingStartTime;
  unsigned long lastDosingEnd;
  unsigned long filterOffStart;         // millis() when filter was first seen off; 0 = filter on
  unsigned long externalStopClearedAt;  // millis() when external stop last cleared; 0 = not in resume delay
  DosingPulse   currentPulse;
  float         sensorValue;

  // Control structures
  FeedbackState feedback;
  AlarmState    alarm;

  // Callbacks
  AlarmCallback        onAlarmTriggered;
  AlarmCallback        onAlarmCleared;
  StatusCallback       onStatusMessage;
  SensorReadCallback   readSensor;
  FilterCallback       filterPumpRunning;
  ExternalStopCallback externalStop;
  TankEmptyCallback    tankEmpty;
  RTCReadCallback      readRTCTime;

  // pH-first priority (J) and cross-settle coupling (A) — per-instance, setup-time only
  ApaDose* _linkedPhPump       = nullptr;  // nullptr = both features disabled
  uint8_t  _crossSettleMinutes = 0;        // 0 = Option A disabled

  // Startup blackout
  uint8_t       startupBlackoutMinutes;  // 0 = disabled; stored as minutes to save 3 bytes vs unsigned long
  unsigned long startupTime;

  // RTC — dosing window and daily counter
  uint8_t dosingWindowStart;   // first hour allowed (0-23)
  uint8_t dosingWindowEnd;     // first hour blocked (0-23), must be > start
  uint8_t lastKnownDay;        // tracks day changes for counter reset (255 = unknown)
  uint8_t dailyDoseCount;      // doses started today
  uint8_t maxDailyDoses;       // 0 = no limit; set via begin() parameter

  // Sensor read timing
  unsigned long lastSensorRead;
  unsigned long lastGoodSensorTime;  // millis() of last finite sensor reading; 0 before first read
  unsigned long lastDailyReset;      // millis() of last 24 h counter reset (used when no RTC)

  // Manual dose and priming
  unsigned long primingStartTime;
  unsigned long primingDuration;

  // Per-instance EEPROM base address
  uint16_t eepromBaseAddress;

  // Last completed dose diagnostics
  float lastDoseSensorBefore;  // averaged before-dose sensor value
  float lastDoseSensorAfter;   // averaged after-dose sensor value

  // Volume tracking
  float pumpFlowRateMlPerMin;  // pump output at max PWM; default 450 mL/min
  float dailyVolumeMl;         // accumulated volume today (resets at midnight with RTC)
  float lastDoseVolumeMl;      // volume of the last completed dose

  // Over-feed alarm (OFA) — cumulative pump run-time limit per day
  uint16_t _dailyPumpRunSec;  // accumulated pump-on time today (seconds); resets at midnight
  uint8_t  _ofaLimitMin;      // reference limit at 20 m³ (minutes); 0 = disabled (default)

  // Dynamic OFA (dOFA) — self-learning proportional-only run-time baseline
  uint16_t _dofaLearnedSec;   // EMA learned daily baseline (seconds); 0 = still learning
  uint16_t _dofaDailyRunSec;  // proportional-only run time today (seconds); excludes shock + manual
  uint8_t  _dofaAdaptDays;    // EMA smoothing factor: N in (N-1)/N; range 3–14, default 10

  // Over-setpoint protection
  unsigned long _overSetpointSince;  // millis() when reading first crossed mirror threshold; 0 = not triggered

  // Tank level estimation — works without a physical tank sensor; complements setTankEmptyCallback()
  uint8_t  _tankCapacityL;   // user tank size in litres; 0 = disabled, default 20; max 65
  uint16_t _tankConsumedMl;  // mL dispensed since last refill/ACK; persisted to EEPROM at midnight
  uint8_t  _dailyAvgDL;     // EMA of daily consumption in decilitres (1 dL = 100 mL); 0 = no data yet

  // Scheduled pre-dose (C-pred) — requires RTC; inert when _schedDurationMs == 0
  uint8_t       _schedHour;          // 0-23
  uint8_t       _schedMinute;        // 0-59
  unsigned long _schedDurationMs;    // 0 = not configured
  float         _schedThreshold;     // 0.0 = no condition; non-zero = sensor boundary
  uint8_t       _schedIntervalDays;  // 1 = daily, 7 = weekly, etc.
  uint8_t       _schedDaysRemaining; // countdown; 0 = fire at next scheduled time
  uint8_t       _schedLastSeenDay;   // day-of-month when last evaluated (255 = never)

  // Adaptive proportional band
  uint8_t nudgePct;   // 0 = disabled; 1–25 = nudge rate per cycle
  float   adaptedPB;  // current learned PB; seeded from proportionalBand on first enable

  // Dose efficiency EMA — tracks delivery health; always active after auto proportional doses
  float   _efficiencyEma          = 0.0f;
  uint8_t _efficiencyCount        = 0;    // cold-start counter; alarm suppressed until > 3
  uint8_t _lastEfficiencyPct      = 100;  // last dose ratio vs baseline (0–100); 100 = at/above baseline
  uint8_t _efficiencyThresholdPct = 20;   // alarm fires below this %; 0 = alarm disabled

  // Shock mode state — 21 bytes per instance
  unsigned long shockStartTime;        // millis() at shock start
  unsigned long postShockCooldownEnd;  // millis() cooldown deadline (no RTC); 0 = inactive; also guards min inter-shock interval
  ApaDoseTime   postShockEndRTC;       // RTC shock-end timestamp (with RTC); year==0 = inactive; survives power cycles
  uint16_t      shockEffectiveStop;    // precomputed ORP stop threshold (startORP + gap*0.90), mV
  uint16_t      shockRiseTarget;       // precomputed ORP rise threshold (startORP + SHOCK_RISE_MIN_MV); 0 = check already fired
  uint8_t       shockMaxDurationHours; // clamped active dosing ceiling in hours
  uint8_t       shockCooldownHours;    // post-shock settling window in hours

  // Shared across all instances — inter-pump lockout and shock interlock
  static unsigned long lastAnyDoseEnd;
  static bool          shockModeActive;  // true while any instance is shocking — blocks all others

  // Shared static values — pool property and tuning parameter, one value per system
  static uint8_t s_poolVolume;   // 0=off; valid 10–90 m³; survives factoryReset()
  static uint8_t s_deadbandPct;  // 0=off; valid 0–20 % of PB; cleared by factoryReset()

  // Sensor profile helpers — read compile-time constants directly from flash; no SRAM copies
  bool isOrpProfile() const { return dosingType == DOSE_CL; }
  bool dosesUp()      const { return dosingType == DOSE_CL || phDirection == PH_PLUS; }

  // Internal methods
  void         readSensors();
  void         manageProportionalDosing();
  void         manageFeedbackSampling();
  void         manageScheduledDose();
  bool         collectSample(unsigned long now, char prefix);
  bool         shouldStartDosing();
  DosingPulse  calculateProportionalPulse();
  void         startDosingPulse(DosingPulse pulse);
  void         stopDosingPulse();
  DosingPulse  applyFeedbackCorrections(DosingPulse originalPulse);
  void         startBeforeDosingMeasurements();
  void         startAfterDosingMeasurements();
  void         evaluateFeedback();
  void         checkSafetyConditions();
  void         triggerAlarm(ApaDoseAlarm type, const char* message);
  void         checkAlarmClearConditions();
  void         clearAlarm();
  static const char* getAlarmName(ApaDoseAlarm type);
  float        getEffectiveSafetyBand() const;
  bool         loadConfiguration();
  void         saveConfiguration();
  bool         validateConfiguration(const ConfigData& config);
  uint16_t     calculateChecksum(const ConfigData& config);
  void         resetToDefaults();
  void         accumulateAndCheckOFA(unsigned long durationMs);
  void         accumulateAndCheckDOFA(unsigned long durationMs);
  void         checkOverSetpoint();
  void         manageShock();
  void         stopShock(const __FlashStringHelper* msg);
  static uint32_t toApproxHours(ApaDoseTime t);
  static float volumeScale();    // s_poolVolume==0 → 1.0; else poolVolume/REFERENCE_VOLUME_M3
  static void  saveGlobalSlot(); // writes poolVolume + deadbandPct + validity atomically

public:
  // Constructor - one pin per pump, through MOSFET.
  // eepromAddress: base EEPROM address for this instance's configuration.
  // Each pump in a multi-pump setup must use a unique address spaced by sizeof(ConfigData).
  // Single-pump sketches can omit it — the default (APA_DOSE_EEPROM_ADDRESS) is used.
  ApaDose(uint8_t pumpPin, uint16_t eepromAddress = APA_DOSE_EEPROM_ADDRESS);

  // Initialization - call from setup()
  void setPumpRange(uint8_t minPWM, uint8_t maxPWM);                              // Calibrate to your pump (call before begin)
  void setPumpFlowRate(float mlPerMin);                                            // Pump output at max PWM (mL/min); optional, default 450
  void setRTCCallback(RTCReadCallback rtcReader);                                  // Connect external RTC (call before begin)
  void setDosingWindow(uint8_t startHour, uint8_t endHour);                       // Restrict dosing to hour range 0-23 (call before begin)
  void setExternalStopCallback(ExternalStopCallback cb);                           // Optional: block all dosing (except priming) when cb returns true
  void setTankEmptyCallback(TankEmptyCallback cb);                                  // Optional: fire ALARM_TANK_EMPTY (latching) when cb returns true; also blocks priming
  // Tank level estimation — independent of setTankEmptyCallback(); both are optional.
  // setTankCapacity: set tank size in litres (1–65); default 20 L. Resets consumed counter to 0.
  //   Call in setup() for a fixed size, or conditionally in loop() when the tank is replaced.
  //   0 = disable estimation; getTankRemainingPct() / getTankDaysUntilEmpty() return 255 (unknown).
  // If setTankEmptyCallback() is also registered, the HW sensor is the sole alarm authority —
  //   ALARM_TANK_EMPTY never fires from estimation. Percentage display still works normally.
  void    setTankCapacity(uint8_t liters);      // 1–65 L; 0 = disable; default 20
  // pH-first priority (J) + cross-settle coupling (A) — call AFTER both pump begin() calls.
  // setPhPump: registers the pH peer; activates J (fixed threshold CL_PH_MAX) automatically.
  // setCrossSettleMinutes: also activates A — CL held N min after each pH dose; 0 = off.
  // Both features are inert (nullptr default) — omit for pH-only or independent setups.
  void setPhPump(ApaDose* phPump);
  void setCrossSettleMinutes(uint8_t minutes);
  bool begin(SensorReadCallback sensorReader,
             FilterCallback   filter,
             ApaDoseType      type,
             ApaDoseDirection dir,
             uint8_t          blackoutMinutes = 0,
             uint8_t          maxDailyDoses   = 0);            // 0 = no limit
  bool begin(SensorReadCallback sensorReader,
             ApaDoseType      type,
             ApaDoseDirection dir,
             uint8_t          blackoutMinutes = 0,
             uint8_t          maxDailyDoses   = 0);            // no-filter shorthand
  void setCallbacks(AlarmCallback alarmTriggered,
                    AlarmCallback alarmCleared  = nullptr,
                    StatusCallback statusMessage = nullptr);

  // Main loop function - call every loop() iteration
  void update();

  // Manual control - works on all pump types
  // restMs: mixing wait before next proportional dose (default = worst-case 20 min).
  // Pass 0 only for sensor-less pumps where the daily-limit already controls frequency.
  bool triggerManualDose(unsigned long durationMs,
                         unsigned long restMs = 20UL * 60UL * 1000UL);
  bool triggerPrime(unsigned long durationMs, uint8_t pwm = 0);  // 0 = use pumpMaxPWM; bypasses all safety guards

  // Scheduled pre-dose — requires RTC callback; inert without one. Call before or after begin().
  // Fires once per intervalDays at the given hour:minute, subject to all triggerManualDose() guards.
  // threshold: condition that must be met before the dose fires.
  //   NAN (default) — use the pump's own setpoint as the condition (dose only when sensor has drifted).
  //   0.0f          — always fire regardless of sensor reading.
  //   Any finite value — explicit override (e.g. 7.4 on a pH pump).
  //   Lowering pumps (PH_MINUS): fires when sensorValue > threshold.
  //   Raising pumps (PH_PLUS / CL_PLUS): fires when sensorValue < threshold.
  //   Sensor-less pumps (algaecide, flocculant): threshold is ignored — always doses.
  // intervalDays: 1 = daily (default), 2 = every 2 days, 7 = weekly, etc.
  // Pass durationMs = 0 to disable.
  void setScheduledDose(uint8_t hour, uint8_t minute,
                        unsigned long durationMs,
                        uint8_t intervalDays = 1,
                        float   threshold    = NAN);

  // Shock / super-chlorination — DOSE_CL instances only; filter callback required
  // Hobbyist: pass SHOCK_ORP_STANDARD (or SHOCK_ORP_MILD / SHOCK_ORP_AGGRESSIVE) and current pH.
  // Pro: additionally specify max active duration (hours, clamped to 4 h) and cooldown window (hours, default 24 h, max 48 h).
  // currentPH: pass phPump.getProbeValue() — must be in CL_PH_MIN–CL_PH_MAX (7.0–7.6).
  // Returns false if any entry guard fails (see API.md for full list).
  bool triggerShock(uint16_t targetORP, float currentPH,
                    uint8_t cooldownHours = SHOCK_COOLDOWN_DEFAULT_HOURS);
  bool triggerShock(uint16_t targetORP, uint8_t maxDurationHours, float currentPH,
                    uint8_t cooldownHours = SHOCK_COOLDOWN_DEFAULT_HOURS);
  bool          isShockActive()            const;  // true while shock dosing is running
  unsigned long getShockRemainingSeconds() const;  // seconds to time ceiling; 0 if not active

  // Configuration
  bool setProbeSetpoint(float newSetpoint);       // Valid range depends on dosing type
  bool setProportionalBand(float newBand);        // Valid range depends on dosing type
  bool setDosingType(ApaDoseType newType);        // Runtime type change (DOSE_PH ↔ DOSE_CL)
  bool setPhDirection(ApaDoseDirection newDir);   // Runtime direction change; ignored for DOSE_CL
  void enableAdaptivePB(uint8_t pct);             // 0 = disable (resets learned value); 1–25 = nudge rate %
  void    setEfficiencyThreshold(uint8_t pct);    // 0 = alarm off; default 20 (active out of the box)
  uint8_t getEfficiencyThreshold()         const;
  // Over-feed alarm — cumulative daily pump run-time limit, auto-scaled by setPoolVolume().
  // referenceMinutes is the limit at the 20 m³ reference pool; 0 = disabled (default).
  // Warning status fires at 70%, ALARM_OFA hard-stops dosing at 90%; requires acknowledgeAlarm().
  // Dosing resumes automatically at midnight after ACK. Priming is exempt.
  void    setOFALimit(uint8_t referenceMinutes = 30);
  uint8_t getOFAPct() const;  // 0–100 % of today's scaled limit consumed; 0 when disabled

  // Dynamic OFA (dOFA) — self-learning daily baseline; always on, no configuration required.
  // Learns the normal proportional run time for THIS pool and fires ALARM_OFA when today's
  // run time exceeds 2× the learned baseline (warning at 1.5×). Both dOFA and fixed OFA
  // are independent — whichever fires first controls. Excludes manual doses, shock, and prime.
  // EMA baseline is persisted to EEPROM at midnight and survives power cycles.
  void    setDOFAAdaptDays(uint8_t days);  // EMA speed: 3–14 days, default 10; call in setup()
  void    disableDOFA();                   // suppress all dOFA checks for this instance
  uint8_t getDOFAPct() const;              // today's proportional run vs baseline (0–100 %); 0 = learning
  bool    isDOFALearning() const;          // true until the first qualifying day (typically day 2)
  void    resetDOFA();                     // clear baseline + daily counter; call at spring opening
  void acknowledgeAlarm();
  void forceConfigurationSave();
  // Resets per-instance EEPROM fields to type-defaults, stops any active dose/prime/shock, clears alarm.
  // Dead-band (tuning parameter) is cleared. Pool volume (installation parameter) is NOT touched.
  void factoryReset();

  // System-wide static values — call once in setup(); affect all pump instances.
  // Pool volume survives factoryReset(); dead-band is cleared by factoryReset().
  static bool    setPoolVolume(uint8_t m3);    // 10–90 m³; 0 = off (scale 1.0); false if out of range
  static uint8_t getPoolVolume();
  static bool    setDeadbandPct(uint8_t pct);  // 0–20 % of PB; 0 = off; false if > 20
  static uint8_t getDeadbandPct();

  // Status queries
  float            getProbeValue()              const;
  float            getCurrentSetpoint()        const;
  float            getCurrentProportionalBand() const;
  ApaDoseType      getCurrentDosingType()      const;
  ApaDoseDirection getPhDirection()            const;
  ApaDoseAlarm getCurrentAlarm()           const;
  bool         isAlarmActive()             const;
  bool         isDosingActive()            const;
  bool         isPrimingActive()           const;
  bool         isInStartupBlackout()       const;
  bool         isExternalStopActive()           const;  // true if external stop callback is registered and currently returning true
  bool         isInExternalStopResumeDelay()    const;  // true during the mandatory 5-min settling wait after external stop clears
  bool         isOutsideDosingWindow()          const;  // true if dosing window is enabled and current hour is outside it
  bool         isConfigurationValid()           const;
  unsigned long getLastDosingTime()        const;  // millis() when last dose ended (alias kept for compatibility)
  unsigned long getLastDosingEnd()         const;  // millis() when last dose ended; 0 if never dosed — used by linked CL pump for Option A
  unsigned long getSecondsSinceLastDose()  const;  // seconds since last dose ended; 0 if no dose yet
  unsigned long getSecondsUntilNextDose()  const;  // seconds remaining in rest period; 0 if eligible now
  uint8_t       getFailedAttempts()        const;
  uint8_t       getDailyDoseCount()        const;  // resets daily only when RTC callback is registered
  uint8_t       getMaxDailyDoses()        const;  // configured limit; 0 = no limit
  float         getDailyVolumeMl()        const;  // total mL dosed today; resets at midnight with RTC
  float         getLastDoseVolumeMl()     const;  // mL dosed in the last completed dose
  uint8_t       getTankRemainingPct()     const;  // 0–100 % tank remaining; 255 = estimation disabled (setTankCapacity not set)
  uint8_t       getTankDaysUntilEmpty()   const;  // estimated days until tank empty; 255 = no data yet (< 1 full day elapsed)
  const char*   getAlarmMessage()          const;  // current alarm text, empty string if no alarm

  // Dose diagnostics — valid after the first complete dose + feedback cycle
  bool  hasDoseHistory()           const;  // false until first full dose+feedback cycle
  float getLastDoseSensorBefore()  const;  // averaged sensor value before last dose
  float getLastDoseSensorAfter()   const;  // averaged sensor value after last dose
  uint8_t getDoseEffectiveness()   const;  // 0–100: last dose vs EMA baseline; 100 until baseline established (3+ doses)

  // Adaptive proportional band
  float getAdaptedPB()             const;  // current effective PB (learned or fixed)
  bool  isAdaptivePBEnabled()      const;  // true when nudgePct > 0

  // Diagnostics
  void getSystemStatus(char* buffer, size_t bufferSize) const;

  // Library info
  static const char* getVersion();
  static void        printLibraryInfo();
};

#endif // APADOSE_H
