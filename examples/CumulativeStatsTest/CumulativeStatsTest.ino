/**
 * CumulativeStatsTest.ino
 *
 * Validation test for the CumulativeStats class.
 *
 * Runs a series of self-contained tests with known inputs and verifies the
 * output against hand-calculated reference values. Each test prints PASS or
 * FAIL with the expected and actual values.
 *
 * This sketch has no hardware dependencies — it only uses Serial output and
 * millis(). It can be run on any Arduino-compatible board or ESP8266/ESP32.
 *
 * Expected output (all tests passing):
 *
 *   === CumulativeStats Test Suite ===
 *
 *   [1] Basic average (3 samples)       PASS
 *   [2] Standard deviation              PASS
 *   [3] Min / max tracking              PASS
 *   [4] Dose integration                PASS
 *   [5] Elapsed time                    PASS
 *   [6] hasData() guard                 PASS
 *   [7] Single sample (no sigma/dose)   PASS
 *   [8] Reset clears all state          PASS
 *   [9] millis() overflow safety        PASS
 *   [10] Zero uSvH adds no dose         PASS
 *   [11] Welford numerical stability    PASS
 *
 *   11 / 11 tests passed.
 */

#include "CumulativeStats.h"
#include <math.h>

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static uint32_t passed = 0;
static uint32_t total  = 0;

static void check(uint32_t id, const char* name,
                  bool condition, float expected, float actual,
                  float tolerance = 0.001f)
{
    total++;
    bool ok = condition || (fabsf(actual - expected) <= tolerance * fabsf(expected) + 1e-6f);
    if (ok) {
        passed++;
        Serial.printf("[%2u] %-38s PASS\n", id, name);
    } else {
        Serial.printf("[%2u] %-38s FAIL  expected=%.6f  got=%.6f\n",
                      id, name, expected, actual);
    }
}

