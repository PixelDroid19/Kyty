# Strict Graphics Bring-up and Portable Graphics Foundation Design

**Status:** Approved in conversation on 2026-07-10

**Reference workload:** local, untracked compatibility fixture

## 1. Outcome

This project phase makes the local reference workload produce a correct,
recognizable frame through Kyty without relying on missing-symbol stubs,
permissive register skipping, guessed surface sizes, game-specific patches, or
GPU-vendor-specific behavior.

The phase also establishes the architectural rules needed to continue toward a
playable game on macOS, Linux, and Windows with AMD, Intel, NVIDIA, and Apple
GPUs. It does not claim Linux/Windows runtime support or full gameplay at the end
of this phase. Those are subsequent phases built on the contracts defined here.

## 2. Verified Baseline

The following observations were reproduced against the current worktree and a
local, untracked compatibility fixture:

1. `ninja -C _build_macos` completes successfully.
2. Both checked-in compatibility scripts point to a removed local directory, so
   the default invocation fails while opening the guest executable.
3. With the correct path and no diagnostic relaxations, the guest initializes a
   Vulkan window and stops at missing NID `hwVSPCmp5tM`. Public Gen5 export
   tables identify the NID as `sceKernelCheckedReleaseDirectMemory`.
4. With only `KYTY_STUB_MISSING=1`, the guest reaches shader creation, produces
   its first graphics DCB and requests a flip. Strict graphics parsing then
   stops at indirect context register `0x90`.
5. Register `0x90` is `PA_SC_GENERIC_SCISSOR_TL`. Other registers skipped during
   the diagnostic run are `0x91` (`PA_SC_GENERIC_SCISSOR_BR`), `0x205`
   (`PA_SU_SC_MODE_CNTL`), and `0x1e0` (`CB_BLEND0_CONTROL`). Kyty already has
   definitions and direct-packet decoders for these states, but the indirect
   dispatch table does not route them.
6. With both diagnostic flags enabled, the guest continuously submits indexed
   draws and completed flips. The visible result is not black, but it is a
   narrow, red, severely corrupted image. This proves that presentation is live
   while the produced render state and/or resource interpretation is incorrect.
7. The latest `Tile.cpp` change assumes four bytes per texel and linear layout
   when the precomputed table has no entry. It makes buffer allocation continue,
   but cannot prove the guest surface layout and is not an acceptable final
   implementation.

These observations replace the stale "black frame stopped in Tile.cpp" status
in the current `AGENT.md`.

## 3. Scope

### 3.1 Included

- One canonical `AGENTS.md` that defines engineering, portability, evidence,
  licensing, and verification rules.
- A location-independent local compatibility-run configuration.
- Strict implementation of guest functions that are actually called on the
  path to the first rendered frame.
- Shared PM4/AGC state decoders used by direct and indirect packets.
- A typed, validated surface-layout calculation for observed texture and render
  target descriptors.
- Structured frame diagnostics that identify unsupported state without changing
  guest-visible behavior.
- Vulkan capability discovery and explicit required/optional feature policy.
- Automated unit and integration checks that do not require proprietary game
  files, plus a local opt-in strict compatibility acceptance run.
- Visual validation of a recognizable, correctly proportioned frame.

### 3.2 Excluded from this phase

- Full-game compatibility.
- Performance optimization beyond removing pathological diagnostic overhead.
- Shipping Linux or Windows binaries.
- Declaring AMD, Intel, or NVIDIA compatibility without running on that hardware.
- A new GUI, updater, compatibility database, save-state system, or network
  emulation.
- Copying source from projects whose licenses are incompatible with Kyty's MIT
  license.

## 4. Architectural Decisions

### 4.1 Repair the current vertical slice

Kyty's existing loader, HLE, PM4 processor, shader translator, Vulkan renderer,
and VideoOut path already execute far enough to produce draws and flips. This
phase repairs that path incrementally instead of replacing the graphics system
or transplanting another emulator's implementation.

Each change must move the strict execution frontier or correct an evidenced
rendering divergence. Unrelated rewrites are out of scope.

### 4.2 One decoder per guest state

Direct `SET_CONTEXT_REG` packets and Gen5 indirect context-register packets are
different encodings of the same guest GPU state. They must not maintain separate
decoding logic.

The design introduces focused state-decoder functions with this conceptual
interface:

```cpp
void DecodeGenericScissorTl(HW::Context& context, uint32_t value);
void DecodeGenericScissorBr(HW::Context& context, uint32_t value);
void DecodeModeControl(HW::Context& context, uint32_t value);
void DecodeBlendControl(HW::Context& context, uint32_t slot, uint32_t value);
```

Direct packet handlers call these functions after validating packet shape.
Indirect table entries call the same functions for each register/value pair.
Tests prove that both packet forms produce identical `HW::Context` state.

Unknown state is an explicit unsupported error containing packet kind, register,
value, submit identifier, and command offset. Production execution never skips
it silently.

### 4.3 Guest API functions have real contracts

Missing NIDs are implemented only after their names, signatures, return values,
and observable effects are supported by evidence. The first confirmed functions
are:

