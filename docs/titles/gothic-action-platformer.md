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

### Status and how to continue (2026-11-17)

Summary of the records below, for whoever picks this up next. Intel Arc A770, Mesa ANV, native
resolution, `KYTY_PRESENT_FPS_LIMIT=60`, strict runs, gameplay route that reaches the first level
at about 160 s and samples 150-175 s.

**Where it stands.** Gameplay runs at 18.7-19.3 fps, against 60 fps on the console. The scene
draws about 3,400 draws per frame in about 158 guest command batches. Not failing: the title is
playable and renders correctly in gameplay; `kyty_agent playable-regression` passes 12 of 15
gates, the same three as the reference. Still failing: the frame rate, and the black new-game
video above.

**Why it is not at native speed.** The command-processor thread limits it, not the GPU:
- Half resolution does not raise the frame rate, and device timestamps show the GPU idle between
  submits for about half of each frame. Its work fits about 40 fps (see "Budget for 30 fps").
- One thread decodes every packet and, for every draw, resolves textures, storage buffers,
  samplers and render targets into memory objects in guest order, then looks up descriptor sets.
  That costs about 12.5 us per draw, about 43 ms of a 52 ms frame. The console does none of this:
  its GPU reads the guest descriptors directly.
- A frame costs:
  - about 28 ms in ordinary prepared draws (8.4 us each, 5.4 us of it resource binding);
  - about 13-14 ms at batch boundaries (the first draw of each batch costs about 65 us: a missing
    front record and a new command buffer);
  - flush and submit, about 12 us per batch.
- No memory object is created or freed per frame. The cost is the volume of ordered lookups
  (about four per draw) and descriptor work, not churn.

**Done on this branch** (each record below has its numbers):
- Two draw fronts prepare shader inputs ahead of the processor.
- Shader ids are carried on the front record.
- A direct-mapped table holds recent materializations.
- Texture-stage results are reused within a command buffer (71 % of binds).
- The draw fronts are woken only from a full ring (+4 %, one pair of runs, indicative).

**Excluded or parked** (do not reopen without new evidence):
- Starting the fronts on the queued batch: the host-write epoch advances before the first draw in
  83 % of boundaries.
- A front-start spin, and texture-stage reuse across command buffers.
- Skipping target resolution on a render-pass continuation (0.26-0.4 us per draw,
  `docs/bringup/performance-architecture-plan.md`).
- A whole descriptor-set memo: storage words change on most draws.
- Lock-free reuse of sampled-texture requests (parked).
- Merging batches into one command buffer: about 3 ms per frame, parked because it changes
  submission order for every title.
- The draw-tail record thread is already built (one replay thread per pool), so splitting
  recording off the processor is not a new lever.

**Ceiling of what is priced.** Every local lever together reaches about 21-22 fps: merging
batches, handing the submit to the replay thread, the sampler-id lookup. 25 fps needs about 2 us
less per ordinary draw beyond that, and 30 fps about 5 us, mostly from resource binding. No
priced lever does that today.

**Next directions, in order:**
1. A freshness rule narrower than the global host-write epoch: only the pages a front record read
   invalidate it. Each epoch advance now discards the fronts' work, at mid-run and at every batch
   start. Count first which writes advance the epoch.
2. Merging consecutive batches that share render targets into one command buffer, behind the
   existing completion and wait contracts. Price it on Minecraft and Astro Bot first, because it
   changes submission order everywhere.
3. A binding model that leaves a draw whose descriptors did not change without per-draw
   resolution. This is the only direction that reaches 30 fps. It needs the object-removal signal
   understood first: removals moved in 74 % of cross-buffer binds, cause not found.

Temporary diagnostics used for these records were never committed; they are kept with the run
evidence outside the repository.

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
  profile (gameplay, about 3,100 samples each, both with `PrintfDirection = Silent`; the second
  also has the shader-id change below): `CreateObject` under `PrepareTextures` 14.6 % -> 12.1 %. Captures of `PPSA21567 - Astro Bot` dunes and the
  `PPSA17221 - Minecraft` world render as before. No fps claim (perf-recorded run).
- Open: most stale table probes come from cache-slot evictions bumping the removal counter; split
  that counter before changing the validity test.

