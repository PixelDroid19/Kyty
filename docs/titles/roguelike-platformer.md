# Roguelike platformer

Title: PPSA15552 - Dead Cells.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

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
