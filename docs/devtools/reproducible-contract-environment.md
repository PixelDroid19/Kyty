# Reproducible contract environment

`flake.nix` declares the Linux build and contract-test toolchain from one exact
nixpkgs commit. The PR contract workflow uses that shell for both the build and
the CPU tests; it also checks both values of `KYTY_BUILD_DEVTOOLS`.

```bash
nix develop --no-write-lock-file
nix develop --no-write-lock-file --command cmake --version
```

The configured contract build requires CMake 3.24 or newer, C++17, Python 3,
Vulkan 1.4 development files, and SPIRV-Tools with Vulkan 1.4 support and the
optimizer target. Those floors are enforced during CMake configuration in
`source/3rdparty/vulkan/CMakeLists.txt`. The flake supplies CMake, Ninja, GCC,
Python, Git, Vulkan headers and loader, and SPIRV-Tools. It also supplies
`shaderc` for `glslc`; `spirv-tools` provides `spirv-val` and the
`SPIRV-Tools-opt` CMake package used by the build. `pkg-config`, X11 and XCB headers,
ALSA, and PulseAudio development files support the bundled SDL host backends;
the SDL Vulkan header includes `xcb/xcb.h` when its X11 backend is enabled. Both
shells disable the compiler wrapper's `format` hardening: the project sets its
own warning flags, and the wrapper's `-Werror=format-security` rejects the
vendored profiler's `-Wno-format`.
On Linux, CMake probes `libavformat`, `libavcodec`, `libavutil`, `libswscale`,
and `libswresample` through `pkg-config`; FFmpeg enables the optional media
decoder path used for Opus and video decoding; ATRAC9 decodes through the bundled
LibAtrac9. At this pin, the top-level `ffmpeg`
attribute resolves to `ffmpeg_9`, version 9.0.1, as declared by the pinned
[FFmpeg package definition](https://github.com/NixOS/nixpkgs/blob/151fa4e8ddfdd8dd25d945ad94ed54a13de9f6e4/pkgs/development/libraries/ffmpeg/default.nix).
This attribute and version were verified by reading that source.

At the pinned nixpkgs revision, the package definitions declare Vulkan Headers
1.4.357.0, the LunarG Vulkan loader 1.4.357.0, and SPIRV-Tools 1.4.357.0. The
shaderc definition declares 2026.1. These source declarations are in the pinned
[Vulkan Headers](https://github.com/NixOS/nixpkgs/blob/151fa4e8ddfdd8dd25d945ad94ed54a13de9f6e4/pkgs/by-name/vu/vulkan-headers/package.nix),
[Vulkan loader](https://github.com/NixOS/nixpkgs/blob/151fa4e8ddfdd8dd25d945ad94ed54a13de9f6e4/pkgs/by-name/vu/vulkan-loader/package.nix),
[SPIRV-Tools](https://github.com/NixOS/nixpkgs/blob/151fa4e8ddfdd8dd25d945ad94ed54a13de9f6e4/pkgs/by-name/sp/spirv-tools/package.nix), and
[shaderc](https://github.com/NixOS/nixpkgs/blob/151fa4e8ddfdd8dd25d945ad94ed54a13de9f6e4/pkgs/by-name/sh/shaderc/package.nix)
definitions. A local evaluation of the shell confirmed the loader version: CMake
found Vulkan 1.4.357 from the store's `vulkan-loader-1.4.357.0`. The existing release workflow requests LunarG SDK
1.4.357.0 as well. CMake's Vulkan 1.4 and SPIR-V 1.4 capability checks remain
the build-time contract.

## PR validation

`.github/workflows/contract-validation.yml` runs on pull requests to `main`.
It configures a Release build with the Qt launcher disabled, builds the default
targets, and runs `KytyUnitTest`, the architecture-checker fixtures, and the
graphics-table and checked-in shader CTest checks. It then runs the strict
repository boundary checker. The matrix covers `KYTY_BUILD_DEVTOOLS=ON` and
`OFF`; the process supervisor tests are included only in the ON configuration.
The ON job also compiles the GPU integration executables without requiring a
device. The launcher is outside this job's coverage because the workflow does
not install Qt.

GPU integration tests run only when the runner exposes a Vulkan render node at
`/dev/dri/renderD*`. Otherwise the workflow records an explicit skip in the
job summary. The CMake tests that can report missing Vulkan capabilities also
use CTest's skip return code where configured. This is integration-test
coverage only; it does not validate a guest title or prove compatibility.

## Pin and local validation status

The flake input is pinned to the exact nixpkgs revision
`151fa4e8ddfdd8dd25d945ad94ed54a13de9f6e4`; no branch or tag is used. The
repository currently has no `flake.lock`, and the PR workflow uses
`--no-write-lock-file`, so it does not add one as a side effect. This keeps the
source revision fixed while leaving lock-file review as a separate change. The
package versions above were read from the package definitions at that exact
revision.

The default shell was evaluated locally with Nix, and the workflow's
`KYTY_BUILD_DEVTOOLS=ON` steps were reproduced inside it: configure, build, the
nine CTest checks, the strict boundary checker and the GPU integration
executables all passed. The `OFF` configuration and the `oracle` shell were
not reproduced locally. `nix develop` does not provide a Vulkan device; hardware
tests still depend on the runner's render device.

The optional `oracle` shell adds the pinned nixpkgs ROCm tools for the public
hardware oracle. Installing those tools does not provide an AMD device; the
oracle needs a compatible GPU exposed through `/dev/kfd`.