### Prepared shader ids returned to the front on a pipeline hit (2026-11-17)

- Gameplay runs (quiet host, profiler attached): `d64a0b11b` 17.62 fps, 13.69 us per draw against
  the table and this change 18.5 fps, 12.85 us. Same-code runs today differed by up to 1.6 us per
  draw, so this pair is within run spread; the attributable part is the profile (about 3.5 % of
  the command-processor thread). Without the profiler,
  `606a4ac78` ran 18.65 fps at 12.82 us. A run without the explicit setting gave the same
  (12.81 us; the guest runner defaults to `Silent`). Single runs; earlier runs of the day at
  14.2-14.4 us carried temporary diagnostics or ran beside other host load and are not compared.
- The draw front builds both shader ids; on a pipeline cache hit the lookup key moved them out of
  the front record and freed them on the command-processor thread (`~ShaderId` about 1 % of it).
  A hit now moves them back into the record, so the front thread that built them frees them when it
  reuses the record. A miss still stores its own copy.
- Profile in gameplay (about 3,100 samples each, `Silent`): `~ShaderId` 1.05 % -> 0 of the
  command-processor thread, now on the two front threads; `CreatePipeline` 5.35 % -> 4.87 %; pixel
  input analysis on the command-processor thread (front misses) 7.3 % -> 6.9 %, so the fronts do
  not fall behind more often. Captures of this title's level, `PPSA21567 - Astro Bot` dunes and the
  `PPSA17221 - Minecraft` world render as before.
- Pipeline index (`PipelineCache::Find`): the remaining cost is the full `ShaderId` compare of the
  match (`memcmp` about 2.3 %) and the multimap walk (about 1.1 %); the Minecraft record already
  measured about one entry per walk, so a finer index key was not built.

### Excluded: skipping the memory-object request on a repeated texture bind (2026-11-17, code reading)

- The texture-stage reuse record left open the side effects of skipping `CreateObject`. A shadow run
  found no repeat where the locked path took anything but the cached reuse, but that reuse is not a
  no-op for sampled textures: they arrive with `read_only` false, so every reuse adds one to
  `write_uses` and sets `write_time`.
- Both fields order later work. `write_time` picks the newest texel-bit source and rejects seeds
  written after it when a new view is created (`GpuMemoryCreate.cpp:1030`, `:1049`), orders the
  render-alias sources (`:2272`) and is compared with device writes on write-back
  (`GpuMemoryWriteback.cpp:363`); `write_uses` is part of the device-address overlay identity
  (`GuestDeviceAddress.cpp:1945`, `:2089`). Skipping the request would change those decisions, so
  the reuse stays excluded while sampled textures count as writable uses.
- Whether a sampled-only bind should count as a write at all is a question about the memory-object
  model, not about this lever; it was not examined here.
- Follow-up (2026-11-17): of the `write_time` readers above, only two can see a sampled `Texture`
  bump: the seed rejection in `create_texel_bits_seed` (`GpuMemoryCreate.cpp:1049`) and the
  superseded-parent test of storage-array growth (`:1836`). `create_raw_source_write_time` ignores
  `write_time` for textures, write-back never runs on them (no write-back function), and the
  write-back alias check reads the written-back object's own time. A temporary shadow `write_time`
  that sampled-texture uses did not bump computed both decisions both ways. It printed only on a
  difference or every 4,096 calls per site: nothing printed in gameplay of this title,
  `PPSA21567 - Astro Bot` dunes, the `PPSA17221 - Minecraft` world or 90 s of
  `PPSA15552 - Dead Cells` menus (no input route), and a difference forced at the 50th seed call
  never printed in this title or Minecraft, so the seed site ran fewer than 50 times there and each
  site fewer than 4,096 times everywhere. A call forced by hand printed, so the counter engaged.
  These runs barely reach the two sites; they do not show the bump is unused where the sites were
  written for. Whether a sampled-only bind should stop counting as a write remains a model change
  that needs those titles and the full regression set.
