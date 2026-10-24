# Kyty Bring-Up Manual (extended)

This is the extended strict-frontier manual: investigation loop, phase gates,
frontier history, and the auxiliary-agent handoff template. Consult it when
advancing the strict PS5 compatibility frontier. Day-to-day rules and
invariants live in the repository root `AGENTS.md`, which takes precedence for
process weight (do not apply this full loop to ordinary code tasks).

Note: frontier facts below are a snapshot — always re-capture the first strict
fail on the current HEAD before acting on them.

## Where records live

Per-title fixes, evidence, exclusions and open items live in `docs/titles/`;
cross-title records live in `docs/bringup/`. Each record keeps its original
heading, so a reference to a section by name resolves through this index.
Record a new finding in the file of the title that exposed it; a finding that
spans titles goes in `docs/bringup/cross-title.md`.

- [Role-playing title](titles/role-playing.md)
  - Role-playing title: blue traveler map from geometry-pass constants (2026-10-22)
  - Role-playing title: GPU hang in the story prologue (2026-10-21, open)
  - Role-playing title: story starts, cube depth samples and recycled ranges (2026-10-21)
  - Role-playing title: lost depth clears, display-buffer samples and stale covers (2026-10-21)
  - Role-playing title: Standard256B UI images ignore the descriptor pitch (2026-10-20)
  - Role-playing title: render-target storage aliases and a clean movement run (2026-10-20)
  - Role-playing title: 3D storage growth and the remaining UI defects (2026-10-20)
  - Role-playing title: storage, DCC and UI format gaps (2026-10-20)
  - Role-playing title: per-frame fills refused by partial buffer views (2026-10-20)
  - Role-playing title: the grading volume lost its blue axis (2026-10-20)
  - Role-playing title: merged geometry stage fills the grading volume (2026-10-20)
  - Role-playing title: story exposure and colour grading (2026-10-20)
  - Role-playing title: world vertex inputs read the wrong data (2026-10-20)
  - Role-playing title: the environment cube read its cleared storage alias (2026-10-20)
  - Role-playing title: mipmapped storage arrays and cube chains (2026-10-19)
  - Role-playing title: selection screen, story entry and stop fixes (2026-10-19/20)
  - Role-playing title: story start, three stops removed (2026-10-17)
  - Role-playing title: wave64 resolve pass under paired transport (2026-10-17)
  - Role-playing title menu defect (2026-10-08, deferred; not gameplay)
  - UE4 JRPG startup frontier (2026-10-07, not gameplay)
- [Robot platformer](titles/robot-platformer.md)
  - Robot platformer: logo particles through a split geometry pass (2026-10-22)
  - Robot platformer: paired compute compile cost (2026-10-21, closed 2026-10-22)
  - Robot platformer: worldmap stall and dark top-left quadrant (2026-10-17)
  - Robot platformer: intro frame cost on Intel (2026-10-18)
  - Robot platformer: title screen save cards (2026-10-14, open)
  - Robot platformer: vertex attribute format words (2026-10-14)
  - Robot platformer: exposure fill and DCC metadata clears (2026-10-14)
  - Robot platformer: Gen5 dispatch TUNNEL_ENABLE (2026-10-13)
  - Robot platformer: 64-entry shader texture table (2026-10-13)
  - Robot platformer: storage image over a sampled and a buffer view (2026-10-13)
  - Robot platformer: pixel GDS append under the launch EXEC (2026-10-13)
  - Robot platformer: image atomic swap with a GLC return (2026-10-13)
  - Robot platformer: Wave32 whole-wave OR reductions (2026-10-13)
  - Robot platformer: wave32 lane masks are one dword (2026-10-13)
  - Robot platformer: SDWA low destination selects (2026-10-13)
  - Robot platformer: raw texels between aliased depth and colour views (2026-10-13)
  - Robot platformer: whole-wave OR reductions as partial waves (2026-10-13)
  - Robot platformer: worldmap admission chain (2026-10-13)
  - Reference workload runs d58-d96: stable 266 s, no fatal, still black (2026-10-04)
- [Insect platformer pair](titles/insect-platformers.md)
  - Platformer pair: intro cinematics were black (2026-10-17)
  - Platformer pair: remaining device-address waits and boot time (2026-10-17)
  - Platformer pair: unreadable glyphs were dropped command-buffer packets (2026-10-17)
  - Platformer pair: open defects seen after the glyph fix (2026-10-17)
  - Platformer pair: index range over a depth target (2026-10-16)
  - Insect sequel: intermittent depth/index overlap (2026-10-16, open)
- [Sandbox title](titles/sandbox.md)
  - Sandbox title: snapshot comparisons in the world (2026-10-24, improved)
  - Sandbox title: GPU hang on the first world frame (2026-10-23, fixed)
  - Sandbox title: shadow cascades bound as slice ranges (2026-10-23, fixed)
  - Sandbox title: drifting dark patch on terrain (2026-10-16, open)
  - Sandbox title: missing terrain in the world (2026-10-15, fixed 2026-10-16)
- [Survival title](titles/survival.md)
  - Survival title: hosting an offline world (2026-10-15, open)
- [Fishing title](titles/fishing.md)
  - Fishing title: gray rectangles behind the first dialogue portrait (2026-10-23, checked)
  - Fishing title: command-processor CPU costs (2026-10-14, partly fixed)
- [Racing title](titles/racing.md)
  - Racing title on AMD: corruption from a degraded GPU, not from code (2026-10-17)
  - Racing title race-window correction (2026-10-11)
- [Card title](titles/card-title.md)
  - Card title: streamed music and an offline service retry (2026-10-16)
- [Preschool Unity title](titles/preschool-unity.md)
  - Preschool Unity title: unreadable EUD snapshot (2026-10-21, open)
  - Preschool Unity title: sky-blue speckles on every outline (2026-10-19)
- [Pixel cave platformer](titles/pixel-cave-platformer.md)
  - Pixel cave platformer: compute fill cost on Intel (2026-10-19)
- [Puzzle title](titles/puzzle-title.md)
  - Puzzle title on AMD: two causes of the gfx ring timeout (2026-10-17)
- [Anime fighter](titles/anime-fighter.md)
  - Anime fighter: Vulkan validation errors (2026-10-23, fixed)
  - Anime fighter: excluded, one submission order across queues (2026-10-23)
  - Anime fighter: black boxes behind glyphs were a capture artifact (2026-10-23)
  - Anime fighter: excluded, submit without waiting for decode (2026-10-23)
  - Anime fighter: write-back restores protection once per run (2026-10-23)
  - Anime fighter: async round-trip latency (2026-10-23)
  - Anime fighter: write-back copies only GPU-flagged pages (2026-10-22)
  - Anime fighter: storage write-back on the label path (2026-10-17)
- [First-person puzzle title](titles/first-person-puzzle.md)
  - First-person puzzle title: scalar-join sources per label (2026-10-22)
  - First-person puzzle title: cold shader compiles, not a GPU stall (2026-10-17)
