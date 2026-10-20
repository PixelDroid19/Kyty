# Anime fighter

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named descriptively only (`AGENTS.md`).

## Anime fighter: storage write-back on the label path (2026-10-17)

- **Observation (Intel, native 3840x2160 output).** ~8.3 fps, frame p50
  118 ms. Two guest threads spin at 93% CPU on single loops. The GPU is idle
  ~75% of the time, and rendering at fixed 1080p gives the same 8.4 fps, so the
  title is not pixel-bound. Per 10 s: ~2,000 dispatches, and each writes back
  one ~15.7 MB storage buffer (about 22 per frame).
- **Mechanism.** The end-of-pipe label the guest waits on is written only
  after the completion write-back. Each write-back cost three things:
  - a page compare of 15.7 MB against the write-back snapshot (1.84 ms);
  - a full XXH3 hash of the range (0.46 ms);
  - another full hash at the next use, because the write-back's own write
    notifications advanced the dirty generation and nothing re-baselined it
    (`hash_tracked_unchanged` = 1,871 of 1,871).

  That came to 58.9 GB hashed and 3.45 s of write-back per 10 s, serialized
  with the guest's waits.
- **Changes.**
  1. After a write-back with changed content, a tracked range re-baselines
     its dirty generation, as `Update` already does after an unchanged compare.
     The range is not hashed: a later guest write compares against the unknown
     (zero) hash and uploads its changed pages. Page generations stay stale,
     so label-fence holes are still uploaded.
  2. Ranges of 256 pages or more compare their pages on up to three helper
     threads. The copy and its write notifications stay on the calling thread,
     in page order.
- **Measured (same 10 s window at 55 s).**

  | Build | Frame p50 | fps | Hashed per 10 s | Write-back per call |
  | --- | --- | --- | --- | --- |
  | before | 118 ms | 8.3 | 58.9 GB | 1.84 ms |
  | + generation baseline | 100 ms | 9.9 | 1.2 GB | 1.84 ms |
  | + parallel compare | 83 ms | 11.6 | 1.4 GB | 1.26 ms |

  Captures are visually unchanged.
- **Completion polling.** The compute and graphics rings noticed a finished
  submission only on their next 1 ms condition-variable timeout, so every
  label round trip paid up to 1 ms of idle latency. A 100 us poll (experiment,
  not kept) moved p50 from 83 to 64 ms, which confirmed the cost.
- **Change 3.** Every queue owns a timeline semaphore; each command-processor
  submission signals the next value in submission order. A ring with pending
  work arms a per-ring waiter thread on the oldest submission's value; the
  thread blocks in `vkWaitSemaphores` and wakes the ring as soon as the GPU
  finishes. Completion is still pumped by the ring itself, under its own
  locks, and the 1 ms timeout stays as the backstop.

  | Build | Frame p50 | Frame p99 | fps |
  | --- | --- | --- | --- |
  | parallel compare | 83 ms | 108 ms | 11.6 |
  | + timeline wake | 66 ms | 88 ms | 15.3 |

  Sandbox (terrain, 6-7.5 fps in world), racing (7 captures) and the puzzle
  title (cold-compile window) show the same states and captures as before.
  Verified on Intel only; RADV is untested.
- **Open.** The compare reads 31 MB per call and is now close to memory
  bandwidth. Removing the compare needs knowledge of which pages the dispatch
  wrote.
