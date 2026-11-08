# First-person puzzle title

Title: PPSA06084 - Superliminal.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## First-person puzzle title: GPU hang in the shadow pass on Intel Arc (2026-11-07, open)

- **Observation.** PPSA06084 - Superliminal loses the device on Intel Arc A770 (ANV, Xe KMD) about once per minute of
  play. The GuC resets the render engine with no page fault.
  - In every decoded batch the hang is in a shadow pass: D16 at 2048x2048, depth write, depth bias. It mixes
    draws without a pixel shader with draws of a pixel shader that kills pixels (an LOD dither).
  - INSTDONE shows the windower, HiZ, depth front end and SBE busy. The samplers are idle and every subslice is
    in the same state.
- **Excluded.** HiZ and depth or colour compression (`INTEL_DEBUG=nohiz,noccs`), SIMD32 dispatch, Vulkan validation
  (clean), page faults and the recent commits; the hang is older.
- **Pixel shader.** Its translation has no loop, no maximal reconvergence and a single kill, taken only when the whole
  quad is dead. Mesa turns that kill into a demote, and the native code ends in the usual predicated render-target
  write. Nothing points at the translation, so this is treated as a driver or hardware hazard whose cause is not
  known yet.
- **Mitigation tried, not committed.** A bottom-of-pipe timestamp before and after every depth-writing kill draw
  (valid Vulkan: per-recording query pools reset from the host).
  - It lowers the hang rate about twentyfold but does not remove it: one hung batch had the marks around every kill
    draw.
  - Cost: 57.5 -> 54.1 fps; GPU 10.1 -> 13.7 ms per frame.
- **Translation gap found on the way (not the hang).** At the final `exp ... done vm`, exec only kills when the whole
  quad's exec is zero. Dead lanes of a partly live quad still write depth, so the dither works per 2x2 quad rather than
  per pixel.

## First-person puzzle title: empty vertex descriptors and empty indexed draws (2026-10-31)

Implements the direction of the record below, and closes a second stop found on the same title.

- **Zero-record V#.** The range-checking section of the RDNA2 ISA states that reads out of range return zero. An
  attribute with an empty V# now stays in the Vulkan layout on one binding past the declared buffers, stride 0, bound
  to a zero-filled buffer. A resource with records and no buffer is still refused.
- **Empty indexed draw.** PPSA06084 - Superliminal also stopped at `src_buffer == nullptr` in `UtilCopyBuffer`, right
  after an invalid staging lease in the index buffer update. The indirect path already returned on zero indices, the
  direct ones did not. A zero size is the only one that survives the `memcpy` before that check. `GraphicsRenderDrawIndex`
  now returns on zero indices for all of its callers.
- **Verified.** Unit test for the layout. A 170 s run on Intel Arc A770 (ANV) reached the first room at 59.2 fps with no
  stop. **Not proved in play:** both stops are intermittent (each appeared once in four runs), the run after the change
  logged neither path, and the zero-count draw was inferred from the code, not observed.

## First-person puzzle title: zero-record vertex buffers stop the menu (2026-10-26, open)

- **Observation.** `main` at ad9cdac6, Intel Arc A770, Linux 7.2.11, strict,
  60 fps limit, no input on the main menu. With a SPIR-V, Mesa and XDG cache
  accumulated over three earlier 95 s runs, the run stops at about 86 s on
  `unsupported vertex input layout: resources=2 buffers=0`
  (`GraphicsRenderPipeline.cpp:503`). Seen in 3 of 3 runs on that cache.
  It does not reproduce on a cold cache: one 100 s run, plus a 900 s menu idle
  that reached frame 35278.
- **Trigger.** `KYTY_VIL_LOG=1` shows two vertex V#s that both have
  `num_records=0`, stride 48, dst VGPRs 9/4 and 13/3, and formats
  dfmt 9 / nfmt 2 and dfmt 8 / nfmt 7. `VulkanBuildVertexInputLayout` builds
  no buffer for them but keeps both resources, so the layout check refuses
  the pipeline.
