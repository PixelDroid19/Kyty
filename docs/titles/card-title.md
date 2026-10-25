# Card title

Title: PPSA21402 - Balatro.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## Card title: streamed music and an offline service retry (2026-10-16)

- Observed (Intel, strict, card title): music and most effects were silent or restarted
  constantly. The audio stream thread was recreated about 1225 times in 40 s. Long runs stopped
  after about four minutes on a full guest thread registry.
- Evidence (live trace of the NGS2 calls): each music voice adds one PCM block with blocks flags
  1 (continue), a sample count covering the whole song and `data_size` 16384, then appends pieces
  of 7.7-8.9 KiB with flags 3 (continue and append). It plays the voice before the first add and
  reads the 56-byte voice state. Kyty refused flags 1 and 3, refused play on an empty voice and
  refused a stereo mastering voice rendering into an eight-channel buffer.
- Change: PCM sampler blocks stream (see `docs/AUDIO.md`, NGS2 sampler voices), play is accepted
  before any block, a stereo mastering voice writes the front pair of an eight-channel buffer, and
  GetState takes the 56-byte layout.
- Evidence (thread leak): the title's network module retries its service init once per second
  while the network state is offline. Each call builds a new activity manager whose event-queue
  thread waits on a condition forever; the old manager is dropped. One blocked guest thread per
  second is guest behavior when offline. Kyty's 256-entry guest thread registry aborted when full,
  although the host thread-local flag already marks guest threads; the registry is removed.
- Verified (Intel, strict, card title, 330 s, in-emulator RMS tap): the mixed output is audible in
  328 of 329 one-second windows, the stream thread is created 14 times in 90 s, and the run
  holds 348 live threads with no abort. Menus run at about 235 fps and an in-game hand at about
  100 fps. Unit tests: `CoreMemoryAlloc.*` and `EmulatorAudio.*` (121 pass).
- The white hovered card seen in one capture is the title's own focus flash (see the graphics
  handoff, Evidence and exclusions), not a rendering defect.
- Regression check (Intel, strict, 100 s, `generic` route): a pixel-art title that imports NGS2 is
  audible in 92 of 97 one-second windows (52 of 95 in an earlier run) and reaches its
  opening scene with no error.
