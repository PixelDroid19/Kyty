# Agent contracts (delegation)

Use when the user explicitly requests parallel work. Default: single agent,
single hypothesis.

## Coordinator

- Owns pinned base SHA, manifest, worktree creation, merge order, shared files.
- Rejects stale-base work and incomplete contract packets.
- Serializes integration builds and strict re-runs.

## Investigation worker

Read-only on source unless assigned probes. Returns:

- First strict failure with evidence lines (no guessed "last HLE").
- Classification and one falsifiable hypothesis.
- Suggested cheap next experiment (gdb, test, log flag).
- Updated manifest item in JSON fragment — does not edit repo manifest file in parallel.

## Implementation worker

Assigned worktree and file list only. Returns:

- Contract interpretation and changed files.
- Test commands and results.
- Risks and unimplemented edges.
- Commit SHA if requested.

Does not edit manifest, integration branch, or unrelated modules.

## Verification worker

Independently re-runs focused tests and one strict replay if in scope. Returns
pass/fail with commands and exit codes. Does not silently fix failures unless
assigned.

## Regression worker

Uses integrated build + `kyty_games_matrix.py` or `kyty_playable_regression.py`.
Returns anonymous case outcomes and deltas vs baseline SHA. No title names in
report text.
