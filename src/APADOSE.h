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
 * Version: 3.8.3
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
#define APA_DOSE_VERSION "3.8.3"
#define APA_DOSE_VERSION_MAJOR 3
#define APA_DOSE_VERSION_MINOR 8
#define APA_DOSE_VERSION_PATCH 3

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

// Maximum pulse duration applied during feedback correction (2+ failed attempts).
// 30% above the 11 s proportional maximum — enough extra reach without over-dosing.
constexpr unsigned long FEEDBACK_PULSE_MAX_MS = 14300UL;

// How long the filtration pump must be continuously off before a status warning fires.
constexpr unsigned long FILTER_OFF_ALARM_MS = 30UL * 60UL * 1000UL;  // 30 minutes

// Mandatory settling time after the external stop callback clears.
// Prevents a dose from firing immediately when an operator toggles between filtration
// modes quickly — water may still be diverted or stationary during the transition.
constexpr unsigned long EXTERNAL_STOP_RESUME_MS = 5UL * 60UL * 1000UL;  // 5 minutes

// EEPROM configuration
// APAPHX2_ADS1115 occupies addresses 128-177 (pH cal + ORP cal).
// APA-Dose starts at 192, leaving a safe gap after the sensor library.
constexpr uint16_t APA_DOSE_EEPROM_ADDRESS = 192;
constexpr uint16_t APA_DOSE_MAGIC_NUMBER   = 0xABCD;

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
  ALARM_DAILY_LIMIT,      // maximum daily dose count reached — requires human check
  ALARM_SENSOR_FAULT      // Sensor reading invalid (out of range / NaN) for >2 min, or no reading for >30 min
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
  uint16_t         checksum;         // Data integrity validation
};
constexpr uint8_t APA_DOSE_CONFIG_VERSION = 4;

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
  unsigned long feedbackCheckTime;
  float         sampleSum;
  uint8_t       sampleCount;           // max AFTER_SAMPLES = 3
  uint8_t       targetSamples;
  unsigned long nextSampleTime;
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

  // Boolean state — 14 flags packed into 2 bytes (vs 14 bytes as individual bools)
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
  RTCReadCallback      readRTCTime;

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

  // Adaptive proportional band
  uint8_t nudgePct;   // 0 = disabled; 1–25 = nudge rate per cycle
  float   adaptedPB;  // current learned PB; seeded from proportionalBand on first enable

  // Shared across all instances — inter-pump lockout
  static unsigned long lastAnyDoseEnd;

  // Sensor profile helpers — read compile-time constants directly from flash; no SRAM copies
  bool isOrpProfile() const { return dosingType == DOSE_CL; }
  bool dosesUp()      const { return dosingType == DOSE_CL || phDirection == PH_PLUS; }

  // Internal methods
  void         readSensors();
  void         manageProportionalDosing();
  void         manageFeedbackSampling();
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

  // Configuration
  bool setProbeSetpoint(float newSetpoint);       // Valid range depends on dosing type
  bool setProportionalBand(float newBand);        // Valid range depends on dosing type
  bool setDosingType(ApaDoseType newType);        // Runtime type change (DOSE_PH ↔ DOSE_CL)
  bool setPhDirection(ApaDoseDirection newDir);   // Runtime direction change; ignored for DOSE_CL
  void enableAdaptivePB(uint8_t pct);             // 0 = disable (resets learned value); 1–25 = nudge rate %
  void acknowledgeAlarm();
  void forceConfigurationSave();
  void factoryReset();                            // force-stop dose/prime, reset all EEPROM fields to type-defaults, clear alarm

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
  unsigned long getLastDosingTime()        const;  // millis() when last dose ended
  unsigned long getSecondsSinceLastDose()  const;  // seconds since last dose ended; 0 if no dose yet
  unsigned long getSecondsUntilNextDose()  const;  // seconds remaining in rest period; 0 if eligible now
  uint8_t       getFailedAttempts()        const;
  uint8_t       getDailyDoseCount()        const;  // resets daily only when RTC callback is registered
  uint8_t       getMaxDailyDoses()        const;  // configured limit; 0 = no limit
  float         getDailyVolumeMl()        const;  // total mL dosed today; resets at midnight with RTC
  float         getLastDoseVolumeMl()     const;  // mL dosed in the last completed dose
  const char*   getAlarmMessage()          const;  // current alarm text, empty string if no alarm

  // Dose diagnostics — valid after the first complete dose + feedback cycle
  bool  hasDoseHistory()           const;  // false until first full dose+feedback cycle
  float getLastDoseSensorBefore()  const;  // averaged sensor value before last dose
  float getLastDoseSensorAfter()   const;  // averaged sensor value after last dose
  float getDoseEffectiveness()     const;  // signed % of band: positive = correct direction

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