- Counted rerun (2026-11-17, counter printed at every power of two and counted the calls that
  examined a `Texture` and those where its two times differed): `PPSA21567 - Astro Bot` dunes,
  seed site 1,024 calls with no `Texture` among the overlaps, growth site 128 calls with one
  `Texture` parent whose times differed; `PPSA09809 - OCTOPATH TRAVELER II` title screen (no
  input route), seed site never called, growth site 512 calls with five `Texture` parents whose
  times differed. No decision changed. The other readers cannot see the bump: the storage-cache
  test (`GpuMemoryCreate.cpp:1530`) reads only storage buffers, storage-image retirement
  (`GpuMemoryWriteback.cpp:142`) and the in-place fill identity (`:1005`,
  `GraphicsRenderAttachments.cpp:121`) read only storage objects, and only the two sampled-image
  binds in `GraphicsRenderBind.cpp` request a `Texture`. The seed site is still untested with a
  `Texture` overlap.
- Correction and open items (2026-11-17, code reading): the raw-render composite order
  (`GpuMemoryCreate.cpp:2272`) reads `create_raw_source_write_time`, which takes `write_time` only
  from storage images, so it cannot see the bump. Not yet confirmed: the type gates of the
  write-back readers at `GpuMemoryWriteback.cpp:611` and `:768` (both cast to a buffer). Not yet
  read: whether the depth-materialized sampled bind (`GraphicsRenderBind.cpp:4727`) and the
  mip-generation texture (`Texture.cpp:1394`) receive GPU copies recorded only by `write_time`; a
  rule limited to textures uploaded from guest bytes would avoid that question.

### Parked: lock-free reuse of sampled-texture requests (2026-11-17)

- A per-thread memo in front of `GpuMemory::CreateObject` would skip only cached hits. The cached
  hit path never calls `Update`, the fast search or the cache store, and the cache lookup runs only
  when the recent table misses, so a memo hit saves the two locks, the recent probe, the recheck
  and the use record while paying its own key build and probe. Profile of the command-processor
  thread (table and returned shader ids, `Silent`): `CreateObject` 15.5 % of the thread for every
  object type (12.1 % under `PrepareTextures`); `Update` 2.6 % (dirty-page tracker map lookups),
  debug-stat timing 3 %, locks 1 %. At the measured 71 % would-hit rate the memo is worth about
  0.7 us per draw, under 1 fps, and it needs the same model change (sampled textures stop counting
  as writes) and full regression set as texture-stage reuse. Not built.
- The whole texture route is bounded by about 0.71 x 2.7 = 1.9 us per draw, roughly 21 fps. The
  `Update` lever (skip the tracker lookups while its evidence version holds) is open and unpriced,
  at most 2.6 % of the thread.

### Budget for 30 fps (2026-11-17, existing gameplay samples and profile)

- GPU: render-engine busy 0.64-0.80 of each second at 18-19 flips per second (`samples.jsonl`
  of two gameplay runs), about 37 ms of engine time per frame. A GPU-bound scene of
  `PPSA21567 - Astro Bot` read 84-88 % on the same counter, so the GPU cap here was extrapolated
  at 23-26 fps (withdrawn below: device timestamps).
- Render scale test (one gameplay run, `Fixed` / `RenderTargets` 960x540, same scene: 3,369 draws
  per frame): 15.8 fps (native runs 18-18.6), draw processor 15.3 us per draw, render-engine busy
  0.36-0.51, about 28 ms of engine time per frame against about 37 ms at native. The scene is
  command-processor bound: 15.3 us x 3,369 draws plus about 10 ms outside draws predicts about
  16 fps. At least a quarter of the GPU frame time follows resolution (scaled cohorts may demote
  to native and add resample copies; the run reports neither count). The slower draw processor in
  this run is one sample and is not explained.
- Fit of render-engine busy per second (100 seconds of two native gameplay runs, this run and an
  earlier half-resolution run that landed in a lighter scene, 1,200 draws per frame at 41 fps,
  about 18 ms of engine time per frame), R^2 0.93: about 11 ms per flip at either resolution,
  10 ms more per flip at native, and 83 us per render-pass begin or 88 us per submit (the two are
  0.97 correlated, so the fit cannot separate them; draws alone fit slightly worse). In the heavy
  scene (about 197 pass begins and 174 submits per frame) that term is about 15-16 ms of the
  37 ms. Whether it is engine work (flushes between batches) or idle time between submits charged
  as busy is not known.
