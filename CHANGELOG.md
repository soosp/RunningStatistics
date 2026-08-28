# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

## [1.1.1] - 2026-08-28

### Fixed

- `RollingStats::addSample()` caps the bin-commit loop at `BINS` iterations.
  Committing more bins than the ring holds only rewrites the same slots, so the
  end state is unchanged, but an out-of-order timestamp — an uninitialised or
  stale value from the caller — underflows the unsigned elapsed-time
  subtraction into a ~49-day gap and previously spun the loop tens of thousands
  of times before arriving there. When the cap is reached the bin start time is
  moved to where the uncapped loop would have left it — subtracting whole bins
  never changes the elapsed time modulo the bin duration — so the end state is
  identical and only the iteration count differs. Without that step every later
  call would commit another `BINS` bins.

### Added

- Minor fix in `README.md`
- Add ExponetialAverage example code to the README.md

## [1.1.0] - 2026-05-31

### Added

- ExponentialAverage.h:
  - New `addSample` overload function to add a new sample using a one-shot alpha.
  - New `computeAlpha` function to compute the EMA smoothing factor.

## [1.0.1] - 2026-05-19

### Changed

- Renamed library to `RunningStatistics` to avoid conflict in Arduino library registry.

## [1.0.0] - 2026-05-19

### Added

- First public release

[unreleased]: https://github.com/soosp/RunningStatistics/compare/1.1.1...HEAD
[1.1.1]: https://github.com/soosp/RunningStatistics/compare/1.1.0...1.1.1
[1.1.0]: https://github.com/soosp/RunningStatistics/compare/1.0.1...1.1.0
[1.0.1]: https://github.com/soosp/RunningStatistics/compare/1.0.0...1.0.1
[1.0.0]: https://github.com/soosp/RunningStatistics/releases/tag/1.0.0
