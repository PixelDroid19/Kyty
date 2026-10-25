# Racing title

Title: PPSA17189 - Formula Retro Racing - World Tour - Special Edition.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## Racing title on AMD: corruption from a degraded GPU, not from code (2026-10-17)

- **Symptom (RADV, Radeon 610M, Mesa 26.2.4, strict 200 s route).** From the
  first menu onward, full-screen backgrounds are missing and older frame
  contents show through as stretched triangles and stripes; text loses spans.
- **Runs.** One clean set at 04:51Z. Six corrupted sets in the afternoon with
  identical harness configuration: the branch head, two binaries saved before
  the write-back and timeline commits, the branch with the three
  shader-codegen commits of 10-17 reverted, a binary built from main's exact
  source, and the branch head with every submission followed by
  `vkQueueWaitIdle`. Khronos synchronization validation reported no hazard.
  Intel runs of the same binaries were clean.
- **Cause.** The gfx ring timeouts at 05:35Z and 05:38Z (puzzle and
  role-playing titles, recorded in the regression sweep below) came after the
  clean set and left the GPU degraded; ring resets alone did not restore it.
- **Verified.** A full GPU reset (`amdgpu_gpu_recover` in debugfs; the AMD
  GPU drives no display here) restored it: the branch head then gave seven
  clean captures through two races (in-race 38.6, 20.9, 40.1 and 17.7 fps)
  with no kernel message.
- **Rule.** After any `ring gfx.*timeout` in `journalctl -k`, reset the GPU
  or reboot before reading another AMD result. The timeouts themselves are an
  open emulator defect (see the puzzle and role-playing device-lost entry).

## Racing title race-window correction (2026-10-11)

- The racing title's "race window" figures above (70 to 88 fps) were
  measured with the car stationary on the grid. The route pressed R2 only
  after the race clock ran out. A scripted R2 also set only the digital bit,
  and the title reads the analog trigger.
- With scripted triggers reporting full travel and acceleration from the
  race start, the moving race measured 43.0 and 42.6 fps (p50 21-22 ms,
  381-413 draws per frame). The time-over flyby that follows measured
  33.8 fps at 538 draws per frame.
- These are the baselines for the racing title from now on. The grid
  figures remain valid only as A/B pairs measured on the same scene.
- The beat 'em up's 11-12 fps window is its intro card (6-7 draws per frame),
  not combat. Its gameplay has not been measured yet.
- Moving captures show small grey squares around the car, likely particles.
  This is not yet investigated.