- [Beat 'em up](titles/beat-em-up.md)
  - Beat 'em up: device-address write-back waits (2026-10-14, open, measured)
- [Mechanical-fist platformer](titles/mechanical-fist-platformer.md)
  - Mechanical-fist platformer startup frontier (2026-10-08, not gameplay)
- [Roguelike platformer](titles/roguelike-platformer.md)
  - A roguelike reaches gameplay (2026-10-07)
- [2D Unity action title](titles/2d-unity-action.md)
  - 2D Unity action title: native plugin waits through `_umtx_op` (2026-10-22)
- [Cross-title records (2026-10-10 onward)](bringup/cross-title.md)
  - Transient snapshot pool: covering vertex snapshots and expiry (2026-10-24)
  - Fishing title CP cost per draw and an excluded usage memo (2026-10-24)
  - Compute queues on a compute-only family (2026-10-24)
  - Early submissions at least half a millisecond apart (2026-10-24)
  - A draw after a dispatch starts a new command buffer (2026-10-23)
  - Guest-window searches skip occupied spans (2026-10-23)
  - Proven one-word buffer fills recorded as transfer fills (2026-10-23)
  - Crash evidence read only for a reported fault (2026-10-23)
  - Direct-memory windows imported as pinned dma-bufs (2026-10-23)
  - Populated direct-memory pages read from the backing's extents (2026-10-23)
  - `sceKernelSleep` sleeps whole seconds (2026-10-23, found by inspection)
  - Stencil test without a stencil format (2026-10-23)
  - Collector context requests delivered only at safe points (2026-10-23)
  - Transient snapshots shared by a command pool's command buffers (2026-10-23)
  - A graphics submit returns after its batch is decoded (2026-10-23)
  - Compute ring runs copied in bytes across the ring end (2026-10-23)
  - Decoded video frames delivered after a decode error (2026-10-22)
  - Recorded work submitted while the GPU queue is idle (2026-10-22, corrected 2026-10-23)
  - Per-draw shader resource scans memoized by layout and instruction class (2026-10-22)
  - Host-only UCONFIG codes in indirect register blocks (2026-10-22)
  - Depth array slices addressed and sampled as arrays (2026-10-22)
  - Color CMASK fast clears and the eliminate pass (2026-10-22)
  - Shader decoder refuses unimplemented encodings (2026-10-22)
  - Output modifiers on vector results (2026-10-22)
  - Command processor cache events and acquires decoded generally (2026-10-22)
  - SW_64KB_R_X addressed by surface coordinates, 16-byte elements (2026-10-22)
  - HLE trace lines formatted only when emitted (2026-10-22)
  - Relocation protection round trips at module load (2026-10-22)
  - Depth-reference samples over a storage-detiled view (2026-10-22)
  - Shader translator: GDS writes in split and native compute waves (2026-10-22)
  - Shader translator: paired wave64 layout removed (2026-10-22)
  - Shader caches: whole-title SPIR-V store and versioned compute recipes (2026-10-22)
  - Shader translator: split geometry pass and compute row DPP (2026-10-22)
  - Shader translator: float contraction and coherent loads (2026-10-22)
  - Depth sampling: integer reads and dropped pending clears (2026-10-22, found by inspection)
  - EXECZ skip over a join shared with an outer branch (2026-10-20)
  - Descriptor churn and frame hitches on Intel (2026-10-19)
  - Open: WAIT_REG_MEM poll address 1 crashes the command processor (2026-10-17)
  - Index snapshots over linked surfaces (2026-10-17)
  - Same-queue storage published on the device before guest-address reads (2026-10-17)
  - Review findings recorded for later (2026-10-17)
  - Uncompressed standard 64 KiB mip chains (2026-10-16)
  - Pixel ADD_TID spill slots across control flow (2026-10-16)
  - Compares that overwrite their own scalar source (2026-10-16)
  - IME panel text without an open panel (2026-10-16)
  - Four-byte render-target micro tile (2026-10-16)
  - Unread vertex snapshot tails (2026-10-16)
  - Index snapshots over idle writable storage (2026-10-16)
  - Materialization-cache clean ranges per draw (2026-10-16)
  - Descriptor-set recycling and deferred-release drain in dense gameplay (2026-10-16)
  - Vertex shader identity limited to attribute dwords the program loads (2026-10-16)
  - Transient snapshot entry ceiling in dense passes (2026-10-16)
  - Service plugins identified by their declared module name (2026-10-16)
  - Image reads through an all-zero descriptor (2026-10-16)
  - Alpha-only stores to one-channel storage images (2026-10-16)
  - Physical ranges withdrawn while device addresses are prepared (2026-10-16)
  - Read-only snapshots over written-back writable buffers (2026-10-15, excluded)
  - Save data directory search patterns (2026-10-15)
  - Guest thread-pointer loads without the TLS handler lock (2026-10-15)
  - Conditional command-buffer branch (2026-10-14)
  - Command-processor per-draw costs: armed ranges and descriptor reuse (2026-10-14)
  - Static pointer-table resource plans: no measured gain (2026-10-14, excluded)
  - Font library: guest outline rasterization (2026-10-14)
  - Runtime-built typed buffer descriptors (2026-10-12)
  - Command-processor costs after render pass continuation (2026-10-12)
  - Render pass continuation across draws (2026-10-12)
  - Depth-only compute HTILE clears (2026-10-12)
  - Render-target demotion under the RenderTargets scope (2026-10-12)
  - Offscreen render-target scaling, opt-in (2026-10-12)
  - Bounded descriptor-set cache (2026-10-12)
  - FSR 1.0 presentation upscaling (2026-10-12)
  - Indexed physical mapping lookup (2026-10-12)
  - Shared physical device-address windows (2026-10-12)
  - Native wave32 compute in whole host subgroups (2026-10-12)
  - Physical device-address imports in whole blocks (2026-10-11)
  - Guest device-address imports and per-submit cost on RADV (2026-10-11)
  - Range-query cache invalidation through compact spans (2026-10-11)
  - Table-loaded comparison samplers paired by instruction (2026-10-11)
  - Dynamic scalar-load spans memoized per program (2026-10-11)
  - Instruction type names resolved once (2026-10-11)
  - Materialization range scans per draw: excluded (2026-10-11)
  - Index buffer objects through the mapping: excluded (2026-10-11)
  - AJM batch input budget (2026-10-11)
  - AJM combined resample markers (2026-10-11)
  - Vertex buffer objects without a staging fence (2026-10-11)
  - Atomic container reference counts (2026-10-11)
  - Vertex snapshots from the first fetched record (2026-10-11)
  - Per-draw scan costs (2026-10-11)
  - Descriptor-set reverse index (2026-10-11)
  - Transient tail clearing (2026-10-11)
  - Range-filtered label fence snapshots (2026-10-11)
  - Per-program shader resource analyses (2026-10-11)
  - Materialization range invalidation (2026-10-11)
  - Dirty-tracker identities revoked by unmap (2026-10-11)
  - Physical view index and residency sweeps (2026-10-11)
  - Per-draw vertex-stream reader scan (2026-10-11)
  - Guest physical memory on host-pointer-restricted drivers (2026-10-11)
  - ATRAC9 decoder checkpoint (2026-10-11)
  - Contract-closure checkpoint (2026-10-10)
- [Cross-title history (through 2026-10-09)](bringup/history.md)
  - Dirty-page lookup and admission during a camp transition (2026-10-09)
  - Contract-closure validation checkpoint at translator127 (2026-10-09)
  - Historical contract-closure validation checkpoint at translator126 (2026-10-09)
  - Interop integration and single-precision MODE contracts (2026-10-09)
  - Classic runtime and input boundaries (2026-10-08)
  - Bounded secondary blend translation (2026-10-08)
  - Shared runtime and resource contracts (2026-10-08, bounded validation)
  - Portable resource completion (2026-10-08)
  - Flexible snapshot lifetime and standard math (2026-10-08)
  - Publication selection and held contracts (2026-10-09)
  - Loading regression under expanded dirty-page admission (2026-10-09, unresolved)
  - Four-front verification (2026-10-08, bounded; acceptance still open)
  - Open source-audit findings (2026-10-08)
  - Recursive mutex condition waits (2026-10-08)
  - Publication regression containment (2026-10-08)
  - Publication validation snapshot (2026-10-08, startup and short routes)
  - Publication blockers from the expanded inventory (2026-10-08)
  - Implicit sampling preserves volume coordinates (2026-10-07)
  - Two Unity titles reach their menus and gameplay (2026-10-07)
  - Integration tests link again (2026-10-06)
  - Storage uploads and physical residency (2026-10-06, guest verified)
  - CPU/GPU overlap and per-draw cost (2026-10-05, guest verified)
  - Performance, new-title repairs and add-on content (2026-10-04, guest verified)
  - Title runtime repairs (2026-10-04, guest verified on seven of eight titles)
  - NGG passthrough front width neutrality (2026-10-03, CPU verified; strict run advances)
  - Writable 3D images, linear volumes, fragment mask flow and the full vertex front (2026-10-03, CPU verified; strict run stalls)
  - First-draw metadata contract (2026-10-03, observation only)
  - Vertex-wave admission frontier (2026-10-03, passthrough program captured)
  - Export operand-tail correction (2026-10-03, CPU and strict advancement verified)
  - Integrated contract corrections (2026-10-02, intermediate evidence)
  - Pad, keyboard and mouse record contracts (2026-10-03, CPU verified, not gameplay)
  - Debt resolution and compute-wave contracts (2026-10-02, CPU verified, not gameplay)
  - Shader float-mode bits (2026-10-02, partial implementation)
  - Arithmetic lowering and cached-module validation (2026-10-01, not gameplay)
  - Fragment wave compiler (2026-09-29, compiler and GPU replay verified)
  - Cold compute compilation capacity (2026-09-29, unresolved)
  - Per-array descriptor layouts (2026-09-29, strict compile progress)
  - Immutable resource-index specialization (2026-09-30, strict compile progress)
  - Descriptor-array feature admission (2026-09-29, unresolved)
  - Loaded image resource lifetime (2026-09-29, compiler verified)
  - Paired wave primitives (2026-09-29, GPU replay verified)
  - Vulkan 1.4 baseline (2026-09-29, opening image verified)
  - Dispatch address preparation timing (2026-09-29, native verified)
  - Compute wave-analysis preparation cost (2026-09-29, bounded verified)
  - Compute dispatch input preparation (2026-09-29, bounded measured)
  - Metadata descriptor liveness preparation (2026-09-29, bounded measured)
  - Sparse physical residency query (2026-09-29, not gameplay)
  - Exercised custom pixel interpolation gap (2026-09-29, unresolved)
  - Storage write-back dependency retry (2026-09-29)
  - Register-default lookup and context restoration (2026-09-29)
  - Title-transition admission (2026-09-29, not gameplay)
  - Image atomic and class translation (2026-09-29, compiler verified)
  - Integer value references (2026-09-29)
  - Zero-LOD HDR sample observation (2026-09-29, not gameplay)
  - JSON value lookup and assignment (2026-09-29, not gameplay)
  - Unsupported-swizzle diagnostic (2026-09-29, corrected 2026-10-02)
  - Mixed sampled-image numeric selection (2026-09-29, compiler verified)
  - Residency preparation cost (2026-09-29, not gameplay)
  - Pixel NULL export admission (2026-09-29, not gameplay)
  - Media file replacement callbacks (2026-09-29, not gameplay)
  - Async DMA argument layout and counter reset (2026-09-29, not gameplay)
  - Physical aliases across CPU protection boundaries (2026-09-29, not gameplay)
  - Guest-address imports creating their own residency (2026-09-29, not gameplay)
  - Raw render alias composition and explicit mip fetch (2026-09-29, not gameplay)
  - Scalar and vector lowering, RGBA16 storage, and mixed image ownership (2026-09-29, not gameplay)
  - Depth mip storage views, D16 arrays, and paired compute frontier (2026-09-28, not gameplay)
  - Bounded storage-image overwrite and next mixed alias (2026-09-28, not gameplay)
  - Strict compute/storage and libc string frontier (2026-09-28, not gameplay)
  - Generic wave64 compute frontier (2026-09-26, not gameplay)
  - Paired compute admission frontier (2026-09-22, not gameplay)
  - Latest startup shader frontier (2026-09-22, not gameplay)
  - Process allocator startup (bounded repair, not gameplay acceptance)
  - Nested arenas and AudioOut2 startup (bounded repair)
  - Original reference workload

## Mission

Kyty is an experimental PlayStation 4 and PlayStation 5 emulator. This fork is
bringing the PS5 path from early execution to correct, interactive rendering
while preserving a design that can run on macOS, Linux, and Windows and on AMD,
Intel, NVIDIA, and Apple GPUs.

Accuracy comes before superficial progress. A frame that is merely non-black,
a process that survives because unsupported behavior was ignored, or a build
that succeeds without exercising the runtime is not compatibility.

## Non-negotiable invariants

1. **Evidence before code.** Reproduce the problem, identify the first strict
   failure, and trace the bad state to its producer before editing.
2. **Never invent guest behavior.** Do not guess NIDs, ABI signatures, packet
   layouts, register meanings, formats, tiling, alignments, or return codes.
3. **One behavior, one implementation.** Direct and indirect encodings of the
   same GPU state share a decoder. Resource sizes, offsets, and pitches come
   from one layout model consumed by every caller.
4. **No behavioral fallbacks.** Never continue with assumed RGBA8, linear
   tiling, default success, skipped state, placeholder shaders, or fabricated
   resources. Unsupported behavior fails with enough evidence to implement it.
5. **Capability-driven rendering.** Select Vulkan strategies from features,
   limits, formats, queues, and extensions. Vendor IDs are diagnostic data, not
   policy switches.
6. **Keep platform code at the boundary.** OS-specific memory, exceptions,
   threads, windows, surfaces, controllers, and dynamic loading stay in focused
   platform modules. Guest HLE and GPU semantics are platform-neutral.
7. **Do not regress the working frontier.** Preserve existing execution,
   rendering, input, and build behavior unless a test proves that behavior is
   itself incorrect.
8. **Report reality.** Distinguish verified behavior, captured evidence,
   hypotheses, and untested assumptions in code reviews and handoffs.

## Investigation and advancement methodology

This section is the practical operating system for agents and humans advancing
the PS5 path. It does not replace the invariants above; it explains *how* to
obey them day to day. Speed without this loop is noise.

### Core principle: one frontier, one failure, one hypothesis

Compatibility is a chain. The only unit of progress is **advancing the first
strict failure** while preserving everything behind it. Parallel “also fix”
branches, multi-hypothesis edits, and opportunistic refactors inside an open
failure destroy the signal that the next run is comparable.

Before any code change, answer out loud (or in the session handoff):

1. **What is the current verified frontier?** (last place the title runs without
   diagnostic flags)
2. **What is the first strict failure after that frontier?** (exact file, line,
   values)
3. **What single producer created the bad state?** (encoder, parser, HLE ABI,
   layout, resource update—not only the assertion that fired)
4. **What one falsifiable hypothesis will I test next?**
5. **What evidence would prove the hypothesis wrong?**

If any answer is missing, investigate first. Do not edit.

### How plans are formed

Plans are ordered checklists, not wish lists:

1. **Baseline** — clean tree, known HEAD, green focused tests, working build.
2. **Reproduce** — strict run of the private fixture; capture the *first* fail
   completely under an untracked scratch directory.
3. **Classify** — HLE/ABI, PM4 encode, PM4 parse, surface layout, GPU memory
   relation, shader, VideoOut, sync/label, or host Vulkan.
4. **Hypothesize** — one cause, expected packet/state delta, success criterion.
5. **Test-first** — smallest deterministic fixture or unit test that fails for
   that cause.
6. **Minimal implement** — only the behavior the test (and capture) require.
7. **Verify** — focused green, then strict re-run; expect the same or a *later*
   frontier.
8. **Commit or revert** — commit evidenced behavior; revert failed experiments
   before trying the next hypothesis.
9. **Handoff** — frontier note: previous fail, new fail or checkpoint, residual
   hypothesis, no private paths.

Do not plan modularization, performance campaigns, or multi-NID sweeps while a
strict post-Play (or earlier) blocker is open. Delivery order below is absolute.

### Focus before advancing

- **Stay on the first failure.** Logs after a crash are often wreckage, not
  new work items.
- **Name the seam.** Touch only the module that owns the bad contract
  (e.g. packet encoder vs CP parser vs GpuMemory update). Cross-cutting drive-bys
  are out of scope for that cycle.
- **Prefer producers over symptoms.** A null WaitRegMem address is fixed by
  finding who should write the address (guest patch HLE, adjacent ReleaseMem,
  encoder contract)—not by making the waiter skip null.
- **Freeze working behavior.** If menu reach, flips, or focused tests regress,
  stop and undo before inventing a second fix.
- **Reject “make it continue” patches.** Silent success, assumed RGBA8, linear
  tile, or fabricated labels without a documented encode/execute contract are
  not progress.

### Investigation loop (read-only until evidence is enough)

1. **Baseline comparison**
   ```bash
   git status --short
   git log -5 --oneline --decorate
   ninja -C _build_macos fc_script
   ```
2. **Strict reproduce** (no `KYTY_BRINGUP_*`, no legacy stub/permissive flags):
   ```bash
   _build_macos/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
   # Optional silent runner for speed; record that logging was Silent.
   ```
3. **Capture the full fail** outside Git: message, file:line, PM4 header/body,
   register values, submit id / command offset when available, and whether
   AUTO_CROSS or other diagnostics were set (diagnostics never count as
   acceptance).
4. **Map encode → execute.** For GPU issues, locate:
   - HLE builder in `Graphics.cpp` (guest call, arguments, returned pointer)
   - Packet dwords at encode time
   - Whether the guest patches later (`GetDataPacketPayloadAddress`, EopPatch,
     direct stores through the returned header)
   - CP parser in `GraphicsRun.cpp` and the values at execute time
5. **Use Silent vs Console deliberately.** `PrintfDirection = Silent` is for
   wall-clock and long runs; it hides HLE prints. For encode/patch sequences,
   use Console or temporary `stderr` probes that do not depend on Printf, then
   **delete probes before commit**. To surface the diagnostic dumps (shader
   dumps, command-processor traces) that are compiled as `KYTY_LOG_DEBUG`, set
   `PrintfLevel = Debug` in the config; `Error`/`Warn`/`Info`/`Debug` mirror the
   severity gates used by other emulators. The gate is a single relaxed atomic
   read, so a level that does not pass costs nothing.
6. **Compare working vs failing forms.** Same export with non-null address
   earlier in the run is ABI evidence; a post-Play null pair is a different
   contract to explain, not a free pass to invent addresses.
7. **Consult references only for names and patterns.** Public references may
   inform vocabulary and architecture, but every PS5-specific claim must
   reappear in a local capture or test. No incompatible code paste.

### Hypothesis and trial-and-error discipline

Trial and error is allowed; **uncontrolled** trial and error is not.

| Rule | Practice |
| --- | --- |
| One variable | Change a single contract (offset, accepted bit, opcode case). |
| Falsifiable | “If PayloadAddress returns cmd+1 for WaitMem, guest patches non-null.” |
| Time-box | If the capture does not move after a clean experiment, stop and re-classify. |
| Revert failed experiments | Do not stack dead ends; `git diff` should only show the live hypothesis. |
| Record negatives | “Return payload not header breaks SizeDw” is permanent evidence. |
| Prefer structure over silence | Unknown stays `EXIT` / guest error with body dump, not success. |

When a hypothesis fails, write one line: *hypothesis, observation, next
hypothesis*. Then remove the code from that experiment before coding the next.

### Debug and execution tactics

- **First failure only.** Fix order is boot → logos → menu → Play/load →
  gameplay. Never jump ahead because a later log line looks interesting.
- **Encoder dump vs CP dump.** Print or test packet dwords at HLE return *and*
  at parser entry; many bugs are “guest never patched” vs “parser wrong layout.”
- **Adjacent packets.** ReleaseMem/WaitRegMem, WaitFlipDone neighbors, and
  SizeDw residual registers (pointer arithmetic at ±packet size) often explain
  deferred address fills.
- **Return-value contracts.** Gen5 builders often return the **packet header**
  for SizeDw / EopPatch. Returning a mid-packet payload pointer can “fix” a
  store test and break SizeDw—prove return use from capture.
- **Thread races.** Render-thread IndexBuffer update vs CP-thread WaitRegMem
  can both be real; fix the first process exit, then re-run for the next.
- **Input.** `KYTY_AUTO_CROSS` is discovery only. Acceptance needs real edges
  or an explicitly recorded non-claim.
- **Performance.** Never compare FPS under Console logging to Silent; record
  logging mode, resolution, and shader-cache state.
- **Scratch evidence.** Save logs under untracked `_scratch_playable/` or a
  session scratch dir. Never commit guest paths, title IDs, screenshots, or raw
  multi-megabyte logs.

### Red → green → strict

For every semantic change:

1. **Red** — add or extend a unit test with sanitized PM4/HLE args that fails
   for the missing contract.
2. **Green** — implement the minimum; focused filter passes.
3. **Strict** — rebuild `fc_script`, rerun the private fixture, confirm the
   old fail is gone and no earlier fail returned.
4. **Regress** — re-run GraphicsPackets/GraphicsState (and other touched
   suites). Unfiltered full suite may include a historical date-dependent test;
   do not use it to hide new failures.
5. **Commit** — message describes emulator behavior only
   (`fix(graphics): …`), no fixture identity.

If strict does not advance, the change is not done—even if unit tests pass.

### What “done” means for one cycle

A cycle is complete when **either**:

- **(A) Frontier advanced:** the previous first fail no longer occurs under
  strict flags; a new first fail or a stable checkpoint is captured; focused
  tests pass; change is committed; or
- **(B) Blocker documented:** evidence is insufficient to implement without
  inventing; the fail remains structured and informative; capture and
  residual hypothesis are recorded; temporary probes are removed.

“Process survived” or “non-black frame once” without a strict, flag-free path
is not done.

### Anti-patterns (do not do these)

- Editing while the fail is unreproduced on the current HEAD.
- Multiple hypotheses in one commit.
- Keeping a failed experiment “just in case.”
- Broad renames/refactors on an open compatibility blocker.
- Vendor or OS branches inside guest decode/layout.
- Claiming playability with AUTO_CROSS, stubs, or permissive GPU skips.
- Leaving permanent dual implementations or feature-flagged legacy paths.

### Multi-title bring-up

When switching private fixtures (or adding a second root):

1. Keep each title’s root in an **env var only** (`$KYTY_GUEST_ROOT` or a second
   untracked env). Never write absolute private paths into tracked files.
2. Re-capture the first strict fail for **that** title; do not assume the other
   title’s frontier applies.
3. Prefer HLE exports that are **named and sized** (measure APIs, standard libc)
   before open-ended stubs. Unknown Share/Ampr NIDs may log arguments and return
   success only when that is the smallest way to reach the next evidenced fail—
   document residual name/ABI debt in scratch, not as playability claims.
4. After any dependency bump (SDL, etc.), re-run **both** focused unit filters
   and the primary title’s strict path before claiming no regression.

### Vendored dependency bumps

- Prefer official upstream releases into `source/3rdparty/` behind the existing
  CMake wrappers (static SDL, etc.).
- **One high-impact dep at a time** (SDL first). Rebuild `fc_script`, focused
  tests, then the primary strict fixture.
- Hold Vulkan headers and other ABI-sensitive pins unless MoltenVK/runtime is
  revalidated. The host layer uses the vendored static SDL 3.4 (SDL2 was
  removed); SDL environment variables use the SDL3 names (`SDL_AUDIO_DRIVER`,
  `SDL_AUDIO_DISK_OUTPUT_FILE`, `SDL_VIDEO_DRIVER`).
- Commit messages: host/build behavior only (e.g. `build(host): migrate the
  host layer from SDL2 to SDL 3.4`), never private title names.

## Current status and verified frontier

The latest regression sweeps below are the frontier a strict run must not
fall below. Per-title state is in the files listed under "Where records
live".

### Regression sweep for the 2026-10-19/20 role-playing changes (2026-10-20)

- **Setup.** Intel Arc A770, Mesa ANV 26.2.5, strict bounded runs with the
  scripted routes, one instance at a time, temporary diagnostics removed.
- **Eight-title set.**
  - All eight ran their whole window without a fatal stop: the card game
    (150 s), the bug platformer (200 s), the racing title (200 s), the
    fishing title (200 s), the roguelike platformer (120 s), the watercolor
    platformer (120 s), the well platformer (100 s) and the preschool title
    (90 s).
  - Final captures render as in the previous sweep; the differences are
    route positions.
  - A repeat of the racing title exited cleanly (status 0, no error) after
    107 s while racing. The fishing title showed the same silent exit once
    before these changes. The cause is not known.
  - The racing route reaches the race at about 104 s, as in the
    2026-10-19 09:22 run. One earlier run reached it sooner by another menu
    path.
- **Role-playing title.** Twelve story runs on 2026-10-20, across builds
  with and without the alias lookup change:
  - Four held 300 s and reached the ship scene; the one on the final tree
    ran it at about 16 fps.
  - Five lost the device.
  - One stopped on the stale boot target (see "the environment cube read
    its cleared storage alias").
  - One stopped on `unsupported sampler coordinate lowering`
    (`GraphicsRenderPipeline.cpp`) for an S# with mirror-clamp modes
    (`clamp=6,7,6 mirror_clamp_enabled=1`, words `5e6f15be 00000032
    0000000d 08700f00`). Open: that lowering is not implemented.
  - One crashed inside a temporary dump that read a retired image.
- **Gates.** `check_emulator_boundaries.py source --strict` and the
  graphics-table, boundary-fixture and detile-SPIR-V ctest gates pass. The
  focused graphics unit suites pass (896 tests).
  `kyty_playable_regression.py` cannot run on this host: it has no private
  profile or guest root.

### Main regression sweep after the #18 merge (2026-10-17)

Build: `main` at 71814344, strict runs, one title at a time, scripted input
routes, 100-240 s each. Hosts: Intel Arc A770 (Xe KMD, Mesa) and the AMD
Raphael iGPU (RADV, 2 CUs). Frame rates are frames between two native
captures divided by the seconds between them, taken in the gameplay window;
single `status` samples are instantaneous and swing widely (an earlier
"86 fps" for the atmospheric platformer was such a sample; its gameplay
window averages 59-61 fps on Intel and 30-31 on AMD on both builds).

No title regressed against its previous strict run. Reached gameplay on
Intel: card title, platformer pair, racing title, Unity fishing title, beat
'em up, sandbox title, roguelite action platformer, watercolour platformer,
isometric roguelike, gothic action platformer, dream platformer, zoo title,
preschool Unity title, pixel exploration title, artillery title, roguelike
dungeon crawler and tower-defence title. The ninja platformer's generic route
stops at its title screen, the anime fighting title stays in its story intro
and the role-playing title reaches the traveler selection, as before.

Open defects found by the sweep, none of them caused by the merge:

- Intermittent early stall (both hosts). Presents stop with no fatal error
  and `phase=stalled`: the ninja platformer on Intel at frame 3812, the
  gothic action platformer on AMD at frame 391. The rerun of each reached
  its usual state (2 stalls in 45 runs). No live evidence yet: the harness
  queried the agent only after exit. Next evidence: `threads`,
  `sync-waits` and `events` taken while `phase=stalled`.
- Gothic action platformer, AMD only: every capture is black for 240 s while
  the title runs at 53-64 fps in `phase=interactive`; one capture shows a
  single large grey triangle. Intel shows gameplay on the same route. First
  AMD run of this title, so there is no AMD baseline. Evidence so far:
  both hosts present `B8G8R8A8_SRGB` 1920x1080 and the AMD frames are RGB
  zero, so readback is not the cause. The grey triangle is a flat white
  triangle with one vertex at the screen centre and two at the right corners,
  a fullscreen pass with one collapsed vertex. The translated SPIR-V of every
  shader is identical on both hosts except for baked guest-address
  constants, so the difference is in how the host runs the same module.
  `RADV_PERFTEST=gewave32` and `gewave32,pswave32,cswave32` kept the screen
  black (stages whose subgroup size Kyty pins to the guest wave are not
  affected by that variable), but those runs came after the gfx ring timeouts
  described below, so this exclusion is void until it is repeated on a
  freshly booted host. The geometry shader synthesizes the
  guest NGG wave info (`s3`) and `exec` from the host subgroup ballot; next
  evidence: the fullscreen vertex shader's per-lane `gl_VertexIndex`,
  `gl_SubgroupInvocationID` and output position on AMD for one draw.
- Excluded: the artillery title's white and blue terrain on AMD is a
  randomly generated arctic map (falling snow, different team names), not a
  colour error; the Intel run drew a stone map. Compare like for like before
  treating a terrain difference as a defect.
- Puzzle title and role-playing title, AMD only: device lost
  (`vkQueueSubmit` result -4) on queue 8. On Intel the puzzle title stalls at
  boot frame 332 without a capture, as in all earlier runs.
  Update 2026-10-17: the puzzle title's two causes are fixed; the
  role-playing title's timeout is still open (see "Puzzle title on AMD: two
  causes of the gfx ring timeout" at the top of this manual).
- **AMD runs after a gfx ring timeout are invalid until the host reboots.**
  - What happened: the kernel log shows six `ring gfx_0.0.0 timeout` events,
    each followed by a ring reset and "device wedged, but no recovery needed".
    Five came from the puzzle title's run (05:35Z) and one from the
    role-playing title's run (05:38Z), the two device-lost runs above.
  - What followed: every later AMD run rendered wrongly while the title kept
    running. The card title, clean at 45-51 fps before the timeouts, gave
    only black captures at about 78 fps. The atmospheric sequel, which
    reached its start room cleanly before the timeouts, drew its intro, title
    card glyphs and first room as stretched, tilted polygons in two
    consecutive runs. Intel runs of the same binary in the same hour were
    clean.
  - The binary differed from the sweep build only by a diagnostic sink that
    is disabled without its environment variable, so the change is the host
    state, not the code. Confirmed on 10-17: after a full GPU reset the
    racing title rendered cleanly again with the same branch head.
  - Consequences for this sweep: the anime fighting title's AMD row was taken
    after the timeouts and must be repeated. So must the AMD runs behind the
    gothic platformer's `RADV_PERFTEST` exclusion. Its black screen itself
    predates the timeouts (05:16Z run).
  - Rule: before reading an AMD result, check
    `journalctl -k | grep 'ring gfx.*timeout'` since the last boot. Run titles
    that lose the device last in an AMD sweep, or alone.
- Role-playing title: a route that starts a new game now gets past the title
  and stops at a missing `ImageSampleCd` emitter (pixel stage) while loading.
  Its title screen background is flat magenta on Intel; earlier captures of
  that screen were dimmed by a loading overlay, so it is not known whether
  this is new.
- AMD routes: the input routes were timed on Intel, so on the slower AMD host
  the preschool Unity title ends in its pause menu, the tower-defence title in
  its name entry and the roguelike dungeon crawler in its dark first room.
  Their AMD frame rates are menu or intro rates, not gameplay.

## Architecture map

- `source/emulator/src/Loader/`: guest image loading, relocation, NID resolution,
  native-call trampolines, and exception integration.
- `source/emulator/src/Kernel/`: guest memory, direct/flexible allocation,
  pthreads, synchronization, files, and time.
- `source/emulator/src/Libs/`: HLE export registration and guest API contracts.
- `source/emulator/src/Graphics/Graphics.cpp`: PS4/PS5 command-buffer builders
  and AGC-facing exports.
- `source/emulator/src/Graphics/GraphicsRun.cpp`: PM4 parsing and normalized
  graphics-state updates.
- `source/emulator/include/Emulator/Graphics/HardwareContext.h`: normalized
  guest GPU state.
- `source/emulator/src/Graphics/GraphicsRender.cpp`: Vulkan resource binding,
  pipelines, draw/dispatch recording, and synchronization.
- `source/emulator/src/Graphics/ShaderParse.cpp` and `ShaderSpirv.cpp`: guest
  shader decoding and SPIR-V generation.
- `source/emulator/src/Graphics/Tile.cpp`: guest surface layout and addressing.
- `source/emulator/src/Graphics/Objects/`: Vulkan-backed resources and guest
  memory tracking.
- `source/emulator/src/Graphics/VideoOut.cpp` and `Window.cpp`: display buffers,
  Vulkan device/swapchain setup, and presentation.
- `source/lib/`: reusable host runtime, platform, threading, memory, filesystem,
  math, and script infrastructure.
- `source/unit_test/`: GoogleTest registration and deterministic fixtures.

Keep guest API decoding, guest GPU semantics, normalized state, Vulkan objects,
and host platform adapters conceptually separate even where legacy files still
contain more than one responsibility. Improve the seam being touched; do not
perform unrelated mass refactors.

## Auxiliary-agent handoff prompt

The following prompt is the canonical brief for an auxiliary agent. Give the
agent a private guest root through `KYTY_GUEST_ROOT`; never paste that path,
title identifiers, binaries, keys, save data, shaders, textures, screenshots,
or logs into tracked files or commit messages.

```text
You are a senior emulator/runtime engineer working inside the Kyty repository.
Your mission is to advance the strict PS5 runtime from the current controllable
gameplay frontier to stable, validated playability, then freeze that frontier
and only afterward perform carefully bounded modularization. Correctness,
evidence, portability, and preservation of working behavior outrank speed or
line-count reduction.

CURRENT FRONTIER

- Build works on Linux (`_build_linux`) and macOS (`_build_macos`); use the host
  you are on. Prefer Release + `PrintfDirection=Silent` for wall-clock.
- Vulkan device/swapchain, Gen5 shaders, indexed draws, VideoOut flips, logos,
  recognizable menu, Play/mode transitions, loading-card pixels, and
  controllable gameplay are exercised under strict flags. A reset 601-frame
  Release+Silent gameplay window reported 41.688 FPS with p50/p95/p99 frame
  times of 27/35/40 ms. Diagnostic controller input reached and moved through
  that scene; this proves the frontier, not formal acceptance or stable 60 FPS.
- In tree (do not regress): GpuMemory multi-parent (VB reclaim + surface link;
  Texture mixed parents; IndexBuffer-in-Texture link; WriteBack parent
  classify); GPU-owned RT layout preserve on Update; tile-27 size+4bpp detile;
  Gen5 EUD type-5; formats 14/29/56/71; multi-RT CB_SHADER_MASK; EXP Param5/6 +
  multi-MRT; structured SPIR-V loops; `v_cvt_i32_f32`; SDWA; SMEM dual-offset +
  variable SBuffer; image_sample dmasks 0x2/0x4/0xb; `ds_read2_b32`; null MRT
  discard tails; kill-enabled `EarlyZThenLateZ` late depth commit; NGS2
  extended max_voices.
- **First strict fail (re-capture on HEAD):** none observed through more than
  24,000 presents in the latest Linux Release+Silent strict run. The next
  structured EXIT or host fault is the process unit of work.
- **Visual frontier:** horizontal stripes and opaque sprite/prop rectangles are
  absent in a gameplay-era native capture. A bounded diagnostic route consumed
  sustained directional input for 180 presentations. Formal acceptance still
  requires a repeatable non-diagnostic controller route, one action beyond
  movement, stable presentation, and validation-clean output.
- **Later symptom only:** `ThreadFlag` bit `0x1` (mode `0x21`, 40 ms) with no
  observed Set. Never fabricate the signal. EventFlag live-handle registry
  (garbage → ESRCH) is not Set. Trace the producer after earlier GPU/shader
  aborts are gone.

IMMEDIATE OBJECTIVE AND SUCCESS CONDITION

Advance the strict post-Play path to **stable, correctly rendered 60 FPS
gameplay** without diagnostics or fabricated success. Formal acceptance must
use a real controller route, exercise movement plus another action, and remain
validation-clean. Process survival, diagnostic input, and HUD-only correctness
are not playability.

If dual-strict shows a process EXIT, that is first priority (GpuMemory, shader,
format, HLE). If the process survives but the world is wrong, treat that as the
rendering frontier: identify the first bad producer at the bound sample,
writer MRT, or consumer/composite boundary with capture evidence — do not
paper over it with permissive flags.

`ThreadFlag` remains deferred while earlier GPU/render issues dominate:

- one event named `ThreadFlag` is created with initial bits `0x0` in older
  captures;
- loading/wait mode `0x21` for bit `0x1` with 40 ms timeout was observed;
- do not Set the bit from Wait, timers, or the render loop.

PRIMARY ORDER OF WORK (DO NOT REORDER)

1. Reproduce the strict frontier with the current checkout and private fixture
   (`$KYTY_GUEST_ROOT` only; never name the title in commits).
2. Fix the first strict failure (re-capture on HEAD) with a documented
   hypothesis and a focused deterministic test or sanitized fixture.
3. Re-run strict execution and advance one failure at a time until the title
   reaches the first controllable gameplay scene under the playability table.
4. Prove real keyboard/controller press+release, movement both ways, one
   action, stable flips, correct geometry/colors, no device-loss, validation
   clean where available. Record Silent FPS + resolution + shader cache.
5. Freeze this working frontier with regression/characterization tests and a
   short evidence report (untracked scratch; no private paths in Git).
6. **Only after steps 1–5 pass and gameplay is reproducible from a baseline
   commit**, modularize oversized files one seam at a time. Every extraction
   must be behavior-neutral and must preserve the frozen gameplay evidence.
   Do not start modularization while a post-Play strict blocker is open.

PHASE GATES AND REQUIRED DELIVERABLES

Phase 0 — Baseline and reproducibility:

- Record HEAD, branch, `git status --short`, build result, focused test result,
  host GPU/capability summary, logging mode, resolution, and shader-cache state.
- Confirm no permissive/stub/trap-skip environment variable is active.
- Reproduce the loading frontier twice so a one-off race is not mistaken for a
  stable contract.
- Save all raw output beneath ignored scratch. The tracked report contains only
  sanitized facts, durations, counts, and source locations.

Phase 1 — Resolve the synchronization frontier:

- Map `ThreadFlag` creation to the guest call site and owning subsystem.
- Map every possible producer path to its HLE export, worker entry point,
  queue/command input, and expected `SetEventFlag` or equivalent completion.
- Capture thread start/exit and the last successful contract on the producer
  thread. The first earlier failure on that thread supersedes the wait timeout.
- Add a deterministic test for the evidenced missing contract before changing
  implementation. A generic EventFlag test alone is insufficient if EventFlag
  itself is behaving correctly and the producer never runs.
- Implement one semantic change and prove the signal now originates from the
  real producer. Record the next strict frontier.

Phase 2 — Reach and prove gameplay:

- Advance one strict blocker at a time through loading and scene creation.
- Use real keyboard/controller press and release edges for acceptance.
- Demonstrate a controllable character, movement in both directions, and at
  least one jump/attack/interact action while frames continue presenting.
- Inspect the scene for correct geometry, colors, texture interpretation,
  viewport/scissor behavior, and stable frame progression.
- Run with Vulkan validation where supported and record zero relevant errors,
  no device loss, no render-thread timeout, and no stuck GPU label.
- Measure performance only with silent function logging, fixed resolution, and
  recorded cache state. Do not make a target FPS claim from console logging.

Phase 3 — Freeze the working frontier:

- Add characterization tests for every compatibility seam required to reach
  gameplay, using sanitized packets/descriptors/ABI arguments only.
- Create a sanitized frontier report containing commit, commands, test counts,
  input sequence, frame/flip evidence, validation result, and performance
  conditions. Do not include the private fixture identity.
- Establish a baseline commit before any architectural extraction. If the
  strict scenario cannot be reproduced from that commit, the freeze is invalid.

Phase 4 — Architecture inventory:

- Measure files and functions, but classify them by responsibility, mutable
  state, ownership, threading, callers, dependencies, and existing tests.
- Current size signals include `ShaderSpirv.cpp` (~8,290 lines),
  `GraphicsRender.cpp` (~5,725), `GraphicsRun.cpp` (~4,521),
  `ShaderParse.cpp` (~3,473), `Shader.cpp` (~3,186), `Pthread.cpp` (~2,807),
  `Graphics.cpp` (~2,740), `Audio.cpp` (~2,716), `Window.cpp` (~2,539), and
  `GpuMemory.cpp` (~2,515). Recount before planning; these numbers are a
  snapshot, not acceptance thresholds.
- Produce an extraction table for each candidate: responsibility to move,
  proposed typed interface, inputs/outputs, owner, thread contract, error
  contract, mutable globals removed or retained, dependency direction,
  characterization tests, and strict-runtime verification command.
- Reject any boundary that cannot be described without generic `Utils`,
  `Common`, `Manager`, forwarding aliases, or bidirectional dependencies.

Phase 5 — Incremental modularization:

- Extract one cohesive responsibility per commit. Do not combine behavior
  changes with file movement or rename campaigns.
- Add characterization coverage first, move the implementation second, delete
  the old implementation in the same change, then rebuild and re-run gameplay.
- Preserve public behavior and one source of truth. Direct and indirect PM4
  paths must still share decoders; all surface consumers must still share one
  layout model; renderer policy must still depend on explicit capabilities.
- Revert an extraction if build, focused tests, menu, loading time, gameplay,
  input, frame output, validation state, or performance materially regresses.
- Update module documentation after each accepted extraction: purpose, public
  interface, invariants, ownership, thread safety, dependency direction, error
  behavior, and tests. Comments must explain contracts, not restate code.

NON-NEGOTIABLE RULES

- Read this entire AGENTS.md before editing. Do not weaken its invariants.
- Reproduce before editing. Capture the first strict error, packet/register
  values, submit ID, command offset, guest call path, and relevant state.
- Never invent a NID, ABI, structure layout, register meaning, tile mode,
  pitch, alignment, return code, or synchronization result. Triangulate from
  guest evidence, local call sites, upstream references, and a test.
- Never use `KYTY_BRINGUP_MODE=unsafe`, trap skipping, default success, assumed
  RGBA8/linear layout, fabricated resources, or placeholder shaders in
  acceptance runs. Unsafe bring-up is discovery-only diagnostics.
- One behavior has one implementation. Direct and indirect PM4 paths share a
  decoder; all resource consumers share one descriptor-to-layout calculation.
- An unsupported behavior must fail structurally and informatively. Do not
  hide it behind a generic fallback, compatibility alias, vendor check, or
  duplicated legacy path.
- Guest semantics remain platform-neutral. macOS, Linux, Windows, Vulkan
  extension, and GPU-vendor details belong at explicit host seams.
- AMD, Intel, NVIDIA, and Apple are capability inputs, never correctness
  policy switches. Select strategies from features, limits, formats, queues,
  and tested semantic alternatives.
- Do not add private fixture paths, title IDs, keys, binaries, saves, assets,
  screenshots, crash dumps, or raw logs to Git. Keep them under an ignored
  local directory and refer to them only as `$KYTY_GUEST_ROOT`.
- Do not use a commit message that identifies the private workload. Describe
  emulator behavior, for example `fix(graphics): validate Gen5 barrier range`.
- Preserve existing working behavior. If an experiment regresses menu reach,
  pixels, input, flips, or build, remove/revert the experiment before trying
  the next hypothesis.

REPRODUCTION AND VERIFICATION COMMANDS

```bash
cmake -S source -B _build_linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_linux
```

Configure and build on macOS:

Line count is a signal, not the goal. Do not split an atomic eight-line
function. Do split a long function that mixes parsing, state mutation,
allocation, Vulkan calls, and logging. Each extracted function/module must have
one purpose, explicit inputs/outputs, ownership, thread contract, error
contract, and a focused test. Delete the superseded implementation in the same
change; do not leave permanent forwarding aliases or duplicate semantics.

REFERENCE MATERIAL

Use reference material only for behavioral facts, architecture patterns, and
test ideas. Every imported claim must be verified with a local capture or a
focused test before it affects Kyty behavior. Do not copy incompatible code,
private assets, proprietary SDK material, firmware, keys, decrypted content, or
implementation details that cannot be relicensed into Kyty.

For each confirmed fact, record the provenance, license, behavior learned, and
the local evidence that proves it. Reimplement confirmed behavior using Kyty's
own types, boundaries, and diagnostics.

HANDOFF REPORT TEMPLATE

End every auxiliary-agent session with:

- Commit/base revision (no private fixture names).
- Build and focused-test commands plus pass/fail output.
- Strict command and first failure or verified gameplay checkpoint.
- Evidence table: verified fact, source, local reproduction, confidence.
- Files changed and why each belongs to the seam.
- Regression checks and performance measurement conditions.
- Exact next blocker and one proposed falsifiable hypothesis.
- Explicit statement that no diagnostic flag was required for acceptance.
- Phase and gate status: baseline, synchronization, gameplay, freeze,
  inventory, or extraction.
- If gameplay is not reached, the exact producer-side first failure and one
  falsifiable next hypothesis; never report the loading screen as success.
- If modularization began, the frozen gameplay commit and before/after evidence
  proving the extraction was behavior-neutral.

If a required fact cannot be evidenced, stop at a structured unsupported error,
report the blocker, and do not paper over it with a fallback or broad refactor.
```

Windows supports the generators and toolchains defined by the CMake files and CI.
Do not invent a Windows command from a Linux or macOS layout; inspect the active
workflow and generator first.

Build only the main script runtime when a full build is unnecessary:

```bash
ninja -C <build-dir> fc_script
```

Run focused tests through `fc_script`:

```bash
<build-dir>/fc_script '{kyty_run_tests()}' \
  --gtest_filter='SuiteName.TestName'
```

Confirm that a new or renamed filter actually selects the intended tests:

```bash
<build-dir>/fc_script --gtest_list_tests '{kyty_run_tests()}'
```

Run an authorized private fixture only when the task requires runtime validation
and `KYTY_GUEST_ROOT` is already available:

```bash
<build-dir>/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

The strict run sets no `KYTY_BRINGUP_*` variables (and no removed legacy flags).
Capture the first error completely, including packet/register values and the
guest/host call path when available.

### 3. Form one hypothesis

State the suspected root cause and the evidence supporting it. Change one
variable at a time. If a hypothesis fails, remove the experiment before testing
the next one.

### 4. Work test-first

For every behavior change:

1. Add the smallest deterministic failing test.
2. Run it and confirm the expected failure.
3. Implement only the behavior required by the test.
4. Run the focused test until it passes.
5. Build and re-run the strict integration scenario.

Sanitized PM4 packets and surface descriptors are acceptable fixtures. Guest
code and assets are not.

### 5. Verify the real outcome

For graphics changes, a successful build and non-black pixels are insufficient.
Verify geometry, colors, resource interpretation, completed flips, absence of
Vulkan errors, and a recognizable correctly proportioned frame. Preserve local
visual evidence outside Git.

### 6. Refactor only behind a frozen frontier

After strict menu and gameplay acceptance exists, inventory oversized modules
with line counts, responsibilities, dependency direction, mutable globals, and
test coverage. Select one cohesive extraction at a time. For each extraction:

1. Record the pre-refactor strict frontier and focused test results.
2. Add missing characterization tests without changing behavior.
3. Move one responsibility behind a narrow interface.
4. Remove the original implementation rather than leaving an alias.
5. Build, run focused tests, and reproduce the strict frontier.
6. Revert the extraction if the frontier, frame, input, or validation state
   regresses.

## HLE and ABI rules

- Every export needs an evidenced name, NID, signature, calling convention,
  argument validation, return code, and side effect.
- Prefer guest error returns for expected invalid input. Assertions and process
  exits are for violated emulator invariants, not ordinary guest errors.
- Do not map a new NID to a convenient existing function until their contracts
  have been compared, including failure behavior.
- Keep registration centralized in the owning `Lib*.cpp` module.
- A generic missing-symbol stub may be used to discover which import is called;
  it must never be required by acceptance runs or releases.

## Graphics rules

### PM4 and normalized state

- Packet envelope validation belongs to packet parsing.
- Register bit decoding belongs to one state-decoder function.
- Direct and indirect packet handlers call the same decoder.
- Unknown registers report packet type, register, value, submit ID, and command
  offset, then stop in strict mode.
- Never label an unknown register harmless without proving its semantics and
  showing that the workload does not depend on it.

### Surface layout

- Format, block geometry, pitch, mip levels, depth/array layers, sample count,
  tile mode, metadata, size, and alignment form one descriptor-to-layout
  calculation.
- Compressed formats use block dimensions; bytes-per-pixel arithmetic is not a
  substitute.
- CPU upload/detiling, overlap tracking, Vulkan allocation, and writeback consume
  the same layout.
- An unknown descriptor returns a structured unsupported error. It does not
  assume four-byte texels or linear memory.

### Vulkan and GPU portability

- Collect device capabilities once and pass them explicitly to consumers.
- Classify each capability as required, optional with a semantically equivalent
  tested strategy, or diagnostic-only.
- A correct alternative for an absent extension is not a behavioral fallback:
  it must preserve guest-visible semantics and have tests for both strategies.
- Do not add AMD-, Intel-, NVIDIA-, Apple-, MoltenVK-, or driver-specific paths
  to guest state decoding or surface layout.
- Keep Vulkan validation clean when the platform supports the required layers.

## Platform portability rules

- macOS is a distinct supported host, not a Linux build label.
- Use portable C++ and existing Core/Sys abstractions in shared code.
- Confine `__APPLE__`, `_WIN32`, and Linux-specific branches to platform-facing
  implementation files.
- Do not use Apple frameworks, Win32 APIs, or Linux syscalls in HLE, PM4,
  shaders, surface layout, or renderer policy.
- Treat host CPU architecture separately from host OS. Preserve the current
  native x86-64 path while keeping future execution backends possible.

## Diagnostic flags

Default runtime mode is **strict**: `EXIT_NOT_IMPLEMENTED` aborts with stack and
subsystem shutdown; missing imports do not receive stubs; unknown indirect GPU
registers are fatal. Strict acceptance runs must not set any `KYTY_BRINGUP_*`
variable (`scripts/run_guest.lua` rejects them unless
`KYTY_BRINGUP_ALLOW_DIAGNOSTIC=1` is set for an authorized smoke only).

### Centralized unsafe bring-up (`Kyty::Core::BringUp`)

Diagnostic continuation is centralized. Do **not** invent per-game exceptions
or cite unsafe survival as compatibility. Neighbor PRX soft-preload is
**unsafe-only** (`prx_preload` feature); strict acceptance never auto-preloads.

| Variable | Meaning |
| --- | --- |
| `KYTY_BRINGUP_MODE=unsafe` | Enable diagnostic policy (absent ⇒ strict). |
| `KYTY_BRINGUP_FEATURES` | CSV: `not_implemented`, `missing_function_import`, `gfx_permissive`, `prx_preload`. Absent under unsafe enables the first three only; **`prx_preload` is always explicit**. |
| `KYTY_BRINGUP_SUBSYSTEMS` | CSV scopes: `core,loader,kernel,graphics,audio,network,hle,other`. Absent ⇒ all. |
| `KYTY_BRINGUP_BURST_LIMIT` | Max hits per site inside the window (default 10000). |
| `KYTY_BRINGUP_BURST_WINDOW_MS` | Window length in ms (default 1000). |

Unknown, empty, zero, or contradictory values abort at process start
(`BringUp::InitFromEnvironment` from Core subsystem init; no silent strict
fallback after a parse error). Circuit-break on a site re-enters the normal
strict abort after printing a summary. Repeated `EXIT_NOT_IMPLEMENTED` continues
log once per site (no full stack spam on every hit). The policy never
fabricates EventFlags, fences, memory, or sync results. Only **Func** imports
may receive missing stubs (with a minimal return-class taxonomy); Object / TLS /
NoType stay strict `EXIT`. Neighbor PRX scan is **not** part of default unsafe
features — set `prx_preload` explicitly.

**Removed (intentional break):** `KYTY_STUB_MISSING` and `KYTY_GFX_PERMISSIVE`.
Using them is a configuration error.

Other diagnostics (unchanged, still not acceptance modes):

- `KYTY_FAULT_LOG=1`: signal-safe fault diagnostics.
- `KYTY_CRASH_REPORT=/absolute/path.json`: writes the bounded fatal-fault JSON
  report. `KYTY_CAPTURE_DIR` supplies `crash-context.json` by default when an
  explicit report path is absent.
- `KYTY_CRASH_MEMORY=1`: on supported POSIX hosts, adds at most 24 fault-safe
  64-byte windows around plausible guest-data pointers found on the captured
  stack. It is disabled by default and its output may contain guest data; keep
  the report in untracked scratch.
- `KYTY_TRACE_LIBC=1`: targeted single-step tracing.
- `KYTY_SKIP_UD2=1`: skips a guest trap for diagnostics; invalidates normal
  execution. **Not** part of the bring-up policy.
- `KYTY_PIPELINE_FAIL_DUMP=<directory>` and `KYTY_TRANSPORT_DUMP=<directory>`:
  opt-in local evidence dumps (SPIR-V of a failed pipeline; the guest program
  that needs fragment wave transport). The directory must already exist and is
  never created. File names are fixed components, created exclusively (an
  existing file, symlink or FIFO is not overwritten), each file is capped at
  16 MiB and the process at 64 MiB, and a cut dump is reported as truncated.
  Each file's status is reported in the fatal transport message or the debug
  log; a rejected dump never changes the outcome of the run. Keep the directory
  in untracked scratch.

Agent diagnostics JSON protocol version is **5** and includes `bringup.mode`,
features, subsystems, limits, unique sites, continuations, missing-import
metrics, and last circuit-break (`BringUp::WriteDiagnosticsJson`).

No diagnostic flag is enabled by default or cited as proof of compatibility.

Integration matrix (process-isolated):

```bash
cmake -S source -B _build_linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_linux fc_script kyty_bringup_integration
ctest --test-dir _build_linux --output-on-failure -R KytyBringUpIntegration
```

## External references and licensing

Reference implementations may establish names, concepts, architecture patterns,
and test ideas. Kyty is MIT-licensed; do not copy GPL implementation code into
this repository. Record the behavioral fact and provenance, then implement it
against Kyty's own types after verifying it locally.

Use PS5-focused references for guest ABI and AGC evidence, mature emulators for
renderer/capability architecture, and official Vulkan documentation for host API
semantics. Do not assume another console's GPU behavior applies to PS5.

## Code quality

- Follow `source/.clang-format` and the existing C++17 style.
- Prefer focused functions and explicit types over duplicated bit manipulation.
- Keep headers minimal and ownership clear.
- Avoid broad renames, compatibility aliases, dead code, commented-out paths,
  magic constants without provenance, and unrelated cleanup.
- Comments explain evidence, invariants, and non-obvious hardware semantics; they
  do not narrate obvious code or advertise another project.
- Treat warnings, `git diff --check`, and new validation messages as failures to
  investigate.

## Completion checklist

Before committing a behavior change:

1. The focused test failed before implementation and passes afterward.
2. `ninja -C _build_macos` succeeds.
3. `git diff --check` succeeds.
4. The strict local scenario advances or renders more correctly.
5. No missing-symbol stub or permissive register skip is needed for the claimed
   behavior.
6. No tracked file contains fixture information or generated evidence.
7. No OS or GPU vendor was made a hidden correctness requirement.
8. Existing working behavior was rechecked.
9. The commit message describes emulator behavior without identifying a private
   compatibility fixture.
10. Any refactor preserves the frozen strict frontier and leaves one active
    implementation of each behavior.
11. New or extracted modules have a documented responsibility, ownership model,
    dependency direction, and focused tests.
