# Survival title

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named descriptively only (`AGENTS.md`).

## Survival title: hosting an offline world (2026-10-15, open)

- Symptom: Host Game showed "Network Connection Lost". With the connection
  check passed, it showed "Launching Server", then "Server Failed To Start",
  and on retry "Networking Failed To Start". The title's offline mode is a
  LAN-only server running in the same process; it reaches it through
  `SOCK_DGRAM_P2P` sockets on the loopback.
- Fixed, in order of discovery:
  - `sceNetInetPton` left the address in host byte order, so the title's
    self-addressed datagram went to the wrong host. It now follows the BSD
    contract and returns 1, 0, or an error.
  - P2P datagram sockets (type 6) carry a virtual port at bytes 8-9 of the
    address. Two server sockets bind the same P2P port with different virtual
    ports. Each now gets its own host UDP port, and a process-wide endpoint
    table routes local sends.
  - **Root cause of "Server Failed To Start": `sceNetGetSockInfo`.** A bounded
    HLE trace showed the server threads calling only `sceNetGetSockInfo` in a
    loop for about 2 s, then tearing the server down. The guest's loop expects
    three things:
    - the number of 160-byte records as the return value;
    - the record whose socket id at 0x24 and type at 0x28 match;
    - state 2 (opened) at 0x44.

    Until then it calls `usleep(1000)`, up to 2000 times. The old
    implementation returned 0 and a host sockaddr. With records and states
    reported, the call succeeds once and the server starts.
  - POSIX `sendmsg`/`recvmsg` were ENOSYS stubs; they are now implemented.
    This was a defect, not this title's blocker (see the exclusions).
  - World generation prints progress dots through `std::cout`. The exported
    `std::cout`/`std::cerr` were zero-filled, and the guest's inlined inserter
    faulted reading the basic_ios offset from a null vtable. Both are now
    constructed with the layout taken from the guest's inlined inserter,
    `std::endl` and `basic_streambuf` constructor. The classic ctype facet's
    `do_widen` also returned 1 for every character; it now returns its
    argument.
- **Local-only network contract (NetCtl).** `sceNetCtlGetState` reported
  Disconnected (0) while `sceNetCtlGetInfo` returned the loopback address as
  the assigned IP. With Disconnected the title never launches its LAN server
  ("Network Connection Lost"). The emulated network is local only: the
  loopback address is assigned, P2P datagrams are routed inside the process,
  and nothing leaves the host. NetCtl now reports that state consistently:
  IP-obtained (3) and `GetInfo(LINK)` connected (1). The address stays
  127.0.0.1 because P2P routing is process-local. Verified with strict runs
  on the current build: this title reaches day one (86 us per draw, 9.7 fps,
  same as with the earlier diagnostic override). Other titles that import
  `sceNetCtlGetState` or `GetInfo`: the sandbox title enters its world at the
  same capture as before (two runs; a bounded HLE trace of the load showed a
  handful of socket calls within 1 ms), the beat 'em up title reaches
  gameplay, the racing route runs at 45-47 us per draw, and no
  `NetCtlGetInfo` unknown-code exit appeared. The UE4 brawler title still
  stops before its first frame on the unsupported volume layout refusal
  (`400x5x5`, tile 27) that an older build already hit.
- Excluded: `poll`, `recvmsg`, `sendmsg` and `recvfrom` were not called while
  the server was starting (DIAG trace during "Launching Server"). The
  `KernelPoll` "every fd ready" answer was not involved.
- Verified live (Intel Arc A770): the server starts and its sockets exchange
  datagrams over the loopback. World generation runs past the former
  `std::cout` fault and completes. Survivor select and day one of the world
  are reached about 3 minutes after boot in a strict run, with the player
  character moving (input from a person and from the agent pad).
- Harness memory limit, not the title: the run harness capped the cgroup at
  `memory.high` 7G with no swap, and the title keeps about 7.2 GB of guest
  shared memory. Generation then sat in `__mem_cgroup_handle_over_high`
  (full memory pressure about 27%), with presents stalled for up to 48 s.
  With 12G/13G the pressure is zero and generation takes about a minute.
  Heavy titles need `KYTY_RUN_MEMORY_HIGH`/`KYTY_RUN_MEMORY_MAX` above the
  default.
