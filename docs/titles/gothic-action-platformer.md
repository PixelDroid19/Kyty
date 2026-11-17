# Gothic action platformer

Title: PPSA35399 - Castlevania: Belmont's Curse.

Records for this title, newest first. The bring-up manual keeps the
invariants, methodology, frontier and an index of every record. Titles are
named by PS5 content ID and public name (`AGENTS.md`).

## The new-game video presents black (2026-11-16, open)

Intel Arc A770, Mesa ANV on Xe, 60 fps present limit, strict runs.

- **Observed.** The title reaches its studio logo, the notice text and the title menu with the correct image.
  After New Game, the video that follows presents black with only its skip prompt drawn, about 30-70 s into
  a scripted run.
- **Excluded: the gamma-twin target alias.** The fix that made another title's studio video visible (a
  UNORM render target sampled as sRGB, `13fafeac5`) does not change this title: the same route still
  presents black, so the cause lies elsewhere in the video chain.
- **Chain, measured (2026-11-16).** The player decodes the 185 s cinematic and hands out NV12 frames at the
  media rate. A compute dispatch converts them: it samples the luma (`8` 2048x1080) and chroma (`8_8` 1024x540)
  planes in the player's frame buffers. In runs where the video stays black, that dispatch never samples the
  planes: no draw, dispatch, DMA or storage-buffer read touches the frame range in 50 s. In runs where it does
  sample them, the video advances.
- **Discriminator: the guest's resync loop.** In the black runs the guest's player wrapper seeks every
  250-330 ms (`Pause` then `Resume`, no jump; the same call sites as its first `Pause`, `JumpToTime(0)`,
  `Resume`). Every audio and video pull happens inside those seeks. Between them the guest makes no player
  call at all, so it never consumes the held frame and never runs the conversion. In a good run the
  main thread consumes the first frame 18 ms after the post-seek audio sync and then pulls frames on
  every game frame. Which runs go bad is not deterministic: both the route's save and the default save
  produce both outcomes.
- **Excluded: a lock leak in the guest wrapper.** Its pollers take the lock with an increment and never
  undo it when it is busy, so one poll that overlapped a seek would block all later polls. The lock word
  reads 1 during every seek and 0 between them, never 2.
- **Excluded: the wrapper's loop-wrap counter.** It stays 0, so it does not block polls.
- **Excluded: audio output pacing.** `AudioOut2ContextPush` blocks for 1000 ms per second at 188 calls per
  second, which is real time.
- **Excluded: switching off the audio-first read-ahead** (`fabb94594`). Without it the guest waits for audio
  and stops the player after 10 s, the same contract that change was made for. Capping the read-ahead at
  1 s of held video also stalls the guest: it takes 939 ms of audio and never polls video again.
- **The resync logic is the guest's own.** The guest's disassembly shows a player-side scheduler that,
  once per main-thread tick, compares the held frame against its own clock and requests a seek when the
  frame is more than five frame periods behind. A request is launched on a job thread the next tick, and
  polling stays blocked until the main thread sees the job finish. Every step is paced by the game's tick,
  not by the player, so no player return value, timestamp or event can sustain the loop.
- **Frame time in that scene (2026-11-16).** Over a 5 s window during the video, a black run measured:
  11.9 fps, 3,573 draws per frame, the command processor busy 4.33 s, and resource binding 2.77 s of it.
  Rendering at 960x540 instead of the guest's 3840x2160 targets left the scene at 11 fps and the loop in
  place, so the scene is bound by command processing, not pixel work. Working hypothesis, not yet
  proven causally: at a 90 ms tick the guest's resync window loses the race in most runs. The causal test
  is raising that scene's frame rate, which needs command-processor throughput. The binding cost is
  higher here than on the titles tuned so far: the scene binds about 7 textures and 5 storage buffers per
  draw.

## Frame rate in the new-game video scene (2026-11-17, open)

