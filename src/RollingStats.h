/**
 * @file RollingStats.h
 * @brief General-purpose sliding window statistics with compile-time sizing.
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
 * HARDWARE-INDEPENDENT:
 *   This library has no dependency on Arduino, ESP-IDF, or any hardware API.
 *   The caller provides the current time as a uint32_t millisecond value, which
 *   can come from millis(), a FreeRTOS tick, or any other monotonic source.
 *
 * COMPILE-TIME SIZING (no dynamic memory allocation):
 *   The buffer size is a template parameter resolved at compile time. This means
 *   no heap allocation (no new / malloc), which is important on microcontrollers
 *   where heap fragmentation can cause hard-to-debug failures. The trade-off is
 *   that the size cannot be changed at runtime.
 *
 *   The template parameter BINS must be a power of 2. This is enforced by
 *   static_assert. The reason: the circular buffer uses bitwise AND for index
 *   wrapping (_head & (BINS-1)), which is equivalent to modulo but executes
 *   as a single CPU instruction.
 *
 * BIN-BASED ACCUMULATION:
 *   Rather than storing individual samples, the library divides time into fixed-
 *   width bins of RESOLUTION_S seconds. Samples arriving within the same bin are
 *   averaged together before being stored. This dramatically reduces memory use:
 *   a 24-hour history at 1-minute resolution needs only 1440 floats (5.6 KB)
 *   instead of potentially millions of individual samples.
 *
 * NaN FOR MISSING DATA:
 *   Bins with no samples (e.g. during gaps in measurement, or at startup) are
 *   stored as NaN (Not a Number). Query functions skip NaN bins, so gaps do not
 *   distort the statistics. The portable isNaN() helper is used instead of the
 *   standard isnan() macro, which is unreliable on older ESP8266 toolchains.
 *
 * WINDOW ALIGNMENT:
 *   Query windows (windowSec) must be exact multiples of RESOLUTION_S. If not,
 *   the query returns NaN. This avoids ambiguity about partial bin inclusion and
 *   makes the statistical interpretation clear. Use hasWindow() to check before
 *   querying.
 *
 * TIMESTAMP ACCURACY:
 *   Always pass the timestamp recorded at the time of measurement, not at the
 *   time of the addSample() call. For GeigerMeasurement, use reading.timestampMs
 *   rather than millis() — the gap between getReading() and addSample() can
 *   introduce timing skew that shifts bin boundaries.
 *
 * -----------------------------------------------------------------------------
 * SIZING GUIDE
 * -----------------------------------------------------------------------------
 *
 * Choose BINS and RESOLUTION_S to cover your longest desired window:
 *
 *   MAX_WINDOW_S = BINS × RESOLUTION_S
 *
 * Examples:
 *   RollingStats<128, 60>   // 128 × 60s  = 7680s ≈ 2 hours,   512 bytes RAM
 *   RollingStats<1440, 60>  // 1440 × 60s = 86400s = 24 hours, 5760 bytes RAM
 *   RollingStats<256, 10>   // 256 × 10s  = 2560s ≈ 43 minutes, 1024 bytes RAM
 *
 * Queryable windows must be multiples of RESOLUTION_S and ≤ MAX_WINDOW_S:
 *   RollingStats<128, 60>: can query 60, 120, 180 ... 3600, 3660 ... 7680 seconds
 *
 * -----------------------------------------------------------------------------
 * USAGE EXAMPLE
 * -----------------------------------------------------------------------------
 * @code
 *   #include "RollingStats.h"
 *   #include "GeigerMeasurement.h"
 *
 *   // 128 bins × 60s = up to 2 hours of history
 *   RollingStats<128, 60> stats;
 *
 *   // In loop() — typically called once per second:
 *   GeigerReading r = geiger.getReading();
 *   if (r.valid) {
 *       // Use reading.timestampMs, not millis(), to avoid timing skew
 *       stats.addSample(r.cpm, r.timestampMs);
 *   }
 *
 *   // Query — windowSec must be a multiple of RESOLUTION_S (60):
 *   if (stats.hasWindow(3600)) {
 *       float avg1h = stats.average(3600);   // 1-hour average
 *       float min1h = stats.minimum(3600);
 *       float max1h = stats.maximum(3600);
 *       float std1h = stats.stdDev(3600);
 *   }
 * @endcode
 */

#pragma once
#include <stdint.h>
#include <math.h>   // sqrtf, NAN

// =============================================================================
// PORTABLE NaN CHECK
// =============================================================================

