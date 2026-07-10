# AGENT.md — How to work on Kyty

This file is the operating manual for any agent (or human) working on this
repository. Read it fully before touching code. It encodes how to think, how to
write code, how to verify, and the current state of the macOS/PS5 port.

## 0. Mission

Kyty is a PlayStation experimental emulator. This fork targets **running real
PS5 games on Apple Silicon (macOS)**. The reference game during bring-up is
Dead Cells (a Haxe/HashLink title). The emulator executes the guest x86-64 code
natively (no CPU recompiler) and translates the guest GPU command stream to
Vulkan via MoltenVK.

## 1. Principles (non-negotiable)

1. **Real results over appearances.** A change is only "done" when the emulator
   builds and the game demonstrably advances further than before (new HLE calls,
   a new frontier error, a rendered frame). "It compiles" is not "it works".
2. **Never invent.** Do not fabricate NIDs, register values, struct layouts, or
   ABI details. Every constant must be derived from evidence: the guest binary,
   a runtime capture, or a validated algorithm. If you don't know a value,
   instrument the code to print the real one — do not guess and move on.
3. **No weird fallbacks, no scattered dead code.** Prefer one correct
   implementation over several half-working paths. When you relax an assertion
   for bring-up, gate it behind an env var and say clearly (in a comment) why it
   is safe and what it may break.
4. **Never give up silently.** When blocked, build a tool that gives you
   visibility (a tracer, a signal-safe logger, a register dump) rather than
   guessing. Diagnose the root cause, then fix that.
5. **Brutal honesty.** Report what actually happened: if a step was skipped, a
   value was guessed, or output is probably wrong, say so. Distinguish
   "verified" from "hypothesis".

## 2. Build

