# RunningStatistics

A general-purpose time-series statistics library for Arduino and embedded systems (ESP32, ESP8266, and any platform with a C++ compiler).

The library provides two complementary classes:

|Class|Purpose|
|---|---|
|`RollingStats`|Sliding window statistics — last N minutes/hours of data|
|`CumulativeStats`|Lifetime statistics — all data since startup, never discarded|

Both classes are **hardware-independent**: no Arduino dependency, no dynamic memory allocation, no global state. The caller provides the timestamp.

---

## Features

### RollingStats

- Compile-time sized circular buffer — no heap allocation
- Bin-based accumulation (samples averaged within each time bin)
- Sliding window average, minimum, maximum, standard deviation
- NaN for missing data — gaps do not distort statistics
- Configurable fill-ratio guard (`hasValidWindow()`)
- Query windows must be multiples of the bin resolution

### CumulativeStats

- Lifetime average, min, max, standard deviation (never discards data)
- Welford's online algorithm — numerically stable for long-running measurements
- Dose accumulation: integrates rate × time for physical quantity totals (e.g. µSv)
- double precision internally — no float accumulation errors over days/weeks
- millis() overflow-safe timestamp arithmetic

### ExponentialAverage

- Computes a single EMA over a stream of scalar samples.

---

## Requirements

No external dependencies. Only `<stdint.h>` and `<math.h>` are required —
both are part of the C99 standard library, available on every platform.

|Platform|Status|Notes|
|---|---|---|
|ESP8266 (NodeMCU, Wemos D1)|✓ Tested||
|ESP32|✓ Tested||
|AVR (ATmega328P, ATmega2560)|✓ Should work|Watch RAM — see below|
|SAMD, STM32, RP2040|✓ Should work|No platform-specific code|
|Desktop C++ (unit tests)|✓ Tested|Timestamp from `std::chrono`|

### AVR memory considerations

On AVR boards (Uno, Nano, Mega), RAM is the limiting factor. Each bin in
`RollingStats` occupies 4 bytes (one `float`), plus a small fixed overhead.

```txt
RollingStats<BINS, RESOLUTION_S>  →  BINS × 4 + ~20 bytes RAM
```

Practical sizing for AVR (2 KB RAM total on ATmega328P):

```cpp
RollingStats<32,  60>   //  32 × 4 =  128 bytes → 32 min history at 1 min res
RollingStats<64,  60>   //  64 × 4 =  256 bytes → 64 min history at 1 min res
RollingStats<128, 60>   // 128 × 4 =  512 bytes → 2 hr  history — borderline
```

`CumulativeStats` is always ~48 bytes regardless of run duration.

`double` arithmetic on AVR is emulated in software and significantly slower
than on ESP8266/ESP32. For AVR, the `float`-based public API is fine; the
`double` internal accumulators in `CumulativeStats` will work correctly but
take more cycles per `addSample()` call.

> **Note:** `GeigerMeasurement` (the companion GM tube library) is
> ESP8266/ESP32-specific due to its ISR and platform abstractions.
> `RunningStats` is deliberately kept platform-independent so it can be
> used with any sensor on any hardware — not just Geiger counters on ESP.

---

## Installation

### Arduino IDE

1. Download this repository as a ZIP file
2. In Arduino IDE: **Sketch → Include Library → Add .ZIP Library**

### PlatformIO

```ini
lib_deps =
    https://github.com/soosp/RunningStats
```

### Manual

Copy `src/*.h` into your sketch folder.

---

## Quick Start

### RollingStats

```cpp
#include "RollingStats.h"

// 128 bins × 60 s = 7680 s ≈ 2 hours of history
RollingStats<128, 60> stats;

// In loop() — call once per second (or whenever you have a new reading):
stats.addSample(value, millis());

// Query the last 30 minutes (must be a multiple of 60s and ≤ 7680s):
if (stats.hasValidWindow(1800, 0.8f)) {
    Serial.printf("30 min avg: %.2f  σ: %.2f\n",
                  stats.average(1800),
                  stats.stdDev(1800));
}
```

