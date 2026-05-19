# RunningStatistics — API Reference

---

## RollingStats\<BINS, RESOLUTION_S\>

Sliding window statistics over time-series data. Samples are accumulated into
fixed-width time bins; query functions operate over a configurable window of
recent bins.

### Template parameters

|Parameter|Type|Constraint|Description|
|---|---|---|---|
|`BINS`|`uint32_t`|Power of 2, ≥ 2|Number of bins in the circular buffer|
|`RESOLUTION_S`|`uint32_t`|≥ 1|Width of each bin in seconds|

Maximum queryable window: `BINS × RESOLUTION_S` seconds.

### Constructor

```cpp
RollingStats<128, 60> stats;
```

No arguments. All bins initialised to NaN (empty).

---

### Data input

#### `void addSample(float value, uint32_t timeMs)`

Add a new sample.

- Samples arriving within the same time bin are **averaged** into that bin.
- When the current bin ends (elapsed time ≥ `RESOLUTION_S`), it is committed
  to the circular buffer and a new bin starts.
- Bins with no samples are stored as NaN.
- Always pass the **measurement timestamp**, not the call time:

```cpp
// Correct — use the timestamp from the reading:
stats.addSample(r.cpm, r.timestampMs);

// Avoid — millis() at call time may skew bin boundaries:
stats.addSample(r.cpm, millis());
```

---

### Query — statistics

All query functions accept a `windowSec` parameter:

- Must be a **non-zero multiple of `RESOLUTION_S`** and ≤ `maxWindowSeconds()`.
- Pass `0` to query all available history (up to `maxWindowSeconds()`).
- Returns NaN if the window contains no valid bins.

#### `float average(uint32_t windowSec = 0) const`

Mean of all valid bins within the window.

#### `float minimum(uint32_t windowSec = 0) const`

Minimum value across all valid bins within the window.

#### `float maximum(uint32_t windowSec = 0) const`

Maximum value across all valid bins within the window.

#### `float stdDev(uint32_t windowSec = 0) const`

Population standard deviation across all valid bins within the window.
Returns NaN if fewer than 2 valid bins are present.

---

### Query — window validity

#### `bool hasValidWindow(uint32_t windowSec, float minFillRatio = 1.0f) const`

Returns true if the fraction of valid (non-NaN) bins within `windowSec`
is ≥ `minFillRatio`.

```cpp
// At least 80% of the last 30 minutes must have data:
if (stats.hasValidWindow(1800, 0.8f)) { ... }

// Full 1-hour window with no gaps:
if (stats.hasValidWindow(3600, 1.0f)) { ... }

// Any data at all in the last hour:
if (stats.hasValidWindow(3600, 0.0f)) { ... }
```

#### `bool hasWindow(uint32_t windowSec) const`

Shorthand for `hasValidWindow(windowSec, 1.0f)` — returns true only if
the window is fully filled with no gaps.

#### `uint32_t validSeconds(uint32_t windowSec = 0) const`

Number of seconds of valid (non-NaN) data within the window.
Useful for displaying warm-up progress:

```cpp
Serial.printf("collecting data (%us / %us valid)\n",
              stats.validSeconds(1800), 1800);
```

---

### Query — history depth

#### `uint32_t filledBins() const`

Total number of bins committed since startup (capped at `BINS`).

#### `uint32_t availableSeconds() const`

Total seconds of history available (`filledBins() × RESOLUTION_S`).

#### `uint32_t maxWindowSeconds() const`

Maximum queryable window in seconds (`BINS × RESOLUTION_S`).

---

### Reset

#### `void reset()`

Clear all data. All bins reset to NaN; the in-progress bin is discarded.

---

## CumulativeStats

Lifetime statistics accumulator. Tracks average, min, max, standard deviation,
and accumulated dose from the first `addSample()` call — no data ever discarded.

### Constructor

```cpp
CumulativeStats lifetime;
```

