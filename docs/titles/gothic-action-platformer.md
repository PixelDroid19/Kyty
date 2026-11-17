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

- **Baseline (console logging on).** Strict run, `KYTY_PRESENT_FPS_LIMIT=60`, agent `perf-snapshot` over 6 s
  while the video plays: 9.3 fps, 3,336 draws per frame, 27.2 us per draw in the draw processor. Binding 14.0 us
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
  rows already did. Same scene, same binary, A/B with console logging on: storage stage 4.1 -> 1.4 us
  per draw, draw processor 21.8 -> 20.4 us, capture calls about 515k -> 20k per 2 s (single fps
  pairs in this scene vary by more than the change and are not quoted). Intro cinematic captures
  unchanged. Minecraft (`PPSA17221`) storage stage 0.39 -> 0.28 us, captures correct. Astro Bot
  (`PPSA21567`) on the dunes script reached the world map, not the dunes, and rendered normally. Slots that a storage-image seed proof reads keep their
  snapshot. Premise, shared with the by-address path: no CPU write to those pages between recording
  and GPU execution.
- **Run note.** With `PrintfDirection = Silent` the title menu answers about 6 s later than with
  console logging, so the logging-timed route misses it; the Silent route presses cross at 26, 31
  and 36 s, and the scene runs from about 41 s.
- **Silent baseline** (after `7473c5bfd`): 13.9 fps, 3,550 draws per frame, 16.96 us per draw
  (textures 4.28, state setup 4.01, finalization 2.92, pipeline 1.72, storage 1.15). The command
  processor is inside `Run` about 89 % of the time and the render engine is busy 55-60 %, so the
  scene is bound by command processing. Four runs of the same binary read 13.0-14.6 fps.
- **Draw front, measured.** Per 2 s: about 4,700 front restarts, one per guest submission (about 77 %)
  or after a `RELEASE_MEM` (about 23 %), each missing its first draw, plus about 3,200 misses later
  in the segment. A miss repeats the vertex (3.9 us) and pixel (20 us) input analysis on the
  command processor, about 1.8 us per draw on average. Most of the pixel cost is
  `ShaderParseUsage2` resource resolution, not code analysis (already cached per program).
- **Descriptor sets cached on a second request** (`caaecf9b0`). About 25-30 % of the per-draw
  descriptor set requests created a cached set that was never found again (storage views carry
  per-draw offsets), and indexing and trimming them cost more than creating them. A set is now
  cached only when its hash misses a second time; the first one is recycled after its submission.
  Same-binary A/B with the filter switched off, two clean pairs, Silent route: finalization 3.07 /
  2.90 -> 1.96 / 1.83 us per draw and binding 9.02 / 8.79 -> 8.01 / 7.81 us, state setup read
  3.70 / 3.97 -> 4.53 / 4.57 us in a binary with counters in a scan both threads run (later runs read
  2.98-3.87 us, so the state-setup arms are not comparable) and the total per draw was 15.90 / 17.73 -> 15.88 / 15.56 us (fps
  15.00 / 13.58 -> 14.83 / 15.08): a net per-draw gain is not established. Uncached sets are deferred
  inside `GetDescriptor`; their recycling runs from a submission completion callback, which may land
  in another stage. Sandbox world: hits stay at 98.9 % of requests. Robot platformer: hit ratio
  35-39 % in both arms on a loaded host in the warp tunnel, no timing, not gameplay. A burst of six
  captures in the dialogue scene showed no stale or missing sprites. On `3901c34bd` (2026-11-17)
  the robot platformer's dunes route reached gameplay with three normal captures at 160-210 s and a
  clean exit; an earlier run on a loaded host ended at status 139 on the title's recorded hub-start
  command-buffer assert (`rip 0x9004a0c3e`), which the route meets when it runs late.
  `kyty_agent playable-regression` on `PPSA06084 - Superliminal`: 10 of 15 gates pass, gate for
  gate the same as the earlier reference (the five failures are the missing baseline, checkpoint
  and input gates).
