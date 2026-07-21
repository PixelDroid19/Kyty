# Changelog

All notable changes to this Kyty fork are documented in this file. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and releases
use semantic version tags where practical.

## [Unreleased]

### Added

- Native Windows, Linux and macOS artifact generation through GitHub Actions.
- Tag-driven GitHub releases containing one archive per supported platform.
- Optional CMake switches for launcher, devtools, unit tests and map generation.
- Windows development code-signing support for local smoke testing.

### Changed

- Vulkan queue selection can share the graphics family when exclusive resources
  do not perform queue-family ownership transfers.
- Vulkan format validation now checks the optimal tiling mode used by render and
  video output images.
- macOS uses the bundled MoltenVK runtime and the command-line launcher.

### Fixed

- Windows guest ABI handling for FreeBSD-compatible `setjmp`/`longjmp` state.
- Windows callback ABI bridging for guest `qsort` comparators.
- MinGW Vulkan import-library and pthread macro compatibility.
- Windows virtual-memory fallbacks and invalid default pthread stack protection.
- GCC aggregate initialization portability in GPU-memory and diagnostics code.

[Unreleased]: https://github.com/PixelDroid19/Kyty/compare/main...HEAD
