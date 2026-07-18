# Agent harness notes (Cursor, Codex, Claude Code)

The canonical skill lives at `skills/kyty-frontier-swarm/` in the Kyty repo.
Resolve bundled scripts relative to this directory, not the chat cwd.

## Cursor

- Project rules: `.cursor/rules/kyty-agent-toolkit.mdc` (always on in this repo).
- Personal skill: `python3 scripts/install_agent_toolkit.py --agent cursor`
- **Session start:** `python3 scripts/kyty_agent_doctor.py --task start`
- Recipes: `skills/kyty-frontier-swarm/references/quick-recipes.md`
- Large logs: keep under `/tmp` or `_matrix_scratch/`; grep/rg snippets into
  chat. Do not paste multi-megabyte child logs.
- Subagents: use only when the user requests parallel lanes. Coordinator keeps
  manifest and merge.

## Codex / Claude Code

- Personal skill install via `install_agent_toolkit.py --agent codex` or
  `--agent claude`.
- MCP (Ghidra, etc.) is optional. Kyty does not require GPL toolkit integrations;
  use your host's RE tools and export snippets to scratch files.

## Path conventions

| Artifact | Location |
|---|---|
| Strict scratch | `/tmp/kyty-*`, `_matrix_scratch/`, `_scratch_playable/` |
| Matrix output | `KYTY_MATRIX_SCRATCH` (anonymous case IDs only in summaries) |
| Guest root | `$KYTY_GUEST_ROOT` or `$KYTY_GAMES_ROOT` — never commit |
| Build (Linux) | `_build_linux/fc_script`, `_build_linux/agent/kyty_agent` |

## Output discipline

- Separate **verified fact**, **capture evidence**, **hypothesis**, **untested
  assumption** in reports.
- Never cite `KYTY_BRINGUP_MODE=unsafe` or matrix runs with bring-up flags as
  compatibility proof.
- Commit messages: emulator behavior only, no private fixture names.