- **Excluded: a register prefilter for the dynamic scalar-load scan.** `ShaderIsDynamicScalarStorageConsumer`
  scans about 30 records per call, but 89 % of calls find a matching storage record, so filtering
  by destination register would skip almost nothing.
- **Texture-stage reuse, priced (excluded as the lever).** Shadow check on the Silent route
  (2026-11-17): of about 170,000 texture-stage binds per 2 s, 84 % repeat the previous bind's
  inputs in the same submission and object generation, with 0 output mismatches (positive control:
  99.9 % of changed inputs change the output); robot platformer 0 of about 7,000, sandbox world has
  no eligible binds. But the binds with no side path (about 90,000 per 2 s) take 100 ms of the
  470 ms texture stage: about 1.1 us each, already `CachedReuse` hits with no other command-buffer
  work. Skipping whole stages without a depth texture saves at most about 50 ms/s (excluded as
  scoped). Timed per texture, the stage cost is in ordinary sampled textures: `guest-upload` about
  626,000 per 2 s taking about 290 ms (about six per draw, 0.46 us each), against `depth-guest`
  67,000 (about 60 ms) and `depth-exact` 41,000 (about 10 ms).
- **Depth refusal text built only on refusal.** The depth path formatted a refusal string on every
  depth bind (about 0.9 % of the command-processor thread); it is now formatted only when the
  depth materialization is refused, the one case that prints it.
- Dialogue scene after the new game (robust route, Silent): about 20 fps, no visual errors in a
  burst of captures.
- **One host submit per guest batch, priced (2026-11-17, Silent route, scene after the new game).**
  The guest submits about 2,700 command batches per second (about 190 per frame); each ends in
  `BufferFlush`, and every one carries a queued graphics interrupt and a decode completion, so no
  batch is free of a completion. The next batch is already queued at 67 % of batch ends, and batch
  streams suspended at most rarely in three runs (the counter never reported); that does not show
  the batches are independent, since the mostly idle GPU may write a label before the next wait
  decodes. Inner timers: about 27 us per submit
  (`vkQueueSubmit` 12, `Begin` 7, fence retirement 6), about 70 ms/s, plus about 10 ms/s publishing.
  The first draw after a batch-end submit takes about 58 us against about 12 us for the others:
  state setup +24 us, resource binding +10 us (texture descriptors +6 us), command emission +6 us.
  The state-setup part is the draw front, not the submit: the front starts at the first draw of a
  run and stops at every ordering packet, so the first draw of every batch takes the inline shader
  input analysis (about 28 us of state setup, about 4,500 per 2 s; a front start inside a batch
  costs about 16 us, and the 12 us difference is unattributed, possibly the fresh command buffer).
  Other front misses add about 3,000 per 2 s at about 25 us (binary with timers on every draw
  metric; about 4,800 per 2 s with counters only), against about 1 us for a front hit: the miss
  path totals about 106 ms/s, the largest single item left in state setup.
  Every live-front miss is a draw the front has not prepared yet (no epoch, freshness or instance
  misses), and almost all of them fall at the third or later draw after a front start, not at the
  start (4,200-4,650 of about 4,800 per 2 s; the front was asleep at 99.6 % of starts). So the miss
  is not wake latency. The front thread uses about 0.65 s of CPU per second, about 12 us per draw
  all included. The pixel input analysis cost about twice as much per call inline on the
  processor thread as on the front in a loaded run (about 22 against 11 us; not repeated on a
  quiet host; cause not separated: colder caches on the processor thread, or heavier draws at the
  miss points). On a loaded host (about 9 fps, half the
  usual draws) the front's lead at a `Take` was usually 3 or more draws and 0 at about 11 % of
  takes; that histogram was not repeated on a quiet host. Excluded: polling the front awake.
  The header comment of `GraphicsRunDrawFront.h` says the replay runs about 4 draws per processor
  draw; in this scene it runs about 1.2 (about 12 against about 14 us per draw), so the comment
  and the measurement disagree.
