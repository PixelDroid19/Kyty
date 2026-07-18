# Validation gates

Match gates to task class (`AGENTS.md`). Full table applies to guest-contract
and strict-frontier work only.

## Branch gate (single hypothesis)

- [ ] `git diff` shows one live hypothesis; dead experiments reverted.
- [ ] Focused test failed before, passes after.
- [ ] `ninja -C _build_linux` (or host build dir) succeeds.
- [ ] New warnings investigated, not ignored.
- [ ] `git diff --check` clean.
- [ ] No private paths, titles, or raw logs in tracked files.

## Guest-contract gate

- [ ] Contract gate checklist complete (`contract-gate.md`).
- [ ] HLE registration in owning `Lib*.cpp` only.
- [ ] No behavioral fallback (assumed format, silent success, fabricated resource).
- [ ] One strict re-run advances or corrects the failure without bring-up flags.

## Integration gate (shared runtime)

- [ ] Touched ctest/integration targets green.
- [ ] `python3 scripts/verify_agent_toolkit.py` passes.
- [ ] If boot path touched: matrix discover-only or single-case strict smoke
  recorded in scratch (anonymous case IDs in any shared summary).

## Playability gate (explicit user request only)

- [ ] `scripts/kyty_playable_regression.py` with strict profile — no
  `KYTY_AUTO_CROSS` / `KYTY_BRINGUP_*`.
- [ ] `kyty_agent` phase/present evidence + optional capture score as **signal**,
  not acceptance by itself.

## Failure policy

- Unit green but strict regresses → requeue with first divergent checkpoint.
- Multiple merges before regression → bisect integration commits.
- Contradictory evidence → `blocked` in manifest; do not patch around uncertainty.
- Lower unresolved-import count alone is not playability.

## Game matrix fields (sanitized output)

Record per anonymous `case_*` id:

| Field | Notes |
|---|---|
| `outcome` | Matrix vocabulary (`runtime_started`, `host_crash`, …) |
| `first_failure` | Message class, not private path |
| `elapsed_s` | Wall time |
| `delta` | vs previous base SHA when comparing |

See `scripts/kyty_games_matrix.py` for outcome enums.
