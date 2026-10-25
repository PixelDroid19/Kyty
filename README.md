# Kyty

[![Build and Release](https://github.com/PixelDroid19/Kyty/actions/workflows/build-release.yml/badge.svg)](https://github.com/PixelDroid19/Kyty/actions/workflows/build-release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-lightgrey.svg)](#supported-platforms)

**Project website:** [pixeldroid19.github.io/kyty-web](https://pixeldroid19.github.io/kyty-web/)

Kyty is an experimental, open-source emulator research project focused on the
execution and rendering requirements of PlayStation 4 and PlayStation 5
software. It is designed for lawful interoperability research, homebrew
development, education, testing, and preservation.

> [!IMPORTANT]
> Kyty is under active development. Compatibility, performance, graphics,
> audio, and stability are incomplete. A title reaching a window or rendering
> frames does not mean it is fully supported.

## Project goals

Kyty explores and implements:

- guest ABI and operating-system compatibility layers;
- executable loading, linking, memory management, and CPU execution;
- graphics command processing, shader translation, and Vulkan presentation;
- audio, input, filesystem, and other guest-facing services;
- deterministic diagnostics and regression tests;
- portable builds for Windows and Linux.

The project favors general, evidence-based implementations over title-specific
hacks. Compatibility claims should always include the tested commit, host
configuration, graphics hardware, driver, workload, and known limitations.

## Legal and responsible use

This repository contains emulator source code only. It does not provide or
request commercial games, firmware, BIOS files, encryption keys, certificates,
decrypted executables, or other copyrighted console material.

Use Kyty only with software and system data that you created or are legally
authorized to use. Do not upload protected content, private dumps, keys, game
assets, or confidential information to issues, pull requests, CI artifacts, or
public discussions.

Kyty does not endorse piracy, unauthorized access, DRM circumvention, or
copyright infringement. Nothing in this document is legal advice.

Kyty is not affiliated with, sponsored by, or endorsed by Sony Interactive
Entertainment or any game publisher. PlayStation and all other trademarks
belong to their respective owners.

## Supported platforms

Release builds are produced for the following targets:

| Platform | Architecture | Build environment | Graphics path |
| --- | --- | --- | --- |
| Windows | x86-64 | Windows Server 2022 / MinGW-w64 | Vulkan 1.4 |
| Linux | x86-64 | Ubuntu 24.04 | Vulkan 1.4 |

These are build targets, not guarantees that every workload behaves identically
on every operating system, GPU, or driver. The macOS (MoltenVK) code paths are
kept in the source tree but are not built or tested by any workflow.

On Windows, MinGW's unwinder cannot see guest code, so titles that propagate
C++ exceptions through guest frames (common in IL2CPP titles) may fail there.

The host layer (window, keyboard, controllers, audio output) uses SDL 3, which
is vendored in `source/3rdparty/sdl3`.

## Downloads

Tagged releases are published on the
[GitHub Releases](https://github.com/PixelDroid19/Kyty/releases) page. Every
release is built by GitHub Actions for Windows and Linux. Versions with a
prerelease label (for example `v0.0.2-beta.1`) are published as prereleases.
The Linux archive contains the emulator and the desktop launcher with its Qt 6
runtime; extract it and run `launcher`. To build the same archive locally, see
[the Linux release package](docs/devtools/reproducible-contract-environment.md#linux-release-package).

Artifacts from untagged commits are development snapshots. They may be useful
for testing but can be unstable and are not releases.

Versions follow [Semantic Versioning](https://semver.org/):

- `major` releases contain incompatible or foundational changes;
- `minor` releases add backward-compatible functionality;
- `patch` releases contain compatible bug fixes.

Every version is cut from `main`. Maintainers create versions through the
**Create Version** GitHub Actions workflow, choosing the bump and, optionally, a
prerelease label such as `beta.1`. Creating a version tag starts the Windows and
Linux release build and publishes the resulting archives.

## Building from source

### Requirements

- Git;
- CMake 3.24 or newer;
- Ninja;
- a C++17 compiler (the build sets `CMAKE_CXX_STANDARD 17`);
- Vulkan 1.4 headers and loader, plus SPIRV-Headers and SPIRV-Tools with
  Vulkan 1.4 support (including the optimizer CMake package);
- Qt 6 (Quick, Widgets, LinguistTools) only when building the launcher;
- the platform display and audio development packages that SDL 3 needs.

Set `VULKAN_SDK` to a Vulkan 1.4 SDK installation or install equivalent host
development packages. Configuration rejects older headers or shader tools;
runtime also requires a Vulkan 1.4 loader and physical device. Shared
SPIRV-Tools libraries must be available to the executable when using a shared
SDK build. The bundled older compiler sources are not used.

### Configure and build

From the repository root:

```sh
cmake -S source -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DKYTY_PROJECT_NAME=Emulator \
  -DKYTY_WARNINGS_ARE_ERRORS=OFF \
  -DKYTY_BUILD_LAUNCHER=OFF \
  -DKYTY_BUILD_DEVTOOLS=OFF \
  -DKYTY_BUILD_UNIT_TESTS=OFF \
  -DKYTY_GENERATE_MAP_CSV=OFF

cmake --build build --target fc_script --parallel 4
```

The resulting executable is `build/fc_script.exe` on Windows and
`build/fc_script` on Linux. Set `-DKYTY_BUILD_LAUNCHER=ON` to also build the
desktop launcher, which lists installed workloads, starts them and offers
themes and personalization.

## Running a lawful test workload

Kyty uses a Lua entry point. The repository includes `scripts/run_guest.lua`
for authorized local workloads:

```sh
build/fc_script scripts/run_guest.lua "/path/to/authorized/workload"
```

On Windows PowerShell:

```powershell
build\fc_script.exe scripts\run_guest.lua "C:\path\to\authorized\workload"
```

Only use files you are legally permitted to access. Paths, dumps, and logs may
contain private information; sanitize them before sharing a report.

### Keyboard and controllers

A connected game controller is used directly. Without one, the keyboard drives
a virtual pad:

| Key | Pad input |
| --- | --- |
| `Z`, `Space`, `Enter` | Cross |
| `X` | Circle |
| `C` | Square |
| `V` | Triangle |
| Arrow keys | D-pad |
| `Q` / `E` | L1 / R1 |
| `1` / `3` | L2 / R2 (full trigger travel) |
| `Tab`, `Backspace` | Options |

`F11` or `Alt+Enter` toggles fullscreen, `F9` pauses presentation and audio,
`F2` toggles text entry for a guest keyboard, and `Escape` exits. The full
host contract is in [docs/HOST_RUNTIME.md](docs/HOST_RUNTIME.md).

### Useful runtime options

- `KYTY_PRESENT_FPS_LIMIT=60` caps host presents per second.
- `KYTY_RENDER_RESOLUTION_MODE` and the related options select the internal
  render resolution; see [docs/render-resolution.md](docs/render-resolution.md).
  On integrated GPUs, `KYTY_RENDER_RESOLUTION_SCOPE=RenderTargets` (the
  launcher's Quality, Balanced and Performance presets) also scales offscreen
  targets and can raise the frame rate of titles bound by pixel work.
- `KYTY_PAD_RECORD=<file>` records the pad input of a manual session, and
  `KYTY_PAD_SCRIPT=<file>` replays it; see
  [docs/input-replay.md](docs/input-replay.md).

## Reporting issues

Before opening an issue, reproduce the problem on the latest `main` build and
search for an existing report. Include:

- the exact Kyty commit or release;
- operating system, CPU, GPU, graphics driver, and Vulkan implementation;
- build and launch commands;
- the first reproducible error and a short sanitized log;
- expected behavior, actual behavior, and reproduction frequency.

Do not attach games, firmware, keys, decrypted assets, proprietary symbols, or
other protected material. A report that depends on private content should use a
minimal sanitized reproducer whenever possible.

## Contributing

Contributions are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) before
opening an issue or pull request. Changes should be focused, testable,
maintainable, and based on lawful clean-room research or other compatible
sources.

Commit messages and squash-merge titles follow
[Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/).
Repository-wide engineering and automation rules are documented in
[AGENTS.md](AGENTS.md).

Every branch (`feature/*`, `fix/*`, `perf/*`, `refactor/*`, documentation)
starts from `main` and returns to `main` through a pull request.

Feature and fix commits can be accumulated without creating a multiplatform
build for every push or pull request. Maintainers can request a manual build
when validation is needed. Official Windows and Linux archives are created only
when a semantic version is released.

## Special thanks

The following independent open-source projects provide useful public
architectural and portability references:

- [RPCSX](https://github.com/RPCSX/rpcsx)
- [Ryubing/Ryujinx](https://git.ryujinx.app/projects/Ryubing/ryujinx)

These projects are independent from Kyty. Their inclusion does not imply an
affiliation, endorsement, partnership, or exchange of source code.

## License

Kyty is distributed under the [MIT License](LICENSE). Third-party components
retain their respective licenses and notices. The Kyty license does not grant
rights to games, firmware, system software, or other third-party content.
