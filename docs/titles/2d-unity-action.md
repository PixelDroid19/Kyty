# 2D Unity action title

Records for the 2D Unity action title, newest first. The bring-up manual keeps
the invariants, methodology, frontier and an index of every record. Titles are
named descriptively only (`AGENTS.md`).

## 2D Unity action title: native plugin waits through `_umtx_op` (2026-10-22)

- **Observed** (Intel A770, Mesa ANV, Xe kernel driver, strict run): the run
  stopped 3 s in on an unresolved lazy import of NID `04AjkP0jO9U`
  (`_umtx_op`) from the title's native utility plugin.
- **Evidence** (the plugin's own disassembly and the published FreeBSD
  `_umtx_op` contract):
  - The plugin's wait helper calls operation 2 (`UMTX_OP_WAIT`, compare of
    a 64-bit word). It passes 0 in `uaddr1` and either null or a relative
    `timespec` in `uaddr2`. A timeout that is already non-positive returns
    60 without the call.
  - The wake helper calls operation 3 (`UMTX_OP_WAKE`) with an `int` count.
  - On a non-zero result the plugin reads errno and treats `ETIMEDOUT` (60)
    as a timeout and `EINTR` as success.
  - In the contract, `uaddr1` is the size of the timeout structure: a size
    up to a `timespec` means a relative `timespec`, and a larger size means
    `struct _umtx_time`. A negative `tv_sec`, or a `tv_nsec` outside
    [0, 10^9), is `EINVAL`. A wake releases at least one waiter.
- **Change.**
  - `_umtx_op` now serves `UMTX_OP_WAIT` and `UMTX_OP_WAKE` on the same
    address-wait registry as `sceKernelSyncOnAddressWait`/`Wake`.
  - Any other operation, and a `struct _umtx_time` timeout, stop the run
    explicitly.
- **Verified.**
  - The 6 existing sync-on-address unit tests pass.
  - Three strict runs (35-77 s) on the final build leave the intro logo
    video, which plays at about 360 fps.
  - The two runs longer than 50 s load the first level and show the
    opening dialogue at about 140 fps.
- **Excluded.**
  - **A frozen logo frame.** A run on an intermediate build of this change
    held one logo frame for 90 s; the final build did not repeat it.
    Reading the decoder showed a related defect: frames decoded before a
    decode error were never delivered, so the player never reached its end.
    That is fixed separately (see the cross-title record of the same date).
  - **Silent run recordings.** The guest's own mix carries audible PCM
    (peak 0.05-0.2 per second from second 5). The host mixer had muted the
    emulator's output stream, and the recorder captures that stream after
    the mute, so every recording since the evening of 2026-10-22 reads 0.
- **Open.**
  - Gameplay past the opening dialogue needs input; no recorded route
    exists for this title yet.
  - Each run logs 8 `AudioOut2: host submit failed` warnings. Temporary
    probes on the failure branches (bad arguments, forced failure,
    shutdown, invalid handle, volume) did not fire. That leaves only the
    branch where the host audio object is absent, which fits teardown;
    this is not proven.