- Device timestamps (temporary diagnostic run, native resolution, same scene, about 3,400 draws per
  frame): a timestamp at the first and last command of every primary command buffer, read in
  windows of 2,048 buffers (about 14 frames at 147 submits per frame). In the 18-19 fps seconds
  the union of in-buffer intervals covered 0.41-0.44 of each window, about 23-24 ms per frame; the
  rest is the device waiting for the next submit. The render-engine counter read 0.45-0.48 there,
  against 0.64-0.80 in the two earlier native runs at the same frame rate, and the half-resolution
  run cost about 28 ms per frame of engine time, so busy time per frame is not a fixed amount of
  work across runs (device frequency is a lead: the sysfs actual-frequency file reads 0 when idle,
  not logged per second yet). The 23-26 fps GPU cap and the per-pass fit above are withdrawn; on
  this run the GPU work fits about 40 fps, and the scene is bound by the command processor (timer
  prediction, half-resolution run and idle device time agree). Later in the run the frame rate fell
  to 9-11 fps at the same 3,300-3,800 draws per frame while engine time rose to about 30 ms per
  frame; the cause is unexplained and those seconds are excluded.
- Command processor (timers): 12.85 us per draw in the draw processor, of which binding 6.9 us,
  plus about 10 ms per frame outside draws, at about 3,400 draws per frame. 30 fps leaves about
  1 us per draw for binding with the rest unchanged, so nearly all of it must leave the thread or
  disappear, against the 10-15 % that is free of order today
  (`docs/bringup/parallel-recording-design.md`, "Descriptor binding split by order"). Profile
  shares (stacks truncated, so lower bounds): `BindDescriptors` 47 % (textures 22 %, descriptor
  cache 10.8 %, storage 7.3 %, samplers 2.2 %), driver 8.3 %, buffer flush and submit about
  11 %, shader input analysis about 11 % (pixel 6.9 %, vertex 2.4 %, redefined buffers 1.6 %).

### Whole-bind repeat shadow (2026-11-17, temporary diagnostic, gameplay, native resolution)

- Each graphics `BindDescriptors` call hashed four parts of its inputs (storage-buffer words; the
  used texture and sampler words with layout, shader and flags; direct SGPRs with GDS pointers;
  layout, shader and flags), each in its own 65,536-slot table per stage, and compared the image
  and sampler arrays handed to the descriptor cache against the last call with the same image key
  while the object-graph, guest-bytes and materialization-removal counters were unchanged.
- 13.1 M binds. Same key as the previous call with that key: images and samplers 99.99 % (vertex)
  and 99.9 % (pixel); storage buffers 23 % and 32 %; direct SGPRs 14 % and 100 %; layout and
  shader about 100 %. With the counters unchanged the image key held 68 % of binds; the misses
  were the removal counter 3.96 M, guest bytes 1.45 M and the object graph 41 k. Among 8.9 M
  valid image hits the arrays differed once, the case forced by hand.
- The stale counters overlap: the removal counter moved in every invalid repeat (3.96 M of
  3.96 M) and guest bytes in a subset, so a removal counter that ignored evictions would lift the
  valid share to at most about 87 %. The zero output differences cover only binds with the
  counters unchanged, and only the image pointers, the samplers and the 2D sampled view indices,
  not the other view arrays; outputs in the removal-stale binds were not compared.
- Price: on the command-processor profile `PrepareTextures` is 22 % of samples (about 3.8 us of
  12.85 us per draw with the sampler stage); a hit still owes the cache lock, the dirty-page check
  in `Update` and the use records, about 4 of those 22 points. At 68 % valid that saves about
  2 us per draw (2.7 us if evictions stopped invalidating), the same order as the parked bound
  above (1.9 us).
