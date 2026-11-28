# PPSA38500 - Grand Theft Auto VI

Records for `PPSA38500 - Grand Theft Auto VI` (01.002.001), newest first. The
bring-up manual keeps the invariants, methodology, frontier and an index of
every record.

## PPSA38500 - Grand Theft Auto VI: strict boot walls (2026-11-27, open, not gameplay)

- **Setup:** strict runs (no `KYTY_BRINGUP_*`, 60 fps limit, private cold
  shader caches), Intel Arc A770, Mesa 26.3.0 ANV, Linux 7.3.6, built from
  `ee5f81020` plus local changes. Each wall is reached 2-3 s after boot; no
  frame is presented yet; the later walls are reached 6-28 s after boot.
- **Walls passed, in order:**
  - **Loader TLS:** guest `fs:0` thread-pointer loads are patched to a host
    thunk; covered by `UnitTestEmulatorLoaderTls`.
    - **Red-zone defect fixed:** a load into a register other than rax was a
      `call` to its thunk, which wrote 32 bytes below the guest `rsp`. A leaf
      function at `0x90135a0f5` keeps a counter there, and the clobbered
      value (a low return address, `0x0f613216`) indexed a TLS stack and
      faulted about 6 s after boot. Each such site now jumps to its own
      trampoline, which moves `rsp` past the 128-byte red zone before the
      call.
  - **libc and kernel imports:** `strtok_r`, `strcat_s`, `erff`, `nextafterf`,
    the `strtoull` alias, `getargc` and `getargv`.
  - **VideoOut `SetGlobalBlendSpace`:** accepted. Its effect on gamma or HDR
    output is not modeled.
  - **Primitive type 9 (patch) without a hull stage:** drawn as triangles,
    covered by `UnitTestEmulatorPrimitiveState`.
  - **AMPR AMM:** `GetVirtualAddressRanges`, `GiveDirectMemory` and the
    command-buffer records MapAsPrt, AllocatePaForPrt and UnmapToPrt. The
    title maps `0x2000000000+0x617000000` as PRT and then backs
    `0x2400000000+0x19000000` with mtype 12 and protection `0xf2`.
    - **UnmapToPrt defect fixed:** UnmapToPrt over backed pages failed with
      `EBUSY`. The fixed reservation landed on a GPU-visible mapping, which
      `DecommitRange` refuses to turn back into reservation. Backed runs are
      now unmapped and then reserved again.
    - **Observed unmap:** `0x2616040000+0x30000` now unmaps cleanly. The wall
      depends on timing, because the title unmaps only at some point of its
      streaming.
  - **`GE_INDX_OFFSET`:** a SET_UCONFIG_REG burst reaching it now has a
    handler.
  - **Vertex front scalar loads:** a vertex front (`gs_prolog`, user data
    from s8) loads through a V# it builds in s0-s7, or through one that
    replaced a bound descriptor. Those loads now take the guest-address path.
  - **ContentSearch `Init`/`Term` and ContentDelete
    `Initialize`/`Terminate`:** bracket calls with no other import. The title
    loads the sysmodules `0xc7` and `0xee` first.
  - **AudioOut2:**
    - **ContextSetAttributes:** the title sets ids 1 and 2 as one-byte flags,
      both 1. They are accepted; their meaning is not established.
    - **GetSpeakerArrayMemorySize:** returns 0xc00 for (8, 0, 1).
    - **SpeakerArrayCreate/Destroy:** the handle is 8 bytes. Both destroy
      sites load it 64-bit and skip a zero handle.
    - **What the title then checks:** its audio init (`0x908385720`) checks
      every step, and a failure traps through `int 0x41`.
  - **`sceAjmInitialize`:** the title passes 4 (`mov $0x4,%edi` at
    `0x908385915`), and Kyty accepted only 0 and `0x300000000`. It now
    accepts 4. What the value selects is not established.
  - **NetCtl `GetInfo` code 3 (MTU):** reports 1500, the Ethernet payload size
    of the wired device that code 1 already reports.
  - **SharePlay `Initialize(heap, 0x1800)`/`Terminate`:** the title imports
    nothing else from SharePlay. A non-zero result frees the heap and clears
    the title's Share Play flag. Two secondary implementations agree that
    the call returns 0.
  - **MIMG opcode `0xa0` (`image_sample_a`):** in PS `0xc656f06d` the
    instruction is `f0800f09 00400202`: a 2D sample, dmask `0xf`, no NSA.
    - **Operands:** `vaddr v2`, `T# s[0:7]` and `S# s[8:11]`. The S# is built
      from immediates (`0x400, 0x00fff000, 0x0af00000, 0`, an anisotropic
      sampler).
    - **Address:** `v2`/`v3` are the UV interpolated from `attr1`, and no
      further operand or dword follows. So the operands are image_sample's,
      and it now decodes as image_sample. The probe dump shows
      `OpImageSampleImplicitLod`.
    - **Unknown bit:** what opcode bit 7 changes is not established. Kyty's
      names put it only on samples with implicit derivatives, so the risk is
      LOD or anisotropy selection.
    - **Still unsupported:** the other `_a` opcodes.
  - **NpManager `RegisterPremiumEventCallback`/`Unregister…`:** the title
    sets its flag only on success and unregisters only when the flag is set.
    No premium event reaches an offline account.
  - **PM4 `IT_REWIND` (0x59):** the title builds it through
    `sceAgcDcbRewind` and later sets bit 31 (valid) of its body through
    `sceAgcRewindPatchSetRewindState`. The CP now waits on that bit like a
    WAIT_REG_MEM equal, then reads the packets after it from guest memory
    again, so it sees what the guest patched meanwhile.
    - **Submitter deadlock fixed:** the guest releases the REWIND only after
      its submit returns, but the submit waited for the decode to complete.
      A run suspended on a REWIND now completes its submit.
    - **Verified:** the title no longer stops after its first present. A
      strict run reported 1031 presents at 60 fps, phase `interactive`, 40 s
      after boot.
  - **`sceKernelMapDirectMemory2` with protection `0x30`:** the title maps
    64 KiB pages of type 12 at fixed addresses with GPU read-write and no
    CPU access, and traps through `int 0x41` on failure. Kyty rejected the
    value. It now maps them, keeping host CPU access, because the GPU
    emulation reads and writes the range through the guest address.
  - **TextToSpeech2 `Speak`/`Close`:** at first boot the title narrates its
    accessibility prompt ("Press Square to enable screen narration") whatever
    the screen-reader setting, which Kyty reports as off. `Speak` takes a
    header of a text pointer, a length in UTF-16 units, flags and 8 opaque
    bytes. It now accepts the request and finishes it at once. `Close` takes
    no argument; on success the title terminates the library. Two secondary
    implementations agree that both return 0 for an open session.
  - **AJM Opus revision 1:** the title creates an Opus instance (codec 24)
    with flags `0x1` and initializes it with `{1 channel, 48000 Hz, 0}`.
    Kyty admitted Opus only with flags `0x1002` (revision 2 with
    resampling). The split decode input is the same length-prefixed packet
    stream: a U16LE size `0xa0` followed by TOC `0xf8` (CELT fullband 20 ms,
    mono), 488 bytes in, 15360 bytes of output room. Initialize and split
    decode now admit both flags; gapless and resample profiles still need
    `0x1002`. A later run (2026-11-28) stopped twice at 54 s initializing an
    Opus instance with flags `0x2` (revision 2 without resampling); split
    decode admits it too, under the same configuration and packet checks.
    The AJM stop now names the codec and instance flags.
  - **Voice without `sceVoiceInit`:** the title imports only `VoiceStart`,
    `VoiceDeletePort` and `VoiceGetBitRate`. It loads the voice module, calls
    `VoiceStart(NULL)`, then probes `VoiceGetBitRate(0xff, &rate)` and marks
    voice available only when that returns 0. The firmware voice module
    checks, in order: the context that only `VoiceInit`/`VoiceInitHQ` create
    (`0x804e0801`), a port id above `0x3f` (`0x804e0804`), and a missing
    argument (`0x804e0805`); `VoiceStart` also needs a non-zero leading word
    in its parameter. An earlier change in this bring-up made `VoiceStart`
    return 0 without init; that was wrong and is replaced by the module's
    guards. `VoiceGetBitRate` and `VoiceDeletePort` now return the same
    errors, so this title sees voice as unavailable, as on the console.
  - **Sampler from a sampler table:** pixel shader `21000260f862f733` loads
    a V# from user data with `s_load_dwordx4`, then its S# with
    `s_buffer_load_dwordx4` at `(index << 4) + immediate`, the index also
    from user data. The S# is now resolved at bind time when the index
    register still holds its user-data value, the V# is readable and the
    record lies inside it (`ShaderCollectTableSamplers`); it binds like a
    constant sampler. Translator version 262. Tests:
    `TableSamplerBindsTheRecordUserDataSelects` and
    `TableSamplerIndexMustHoldUserData`.
  - **`swscanf`:** a lazy import during the setup screens. Guest `wchar_t` is
    16 bits, so input and format are widened unit by unit, host scratch holds
    `%lc`/`%ls`/`%l[` results, and only the first `result` counted
    destinations are narrowed back. A `$` positional conversion stops the
    run. Test: `ResolvedSwscanfStoresSixteenBitCharactersAndCountsGuestUnits`.
    The Windows branch was not compiled.
  - **Alpha-only colour target:** the legal-notices screen renders to an
    8-bit UNORM target with COMP_SWAP 3 (ALT_REV). For a one-channel format
    that swap stores export W, an alpha-only surface, per the AMD driver's
    colour-swap table. Only a mirror rendering of that table was read, not
    the upstream file. `SetColorInfo` marks such a target with
    `kColorExportOrderOneChannelAlpha`: the export moves W into channel 0,
    the write mask keeps the W bit, and the colour equation takes the
    guest's alpha equation with destination-alpha, constant-colour,
    src1-colour and saturate factors mapped to their alpha-channel values.
    Test: `OneChannelAltRevSwapExportsWIntoTheChannel`. The blend remap and
    the DCC clear of such a target are untested.
  - **`sceNpEntitlementAccessGetGameTrialsFlag`:** the title reads a
    `uint32_t` and treats 1 as a trial. The published contract defines OFF
    (0) as "not a Game Trials application"; a full install answers OFF, like
    the existing SKU flag answer. The guard order (initialized, then
    pointer) follows `GetSkuFlag`; the firmware module dump does not export
    this function.
  - **`sceFontCharacterGetBidiLevel(NULL, NULL)`:** a probe. The firmware
    font module writes the byte at offset `0x3b` of the character record and
    returns 0 when both pointers are set; otherwise it returns
    `0x80460002` and clears the level when only that pointer is set. Kyty
    now does the same.
