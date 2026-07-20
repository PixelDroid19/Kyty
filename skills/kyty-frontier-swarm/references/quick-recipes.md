# Quick recipes (copy-paste)

Repo-relative paths. Replace `$KYTY_GUEST_ROOT` with a user-supplied package root.

## 0. Session start

```bash
cd /path/to/Kyty
python3 scripts/kyty_agent_doctor.py --task start
export KYTY_FRONTIER_MANIFEST=/tmp/kyty-frontier.json
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py new \
  --output "$KYTY_FRONTIER_MANIFEST" --base-sha "$(git rev-parse HEAD)" --force
```

## 1. Strict private replay (diagnostic)

```bash
# No KYTY_BRINGUP_* for compatibility claims. Optional discovery only:
export KYTY_FAULT_LOG=1
export KYTY_TLS_DIAG=1

ninja -C _build_linux fc_script
_build_linux/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT" \
  2>&1 | tee /tmp/kyty-child.log

python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py import-log \
  "$KYTY_FRONTIER_MANIFEST" --log /tmp/kyty-child.log \
  --id "$(git rev-parse --short HEAD)-strict" --class auto
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py summary \
  "$KYTY_FRONTIER_MANIFEST"
```

## 2. kyty_agent observe (preferred runtime loop)

Terminal A:

```bash
export KYTY_AGENT_SOCK=/tmp/kyty-agent.sock
export KYTY_NATIVE_CAPTURE_DIR=/tmp/kyty-captures
mkdir -p "$KYTY_NATIVE_CAPTURE_DIR"
ninja -C _build_linux fc_script kyty_agent
_build_linux/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

Terminal B:

```bash
export KYTY_AGENT_SOCK=/tmp/kyty-agent.sock
A=_build_linux/agent/kyty_agent
$A wait-ready --timeout-ms 60000
$A status
$A wait-phase interactive --timeout-ms 45000
$A capture
$A watch --seconds 10 --min-fps 2    # stall detector
```

Exit `125` = guest dead — relaunch, do not sleep-loop.
For automated startup input, launch with
`python3 scripts/kyty_playable_regression.py --guest-root "$KYTY_GUEST_ROOT"`
instead of Terminal A. It confirms each of exactly three Cross taps, clears the
overlay, then waits through loading without sending more input. Do not set
`KYTY_AUTO_CROSS`.

## 3. Matrix discover + run

```bash
export KYTY_GAMES_ROOT=/path/to/games_parent
export KYTY_MATRIX_SCRATCH=_matrix_scratch

python3 scripts/kyty_games_matrix.py --discover-only

KYTY_GAMES_ROOT="$KYTY_GAMES_ROOT" python3 scripts/kyty_games_matrix.py \
  --fc-script _build_linux/fc_script \
  --kyty-agent _build_linux/agent/kyty_agent

# Import latest summary into manifest:
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py from-matrix \
  "$KYTY_FRONTIER_MANIFEST" --summary _matrix_scratch/run_*/summary.json
```

## 4. HLE / guest contract (minimal loop)

1. `import-log` or manual `add-blocker` with **observation line**.
2. Write falsifier in manifest: `set-field … --hypothesis … --falsifier … --status hypothesized`
3. Red test (unit or integration under `source/unit_test` / `source/integration_test`).
4. Minimal implementation in owning module.
5. `ninja -C _build_linux` + focused gtest filter.
6. One strict re-run; `record-validation` with command + exit code.

```bash
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py set-field \
  "$KYTY_FRONTIER_MANIFEST" --id ITEM_ID --status contract-reviewed \
  --hypothesis "one sentence" --falsifier "what would disprove it"

python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py record-validation \
  "$KYTY_FRONTIER_MANIFEST" --id ITEM_ID --strict-command \
  "_build_linux/fc_script scripts/run_guest.lua \$KYTY_GUEST_ROOT" --exit-code 0
```

## 5. Cheap crash evidence (before long replay)

```bash
export KYTY_FAULT_LOG=1
# gdb at guest RIP from FAULTR line — see child.log
rg -n 'FAULTR|FATAL-ACCESS|EXIT_NOT_IMPLEMENTED|CALLED missing' /tmp/kyty-child.log | tail -20
```

## 6. Verify toolkit after script changes

```bash
python3 scripts/verify_agent_toolkit.py
python3 scripts/kyty_agent_doctor.py --json
```

## 7. Handoff snippet for chat

```bash
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py export-report \
  "$KYTY_FRONTIER_MANIFEST" --max-items 8
```

Paste output into the session summary (no private paths).
