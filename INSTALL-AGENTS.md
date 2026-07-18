# Installation instructions for agents

Deterministic bootstrap for installing the Kyty Frontier Swarm skill and
verifying harness scripts. Read `docs/legal-and-data-boundaries.md` first.

## Inputs the agent must obtain from the user

- Absolute path to the Kyty checkout.
- Agent host: `cursor`, `codex`, `claude`, or `all`.
- Optional: `$KYTY_GUEST_ROOT` or `$KYTY_GAMES_ROOT` for runtime checks (user
  supplies lawful local paths only).

Do not search for, download, or redistribute guest content.

## Bootstrap

```bash
cd /absolute/path/to/Kyty
python3 scripts/kyty_agent_doctor.py --task start
python3 scripts/verify_agent_toolkit.py

python3 scripts/install_agent_toolkit.py --agent cursor --dry-run
python3 scripts/install_agent_toolkit.py --agent cursor
```

`kyty_agent_doctor.py` is the **first command** every agent session should run.
It checks build artifacts, env hygiene, skill install, and prints the next
commands for the task at hand (`--task guest-crash|matrix|code`).

Use `--force` only after inspecting a destination diff. Never overwrite a
modified personal skill silently.

### Host notes

| Host | Install path |
|---|---|
| Cursor | `~/.cursor/skills/kyty-frontier-swarm/` |
| Codex | `$CODEX_HOME/skills/kyty-frontier-swarm/` |
| Claude Code | `$CLAUDE_CONFIG_DIR/skills/kyty-frontier-swarm/` |

Project rules: root `AGENTS.md`. Skill: `skills/kyty-frontier-swarm/SKILL.md`.

Disable conflicting global `~/AGENTS.md` (CCGS template) when it forces
"ask before every change" on Kyty engineering tasks.

## Build prerequisites (runtime harness)

```bash
cmake -S source -B _build_linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_linux fc_script kyty_agent
```

## Acceptance checks

```bash
python3 scripts/kyty_agent_doctor.py --json
python3 scripts/verify_agent_toolkit.py

python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py --help

# Matrix discovery (no launches) with synthetic or user games root:
python3 scripts/kyty_games_matrix.py --discover-only --games-root /path/to/root

# Agent client (guest must be running with KYTY_AGENT_SOCK set):
_build_linux/agent/kyty_agent --sock "$KYTY_AGENT_SOCK" doctor
```

Acceptance requires:

- Skill directory complete under repo and installed host path.
- `verify_agent_toolkit.py` green.
- `git status` shows no guest binaries or private paths staged.

## Strict replay (diagnostic only)

```bash
export KYTY_GUEST_ROOT=/user/supplied/package/root
_build_linux/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

No `KYTY_BRINGUP_*` for compatibility claims. Record first strict failure to
scratch; update frontier manifest:

```bash
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py new \
  --output /tmp/kyty-frontier.json --base-sha "$(git rev-parse HEAD)"
```

## Reusable prompt for an installation agent

```text
Install the Kyty agent toolkit from my Kyty checkout. Read INSTALL-AGENTS.md and
docs/legal-and-data-boundaries.md. Run verify_agent_toolkit.py, then
install_agent_toolkit.py with --dry-run for cursor. Preserve unrelated files;
stop on differing destinations unless I approve --force. Report install paths and
check results. I will supply my own lawful local guest roots; do not obtain or
redistribute game data.
```

## Documentation map

| Doc | When to read |
|---|---|
| `AGENTS.md` | Every session (short invariants + task weighting) |
| `skills/kyty-frontier-swarm/SKILL.md` | Frontier/HLE/runtime orchestration |
| `docs/agent-tools.md` | `kyty_agent` protocol and loops |
| `docs/BRINGUP.md` | Strict-frontier phase gates only |
| `docs/legal-and-data-boundaries.md` | Before any private replay |