### CumulativeStats

```cpp
#include "CumulativeStats.h"

CumulativeStats lifetime;

// In loop():
lifetime.addSample(cpm, uSvH, millis());

// Query at any time:
Serial.printf("Avg: %.2f CPM  Dose: %.4f µSv  Uptime: %us\n",
              lifetime.averageCpm(),
              lifetime.totalDoseUSv(),
              lifetime.elapsedSeconds());
```

### Together with GeigerMeasurement

```cpp
#include "GeigerMeasurement.h"
#include "RollingStats.h"
#include "CumulativeStats.h"

GeigerMeasurement geiger(TUBE_SBM20, SOURCE_BACKGROUND);
RollingStats<128, 60> stats;   // sliding window: last 2 hours
CumulativeStats lifetime;       // lifetime average and dose

void IRAM_ATTR geigerISR() { geiger.onPulse(); }

void setup() {
    Serial.begin(115200);
    pinMode(D7, INPUT);
    attachInterrupt(digitalPinToInterrupt(D7), geigerISR, FALLING);
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();

    GeigerReading r = geiger.getReading();
    if (!r.valid) return;

    stats.addSample(r.cpm, r.timestampMs);
    lifetime.addSample(r.cpm, r.uSvH, r.timestampMs);

    if (stats.hasValidWindow(3600))
        Serial.printf("1h avg: %.1f CPM  Total dose: %.4f µSv\n",
                      stats.average(3600),
                      lifetime.totalDoseUSv());
}
```

---

## RollingStats API

### Template parameters

```cpp
RollingStats<BINS, RESOLUTION_S>
```

|Parameter|Type|Description|
|---|---|---|
|`BINS`|`uint32_t`|Number of bins — **must be a power of 2**|
|`RESOLUTION_S`|`uint32_t`|Bin width in seconds|

Maximum window: `BINS × RESOLUTION_S` seconds.

### Sizing guide

```cpp
RollingStats<128,  60>  // 128 × 60s  = 7680s ≈ 2.1 hr  —  512 bytes RAM
RollingStats<1440, 60>  // 1440 × 60s = 86400s = 24 hr  — 5760 bytes RAM
RollingStats<256,  10>  // 256 × 10s  = 2560s  ≈ 43 min —  1024 bytes RAM
```

### Methods

|Method|Description|
|---|---|
|`addSample(value, timeMs)`|Add a sample. Samples within the same bin are averaged.|
|`average(windowSec)`|Mean over the last `windowSec` seconds. NaN if no data.|
|`minimum(windowSec)`|Minimum over window. NaN if no data.|
|`maximum(windowSec)`|Maximum over window. NaN if no data.|
|`stdDev(windowSec)`|Population standard deviation. NaN if < 2 bins.|
|`hasValidWindow(windowSec, minFill)`|True if window has at least `minFill` fraction of valid bins.|
|`hasWindow(windowSec)`|True if window is fully filled (no gaps).|
|`validSeconds(windowSec)`|Seconds of valid data within window.|
|`filledBins()`|Total number of bins written since startup.|
|`availableSeconds()`|Total seconds of history available.|
|`maxWindowSeconds()`|Maximum queryable window (`BINS × RESOLUTION_S`).|
|`reset()`|Clear all data.|

> **Note:** `windowSec` must be a non-zero multiple of `RESOLUTION_S` and ≤ `maxWindowSeconds()`. Pass `0` to query the full available history.

---

## CumulativeStats API

### Methods