- **Direction.** Hardware returns zero for every fetch outside
  `num_records`, so a zero-record V# is a valid guest state. The layout
  should bind it like an out-of-range fetch (a zero-filled binding) rather
  than refuse it. Separately, it is still open why only the warm cache
  reaches this draw.

## First-person puzzle title: scalar-join sources per label (2026-10-22)

- **Observation.** Cold boot, Intel, 40 s sampled on the command-processor
  thread from 25 s: `ScJoinCollectSources` takes 37% of the thread inside
  `Spirv::WriteLabel`; its loop-exit test and `ShaderCode::ReadBlock` scan the
  whole program for every branch, and the collection itself ran once per
  label.
- **Change.** The sources of every join are collected in one pass per program
  and kept in the existing per-program join memo, indexed by reconvergence
  point (`ShaderSpirvScJoin.cpp`). The memo keeps its validity rule (program
  identity and enabled-label count).
- **Measured (90 s cold runs).** Translator source generation per 26 s window
  7.9-13.3 s -> 1.7-1.9 s. The 317 SPIR-V cache entries both runs produced
  are byte-identical. Pipeline misses resolved in the second window: 16 ->
  114.
- **Open.** First boot is now bound by `spirv-opt` (~14 s per window) and
  driver pipeline creation (~15 s per window).

## First-person puzzle title: cold shader compiles, not a GPU stall (2026-10-17)

The 10-17 regression set recorded this title as stalling early on a
`gfx_storage_frontier` refusal. That attribution was wrong.

- **Observation.** In the 120 s sweep window the command-processor thread runs
  at ~93% CPU translating and compiling shaders; presents pause while it does.
  The same binary given 300 s reaches the first lit room and holds it
  (8 captures, ~58 fps once compiled). The sweep harness starts every run with
  empty SPIR-V, driver and pipeline caches, so this is the worst-case first
  boot; the on-disk caches remove it on later boots.
- **Compile cost in one 35 s stall window** (opt-in toolchain timing,
  Intel): translator source generation ~5.9 s, `spirv-opt` ~11 s,
  driver pipeline creation ~8 s. Two pixel shaders of 3.7 MB of SPIR-V text
  dominate.
- **Why it grew.** The native pixel mask emulation transfers every ballot
  through the quad (`NativeQuadUniform`, 11 quad broadcasts per ballot instead
  of 4), and every VALU result with software EXEC was stored inside its own
  selection construct. Thousands of tiny selections made `spirv-opt` and the
  driver slow.
- **Change.** Inactive lanes keep their VALU result through `OpSelect` instead
  of a branch (`ShaderSpirvVector.cpp`, translator version 134). The large
  shaders (3.7 MB of text, ~490 ballots, 6,582 quad broadcasts each) drop
  from ~2,230 to 585 blocks and from ~1,000 to 185 selections; `spirv-opt`
  time per compile halves.
- **Exclusions.**
  - Compiling with `spirv-opt` disabled moves the same cost into the driver;
    no net gain.
  - Removing ballots whose readers only take their own lane bit could remove
    at most ~40% of them; not pursued yet, because it changes mask semantics in
    every pixel shader.
- **Verified (Intel, strict, 22-title sweep, cold caches):** 20 titles match
  the previous baseline in status, captures and frame rate; captures of the
  sequel, the underworld title, the fishing title and the anime fighter are
  visually unchanged. The role-playing title stops at the open storage-view
  refusal recorded in the next section. The puzzle title shows its first frame inside
  120 s (none before) but does not reach the room: the regression is reduced,
  not closed.
- **Allocator lock (measured, not yet a fix on its own).** In optimized builds
  every host allocation still took the tracker's global recursive lock, which
  only the debug tracker needs. Without it, one cold 120 s run of the puzzle
  title spent 97 ms per graphics pipeline miss instead of 112 ms and finished
  254 misses instead of 230.
- **Open:** first-boot time is still bounded by single-threaded compilation
  on the command-processor thread.