- **Baseline.** Strict run, `KYTY_PRESENT_FPS_LIMIT=60`, agent `perf-snapshot` over 6 s while the video
  plays: 9.3 fps, 3,336 draws per frame, 27.2 us per draw in the draw processor. Binding 14.0 us
  (textures 5.8, storage 4.1, descriptor finalization 2.7), state setup 7.5, pipeline setup 3.1. Other
  windows of the same scene read 11-14 fps and 21 us per draw. The swing follows draw-front coverage:
  a draw the front did not prepare repeats the pixel-shader analysis inside state setup (see Draw
  front). After the skip, gameplay runs at 13-14 fps.
- **Why the draw is expensive.** Each draw binds about 7.4 textures and 5.6 storage buffers, at roughly
  0.7-0.8 us each. On the sandbox title, at 6.3 us per draw, binding is under 2 us. Rendering at 960x540
  does not change the frame rate, so the scene is bound by command processing.
- **Profile of the command-processor thread** (stack samples at about 45 Hz over 14 s): descriptor
  binding 49 % (textures 18 %, storage buffers 16 %, set lookup 9 %), snapshot capture of storage buffers
  13 % (write tracking re-armed and released with `mprotect` per snapshot), submission 9 %, pixel-shader
  input analysis 7 %, pipeline lookup 5 %.
- **Storage buffers.** 66 % of them are captured into transient snapshots and 34 % are read by address.
  Every captured one fails the by-address route at `dword_loads_only`, and 84 % of them are V#s the
  shader loads from a descriptor table (`ShaderStorageBindingSource::DynamicScalarLoad`), which the
  by-address route leaves on the descriptor path by design (`48cfd70c8`). Descriptor-set misses are
  1.7 % real misses and 34 % uncached sets that bind a fresh snapshot by design.
- **Parked: reusing the previous texture-stage result.** 87.5 % of stage binds repeat the previous
  bind's T# and S# words, and a shadow check found no output mismatch on 83 % of binds with the same
  inputs, submission and object-graph generation; a positive control (different inputs) changed the
  outputs in 99.9 %. Skipping is still not equivalent: a sampled texture's `GpuMemory` object is created
  with `read_only = false`, so every bind advances its `write_time` on a per-call clock. Skipping
  could change which overlapping object is newest; that ordering would need its own proof first. 37 % of the eligible binds also sample a
  depth target (`depth-guest`, `depth-exact`).
- **Draw front.** 92-97 % of draws take the inputs the front thread prepared; the rest repeat the
  pixel-shader analysis on the command-processor thread at about 35 us each. With storage binding
  cheaper (below), the command processor outruns the front more often: empty-queue misses went from
  about 1,400 to 7,000 per window. The front is the next limit.
- **Fixed: read-only storage bound in place** (`7473c5bfd`). 99.9 % of the snapshot captures were
  read-only ranges inside an imported guest-memory buffer at a bindable offset (4-byte storage
  alignment on the Intel driver), about 15 KB each. They now bind that buffer directly, as by-address
  rows already did. Same scene, same binary, A/B: storage stage 4.1 -> 1.4 us per draw, draw
  processor 21.8 -> 20.4 us, capture calls about 515k -> 20k per 2 s, 13.35 -> 13.67 fps; a second
  A/B under a foreign host build read 32.3 -> 28.4 us and 7.0 -> 7.9 fps. Intro cinematic captures
  unchanged. Minecraft (`PPSA17221`) storage stage 0.39 -> 0.28 us, captures correct; Astro Bot
  dunes route reached and rendered normally. Slots that a storage-image seed proof reads keep their
  snapshot. Premise, shared with the by-address path: no CPU write to those pages between recording
  and GPU execution.
- **Run note.** With `PrintfDirection = Silent` the scripted route reaches the title menu at a
  different time and stays there, so the scene never loads; use console logging for this route
  until it waits on a scene condition.
