# Pixel cave platformer

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named descriptively only (`AGENTS.md`).

## Pixel cave platformer: compute fill cost on Intel (2026-10-19)

- **Method.** Intel Arc A770, Mesa ANV 26.2.5, Xe kernel driver. A temporary
  timestamp query was written at bottom of pipe before every render pass and
  dispatch, after every dispatch and between the steps of the uniform-fill
  path. It was read back when the command buffer was reused. The results come
  from one present frame on the wake-up screen; the title is GPU-bound (GPU
  94% busy, command processor 13%). The query stalls the pipe, so small
  segments read high.
- **Frame breakdown before the fix (about 19 ms of GPU per frame).**
  - The title clears its buffers with one compute kernel: it stores a
    16-byte value per invocation, which is a proven uniform fill.
  - There are about 25 such dispatches per frame. One covers a
    3840x2160 RGBA8 target (33 MB), one 3.9 MB, the rest 393 KB.
  - Each fill is written three times on the host:
    - the dispatch into the storage object, which lives in host-cached
      system memory;
    - a clear of the aliased color image;
    - a publication into the imported guest range, which is system memory
      too.

    | Segment (33 MB fill) | GPU time |
    | --- | --- |
    | Dispatch into the storage object | 2.4 ms |
    | Alias lookup and the barrier before the clear (likely the write flush) | 0.8 ms |
    | Clear of the 3840x2160 alias image | 0.17 ms |
    | Publication copy to the guest range | 3.2 ms |

  - The 23 small publications cost about 85 µs each.
  - The render passes themselves take about 3-4 ms.
- **Fix: publish a one-word fill with `vkCmdFillBuffer`.**
  - Every observed fill repeats a single 32-bit word (0, 0xff000000,
    0xffff8080, 0x00ffffff).
  - `GuestDeviceAddressRecordFill` writes that word into the imported range,
    instead of copying the storage object back across the bus.
  - Fills with four different words still use the copy. So does any range
    whose pieces are not 4-byte aligned, or would not keep the word phase.
  - The barriers are unchanged.
  - Results:
    - The 33 MB publication fell from 3.2 ms to 1.9 ms.
    - Small publications fell under 50 µs.
    - GPU time per frame fell from 19.3 ms to 16.8 ms.
    - In 120 s strict runs on the Circle route, the frame-time median on
      the wake-up and jump-tutorial segments fell from 28 ms to 26 ms.
      That run presented frame 4214 at the end, against 3959 before.
- **Open leads, measured but not changed.**
  - Both host copies of a fill live in system memory, so a 4K clear still
    costs about 5 ms of PCIe traffic. On the console it is one write to
    video memory.
  - A device-local storage object would make CPU write-back reads
    uncached. Publishing lazily would need proof that nothing reads the
    range in between.
  - Both are contract changes and need their own evidence.
- **Verification.**
  - 2479 unit tests pass. One earlier full-suite run hung for more than
    20 minutes and was stopped. The rerun passed, so which test hung is
    unknown.
  - `check_emulator_boundaries.py --strict` and `check_graphics_tables.py`
    pass.
  - A strict regression sweep of seven titles ended without a stop or an
    error. The void platformer's late segments rose from about 92 to
    122 fps. The traveller role-playing title's last two segments read
    28 ms median in the sweep against 24 ms before. A 120 s rerun gave
    24 ms with the same draws per segment and the same µs per draw, so
    that difference was noise. The other titles kept their segment fps.
  - The open-world fishing title had one 354 ms gameplay frame on cold
    caches, with no pipeline miss or compile. A warm-cache repeat with
    compaction active had no slow gameplay frame other than the harness
    captures. This matches the open submit stall in the next section.
