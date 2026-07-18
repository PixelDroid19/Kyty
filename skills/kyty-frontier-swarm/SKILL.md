---
name: kyty-frontier-swarm
description: "Kyty emulator agent toolkit: run kyty_agent_doctor first, then strict reproduce, frontier manifest, contract gates, kyty_agent observation, matrix/playable regression. Use for guest crashes, HLE/NID work, PM4/GPU bugs, multi-case matrix, or any Kyty bring-up task."
license: MIT
compatibility: "Agent Skills + Cursor rules. Python 3.10+, Kyty checkout. Guest roots via KYTY_GUEST_ROOT / KYTY_GAMES_ROOT only."
---

# Kyty Frontier Swarm

**Start every session:**

```bash
python3 scripts/kyty_agent_doctor.py --task start
export KYTY_FRONTIER_MANIFEST=/tmp/kyty-frontier.json
```

Copy-paste commands: [references/quick-recipes.md](references/quick-recipes.md)

## Decision tree

```
User asks…
├─ build / script / doc only     → ninja + verify_agent_toolkit.py (no guest replay)
├─ diagnose crash / AV / hang    → doctor --task guest-crash → import-log → cheap evidence
├─ implement HLE / PM4 / layout  → contract gate → red test → fix → record-validation
├─ matrix / many packages        → kyty_games_matrix.py → from-matrix
└─ strict frontier / playability → BRINGUP.md loop + playable_regression.py
```

Coordinator rule: **one frontier, one failure, one hypothesis.**

Read `AGENTS.md` for invariants. Read `docs/BRINGUP.md` only for strict-frontier
sessions.

## Preserve scope

| User request | Agent stops at |
|---|---|
| Analysis / diagnosis / report | `export-report` + manifest; no edits unless asked |
| Implementation | Contract gate → tests → one strict re-run |
| Validation only | Tests + inspect; no drive-by fixes |
| Host tooling | Edit + verify; no BRINGUP handoff |

Never search for or redistribute guest dumps, keys, or title binaries.

## Preflight

1. `python3 scripts/kyty_agent_doctor.py` — fix `!!` checks before strict claims.
2. Repo branch/SHA; preserve unrelated dirty files.
3. Build dir: `_build_linux` (Linux) / `_build_macos` (macOS).
4. Runtime: `KYTY_AGENT_SOCK`, `KYTY_NATIVE_CAPTURE_DIR`, `kyty_agent doctor`.
5. Pin base SHA in `$KYTY_FRONTIER_MANIFEST` before parallel work.

## 1. Capture (manifest)

```bash
MANIFEST="$KYTY_FRONTIER_MANIFEST"
MF=skills/kyty-frontier-swarm/scripts/frontier_manifest.py

python3 "$MF" new --output "$MANIFEST" --base-sha "$(git rev-parse HEAD)" --force
# after strict replay:
python3 "$MF" import-log "$MANIFEST" --log /tmp/kyty-child.log --class auto
# after matrix:
python3 "$MF" from-matrix "$MANIFEST" --summary _matrix_scratch/run_*/summary.json
```

Schema: [references/manifest-schema.md](references/manifest-schema.md)

## 2. Investigate (real hypotheses)

1. Classify seam: HLE, kernel, loader, PM4, layout, shader, VideoOut, host.
2. One hypothesis **with cited observation** (log line, register, test name).
3. Falsifier required — `set-field --hypothesis … --falsifier … --status hypothesized`
4. Cheap evidence first: `KYTY_FAULT_LOG`, gdb, unit test, `/tmp` logs.
5. Two failed experiments → `blocked` + re-classify.

[references/contract-gate.md](references/contract-gate.md)

## 3. Implement + validate

Red → green → `record-validation --promote` when strict exit 0.

[references/validation-gates.md](references/validation-gates.md)

## 4. Runtime harness (not stdout scraping)

See [references/quick-recipes.md](references/quick-recipes.md) §2–3 and
`docs/agent-tools.md`.

## 5. Handoff

```bash
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py validate "$MANIFEST"
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py export-report "$MANIFEST"
```

Facts vs hypotheses vs assumptions must be labeled in chat.

Install: [INSTALL-AGENTS.md](../../INSTALL-AGENTS.md)
