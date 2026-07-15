# DevTools macOS Host-Boundary Portability Design

## Goal

Make the existing native diagnostics supervisor buildable and runnable on the
current macOS host without changing guest execution, GPU semantics, or the
diagnostic protocol contract.

## Evidence and scope

The clean `gen5-integration` baseline at `c44083d` fails before runtime:

- `DurableFilePosix.cpp` reads Linux-only `stat::st_mtim`; macOS exposes
  `stat::st_mtimespec`.
- `QuerySelfProcessIdentity` and `ProbeProcessIdentity` assume Linux `/proc`.
- `RunSelfTest` resolves the executable through `/proc/self/exe`.
- POSIX process launch rejects non-Linux hosts because it has no proven
  close-all descriptor action, even though macOS provides
  `POSIX_SPAWN_CLOEXEC_DEFAULT`.
- The unit-test target includes emulator headers but not the vendored Vulkan
  include root, so graphics tests require an out-of-band `CPATH` workaround.
- The macOS build also reports an existing deprecated `syscall` use in
  `VirtualMemory.cpp`; that warning is recorded for a separate host-runtime
  cycle and is not bundled with this diagnostics fix.

This cycle is intentionally limited to the diagnostics host boundary and the
unit-test build contract. It does not implement the missing Windows source
files, change guest process identity semantics, alter the GPU frontier, or
start broad module extraction.

## Design

### One process-identity contract

Extend the existing supervisor header with a typed `QueryProcessIdentityByPid`
operation. `QueryProcessIdentity(ProcessHandle, ...)`, stale-temp probing, and
worker-side current-process lookup all use that one producer.

The POSIX implementation keeps Linux `/proc/<pid>/stat` parsing unchanged and
adds a macOS adapter using `proc_pidinfo(PROC_PIDTBSDINFO)` and the checked
`pbi_start_tvsec`/`pbi_start_tvusec` value. A zero or unavailable identity
fails closed. `ProbeProcessIdentity` maps an absent process to `Dead` and all
other query failures to a retaining state; it never treats an unreadable
identity as safe to delete.

### Host file metadata

Keep the mtime-to-nanoseconds conversion in the POSIX file adapter. It selects
`st_mtimespec` on Apple and `st_mtim` on Linux. The cleanup policy, age
threshold, owner check, and retention behavior remain identical.

### macOS process launch and executable discovery

Use `POSIX_SPAWN_CLOEXEC_DEFAULT` on macOS, then explicitly duplicate the
mapping and liveness descriptors to guest-facing descriptors 3 and 4. Linux
continues using its existing close-from action. Current-executable discovery
uses `_NSGetExecutablePath` on macOS and `/proc/self/exe` on Linux, behind one
host-facing helper used by `RunSelfTest`.

### Build contract

Add the vendored Vulkan include root to the `unit_test` target because the
tests directly include emulator graphics headers. This removes the need for
`CPATH` and makes the configured CMake target self-contained.

## Verification

The red/green gate is:

1. Add a current-host self-test covering process identity and supervisor
   launch; build the existing test target and observe the known portability
   failure before implementation.
2. Build without `CPATH` after the minimal implementation.
3. Run the focused filters
   `DevToolsBundle.*:DevToolsLifecycle.SelfTestNormalExitRunsOnCurrentHost` and
   the existing `CoreMemoryAlloc.*` host-memory regression suite.
4. Run the complete `fc_script` build and inspect new warnings.
5. Run the devtools self-test path on macOS with no behavior-changing
   `KYTY_*` flags. No guest strict replay is claimed until this build gate is
   green.

The cycle is complete only if the source diff is limited to this host-boundary
contract, the focused tests pass, and the next build/runtime frontier is
recorded outside Git. Windows remains a separately verifiable workstream.
