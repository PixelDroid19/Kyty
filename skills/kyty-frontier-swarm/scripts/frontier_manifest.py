#!/usr/bin/env python3
"""Frontier work manifest: create, validate, summarize, import, and export blockers."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1
VALID_STATUSES = frozenset(
    {
        "observed",
        "hypothesized",
        "contract-reviewed",
        "implementing",
        "implemented",
        "branch-verified",
        "integrated",
        "strict-verified",
        "blocked",
    }
)
VALID_CLASSES = frozenset(
    {"hle", "kernel", "loader", "pm4", "layout", "shader", "videoout", "host", "other", "auto"}
)
ORDERED_STATUSES = [
    "observed",
    "hypothesized",
    "contract-reviewed",
    "implementing",
    "implemented",
    "branch-verified",
    "integrated",
    "strict-verified",
]

_LOG_PATTERNS: list[tuple[str, str, str]] = [
    (r"FAULTR\b", "host", "access_violation_read"),
    (r"FATAL-ACCESS-VIOLATION", "host", "access_violation"),
    (r"EXIT_NOT_IMPLEMENTED", "other", "not_implemented"),
    (r"CALLED missing stub", "hle", "missing_stub"),
    (r"stack fail!!!", "host", "stack_canary"),
    (r"KYTY_BRINGUP_CONTINUE.*\[Func\]", "hle", "bringup_continue_func"),
    (r"sceKernelReserveVirtualRange", "kernel", "reserve_virtual"),
    (r"GraphicsRun\.cpp", "pm4", "graphics_run"),
    (r"GraphicsState\.cpp", "pm4", "graphics_state"),
    (r"ShaderSpirv\.cpp", "shader", "shader_spirv"),
]


def _die(msg: str) -> None:
    print(f"frontier_manifest: {msg}", file=sys.stderr)
    sys.exit(1)


def _load(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        _die(f"cannot read {path}: {exc}")
    if not isinstance(data, dict):
        _die("root must be an object")
    return data


def _save(path: Path, data: dict[str, Any]) -> None:
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _find_item(items: list[dict[str, Any]], item_id: str) -> dict[str, Any]:
    for it in items:
        if it.get("id") == item_id:
            return it
    _die(f"unknown id: {item_id}")
    return {}


def _slug(text: str, max_len: int = 48) -> str:
    s = re.sub(r"[^a-zA-Z0-9]+", "-", text.strip().lower()).strip("-")
    return (s[:max_len] or "item")


def classify_log_line(line: str) -> tuple[str, str]:
    for pattern, klass, label in _LOG_PATTERNS:
        if re.search(pattern, line):
            return klass, label
    return "other", "runtime"


def extract_frontier_lines(log_text: str, *, tail: int = 40) -> list[str]:
    hits: list[str] = []
    for line in log_text.splitlines():
        if any(re.search(p, line) for p, _, _ in _LOG_PATTERNS):
            hits.append(line.strip())
    if not hits:
        return []
    return hits[-tail:]


def cmd_new(args: argparse.Namespace) -> None:
    out = Path(args.output)
    if out.exists() and not args.force:
        _die(f"{out} exists (use --force)")
    data = {
        "schema_version": SCHEMA_VERSION,
        "run": {
            "created_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "base_sha": args.base_sha or "",
            "host": args.host or "",
            "notes": args.notes or "",
        },
        "items": [],
    }
    _save(out, data)
    print(out)


def cmd_add_blocker(args: argparse.Namespace) -> None:
    path = Path(args.manifest)
    data = _load(path)
    if data.get("schema_version") != SCHEMA_VERSION:
        _die("unsupported schema_version")
    status = args.status or "observed"
    if status not in VALID_STATUSES:
        _die(f"invalid status: {status}")
    if args.klass not in VALID_CLASSES:
        _die(f"invalid class: {args.klass}")
    klass = args.klass
    if klass == "auto":
        klass = "other"
    items: list[dict[str, Any]] = data.setdefault("items", [])
    for it in items:
        if it.get("id") == args.id:
            _die(f"duplicate id: {args.id}")
    items.append(
        {
            "id": args.id,
            "class": klass,
            "nid": args.nid,
            "symbol": args.symbol,
            "status": status,
            "priority": args.priority,
            "observations": [args.evidence] if args.evidence else [],
            "hypothesis": args.hypothesis,
            "falsifier": args.falsifier,
            "contract": {},
            "implementation": {},
            "validation": {},
            "blockers": [],
        }
    )
    _save(path, data)
    print(args.id)


def _status_index(status: str) -> int:
    if status == "blocked":
        return -1
    try:
        return ORDERED_STATUSES.index(status)
    except ValueError:
        return -2


def cmd_validate(args: argparse.Namespace) -> None:
    path = Path(args.manifest)
    data = _load(path)
    if data.get("schema_version") != SCHEMA_VERSION:
        _die("unsupported schema_version")
    run = data.get("run")
    if not isinstance(run, dict):
        _die("run must be an object")
    items = data.get("items")
    if not isinstance(items, list):
        _die("items must be an array")
    errors: list[str] = []
    seen: set[str] = set()
    for i, it in enumerate(items):
        if not isinstance(it, dict):
            errors.append(f"items[{i}] not object")
            continue
        iid = it.get("id")
        if not iid or not isinstance(iid, str):
            errors.append(f"items[{i}] missing id")
            continue
        if iid in seen:
            errors.append(f"duplicate id: {iid}")
        seen.add(iid)
        st = it.get("status")
        if st not in VALID_STATUSES:
            errors.append(f"{iid}: invalid status {st!r}")
        cls = it.get("class")
        if cls not in VALID_CLASSES:
            errors.append(f"{iid}: invalid class {cls!r}")
        obs = it.get("observations", [])
        if st == "observed" and not obs:
            errors.append(f"{iid}: observed requires observations")
        if st in ("hypothesized", "contract-reviewed", "implementing") and not it.get("hypothesis"):
            errors.append(f"{iid}: {st} requires hypothesis")
        if st == "strict-verified":
            val = it.get("validation")
            if not isinstance(val, dict) or not val.get("strict_command"):
                errors.append(f"{iid}: strict-verified requires validation.strict_command")
        impl = it.get("implementation") or {}
        if st in ("implemented", "branch-verified", "integrated") and not impl.get("files"):
            errors.append(f"{iid}: {st} requires implementation.files")
        # monotonic promotion (blocked exempt)
        if st != "blocked" and _status_index(st) < 0:
            errors.append(f"{iid}: unknown status ordering for {st}")
    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"OK ({len(items)} items)")


def cmd_summary(args: argparse.Namespace) -> None:
    path = Path(args.manifest)
    data = _load(path)
    items = data.get("items") or []
    by_status: dict[str, int] = {}
    for it in items:
        st = it.get("status", "?")
        by_status[st] = by_status.get(st, 0) + 1
    print(f"base_sha={data.get('run', {}).get('base_sha', '')}")
    print(f"items={len(items)}")
    for st in ORDERED_STATUSES + ["blocked"]:
        if st in by_status:
            print(f"  {st}: {by_status[st]}")
    for it in items:
        if it.get("status") in ("observed", "blocked", "hypothesized"):
            obs = (it.get("observations") or [""])[0]
            print(f"- [{it.get('status')}] {it.get('id')} ({it.get('class')}): {obs[:120]}")


def cmd_import_log(args: argparse.Namespace) -> None:
    path = Path(args.manifest)
    log_path = Path(args.log)
    if not log_path.is_file():
        _die(f"log not found: {log_path}")
    text = log_path.read_text(encoding="utf-8", errors="replace")
    lines = extract_frontier_lines(text)
    if not lines:
        _die("no frontier patterns found in log")
    primary = lines[-1]
    klass, label = classify_log_line(primary)
    if args.klass and args.klass != "auto":
        klass = args.klass
    item_id = args.id or _slug(f"{label}-{hashlib.sha256(primary.encode()).hexdigest()[:8]}")
    data = _load(path)
    items: list[dict[str, Any]] = data.setdefault("items", [])
    for it in items:
        if it.get("id") == item_id:
            obs: list[str] = it.setdefault("observations", [])
            for line in lines[-5:]:
                if line not in obs:
                    obs.append(line)
            _save(path, data)
            print(item_id)
            return
    items.append(
        {
            "id": item_id,
            "class": klass,
            "nid": args.nid,
            "symbol": args.symbol,
            "status": "observed",
            "priority": args.priority,
            "observations": lines[-8:],
            "hypothesis": None,
            "falsifier": None,
            "contract": {},
            "implementation": {},
            "validation": {"log": str(log_path.name)},
            "blockers": [],
        }
    )
    _save(path, data)
    print(item_id)


def cmd_from_matrix(args: argparse.Namespace) -> None:
    path = Path(args.manifest)
    summary_path = Path(args.summary)
    if not summary_path.is_file():
        _die(f"summary not found: {summary_path}")
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    cases = summary.get("all_cases") or summary.get("failed_before_runtime") or []
    if not isinstance(cases, list):
        _die("summary has no case list")
    data = _load(path)
    items: list[dict[str, Any]] = data.setdefault("items", [])
    existing = {it.get("id") for it in items}
    added = 0
    for case in cases:
        if not isinstance(case, dict):
            continue
        if args.only_failed and case.get("progressed"):
            continue
        case_id = case.get("case_id") or "case_unknown"
        frontier = case.get("first_frontier") or case.get("outcome") or "unknown"
        item_id = _slug(f"{case_id}-{frontier}")
        if item_id in existing:
            continue
        evidence = (
            f"outcome={case.get('outcome')} first_frontier={frontier} "
            f"exit={case.get('child_exit')} present={case.get('present')}"
        )
        klass = "host" if frontier in ("host_crash", "access_violation") else "other"
        items.append(
            {
                "id": item_id,
                "class": klass,
                "nid": None,
                "symbol": None,
                "status": "observed",
                "priority": 0,
                "observations": [evidence],
                "hypothesis": None,
                "falsifier": None,
                "contract": {"case_id": case_id},
                "implementation": {},
                "validation": {"matrix_summary": summary_path.name},
                "blockers": [],
            }
        )
        existing.add(item_id)
        added += 1
    _save(path, data)
    print(f"added={added}")


def cmd_set_field(args: argparse.Namespace) -> None:
    path = Path(args.manifest)
    data = _load(path)
    items: list[dict[str, Any]] = data.setdefault("items", [])
    it = _find_item(items, args.id)
    if args.status:
        if args.status not in VALID_STATUSES:
            _die(f"invalid status: {args.status}")
        it["status"] = args.status
    if args.klass:
        if args.klass not in VALID_CLASSES - {"auto"}:
            _die(f"invalid class: {args.klass}")
        it["class"] = args.klass
    if args.hypothesis is not None:
        it["hypothesis"] = args.hypothesis
    if args.falsifier is not None:
        it["falsifier"] = args.falsifier
    if args.evidence:
        obs: list[str] = it.setdefault("observations", [])
        if args.evidence not in obs:
            obs.append(args.evidence)
    if args.blocker:
        blockers: list[str] = it.setdefault("blockers", [])
        if args.blocker not in blockers:
            blockers.append(args.blocker)
    _save(path, data)
    print(args.id)


def cmd_record_validation(args: argparse.Namespace) -> None:
    path = Path(args.manifest)
    data = _load(path)
    items: list[dict[str, Any]] = data.setdefault("items", [])
    it = _find_item(items, args.id)
    val = it.setdefault("validation", {})
    if args.strict_command:
        val["strict_command"] = args.strict_command
    if args.exit_code is not None:
        val["exit_code"] = args.exit_code
    if args.notes:
        val["notes"] = args.notes
    if args.exit_code == 0 and args.promote:
        it["status"] = "strict-verified"
    _save(path, data)
    print(args.id)


def cmd_export_report(args: argparse.Namespace) -> None:
    path = Path(args.manifest)
    data = _load(path)
    run = data.get("run") or {}
    items = data.get("items") or []
    print(f"# Kyty frontier report")
    print(f"- base_sha: {run.get('base_sha', '')}")
    print(f"- created: {run.get('created_at', '')}")
    print(f"- items: {len(items)}")
    print()
    shown = 0
    for it in items:
        if shown >= args.max_items:
            break
        st = it.get("status")
        if args.active_only and st in ("strict-verified", "integrated"):
            continue
        print(f"## {it.get('id')} ({st}, {it.get('class')})")
        for obs in (it.get("observations") or [])[:3]:
            print(f"- evidence: {obs}")
        if it.get("hypothesis"):
            print(f"- hypothesis: {it.get('hypothesis')}")
        if it.get("falsifier"):
            print(f"- falsifier: {it.get('falsifier')}")
        if it.get("blockers"):
            print(f"- blockers: {', '.join(it.get('blockers'))}")
        val = it.get("validation") or {}
        if val.get("strict_command"):
            print(f"- strict: `{val.get('strict_command')}` exit={val.get('exit_code')}")
        print()
        shown += 1


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Kyty frontier manifest utility")
    sub = p.add_subparsers(dest="cmd", required=True)

    n = sub.add_parser("new", help="create empty manifest")
    n.add_argument("--output", required=True)
    n.add_argument("--base-sha", default="")
    n.add_argument("--host", default="")
    n.add_argument("--notes", default="")
    n.add_argument("--force", action="store_true")
    n.set_defaults(func=cmd_new)

    a = sub.add_parser("add-blocker", help="append blocker item")
    a.add_argument("manifest")
    a.add_argument("--id", required=True)
    a.add_argument("--class", dest="klass", default="other")
    a.add_argument("--evidence", default="")
    a.add_argument("--status", default="observed")
    a.add_argument("--nid", default=None)
    a.add_argument("--symbol", default=None)
    a.add_argument("--priority", type=int, default=0)
    a.add_argument("--hypothesis", default=None)
    a.add_argument("--falsifier", default=None)
    a.set_defaults(func=cmd_add_blocker)

    i = sub.add_parser("import-log", help="add/update item from child.log patterns")
    i.add_argument("manifest")
    i.add_argument("--log", required=True)
    i.add_argument("--id", default="")
    i.add_argument("--class", dest="klass", default="auto")
    i.add_argument("--nid", default=None)
    i.add_argument("--symbol", default=None)
    i.add_argument("--priority", type=int, default=0)
    i.set_defaults(func=cmd_import_log)

    m = sub.add_parser("from-matrix", help="import cases from matrix summary.json")
    m.add_argument("manifest")
    m.add_argument("--summary", required=True)
    m.add_argument("--only-failed", action="store_true", default=True)
    m.add_argument("--all-cases", dest="only_failed", action="store_false")
    m.set_defaults(func=cmd_from_matrix)

    f = sub.add_parser("set-field", help="update item fields")
    f.add_argument("manifest")
    f.add_argument("--id", required=True)
    f.add_argument("--status", default="")
    f.add_argument("--class", dest="klass", default="")
    f.add_argument("--hypothesis", default=None)
    f.add_argument("--falsifier", default=None)
    f.add_argument("--evidence", default="")
    f.add_argument("--blocker", default="")
    f.set_defaults(func=cmd_set_field)

    r = sub.add_parser("record-validation", help="attach strict run result")
    r.add_argument("manifest")
    r.add_argument("--id", required=True)
    r.add_argument("--strict-command", default="")
    r.add_argument("--exit-code", type=int, default=None)
    r.add_argument("--notes", default="")
    r.add_argument("--promote", action="store_true", help="set strict-verified when exit 0")
    r.set_defaults(func=cmd_record_validation)

    e = sub.add_parser("export-report", help="markdown handoff for chat")
    e.add_argument("manifest")
    e.add_argument("--max-items", type=int, default=12)
    e.add_argument("--active-only", action="store_true", default=True)
    e.add_argument("--all", dest="active_only", action="store_false")
    e.set_defaults(func=cmd_export_report)

    v = sub.add_parser("validate", help="validate manifest invariants")
    v.add_argument("manifest")
    v.set_defaults(func=cmd_validate)

    s = sub.add_parser("summary", help="print short summary")
    s.add_argument("manifest")
    s.set_defaults(func=cmd_summary)

    args = p.parse_args(argv)
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
