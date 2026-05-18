/**
 * ExponentialAverageDemo.ino
 *
 * Demonstrates ExponentialAverage with a synthetic sensor signal:
 * a slow baseline drift plus occasional short spikes.
 *
 * Two EMA instances run in parallel:
 *   emaFast — short time constant, tracks the signal closely
 *   emaSlow — long time constant, estimates the slowly-changing baseline
 *
 * Their difference (trend) reveals sustained changes while ignoring
 * short-lived spikes — a simple but effective anomaly detector.
 *
 * No real sensor required — the sketch generates a synthetic signal
 * sampled once per second.
 *
 * Output (Serial, 115200 baud):
 *   time(s)  raw    fast   slow   trend  event
 *      1.0   22.1   22.1    ---    ---
 *      2.0   22.3   22.1   22.1   +0.0
 *      ...
 *     61.0   28.4   27.9   22.8   +5.1   << alert: sustained rise
 *
 * Alpha selection (1 s update interval):
 *   tau = time constant [s], alpha = 1 - exp(-dt / tau)
 *
 *   tau =  30 s → alpha ≈ 0.033  (emaFast here)
 *   tau = 300 s → alpha ≈ 0.003  (emaSlow here)
 */

#include <ExponentialAverage.h>
#include <math.h>

// ─── EMA configuration ────────────────────────────────────────────────────────
//
// UPDATE_INTERVAL_S: how often a new sample is added [s].
// TAU_FAST_S, TAU_SLOW_S: desired time constants [s].
// alpha = 1 - exp(-UPDATE_INTERVAL_S / TAU)
//
constexpr float UPDATE_INTERVAL_S = 1.0f;
constexpr float TAU_FAST_S        = 30.0f;   // fast: responds in ~30 s
constexpr float TAU_SLOW_S        = 300.0f;  // slow: responds in ~5 min

// Pre-computed alphas.
// expf() is not constexpr in most toolchains, so these are plain floats
// initialised at startup. Formula: alpha = 1 - exp(-dt / tau)
//   alpha_fast = 1 - exp(-1/30)  ≈ 0.033
//   alpha_slow = 1 - exp(-1/300) ≈ 0.003
const float ALPHA_FAST = 1.0f - expf(-UPDATE_INTERVAL_S / TAU_FAST_S);
const float ALPHA_SLOW = 1.0f - expf(-UPDATE_INTERVAL_S / TAU_SLOW_S);

ExponentialAverage emaFast(ALPHA_FAST);
ExponentialAverage emaSlow(ALPHA_SLOW);

// ─── Alert threshold ─────────────────────────────────────────────────────────
//
// Alert when the fast EMA rises more than ALERT_THRESHOLD above the slow EMA.
// In a radiation monitor this could be "2× background"; here it's an absolute
// offset for simplicity.
//
constexpr float ALERT_THRESHOLD = 3.0f;

// ─── Synthetic signal ────────────────────────────────────────────────────────
//
// Signal model (sampled at 1 Hz):
//   - Slow baseline: linear drift from 22 °C to 30 °C over 10 minutes
//   - Short spike: +8 °C for 5 seconds every 2 minutes (should NOT alert)
//   - Sustained rise: +6 °C step starting at t=5 min (SHOULD alert)
//   - Small noise: ±0.3 °C
//
static float syntheticSample(uint32_t timeMs) {
    float t = timeMs / 1000.0f;

    // Slow baseline drift: 22 °C at t=0, reaching 30 °C at t=600 s
    float baseline = 22.0f + 8.0f * min(t / 600.0f, 1.0f);

    // Short spike: 5 s long, every 120 s — should not trigger alert
    float spike = 0.0f;
    float phase = fmodf(t, 120.0f);
    if (phase < 5.0f) spike = 8.0f;

    // Sustained step: +6 °C from t=300 s onward — should trigger alert
    float step = (t >= 300.0f) ? 6.0f : 0.0f;

    // Pseudo-random noise (LCG)
    uint32_t lcg = (uint32_t)(timeMs) * 1664525UL + 1013904223UL;
    float noise  = ((int32_t)lcg / (float)0x7FFFFFFF) * 0.3f;

    return baseline + spike + step + noise;
}

// ─── State ────────────────────────────────────────────────────────────────────
uint32_t lastSampleMs = 0;
bool     alertActive  = false;

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("=== ExponentialAverageDemo ===");
    Serial.printf("alpha_fast = %.4f  (tau = %.0f s)\n", ALPHA_FAST, TAU_FAST_S);
    Serial.printf("alpha_slow = %.4f  (tau = %.0f s)\n", ALPHA_SLOW, TAU_SLOW_S);
    Serial.printf("Alert threshold: fast - slow > %.1f\n\n", ALERT_THRESHOLD);
    Serial.println(" time(s)   raw    fast    slow   trend  event");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    if (now - lastSampleMs < (uint32_t)(UPDATE_INTERVAL_S * 1000)) return;
    lastSampleMs = now;

    float sample = syntheticSample(now);
    emaFast.addSample(sample);
    emaSlow.addSample(sample);

    // emaSlow needs many samples before it's meaningful — skip early output
    // for the slow EMA, but still print the fast EMA from the first sample.
    float fast  = emaFast.value();  // always valid after first sample
    float slow  = emaSlow.isValid() ? emaSlow.value() : NAN;
    float trend = (!isnan(slow))    ? (fast - slow)   : NAN;

    // Alert: sustained rise detected when trend exceeds threshold
    bool newAlert = (!isnan(trend) && trend > ALERT_THRESHOLD);
    const char* event = "";
    if (newAlert && !alertActive) event = "<< ALERT: sustained rise";
    if (!newAlert && alertActive) event = "   (alert cleared)";
    alertActive = newAlert;

    Serial.printf("%7.1f  %6.2f  %6.2f  ",
                  now / 1000.0f, sample, fast);

    if (!isnan(slow))  Serial.printf("%6.2f  ", slow);
    else               Serial.printf("   ---  ");

    if (!isnan(trend)) Serial.printf("%+6.2f  ", trend);
    else               Serial.printf("   ---  ");

    Serial.println(event);
}