- `hwVSPCmp5tM`: `sceKernelCheckedReleaseDirectMemory`.
- `UZbQjYAwwXM`: `sceAgcCbSetShRegistersDirect`.
- `k3GhuSNmBLU`: `sceAgcCbDispatch`.

Existing Kyty primitives may be reused only when their validation and error
semantics match. A NID must not be mapped to a convenient function solely because
the happy-path effect looks similar.

### 4.4 Surface layout is data, not a fallback

Texture and render-target sizing becomes a typed result instead of a table hit
followed by a guessed byte count:

```cpp
struct SurfaceLevelLayout
{
    uint64_t offset;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t row_pitch_bytes;
};

struct SurfaceLayout
{
    uint64_t total_size;
    uint64_t alignment;
    Core::Vector<SurfaceLevelLayout> levels;
};

Result<SurfaceLayout, SurfaceLayoutError> ComputeSurfaceLayout(
    const SurfaceDescriptor& descriptor);
```

`SurfaceDescriptor` contains the decoded format, dimensions, pitch, mip count,
array/depth information, tile mode, sample count, and compression metadata
needed by the layout algorithm. Format properties come from one centralized
table. Block-compressed formats use block dimensions rather than bytes per
pixel.

The calculator either returns a fully validated layout or a descriptive error.
It never assumes RGBA8, linear tiling, a default alignment, or a neighboring
buffer boundary. The current generic linear fallback remains only until the
first correct calculator path is integrated, then is removed in the same
reviewable task.

CPU upload/detiling and Vulkan image allocation consume the same layout result.
They cannot independently infer sizes.

### 4.5 Capabilities, not vendors

The renderer may log `vendorID` and `deviceID` for diagnostics, but behavior is
selected from Vulkan features, limits, formats, queue families, and extensions.
No rendering decision may be keyed solely on AMD, Intel, NVIDIA, or Apple.

Capabilities are collected once into a `VulkanCapabilities` value during device
selection and passed to resource and pipeline creation. A capability is one of:

- required: device selection fails with a precise explanation when absent;
- optional with a specification-correct strategy: both strategies have tests;
- diagnostic only: it never changes rendering semantics.

MoltenVK-specific surface creation stays inside the macOS platform adapter.
Core graphics state, surface layout, shader translation, and synchronization do
not contain macOS policy. Linux and Windows adapters will provide native Vulkan
surface creation without changing guest GPU behavior.

### 4.6 Portability boundaries

Operating-system behavior is confined to narrow modules for:

- process and virtual memory;
- guest thread and exception integration;
- monotonic clocks and synchronization;
- window and Vulkan surface creation;
- controller discovery;
- dynamic library loading.

`__APPLE__`, `_WIN32`, and Linux-specific branches belong inside those modules,
not in guest HLE, PM4 decoding, shader translation, or surface layout. macOS must
not be described or compiled as Linux merely because both are POSIX-like.

Host CPU execution is a separate portability dimension. The current native
x86-64/Rosetta path is preserved. A future CPU backend for non-x86 hosts must sit
behind the execution interface; this graphics phase does not introduce one.

## 5. Data Flow

```text
Guest executable
  -> Runtime linker and verified HLE exports
  -> AGC command-buffer builders
  -> PM4 packet parser
  -> shared guest-state decoders
  -> normalized HW::Context
  -> surface layout and resource cache
  -> shader translation and Vulkan pipeline
  -> command submission and synchronization
  -> VideoOut image
  -> Vulkan swapchain presentation
```

Diagnostics observe boundaries in this flow. They do not mutate state, inject
default return values, skip packets, or replace guest resources.

## 6. Error and Diagnostic Policy

### 6.1 Strict mode is authoritative

A successful acceptance run uses neither `KYTY_STUB_MISSING` nor
`KYTY_GFX_PERMISSIVE`. Progress measured only under those flags is diagnostic,
not compatibility.

### 6.2 Diagnostic mode is explicit and non-shipping

Existing diagnostic flags may remain while bring-up is active, but output must
state that guest behavior is invalidated. They cannot be enabled by default,
used in release tests, or cited as proof that a game works.

### 6.3 Unsupported behavior fails with evidence

An unsupported import, packet, register, shader instruction, surface format, or
Vulkan requirement reports enough structured data to reproduce it. It does not
continue with guessed state.

Expected guest errors, such as releasing an unallocated direct-memory range,
return the documented guest error code and do not terminate the emulator.

## 7. Delivery Sequence

### Milestone A: Reproducible strict baseline

- Replace the machine-specific game path with an argument or environment-driven
  local configuration.
- Add a concise capture of build version, platform, GPU capabilities, game title
  and version, and diagnostic flags at run start.
- Implement and test `sceKernelCheckedReleaseDirectMemory`.
- Confirm the strict frontier advances beyond that NID.

### Milestone B: Complete the observed AGC state path

- Centralize the existing direct-register decoders.
- Route registers `0x90`, `0x91`, `0x205`, and `0x1e0` through them from the
  indirect table.