/**
 * @brief Portable NaN (Not a Number) check.
 *
 * WHY NOT USE isnan()?
 *   The standard isnan() macro is defined in <math.h> and works correctly on
 *   most platforms. However, on older ESP8266 toolchains (xtensa-lx106-elf-gcc),
 *   isnan() has been observed to return incorrect results for certain NaN
 *   bit patterns due to the softfloat library implementation.
 *
 * THE RELIABLE ALTERNATIVE:
 *   NaN is the only floating-point value that is not equal to itself. This is
 *   guaranteed by the IEEE 754 standard and is independent of the toolchain's
 *   math library:
 *     if (x != x)  →  x is NaN
 *
 * This function wraps the idiom in a named helper for readability.
 */
inline bool isNaN(float x) { return x != x; }

// =============================================================================
// ROLLINGSTATISTICS CLASS
// =============================================================================

/**
 * @brief Sliding window statistics over time-binned samples.
 *
 * @tparam BINS          Number of time bins. MUST be a power of 2 (enforced by
 *                       static_assert). Determines memory usage: BINS × 4 bytes.
 * @tparam RESOLUTION_S  Duration of each bin in seconds. Determines the minimum
 *                       time granularity and, together with BINS, the maximum
 *                       window: MAX_WINDOW_S = BINS × RESOLUTION_S.
 */
template<uint32_t BINS, uint32_t RESOLUTION_S>
class RollingStats {

    // =========================================================================
    // COMPILE-TIME VALIDATION
    // =========================================================================

    /**
     * These checks run at compile time and produce clear error messages if the
     * template parameters are invalid. They cost nothing at runtime.
     *
     * BINS must be a power of 2 because the ring buffer uses:
     *   _head = (_head + 1) & (BINS - 1)
     * This is only equivalent to modulo when BINS is a power of 2.
     * For BINS = 200: (200 - 1) = 199 = 0b11000111, which does NOT mask
     * correctly — indices would go out of range.
     */
    static_assert(BINS > 0,
                  "BINS must be greater than 0");
    static_assert(RESOLUTION_S > 0,
                  "RESOLUTION_S must be greater than 0");
    static_assert((BINS & (BINS - 1)) == 0,
                  "BINS must be a power of 2 for correct ring buffer indexing "
                  "(e.g. 64, 128, 256, 512, 1024)");

public:

    // =========================================================================
    // COMPILE-TIME CONSTANT
    // =========================================================================

    /// Maximum queryable window in seconds = BINS × RESOLUTION_S.
    /// Queries with windowSec > MAX_WINDOW_S are capped at this value.
    static constexpr uint32_t MAX_WINDOW_S = BINS * RESOLUTION_S;

    // =========================================================================
    // CONSTRUCTOR
    // =========================================================================

    /**
     * @brief Initialise with all bins set to NaN (no data).
     *
     * NaN indicates "no sample in this bin" — query functions skip NaN bins.
     * The accumulator is also reset to its uninitialised state.
     */
    RollingStats()
        : _head(0)
        , _count(0)
        , _accumSum(0.0f)
        , _accumN(0)
        , _accumStartMs(0)
        , _hasAccumStart(false)
    {
        for (uint32_t i = 0; i < BINS; i++) _bins[i] = NAN;
    }

    // =========================================================================
    // DATA INPUT
    // =========================================================================

    /**
     * @brief Add a new measurement sample.
     *
     * Samples are accumulated within the current bin. When RESOLUTION_S seconds
     * have elapsed since the bin started, the bin is closed (its average is
     * stored in the ring buffer) and a new bin begins.
     *
     * TIMING ACCURACY:
     *   Pass the timestamp recorded at the moment of measurement, not at the
     *   moment of this call. For GeigerMeasurement use reading.timestampMs:
     *   @code
     *     stats.addSample(r.cpm, r.timestampMs);   // correct
     *     stats.addSample(r.cpm, millis());         // may have skew
     *   @endcode
     *   For other sensors, record the time before the (potentially slow) read:
     *   @code
     *     uint32_t t = millis();
     *     float temp = sensor.readTemperature();  // may take several ms
     *     stats.addSample(temp, t);
     *   @endcode
     *
     * OVERFLOW-SAFE TIMING:
     *   timeMs is a uint32_t, which wraps at ~49.7 days (millis() period).
     *   The elapsed time computation uses uint32_t subtraction:
     *     elapsedMs = timeMs - _accumStartMs
     *   This is correct even when timeMs has wrapped past zero, because
     *   unsigned subtraction wraps identically. No special handling needed.
     *
     * GAP HANDLING:
     *   If addSample() is not called for more than one bin duration (e.g. device
     *   was sleeping), the while loop closes multiple bins in sequence. Bins with
     *   no samples are stored as NaN, which query functions skip automatically.
     *
     * @param value   Measurement value (e.g. CPM, temperature in °C)
     * @param timeMs  Timestamp of the measurement in milliseconds
     */
    void addSample(float value, uint32_t timeMs) {
        // Initialise the accumulator start time on the very first call
        if (!_hasAccumStart) {
            _accumStartMs  = timeMs;
            _hasAccumStart = true;
        }

        // Time elapsed since the current bin started (overflow-safe)
        uint32_t elapsedMs = timeMs - _accumStartMs;

        // Close completed bins and open new ones.
        // The while loop handles the case where multiple bins have elapsed
        // (e.g. after a sleep or measurement gap). Intermediate bins get NaN.
        while (elapsedMs >= RESOLUTION_S * 1000UL) {
            _commitBin();
            // Advance the bin start time by exactly one bin duration.
            // We ADD rather than setting to timeMs to avoid drift: small
            // timing errors in addSample() calls would otherwise accumulate
            // and shift bin boundaries unpredictably over time.
            _accumStartMs += RESOLUTION_S * 1000UL;
            elapsedMs     -= RESOLUTION_S * 1000UL;
        }

        // Accumulate this sample into the current (open) bin
        _accumSum += value;
        _accumN++;
    }

