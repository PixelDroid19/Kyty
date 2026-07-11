[![Build status](https://ci.appveyor.com/api/projects/status/0du32fg9flol63to?svg=true)](https://ci.appveyor.com/project/InoriRus/kyty) [![CI](https://github.com/InoriRus/Kyty/actions/workflows/ci.yml/badge.svg)](https://github.com/InoriRus/Kyty/actions/workflows/ci.yml)

# Kyty

Kyty is an experimental, open-source research project that studies the
execution and rendering requirements of PlayStation 4 and PlayStation 5
software. The project is intended for lawful development, testing,
interoperability research, education, and preservation work.

Kyty is early-stage software. Compatibility is incomplete and may vary by
title, host operating system, graphics driver, and hardware. A successful boot,
window, or frame does not mean that a title is fully supported.

## Legal and responsible use

Kyty does not endorse copyright infringement, piracy, unauthorized access, or
the circumvention of access controls. You are responsible for complying with
the laws and regulations that apply where you live and for respecting the
terms under which you obtained any software or hardware.

This repository contains emulator source code only. It does not provide,
request, or link to:

- commercial games or other copyrighted game assets;
- console firmware, BIOS files, encryption keys, certificates, or title keys;
- unauthorized dumps, decrypted executables, extracted shaders, or textures;
- instructions for bypassing platform security, DRM, licensing, or account
  controls;
- hosted download services or third-party repositories distributing protected
  material.

Use only software, system files, and data that you created yourself or are
authorized to use, and only in ways permitted by applicable law. Do not upload
copyrighted game files, firmware, keys, save data, crash dumps, screenshots, or
other proprietary material to this repository, its issue tracker, pull
requests, CI artifacts, or public discussion. When reporting a problem, use a
minimal sanitized reproducer and describe the relevant behavior without
attaching protected content.

Nothing in this README is legal advice. Laws differ by jurisdiction and can
change. If your use case involves commercial distribution, interoperability
exceptions, reverse engineering, preservation, or security research, obtain
advice from a qualified lawyer in the relevant jurisdiction.

## Project scope

The project focuses on emulator engineering, including:

- guest ABI and operating-system compatibility layers;
- CPU execution and memory-management research;
- graphics command processing, shader translation, resource layouts, and
  presentation;
- portable Vulkan integration and host capability detection;
- deterministic tests and sanitized hardware-behavior fixtures.

Kyty is not affiliated with, sponsored by, or endorsed by Sony Interactive
Entertainment or any game publisher. PlayStation is a trademark of its owner.
All other names and marks belong to their respective owners and are used only
for identification.

## Current status

Kyty is experimental. Some homebrew and research workloads may run, while
others may fail to boot, render incorrectly, lose audio, freeze, crash, or
perform poorly. Known areas of incomplete support include, among others,
audio, video playback, networking, multi-user behavior, system settings, and
parts of PS4/PS5 graphics and guest services.

Compatibility claims must be backed by a reproducible test and must identify
the host, build, graphics device, workload category, and limitations. Do not
interpret screenshots or a process that remains alive as a compatibility
guarantee.

## Building

The build uses CMake and Ninja. The exact toolchain and dependencies depend on
the host and are maintained by the build files and CI configuration. Vulkan is
required for the graphics path. Start by installing a current C++17-capable
compiler, CMake, Ninja, and a Vulkan SDK appropriate for your platform, then
configure a separate build directory:

```sh
cmake -S . -B _build -G Ninja
ninja -C _build
```

Host-specific setup and generated files should remain outside tracked source
files. Check the CI workflows and CMake configuration for the currently tested
platform combinations; do not assume that a successful build on one GPU or OS
proves portability.

## Testing and reporting issues

Run focused tests while developing. The repository's `AGENTS.md` is the
canonical engineering guide and documents strict-runtime verification.

When reporting an issue, include:

- Kyty commit or release identifier;
- host operating system, CPU, GPU, driver, and Vulkan implementation;
- exact build and test commands;
- the first reproducible error and relevant sanitized logs;
- whether the behavior occurs with a minimal lawful test workload.

Do not include game dumps, firmware, keys, private paths, title identifiers,
or other proprietary data. Maintainers may close or remove reports that request
copyrighted files, circumvention help, or unauthorized distribution.

## Contributions

By contributing, you agree that your contribution is submitted under the
repository's license and that you have the right to submit it. Contributions
must be original or compatible with the applicable license, must not include
proprietary game material, and must not add piracy or circumvention features.

Please read [AGENTS.md](AGENTS.md) before changing code. Contributions should
include focused tests, preserve supported behavior, document evidence for guest
ABI and GPU semantics, and keep platform-specific code at platform boundaries.
Do not copy code whose license is incompatible with this repository.

## Security and responsible disclosure

Do not publish private dumps, keys, exploit chains, or instructions that enable
unauthorized access. For a security issue in the project itself, contact the
maintainers privately before public disclosure when possible. Security reports
should include a minimal reproduction that contains no protected third-party
content.

## License

Kyty source code is distributed under the MIT License; see [LICENSE](LICENSE).
Third-party components may have separate licenses, which remain applicable to
those components. Nothing in this project grants rights to software or data
owned by third parties.

## Contact

For project questions, use GitHub Issues or Discussions. Please keep support
requests focused on emulator behavior and do not ask other users to share
copyrighted game files, firmware, keys, or private dumps.
