# Temporal upscalers: DLSS and XeSS decision (design, 2026-10-29)

Status: decided 2026-10-29. Design only. No DLSS, XeSS or FSR 2 backend, no
dependency and no code change is part of this record.

Scope: whether Kyty should add DLSS or XeSS temporal upscaling next to the
existing FSR 1.0 presentation path. The record covers the inputs they need,
whether Kyty can produce those inputs from guest frames, licensing and
dependency cost, Linux/Vulkan availability per vendor, and a recommendation.

Pinned revision: repository citations name `origin/main` at `598de8e2d`
(read 2026-10-29). Line numbers refer to that revision.

Evidence labels, used the same way in every section:

- `[cited]`: checked on 2026-10-29 against a repository file at the pinned
  revision (path and lines given), or against an official web page listed under
  "Web sources" in Sources (section given).
- `[UNVERIFIED]`: not checked against a primary source in this record. The
  statement is a lead only and must not drive a decision until it is checked.
- Sentences marked "(judgement)" are reasoning from the cited facts. They are
  not evidence.

## Decision

Do not add DLSS or XeSS now. FSR 1.0 stays the only upscaler.

If temporal upscaling is pursued later, the order is:

1. Establish a general, evidenced source for the temporal inputs (motion
   vectors, depth, jitter). Without it no temporal backend can be correct
   (judgement; the input list is the FSR 2 README's, see "Inputs").
2. Evaluate FSR 2 first as the vendor-neutral candidate (judgement). Its MIT
   grant and its DirectX 12 and Vulkan backends are `[cited]`. Whether it runs
   on the target GPUs, and on non-AMD GPUs, is `[UNVERIFIED]`. It needs the same
   inputs as step 1.
3. Consider a proprietary SDK (DLSS or XeSS) only after 1 and 2 are measured
   and a shipping licence review is done. The obligations that review must cover
   are listed in "Licensing and dependency cost".

Reasons, in short:

- Per-pixel motion vectors, sub-pixel jitter and exposure are not produced by
  Kyty. The lexical search in "Current state" finds no previous-position output
  and no jitter or exposure data `[cited]`. Depth attachments exist, but no
  upscaler-facing depth contract does `[cited]`.
- DLSS: the RTX Supplement licenses the DLSS SDK "to develop applications only
  for their use in systems with NVIDIA GPUs" (RTX Supplement section 1) `[cited]`. The licence
  forbids distributing or sublicensing the SDK "as a stand-alone product"
  (licence section 4(b)) `[cited]`. Shipping it adds a proprietary binary
  dependency that is limited to NVIDIA systems.
- XeSS: the Intel Simplified Software License forbids "reverse engineering,
  decompilation, or disassembly" of the Software and permits redistribution
  "in binary form only" `[cited]`. Its Linux and Vulkan support is
  `[UNVERIFIED]`. A proprietary binary dependency with unverified Linux support
  is not justified while the inputs are missing (judgement).
- The render-resolution policy limits what any upscaler can gain on titles bound
  by offscreen passes (see "Render-resolution interaction") `[cited]`.
- Inferred inputs produce ghosting and disocclusion artefacts (judgement). The
  project's mission requires playable titles "without visual errors"
  (`AGENTS.md`, Mission) `[cited]`.

## Current state

- Presentation filters are `Linear`, `Nearest` and `Fsr`
  (`source/emulator/include/Emulator/Config.h`, lines 65-70, enum
  `PresentationFilter`) `[cited]`.
- FSR 1.0 is the only upscaler. Its header states the applicability rule: it
  runs only when the source fits inside the target and is smaller on at least
  one axis (`source/emulator/include/Emulator/Graphics/PresentationFsr.h`, lines
  21-22) `[cited]`. The definition is `PresentationFsrApplies` in
  `source/emulator/src/Graphics/PresentationFsr.cpp`, lines 340-345 `[cited]`.
- The scaler maps `Fsr` to `VK_FILTER_LINEAR`
  (`source/emulator/src/Graphics/PresentationScaler.cpp`, line 27) `[cited]`.
  The FSR branch is gated at lines 203-204 (`Fsr` filter and
  `PresentationFsrApplies`) and records the passes at line 207. When the gate
  fails, the scaler uses the blit path: `UtilBlitImage` at line 215 when no sRGB
  encode stage is needed, otherwise `BlitThroughEncodeStage` at line 227 `[cited]`.
- The applicability rule and the sharpness mapping are pinned by
  `TEST(EmulatorGraphicsState, RunsFsrOnlyForPresentationUpscales)`
  (`source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`, lines
  3648-3661) `[cited]`.
- FSR 1.0 headers are vendored under MIT in `source/3rdparty/fidelityfx_fsr1/`
  with `LICENSE.txt`. The README states they are not compiled into the C++ build
  (`source/3rdparty/fidelityfx_fsr1/README.md`) `[cited]`.
- Render-resolution policy (`docs/render-resolution.md`): mode `Fixed` or
  `Native` (line 12), scope `Display` (default) or `RenderTargets` (opt-in,
  `Fixed` only, lines 13 and 79-85) `[cited]`. `Fsr` does not change the render
  resolution or the guest's work (lines 185-187) `[cited]`.
- Lexical search, case-insensitive, over `source/emulator` (its `src` and
  `include` directories), in the working copy read on 2026-10-29 `[cited]`:
  - `motion` matches 112 lines (80 in `src`, 32 in `include`). Of these, 35
    lines contain `Promotion` or `Demotion`, which include the substring
    `motion`. The other lines are controller and pad motion sensors, mouse and
    finger motion events
    (`source/emulator/src/Graphics/Window.cpp`, lines 1075-1092 and 1124-1126;
    `source/emulator/src/Host/HostInputSdl.cpp`), and motion sensor state.
  - `velocity` matches 22 lines. All of them concern the angular velocity of a
    controller (`source/emulator/include/Emulator/Host/HostInput.h`, line 176;
    `source/emulator/src/Controller.cpp`, lines 1385 and 1421-1426).
  - `jitter` and `exposure` have no hits. Of the four terms, only `motion` and
    `velocity` have hits.
  - `previous` with `position`, `clip` or `pos` (either order): four lines, none
    about motion. `source/emulator/src/Graphics/GraphicsRenderContext.cpp` line
    113 is a previous context, `source/emulator/src/Graphics/SampleLocations.cpp`
    lines 82 and 91 are array indices, and `source/emulator/src/Kernel/Memory.cpp`
    line 1083 is a comment.
  - `mvp`, `prev_`, `previous_` or `last_` combined with `pos`, `clip`, `view`
    or `proj` (either order, same line): the matches are view, mip and slice
    names, for example `view_last_level` in
    `source/emulator/src/Graphics/GraphicsRenderBind.cpp` and
    `last_array_slice_index` in
    `source/emulator/src/Graphics/GraphicsRenderAttachments.cpp`, plus the
    `previous_ctx` line above. None is a previous transform or position.
  - Reactive, scene-cut and history-reset search: pattern
    `reactive|scene.?cut|history.?reset|reset.?history`, case-insensitive, over
    the same directories: no hits `[cited]`.
  - Limits: the search is lexical. It does not classify names such as `history`
    (for example `GpuWriteHistory`), and it covers source only, not guest data.
    The absence of a per-pixel motion vector path is therefore a finding of this
    search, not a proof about every translator output.

## Inputs the temporal upscalers need

The input list comes from the FSR 2 README (resource table, lines 187-190, and
camera jitter, lines 111-112 and 382-387) `[cited]`. Web line numbers are not
pinned to a commit (see Sources and Open issues).

| Input | Used for | Kyty status |
| --- | --- | --- |
| Per-pixel motion vectors | Reprojecting history to the current frame | Not produced. The lexical search above finds no previous-position output `[cited]`. |
| Depth | Reprojection and disocclusion detection | Partly available, scope-dependent. The render-resolution coordinator receives depth/stencil attachments (`docs/render-resolution.md`, lines 61-62) `[cited]`. Under the default `Display` scope, the VideoOut depth is scaled with the display cohort under `Fixed` (lines 81 and 185-187) `[cited]`. Under the opt-in `RenderTargets` scope, offscreen depth whose guest extent equals the display extent is a candidate (lines 81-83). It follows the same rule as colour: GPU-owned, single-sample, and aliased only by storage views of its planes (lines 87 and 97-99) `[cited]`. It scales only as part of a cohort whose members all agree on the extent (lines 101-105) `[cited]`. Depth is never demoted, and a committed scaled depth beside a native member stops the run (lines 129-133) `[cited]`. HTILE and multisampled depth stay native (line 99) `[cited]`. Depth is never uploaded or written back (lines 97-98) `[cited]`. No upscaler-facing contract exists: `docs/render-resolution.md` defines no linearisation, reversed-Z or stage and image choice `[cited]`. |
| Sub-pixel jitter (projection offset) | Sub-pixel sampling for accumulation | Not captured. The FSR 2 README requires the application to apply it (lines 111-112) `[cited]`. Whether a guest jitters at all, and where, is guest-specific `[UNVERIFIED]` for any title. |
| Exposure | HDR normalisation of colour input | Not produced. No exposure data in the lexical search `[cited]`. FSR 2 treats it as optional (README line 190) `[cited]`. |
| Reactive mask | Limits history use on transparent or animated pixels | Not evaluated. The FSR 2 README describes it (line 189) `[cited]`. The reactive-name search in "Current state" finds no match `[cited]`. |
| History reset (scene cut) | Discarding stale history | No guest signal in the source. The scene-cut and history-reset search in "Current state" finds no match `[cited]`. |

## Can Kyty obtain these from guest frames in general

Short answer: not today, and not with evidence that holds across titles
(judgement).

- Motion vectors need the previous transform of every vertex. A general method
  would have the shader translator emit current and previous clip positions for
  each draw, with previous state taken from the guest's previous constants. That
  is a new translator output, which also touches the translator's cache
  versioning. It covers geometry only: particles, animated materials and post
  effects are missed unless handled separately (judgement).
- A guest's own velocity buffer, where one exists, is a per-title layout. Using
  it would be a per-title branch, which `AGENTS.md` forbids (Non-negotiable
  rules) `[cited]`.
- Jitter can only be read from the guest's projection constants, which are
  guest-defined. Inferring it by comparing successive projections is an
  inference, not evidence (judgement).
- Depth is available as a render target in general, subject to the scope rules
  above, but its meaning for an upscaler is not defined by any guest contract
  Kyty has `[cited]`.
- Exposure has no general guest contract `[cited]`.

Any temporal path today would therefore run on inferred inputs. Inferred motion
and depth are the known source of ghosting and disocclusion errors, which the
mission excludes (judgement).

## Licensing and dependency cost

Each new dependency needs a written justification (judgement, from the project's
engineering standards). None is justified at present because none of the inputs
exists (judgement).

| Option | Licence and distribution | Linux and Vulkan | Dependency cost |
| --- | --- | --- | --- |
| DLSS (NVIDIA) | NVIDIA RTX SDKs License (v. January 23, 2023) `[cited]`: the grant covers distributing SDK software "as incorporated in object code format into a software application subject to the distribution requirements" (licence section 1(c)). Requirements: the application "must have material additional functionality, beyond the included portions of the SDK" (licence section 2(a)), and distribution on terms "at least as protective" as the licence (licence section 2(c)). Reverse engineering is forbidden (licence section 4(a)), and "you may not distribute or sublicense the SDK as a stand-alone product" (licence section 4(b)). The SDK may not be used in a way that makes it subject to an open-source licence (licence section 4(e)) `[cited]`. RTX Supplement, DLSS: development "only for their use in systems with NVIDIA GPUs" (RTX Supplement section 1) `[cited]`. Before commercial release, NVIDIA must be notified (RTX Supplement section 3) `[cited]`. NVIDIA may make over-the-air DLSS updates, and a public integration with material issues must be fixed or NVIDIA may disable it (RTX Supplement section 5) `[cited]`. The DLSS marketing terms require attribution and NVIDIA Marks on splash screens, in the about box and in game credits (RTX Supplement section 6.1(b)) `[cited]`. Which DLSS components are distributable is `[UNVERIFIED]`. Whether the 2023 text is current is `[UNVERIFIED]`. | DLSS Vulkan and Linux support: `[UNVERIFIED]`. No NVIDIA DLSS SDK documentation page was read. | Proprietary binary, NVIDIA systems only, with the notification, update and attribution obligations above. Other vendors would need a separate path. |
| XeSS (Intel) | Intel Simplified Software License (Version October 2022) `[cited]`. Redistribution is permitted for software "provided in binary form only", "without modification" (use and redistribution clause). "No reverse engineering, decompilation, or disassembly of the Software is permitted, nor any modification or alteration of the Software or its operation at any time, including during execution" `[cited]`. The licence names no operating system `[cited]`. | Linux and Vulkan support: `[UNVERIFIED]`. The XeSS 3 SDK README names no operating system and no graphics API `[cited]`. | Proprietary binary with vendor-specific paths. The README describes the DP4a path for XeSS-SR `[cited]`, see "Vulkan and Linux availability per vendor". Whether that path runs on Linux or Vulkan is `[UNVERIFIED]`. |
| FSR 2 (AMD FidelityFX) | The FSR 2 README (version 2.2.1) carries the MIT permission text, copyright AMD 2023 `[cited]`. The public release has DirectX 12 and Vulkan backends `[cited]`. | Vulkan backend: `[cited]`. FP16 is recommended. The README says that for Vulkan, if `VkPhysicalDeviceFloat16Int8FeaturesKHR::shaderFloat16` or `VkPhysicalDevice16BitStorageFeatures::storageBuffer16BitAccess` is not set, the FP32 version is used (line 460) `[cited]`. Defining `FFX_HALF` as `1` compiles the FP32 path (line 462) `[cited]`. Whether the two Vulkan features are present on the target GPUs: `[UNVERIFIED]`. | Source-level MIT code, the same vendoring pattern as `source/3rdparty/fidelityfx_fsr1/` (judgement). It needs a justification and a third-party notice. It needs the same missing inputs. |
| FSR 4 (ML) | Repository text, `docs/render-resolution.md` (lines 208-209): FSR 4 needs "its signed binary distribution" and "an AMD 7000 series discrete GPU or AMD 9000 series GPU or later" `[cited, as a repository quote]`. The FidelityFX SDK page states that the SDK improves DirectX 12 or Vulkan applications and that the samples are MIT-licensed `[cited]`. The signed-binary and GPU wording, and the sentence "Vulkan is currently not supported in SDK" (`docs/render-resolution.md`, line 212), were not found on the pages read, and the known-issues table was not readable: `[UNVERIFIED]`. | Not evaluated further (see the Vulkan sentence above). | Not evaluated further. It is temporal, so it needs the same inputs (`docs/render-resolution.md`, lines 204-206) `[cited]`. Its binary and hardware limits are `[UNVERIFIED]`. |

## Vulkan and Linux availability per vendor

- NVIDIA: DLSS is licensed only for systems with NVIDIA GPUs `[cited]`. Its
  Vulkan and Linux path is `[UNVERIFIED]`. FSR 2 has a Vulkan backend `[cited]`;
  its use on NVIDIA hardware is `[UNVERIFIED]`.
- XeSS, DP4a path: the XeSS 3 SDK README states that XeSS-SR "boosts frame rates
  on all GPUs with SM 6.4 (DP4a) support" `[cited]`, and it names no XMX path
  `[cited]` (Web sources, XeSS 3 SDK README, "What's Inside?"). Whether DP4a is
  the non-XMX path, and whether it runs on NVIDIA GPUs, Linux or Vulkan, is
  `[UNVERIFIED]`.
- Intel: Kyty verified the render-resolution presentation path on Intel
  (`docs/render-resolution.md`, line 197, "Verified on Intel") `[cited]`. XeSS is
  the vendor path. Its Linux and Vulkan support is `[UNVERIFIED]`.
- AMD: `docs/render-resolution.md`, lines 189-192, records frame-rate
  measurements of a title on an AMD GPU. The model is not named there `[cited]`.
  FSR 4 is not evaluated further (see the licensing table). The XeSS-SR DP4a
  path applies to SM 6.4 GPUs `[cited]`. FSR 2 is the candidate (judgement).

## Render-resolution interaction

- Under `Fixed` with the default `Display` scope, only the VideoOut cohort (its
  colour and depth) is scaled. Offscreen targets keep guest size and cost
  (`docs/render-resolution.md`, lines 185-187) `[cited]`. Under the opt-in
  `RenderTargets` scope, offscreen colour targets and their depth whose guest
  extent equals the display extent are candidates (lines 81-83) `[cited]`. A
  depth target follows the colour rule, with storage views of its planes as its
  only allowed aliases (lines 97-98) `[cited]`. A cohort scales only when every
  member agrees (lines 101-105) `[cited]`. A committed scaled depth beside a
  native member, or a refused use of a scaled image, stops the run (lines
  129-133) `[cited]`. A scaled colour target can be demoted instead (lines
  111-127) `[cited]`.
- Measured evidence: `docs/render-resolution.md` (lines 185-192) records that
  display scaling did not move the frame rate of a title bound by offscreen
  passes. The figures are kept there, so this record does not copy them
  `[cited]`.
- A temporal upscaler would act on the same display cohort. Its gain is bounded
  by the share of GPU time spent on display-cohort passes. That share is not
  measured for any title `[UNVERIFIED]`. It would not help titles bound by
  offscreen passes or by the command processor, which the render-resolution
  document already records `[cited]`.
- The repository already records that temporal upscalers are not offered
  (`docs/bringup/cross-title.md`, lines 3521-3523, section "FSR 1.0 presentation
  upscaling") `[cited]`. This record is consistent with that entry.

## Recommendation

1. Do not add a DLSS or XeSS backend. No code, no dependency.
2. Keep FSR 1.0 as the only upscaler. No change to `Fsr` or `PresentationFsr`.
3. Open a separate design item only when all of these hold:
   - a general, evidenced source of motion vectors (and depth, jitter) exists
     for a class of guests, not for a single title;
   - a measured frame-rate or visual gain at a lower render resolution on
     titles bound by display-cohort passes;
   - a strict run on the current head is not regressed;
   - the licence of the chosen SDK is cleared against its current primary text,
     including the notification, update and attribution terms above;
   - FSR 2 has been evaluated as the vendor-neutral option first.

## Reference-only observation (lead, not evidence)

An external design was reviewed as a lead only. It proposed capturing current
and previous vertex positions and fragment depth into a spare attachment during
the original draw, validated by coverage, with optical flow for uncovered
pixels and rejection of uncovered history in geometry mode. The same material
states that motion and depth inputs are inferred unless a validated guest guide
exists, and that real-game validation is still needed. The reviewed material
records the design as closed, with no closing reason. Nothing in this record
depends on it.

## Open issues

- DLSS Vulkan and Linux support, and the list of distributable DLSS components,
  are `[UNVERIFIED]`. Check the current NVIDIA DLSS SDK documentation and licence
  before any decision that depends on them.
- The NVIDIA licence checked is the January 23, 2023 text. Whether it is still
  current is `[UNVERIFIED]`.
- XeSS Linux and Vulkan support is `[UNVERIFIED]`. The README and licence do not
  state it `[cited]`.
- Lead, not a finding: `docs/render-resolution.md` lines 207-212 quote FSR SDK
  wording (AMD 7000 and 9000 series, signed binary, "Vulkan is currently not
  supported in SDK"). The FidelityFX SDK page read on 2026-10-29 says the SDK
  improves DirectX 12 or Vulkan applications. The hardware wording was not found
  on the pages read, and the known-issues table was not readable, so the Vulkan
  sentence is `[UNVERIFIED]`. Re-check against the FidelityFX SDK documentation
  in a separate change. This record does not edit `docs/render-resolution.md`.
- Depth scope wording differs between two repository files.
  `source/emulator/include/Emulator/Config.h` lines 56-58 describe `RenderTargets`
  as covering offscreen colour targets only. `docs/render-resolution.md` lines
  81-83 also name their depth. Which description is correct is open `[cited]`.
- The two FSR 2 FP16 Vulkan features (`shaderFloat16`,
  `storageBuffer16BitAccess`) on the target GPUs are `[UNVERIFIED]`. The
  requirement itself is `[cited]` (FSR 2 README, line 460).
- Web line numbers (FSR 2 README, XeSS README and licence, FidelityFX SDK page)
  were read on 2026-10-29 from moving branches (`master`, `main`). They are not
  pinned to a commit and must be re-read before a decision depends on them.
- This record is not yet indexed in `docs/BRINGUP.md`. The index entry must be
  added by the orchestrator, since that file is outside this item's scope. The
  index has no design-record entries at the pinned revision, so the proposed
  entry follows its link form (a top-level link with one summary sub-bullet),
  placed directly above the "Cross-title records (2026-10-10 onward)" link
  (`docs/BRINGUP.md` line 111 at the pinned revision). Proposed text:
  `- [Temporal upscaler decision (design, 2026-10-29)](bringup/temporal-upscalers-decision.md)`
  followed by `  - No DLSS or XeSS backend; FSR 1.0 stays the only upscaler (2026-10-29)`.
  The orchestrator chooses the final position.
- No measurement exists on any title for a temporal backend. Nothing here
  measures frame-rate or visual benefit.
- The guest motion-vector question (how a general previous-position contract
  could be obtained from translated shaders) is unanswered. It needs its own
  design item.

## Sources

Repository files (`origin/main` at `598de8e2d`; line numbers checked in this
record):

- `docs/render-resolution.md` (lines 12-13, 61-62, 79-133, 185-197, 204-212)
- `docs/bringup/cross-title.md` (lines 3496-3523)
- `docs/BRINGUP.md` (line 111)
- `source/emulator/include/Emulator/Config.h` (lines 56-58, 65-70)
- `source/emulator/include/Emulator/Graphics/PresentationFsr.h` (lines 21-22)
- `source/emulator/src/Graphics/PresentationFsr.cpp` (`PresentationFsrApplies`, lines 340-345)
- `source/emulator/src/Graphics/PresentationScaler.cpp` (lines 25-27, 203-215, 227)
- `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp` (lines 3648-3661)
- `source/3rdparty/fidelityfx_fsr1/README.md` and `LICENSE.txt`
- `source/emulator/src/Controller.cpp` (lines 1385, 1421-1426), `source/emulator/include/Emulator/Host/HostInput.h` (line 176), `source/emulator/src/Host/HostInputSdl.cpp`, `source/emulator/src/Graphics/Window.cpp` (lines 1075-1092, 1124-1126): lexical search hits
- `source/emulator/src/Graphics/GraphicsRenderContext.cpp` (line 113), `source/emulator/src/Graphics/SampleLocations.cpp` (lines 82, 91), `source/emulator/src/Kernel/Memory.cpp` (line 1083): lexical search hits
- `AGENTS.md` (Mission; Non-negotiable rules)

Web sources (opened and read on 2026-10-29; line numbers are as read from
moving branches and are not pinned to a commit):

- XeSS 3 SDK README, `https://github.com/intel/xess`, section "What's Inside?":
  XeSS-SR "boosts frame rates on **all GPUs** with SM 6.4 (DP4a) support"; XeSS-FG
  available on non-Intel GPUs with SM 6.4 support. The README names no operating
  system, no graphics API and no XMX path.
- XeSS licence, `https://github.com/intel/xess/blob/main/LICENSE.txt`: Intel
  Simplified Software License (Version October 2022), use and redistribution
  clause and reverse-engineering clause.
- FSR 2 README (FidelityFX Super Resolution 2.2.1), `https://github.com/GPUOpen-Effects/FidelityFX-FSR2`
  (`master` branch): MIT permission text; DirectX 12 and Vulkan backends (library
  table and "public release" note); resource table (lines 187-190: depth, reactive
  mask, optional exposure); camera jitter (lines 111-112, 382-387); "Falling back
  to 32-bit floating point" (lines 460-462).
- FidelityFX SDK repository, `https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`:
  "AMD FSR™ Samples are open source, and available under the MIT license"; the
  SDK improves "DirectX® 12 or Vulkan® applications". Its known-issues table was not
  readable on the page read, so its Vulkan limits are `[UNVERIFIED]`.
- NVIDIA RTX SDKs License (v. January 23, 2023) with the RTX Supplement,
  `https://developer.download.nvidia.com/gameworks/NVIDIA-RTX-SDKs-License-23Jan2023.pdf`:
  licence sections 1(c), 2(a), 2(c), 4(a), 4(b), 4(e); RTX Supplement sections
  1, 3, 5 and 6.1(b). Read as text on 2026-10-29.
