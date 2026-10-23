# First-person puzzle title

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named descriptively only (`AGENTS.md`).

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