- Bounds at 3,000-3,300 draws per frame and about 10 ms per frame outside draws:
  - deleting the texture and sampler stages outright leaves about 9.1 us per draw: 25-27 fps;
  - also removing the emulator's descriptor set cache leaves about 7.5 us: 29-31 fps. That cache's
    share is at most 1.6 us here (finalize is 2.0 us per draw, about 0.4 us of it driver binding);
    the 2 us figure in `docs/bringup/cp-draw-plan.md`, "Dynamic offsets and descriptor buffers on
    Intel Arc", was measured on the DREDGE dock with 2.71 us of finalize;
  - expected with an image memo at 68-87 % valid binds plus that cache removed: 25-28 fps.
  - Measured later ("Texture-stage reuse" below): the shipped reuse saves 0.7-1.2 us per draw of
    binding, not 2-2.7 us, because a full path that can be reused costs about 1.3 us, not the
    3.8 us average, and a miss adds about 0.4 us; the 25-28 fps expectation does not hold.
  - Storage words change on most draws, so a whole-set memo cannot hit without them leaving the
    set. The rest of the way to 30 fps has to come from state setup (3.3 us per draw), pipeline
    setup (1.2 us) or the time outside draws.
- GPU clock (sysfs actual frequency averaged per second; first diagnostic run, 2,560-3,700 draws
  per frame): 1,030-2,270 MHz of a 2,400 MHz maximum. The rate held at 11-13 fps both at
  1,030 MHz and at 1,945-2,240 MHz, so it did not follow the clock; the clock falls while the
  device idles. Busy time scaled to the full clock (busy x actual / maximum, linear scaling
  assumed) is 17-23 ms per frame below 3,000 draws and 26-28 ms per frame at 3,400-3,500 draws,
  a GPU limit of about 36-38 fps on the gameplay scene if that scaling holds.

### Texture-stage repeat shadow restricted to plain binds (2026-11-17, temporary diagnostic, native)

- Each `PrepareTextures` call was keyed by its full texture and sampler resources, the seed-skip
  mask, the stencil-attached depth image, the pass colour contents, the stage and the scaled-sampling
  refusal; valid while the command buffer, submit, object-graph, guest-bytes and removal counters
  were unchanged. A call was "plain" when it reached none of the thirteen sites that record on the
  command buffer or stamp an image (stencil and depth plane seeds, depth slice arrays, scaled
  demotion, CPU-write merges, inline D16 detile, block copies, storage prefix-layer copies, DCC
  clear consumption, storage write stamps) and every `GpuMemoryCreateObject` inside was a cached
  reuse. Only plain calls were stored; outputs compared were all image and view arrays, the SGPR
  words written and the seed-skip mask, with one forced difference of each kind (all three seen).

| Workload | Calls | Plain | Would reuse | Of which not plain | Output differences |
|---|---|---|---|---|---|
| PPSA35399 - Castlevania: Belmont's Curse, gameplay | 12.3 M | 86 % | 72 % | 9 (before the buffer and submit check) | 0 |
| PPSA21567 - Astro Bot, dunes | 1.31 M | 38 % | 29 % | 0 | 0 |
| PPSA17221 - Minecraft, world | 4.98 M | 75 % | 71 % | 0 | 0 |

- The side-work sites fired on Astro (depth slices 118 k, CPU-write merges 678 k, prefix-layer
  copies 116 k, DCC clears 151 k, storage stamps 130 k, non-cached creates 208 k) and Minecraft,
  so the instrument engaged; none of them fired on a would-reuse call. Every reuse was inside one
  submit, and the removal counter again explained nearly all stale repeats.
- Hashing the 3,908-byte texture and 488-byte sampler resources cost about 0.9 us per call in
  this diagnostic. The shipped reuse compares the whole resources with `memcmp` instead (0.16-0.37
  us per reuse including the object recheck), which is cheap enough without a hash.
- Skipping `GpuMemoryCreateObject` on a reuse is not neutral: a sampled texture's cached hit bumps
  its write time, which the texel-seed check and the superseded-parent check read, and records its
  use for deferred deletion (a per-queue high water, so a repeat in the same submission is a
  no-op). A reuse therefore has to repeat those cached-hit updates on the same objects.

### Texture-stage reuse (2026-11-17, native, 60 fps limit)

