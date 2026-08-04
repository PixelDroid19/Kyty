# Kyty engineering guide

This file defines the repository-wide rules for maintainers, contributors, and
automated coding agents. More specific instructions may exist inside a
subdirectory; when they do, the closest applicable file takes precedence.

## Documentation map

The operating manual is `docs/BRINGUP.md` — mission, non-negotiable
invariants, investigation methodology, the architecture map, and the
**current verified frontier**. Read it before touching the runtime, GPU, or
audio stacks; its methodology is not optional.

| Area | Read this first |
| --- | --- |
| PS5 bring-up, invariants, strict-mode rules, current frontier | `docs/BRINGUP.md` |
| Agent-facing runtime diagnostics (`kyty_agent`) | `docs/agent-tools.md` |
| Gen5 graphics: verified advances, evidence and exclusions, validation gate | `docs/kyty-runtime-graphics-investigation-handoff.md` |
| Reproducible guest input routes | `docs/input-replay.md` |
| Graphics captures and offline scoring | `docs/graphics-captures.md` |
| Audio, ATRAC9/rack ABI, video paths | `docs/AUDIO.md`, `docs/ngs2-rack-voice-abi.md`, `docs/avplayer-video-path.md` |
| Host runtime layout and platform boundaries | `docs/HOST_RUNTIME.md` |
| Legal boundaries for data and research | `docs/legal-and-data-boundaries.md` |
| NID / export catalog conventions | `docs/devtools/export-catalog.md` |
| Render resolution policy | `docs/render-resolution.md` |
| Runtime stall snapshots | `docs/devtools/runtime-stall-snapshot.md` |

`docs/BRINGUP.md` § Current verified frontier is the single source of truth
for status; this file deliberately does not restate it. Before forming a
hypothesis about a graphics failure, read the "Evidence and exclusions"
section of the handoff doc and the invariants of the bring-up manual — the
manual already warns against walking into dead ends, and that warning costs
real hours every time it is ignored.

## Commit messages

Kyty follows [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/).
Every commit and squash-merge title must use this form:

```text
<type>[optional scope][optional !]: <description>

[optional body]

[optional footer(s)]
```

Use lowercase types and an imperative, concise description. The primary types
are:

- `feat`: a new user-visible or emulator capability; normally implies a minor
  release;
- `fix`: a bug fix; normally implies a patch release;
- `perf`: a measurable performance improvement;
- `refactor`: an internal change without new behavior or a bug fix;
- `test`: test-only changes;
- `docs`: documentation-only changes;
- `build`: build-system or dependency changes;
- `ci`: automation and release-pipeline changes;
- `chore`: repository maintenance that fits no more specific type;
- `revert`: reverts a previous change and identifies the reverted commit.

Scopes should identify a stable subsystem, for example `graphics`, `loader`,
`audio`, `kernel`, `build`, `ci`, or `docs`:

```text
feat(graphics): add attachment resolve tracking
fix(loader): reject truncated relocation entries
docs(contributing): clarify patch branch policy
```

Breaking changes must use `!` in the prefix or a `BREAKING CHANGE:` footer:

```text
feat(runtime)!: replace the guest module lifecycle contract

BREAKING CHANGE: modules now initialize through the lifecycle coordinator.
```

Do not include private workload names, local paths, secrets, or unsupported
compatibility claims in commit messages. Keep unrelated changes in separate
commits. Pull-request titles must follow the same rules because Kyty uses
squash merges to produce changelog-ready history.

## Branch model

- Create `feature/*`, `refactor/*`, `perf/*`, and documentation branches from
  `main`; merge them back into `main`.
- Create `fix/*` and `hotfix/*` branches from `release`; merge them back into
  `release`.
- Do not merge new features directly into `release`.
- Bring accepted release fixes forward to `main` without rewriting published
  history.
- Never force-push `main`, `release`, or version tags during normal development.

## Versioning and builds

Kyty uses Semantic Versioning:

- `patch` versions are created from `release`;
- `minor` and `major` versions are created from `main`;
- breaking changes require a major version.

Ordinary pushes and pull requests do not create multiplatform binaries. The
full Windows, Linux, and macOS build runs only for an explicit manual request
or an immutable `vMAJOR.MINOR.PATCH` release tag. Use the **Create Version**
workflow to calculate and publish a release; do not create release tags by hand
unless recovering the automation.

## Engineering expectations

- Prefer general, evidence-backed implementations over title-specific hacks.
- Use lawful clean-room research and license-compatible public sources.
- Never commit proprietary SDK code, firmware, keys, decrypted assets, game
  files, private dumps, or other protected material; `docs/legal-and-data-boundaries.md`
  defines the boundary.