macOS is treated as a POSIX (`KYTY_PLATFORM_LINUX`) target with `__APPLE__`
divergences. The binary is built x86_64 and runs under Rosetta 2 on Apple
Silicon (Kyty executes the guest's x86-64 code natively).

```
cmake -S source -B _build_macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
ninja -C _build_macos
```

Output: `_build_macos/fc_script` (the Lua CLI runner; the Qt launcher is skipped
on macOS). MoltenVK ships in `source/3rdparty/vulkan/lib/macos/`.

## 3. Run

Run configs live in `_run/`. The HLE-libc bring-up config for the reference game
is `_run/deadcells_hle.lua` (loads the eboot without the game's own libc.prx and
loads Kyty's HLE modules). Typical invocation:

```
cd _build_macos
KYTY_STUB_MISSING=1 KYTY_GFX_PERMISSIVE=1 ./fc_script ../_run/deadcells_hle.lua
```

`EXIT()` calls `std::_Exit` — on macOS `dbg_exit` flushes stdout first, and main
sets `setvbuf(stdout, _IONBF)`, so fatal errors are visible. Redirect to a log
and grep it; the graphics/MoltenVK output is very verbose.

### Bring-up env flags (all gated, off by default)

- `KYTY_STUB_MISSING=1` — unimplemented imports resolve to a per-symbol stub that
  logs the NID the first time it is actually **called** ("CALLED missing stub:
  NID [lib]"). The stub table is statically compiled (a runtime-generated code
  page is not translatable under Rosetta).
- `KYTY_GFX_PERMISSIVE=1` — unknown indirect GPU registers are skipped with a
  warning instead of aborting. Use only for bring-up; skipped state can produce
  incorrect rendering.
- `KYTY_FAULT_LOG=1` — async-signal-safe fault logger (vaddr/rip/regs via
  `write(2)`), for diagnosing SIGSEGV/SIGBUS without a debugger.
- `KYTY_TRACE_LIBC=1` — single-step (TF) tracer over a code range.
- `KYTY_SKIP_UD2=1` — SIGILL handler skips a guest `ud2` (diagnostic only).

## 4. How to diagnose (Rosetta constraints)

Under Rosetta 2, ordinary debugging is limited:

- `lldb` cannot reliably pause the process.
- The guest thread's segment registers (`fs`/`gs`) are managed by Rosetta for
  the guest, **not** macOS's TSD. Therefore native code that runs on a guest
  thread must **not** use C++ `thread_local`, `pthread_getspecific`, or std
  primitives that touch TSD — they fault. Use a thread id from
  `syscall(SYS_thread_selfid)` and index a plain array instead.
- Signal handlers that run in guest context must be **async-signal-safe**: no
  `printf`, no `malloc`, no C++ string. Use `write(2)` and pre-mapped buffers.
- Single-step (TF) works and is the basis of the in-tree tracer. `wrfsbase` and
  direct `fs:`/`gs:` accesses fault.

When something crashes: reproduce with the smallest run, add a signal-safe log or
the TF tracer, read the real register/address values, then fix the root cause.

## 5. NID resolution

Imported/exported symbols are keyed by NID:
`base64_custom(first 8 bytes LE of SHA1(name + suffix))`, where suffix =
`518D64A635DED8C1E6B039B1C3E55230` and the base64 alphabet is
`ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-`. Symbols are
further keyed by `name[library_vN][module_vN.N][type]`. To implement a function
you must know its real name/signature; recover it from the guest binary's import
tables and public API knowledge, never by guessing.

## 6. Architecture map

- **CPU**: guest x86-64 runs natively; HLE functions are reached via SafeCall
  trampolines. `source/emulator/src/Loader/RuntimeLinker.cpp` links the eboot,
  resolves symbols, and installs the exception handler.
- **Memory**: `source/emulator/src/Kernel/Memory.cpp` (direct + flexible
  memory). Demand-paging is used to back regions lazily (mmap in the fault
  handler, malloc-free so it is signal-safe).
- **libc (HLE)**: `source/emulator/src/Libs/LibC.cpp`. We HLE the C library and
  run the game **without** its own libc.prx, which avoids executing the guest
  libc's startup (segment/TSD/host-guest issues under Rosetta). The x86-64 SysV
  `va_list` layout matches the host's, so variadic functions forward to the host
  `v*printf`/`v*scanf` after the fixed args are consumed via the VaContext.
- **Kernel/threads**: `source/emulator/src/Kernel/Pthread.cpp`,
  `source/lib/Core/src/Threads.cpp`.
- **Graphics**: the PS5 GPU API (Agc) is implemented as the "Gen5" backend in
  `source/emulator/src/Graphics/Graphics.cpp` (command-buffer builders and
  shader/register setup) and `.../GraphicsRun.cpp` (the PM4 command-processor
  parser). The command stream is PM4; draws are translated in
  `.../GraphicsRender.cpp`; GCN shader bytecode is recompiled to SPIR-V in
  `.../ShaderParse.cpp` + `.../ShaderSpirv.cpp`. Texture tiling/sizes are in
  `.../Tile.cpp`. Present goes through `.../Window.cpp` and VideoOut.
- **Module registration**: `source/emulator/src/Libs/Libs.cpp` maps module ids
  (e.g. `libGraphicsDriver_1`) to their `Init*` functions; the run's Lua must
  `kyty_load_symbols(...)` each module the game imports.

## 7. Current state (bring-up)

The reference game **boots, runs its engine, loads assets, submits GPU command
buffers, translates render state, recompiles its shaders to SPIR-V, and presents
frames** on Apple M2 (the window title shows an advancing frame counter). Frames
are currently black: the draw path stops in the texture size/tiling code
(`Tile.cpp`), which is table-driven and does not cover the game's textures.

**Next frontier**: a generic (not table-driven) Agc texture size/tiling
computation so textures resolve and pixels appear, then auditing the permissive
relaxations (skipped registers, ignored render state) for correct visuals.

## 8. Checklist before committing

1. Does it build clean (`ninja -C _build_macos`, no new errors)?
2. Did you run the game and confirm it advanced (new log, new frontier, or a
   frame)? Paste the evidence.
3. Is every new constant/NID justified by evidence, not a guess? If a value is a
   hypothesis, is it commented as such?
4. Are bring-up relaxations gated behind an env flag with a comment on why they
   are safe and what they may break?
5. No scattered dead code, no external project names in comments, no build
   artifacts committed.