No arguments. All accumulators initialised to zero / NaN.

---

### Data input

#### `void addSample(float cpm, float uSvH, uint32_t timeMs)`

Add a new sample. Updates all accumulators.

|Parameter|Description|
|---|---|
|`cpm`|Count rate in counts per minute|
|`uSvH`|Dose rate in µSv/h|
|`timeMs`|Measurement timestamp in milliseconds (e.g. `millis()` or `r.timestampMs`)|

**Dose integration:** dose is integrated from the second sample onwards using
rectangular integration (`dDose = uSvH × Δt_hours`). Gaps between calls are
not integrated — only the interval between consecutive `addSample()` calls
contributes. This is the conservative and physically correct behaviour.

**Overflow safety:** `uint32_t` subtraction is used for all time intervals,
making the implementation safe across `millis()` wrap-around (~49.7 days).

---

### Query — rate statistics

#### `float averageCpm() const`

Lifetime average CPM since the first sample. Returns NaN if no samples.

#### `float sigmaCpm() const`

Population standard deviation of CPM. Uses Welford's numerically stable
online algorithm. Returns NaN if fewer than 2 samples.

#### `float minCpm() const`

Minimum CPM observed since the first sample. Returns NaN if no samples.

#### `float maxCpm() const`

Maximum CPM observed since the first sample. Returns NaN if no samples.

#### `uint32_t sampleCount() const`

Total number of samples added.

---

### Query — dose

#### `float totalDoseUSv() const`

Total accumulated dose in microsieverts [µSv].
Returns 0.0 if fewer than 2 samples (Δt requires two timestamps).

#### `float totalDoseMSv() const`

Total accumulated dose in millisieverts [mSv] (`= totalDoseUSv() / 1000`).

---

### Query — time

#### `uint32_t elapsedSeconds() const`

Elapsed time in seconds between the first and last `addSample()` call.
Returns 0 if fewer than 2 samples.

#### `bool hasData(uint32_t minSamples = 1) const`

Returns true if `sampleCount() >= minSamples`. Use as a startup guard:

```cpp
if (lifetime.hasData(60)) {   // at least 60 one-second samples
    Serial.printf("Avg: %.2f CPM\n", lifetime.averageCpm());
}
```

---

### Reset

#### `void reset()`

Clear all accumulators. The next `addSample()` starts a fresh session.
Use this after replacing the tube, changing measurement conditions, or
starting a new dose period.

---

## ExponentialAverage

Single exponential moving average (EMA) for scalar time-series data.
Useful for smoothing noisy sensor readings and detecting sustained changes
by running two instances with different time constants in parallel.

No Arduino dependency, no dynamic memory. All state fits in two floats (~8 bytes).

### Constructor

```cpp
ExponentialAverage ema(float alpha = 0.1f);
```

|Parameter|Constraint|Description|
|---|---|---|
|`alpha`|`(0, 1]`|Smoothing factor. Invalid values are clamped to `0.1`.  High = fast response, low = heavy smoothing.|

**Alpha selection:** For a desired time constant `tau` [s] at a fixed update
interval `dt` [s]:

```txt
alpha = 1 - exp(-dt / tau)
```

Examples at 1 s update interval:

|tau|alpha|Lag (approx.)|
|---|---|---|
|10 s|0.095|~10 samples|
|30 s|0.033|~30 samples|
|300 s|0.003|~300 samples|

Note: `expf()` is not `constexpr` in most toolchains — compute alpha as a
`const` (runtime initialised) rather than `constexpr`:

```cpp
const float ALPHA = 1.0f - expf(-1.0f / 30.0f);   // tau = 30 s, dt = 1 s
ExponentialAverage ema(ALPHA);
```

---

### Data input

#### `void addSample(float sample)`

Update the EMA with a new sample.

- The **first** valid call initialises the EMA directly to `sample`,
  avoiding slow convergence from zero.
