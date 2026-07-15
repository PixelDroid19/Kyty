# DevTools macOS Host-Boundary Portability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore a self-contained macOS build and runnable native diagnostics self-test without changing guest or GPU semantics.

**Architecture:** Reuse the existing POSIX supervisor APIs, centralize process identity queries behind one typed by-pid contract, and keep Apple/Linux differences in the host adapter. The CMake fix makes the unit-test target own the include path it requires.

**Tech Stack:** C++17, CMake/Ninja, GoogleTest through `fc_script`, POSIX `proc_pidinfo`/`posix_spawn`, macOS x86_64/Rosetta build.

## Global Constraints

- Do not enable `KYTY_STUB_MISSING`, `KYTY_GFX_PERMISSIVE`, or `KYTY_SKIP_UD2`.
- Do not change guest ABI, memory semantics, PM4 decoding, surface layout, shader translation, or Vulkan rendering policy.
- Keep Linux `/proc` behavior and existing parser tests unchanged.
- Fail closed on unavailable or malformed process identity; never delete an unverified temp directory.
- Do not use `CPATH` as a build requirement; target include directories must be explicit.
- Do not implement Windows or broad diagnostics extraction in this cycle.

---

### Task 1: Establish the red host-boundary test

**Files:**
- Modify: `source/unit_test/src/devtools/UnitTestDevToolsLifecycle.cpp`

**Interfaces:**
- Consumes: `RunSelfTest` and the existing scratch-directory helper.
- Produces: `DevToolsLifecycle.SelfTestNormalExitRunsOnCurrentHost`.

- [ ] **Step 1: Add the failing test**

Add a test that calls the production self-test entrypoint directly so it uses
the current `fc_script` executable path and does not depend on a private or
Linux-only build path:

```cpp
TEST(DevToolsLifecycle, SelfTestNormalExitRunsOnCurrentHost)
{
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	EXPECT_EQ(RunSelfTest("normal-exit", dir.c_str(), 20, 50, 500), 0);
}
```

- [ ] **Step 2: Run the red build**

Run:

```bash
ninja -C _build_macos fc_script
```

Expected: failure still names `DurableFilePosix.cpp:383` (`st_mtim`) and the
unit-test compile still lacks `vulkan/vulkan_core.h`; no implementation is
added before this failure is recorded.

- [ ] **Step 3: Commit the red test only**

```bash
git add source/unit_test/src/devtools/UnitTestDevToolsLifecycle.cpp
git commit -m 'test(devtools): require current-host supervisor self-test'
```

---

### Task 2: Centralize the POSIX process identity adapter

**Files:**
- Modify: `source/devtools/include/Kyty/DevTools/Supervisor/ProcessLauncher.h`
- Modify: `source/devtools/include/Kyty/DevTools/Supervisor/DurableFile.h`
- Modify: `source/devtools/src/ProcessLauncherPosix.cpp`
- Modify: `source/devtools/src/DurableFilePosix.cpp`

**Interfaces:**
- Consumes: existing `ProcessIdentity`, `ProcessIdentityError`, and
  `ProcessIdentityProbe` types.
- Produces: `QueryProcessIdentityByPid(uint64_t, ProcessIdentity*)` and
  host-correct implementations of `QueryProcessIdentity`,
  `ProbeProcessIdentity`, and `QuerySelfProcessIdentity`.

- [ ] **Step 1: Add the declaration and migrate callers**

Declare:

```cpp
[[nodiscard]] ProcessIdentityError QueryProcessIdentityByPid(uint64_t pid,
                                                             ProcessIdentity* out) noexcept;
```

Change the durable-file current-process query to call this function with
`getpid()`; remove its private `/proc/self/stat` reader.

- [ ] **Step 2: Implement Linux and macOS producers**

Keep the existing Linux parser and `/proc/<pid>/stat` behavior. Under
`__APPLE__`, query `struct proc_bsdinfo` with
`proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, ...)`, normalize
`pbi_start_tvsec * 1'000'000 + pbi_start_tvusec`, and return
`ProcessIdentityError::Unavailable` for a zero/failed result. Have the
existing handle query delegate to the by-pid function, and have stale-temp
probing use the same function so all consumers share one producer.