    // =========================================================================
    // QUERIES
    // =========================================================================

    /**
     * @brief Compute the average over the most recent windowSec seconds.
     *
     * Scans the ring buffer backwards (most recent first), skipping NaN bins.
     * The average is computed only from bins with valid (non-NaN) data.
     *
     * WINDOW RULES:
     *   - windowSec = 0: use the full MAX_WINDOW_S window.
     *   - windowSec must be an exact multiple of RESOLUTION_S. If not, returns
     *     NaN. This ensures each query covers whole bins only, which makes
     *     the statistical meaning unambiguous.
     *   - If windowSec > MAX_WINDOW_S, it is capped silently.
     *   - If fewer bins are available than requested, returns the average of
     *     whatever is available (no NaN for "not enough data" — use hasWindow()
     *     before querying if you need a guarantee).
     *
     * @param windowSec  Window duration in seconds. Must be a multiple of
     *                   RESOLUTION_S, or 0 for the full window.
     * @return           Average value, or NaN if no valid bins in window or
     *                   windowSec is not a multiple of RESOLUTION_S.
     */
    float average(uint32_t windowSec = 0) const {
        uint32_t bins = _binsForWindow(windowSec);
        if (bins == 0) return NAN;

        float    sum = 0.0f;
        uint32_t n   = 0;
        for (uint32_t i = 0; i < bins; i++) {
            float v = _bins[(_head - 1 - i) & (BINS - 1)];
            if (!isNaN(v)) { sum += v; n++; }
        }
        return (n > 0) ? sum / n : NAN;
    }

    /**
     * @brief Find the minimum value over the most recent windowSec seconds.
     *
     * Skips NaN bins. Returns NaN if no valid data is available.
     * See average() for window rules.
     *
     * @param windowSec  Window in seconds (multiple of RESOLUTION_S, or 0)
     * @return           Minimum value, or NaN if no valid data.
     */
    float minimum(uint32_t windowSec = 0) const {
        uint32_t bins = _binsForWindow(windowSec);
        if (bins == 0) return NAN;

        float minVal = NAN;
        for (uint32_t i = 0; i < bins; i++) {
            float v = _bins[(_head - 1 - i) & (BINS - 1)];
            // isNaN(minVal) on first valid bin initialises it directly
            if (!isNaN(v) && (isNaN(minVal) || v < minVal)) minVal = v;
        }
        return minVal;
    }

    /**
     * @brief Find the maximum value over the most recent windowSec seconds.
     *
     * Skips NaN bins. Returns NaN if no valid data is available.
     * See average() for window rules.
     *
     * @param windowSec  Window in seconds (multiple of RESOLUTION_S, or 0)
     * @return           Maximum value, or NaN if no valid data.
     */
    float maximum(uint32_t windowSec = 0) const {
        uint32_t bins = _binsForWindow(windowSec);
        if (bins == 0) return NAN;

        float maxVal = NAN;
        for (uint32_t i = 0; i < bins; i++) {
            float v = _bins[(_head - 1 - i) & (BINS - 1)];
            // isNaN(maxVal) on first valid bin initialises it directly
            if (!isNaN(v) && (isNaN(maxVal) || v > maxVal)) maxVal = v;
        }
        return maxVal;
    }