- NaN samples are **silently ignored** — the EMA retains its current value.

---

### Query

#### `float value() const`

Returns the current EMA value, or NaN if no valid sample has been added yet.

Always check `isValid()` before use, or propagate NaN explicitly:

```cpp
if (ema.isValid())
    Serial.printf("EMA: %.2f\n", ema.value());
```

#### `bool isValid() const`

Returns `true` if at least one valid (non-NaN) sample has been added.

#### `float alpha() const`

Returns the current smoothing factor.

---

### Configuration

#### `void setAlpha(float alpha)`

Change the smoothing factor. The EMA is **reset** to the uninitialised state —
the next `addSample()` call will re-initialise it directly to the new sample.

Changing alpha while the EMA is running would produce history inconsistent
with the new time constant, so the reset is intentional.

Invalid values (`≤ 0`, `> 1`, NaN) are silently ignored.

#### `void reset()`

Reset to the uninitialised state. `isValid()` returns `false` until the next
`addSample()` call.

---

### Typical use — dual EMA anomaly detection

Running two instances with different time constants allows sustained changes
to be distinguished from short-lived noise:

```cpp
// tau = 30 s (fast) and tau = 300 s (slow), updated once per second
const float ALPHA_FAST = 1.0f - expf(-1.0f / 30.0f);
const float ALPHA_SLOW = 1.0f - expf(-1.0f / 300.0f);

ExponentialAverage emaFast(ALPHA_FAST);
ExponentialAverage emaSlow(ALPHA_SLOW);

// In loop():
emaFast.addSample(reading);
emaSlow.addSample(reading);

if (emaFast.isValid() && emaSlow.isValid()) {
    float trend = emaFast.value() - emaSlow.value();
    // trend ≈ 0:    slow drift — both EMAs follow together
    // trend >> 0:   sustained rise — emaFast responds, emaSlow lags
    // large spike:  emaFast briefly rises, but not enough for a large trend
}
```

See `ExponentialAverageDemo.ino` for a complete example with simulated sensor
data, spike rejection, and alert detection.

---

## Notes

### Welford's algorithm (CumulativeStats)

Standard deviation is computed using Welford's single-pass online algorithm
rather than the naive `Σx²` approach. This avoids catastrophic cancellation
when values are large and variance is small — exactly the case for slowly
varying background radiation measurements over long periods.

### double precision (CumulativeStats)

Internal accumulators (`_meanCpm`, `_M2`, `_totalDoseUSv`) use `double`
precision to prevent float accumulation errors over long measurement sessions
(days to weeks). Public accessors return `float` for display convenience.

### NaN behaviour (RollingStats)

Missing bins are stored as NaN and excluded from all query functions.
This means:

- Startup gaps do not pull the average down
- Device sleep or tube fault periods do not distort statistics
- `validSeconds()` reflects only genuinely measured time

### Window alignment (RollingStats)

Query windows must be exact multiples of `RESOLUTION_S`. If not,
`hasValidWindow()` returns false and statistical queries return NaN.
This is by design — partial bin inclusion would make the statistical
interpretation ambiguous.

### Thread safety

None of the classes in the library are thread-safe. `addSample()`
performs multi-step floating-point calculations that are not atomic.
If `addSample()` and `value()` are called from different tasks or from
an ISR and the main loop, the caller is responsible for mutual exclusion.
For multi-task use, the mutex must protect **all** calls — both `addSample()`
and query functions (`average()`, `averageCpm()`, etc.).

|Context|Safe?|Notes|
|---|---|---|
|Single Arduino `loop()`|✓|No locking needed|
|Single FreeRTOS task|✓|No locking needed|
|Two FreeRTOS tasks (ESP32)|✗|Wrap in `SemaphoreHandle_t` mutex|
|ISR (`IRAM_ATTR`)|✗|Never — use `GeigerMeasurement::onPulse()` instead in GM counters|