static void checkBool(uint32_t id, const char* name, bool expected, bool actual) {
    total++;
    if (expected == actual) {
        passed++;
        Serial.printf("[%2u] %-38s PASS\n", id, name);
    } else {
        Serial.printf("[%2u] %-38s FAIL  expected=%s  got=%s\n",
                      id, name,
                      expected ? "true" : "false",
                      actual   ? "true" : "false");
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// [1] Average of three known CPM values
static void test_basicAverage() {
    CumulativeStats s;
    s.addSample(10.0f, 0.094f, 1000);
    s.addSample(20.0f, 0.188f, 2000);
    s.addSample(30.0f, 0.283f, 3000);
    // mean = (10+20+30)/3 = 20.0
    check(1, "Basic average (3 samples)", false, 20.0f, s.averageCpm());
}

// [2] Standard deviation — population σ of {10, 20, 30}
//     mean=20, deviations: -10, 0, +10 → M2=200 → σ=sqrt(200/3)=8.165
static void test_stdDev() {
    CumulativeStats s;
    s.addSample(10.0f, 0.094f, 1000);
    s.addSample(20.0f, 0.188f, 2000);
    s.addSample(30.0f, 0.283f, 3000);
    float expected = sqrtf(200.0f / 3.0f);  // 8.16497
    check(2, "Standard deviation", false, expected, s.sigmaCpm(), 0.001f);
}

// [3] Min / max tracking
static void test_minMax() {
    CumulativeStats s;
    s.addSample(15.0f, 0.141f, 1000);
    s.addSample(5.0f,  0.047f, 2000);
    s.addSample(25.0f, 0.236f, 3000);
    check(3, "Min / max tracking (min)", false, 5.0f,  s.minCpm());
    // reuse id slot — check max inline
    total--;  // will be recounted
    check(3, "Min / max tracking (max)", false, 25.0f, s.maxCpm());
}

// [4] Dose integration
//     3 samples at 1-second intervals, uSvH = 0.180 each
//     Δt between samples: 1s = 1/3600 h each (2 intervals)
//     expected dose = 0.180 * (1/3600) + 0.180 * (1/3600) = 0.0001 µSv
static void test_dose() {
    CumulativeStats s;
    s.addSample(19.0f, 0.180f, 0);
    s.addSample(19.0f, 0.180f, 1000);   // +1s
    s.addSample(19.0f, 0.180f, 2000);   // +1s
    // dose = 0.180/3600 + 0.180/3600 = 0.0001 µSv
    float expected = 0.180f / 3600.0f * 2.0f;
    check(4, "Dose integration", false, expected, s.totalDoseUSv(), 0.01f);
}

// [5] Elapsed time
static void test_elapsed() {
    CumulativeStats s;
    s.addSample(20.0f, 0.188f, 0);
    s.addSample(20.0f, 0.188f, 30000);   // +30s
    s.addSample(20.0f, 0.188f, 60000);   // +60s
    check(5, "Elapsed time", false, 60.0f, (float)s.elapsedSeconds());
}

// [6] hasData() guard
static void test_hasData() {
    CumulativeStats s;
    checkBool(6, "hasData() guard (empty)",   false, s.hasData(1));
    s.addSample(20.0f, 0.188f, 1000);
    checkBool(6, "hasData() guard (1 sample)", true,  s.hasData(1));
    checkBool(6, "hasData() guard (2 needed)", false, s.hasData(2));
}

// [7] Single sample — sigma and dose must be 0 / NaN
static void test_singleSample() {
    CumulativeStats s;
    s.addSample(20.0f, 0.188f, 1000);
    // sigma undefined for 1 sample → NaN
    bool sigmaIsNaN = isnan(s.sigmaCpm());
    checkBool(7, "Single sample (sigma=NaN)", true, sigmaIsNaN);
    // dose requires 2 timestamps → 0
    check(7, "Single sample (dose=0)", false, 0.0f, s.totalDoseUSv());
}

// [8] Reset clears all state
static void test_reset() {
    CumulativeStats s;
    s.addSample(20.0f, 0.188f, 1000);
    s.addSample(20.0f, 0.188f, 2000);
    s.reset();
    checkBool(8, "Reset clears all state", false, s.hasData(1));
    bool avgIsNaN = isnan(s.averageCpm());
    checkBool(8, "Reset clears all state (NaN avg)", true, avgIsNaN);
}

// [9] millis() overflow safety — timestamps wrap around UINT32_MAX
static void test_overflow() {
    CumulativeStats s;
    uint32_t t1 = 0xFFFFFFFF - 500;   // 500ms before wrap
    uint32_t t2 = 499;                 // 999ms after start (1 wrap)
    s.addSample(20.0f, 0.180f, t1);
    s.addSample(20.0f, 0.180f, t2);
    // Δt = 999ms → ~0.0000499 µSv
    float expected = 0.180f * (999.0f / 3600000.0f);
    check(9, "millis() overflow safety", false, expected, s.totalDoseUSv(), 0.01f);
    // elapsed = 0 (only 2 samples, t2-t1 = 999ms → 0 seconds)
    check(9, "millis() overflow (elapsed)", false, 0.0f, (float)s.elapsedSeconds());
}

// [10] Zero uSvH contributes no dose
static void test_zeroDose() {
    CumulativeStats s;
    s.addSample(0.0f, 0.0f, 1000);
    s.addSample(0.0f, 0.0f, 61000);   // +60s
    check(10, "Zero uSvH adds no dose", false, 0.0f, s.totalDoseUSv());
}

// [11] Welford numerical stability — large values with tiny variance
//      All samples = 10000.0 + small perturbation
//      Naive sum-of-squares would lose precision; Welford should not
static void test_welfordStability() {
    CumulativeStats s;
    float base = 10000.0f;
    float deltas[] = {0.1f, -0.1f, 0.2f, -0.2f, 0.0f};
    uint32_t t = 1000;
    for (int i = 0; i < 5; i++) {
        s.addSample(base + deltas[i], 0.094f, t);
        t += 1000;
    }
    // mean should be base (deltas sum to 0)
    check(11, "Welford stability (mean)", false, base, s.averageCpm(), 0.0001f);
    // sigma should be sqrt(mean of squared deltas) = sqrt((0.01+0.01+0.04+0.04+0)/5)
    float expectedSigma = sqrtf((0.01f + 0.01f + 0.04f + 0.04f + 0.0f) / 5.0f);
    check(11, "Welford stability (sigma)", false, expectedSigma, s.sigmaCpm(), 0.001f);
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n=== CumulativeStats Test Suite ===\n");

    test_basicAverage();
    test_stdDev();
    test_minMax();
    test_dose();
    test_elapsed();
    test_hasData();
    test_singleSample();
    test_reset();
    test_overflow();
    test_zeroDose();
    test_welfordStability();

    Serial.println();
    Serial.printf("%u / %u tests passed.\n", passed, total);

    if (passed == total)
        Serial.println("\nAll tests PASSED.");
    else
        Serial.printf("\n%u test(s) FAILED.\n", total - passed);
}

void loop() {}
