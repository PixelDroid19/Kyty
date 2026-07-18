# Frontier manifest schema

One JSON manifest per pinned integration run. Coordinator owns writes; workers
return summaries for merge.

## Top level

```json
{
  "schema_version": 1,
  "run": {
    "created_at": "ISO-8601 UTC",
    "base_sha": "git rev",
    "host": "linux|macos|windows",
    "notes": "optional sanitized context"
  },
  "items": []
}
```

## Item fields

| Field | Purpose |
|---|---|
| `id` | Stable slug (`av-null-rbp-8`, `libc-wcslen`, `pm4-barrier-range`) |
| `class` | `hle`, `kernel`, `loader`, `pm4`, `layout`, `shader`, `videoout`, `host`, `other` |
| `nid` | Optional 11-char NID when applicable |
| `symbol` | Export name or null |
| `status` | Lifecycle below |
| `priority` | Coordinator rank (criticality, fanout, dependency) |
| `observations` | Evidence lines: log excerpt class, RIP, registers, test name |
| `hypothesis` | Single active hypothesis or null |
| `falsifier` | What would disprove the hypothesis |
| `contract` | Gate fields when known |
| `implementation` | branch, commit, files (no private paths) |
| `validation` | commands, exit codes, case ids |
| `blockers` | Missing evidence |

## Status lifecycle

`observed` → `hypothesized` → `contract-reviewed` → `implementing` →
`implemented` → `branch-verified` → `integrated` → `strict-verified`

Use `blocked` from any state with reason.

Never skip `observed` → `implemented`. Never set `strict-verified` without a
strict re-run command recorded under `validation`.

## CLI

```bash
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py new --output MANIFEST.json --base-sha SHA
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py add-blocker MANIFEST.json --id ID --class CLASS --evidence "..."
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py import-log MANIFEST.json --log child.log --class auto
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py from-matrix MANIFEST.json --summary summary.json
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py set-field MANIFEST.json --id ID --status hypothesized --hypothesis "..." --falsifier "..."
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py record-validation MANIFEST.json --id ID --strict-command "..." --exit-code 0 --promote
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py export-report MANIFEST.json
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py validate MANIFEST.json
python3 skills/kyty-frontier-swarm/scripts/frontier_manifest.py summary MANIFEST.json
```

Environment: `KYTY_FRONTIER_MANIFEST` defaults to `/tmp/kyty-frontier.json` in recipes.
