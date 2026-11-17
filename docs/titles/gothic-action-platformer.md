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
  is raising that scene's frame rate, which needs command-processor throughput (the per-draw binding
  cost recorded for other titles).