- A vertex or pixel bind whose texture and sampler resources, seed-skip mask, stencil-attached
  depth, pass colour, scaled-sampling refusal and stage equal an earlier bind's in the same
  command buffer and submission takes that bind's images, views and SGPR words instead of running
  `PrepareTextures`. The earlier bind must have recorded no side work (any of the side-work sites,
  or an image stamp comparison), and every object it resolved must have been a cached reuse, while
  the object-graph, guest-bytes, materialization-removal counters and the frame stayed unchanged.
  The reuse rechecks each object (free, scenario, logical generation, submit, in use, check hash)
  and repeats the cached-hit updates on it, then repeats the bind's image reads. Compute binds,
  engaged material or render-target traces and the opt-in texture dumps take the full path.
- A first version did not recheck the object's check hash; another bind can change it in between,
  and the full path then is not a cached reuse. The comparator below found it on Astro (27 calls)
  before the recheck was added.
- Comparator (temporary diagnostic): every reusable bind was also replayed into scratch outputs and
  the full path then ran; images, views, SGPR words, the read list and the captured object uses were
  compared, with one forced difference of each kind (all five seen). Castlevania and the second
  Minecraft run used the final logic and also classified every full path that was not plain.

| Workload | Reused | Output differences | Read or use differences | Reuse whose full path was not plain | Replay | Full path when reused |
|---|---|---|---|---|---|---|
| PPSA35399 - Castlevania: Belmont's Curse, gameplay | 71 % | 0 | 0 (245 moved) | 0 | 0.29 us | 1.3 us |
| PPSA21567 - Astro Bot, dunes | 20 % | 0 | 0 (5 moved) | 0 | 0.37 us | 1.5 us |
| PPSA17221 - Minecraft, world | 61-70 % | 0 | 0 (23 and 52 moved) | 0 | 0.16-0.18 us | 0.47-0.52 us |

- "Moved": a registry counter changed while the full path itself ran, so its capture was dropped;
  outputs matched. Classified on Castlevania and Minecraft: none had side work and none had a
  non-cached object; the guest-bytes or frame counter moved in all of them, and in the 3 where the
  graph or removal counter also moved the frame moved too (one of them resolved no object at all),
  which points to a change from outside the call. A missed reuse costs about 0.1 us of lookup plus 0.15-0.32
  us to capture and store, so Astro is neutral (reuse saves 176 ms, misses cost 155 ms per 786 k
  calls), Minecraft gains about 0.13 us per call and Castlevania about 0.5 us per call.
- Castlevania A/B against the parent build, same route, alternating (draw processor and binding,
  us per draw): reuse 12.51/6.39 and 13.64/6.95, parent 13.98/7.64 and 7.88 in an earlier run; one
  parent run with every untouched timer 25-30 % higher was discarded. Binding falls by 0.7-1.2 us
  per draw; the frame-rate gain is within the run-to-run noise and not established.
- Captures of the reuse build: Castlevania gameplay level, Astro dunes (three shots) and the
  Minecraft world render correctly. One Astro run ended with status 139 at the `hub_crashsite_tutorial`
  load, the title's own command-buffer assert recorded above when the route runs late; the rerun
  reached the dunes and exited cleanly.
- `GpuMemoryCreateObject` statistics no longer count the cached reuses a bind reuse repeats.
- `kyty_agent playable-regression` (PPSA06084 - Superliminal, strict, 60 fps limit) on the reuse
  commit: 12 of 15 gates against 10 of 15 for the morning reference; the two extra passes are the
  input gates (`input_delivered`, `input_sequence_complete`), which vary between runs. The gate
  still exits 3 on the same three gates that fail in the reference (`visual_compare`,
  `scene_checkpoint`, `action_response`).
- Open: one Castlevania run with compute binds also reused stayed on a one-draw screen for its
  whole 185 s; unexplained, so compute binds stay on the full path. Pixel binds were not reused
  at first because every pixel bind passes a trace context; only an engaged trace now bypasses.

### Draw fronts woken only from a full ring (2026-11-17, native, 60 fps limit)

The command-processor thread was on the CPU 95 % of the gameplay window, 9 % of it in the kernel
(per-thread `utime`/`stime`/`schedstat`, 23 s window); the lock waits that `sync-waits` would show
are small (about 1 s blocked in 23 s). A temporary timer charged
that kernel time:

