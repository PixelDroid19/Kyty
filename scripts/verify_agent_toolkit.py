#!/usr/bin/env python3
"""Verify Kyty agent toolkit: manifest CLI, script tests, skill layout."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(cmd: list[str], *, cwd: Path | None = None) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def check_skill_layout() -> None:
    skill = ROOT / "skills" / "kyty-frontier-swarm"
    required = [
        skill / "SKILL.md",
        skill / "scripts" / "frontier_manifest.py",
        skill / "references" / "validation-gates.md",
        skill / "references" / "contract-gate.md",
        skill / "references" / "manifest-schema.md",
        skill / "references" / "agent-harnesses.md",
        skill / "references" / "agent-contracts.md",
        skill / "references" / "quick-recipes.md",
        ROOT / ".cursor" / "rules" / "kyty-agent-toolkit.mdc",
        ROOT / "scripts" / "kyty_agent_doctor.py",
    ]
    missing = [str(p.relative_to(ROOT)) for p in required if not p.is_file()]
    if missing:
        raise SystemExit(f"missing skill files: {missing}")
    print("skill layout OK")


def check_manifest_cli() -> None:
    manifest = ROOT / "skills" / "kyty-frontier-swarm" / "scripts" / "frontier_manifest.py"
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "m.json"
        log = Path(td) / "child.log"
        log.write_text(
            "Patch tls GD call REX.W on main image: 2 site(s)\n"
            "FAULTR 0x0000000000000008 0x0000000900d907c5\n"
            "FATAL-ACCESS-VIOLATION 0x0000000000000008 0x0000000900d907c5\n",
            encoding="utf-8",
        )
        summary = Path(td) / "summary.json"
        summary.write_text(
            json.dumps(
                {
                    "all_cases": [
                        {
                            "case_id": "case_abc123",
                            "outcome": "host_crash",
                            "first_frontier": "host_crash",
                            "child_exit": 139,
                            "present": 0,
                            "progressed": False,
                        }
                    ]
                }
            ),
            encoding="utf-8",
        )
        run([sys.executable, str(manifest), "new", "--output", str(out), "--base-sha", "abc123"])
        run(
            [
                sys.executable,
                str(manifest),
                "add-blocker",
                str(out),
                "--id",
                "test-item",
                "--class",
                "host",
                "--evidence",
                "synthetic",
            ]
        )
        run([sys.executable, str(manifest), "import-log", str(out), "--log", str(log), "--id", "from-log"])
        run([sys.executable, str(manifest), "from-matrix", str(out), "--summary", str(summary)])
        run(
            [
                sys.executable,
                str(manifest),
                "set-field",
                str(out),
                "--id",
                "from-log",
                "--status",
                "hypothesized",
                "--hypothesis",
                "null parent frame",
                "--falsifier",
                "terminator stops walk",
            ]
        )
        run(
            [
                sys.executable,
                str(manifest),
                "record-validation",
                str(out),
                "--id",
                "from-log",
                "--strict-command",
                "fc_script run",
                "--exit-code",
                "0",
                "--promote",
            ]
        )
        run([sys.executable, str(manifest), "validate", str(out)])
        run([sys.executable, str(manifest), "summary", str(out)])
        run([sys.executable, str(manifest), "export-report", str(out), "--max-items", "5"])
    print("manifest CLI OK")


def check_doctor() -> None:
    run([sys.executable, str(ROOT / "scripts" / "kyty_agent_doctor.py"), "--json"])
    print("doctor OK")


def check_script_tests() -> None:
  tests = [
      ROOT / "scripts" / "test_kyty_games_matrix.py",
      ROOT / "scripts" / "test_kyty_playable_regression.py",
      ROOT / "scripts" / "test_kyty_capture.py",
  ]
  for t in tests:
      if t.is_file():
          run([sys.executable, str(t)])
  print("script unit tests OK")


def main() -> int:
    check_skill_layout()
    check_manifest_cli()
    check_doctor()
    check_script_tests()
    print("verify_agent_toolkit: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
