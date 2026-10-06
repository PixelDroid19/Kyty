# Kyty engineering guide

Rules for maintainers, contributors and automated coding agents working on Kyty.
A more specific `AGENTS.md` inside a subdirectory takes precedence for files
below it.

## Mission

Run PlayStation 5 titles to a playable state, without visual errors and at
native-like performance, through general emulation that holds on other hosts,
GPUs and resolutions. A fix is accepted only when it is backed by evidence and
does not regress any title that already works.

`docs/BRINGUP.md` is the operating manual and the single source of truth for
status (invariants, methodology, architecture map, current verified frontier).
Read it before touching the runtime, GPU or audio stacks.

## Documentation map

| Area | Read this first |
| --- | --- |
| Bring-up, invariants, strict mode, current frontier | `docs/BRINGUP.md` |
| Runtime diagnostics (`kyty_agent`) | `docs/agent-tools.md` |
| Gen5 graphics: verified advances, evidence and exclusions | `docs/kyty-runtime-graphics-investigation-handoff.md` |
| Reproducible guest input routes | `docs/input-replay.md` |
| Graphics captures and offline scoring | `docs/graphics-captures.md` |
| Audio, ATRAC9/rack ABI, video paths | `docs/AUDIO.md`, `docs/ngs2-rack-voice-abi.md`, `docs/avplayer-video-path.md` |
| Host runtime layout and platform boundaries | `docs/HOST_RUNTIME.md` |
| Legal boundaries for data and research | `docs/legal-and-data-boundaries.md` |
| NID / export catalog conventions | `docs/devtools/export-catalog.md` |
| Render resolution policy | `docs/render-resolution.md` |
| Runtime stall snapshots | `docs/devtools/runtime-stall-snapshot.md` |

Before forming a hypothesis about a failure, read the "Evidence and exclusions"
sections of the bring-up manual and the graphics handoff. Re-testing a recorded
dead end wastes hours.

## Non-negotiable rules

- **Never invent guest behavior.** NIDs, ABI layouts, packet formats, register
  meanings, tiling, alignments, return codes and result values come from
  evidence. Rank sources: a live trace of the title > a published contract >
  the guest's own disassembly > a single secondary implementation. Agreement
  between independent implementations outweighs any one of them; a lone
  secondary implementation is a lead to verify, never an answer to ship.
- **Reimplement, never copy.** Other emulators and research projects are
  behavior references only. Do not copy their code, and do not name or link
  them in code, comments, commits or documentation.
- **No title-specific hacks.** No per-title branches, stubs that report success
  for unimplemented work, skipped draws or waits, or permissive bring-up flags
  as a fix. Implement the general contract.
- **Do not regress the frontier.** A strict run on current HEAD must stay at
  least as good as the frontier in `docs/BRINGUP.md`, unless a focused test
  proves the replaced behavior was itself wrong.
- **Respect the data boundary.** Never commit proprietary SDK code, firmware,
  keys, decrypted assets, game files, dumps, captures, logs, shader dumps,
  save data, title IDs, local paths or credentials.
  `docs/legal-and-data-boundaries.md` defines the boundary.

## Investigation workflow

1. **Reproduce** with a bounded strict run and a scripted input route
   (`docs/input-replay.md`). Record commit, host, GPU, driver and environment.
2. **Localize with machine evidence.** Use `kyty_agent` first (see Runtime
   diagnostics), native captures around the event, and the existing opt-in
   traces. For nondeterministic failures, always compare a good run with a bad
   run and repeat before concluding.
3. **One hypothesis at a time.** Design an experiment that can falsify it,
   measure, and record the outcome before moving on.
4. **Fix the general contract**, then verify on the affected title and on the
   titles that share the touched path.
5. **Record results.** Every disproven hypothesis goes into the docs with the
   observation that excluded it (title-specific: the graphics handoff;
   cross-title: `docs/BRINGUP.md`). A defect found but not fixed is recorded at
   once with file and line, the triggering input or state, and a suggested
   direction. Search existing issues and pull requests before starting
   non-trivial work.

## Temporary diagnostics

- Prefer existing diagnostics (`kyty_agent`, `KYTY_TRACE_*`, catalogs, native
  captures). Add temporary code only when they cannot answer the question.
- Tag every temporary line or block `DIAG-<TOPIC>`, gate it behind an
  environment variable or a bounded present window, cap its output, and write
  only to stderr or the run's evidence directory.
- Temporary diagnostics are never committed: `git diff` must contain no `DIAG`
  tag before staging.
- Agent-facing mutations must be explicit, bounded, local-only, auditable and
  disabled by default. Never expose arbitrary host memory, host paths, shell
  execution, credentials or protected workload data through the agent
  protocol.

## Runs and host resources

- Run one emulator instance at a time, always with a bounded duration. Never
  wait indefinitely on a title; use condition-based waits with timeouts.
- Do not build, and do not run heavy host analysis, while a title is running.
  Both distort timing and can trip the host memory guard.
