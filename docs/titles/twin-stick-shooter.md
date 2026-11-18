# Twin-stick shooter

Title: PPSA03565 - Hentai vs. Evil.

Records for this title, newest first. The bring-up manual keeps the
invariants, methodology, frontier and an index of every record. Titles are
named by PS5 content ID and public name (`AGENTS.md`).

## First strict runs: the menu is reached, play is not (2026-11-17, open)

Intel Arc A770, Mesa ANV on Xe, 60 fps present limit, strict runs, a fresh save directory per run.

- **Fixed: ATRAC9 decode of a buffer ending inside a superframe (`06a640f5a`).** The title exited at about
  11 s with "ATRAC9 job extent is not a whole number of samples or superframes". It streams music with one
  decode per 65,536-byte window at a 320-byte block alignment and 32,768 output bytes. The decode now
  consumes the whole superframes that fit the output. A trace of 60 consecutive jobs showed every job
  starting at the previous address plus the reported bytes consumed (2,560), so the guest advances by that
  count and the trailing bytes are resubmitted.
- **Fixed: 16_16_16_16_UINT storage image (`8b18596cd`).** After PLAY on the main menu, the process exited with
  "unsupported storage image declaration: format=69". The format gets the core `Rgba16ui` view.
- **Fixed: false storage frontier.** Every run reported `gfx_storage_frontier` for a stride-4 32_SINT buffer
  view. The bind was unaffected (raw path), but the event pointed diagnosis at a non-cause. 32_SINT is now
  admitted as a scalar 32-bit format with 32_UINT and 32_FLOAT.
- **Open: the main menu is nearly black at about 17 fps.** The menu text and logo draw; the background is
  black with a noisy horizontal band through the centre. Before the menu, presents stop for about 16 s
  while three guest threads run.
- **Open: after PLAY the title crawls at 0.1-0.8 fps.** The black screen presents one frame every few
  seconds; the run accumulated 14.8 s of fence waits.
- **Open: render-engine resets.** Two of the runs ended in an xe `Engine reset: engine_class=rcs` and
  `Timedout job`, followed by `VK_ERROR_DEVICE_LOST`: one at about 10 s (before the menu, before the
  16_16_16_16_UINT path is reached) and one at about 40 s (after PLAY). Only the first devcoredump was kept.
  The crawl and the resets may be the same overlong GPU work; this is not established.
