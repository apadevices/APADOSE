/*
 * APA-Dose Example — 01: Flow Rate Calibration  [UTILITY]
 *
 * Measures your pump's flow rate (mL/min) at full speed.
 * The library uses this value to calculate getDailyVolumeMl() and
 * getLastDoseVolumeMl() — without it, both return zero.
 *
 * How it works:
 *   The library models volume as:
 *     mL = (PWM / 255) × (flowRate_mL_per_min / 60000) × duration_ms
 *   Flow rate is the reference at PWM 255 (full speed). Every other PWM
 *   value scales proportionally from this baseline. Calibrating at 255
 *   gives the library the correct reference for all dose intensities.
 *
 * Run this sketch once per pump after completing PWM calibration (example 00).
 * Use the actual chemical — water has a different viscosity and will give
 * inaccurate results.
 *
 * Hardware:
 *   PIN_PUMP   any PWM-capable pin (~)   MOSFET gate → your dosing pump
 *
 * Instructions:
 *   1. Set PIN_PUMP below to match your wiring.
 *   2. Fill the chemical container with exactly VOLUME_ML (default 500 mL).
 *      Route the pump outlet back into a waste container (not back into the pool).
 *   3. Upload and open Serial Monitor at 115200 baud, line ending: Newline.
 *   4. Type 'run' + Enter — pump starts at full speed.
 *   5. Watch the container. The moment it empties, type 'stop' + Enter.
 *   6. The sketch calculates flow rate and prints the setPumpFlowRate() line
 *      to copy into your main sketch.
 *   7. Repeat 2–3 times and average for higher accuracy.
 *   8. Repeat for each pump (change PIN_PUMP at the top).
 *
 * Author: kecup@vazac.eu (APA Devices)
 */

#include <Arduino.h>

const uint8_t PIN_PUMP   = 9;    // ← change to your pump pin (must be PWM-capable, ~)
const float   VOLUME_ML  = 500.0f; // mL in the container — change if you use a different amount

bool          running       = false;
unsigned long runStartTime  = 0;

void printResult(unsigned long elapsedMs) {
  float mlPerMin = (VOLUME_ML / (float)elapsedMs) * 60000.0f;

  Serial.println();
  Serial.println(F("--- Result ---"));
  Serial.print(F("Volume pumped : "));
  Serial.print(VOLUME_ML, 0);
  Serial.println(F(" mL"));
  Serial.print(F("Time elapsed  : "));
  Serial.print(elapsedMs / 1000UL);
  Serial.print(F("."));
  Serial.print((elapsedMs % 1000UL) / 100UL);
  Serial.println(F(" s"));
  Serial.print(F("Flow rate     : "));
  Serial.print(mlPerMin, 1);
  Serial.println(F(" mL/min  (at PWM 255)"));
  Serial.println();
  Serial.print(F("Copy into your main sketch:  setPumpFlowRate("));
  Serial.print(mlPerMin, 1);
  Serial.println(F(");"));
  Serial.println();
  Serial.println(F("Call setPumpFlowRate() in setup(), after setPumpRange() and before begin()."));
  Serial.println(F("Refill and type 'run' again to repeat, or change PIN_PUMP for the next pump."));
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PUMP, OUTPUT);
  analogWrite(PIN_PUMP, 0);

  Serial.println();
  Serial.println(F("=== APA-Dose Flow Rate Calibration ==="));
  Serial.print(F("Fill the container with exactly "));
  Serial.print(VOLUME_ML, 0);
  Serial.println(F(" mL of chemical, then type 'run'."));
  Serial.println();
}

void loop() {
  if (!Serial.available()) return;

  String input = Serial.readStringUntil('\n');
  input.trim();
  if (input.length() == 0) return;

  if (!running && input.equalsIgnoreCase(F("run"))) {
    analogWrite(PIN_PUMP, 255);
    runStartTime = millis();
    running      = true;
    Serial.println(F("Pump running... type 'stop' the moment the container empties."));

  } else if (running && input.equalsIgnoreCase(F("stop"))) {
    unsigned long elapsed = millis() - runStartTime;
    analogWrite(PIN_PUMP, 0);
    running = false;
    printResult(elapsed);

  } else if (running) {
    Serial.println(F("Pump is running — type 'stop' to stop it."));

  } else {
    Serial.println(F("Type 'run' to start."));
  }
}