- **Waiting for the front instead of analysing inline (excluded).** A processor that reached the
  draw the front was analysing waited for it (interleaved A/B, 3 runs each): state setup fell from
  3.2-3.8 to 2.4-2.5 us per draw, but the wait sits outside that timer. The processor locked step
  with the front: about 22,000 waits per 2 s at about 9.3 us, about 100 ms/s, against about
  53 ms/s of inline analysis removed; the per-draw processor time did not move beyond noise
  (14.8 against 15.1 us). Reverted.
  A command-processor thread profile of the same scene (clean build, 6 s, about 3,200 samples)
  puts the inline pixel-shader input analysis at 11 % of samples and the vertex one at 2.3 %.
  Most of the pixel part is `ShaderParseUsage2` (8.8 %): dynamic scalar resources, sampled texture
  to sampler association and redefined buffer descriptors. Their code-only scans already go
  through `ShaderCodeResourceMemo`; what remains depends on the bound descriptors, so a cache must
  key on the descriptors the analysis reads from guest memory.
  Merging submits of batches whose successor is queued is worth roughly 50-75 ms/s, and it is not
  a local change: a merged batch's `m_batch_ends` entry would name a recording submission, which
  `GraphicsRing::WaitRetired` on a guest thread then submits mid-batch through `WaitSubmission`
  (serialized with draw recording, which also holds `m_mutex`), and every batch's interrupt and
  completion would move to a later submission. Not built; its
  precondition (whether a batch's waits name labels the previous batch writes) is unmeasured.
- **Gameplay has the same profile (2026-11-17).** The route reached the first controllable level
  (HUD and character in the captures, 160-182 s): about 3,470 draws per frame at about 14.0 us per
  draw (binding 7.6, state setup 3.0, descriptor textures 3.7 us), 17 fps, about 3,000 submits
  per second. The dialogue before it runs at 19-22 fps. The scene levers above apply unchanged.
- **Per-texture guest-upload memo, priced (not built).** `GpuMemory::CreateObject` takes about
  125 ms/s over about 511,000 calls per second in gameplay; textures are about 409,000 of them and
  90 % end in `CachedReuse`, whose remembered-key path already skips the materialization cache.
  The GPU-owned lookups before it are already skipped by `SampledLookupMissMemo`. On a repeat
  inside one submission `RecordUse` and `use_num` change nothing, but the hit requires `in_use`,
  and `GpuMemory::WriteBackObjectLocked` clears it without changing any generation, so a memo
  above `CreateObject` would still recheck the object under the registry lock. It could remove
  the backing lock, the key build, the remembered-key scan, the statistics and the texture
  descriptor setup: about 50-65 ms/s at best, about 1 us per draw, while moving registry rules
  into the bind path. The create statistics cost about
  4 % of the processor thread's samples.
- **Excluded: a register-keyed memo of the pixel input analysis.** Shadow on the gameplay route
  (2026-11-17), key = the pixel stage registers, user SGPRs, the shader registers the analysis reads,
  the colour mask and the vertex fields it reads; a 65,536-entry table per thread; hashes of the
  output before `bind` and of `bind` compared on every repeat, with an injected mismatch every
  4,096 repeats (242 of 242 caught). The front analysed about 52,000 draws per second and repeated
  a key on 32.4 % of them; the processor's inline misses (about 8,400 per second) repeated on
  22 %. 68-78 % of insertions replaced a different key. On 76 % of the front's calls and 96 % of
  the inline ones the program and registers had been seen before and only the user SGPRs
  differed. No repeat changed the output before `bind`; 26 % (front) and 66 % (inline) of repeats
  changed `bind`, mostly across a host-write epoch change. Useful inline hits are about 7.6 % of
  the misses, roughly 13 ms/s. A cache that pays would have to split the program-dependent layout
  work from the SGPR- and descriptor-dependent part inside `ShaderParseUsage2`, a redesign of that
  function rather than a memo. (A first shadow with a 512-entry table read 24.5 % and 4.1 %; it
  could not see repeats across frames.)
- **Excluded: the replay worker sleeping instead of spinning.** `DeferredReplayWorker` polls for
  200 us before sleeping and spends about 27 % of the process's samples in that spin (6-core SMT
  host). With the window set to 0 (interleaved, 2 runs per arm) the processor got slower,
  15.72 / 15.60 against 15.27 / 15.04 us per draw (fps 14.9 / 14.8 against 15.5 / 15.8). Sleeping
  adds a wake-up per hand-off, so this shows sleeping is not a gain; it does not show what the
  spin itself costs the processor thread.