    /**
     * @brief Compute the population standard deviation over windowSec seconds.
     *
     * Uses the two-pass algorithm: first compute the mean (via average()),
     * then sum squared deviations from the mean. NaN bins are excluded from
     * both passes. Returns NaN if fewer than 2 valid bins are available
     * (standard deviation is undefined for a single point).
     *
     * NOTE: This computes population std dev (divides by n), not sample std dev
     * (divides by n-1). For bin counts typical in this library (≥ 10) the
     * difference is negligible.
     *
     * @param windowSec  Window in seconds (multiple of RESOLUTION_S, or 0)
     * @return           Standard deviation, or NaN if < 2 valid bins.
     */
    float stdDev(uint32_t windowSec = 0) const {
        uint32_t bins = _binsForWindow(windowSec);
        if (bins < 2) return NAN;

        float avg = average(windowSec);
        if (isNaN(avg)) return NAN;

        float    sumSq = 0.0f;
        uint32_t n     = 0;
        for (uint32_t i = 0; i < bins; i++) {
            float v = _bins[(_head - 1 - i) & (BINS - 1)];
            if (!isNaN(v)) {
                float diff = v - avg;
                sumSq += diff * diff;
                n++;
            }
        }
        return (n > 1) ? sqrtf(sumSq / n) : NAN;
    }

    // =========================================================================
    // UTILITY
    // =========================================================================

    /// Number of fully committed bins currently in the buffer (0 … BINS).
    uint32_t filledBins() const { return _count; }

    /// Total seconds of committed data available (= filledBins() × RESOLUTION_S).
    /// Note: includes NaN bins (gaps). Use validSeconds() for non-NaN data only.
    uint32_t availableSeconds() const { return _count * RESOLUTION_S; }

    /**
     * @brief Count non-NaN (valid) bins within a window.
     *
     * Scans the window and counts bins that actually contain data.
     * NaN bins (gaps in measurement) are not counted.
     *
     * @param windowSec  Window in seconds (multiple of RESOLUTION_S, or 0 = max)
     * @return           Number of valid bins, or 0 if windowSec is not aligned
     */
    uint32_t validBins(uint32_t windowSec = 0) const {
        uint32_t bins = _binsForWindow(windowSec);
        if (bins == 0) return 0;
        uint32_t n = 0;
        for (uint32_t i = 0; i < bins; i++) {
            float v = _bins[(_head - 1 - i) & (BINS - 1)];
            if (!isNaN(v)) n++;
        }
        return n;
    }

    /**
     * @brief Total seconds of valid (non-NaN) data within a window.
     *
     * Unlike availableSeconds(), this counts only bins that actually contain
     * measurements — NaN bins (gaps) are excluded.
     *
     * @param windowSec  Window in seconds (multiple of RESOLUTION_S, or 0 = max)
     * @return           Valid seconds = validBins() × RESOLUTION_S
     */
    uint32_t validSeconds(uint32_t windowSec = 0) const {
        return validBins(windowSec) * RESOLUTION_S;
    }

    /**
     * @brief Check whether enough *valid* (non-NaN) data is available.
     *
     * This is the preferred guard for query functions during startup or after
     * measurement gaps. Unlike hasWindow(), it checks the fill ratio of actual
     * data rather than just committed bin count.
     *
     * WHY THIS MATTERS:
     *   During startup, bins accumulate gradually. If you call average(1800)
     *   after only 2 minutes, the window contains 28 NaN bins and 2 valid bins.
     *   The average of those 2 bins may be misleadingly low or high. This
     *   function lets you require a minimum fill ratio before trusting the result.
     *
     * Example — require at least 80% valid data:
     * @code
     *   if (stats.hasValidWindow(1800, 0.8f)) {
     *       float avg = stats.average(1800);  // at least 24/30 bins are valid
     *   } else {
     *       // Show progress: how many valid minutes so far
     *       Serial.printf("30min avg: %us / 1800s valid data\n",
     *                     stats.validSeconds(1800));
     *   }
     * @endcode
     *
     * @param windowSec    Window in seconds (multiple of RESOLUTION_S)
     * @param minFillRatio Minimum fraction of valid bins required (0.0–1.0,
     *                     default: 1.0 = all bins must be valid)
     * @return             true if enough valid data is available
     */
    bool hasValidWindow(uint32_t windowSec, float minFillRatio = 1.0f) const {
        if (windowSec % RESOLUTION_S != 0) return false;
        uint32_t requested = windowSec / RESOLUTION_S;
        if (requested == 0 || requested > _count) return false;
        uint32_t valid = validBins(windowSec);
        return valid >= (uint32_t)(requested * minFillRatio + 0.5f);
    }

