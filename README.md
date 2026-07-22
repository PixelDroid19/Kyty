# Kyty

[![Build and Release](https://github.com/PixelDroid19/Kyty/actions/workflows/build-release.yml/badge.svg)](https://github.com/PixelDroid19/Kyty/actions/workflows/build-release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](#supported-platforms)

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
- portable builds for Windows, Linux, and macOS.

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

The continuous-integration pipeline currently builds the following targets:

| Platform | Architecture | CI environment | Graphics path |
| --- | --- | --- | --- |
| Windows | x86-64 | Windows Server 2022 / MinGW-w64 | Vulkan |
| Linux | x86-64 | Ubuntu 24.04 | Vulkan |
| macOS | x86-64 | macOS 15 Intel | MoltenVK |

These are build targets, not guarantees that every workload behaves identically
on every operating system, GPU, or driver.

## Downloads

Tagged releases are published on the
[GitHub Releases](https://github.com/PixelDroid19/Kyty/releases) page. Every
release is built by GitHub Actions for Windows, Linux, and macOS.

Artifacts from untagged commits are development snapshots. They may be useful
for testing but can be unstable and are not releases.

Versions follow [Semantic Versioning](https://semver.org/):

- `major` releases contain incompatible or foundational changes and come from
  `main`;
- `minor` releases add backward-compatible functionality and come from `main`;
- `patch` releases contain compatible bug fixes and come from `release`.

Maintainers create versions through the **Create Version** GitHub Actions
workflow. Creating a version tag starts the Windows, Linux, and macOS release
build and publishes the resulting archives.

## Building from source

### Requirements

- Git;
- CMake;
- Ninja;
- a C++17-capable compiler;
- Vulkan development files appropriate for the host platform;
- platform-specific window, input, and audio development packages.

The CI workflow is the authoritative reference for tested dependencies and
compiler configuration.

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
`build/fc_script` on Linux and macOS.

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

Development uses two long-lived branches:

- new features and other evolutionary work branch from and return to `main`;
- bug fixes branch from and return to `release`.

Feature and fix commits can be accumulated without creating a multiplatform
build for every push or pull request. Maintainers can request a manual build
when validation is needed. Official Windows, Linux, and macOS archives are
created only when a semantic version is released.

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