- **Pixel input analysis profile (gameplay route, 2026-11-17).** The tables of the register-keyed
  shadow above were per thread; a table shared with the front could at most add the front's 24 %
  repeat share, so that exclusion holds. Inline, the analysis costs the processor thread about
  93 ms/s (about 1.6 us per draw). Its samples over all threads (mostly the front) split into the dynamic scalar collector (17 %,
  reads the snapshot words, so content-dependent), the sampler association (16 %), the
  redefined-buffer scan (12 %) and the descriptor-load `any_of` (19 %); the last three read only
  the code and the binding set. `ShaderAnalyzeImageCopyReads` drew no samples: its layout memo
  always hits, so binding layouts repeat. `0469499d3` marks static EUD words in one pass and hashes
  the image-copy layouts; results unchanged, unit tests pass, Astro Bot dunes and the Minecraft
  world render as before. Its sample saving (about 7 %) was read from a profile dumped with
  different inline flags under foreign load, so it is unconfirmed; no frame-rate claim.
- **Not built: a binding-layout memo of the code-and-binding work.** It could remove at most about
  43 ms/s directly (about 46 % of the inline samples, before the key cost), below the gate that
  excluded the texture memo. More would need a faster front to turn inline analyses into front
  hits; the split of the 8,400 inline analyses per second into structural ones (batch start, epoch
  change) and front-behind ones is not measured. Even removing the whole inline analysis and the
  miss path (about 106 ms/s) leaves about 12.2 us per draw, about 19.5 fps: 30 fps needs the
  processor redesign, not local levers.
- **Merge precondition counters (gameplay).** Per second: `wait_reg_mem_satisfied` 2,651,
  `wait_reg_mem_current_producer` 1,662; queued-producer, producer-mismatch, not-found and
  suspended waits 0, against about 3,000 flushes. The guest-thread `WaitRetired` rate is not
  measured, so the batch merge is neither opened nor closed.
- **Excluded: merging consecutive guest batches into one submission.** Per-call-site census of
  `SubmitCurrentLocked` (heavy scene, temporary tag, 2026-11-17): 95 % of the submissions come from
  the flush at the end of each guest graphics batch, about 2,700 batches per second in the heavy
  scene; register waits, flips and the idle-submit path add under 5 %. Every batch carries a
  completion callback and a decode signal. The next batch was already queued at the end of 66 % of
  batches, and almost exactly those batches had returned before decode; the other 34 % have a
  submitter blocked on this batch's decode signal, so they cannot merge. The end-of-batch flush
  costs the processor thread 5.9 % of its samples (about 52 ms/s on the gameplay route), so a merge
  could save at most about 34 ms/s from the submit alone. Unverified: whether the batch-start state
  setup (about 28 us on the first draw of a batch) follows the batch or the command buffer; if it
  follows the buffer, a merge would also save part of it and could pass the gate. Not measured: the guest-thread
  `WaitReleaseTicket` rate, which a merge would turn into mid-batch submits from guest threads.
- **Check of `0469499d3` on this title:** heavy-scene run, `last-error` empty, events only
  informational, one capture scored healthy (it landed on the title menu, not the scene).
