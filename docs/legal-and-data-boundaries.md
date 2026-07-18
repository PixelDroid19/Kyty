# Legal and data boundaries (agents)

Kyty agent tooling orchestrates **your** local emulator checkout and **your**
lawfully obtained guest inputs. This repository does not distribute firmware,
games, keys, decrypted executables, or proprietary decompilation.

## You must

- Keep private dumps, saves, keys, and scratch logs **outside Git**.
- Reference fixtures only as `$KYTY_GUEST_ROOT`, `$KYTY_GAMES_ROOT`, or anonymous
  `case_*` ids in shared summaries.
- Obtain and decrypt content only where your jurisdiction permits.
- Record behavioral facts with local captures or tests before implementing guest
  contracts.

## Agents must not

- Search the network or chat for copyrighted dumps, keys, or title-specific
  binaries to "unblock" a run.
- Commit absolute guest paths, title IDs, screenshots, or multi-megabyte logs.
- Paste GPL implementation code from other emulators into Kyty (MIT tree).
- Treat `KYTY_BRINGUP_MODE=unsafe`, `KYTY_AUTO_CROSS`, or missing-import stubs
  as compatibility acceptance.

## Scratch directories

Use untracked paths only:

- `/tmp/kyty-*`
- `_matrix_scratch/`
- `_scratch_playable/`

Matrix and playable scripts emit **sanitized** JSON summaries; raw child logs
stay in scratch.

## Third-party toolkits

Patterns may be inspired by external agent kits (e.g. evidence manifests,
contract gates, visual regression harnesses). Kyty ships its own MIT skill and
scripts; optional Ghidra/MCP integrations are host-specific and not required.

This document is not legal advice.