- **Display defect (open): the frame fills only the top-left quadrant.**
  The final draw into the display buffer is a rect (primitive type 7, three
  vertices from a vertex buffer, VS `0a0003e00a9e89f5`, PS
  `21000160c656f06d`). Two causes were stacked:
  - **Fixed: Kyty drew a rect fed by vertex buffers as one triangle.** The
    plan in `GraphicsResolvePrimitiveDrawPlan` added the fourth corner only
    for a three-vertex draw without vertex buffers, through vertex id 3. This
    rect's vertices are top-left, bottom-left and bottom-right, so the right
    angle is at the second vertex, and its UVs follow the same corners.
    - **Change:** every other rect list is now a triangle list through a host
      geometry stage. Following the public topology contract recorded in
      `docs/bringup/history.md`, type 7 is the bounding box of an arbitrary
      2D triangle and type 17 is three corners of an axis-aligned rect. The
      stage emits the screen-space bounding box of the three shaded vertices
      as a strip with the triangle's winding.
      - **Smooth outputs:** the position and each smooth parameter extend the
        triangle's plane through its barycentric weights at each corner. For
        a rect, the fourth corner is `a + b - right-angle corner`.
      - **Flat parameters:** a parameter the pixel stage reads flat keeps the
        first vertex's raw value, the provoking vertex Kyty uses elsewhere.
    - **Not covered:** the stage forwards only the position and the
      parameters, and it shares the one geometry slot. These rects are still
      drawn as their first triangle, with a warning:
      - the vertex stage exports a layer or clip distance;
      - the pixel shader needs interpolation geometry or fragment transport;
      - the host has no geometry shaders.

      A vertex stage whose `w` differs between the three vertices is
      combined affinely in clip space, not perspective-correctly.
    - **Verified:** the capture at frame 802 shows the full quadrant with the
      accessibility prompt and both button hints, where only a triangle
      showed before.
      - **Unit test:** `AssemblesRectGeometryForEveryParameterCount` assembles
        the stage for 0, 1 and 32 parameters, with and without flat ones.
      - **Astro Bot:** `PPSA21567 - Astro Bot` with translator 261 and a cold
        private cache reached its title screen in a 150 s run (6278
        presents). A second cold run on the binary with the heap-index change,
        at warning level, reached the same screen (6280 presents in 150 s)
        with no rect warning. Both are smoke checks; gameplay was not run.
  - **The guest draws a 1920×1080 texture on a 3840×2160 canvas.** The VS
    multiplies by an identity world matrix and an orthographic projection
    with `2/3840` and `-2/2160`. The rect is the source texture's own size,
    the u16 width and height of its descriptor shifted by the mip level. The
    projection's bounds come from a pair of guest globals that a
    configuration parser fills, and that pair is also the size the title
    gives a second VideoOut output (option `0x20`). That second output is
    opened only when a flag byte is 1, and it is 0 here. The main buffer
    size instead follows `sceVideoOutGetOutputStatus`: with a 1280×720
    screen, Kyty reports resolution 1 and the title registers two linear
    1920×1080 buffers.
  - **Excluded as the scale cause:** fixed render resolution (Native mode
    shows the same), instancing (`instance_count` 1), the index offset,
    `SubmitChangeBufferAttribute2` (not called), the safe-area ratio (1.0)
    and a misread of the vertex data (the raw words are floats 0, 1080 and
    1920).
  - **Reporting a 4K output** (output status resolution 2) makes the title
    register 3840×2160 buffers, and the first-boot screen then fills the whole
    frame (a temporary override with a 1280×720 window, 2026-11-28). The
    title picks the buffer size with `cmpl $2` on the output status at
    `0x90417c0f2` (3840×2160 or 1920×1080). With resolution 1 the canvas pair
    stays 3840×2160; what fills it is still open.
  - **The SIGSEGV seen with a 4K output was the title's own watchdog, fed by
    a 20 s loading pause.** At `0x90847c55a` a thread that counts down 30
    checks of a progress value (`0x1e` in `r13d`/`r15d`) writes to address 0
    and executes `int 0x41` when the value stops changing. At 1080p the same
    pause lasted 20.8 s and the watchdog did not fire. The pause was one
    host thread at 100% with no file reads: every 64 KiB
    `KernelMapDirectMemory` registered a GPU heap, and
    `GpuMemory::SetAllocatedRange` re-sorted the index of every heap each
    time. A heap that overlaps none now inserts its own span; overlaps still
    rebuild. After the change the worst frame at 1080p is 0.74 s (was
    20.8 s), 1297 flips in 23 s (was 803 in 35 s), and the 4K run no longer
    trips the watchdog. Both figures come from runs with console logging at
    error level. The heap count and the watchdog period were not measured;
    two of three host samples during the pause were inside the rebuild.