    /**
     * @brief Check whether enough data is available for a given window query.
     *
     * Returns false if:
     *   - windowSec is not a multiple of RESOLUTION_S (query would return NaN)
     *   - Fewer than windowSec / RESOLUTION_S bins have been committed
     *
     * NOTE: This checks committed bin count, not valid (non-NaN) bin count.
     * During startup some committed bins may be NaN. Use hasValidWindow() for
     * a stricter check that ensures actual data fill ratio.
     *
     * @param windowSec  Desired window in seconds
     * @return           true if the required number of bins have been committed
     */
    bool hasWindow(uint32_t windowSec) const {
        if (windowSec % RESOLUTION_S != 0) return false;
        return availableSeconds() >= windowSec;
    }

    /// Maximum queryable window in seconds (= BINS × RESOLUTION_S).
    uint32_t maxWindowSeconds() const { return MAX_WINDOW_S; }

    /**
     * @brief Reset all state to initial (empty) condition.
     *
     * Clears the ring buffer (all bins set to NaN), resets the accumulator,
     * and resets the bin counter. After reset(), addSample() starts fresh
     * as if the object was just constructed.
     */
    void reset() {
        _head          = 0;
        _count         = 0;
        _accumSum      = 0.0f;
        _accumN        = 0;
        _hasAccumStart = false;
        for (uint32_t i = 0; i < BINS; i++) _bins[i] = NAN;
    }

private:

    // =========================================================================
    // MEMBER VARIABLES
    // =========================================================================

    // --- Ring buffer (committed bins) ---
    float    _bins[BINS];  ///< Circular buffer of bin averages. NaN = no data.
    uint32_t _head;        ///< Index of the next bin to write (most recent bin is at head-1)
    uint32_t _count;       ///< Number of committed bins (0 … BINS)

    // --- Accumulator (current open bin) ---
    float    _accumSum;      ///< Sum of samples in the current bin
    uint32_t _accumN;        ///< Number of samples in the current bin
    uint32_t _accumStartMs;  ///< Timestamp of when the current bin started [ms]
    bool     _hasAccumStart; ///< False until the first addSample() call

    // =========================================================================
    // PRIVATE HELPERS
    // =========================================================================

    /**
     * @brief Close the current bin and write its average to the ring buffer.
     *
     * If the accumulator has samples (_accumN > 0), computes the mean and
     * stores it. If the bin received no samples (gap in measurement), stores
     * NaN so query functions can skip it.
     *
     * After committing, the accumulator is reset to zero — ready for the
     * next bin. Note: _accumStartMs is updated by the caller (addSample),
     * not here, to maintain precise bin boundary alignment.
     *
     * RING BUFFER WRAP:
     *   _head advances with bitwise AND: (_head + 1) & (BINS - 1).
     *   When the buffer is full (_count == BINS), the oldest bin is silently
     *   overwritten. The buffer always holds the most recent BINS bins.
     */
    void _commitBin() {
        // Compute bin average, or NaN if no samples arrived
        float binValue = (_accumN > 0) ? (_accumSum / _accumN) : NAN;

        // Write to ring buffer and advance head
        _bins[_head] = binValue;
        _head = (_head + 1) & (BINS - 1);  // wrap using bitmask (power-of-2 only)
        if (_count < BINS) _count++;

        // Reset accumulator for next bin
        _accumSum = 0.0f;
        _accumN   = 0;
    }

    /**
     * @brief Convert a window duration in seconds to a bin count.
     *
     * Returns 0 (which causes callers to return NaN) in these cases:
     *   - windowSec is not a multiple of RESOLUTION_S
     *   - No bins are available (buffer empty)
     *
     * Returns the minimum of (requested bins, available bins) — callers will
     * compute statistics over however much data is actually present.
     *
     * WINDOW ALIGNMENT REQUIREMENT:
     *   windowSec must be an exact multiple of RESOLUTION_S. For example, with
     *   RESOLUTION_S = 60, requesting windowSec = 90 returns 0 (NaN), because
     *   90s spans 1.5 bins — it's ambiguous whether to include 1 or 2 bins.
     *   Request either 60 or 120 instead.
     *
     * @param windowSec  Desired window (0 = MAX_WINDOW_S)
     * @return           Number of bins to scan, or 0 if invalid/empty
     */
    uint32_t _binsForWindow(uint32_t windowSec) const {
        if (windowSec == 0) windowSec = MAX_WINDOW_S;
        if (windowSec % RESOLUTION_S != 0) return 0;  // not aligned → NaN
        uint32_t requested = windowSec / RESOLUTION_S;
        return (requested < _count) ? requested : _count;
    }
};