- **Built: shader ids compared with `memcmp` (`b0d7c323c`).** On the gameplay route the pipeline
  lookup spent 161 of the processor thread's 3,193 samples in the element loop of
  `ShaderId::operator==`. With `memcmp` the lookup fell from 205 to 99 samples and pipeline creation
  from 299 to 187 (3,185 samples). One run without the profiler (2026-11-17, quiet host): 13.15 us
  per draw and 17.96 fps against 14.0 and 17 on `ac7cbcbba` (`gp2`); pipeline setup 1.18 against
  about 2.1 us per draw. Single runs, not interleaved arms. Remaining texture-stage items on that
  profile: `GpuMemory::CreateObject` 12 % of the thread (statistics about 2.3 %), the VideoOut
  lookup for every sampled texture that is neither a render target nor a depth target 1.6 %.
  Astro Bot dunes and the Minecraft world render as before on `01edd8484`.
- **Cost of one extra submission, inconclusive (2026-11-17).** A temporary override that submitted
  every 10 operations mid-batch (heavy scene) raised submissions from about 180 to about 458 per
  frame. Against baselines on the same binary the processor run time rose 3.35 ms per frame
  (12 us per added submission) in one pair and 11.77 ms (43 us) in a quiet pair; a pair under
  foreign load read 41 us. Quiet baselines alone spread from 48.9 to 55.9 ms per frame, as wide as
  the effect, so the batch merge stays excluded until a within-run toggle measures the slope. The
  draw stage timers did not move; the added time is in the submission itself. On the gameplay
  profile the batch-end flush is 225 of the processor thread's 3,185 samples: `CommandBuffer::Begin`
  112 (the per-buffer transient pool reset 55, which re-indexes every committed buffer in an
  ordered map and clears the content hash; `vkResetQueryPool` 19; driver begin 20), fence
  retirement 33, `vkQueueSubmit` path 23, publication 40. Each of these is 0.6-1.7 % of the thread.
- **Second draw front (2026-11-17).** Two front threads now replay the same packets from each start,
  each with its own shadow processor and record ring; the first to reach a draw claims it on a shared
  packet cursor and analyses it, and the processor takes from whichever front holds it. Measured on
  the gameplay route with a temporary within-run toggle (second front off every other 3 s window),
  quiet host: draws taken without prepared inputs 7.0 % -> 5.6 % (the rest are the first draw of each
  start), state setup 2.90 -> 2.65 us per draw, draw processor 14.23 -> 13.91 us per draw (10 and 12
  one-second rows). A run under foreign compile load read 7.5 % -> 5.5 % and 2.91 -> 2.63. About 2 %
  of the processor; no fps claim. A temporary check recomputed every taken draw's inputs inline and
  compared both shader ids (they serialize every input): 0 mismatches over 1.23 million draws on
  `PPSA21567 - Astro Bot` (dunes) and 6.0 million on `PPSA17221 - Minecraft` (world), and none
  reported on this title's gameplay route; captures of both match earlier runs. The misses left are
  the start draws, about 3,400 per second at about 28 us each.
- **Front starts counted (2026-11-17, `cc3f74e58`, gameplay route, temporary counters).** Per second:
  about 1,950 guest batches, each one top-level run with no nested indirect buffer, about 30 draws each;
  about 1,700 front starts, 99.4 % of them the first draw of a run (mid-run restarts after an ordering
  packet are about 10 per second). From the end of a run to the next run's first draw: 7.2-8.0 us on
  average, and the host-write epoch moved in none of 13,421 such gaps. 64 % of runs reach their first
  draw through register packets only; the other 36 % meet a non-register packet first, almost all
  `CLEAR_STATE`. So the start draws total about 1,700 x 22-28 us, about 40-48 ms per second (half the
  earlier estimate, which inferred the start count from the miss rate), and a front started at the
  previous run's end would lead the processor by about 7 us, less than one analysis on the front
  (about 11 us). The other misses (about 2 % of takes) are draws the front did not prepare in time or
  could not prepare. The capture at 172 s shows the level, and `last-error` is empty.
