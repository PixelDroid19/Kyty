#!/usr/bin/env python3
"""Build a static import backlog from a Kyty symbol dump.

The report is inventory only. It compares guest imports against the HLE symbol
database and may annotate NIDs with names from a caller-provided public catalog,
but it never registers HLE handlers or changes runtime behavior.
"""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import re
from collections import Counter
from pathlib import Path


NID_SUFFIX = bytes(
    [
        0x51,
        0x8D,
        0x64,
        0xA6,
        0x35,
        0xDE,
        0xD8,
        0xC1,
        0xE6,
        0xB0,
        0x39,
        0xB1,
        0xC3,
        0xE5,
        0x52,
        0x30,
    ]
)
IDENTITY_RE = re.compile(r"^(?P<nid>[^\[]+)\[(?P<library>[^\]]+)\]\[(?P<module>[^\]]+)\]\[(?P<type>[^\]]+)\]$")


def nid_for_name(name: str) -> str:
    digest = hashlib.sha1(name.encode("utf-8") + NID_SUFFIX).digest()
    return base64.b64encode(digest[:8][::-1]).decode("ascii").rstrip("=").replace("/", "-")


def read_symbol_identities(path: Path) -> set[str]:
    identities: set[str] = set()
    if not path.exists():
        return identities
    for line in path.read_text(errors="replace").splitlines():
        fields = line.split(maxsplit=1)
        if len(fields) == 2:
            identities.add(fields[1])
    return identities


def load_name_catalog(path: Path | None) -> dict[str, list[str]]:
    if path is None:
        return {}
    by_nid: dict[str, list[str]] = {}
    for raw in path.read_text(errors="replace").splitlines():
        name = raw.strip()
        if not name:
            continue
        by_nid.setdefault(nid_for_name(name), []).append(name)
    return by_nid


def row_for(identity: str, resolved: bool, names_by_nid: dict[str, list[str]]) -> dict[str, str | int]:
    match = IDENTITY_RE.match(identity)
    if match is None:
        return {
            "status": "resolved" if resolved else "missing",
            "identity": identity,
            "nid": "",
            "library": "",
            "module": "",
            "type": "",
            "decoded_names": "",
            "decoded_count": 0,
        }
    nid = match.group("nid")
    names = names_by_nid.get(nid, [])
    return {
        "status": "resolved" if resolved else "missing",
        "identity": identity,
        "nid": nid,
        "library": match.group("library"),
        "module": match.group("module"),
        "type": match.group("type"),
        "decoded_names": ";".join(names[:8]),
        "decoded_count": len(names),
    }


def write_csv(path: Path, rows: list[dict[str, str | int]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as out:
        writer = csv.DictWriter(
            out,
            fieldnames=["status", "identity", "nid", "library", "module", "type", "decoded_names", "decoded_count"],
        )
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare guest imports against Kyty HLE symbols")
    parser.add_argument("dump_dir", type=Path, help="Directory produced by scripts/dump_guest_symbols.lua")
    parser.add_argument("--names", type=Path, default=None, help="Optional ps5_names-style catalog")
    parser.add_argument("--out", type=Path, default=None, help="CSV output path")
    parser.add_argument("--include-resolved", action="store_true", help="Include resolved imports in the CSV")
    args = parser.parse_args()

    dump_dir: Path = args.dump_dir
    hle = read_symbol_identities(dump_dir / "hle_symbols.txt")
    imports: set[str] = set()
    for path in dump_dir.glob("*/import_symbols.txt"):
        imports.update(read_symbol_identities(path))

    names_by_nid = load_name_catalog(args.names)
    missing = sorted(imports - hle)
    resolved = sorted(imports & hle)
    rows = [row_for(identity, False, names_by_nid) for identity in missing]
    if args.include_resolved:
        rows.extend(row_for(identity, True, names_by_nid) for identity in resolved)

    out = args.out if args.out is not None else dump_dir / "static_nid_backlog.csv"
    write_csv(out, rows)

    by_library = Counter(row["library"] for row in rows if row["status"] == "missing")
    decoded_missing = sum(1 for row in rows if row["status"] == "missing" and int(row["decoded_count"]) > 0)
    print(f"imports={len(imports)} resolved={len(resolved)} missing={len(missing)}")
    print(f"decoded_missing={decoded_missing} output={out}")
    for library, count in by_library.most_common():
        print(f"missing_by_library {library}={count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