- Never close, kill or reconfigure processes you did not start (user
  applications, debuggers, build jobs, other agents' runs). If one blocks a
  run, wait for it.
- Use your own build directory (`_build_linux_<slug>`) and limit build
  parallelism so the host stays responsive. Never reconfigure or delete a
  build directory you did not create.
- Keep evidence, captures and scratch files outside the repository and delete
  old run artifacts regularly.
- Wall-clock and frame-time runs use `PrintfDirection = Silent`; console
  logging is evidence-only.

## Engineering standards

### Code

- Simple, clean, readable and maintainable code.
- Each function or module has one clear responsibility.
- At most three levels of nested conditionals; prefer early returns.
- Modular architecture with low coupling; keep subsystems behind their own
  interfaces and platform-specific behavior behind explicit platform
  boundaries.
- No over-engineering: add an abstraction only when there is a real, present
  need for it.
- No legacy paths and no unnecessary fallbacks. When a behavior is replaced,
  remove the old one instead of keeping both.
- Write code that reads like the surrounding code: same naming, idioms and
  comment density. Code, comments, commits and documentation are in English.

### Runtime safety

- Validate guest-controlled sizes, offsets, counts, handles and pointers.
- Bound caches, captures, logs and every other host resource.
- Keep diagnostics actionable; no per-frame logging in normal runs.
- Preserve unrelated user changes and generated local artifacts.

## Testing

- Prioritize integration tests and real flows: a title reaching the affected
  state in a strict run, gate scripts, scored captures.
- Do not add a unit test for every change, and do not add a regression test
  for every bug; add one only when the risk of the regression justifies it.
- Use unit tests only for real logic that needs isolation: decoders, parsers,
  algorithms, address and format calculations, state machines with non-trivial
  rules.
- Test behavior and contracts (guest-visible results, ABI layouts, return
  codes), not internal implementation details that the behavior already
  covers.
- Never test presentation: overlay or window visuals, log and error message
  wording, or other text that can change.
- No trivial tests and no tests written only to raise coverage.
- When a change alters a contract covered by existing tests, update those
  tests in the same change.
- Run the narrowest relevant suite (`ctest` with the focused target or
  `scripts/run_unit_tests.lua`), complemented by manual QA on the affected
  titles when it makes sense.
- A successful build is not working functionality: validate the affected flow
  on the titles that exercise it.

## Documentation

- `docs/BRINGUP.md` holds status, fixes with their evidence, and exclusions.
- Refer to titles descriptively ("the sandbox title", "the remaining Unity
  title"); never by name, title ID, internal class name or install path.
- Keep entries factual: what was observed, what changed, how it was verified,
  what remains open.

## Git workflow

- Work on the active feature branch. Never commit directly to `main` or
  `release`, never force-push them, never create release tags by hand.
- Never reset, stash, clean or check out over uncommitted work you did not
  create. Stage explicit paths so unrelated changes stay out of the commit.
- Keep each commit to one logical change.

### Branch model

- `feature/*`, `refactor/*`, `perf/*` and documentation branches start from
  `main` and merge back into `main`.
- `fix/*` and `hotfix/*` branches start from `release` and merge back into
  `release`; accepted release fixes are brought forward to `main` without
  rewriting published history.

### Commit messages

[Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/):

```text
<type>[optional scope][!]: <imperative description>

[optional body]

[optional footer(s)]
```

Types: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `ci`,
`chore`, `revert`. Scopes name a stable subsystem (`graphics`, `loader`,
`kernel`, `audio`, `net`, `libc`, `dialog`, `docs`, …). Breaking changes use
`!` or a `BREAKING CHANGE:` footer. Commit messages never contain title names,
local paths, secrets, unsupported compatibility claims or references to other
emulator projects. Pull-request titles follow the same rules (squash merges).

## Versioning and builds

Semantic Versioning: patch versions come from `release`; minor and major from
`main`; breaking changes require a major version. Multiplatform binaries are
built only on explicit request or for an immutable `vMAJOR.MINOR.PATCH` tag,
created through the **Create Version** workflow.

## Verification before a commit or pull request

1. Review the complete diff for scope, secrets, private data, generated files
   and leftover `DIAG` code.
2. Build every affected target in your own build directory.
3. Run the relevant tests and the gate scripts for the touched domain:
   `scripts/check_emulator_boundaries.py`, `scripts/check_graphics_tables.py`,
   `scripts/kyty_playable_regression.py`.
4. Re-run the affected titles and the titles sharing the path; confirm no
   regression against the documented frontier.
5. Document commands, environment, results and untested limits.

## Compatibility claims

A boot, a window or one rendered frame does not mean a title is supported. A
"runs" claim requires a strict run (no `KYTY_BRINGUP_*` or permissive flags)
that reaches the stated state and holds it. Diagnostic input, stubs, permissive
GPU skips and console logging are not supported runtime modes. Reports name the
commit, host, GPU, driver, workload, duration and known limitations; graphics
claims need captures scored with `scripts/kyty_capture.py`, and gameplay claims
need `scripts/kyty_playable_regression.py`.

## Runtime diagnostics

`kyty_agent` (`docs/agent-tools.md`) is the canonical runtime debugging
surface; do not add a Python- or debugger-dependent workflow when the agent can
provide the same evidence. For hangs and runtime failures collect, in order:
`wait-ready`, `doctor`, a condition-based wait or `watch`, `events`,
`last-error`, `threads`, `sync-waits`, `diagnostics`, and a capture when
graphics are live. Prefer machine-readable JSON and bounded timeouts over
terminal scraping and fixed sleeps.

## Communication

Report to the maintainer in the language they use. State plainly what was
verified and what was not, give short progress notes during long runs, and ask
before any action that is hard to reverse or visible outside the machine.
