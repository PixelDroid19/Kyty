#!/usr/bin/env python3
"""
Install Kyty agent skill into personal agent config directories.

Safe by default: dry-run shows actions; refuses to overwrite differing files
unless --force.
"""

from __future__ import annotations

import argparse
import filecmp
import os
import shutil
import sys
from pathlib import Path

SKILL_NAME = "kyty-frontier-swarm"


def repo_root_from_script() -> Path:
    # scripts/install_agent_toolkit.py -> repo root
    return Path(__file__).resolve().parent.parent


def skill_source(root: Path) -> Path:
    return root / "skills" / SKILL_NAME


def destinations(agent: str, root: Path) -> list[Path]:
    home = Path.home()
    src = skill_source(root)
    if agent == "cursor":
        return [home / ".cursor" / "skills" / SKILL_NAME]
    if agent == "codex":
        codex_home = Path(os.environ.get("CODEX_HOME", home / ".codex"))
        return [codex_home / "skills" / SKILL_NAME]
    if agent == "claude":
        claude_dir = Path(os.environ.get("CLAUDE_CONFIG_DIR", home / ".claude"))
        return [claude_dir / "skills" / SKILL_NAME]
    if agent == "all":
        return destinations("cursor", root) + destinations("codex", root) + destinations("claude", root)
    raise ValueError(agent)


def install_tree(src: Path, dst: Path, *, dry_run: bool, force: bool) -> list[str]:
    actions: list[str] = []
    if not src.is_dir():
        raise FileNotFoundError(f"missing skill source: {src}")
    if dst.exists():
        if dst.is_symlink() or not dst.is_dir():
            actions.append(f"would remove {dst}")
            if not dry_run and force:
                dst.unlink() if dst.is_symlink() else shutil.rmtree(dst)
        elif not force:
            # compare trees
            diff = not filecmp.dircmp(src, dst).same_files
            if diff:
                actions.append(f"REFUSE overwrite differing {dst} (use --force)")
                return actions
            actions.append(f"skip identical {dst}")
            return actions
    actions.append(f"copy {src} -> {dst}")
    if not dry_run:
        if dst.exists() and force:
            shutil.rmtree(dst)
        shutil.copytree(src, dst)
    return actions


def main() -> int:
    p = argparse.ArgumentParser(description="Install Kyty frontier swarm skill")
    p.add_argument("--agent", choices=["cursor", "codex", "claude", "all"], default="cursor")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--force", action="store_true")
    args = p.parse_args()

    root = repo_root_from_script()
    src = skill_source(root)
    if not src.is_dir():
        print(f"error: {src} not found", file=sys.stderr)
        return 1

    refused = False
    for dst in destinations(args.agent, root):
        for line in install_tree(src, dst, dry_run=args.dry_run, force=args.force):
            print(line)
            if line.startswith("REFUSE"):
                refused = True
    if refused:
        return 2
    if args.dry_run:
        print("dry-run complete")
    else:
        print("install complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
