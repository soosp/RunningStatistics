/**
 * TemperatureAverage.ino
 *
 * Example sketch for RollingStats — sliding window statistics on
 * simulated temperature sensor data.
 *
 * Demonstrates:
 *   - addSample() with a millisecond timestamp
 *   - average(), minimum(), maximum(), stdDev() over different windows
 *   - hasValidWindow() and validSeconds() for startup guard
 *   - maxWindowSeconds() for reporting capacity
 *   - reset() to clear history
 *
 * No real sensor required — the sketch generates a synthetic temperature
 * signal: a slow sine wave (daily cycle) plus small random noise, sampled
 * once per second.
 *
 * Output (Serial, 115200 baud):
 *   One line per second with the current reading and all available window
 *   statistics. Every 5 minutes: a summary block.
 *
 * Memory (at default settings — 120 bins × 60 s):
 *   Each bin uses 20 bytes → 120 × 20 = 2400 bytes RAM.
 *   Adjust BINS and RESOLUTION_S below to fit your target platform.
 */

#include <RollingStats.h>
#include <math.h>

// ─── RollingStats configuration ───────────────────────────────────────────────
//
// 120 bins × 60 s/bin = 7200 s = 2 hours maximum window.
// Queryable windows: any multiple of 60 s up to 7200 s.
//
// To change resolution or capacity, adjust the template parameters:
//   RollingStats<BINS, RESOLUTION_S> stats;
//
RollingStats<120, 60> stats;

// ─── Simulation parameters ────────────────────────────────────────────────────
//
// Synthetic temperature signal:
//   base      — mean temperature [°C]
//   amplitude — half-range of the sine wave [°C]
//   period    — full cycle duration [s] (86400 = 24 hours)
//   noise     — peak random noise [°C]
//
constexpr float BASE_TEMP_C  = 22.0f;
constexpr float AMPLITUDE_C  =  4.0f;
constexpr float PERIOD_S     = 86400.0f;
constexpr float NOISE_C      =  0.3f;

// ─── State ────────────────────────────────────────────────────────────────────
uint32_t lastSampleMs  = 0;
uint32_t lastSummaryMs = 0;
uint32_t sampleCount   = 0;

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Generate a synthetic temperature reading.
// Returns a sine wave with small pseudo-random noise.
static float syntheticTemperature(uint32_t timeMs) {
    float t       = timeMs / 1000.0f;
    float sine    = sinf(2.0f * (float)M_PI * t / PERIOD_S);
    // Simple LCG noise — deterministic but looks random enough for demo
    uint32_t lcg  = timeMs * 1664525UL + 1013904223UL;
    float noise   = ((int32_t)lcg / (float)INT32_MAX) * NOISE_C;
    return BASE_TEMP_C + AMPLITUDE_C * sine + noise;
}

// Print statistics for a given window if enough data is available.
static void printWindow(uint32_t windowSec, const char* label) {
    // hasValidWindow(windowSec, 0.8f): require at least 80% of bins to have
    // valid data — avoids misleadingly narrow ranges during startup.
    if (!stats.hasValidWindow(windowSec, 0.8f)) {
        Serial.printf("  %s — collecting  (%u s / %u s)\n",
                      label,
                      stats.validSeconds(windowSec),
                      windowSec);
        return;
    }
    Serial.printf(
        "  %s — avg: %5.2f °C  min: %5.2f  max: %5.2f  σ: %.2f\n",
        label,
        stats.average(windowSec),
        stats.minimum(windowSec),
        stats.maximum(windowSec),
        stats.stdDev(windowSec)
    );
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("=== TemperatureAverage — RollingStats demo ===");
    Serial.printf("Capacity: %u s (~%.1f hours)  |  Resolution: 60 s/bin\n",
                  stats.maxWindowSeconds(),
                  stats.maxWindowSeconds() / 3600.0f);
    Serial.println();
    Serial.println(" time(s)  temp(°C)   5-min avg    30-min avg   1-hr avg");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    // ── Every second: sample and display ──────────────────────────────────
    if (now - lastSampleMs >= 1000) {
        lastSampleMs = now;
        sampleCount++;

        float temp = syntheticTemperature(now);
        stats.addSample(temp, now);

        // Compact one-liner: current reading + short windows if available
        float avg5  = stats.hasValidWindow( 300, 0.5f) ? stats.average( 300) : NAN;
        float avg30 = stats.hasValidWindow(1800, 0.5f) ? stats.average(1800) : NAN;
        float avg60 = stats.hasValidWindow(3600, 0.5f) ? stats.average(3600) : NAN;

        Serial.printf("%7.1f  %7.2f °C",
                      now / 1000.0f, temp);

        if (!isnan(avg5))  Serial.printf("   %7.2f °C", avg5);
        else               Serial.printf("   %11s", "---");

        if (!isnan(avg30)) Serial.printf("   %7.2f °C", avg30);
        else               Serial.printf("   %11s", "---");

        if (!isnan(avg60)) Serial.printf("   %7.2f °C", avg60);
        else               Serial.printf("   %11s", "---");

        Serial.println();
    }

    // ── Every 5 minutes: full statistics summary ───────────────────────────
    if (lastSummaryMs > 0 && now - lastSummaryMs >= 300000UL) {
        lastSummaryMs = now;

        Serial.println("─── Statistics summary ─────────────────────────");
        Serial.printf("    samples: %u  |  valid data: %u s\n",
                      sampleCount, stats.validSeconds());

        printWindow( 300, " 5 min ");
        printWindow(1800, "30 min ");
        printWindow(3600, " 1 hr  ");
        printWindow(7200, " 2 hr  ");
        Serial.println("────────────────────────────────────────────────");
    }

    if (lastSummaryMs == 0) lastSummaryMs = now;
}
