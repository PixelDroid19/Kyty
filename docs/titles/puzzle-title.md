# Puzzle title

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## Puzzle title on AMD: two causes of the gfx ring timeout (2026-10-17)

- **Symptom (RADV, Radeon 610M, Mesa 26.2.4).** A few seconds into the
  intro, the run ends with `VK_ERROR_DEVICE_LOST`, and `journalctl -k` shows
  `ring gfx_0.0.0 timeout` followed by a page fault. Intel runs of the same
  binary do not fault. The RADV hang report (`RADV_DEBUG=hang`) names the
  pixel shader of the faulting draw.
- **Cause 1: a redefined V# register read through its descriptor slot.**
  - The guest pixel shader loads its descriptors from a bound user-data
    V#, then overwrites the same SGPR quad with an `s_getpc`-based address
    and issues more buffer loads through it.
  - The translator mapped every buffer instruction that names a bound
    descriptor's start register to that binding, so those later loads indexed
    the descriptor array with a value read from guest memory. The index had no
    bound, and the access fell outside the set.
  - Fix: the scalar flow already records, per instruction, which SGPRs still
    hold their entry value. A buffer instruction whose V# register names a
    bound descriptor but no longer holds its user-data value now reads through
    the runtime V# (scalar loads through the guest-address path, bounded by
    stride times records). Only resolved control flow is analyzed, and the
    consumer list is part of the shader id.
  - Offline check: with shader logs from the same route before and after,
    only the faulting pixel shader changed. Its redefined loads became
    guest-address loads bounded at 176 bytes.
- **Cause 2: a pixel module reused with another stage's push block offset.**
  - The pixel stage's push block starts after the vertex stage's block, and
    its descriptor set slot depends on whether the vertex stage uses
    descriptors. Both values are baked into the SPIR-V, but neither was part of
    the pixel shader's id.
  - In the RADV dump, the vertex block was 128 bytes and the reused pixel
    module read its block at offset 112. Its first descriptor word was
    therefore the vertex block's last row (0x100000), and the GPU faulted at
    0x02203000.
  - Fix: the bind ids now hash `push_constant_offset` and
    `descriptor_set_slot`, so the pixel module is rebuilt for each pairing.
- **Verified.**
  - AMD: two 120 s strict runs of the puzzle title had no fault, no ring
    timeout and no device lost (1.95 and 9.7 fps). Captures show the intro
    text.
  - Unit test: `EmulatorShaderGetpc.RedefinedBoundDescriptorReadsRuntimeDescriptor`.
  - Intel strict sweep with both fixes: the racing title gave 7 captures
    (46-81 fps in race), the sandbox title 9 captures with the textured world,
    the puzzle title 1 capture (8.1 fps, cold compile as before), and the
    anime fighter 4 captures (16.6 fps).
- **Open: the role-playing title still times out on AMD.**
  - The gfx ring times out about 1:45 into the run, with no page fault.
  - In the RADV hang report, the hanging draw is a 48-index `DRAW_INDEX_2`.
    Its fragment shader has nested loops. The outer bound comes from a storage
    load at offset 0x20. The inner bound comes from offset 0 of a descriptor
    indexed by a push constant (set + index * 16 + 512). The inner body reads
    64-byte records.
  - Excluded: an unbounded loop. Halting the shader engine with `umr`
    (write `SQ_CMD` = 0x111, dump `-wa gfx_0.0.0`, resume with 0x11) while
    the ring was stalled showed the pixel waves at different PCs with loop
    counters of 3 (outer) and 1 (inner).
  - Excluded: a CPU/GPU race. Waiting for the queue to go idle after every
    submit still timed out.
  - Observed: skipping that one pixel shader removes the timeout. Under
    `RADV_DEBUG=hang` the same submission finishes after about 1.9 s, and the
    run reaches its time limit. The fence log shows the ring timeout firing
    2 s after the last fence progress, which is the default gfx lockup
    timeout of the host kernel (7.2.9).
  - Conclusion: one submission carrying that run of draws into a 1024x1024
    target takes about as long as the kernel watchdog on a 2-CU iGPU. The
    title's AMD history has mostly been device lost, so this is not a
    regression. Open: why each of those draws costs hundreds of
    milliseconds there, and whether long guest command buffers should be
    split into several Vulkan submissions. As a host workaround,
    `amdgpu.lockup_timeout` raises the watchdog.