- **Defects found, not fixed:**
  - `VideoOutSubmitChangeBufferAttribute2` returns success and ignores the
    attribute.
  - A draw with an unsupported primitive type is skipped in both draw entry
    points. The only trace is a debug-level line, capped at 16, so a normal
    run shows nothing.
- **Display size:** with `ScreenWidth = 3840` and `ScreenHeight = 2160` the
  first-boot screen fills the frame and holds (2374 flips in 43 s at the
  60 fps limit, 2026-11-28, console logging on). With a smaller window the
  frame still fills only the top-left quadrant.
- **Frontier (2026-11-28, diagnostic `pad tap` input, 3840×2160 screen):**
  the intro, the setup screens (subtitles, finish setup), legal notices and
  the legal terms (both boxes ticked, Confirm) pass at the 60 fps limit, and
  the title starts loading (dark screen with a spinner) about 165 s after
  boot. Agent taps are diagnostic input, so this is not a supported-run
  claim. Taps closer than about 300 presents apart were sometimes ignored by
  the menus; 500 to 700 apart all registered.
- **Acm (fixed 2026-11-28):** during that load the title builds an Acm batch:
  `sceAcm_FFT(batch, 2·N, channels, format 0, inputs[], format, outputs[],
  ZERO_2ND_HALF)` with up to ten channel pointers, then
  `sceAcmBatchStartBuffers`; a failure puts its audio engine in error state
  4. Its Acm imports are `FFT`, `ConvReverb_SharedInput`, `BatchStartBuffers`,
  `BatchWait`, `ContextCreate` and `ContextDestroy`. Kyty bound only
  `ContextCreate`, and the firmware dump has no Acm module with code. All six
  are now bound and computed on the CPU (cross-title record "Acm FFT and
  convolution reverb"); the next run passed the load's Acm calls and stopped
  10 s later, at 187 s, on a non-atomic image instruction with GLC=1, which the parser
  refused. A GLC load or store now translates as a coherent storage image (cross-title
  record on GLC loads).
  Later stops on the same route (2026-11-28), each fixed:
  - 54 s: AJM Opus initialize with flags `0x2` (AJM bullet above).
  - 72 s: a MIMG instruction with UNRM=1. The ISA defines UNRM as "force
    address to be un-normalized" and requires it on stores and atomics;
    loads, stores and atomics already take integer texel addresses, so it is
    now admitted on them. Any other opcode still stops, now naming the
    opcode and pc.
  - 81 s: unresolved libc import `QaTrhMKUT18`, which hashes to
    `nexttowardf`. Its call sites pass x in `xmm0` and store the direction
    with `fstpt (%rsp)`, the SysV x87 `long double` class; it now forwards to
    the host `nexttoward`. This holds where the host `long double` is x87
    extended; a Windows build, whose `long double` is 64-bit, was not built
    or checked.
  - 76 s: `image_store_mip` with DMASK `0x1` on a 2D view; only `0xf` was
    parsed. It now takes the enabled components from consecutive data VGPRs
    and writes the disabled ones as zero, the rule plain `image_store`
    already follows; the `0xf` form emits the same SPIR-V as before. Test:
    `PartialMaskStoreMipTakesEnabledComponentsFromConsecutiveRegisters`.
  - 76 s: a 384x384 storage image (format 71) in tile mode 1, Standard256B.
    The storage path admitted 0/5/8/9/13/27 only; it now detiles tile 1 the
    way sampled textures already do (`TileConvertStandard256BToLinear`, the
    element arithmetic Standard4KB shares). Not yet confirmed on the title:
    the runs since stalled at about 70 s, before this point (stall below).
  - 80 s (`gta6mip`): a 512x512 storage chain (format 75, ten levels) in tile
    mode 1, bound at base level 0 and then at base level 1. Mip backing (one
    host image mip per level) admitted linear, Standard4KB, Standard64KB and
    render-target chains only, so the second descriptor became a second
    object over the same range and object creation stopped
    (`!create_all_the_same`). Standard256B chains now use mip backing too,
    which uploads level 0 the same way tile 5 does.
  - 76 s, alternating with the stop above: a full-screen vertex front loads a
    V# through its user-data pointer over the same registers,
    `s_load_dwordx4 s[8:11], s[10:11], 0`, then reads constants through it
    with `s_buffer_load`. The redefinition analysis only tracked registers
    that name a binding, so the consumers counted as neither redefined nor
    runtime and the stage never enabled device addresses: no lowering for
    the load. A consumer whose V# the program wrote on every path, over
    registers that name no binding, is now recorded as reading a descriptor
    the program built. Test:
    `ConsumerOfAProgramBuiltDescriptorReadsRuntimeDescriptor`. Not yet
    confirmed on the title, for the same reason.
  - About 100 s, in four runs (`gta6unrm`, `gta6s256`, `gta6dsl4`,
    `gta6vsb`): SIGSEGV with no core. The crash report shows a write to
    address 0 at a guest function that is only `movl $0x4d8bd421, 0x0; ret`,
    which the title calls on purpose. The caller is a watchdog thread: every
    `sceKernelUsleep(1000000)` it compares a heartbeat timestamp
    (`sceKernelReadTsc`, scaled by `1 / sceKernelGetTscFrequency`) and
    crashes after 30 iterations without a change (r13 = r15 = 0 at the
    fault); a second check crashes when a monitored entry is 300 s old. A
    sleep that takes more than 2 s resets both. Kyty's TSC and its frequency
    come from the same clock, so the crash means the title made no progress
    for 30 s, from about 70 s on. Agent snapshots at 62 s and 88 s
    (`gta6stall`) show why: one compute pipeline precompile has been running
    since boot (`compute_precompile_running` 1, 14 others done in 1.2 ms from
    the pipeline cache), and from about 71 s the command processor waits on
    it (`compute_precompile_waiting_ns` 0 to 17.1 s); flips fall from about
    52/s to under 7/s. The largest recipe in the private cache is a 3.0 MB
    compute module (144721 ids): 48 runtime-M0 `v_movreld` writes, each a
    gated store over about 180 candidate VGPRs, plus `v_movrels` select
    chains, in a block that repeats 16 times. Which recipe is the running
    one is inferred from the largest-first order, not observed.
  - **Cause and fix of that stall.** The ISA dump of the shader (`gta6mrel`,
    3098 instructions) has 176 relative moves, all in the guest's own
    waterfall loops (`v_readfirstlane`, `v_cmpx_eq_u32`, `s_mov m0`, moves,
    `s_andn2` of the remaining lanes, branch back). Sixteen loops were not
    lowered because the guest schedules the `s_andn2` between its
    `v_movreld` writes, and because a write was assumed able to reach the
    index register. Unlowered, each move became a select or store over
    every candidate VGPR. Two changes in the waterfall lowering:
    - The loop matcher accepts the `s_andn2` between the moves, and decides
      whether a move can overwrite the index from the index's values on
      loop entry.
    - A forward scan from the last label collects the values a lane index
      can take (from `v_mov`, `v_bfe_u32`, `v_and` with a small mask, and
      shifts and multiplies of known sets), valid only under an EXEC that
      descends from the one each value was written under. A lowered move
      then picks only the registers those offsets reach.
    All sixteen loops lower (3034 instructions after lowering). With
    translator 263, agent snapshots at 50, 60 and 75 s show
    `compute_precompile_running` 0 and `compute_precompile_waiting_ns` 0
    (`gta6tsh`, `gta6wd`). Flips go from 3216 to 3493 between 60 and 75 s,
    about 18 per second on the loading screen. Tests:
    `LowersStoresAroundTheRestUpdateWhenTheIndexValuesMissTheIndex`,
    `RefusesStoresAfterOneWhoseIndexValuesReachTheIndex`,
    `LaneIndexValuesHoldOnlyWhereTheirDefinitionRanAndNoRelativeWriteReaches`.
  - Whether the watchdog crash is gone is not established. One run
    (`gta6long`) still ended in SIGSEGV at 103 s, which is 30 s after the
    70 s mark. It had no crash report enabled, and its next run stopped
    earlier, on the bindless stop below.
  - 77 s (`gta6lane`, `gta6tsh`): a Standard4KB texture of format 75
    (32_32_32_32_UINT) declares more levels than its own chain has: 512x512
    with `max_mip` 11 (12 levels), and 64x64 with 9. The mip layout refused
    a count above log2 of the larger side plus one. The ISA describes
    `max_mip` as the resource's level count minus one, and the layout already
    sizes each level at one element at least, so the count is now accepted
    as declared. Both cases are exactly two levels over, which fits a
    16-byte-per-block view of a BC chain four times as wide (2048 has 12
    levels, 256 has 9). That reading is inferred, not observed: no BC
    descriptor at the same address was seen in the bound run. A sampled
    host image still ends at its 1x1 level, so guest levels 10 and 11 of
    the 512x512 view have no host storage (Vulkan's limit). The same rule
    already applied to storage chains, and compressed chains keep fewer.
    Test: `BlockViewOfCompressedChainSharesTheChainLayout`.
  - **74 to 93 s, open: bindless deferred-lighting pixel shaders.** Four
    variants stopped the same way in seven runs (crcs `8e27aa55`,
    `510863a1`, `4805784d`, `879f4e62`; first failing sample at pc `0xb4`,
    `0xc8` or `0xa0`). Which one comes first varies, and one run passed
    95 s without any. The description below is of `8e27aa55`; the other
    three read the same heaps from the same user-data registers. The generator stops with `shader emitter missing` on
    `image_sample` (`ShaderSpirvGenerator.cpp`, the `!ok` branch of the
    instruction loop) because the shader binds no sampled texture
    (`sampled=0/0/0`, `tex_index=-1`). Its T# words come from a texture
    heap: `s_load_dwordx8 s[16:23], s[0:1], 0` loads the heap V# at
    `s[20:23]` and the sampler-heap V# at `s[16:19]`, then
    `s_buffer_load_dwordx8 s[8:15], s[20:23], vcc_lo` reads the T#, where
    `vcc_lo` is `mem32[mem64[user s[4:5] + 16] + 64] << 5`. That first sample
    is fixed for the draw. Later samples are in a loop over light records:
    `s[38:39]` holds the first and end light, each light is a 304-byte
    record of the constant buffer at `s[32:35]`, and its texture and
    sampler indexes select the T# (`<< 5`) and the S# (`<< 4`) inside the
    loop. Kyty resolves table samplers only when the index is a user-data
    SGPR (`ShaderFindTableSamplerReads`), and has no binding for a T# read
    at a register offset. Suggested direction: resolve per-draw reads
    through reaching writes rooted in user data, and give the in-loop reads
    an indexed binding whose contents come from the heap entries the light
    records name.
    - Measured (`gta6hc`, `gta6hs`, a temporary diagnostic at the pixel-shader input
      analysis): the texture heap is one global table of 131072 32-byte
      slots, with about 24.5k filled (20138 2D, 2902 2D array, 1094 1D,
      280 3D, 84 cube, 2 2D MSAA; the last filled slot is 28227). The
      sampler heap has 128 records. Binding the whole heap per draw is not
      viable.
    - The light records are a V# with stride 304 whose record count equals
      the light count (`NumRecords` 3 and 6 in two draws, matching the
      loop bound). Each record's index fields therefore range over at most
      `NumRecords` values per draw, which bounds the candidate T#s of every
      in-loop sample. The two per-draw indexes were 9 and 11 (both 2D).
    - All 70 instruction/format pairs of three of the variants have
      emitters; only the resource resolution is missing.
  - 74 s, once in five runs (`gta6heap`): `vkCreateComputePipelines`
    returned -13 (`VK_ERROR_UNKNOWN`) for a compute program created on
    the command-processor thread (`GraphicsRenderPipeline.cpp`, the
    compute pipeline creation `EXIT`). The failing module was not dumped;
    route runs now set `KYTY_PIPELINE_FAIL_DUMP`.
- **73 to 106 s, open (2026-11-28): the blockers behind the load.** Runs
  stop on one of these, a different one from run to run.
  - **Device loss in compute `c7982181`** (1024 threads): `vkWaitForFences`
    returns -4 on queue 8 at 73 to 80 s, after an Xe preemption reset. The
    program loads `s22` from `mem64[srt + 32] + 36`. The loop at 0x2c0 to
    0x4fc starts at `s_ff1_i32_b32(s22)`, counts down to zero and runs an
    `s_barrier` on each pass. With `s22 = 0` the start is -1, which means
    about 2^32 passes.
    - On the CPU that word is 1024 at record time and after the fence
      failure. The device-address table maps the address into a valid
      window of the imported guest memory.
    - A diagnostic exit on `s22 == 0` at 0x4fc removed the device loss in
      two runs. Both stopped on the blockers below.
    - Excluded:
      - a device-address table miss;
      - bad pointer tags;
      - the GPU overwriting that buffer;
      - a lookup defect.
    - The diagnostic write-back reported (2026-11-28): at the loop's back
      edge the GPU holds `s22 == 0`. The loop only reads `s22`, so the GPU
      loaded 0, while the CPU read 0x400 at record time. The loop runs
      `s_ff1_i32_b32(s22)` times, and `s_ff1` of 0 is -1, which gives about
      2^32 passes. The tagged GPU write to word 11 of the same line reaches
      guest memory, so that write path is coherent.
    - Open: whether the GPU read a different pointer at `srt + 32`, or the
      count was not yet in memory when the GPU ran. The write-back so far is
      of the last dispatch only and does not separate the two.
  - **Bindless compute `1b06ebb9`, sibling `0c7fb1c4`:** `image_get_resinfo`
    and `image_load_mip` through a T# selected per workgroup. Compute
    programs `0a805aa5` and `219a1b1a` stop on the same instruction types.
    - The chain:
      - `s7` is TGID_X (`USER_SGPR` 7, `TGID_X_EN` 1, 64 threads);
      - `s62 = mem32[V#(user s[4:5]) + 4 * tgid]`;
      - `s63 = mem32[V#(user s[2:3] + 1376) + s62]`;
      - `index = mem32[that V# + s63 + 924]`;
      - `s_buffer_load_dwordx8` reads the T# at `index << 5` of the heap V#
        at `user s[0:1] + 16`, the 4 MiB texture heap above.
    - The heap path's chain builder stops at the `s_lshr_b32` of the
      `v_readfirstlane_b32` result.
    - The CPU view at record time covers three dispatches at the boot
      screen. They have 10 to 15 list entries, and every entry reaches heap
      slot 7 (2D, 1x1, format 56, tile mode 5). Only one of the four T#
      sites was walked.
    - Whether the CPU or the GPU writes the lists is not established.
    - This access form is the one `docs/bringup/bindless-tables-design.md`
      describes. That document is a design only.
  - **Pixel programs `55fe3ff7` and `c3404d5d`** (a separable blur):
    `image_sample_l` with no binding (`tex_index=-1`, `sloads=0`). Two
    independent causes:
    - The samples sit in a loop after a conditional branch (exit test at
      0x84, samples at 0xb4 and 0xc0, back edge at 0x100). Dynamic
      descriptor loads through user data are matched by a straight-line scan
      that stops at the first branch, so it finds no consumer. Only table and
      GS user-data loads use the analysis that follows every CFG path, and
      that analysis rejects reads after a branch that the straight-line scan
      never sees. Switching it over is not a superset and needs the regression
      set.
    - The T# is eight contiguous dwords at `srt + 16`, written by two
      S_LOADs: dwords 4 to 7 of an x8 at `srt + 0`, and an x4 at `srt + 32`.
      The V# of the blur weights is dwords 0 to 3 of the same x8.
    - The S# is built from literals: `s_movk_i32 146`, `s_bfm_b32 12, 12`
      and `s_mov_b64 0x06500000`.
    - Tried and withdrawn: a parse-time pass that re-splits an x8+x4 pair
      into x4+x8 so the T# gets its own load. It never fired on this shader,
      because its consumer search also stopped at the branch.
  - **Compute `9be22ee5`, fixed (2026-11-28):** a writable descriptor
    (descriptor 3, EUD dword 36, dynamic load) whose eight T# words are zero
    at bind. The storage plan refused format 0.
    - The program loads that T# (`s_load_dwordx8 s[8:15], s[4:5], 144`) and
      stores through it only when bit 3 of user SGPR `s7` is set
      (`s_bitcmp1_b32 s7, 3` + `s_cbranch_scc0`), so the guest leaves the
      slot zero when the output is off.
    - Contract: the ISA defines an all-zero resource as unbound. A store
      through a T# loaded from memory whose eight words are all zero is now
      discarded; the load writes zero, as for the loaded read-only case. Only
      an all-zero T# qualifies for a store, not a buffer-type one. Stores
      through a user-data T# are unchanged.
    - Unit test: `DiscardsStoresThroughLoadedAllZeroDescriptor`, with an
      image T# in the same words as the control.
    - Verified, in a run that skipped missing emitters (localization only):
      the program translated past the store. It then
      missed `image_get_resinfo` at 0x58c, and stopped on a dispatch where
      the same slot holds a real volume (below).
  - **Volume layout `SW_4KB_S` with a mip chain (implemented, not yet
    reached in a run):** `unsupported Gen5 volume layout: format=1 32x32x32
    levels=6 tile=5 type=10`, at the slot of `9be22ee5`. Kyty laid out a
    volume mip chain only for `SW_64KB_S` with 4-byte elements.
    - Change: 4 KiB thick chains for 1-byte elements, from the published GFX10
      address library: 16×16×16 blocks, at most 5 tail levels, the tail halves
      the height, smallest level first in each block-depth slab. Other element
      sizes stay refused until a title shows them.
    - Unit test: `Standard4KVolumeMipChainUsesTheThickTail` (the in-block bit
      order and the 32³ six-level offsets: tail at 0, mip 1 at 4096, mip 0 at
      8192, 49152 bytes).
  - **`image_gather4_l` (0x44) and `image_gather4_l_o` (0x54), with NSA
    addresses,** now parse as a gather with an explicit LOD. The emitter
    refuses them, because a Vulkan gather reads only the base level. One
    run reached one: compute program `636f9f10`, `image_gather4_l` at 0xe9c,
    dmask 0x1, addresses `v[15:17]` (x, y, lod), T# bound (2D). The LOD is
    computed (`v_max_f32 v17, 0, v5`, or `v10` on one path), so it is not
    provably zero. An exact emitter needs the level a gather with an
    explicit LOD reads; no evidence for that rule is collected yet.
  - **AMM `AllocatePaForPrt` (action kind 4)** fails with EACCES
    (`0x8002000d`) on runs of actions: 64 KiB pieces at a 2 MiB stride,
    mtype 12, prot `0xf2`. In two of five runs it ended the run (79 s and
    89 s).
    - The physical side is excluded: a direct-memory query of the failing
      pool page right after the failure returns the allocation that holds it.
    - The failure is on the virtual side. `KernelMapDirectMemory` takes the
      reservation-replacement path (`replace=1`), and
      `ReplaceAndConsume` (`Kernel/Memory.cpp:608`) returns false before the
      mapper runs (`consumed=0`). That happens only when
      `VirtualMemory::UnregisterDemandRange` (`Kernel/Memory.cpp:623`)
      refuses. The refusal is either a piece not covered by demand ranges, or
      the published table exceeding `kDemandRangeCapacity = 4096`
      (`lib/Core/src/VirtualMemory.cpp:735`). Each 64 KiB piece punched out
      of a reservation splits a range in two.
    - Observed (2026-11-28): the capacity. Every refusal reported 4096 live
      ranges and 4097 after the split, at the first failing piece.
    - Change: the bound is 65536 ranges. Updates and the fault-side lookup
      cost the live count, not the capacity. The run after the change did not
      reach 4096 ranges, so the new bound is not yet exercised.
    - Defect, found and not fixed: a virtual-side refusal comes back as
      EACCES. `physical_range_valid` stays false when the mapper never runs,
      and `KernelMapDirectMemory` (`Kernel/Memory.cpp:3378`) reports that as
      a physical-range error. Direction: report the reservation refusal with
      its own code.
  - **First stop of a clean run (2026-11-28, `e4fe96772`, fresh shader
    cache, no diagnostics):** device lost at 90 s (`vkWaitForFences`
    `result=-4`, queue 8); an earlier run of the same binary family lost the
    device at 77 s. No emitter was missing before it. The loss matches the
    `c7982181` hang above, which skipping that dispatch localized; this run
    did not re-localize it.
  - **A run with every missing emitter skipped** (a temporary diagnostic,
    localization only) reached 106 s. It stopped on a guest write to
    address 0. Its programs were wrong, so the fault is not attributed.
- **Visual defect (open): vertical bands.** From the legal-notices screen on,
  several captures of a static menu show dark vertical bands over the
  background and the selection bar, changing from one capture to the next
  (frames 4038 and 7941 of one run, most captures of three others). The
  earlier setup screens are clean. Those screens are also the first with the
  alpha-only target above, so that path is a candidate, as are the 8-bit
  surface's layout and synchronization. Which pass first shows the bands is
  not established.
- **Open:**
  - The image_sample_a bit, the AudioOut2 attribute ids, the Ajm value 4 and
    the ContentSearch/ContentDelete parameters are not modeled.
  - AMM window and record sizes come from a secondary implementation.
  - AMM unbacked PRT pages are reservations, not zero-filled.
  - The AMM submit argument signature is unverified.
  - Primitive type 9 rests on secondary sources.
  - The rax form of the TLS patch still calls its handler from the site, so
    the return address and the handler's saved rcx land below the guest
    `rsp`. No title has shown a fault from it yet.
  - Guest CPU access to a GPU-only mapping is not trapped.
  - TextToSpeech2 synthesizes no speech.
  - What AJM revision 1 changes in Opus results, if anything, is not
    established.
  - The REWIND body bit 24 has no established effect, and a REWIND in a
    command stream without a guest origin stops the run.
  - The vertex-front scalar gate and the `image_sample_a` decode change
    shader translation. Astro Bot reached its title screen with a cold cache
    at translator 261; versions 262 (table samplers) and 263 (lane value
    sets in the waterfall lowering) have not been run on it.
