/**
 * @file CumulativeStats.h
 * @brief Cumulative (lifetime) statistics and dose accumulation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2025 Péter Soós — https://github.com/soosp
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *
 * -----------------------------------------------------------------------------
 * DESIGN PRINCIPLES
 * -----------------------------------------------------------------------------
 *
 * COMPLEMENT TO RollingStats:
 *   RollingStats provides sliding-window statistics (last N minutes/hours).
 *   CumulativeStats provides lifetime statistics: average, min, max, σ, and
 *   accumulated dose — from the first addSample() call until now, with no
 *   data ever discarded.
 *
 * HARDWARE-INDEPENDENT:
 *   No dependency on Arduino, ESP-IDF, or any hardware API. The caller
 *   provides CPM, µSv/h, and a millisecond timestamp — which can come from
 *   millis(), FreeRTOS ticks, or any monotonic source.
 *
 * NO DYNAMIC MEMORY:
 *   All state fits in a small fixed struct (~40 bytes). No heap allocation.
 *
 * DOSE ACCUMULATION:
 *   Each call to addSample() integrates the dose rate over the interval since
 *   the previous call:
 *     dDose [µSv] = uSvH × Δt [h]
 *   This is a simple rectangular (Euler forward) integration. For typical
 *   1-second update intervals and slowly varying background radiation, the
 *   integration error is negligible.
 *
 *   Dose is accumulated in double precision internally to avoid float
 *   accumulation errors over long measurement periods (days/weeks). The
 *   public accessors return float for display convenience.
 *
 * WELFORD'S ONLINE ALGORITHM:
 *   Standard deviation is computed using Welford's numerically stable single-
 *   pass algorithm. This avoids the catastrophic cancellation that can occur
 *   with the naive two-pass (sum of squares) approach when values are large
 *   and variance is small — which is exactly the case for slow-varying
 *   background radiation measurements.
 *
 * OVERFLOW PROTECTION:
 *   Sample count (_n) is a uint32_t, giving ~4 billion samples before wrap.
 *   At 1 sample/second this is ~136 years. Dose accumulation uses double,
 *   which handles decades of background radiation without precision loss.
 *   Elapsed time uses uint32_t milliseconds (wraps at ~49.7 days); the
 *   per-interval Δt computation is overflow-safe via uint32_t subtraction.
 *
 * -----------------------------------------------------------------------------
 * TYPICAL USE
 * -----------------------------------------------------------------------------
 * @code
 *   #include "CumulativeStats.h"
 *   #include "GeigerMeasurement.h"
 *
 *   CumulativeStats lifetime;
 *
 *   // In loop() — call once per second with each valid reading:
 *   GeigerReading r = geiger.getReading();
 *   if (r.valid) {
 *       lifetime.addSample(r.cpm, r.uSvH, r.timestampMs);
 *   }
 *
 *   // Query at any time:
 *   Serial.printf("Avg: %.2f CPM  Dose: %.4f µSv  Uptime: %us\n",
 *       lifetime.averageCpm(),
 *       lifetime.totalDoseUSv(),
 *       lifetime.elapsedSeconds());
 * @endcode
 *
 * -----------------------------------------------------------------------------
 * TUBE COMPARISON USE
 * -----------------------------------------------------------------------------
 * Run identical firmware on multiple devices (different GM tubes), collect
 * the averageCpm() after ≥ 2 hours, then compare ratios:
 *
 *   J305_avg / SBM20_avg  → expected ≈ 1.274  (Rad Lab: 135.2 / 106.1)
 *   M4011_avg / SBM20_avg → expected ≈ 1.021  (Rad Lab: 108.3 / 106.1)
 *
 * Unlike RollingStats, CumulativeStats never discards old data, so even a
 * 24-hour run produces a single stable average without window-boundary effects.
 */

#pragma once
#include <stdint.h>
#include <math.h>   // sqrtf, NAN

// =============================================================================
// CumulativeStats CLASS
// =============================================================================