- [ ] **Step 3: Build and run identity/cleanup tests**

Run:

```bash
ninja -C _build_macos fc_script
_build_macos/fc_script '{kyty_run_tests()}' '--gtest_filter=DevToolsBundle.RejectsLiveOwnerTemporaryDirectory:DevToolsSupervisor.LinuxProcessStatParsesParenthesizedCommand'
```

Expected: both tests pass on macOS; a malformed or unavailable identity keeps
the directory rather than deleting it.

---

### Task 3: Make POSIX file age and macOS launch/executable discovery portable

**Files:**
- Modify: `source/devtools/src/DurableFilePosix.cpp`
- Modify: `source/devtools/src/ProcessLauncherPosix.cpp`
- Modify: `source/devtools/include/Kyty/DevTools/Supervisor/ProcessLauncher.h`
- Modify: `source/devtools/src/Supervisor.cpp`

**Interfaces:**
- Consumes: the centralized identity query and existing descriptor contract.
- Produces: host-correct mtime extraction, `QueryCurrentExecutablePath`, and
  macOS supervisor launch.

- [ ] **Step 1: Implement platform mtime selection**

Use `st_mtimespec` under `__APPLE__` and `st_mtim` elsewhere in the one mtime
conversion site. Do not change the age threshold or cleanup policy.

- [ ] **Step 2: Implement macOS spawn and executable path**

On macOS set `POSIX_SPAWN_CLOEXEC_DEFAULT` on a spawn attribute, then keep the
existing `dup2` actions for descriptors 3 and 4. Add
`QueryCurrentExecutablePath`: `_NSGetExecutablePath` on Apple and
`readlink("/proc/self/exe")` on Linux. Replace the direct `/proc` read in
`RunSelfTest` with this helper.

- [ ] **Step 3: Build and run the green focused tests**

```bash
ninja -C _build_macos fc_script
_build_macos/fc_script '{kyty_run_tests()}' '--gtest_filter=DevToolsBundle.*:DevToolsLifecycle.SelfTestNormalExitRunsOnCurrentHost'
```

Expected: the named test count is nonzero, all selected tests pass, and the
self-test creates its normal-exit bundle through the production supervisor.

---

### Task 4: Make the unit-test target self-contained and verify the cycle

**Files:**
- Modify: `source/unit_test/CMakeLists.txt`

**Interfaces:**
- Consumes: direct emulator graphics-header includes in unit tests.
- Produces: a CMake target that builds without `CPATH`.

- [ ] **Step 1: Add the vendored Vulkan include directory**

Add `${CMAKE_SOURCE_DIR}/3rdparty/vulkan/include` to `unit_test`'s private
include directories beside its existing devtools and emulator roots.

- [ ] **Step 2: Reconfigure and build without environment workarounds**

```bash
env -u CPATH cmake -S source -B _build_macos -G Ninja -DCMAKE_BUILD_TYPE=Release
env -u CPATH ninja -C _build_macos fc_script
```

Expected: exit status zero. Record any remaining warning separately; do not
hide warnings through a global suppression.

- [ ] **Step 3: Run focused host and graphics suites**

```bash
env -u CPATH _build_macos/fc_script '{kyty_run_tests()}' '--gtest_filter=DevToolsBundle.*:DevToolsLifecycle.SelfTestNormalExitRunsOnCurrentHost:CoreMemoryAlloc.*:EmulatorGraphicsPackets.*:EmulatorGraphicsState.*'
```

Expected: nonzero selected test count and zero failures.

- [ ] **Step 4: Review and commit the cohesive implementation**

```bash
git diff --check
git status --short
git diff --stat
git add source/devtools source/unit_test/CMakeLists.txt
git commit -m 'fix(devtools): make macOS host boundary portable'
```

Do not claim the primary/secondary guest frontiers from this cycle; the next
cycle must re-run both strict roots after the build gate is green.