- Implement the called SH-register and dispatch AGC exports with verified packet
  layouts.
- Continue strict runs one unsupported item at a time until the first frame is
  submitted without missing stubs or skipped state.

### Milestone C: Correct resources and frame

- Capture the exact descriptors for the corrupt frame's textures, color targets,
  depth targets, and VideoOut buffers.
- Add failing layout tests using sanitized descriptor values from the capture.
- Implement format and layout rules required by those descriptors.
- Remove the generic linear fallback.
- Validate upload/detiling, shader resource binding, render areas, scissors,
  blending, synchronization, and the VideoOut blit.
- Produce a recognizable, correctly proportioned reference frame.

### Milestone D: Prepare the playable phase

- Record stable draw, flip, input, audio, timing, and memory behavior at the
  first interactive screen.
- Write the next focused design for controller-to-first-room gameplay, audio,
  stability, and performance.
- Preserve all strict-frame tests as regression gates.

### Milestone E: Prepare cross-platform validation

- Add compile-only CI targets for Linux and Windows once platform boundaries are
  clean enough to build there.
- Write the subsequent runtime-validation design and hardware matrix.
- Do not claim a GPU vendor supported until the strict acceptance scenario runs
  on representative hardware from that vendor.

## 8. Testing Strategy

### 8.1 Unit tests

- Direct-memory allocation/release success, wrong range, wrong size, zero size,
  and double release.
- Direct and indirect context-register equivalence for every centralized state.
- Format metadata for uncompressed and block-compressed formats used by the
  captured frame.
- Surface layout for every captured descriptor, including mip offsets, row
  pitches, total size, and alignment.
- Vulkan capability classification independent of vendor identifiers.

### 8.2 GPU-independent integration tests

- Parse sanitized PM4 fixtures and compare normalized `HW::Context` snapshots.
- Build AGC packets through HLE exports and parse them back to the expected
  normalized state.
- Reject unknown registers and invalid surface descriptors with stable error
  categories.

Fixtures contain no copyrighted game code, shaders, textures, keys, or assets.

### 8.3 Local game acceptance

The opt-in acceptance run must prove all of the following:

1. The checked-in build succeeds.
2. The supplied title and version are detected.
3. No missing stub is called.
4. No PM4/AGC register or packet is skipped.
5. Shader translation and Vulkan pipeline creation report no fatal error.
6. At least 120 consecutive flips complete without crash or hang.
7. The captured frame has the expected 1280x720 presentation extent.
8. The image is recognizable and correctly proportioned, not merely non-black.
9. A frame capture and structured run summary are retained as local evidence;
   proprietary game files are never committed.

Pixel entropy or a non-black-pixel count is useful as a regression signal but is
not sufficient evidence of visual correctness.

## 9. Repository Operating Manual

The current `AGENT.md` is replaced by one canonical `AGENTS.md`; no duplicate or
legacy alias remains. The manual must include:

- mission and current verified status;
- architecture map and ownership boundaries;
- strict no-guess/no-silent-skip invariants;
- evidence-first debugging workflow;
- shared-decoder and centralized-layout rules;
- OS and GPU portability rules;
- reference-project and license policy;
- formatting, testing, build, run, and commit commands;
- proprietary asset and log hygiene;
- regression and completion checklists.

The manual must distinguish a correct alternative implementation for an absent
Vulkan capability from a behavioral fallback. The former is acceptable only
when semantics are preserved and both paths are tested. The latter is forbidden.

## 10. Reference Use and Licensing

- Public Gen5 export tables: names/signatures and PS5 subsystem vocabulary only.
  No GPL implementation code is copied into this MIT repository.
- RPCSX: CMake/platform organization, GPU subsystem decomposition, and AMD
  graphics research patterns. GPL implementation code is not copied.
- Ryubing/Ryujinx: renderer abstraction, capability modeling, resource lifetime,
  and cross-vendor testing patterns. Console-specific behavior is not assumed to
  match PS5.
- EmuC0re: small subsystem boundaries and native PS5-facing API observations.
  Its on-console execution architecture is not used as a desktop renderer model.

Every implementation derived from external research documents the behavioral
fact being implemented and its provenance. Code is written against Kyty's own
types and architecture after verifying the behavior against local evidence.

## 11. Completion Criteria

This phase is complete only when current evidence proves all of the following:

- `AGENTS.md` is the single professional operating manual.
- The local run configuration resolves an untracked compatibility fixture
  without a hardcoded personal path in tracked files.
- The reference workload reaches and presents a correct, recognizable frame in
  strict mode.
- No generic missing-symbol stub or permissive GPU register skip is required.
- The generic texture-size fallback has been removed and replaced by validated
  surface layouts for all resources used by the accepted frame.
- Direct and indirect register paths share decoders and pass equivalence tests.
- Vulkan behavior is selected by capabilities rather than GPU vendor.
- The macOS build, automated tests, strict 120-frame run, and visual capture all
  pass without regressing the existing functional path.
- Remaining work toward first-room gameplay and Linux/Windows multivendor
  validation is recorded as subsequent designs, not claimed complete.