- `ReleaseDrawFronts`, after every draw, took 0.73 us per call and issued 1.2 wakes per call; each
  front thread switched out about 28,000 times a second. `DrawFront::Release` woke a front whenever
  it slept with free ring space, including a front sleeping for its next job, which has nothing to
  read until `Start`; that front woke, found no job and slept again.
- `vkQueueSubmit` took about 12 us per submit, about 2 ms per frame.

Fix: the replay marks the full-ring wait (`m_parked`), and `Release` wakes only a parked front;
`Start` still wakes a front waiting for a job. Same diagnostic, fix applied: `ReleaseDrawFronts`
0.034 us per call and 0.095 wakes per call (that run reached a lighter scene, about 2,000 draws per
frame, so its per-window totals are not compared). A/B against the parent build, same route,
one run each: 18.0 -> 18.7 fps (3,400 draws per frame in both), draw processor 13.09 -> 12.32 us
per draw; one pair on a noisy host, so the gain is indicative. Astro dunes (three captures) and the
Minecraft world render correctly; 414 packet and front unit tests and the four CTest gate tests pass.
Repeated on a clean build of the branch head after the boundary records: Astro reached the dunes
(different shot, 620 draws per frame, so fps not compared) with the same storage-frontier event as
every earlier run of the route (capture checked), and the Minecraft world held 48.5 fps (60 fps
limit). `kyty_agent playable-regression` (PPSA06084 - Superliminal, strict) on that head: 12 of 15
gates, the same three failures as the reference (visual compare, scene checkpoint, action response).

Texture-stage reuse misses in the same window (fingerprint without the command buffer): 21 % of
binds had the same inputs stored under another command buffer (an upper bound: the frame,
removal and guest-bytes counters were not checked for this class), 0.15 % under another submission of
the same buffer, 1.5 % never seen; 3.7 % matched but a recorded object had changed. Command-buffer
turnover is the main source of misses; reusing across buffers needs the use records and image
barriers proven for a new buffer, not built.

### Excluded: front misses at run starts, and texture-stage reuse across command buffers (2026-11-17)

Temporary counters, same gameplay route, 16,384 runs in the window:

- Draw fronts at run starts. The start draw of each run is prepared by the command processor
  itself (4.7 % of draws, one run per batch). After it, the first front hit is the next draw in
  98.5 % of runs; draws without a front record before the first hit are 0.5 % of all draws, and
  after it 1.0 %. Front wake-up latency does not cost draws, so a spinning job wait would gain
  nothing. The start draws themselves are not cheap: see the next section.
- Texture-stage reuse across command buffers. Of the binds whose inputs were stored under another
  command buffer (21 % of binds), 0.006 % had the frame, materialization removals, guest bytes and
  object graph unchanged; materialization removals moved in 74 % and the frame in 26 %. Reuse across
  buffers would need removals tracked per entry instead of one global counter; even then the frame
  limit leaves at most about 1 ms per frame. Not built.

### Cost of a batch boundary (2026-11-17, temporary counters, native, 60 fps limit)

A run is one guest command batch, and each batch records into a new command buffer. Timed on the
command-processor thread (stage timers read before and after each draw; prepared draws sampled one
in 64), in microseconds per draw:

| Stage | Prepared | Not prepared, mid-run | Not prepared, first in a new buffer |
| --- | --- | --- | --- |
| State setup | 0.99 | 22.5 | 32.0 |
| Resource binding (textures) | 5.6 (1.7) | 9.6 (3.7) | 20.6 (11.4) |
| Pipeline setup | 0.92 | 3.0 | 4.1 |
| Command emission | 0.22 | 0.48 | 6.1 |
| Materialization | 0.52 | 0.59 | 2.4 |

- The first draw of a run took 65 us against 8.4 us for an ordinary draw (whole `DrawIndex`,
  CPU time equal to wall time, no submit inside it). At about 158 runs per frame that is about
  9 ms of a 52 ms frame, before the batch's flush and submit (about 12 us per submit).
- About 28 us of the extra is the missing front record (shader input analysis on the processor);
  about 29 us coincides with the new command buffer. A forced new command buffer every 64th draw
  (prepared draws, same batch) measured that part alone: +13 us (binding +6.2, of it textures +3.9;
  emission +3.8; state +1.7; materialization +1.0), with 58 more command buffers per frame. The
  expected net change, about 0.75 ms per frame, is not resolvable against the spread between runs
  (processor draw time 43.0 against 43.2 ms). About 15 us of a batch start
  is explained by neither.