|Method|Description|
|---|---|
|`addSample(cpm, uSvH, timeMs)`|Add a sample. Updates all accumulators.|
|`averageCpm()`|Lifetime average CPM. NaN if no samples.|
|`sigmaCpm()`|Population standard deviation. NaN if < 2 samples.|
|`minCpm()`|Minimum CPM observed. NaN if no samples.|
|`maxCpm()`|Maximum CPM observed. NaN if no samples.|
|`totalDoseUSv()`|Accumulated dose in µSv.|
|`totalDoseMSv()`|Accumulated dose in mSv.|
|`elapsedSeconds()`|Seconds since first sample.|
|`sampleCount()`|Total number of samples added.|
|`hasData(minSamples)`|True if at least `minSamples` have been added.|
|`reset()`|Clear all accumulators.|

### Dose integration

Dose is integrated using rectangular (Euler forward) integration:

```txt
dDose [µSv] = uSvH × Δt [h]
```

Gaps between `addSample()` calls are **not** integrated — no data means no dose. This is the conservative and physically correct behaviour.

## ExponentialAverage API

### Methods

|Method|Description|
|---|---|
|`addSample(float sample)`|Adds a new sample and update the EMA.|
|`value()`|Returns the current EMA value, or NaN if no valid sample has been added yet.|
|`isValid()`|Returns true if at least one valid (non-NaN) sample has been added.|
|`alpha()`|Returns the smoothing factor alpha.|
|`setAlpha(float alpha)`|Changes the smoothing factor and reset the EMA.|
|`reset()`|Resets the EMA to the uninitialised state.|

---

## Memory usage

|Class|RAM|Notes|
|---|---|---|
|`RollingStats<128, 60>`|~520 bytes|128 × float bins + overhead|
|`RollingStats<1440, 60>`|~5780 bytes|24-hour history at 1 min resolution|
|`CumulativeStats`|~48 bytes|Fixed, independent of run duration|

---

## Example Sketches

|Sketch|Description|
|---|---|
|`TemperatureAverage.ino`|Basic RollingStats usage with a temperature sensor|
|`CumulativeStatsTest.ino`|Validates CumulativeStats output against known reference values|
|`ExponentialAverageDemo.ino`|Demonstrates ExponentialAverage with a synthetic sensor signal|

---

## Thread safety

`RollingStats` and `CumulativeStats` are **not thread-safe**. They contain no
internal locking mechanism, by design:

- Both classes perform multi-step floating-point calculations in `addSample()`
  that are not atomic — concurrent access from multiple contexts will corrupt
  internal state.
- Keeping the classes lock-free makes them hardware-independent and avoids
  pulling in platform-specific RTOS APIs.

### Safe usage patterns

**Single-task (typical):** all calls to `addSample()` and query functions
happen in the Arduino `loop()` or a single FreeRTOS task. No locking needed.

```cpp
// loop() — safe: single execution context
GeigerReading r = geiger.getReading();
if (r.valid) {
    stats.addSample(r.cpm, r.timestampMs);      // only called here
    lifetime.addSample(r.cpm, r.uSvH, r.timestampMs);
}
```

**Multi-task (ESP32):** if `addSample()` and query functions are called from
different FreeRTOS tasks, wrap all accesses in a mutex:

```cpp
SemaphoreHandle_t statsMutex = xSemaphoreCreateMutex();

// Task A — writer:
if (xSemaphoreTake(statsMutex, portMAX_DELAY)) {
    stats.addSample(r.cpm, r.timestampMs);
    xSemaphoreGive(statsMutex);
}

// Task B — reader:
if (xSemaphoreTake(statsMutex, portMAX_DELAY)) {
    float avg = stats.average(1800);
    xSemaphoreGive(statsMutex);
}
```

**Never call `addSample()` from an ISR.** Both classes perform multi-step
floating-point calculations that are far too slow for interrupt context.
Use `GeigerMeasurement::onPulse()` from the ISR instead — it is explicitly
designed for ISR use and contains its own critical section.

---

## Development note

This library was designed and developed by Péter Soós in collaboration
with Claude (Anthropic AI assistant). The iterative design process —
including algorithm selection, architecture decisions, code review,
and documentation — was conducted through an extended conversation with Claude.

---

## License

MIT License — see [LICENSE](LICENSE) for details.