/**
 * @brief Lifetime statistics and dose accumulator.
 *
 * Tracks average CPM, min/max CPM, standard deviation, elapsed time, and
 * accumulated radiation dose from the first sample onwards — no sliding
 * window, no data ever lost.
 *
 * Memory: ~48 bytes (fixed, no heap allocation).
 */
class CumulativeStats {

public:

    // =========================================================================
    // CONSTRUCTOR
    // =========================================================================

    /**
     * @brief Initialise in the empty state (no samples yet).
     *
     * All accumulators are zero. The first addSample() call initialises
     * the elapsed-time reference and min/max tracking.
     */
    CumulativeStats()
        : _n(0)
        , _meanCpm(0.0)
        , _M2(0.0)
        , _minCpm(NAN)
        , _maxCpm(NAN)
        , _totalDoseUSv(0.0)
        , _firstTs(0)
        , _lastTs(0)
        , _hasFirst(false)
    {}

    // =========================================================================
    // DATA INPUT
    // =========================================================================

    /**
     * @brief Add a new measurement sample.
     *
     * Updates the cumulative average, standard deviation (Welford), min/max,
     * and integrates the dose rate over the interval since the previous call.
     *
     * TIMING:
     *   Pass the timestamp recorded at measurement time, not at call time.
     *   For GeigerMeasurement, use reading.timestampMs:
     *   @code
     *     lifetime.addSample(r.cpm, r.uSvH, r.timestampMs);   // correct
     *     lifetime.addSample(r.cpm, r.uSvH, millis());         // may skew dose
     *   @endcode
     *
     * DOSE INTEGRATION:
     *   Dose is integrated only from the second sample onwards (Δt requires
     *   two timestamps). The first call establishes t₀ only.
     *
     *   If addSample() is not called for an extended period (e.g. device
     *   sleeping, tube fault), the dose for the gap is NOT integrated —
     *   only the interval between consecutive addSample() calls contributes.
     *   This is the conservative and correct behaviour: no data → no dose.
     *
     * @param cpm       Count rate in counts per minute
     * @param uSvH      Dose rate in µSv/h
     * @param timeMs    Measurement timestamp in milliseconds (e.g. millis())
     */
    void addSample(float cpm, float uSvH, uint32_t timeMs) {

        // ── Dose integration ──────────────────────────────────────────────
        // Integrate uSvH × Δt [h] using the interval since the last sample.
        // uint32_t subtraction is overflow-safe across millis() wrap-around.
        if (_hasFirst) {
            uint32_t dtMs    = timeMs - _lastTs;       // overflow-safe
            double   dtHours = dtMs / 3600000.0;
            _totalDoseUSv   += (double)uSvH * dtHours;
        } else {
            _firstTs  = timeMs;
            _hasFirst = true;
        }
        _lastTs = timeMs;

        // ── Welford's online mean and variance ────────────────────────────
        // Numerically stable single-pass algorithm:
        //   δ  = x - mean_prev
        //   mean += δ / n
        //   δ2 = x - mean_new
        //   M2 += δ * δ2        (M2 = Σ(xᵢ - mean)²)
        //   σ  = sqrt(M2 / n)   (population std dev)
        _n++;
        double delta  = (double)cpm - _meanCpm;
        _meanCpm     += delta / _n;
        double delta2 = (double)cpm - _meanCpm;
        _M2          += delta * delta2;

        // ── Min / max ─────────────────────────────────────────────────────
        if (_n == 1 || cpm < _minCpm) _minCpm = cpm;
        if (_n == 1 || cpm > _maxCpm) _maxCpm = cpm;
    }

    // =========================================================================
    // QUERIES — RATE STATISTICS
    // =========================================================================

    /**
     * @brief Cumulative average CPM since the first sample.
     * @return Average CPM, or NaN if no samples yet.
     */
    float averageCpm() const {
        return (_n > 0) ? (float)_meanCpm : NAN;
    }

    /**
     * @brief Population standard deviation of CPM since the first sample.
     *
     * Uses the accumulated M2 value from Welford's algorithm.
     * Returns NaN for fewer than 2 samples (σ undefined for a single point).
     *
     * @return Standard deviation in CPM, or NaN if < 2 samples.
     */
    float sigmaCpm() const {
        return (_n > 1) ? sqrtf((float)(_M2 / _n)) : NAN;
    }

