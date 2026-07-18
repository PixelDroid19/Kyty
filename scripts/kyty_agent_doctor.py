#!/usr/bin/env python3
"""
Kyty agent toolkit doctor — run at session start.

Checks repo layout, build artifacts, skill install, env hygiene, and optional
runtime socket. Prints (or emits JSON with) the next commands to run.

Usage:
  python3 scripts/kyty_agent_doctor.py
  python3 scripts/kyty_agent_doctor.py --json
  python3 scripts/kyty_agent_doctor.py --task guest-crash
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

FORBIDDEN_ENV = (
    "KYTY_BRINGUP_MODE",
    "KYTY_BRINGUP_FEATURES",
    "KYTY_BRINGUP_SUBSYSTEMS",
    "KYTY_BRINGUP_BURST_LIMIT",
    "KYTY_BRINGUP_BURST_WINDOW_MS",
    "KYTY_BRINGUP_ALLOW_DIAGNOSTIC",
    "KYTY_STUB_MISSING",
    "KYTY_GFX_PERMISSIVE",
)

DIAGNOSTIC_ENV = (
    "KYTY_FAULT_LOG",
    "KYTY_TLS_DIAG",
    "KYTY_AUTO_CROSS",
    "KYTY_SKIP_UD2",
    "KYTY_BRINGUP_MODE",
)


@dataclass
class Check:
    name: str
    ok: bool
    detail: str = ""


@dataclass
class DoctorReport:
    repo_root: str
    base_sha: str
    host_os: str
    checks: list[Check] = field(default_factory=list)
    next_commands: list[str] = field(default_factory=list)
    docs: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    @property
    def healthy(self) -> bool:
        critical = ("skill_in_repo", "frontier_manifest", "cursor_rule")
        return all(c.ok for c in self.checks if c.name in critical)


def find_repo_root(start: Path | None = None) -> Path:
    here = (start or Path(__file__).resolve()).parent
    for base in [here, *here.parents]:
        if (base / "AGENTS.md").is_file() and (base / "source" / "CMakeLists.txt").is_file():
            return base
    raise SystemExit("kyty_agent_doctor: cannot find Kyty repo root (AGENTS.md + source/)")


def git_sha(repo: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(repo), "rev-parse", "--short", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def build_dir(repo: Path) -> Path:
    if platform.system() == "Darwin":
        return repo / "_build_macos"
    return repo / "_build_linux"


def run_doctor(repo: Path, *, task: str, try_socket: bool) -> DoctorReport:
    report = DoctorReport(
        repo_root=str(repo),
        base_sha=git_sha(repo),
        host_os=platform.system().lower(),
    )
    report.docs = [
        "AGENTS.md",
        "skills/kyty-frontier-swarm/SKILL.md",
        "skills/kyty-frontier-swarm/references/quick-recipes.md",
        "docs/agent-tools.md",
        "INSTALL-AGENTS.md",
    ]

    skill = repo / "skills" / "kyty-frontier-swarm" / "SKILL.md"
    report.checks.append(Check("skill_in_repo", skill.is_file(), str(skill.relative_to(repo))))

    cursor_skill = Path.home() / ".cursor" / "skills" / "kyty-frontier-swarm" / "SKILL.md"
    if cursor_skill.is_file():
        report.checks.append(Check("skill_installed_cursor", True, str(cursor_skill)))
    else:
        report.checks.append(
            Check(
                "skill_installed_cursor",
                False,
                "run: python3 scripts/install_agent_toolkit.py --agent cursor",
            )
        )

    cursor_rule = repo / ".cursor" / "rules" / "kyty-agent-toolkit.mdc"
    report.checks.append(Check("cursor_rule", cursor_rule.is_file(), str(cursor_rule.relative_to(repo))))

    bdir = build_dir(repo)
    fc = bdir / "fc_script"
    agent = bdir / "agent" / "kyty_agent"
    report.checks.append(Check("fc_script", fc.is_file(), str(fc)))
    report.checks.append(Check("kyty_agent", agent.is_file(), str(agent)))

    manifest_py = repo / "skills" / "kyty-frontier-swarm" / "scripts" / "frontier_manifest.py"
    report.checks.append(Check("frontier_manifest", manifest_py.is_file(), str(manifest_py)))

    guest = os.environ.get("KYTY_GUEST_ROOT", "")
    games = os.environ.get("KYTY_GAMES_ROOT", "")
    if guest:
        ok = Path(guest).expanduser().is_dir()
        report.checks.append(Check("guest_root", ok, "KYTY_GUEST_ROOT set"))
    else:
        report.checks.append(Check("guest_root", False, "unset (OK for code-only tasks)"))

    if games:
        ok = Path(games).expanduser().is_dir()
        report.checks.append(Check("games_root", ok, "KYTY_GAMES_ROOT set"))
    else:
        report.checks.append(Check("games_root", False, "unset (needed for matrix)"))

    forbidden_set = [k for k in FORBIDDEN_ENV if os.environ.get(k)]
    report.checks.append(
        Check(
            "strict_env",
            len(forbidden_set) == 0,
            "clear: " + ", ".join(forbidden_set) if forbidden_set else "no bring-up flags",
        )
    )
    diag_set = [k for k in DIAGNOSTIC_ENV if os.environ.get(k)]
    if diag_set:
        report.warnings.append(f"diagnostic env active (not acceptance): {', '.join(diag_set)}")

    sock = os.environ.get("KYTY_AGENT_SOCK", "")
    if try_socket and sock and agent.is_file():
        try:
            out = subprocess.check_output(
                [str(agent), "--sock", sock, "doctor"],
                text=True,
                stderr=subprocess.STDOUT,
                timeout=5,
            )
            ok = "healthy" in out.lower() or '"ok"' in out.lower() or "pong" in out.lower()
            report.checks.append(Check("kyty_agent_socket", ok, sock))
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            report.checks.append(Check("kyty_agent_socket", False, f"{sock}: {exc}"))
    elif sock:
        report.checks.append(Check("kyty_agent_socket", False, "kyty_agent binary missing"))
    else:
        report.checks.append(Check("kyty_agent_socket", False, "KYTY_AGENT_SOCK unset"))

    # Next commands by task
    py = sys.executable
    manifest_default = os.environ.get("KYTY_FRONTIER_MANIFEST", "/tmp/kyty-frontier.json")
    report.next_commands.append(f"{py} {repo / 'scripts' / 'verify_agent_toolkit.py'}")

    if task in ("", "start"):
        report.next_commands.append(
            f"{py} {manifest_py} new --output {manifest_default} --base-sha $(git rev-parse HEAD) --force"
            if not Path(manifest_default).is_file()
            else f"{py} {manifest_py} summary {manifest_default}"
        )

    if task in ("", "start", "code"):
        report.next_commands.append(f"ninja -C {bdir} fc_script")

    if task in ("guest-crash", "hle", "frontier") and guest:
        report.next_commands.append(
            f"export KYTY_FAULT_LOG=1 KYTY_AGENT_SOCK=/tmp/kyty-agent.sock "
            f"KYTY_NATIVE_CAPTURE_DIR=/tmp/kyty-captures"
        )
        report.next_commands.append(f"{fc} {repo / 'scripts' / 'run_guest.lua'} \"$KYTY_GUEST_ROOT\"")
        report.next_commands.append(f"{agent} --sock \"$KYTY_AGENT_SOCK\" wait-ready --timeout-ms 60000")
        report.next_commands.append(
            f"{py} {manifest_py} import-log {manifest_default} --log /path/to/child.log --id frontier-1"
        )

    if task in ("matrix", "") and games:
        report.next_commands.append(
            f"KYTY_GAMES_ROOT=\"$KYTY_GAMES_ROOT\" KYTY_MATRIX_SCRATCH={repo / '_matrix_scratch'} "
            f"{py} {repo / 'scripts' / 'kyty_games_matrix.py'} --discover-only"
        )

    if not cursor_skill.is_file():
        report.next_commands.append(f"{py} {repo / 'scripts' / 'install_agent_toolkit.py'} --agent cursor")

    return report


def print_human(report: DoctorReport) -> None:
    print(f"kyty_agent_doctor  repo={report.repo_root}  sha={report.base_sha}  os={report.host_os}")
    print()
    for c in report.checks:
        mark = "OK" if c.ok else "!!"
        print(f"  [{mark}] {c.name}: {c.detail}")
    if report.warnings:
        print()
        for w in report.warnings:
            print(f"  warn: {w}")
    print()
    print("Read first:")
    for d in report.docs[:4]:
        print(f"  - {d}")
    print()
    print("Suggested commands:")
    for cmd in report.next_commands:
        print(f"  {cmd}")
    print()
    if report.healthy:
        print("toolkit: ready (optional checks may show !!)")
    else:
        print("toolkit: fix critical checks before continuing")


def main() -> int:
    p = argparse.ArgumentParser(description="Kyty agent toolkit doctor")
    p.add_argument("--repo", default="", help="Kyty repo root (auto-detect)")
    p.add_argument("--json", action="store_true", help="machine-readable report")
    p.add_argument(
        "--task",
        choices=["", "start", "code", "guest-crash", "hle", "frontier", "matrix"],
        default="",
        help="bias next-command suggestions",
    )
    p.add_argument("--try-socket", action="store_true", help="ping KYTY_AGENT_SOCK if set")
    args = p.parse_args()

    repo = Path(args.repo).resolve() if args.repo else find_repo_root()
    report = run_doctor(repo, task=args.task, try_socket=args.try_socket)

    if args.json:
        print(json.dumps(asdict(report), indent=2))
    else:
        print_human(report)
    return 0 if report.healthy else 1


if __name__ == "__main__":
    raise SystemExit(main())