- Draw cost in the world (day one, 20 s windows, about 850 draws per frame):
  - Before 40c887ad: 128 us per draw, 8.2 fps. The command-buffer
    transient pool filled at 15.7 MB, all of its 1,970 entries in use, with
    vertex snapshots of up to 120 KB. Refused index ranges became GpuMemory
    objects whose `UtilCopyBuffer` upload waits on its own fence: 94 fence
    waits and 97 submits per frame.
  - With a 64 MiB pool (40c887ad): no refusals for lack of space, peak
    31.4 MB per pool, 5 fence waits and 8 submits per frame, 89 us per draw,
    9.9 fps. Anonymous memory did not grow (1.33 against 1.44 GB).
  - Same build, other titles. Racing route: 45-48 us per draw against
    47-48. Fishing route: about 15 fps in both, four boots of 45 s pass.
    That fishing title still fills 64 MiB, with vertex snapshots of 2.4 MB.
    Beat 'em up route: captures identical to the previous run.
- Fence waits in the world (fixed in e50014c6 and ddd43522):
  - Attribution (temporary capped diagnostics, removed): about 99% of fence
    time was the device-address draw retry
    (`CommandProcessor::DrawIndex` → `RetryGuestDeviceAddressDraw` →
    `Gpu::WaitSubmission`). The pending storage came from a 64x1x1 compute
    fill with 16-byte records whose value is uniform per dispatch.
  - Ceiling (diagnostic only): forcing device-side publication of every
    uniform fill took 86 to 40 us per draw and 9.7 to 11.1 fps, so the
    retry, not the draws, set the frame time.
  - Root causes:
    - `GpuMemory::QueryRangeProvenance` walked at most 64 candidates over
      1 MiB buckets and reported `truncated` for the 3840x2160 render target,
      which has two real overlaps. The uniform-fill identity then refused at
      random. The query now reports the exact `FindBlocks` overlap set and is
      truncated only above 16 entries.
    - Display buffers (`VideoOutBuffer`) that equal the fill range were not
      admitted as color-image aliases.
    - Fills of a depth target's depth plane or stencil plane were never
      admitted by `MarkStorageGuestPublished`, and the depth plane was never
      propagated. The depth-stencil update function is a no-op and a tiled
      display buffer upload returns early, so invalidating those parents
      after a write-back had no content effect. Propagation is what gives
      them correct content.
  - Fix: exact provenance; display buffers join the uniform-fill image
    aliases; a uniform depth fill (32-bit words, value in [0, 1], D32 depth
    formats, address equal to the depth plane, image already initialized,
    no sample-location state) clears the depth aspect on the device; depth
    and stencil plane propagation stamps the device write time
    (`GpuMemoryResetPlaneHash`); publication admits an equal color image or
    a depth plane (not the HTILE plane) only when its device write is newer
    than the guest write.
  - Result (day one, Intel Arc A770, strict): 86 to 35.5 us per draw, fence
    time 40 to 8.6 ms per frame, 9.7 to 11.8 fps (p50 86 ms). Captures are
    equivalent to the previous build.
  - Regression, same binary: the 2D action title's walk route renders
    correctly (about 134 fps, 67 us per draw); the beat 'em up reaches combat
    with correct captures (163-179 us per draw against 167-169); the sandbox
    title enters its world with the HUD (29.6-35.4 against 31.7-38.0 us per
    draw, terrain still missing as before); the racing route renders the
    road correctly (44.8 against 47.0 us per draw at about 480 draws per
    frame); the fishing
    dock is unchanged visually at 44 us per draw and about 17 fps.
  - AMD (integrated Radeon, same binary, strict): day one is reached and the
    captures match the Intel ones. 31 us per draw, 2 fence waits for 3.3 ms
    per frame, about 10 fps with a 98 ms median frame; the command processor
    takes 30.5 ms of it while `gpu_busy_percent` averages 77% over 40 s, so
    on this host the frame is bound by the GPU at 3840x2160.
  - Still open in this title: a 1920x1080 zero fill contained in a texture
    still refuses (about 8.6 ms per frame of fence waits remain). The frame
    is not bound by the command processor: a sampler at day one shows the
    processor thread about 33% busy and the main guest thread about 10%,
    with three guest threads spinning on `gettimeofday`. The title renders
    at 3840x2160; GPU time is the next measurement.
- Open:
  - **World generation speed:** the 1 fps runs were the harness memory limit
    above. Three guest threads still spin on `gettimeofday` with nanosleep;
    they cost CPU but did not block generation.
  - `std::cin` (imported) and the 104-byte versioned `cout`/`cerr`/`clog`
    objects are still zero-filled. Their layout is a different ABI without
    evidence yet.
  - These are rejected and may be defects:
    - `SO_LINGER` with a 4-byte value (EINVAL);
    - `IPPROTO_IP` option 2 on UDP (ENOPROTOOPT).

    The title ignores both results.
  - Imported but never called:
    - `sceNetGetMemoryPoolStats`;
    - `sceNetCtlGetResult`;
    - `sceNetCtlUnregisterCallback`;
    - three unnamed NIDs.
  - P2P peers outside this process are not routed.
