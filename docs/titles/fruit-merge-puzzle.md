# Fruit-merge puzzle

Title: PPSA38010 - Suika Game.

Per-title records, newest first. `docs/BRINGUP.md` keeps the invariants,
methodology, frontier and the index of every record.

## Fruit-merge puzzle: frozen on the countdown overlay (2026-11-16, fixed)

- Observed (Intel Arc A770, mesa-git ANV, strict run, 60 fps limit): after "Go!!" the
  picture stayed on the countdown overlay while the game kept running (draws per frame grew
  from 8 to about 50, audio and input stayed live).
- Evidence (RenderDoc capture, dispatch dumps): the overlay is a 1920x1080 RGBA8 sRGB render
  target with DCC, composited over the scene with ONE / ONE_MINUS_SRC_ALPHA. The guest clears
  it every frame in two steps: a uniform fill writes DCC key byte 0x10 over the metadata, and a
  compute dispatch stores one word (alpha 0) at the head of every 256-byte block through a
  `BUF_FMT_32_UINT` descriptor with a 256-byte stride. Kyty rejected key 0x10, so the clear
  was dropped and the stale opaque layer covered gameplay.
- Change: key 0x10 is the single-element key (every block reads as its first element). The
  block-head store is recorded as its own event while the dispatch still runs; a bound target
  whose DCC carries the key together with a live head event for the same range is cleared to
  the head word (`GraphicsRenderAttachments.cpp`, `ConsumeColorBlockHeads`).
- Verified: the title reaches the play field and drops fruit with score updates (scripted
  route, 65 s). Minecraft and Astro Bot strict runs reach their worlds and never decode the key.
  `kyty_unit_test` passes (2699 run, 0 failed) with the DCC key tests updated.
- Open: texture reads of a surface under key 0x10 still drop the clear. The guest also writes
  R8 and RG8 typed stores (strides 1 and 2) into the DCC of the display buffers; those
  descriptors are outside the supported storage formats and only reach the storage frontier.