- Fronts woke within 8 us of a start in 98 % of starts and had their first record ready 16-32 us
  after it in about half (77 % within 32 us, 98 % within 64 us).
- No constant-stream run carried packets in the route (the stream is empty on this title), so a
  batch boundary has no state change between two draw-stream runs.
- Register state at the start of each top-level run equalled the state at the end of the previous
  one in all 896,147 boundaries compared (context, user config and shader registers). The
  comparison is between consecutive top-level runs, so a change made inside a constant-stream run
  between two draw-stream runs is not excluded by it.
- Draws without a front record in mid-run are 1.6 % of draws, and cost about 28 us more than a
  prepared draw.

- The index type, size and base, the instance count and the user-data marker that a front start
  copies were also equal at every boundary.
- At the end of a batch's streams (before its flush) the next batch was already queued in 70 % of
  batches; from there to the next run's first draw took a mean 50 us (most 32-64 us), the time a
  front started on the queued batch would have to prepare that draw (about 22 us of analysis plus
  a thread wake). Without a queued batch the gap was 72 us, mostly waiting for the batch.
- Mid-run draws without a front record: 58 % had a record prepared before a host write moved the
  epoch (rejected as stale), 42 % had no record at or past the draw (not prepared yet, or declined
  by the front); none differed in epoch or instance count at the record.
- Command buffers per frame 173, render-pass begins 196: nearly every batch begins its own render
  pass, so the emission and materialization part of the boundary may follow the target change and
  not the buffer; merging batches would recover it only where consecutive batches share targets.

Bound: removing the whole boundary cost gives about 38 ms per frame (about 26 fps). With the
front start excluded below, what remains is merging batches into one command buffer: about 3 ms
per frame (the submit and part of the 13 us buffer cost; render-pass begins, 196 per frame against
173 buffers, stay), at the price of changing submission and completion order for every title. A front continuing into the queued batch recovers at
most the analysis part, about 24 us x 0.7 x 158, 2.7 ms per frame (about +5 %).

### Excluded: starting the fronts on the queued batch (2026-11-17)

Counted at each batch end with a queued next batch (258,048 boundaries in the route): the next
draw stream reached its first indexed draw without an ordering packet in 90 % of them, but the
host-write epoch advanced before that draw in 83 % of those (cause not counted). `Take` rejects every record analysed before an advance, so a
front started at the batch end would serve the first draw in about 15 % of queued boundaries:
about 0.4 ms per frame. Starting them after the flush leaves under 20 us of lead against 16-32 us
to a first record. Not built. Reopen
only with a freshness rule narrower than the global epoch, which would also serve the mid-run
records rejected as stale.

### Where the remaining processor time goes (2026-11-17, existing counters)

- The draw tail (pipeline, binds, push constants, draw) already records on the per-pool replay
  thread on render-pass continuations, about 3,200 of 3,400 draws per frame here; command emission
  on the processor is 0.22 us per prepared draw. Moving the rest of recording to that thread
  leaves only the command-buffer end and queue submit, about 2 ms per frame.
- Memory objects in the gameplay window, per frame: no object was created, reclaimed or freed
  (new and reclaim counters 0.1 or less for every type); about 13,900 lookups were served from
  the caches (texture 7,900, depth 3,370, render target 2,520, storage 120), about four per draw;
  at most about 5 ms per frame, estimated from one sample row's create time per call (cached reuses
  are not shown to pass that timer). Storage resolution and descriptor finalize (about 9 ms per
  frame) are not object lookup: storage lookups are 120 per frame.
- The processor cost is the volume of ordered per-draw resolution, not object creation (an earlier
  record found the removal counter moving in 74 % of cross-buffer binds, so the object set is not
  shown unchanged). With every local lever counted (batch merge, submit hand-off, sampler ids), the
  estimate ends near 21-22 fps; no priced lever reaches 30 fps (texture-stage reuse already skips
  resolution for 71 % of texture binds, and a whole-set memo cannot hit).
