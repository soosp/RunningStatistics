# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

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

[unreleased]: https://github.com/soosp/RunningStatistics/compare/1.1.0...HEAD
[1.1.0]: https://github.com/soosp/RunningStatistics/compare/1.0.1...1.1.0
[1.0.1]: https://github.com/soosp/RunningStatistics/compare/1.0.0...1.0.1
[1.0.0]: https://github.com/soosp/RunningStatistics/releases/tag/1.0.0