- **Never invent guest behavior.** NIDs, ABI signatures, packet layouts,
  register meanings, formats, tiling, alignments, and return codes come from
  evidence. Rank sources when they disagree: a live capture or trace of the
  real title is worth more than a published contract, a published contract is
  worth more than the guest's own disassembly where it is newer, and agreement
  among independent implementations is worth more than any single one of them.
  A lone secondary implementation is a lead to verify, not an answer to ship.
  Whatever you adopt, reimplement it in Kyty's own decoder and HLE — reuse the
  behavior, never another project's code.
- **Do not stack dead ends.** When a hypothesis is disproven, write it down
  before moving on — hypothesis, observation, next hypothesis, plus the commit
  or issue that closed it. Title-specific dead ends go in the handoff doc's
  "Evidence and exclusions"; cross-title ones belong in `docs/BRINGUP.md`. A
  dead end that exists only in a session log is invisible to the next session
  and will be walked into again.
- **A bug you do not fix is a bug you record.** When a defect is found but not
  fixed in the same session, record it immediately with the file and line, the
  input or state that triggers it, and a suggested direction. Search existing
  issues and pull requests before starting non-trivial work so the fix is not
  already specified elsewhere.
- Do not regress the working frontier: a strict run on current HEAD must stay
  at least as good as the frontier described in `docs/BRINGUP.md`, unless a
  focused test proves the behavior being changed is itself incorrect.
- Validate guest-controlled sizes, offsets, counts, handles, and pointers.
- Keep platform-specific behavior behind explicit platform boundaries.
- Bound caches, captures, logs, and other host resource use.
- Add focused tests for corrected contracts and regression-prone behavior.
- Keep diagnostics actionable and avoid noisy per-frame logging.
- Preserve unrelated user changes and generated local artifacts.

## Verification

Before opening or merging a pull request:

1. review the complete diff for scope, secrets, private paths, and generated
   files;
2. run the narrowest relevant tests — `ctest` with the focused target, or
   `scripts/run_unit_tests.lua` — and the integration tests that cover the
   changed contract;
3. build every affected target locally in your own build directory;
4. run the relevant scripted gates where the change touches their domain:
   `scripts/check_emulator_boundaries.py`, `scripts/check_graphics_tables.py`,
   `scripts/kyty_playable_regression.py`;
5. request a manual hosted build when platform risk justifies it;
6. document exact commands, environment, results, and untested limitations.

Prefer automated evidence — a test that fails on the old behavior, a capture
score, a gate script's exit code — over judging a run by eye.

A boot, a window, or a single rendered frame does not mean a title is
supported.
Compatibility reports must identify the commit, host, GPU, driver, workload,
duration, and known limitations. A "runs" claim means a strict run — no
`KYTY_BRINGUP_*` or legacy permissive flags — that reaches the stated state
and holds it; diagnostic input, stubs, permissive GPU skips, and console
logging are not supported runtime modes. Use `PrintfDirection = Silent` for
wall-clock runs; Console logging is evidence-only and destroys frame-time
comparability. Graphics claims need captures scored with
`scripts/kyty_capture.py` and, for gameplay, `scripts/kyty_playable_regression.py`
rather than a screenshot someone looked at.

## Agent coordination and local work

- Work in your own build directory. The `_build_*` directories are shared
  scratch; another agent may be mid-build in one. Create your own
  (`_build_linux_<slug>` pattern), never reconfigure or delete a build
  directory you did not create, and keep generated artifacts out of tracked
  paths.
- Never commit captures, frame dumps, `_Shaders/` dumps, logs, `_SaveData`,
  guest paths, title IDs, or raw multi-megabyte run output. Keep session
  evidence in an untracked scratch area outside the repository.
- The reference workload and guest dumps live outside the repo; never commit
  them and never paste protected content into issues, PRs, or docs.

## Runtime diagnostics

Use the native `kyty_agent` interface documented in
[`docs/agent-tools.md`](docs/agent-tools.md) as the canonical runtime debugging
surface. Do not add a Python- or debugger-dependent workflow when the native
agent can provide the same evidence.

For hangs or runtime failures, collect evidence in this order: `wait-ready`,
`doctor`, a condition-based wait or `watch`, `events`, `last-error`, `threads`,
`sync-waits`, `diagnostics`, and a capture when graphics are live. Prefer
machine-readable JSON and bounded timeouts over terminal scraping and fixed
sleeps. For reproducible input, script the route
(`docs/input-replay.md`) instead of driving input by hand.

Agent-facing mutations must be explicit, bounded, local-only, auditable, and
disabled by default unless they are established diagnostic input. Never expose
arbitrary host memory, arbitrary host paths, shell execution, credentials, or
protected workload data through the agent protocol.