    /**
     * @brief Minimum CPM observed since the first sample.
     * @return Minimum CPM, or NaN if no samples yet.
     */
    float minCpm() const { return _minCpm; }

    /**
     * @brief Maximum CPM observed since the first sample.
     * @return Maximum CPM, or NaN if no samples yet.
     */
    float maxCpm() const { return _maxCpm; }

    /**
     * @brief Number of samples added so far.
     * @return Sample count (0 if empty).
     */
    uint32_t sampleCount() const { return _n; }

    // =========================================================================
    // QUERIES — DOSE
    // =========================================================================

    /**
     * @brief Total accumulated dose in microsieverts [µSv].
     *
     * Integrated from the first pair of consecutive addSample() calls.
     * Gaps between calls are excluded (conservative: no data → no dose).
     *
     * @return Accumulated dose in µSv (0.0 if fewer than 2 samples).
     */
    float totalDoseUSv() const { return (float)_totalDoseUSv; }

    /**
     * @brief Total accumulated dose in millisieverts [mSv].
     * @return Accumulated dose in mSv (= totalDoseUSv() / 1000).
     */
    float totalDoseMSv() const { return (float)(_totalDoseUSv / 1000.0); }

    // =========================================================================
    // QUERIES — TIME
    // =========================================================================

    /**
     * @brief Elapsed time in seconds since the first sample.
     *
     * Uses the difference between the first and last timestamp passed to
     * addSample(). Overflow-safe via uint32_t subtraction.
     *
     * @return Elapsed seconds, or 0 if fewer than 2 samples.
     */
    uint32_t elapsedSeconds() const {
        return (_hasFirst && _n > 1)
            ? (_lastTs - _firstTs) / 1000UL
            : 0;
    }

    /**
     * @brief Check whether at least minSamples samples are available.
     *
     * Useful as a startup guard before trusting averageCpm() or sigmaCpm():
     * @code
     *   if (lifetime.hasData(60)) {   // at least 60 seconds of 1 Hz data
     *       Serial.printf("Avg: %.2f CPM\n", lifetime.averageCpm());
     *   }
     * @endcode
     *
     * @param minSamples  Minimum number of samples required (default: 1)
     * @return            true if sampleCount() >= minSamples
     */
    bool hasData(uint32_t minSamples = 1) const { return _n >= minSamples; }

    // =========================================================================
    // RESET
    // =========================================================================

    /**
     * @brief Reset all accumulators to the initial (empty) state.
     *
     * After reset(), the next addSample() starts a fresh measurement session.
     * Use this if the tube is replaced, the measurement conditions change
     * significantly, or you want to start a new dose period.
     */
    void reset() {
        _n             = 0;
        _meanCpm       = 0.0;
        _M2            = 0.0;
        _minCpm        = NAN;
        _maxCpm        = NAN;
        _totalDoseUSv  = 0.0;
        _firstTs       = 0;
        _lastTs        = 0;
        _hasFirst      = false;
    }

private:

    // =========================================================================
    // MEMBER VARIABLES
    // =========================================================================

    // --- Sample count ---
    uint32_t _n;             ///< Number of samples added

    // --- Welford's online algorithm state (double for numerical stability) ---
    double _meanCpm;         ///< Running mean CPM
    double _M2;              ///< Running sum of squared deviations (for σ)

    // --- Min / max ---
    float _minCpm;           ///< Minimum CPM observed (NaN until first sample)
    float _maxCpm;           ///< Maximum CPM observed (NaN until first sample)

    // --- Dose accumulator (double: avoids float precision loss over days) ---
    double _totalDoseUSv;    ///< Accumulated dose [µSv]

    // --- Timestamps ---
    uint32_t _firstTs;       ///< Timestamp of first addSample() call [ms]
    uint32_t _lastTs;        ///< Timestamp of most recent addSample() call [ms]
    bool     _hasFirst;      ///< False until the first addSample() call
};
