# Roguelike platformer

Title: PPSA15552 - Dead Cells.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## Depth wiped mid-frame by the write-back of a fill (2026-10-31)

PPSA15552 - Dead Cells lost sprites while moving or attacking: for several consecutive frames only the scarf and the
head flame of the character remained. A pixel history showed the character quad passing its depth test and later
draws overwriting it under LessEqual with a farther depth, after a `vkCmdClearDepthStencilImage` at the start of the
second command buffer of the same G-buffer pass.

- **Cause.** The guest clears HTILE with a compute fill before the pass, and the fill publishes its clear when it is
  recorded. The later write-back of that same storage saw the clear pattern and published a second clear, which the
  next draw consumed. Log of the events for one depth target: fill, fill, write-back, repeating every frame.
- **Fix.** A write-back that carries an already published fill is that same write and publishes nothing. A write-back
  with no published fill to account for it still reports a clear made by a shader the fill proof does not recognize.
- **Verified.** Run on Intel Arc A770 (ANV) with no permissive flags: consumed events in a logged 60 ms window went from a
  repeating fill and write-back pair to fills only, and the character is whole in 8 of 8 frames captured while
  walking and attacking. PPSA21567 - Astro Bot title screen stays free of the depth ghosts.
- **Excluded while localizing.** The hard moving edge on walls is the head-flame light polygon against the blocks. A
  G-buffer target holding the previous final image at pass start is harmless: the guest shares that target and
  overwrites it during the pass. The full-screen yellow and brown flash is the intro cutscene.
- **Open.** The maintainer also reported trails lasting seconds. The dust after breaking a door looks like a game
  effect, but no reference from original hardware was compared, so that report stays open until confirmed in play.

## A roguelike reaches gameplay (2026-10-07)

A 2D roguelike froze after its second frame: the main thread waited on an event flag that its render thread only
sets after the GPU copies a frame label, and the render thread polled that label forever. Three general fixes take
it through the title menu into its first areas (walking verified, about 140 fps under the harness limits):

- **Data packet payloads.** `GetDataPacketPayloadAddress` type 0 describes a NOP data packet whose payload starts
  right after the header. The guest sizes it exactly: a 3-dword NOP carries one 64-bit value that a DMA copies into
  the label the render thread polls; larger payloads are aligned by the guest itself. The payload was returned one
  dword later, so the 64-bit store overwrote the next packet's header and the copy never ran. Type 1 (register
  packets, values after the offset dword) is unchanged.
- **Ngs2 waveform parsing.** `ParseWaveformData` and `CalcWaveformBlock` are implemented for VAG (including the
  HEVAG flag layout, loop start/repeat/end bits in the low flag nibble) and RIFF WAVE with an ATRAC9 extensible
  format (`fact` gives the sample count and decoder delay, `smpl` the loop). The info is 232 bytes: a 24-byte
  format, u32 offsets and counts, `numBlocks` at +0x44 and four 40-byte blocks with 64-bit offset and size. A
  looped waveform splits into lead-in, a loop block repeating forever (the guest replaces that count with its own)
  and the tail. Header-only buffers parse (streams pass the first 0xa8 bytes). Playback of these formats is still
  silent; the sampler rack only renders the custom PCM path.
- **`image_sample_o`** shares the offset path of `image_sample_lz_o` with an implicit LOD; the offset is applied
  in base-level texels.