- **Excluded: starting the front across the batch boundary (2026-11-17).** It can apply only where the
  next batch is already queued at a run's end (66 % in the heavy scene) and opens with register
  packets only (64 %), about 700 starts per second, about 15 ms per second at best; and a 7 us lead
  against an 11 us analysis on the front still misses the first draw.
- **Display-buffer lookup filtered (2026-11-17).** Every sampled texture the texture memo did not
  resolve asked the video output for a registered display image at its address, taking the host
  access gate and the video output lock, 54 of 3,185 processor-thread samples (1.7 %). The video
  output now keeps every address a display buffer was registered at (64 entries, then every lookup
  searches) and returns at once for any other address. Gameplay profile (under foreign compile load):
  4 of 3,104 samples. On `d64a0b11b` the captures show this title's level and the
  `PPSA21567 - Astro Bot` dunes; `PPSA17221 - Minecraft` missed the world once (the menu route
  ended on another menu screen, drawn correctly) and entered it at 86.7 s on the rerun, ending on
  the save's usual death screen. No fps claim.

### Recent materializations in a direct-mapped table (2026-11-17)

- Shadow run in gameplay (73.4 M texture and buffer object requests): the 8-entry recent ring
  answered 10 % of them; a 256-slot table keyed by the full materialization key would answer 71 %
  under the same validity test (object-graph, guest-bytes and cache-removal generations). A
  lock-free variant was not built: sampled textures arrive with `read_only` false, so every reuse
  writes `write_uses` and `write_time`, and `ResetHash`/`ResetPlaneHash` bump only the cache
  removal counter, which is read under the registry lock.
- Change: the ring became a 256-slot direct-mapped table indexed by a cheap hash of the first range,
  its size, the first parameter and the object type; the full key compare, the generation test and
  the field recheck under the lock are unchanged.
- Verification: a temporary diagnostic run compared every table hit against the materialization cache: 46.8 M hits
  of 67.1 M requests (70 %), 0 object mismatches besides one forced by hand. Command-processor
  profile (gameplay, about 3,100 samples each): `CreateObject` under `PrepareTextures` 14.6 % ->
  11.6 %, `PrepareTextures` 23.2 % -> 20.8 %. Captures of `PPSA21567 - Astro Bot` dunes and the
  `PPSA17221 - Minecraft` world render as before. No fps claim (perf-recorded run).
- Open: most stale table probes come from cache-slot evictions bumping the removal counter; split
  that counter before changing the validity test.

### Prepared shader ids returned to the front on a pipeline hit (2026-11-17)

- A clean gameplay run on `606a4ac78` (quiet host, no profiler): 18.65 fps, 12.82 us per draw
  (binding 6.91 us), against 14.24 us and binding 7.84 us in a run of the previous table build.
  Single runs; no fps claim.
- The draw front builds both shader ids; on a pipeline cache hit the lookup key moved them out of
  the front record and freed them on the command-processor thread (`~ShaderId` about 1 % of it).
  A hit now moves them back into the record, so the front thread that built them frees them when it
  reuses the record. A miss still stores its own copy.
- Profile in gameplay (about 3,100 samples each): `~ShaderId` 1.05 % -> 0 of the command-processor
  thread, now on the two front threads; `CreatePipeline` 7.2 % -> 4.9 %; pipeline setup
  1.11 -> 1.02 us per draw. Captures of this title's level, `PPSA21567 - Astro Bot` dunes and the
  `PPSA17221 - Minecraft` world render as before.
- Pipeline index (`PipelineCache::Find`): the remaining cost is the full `ShaderId` compare of the
  match (`memcmp` about 2.3 %) and the multimap walk (about 1.1 %); the Minecraft record already
  measured about one entry per walk, so a finer index key was not built.
