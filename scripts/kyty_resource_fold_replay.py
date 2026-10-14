#!/usr/bin/env python3
"""Wrapper for the offline resource-fold replay CLI.

Both subcommands delegate to the built C++ tool, which owns the document schema,
its bounds and the production evaluator. Nothing is validated or evaluated here:
without a usable binary the wrapper reports the CLI as unavailable instead of
returning a partial result.

  validate <document> --binary <tool>    schema and safe-input checks (no replay)
  replay <document> --binary <tool>      replay and exact output comparison

The binary may also come from KYTY_RESOURCE_FOLD_REPLAY_BIN. Exit codes are the
CLI's own (0 ok, 1 usage, 2 malformed, 3 unused reads, 4 output mismatch,
5 runtime initialization), plus 6 when the CLI is unavailable.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

EXIT_UNAVAILABLE = 6
BINARY_ENV = "KYTY_RESOURCE_FOLD_REPLAY_BIN"


def resolve_binary(path: str | None) -> str | None:
    if not path or not os.path.isfile(path) or not os.access(path, os.X_OK):
        return None
    return path


def run(binary: str, document: str, option: str | None) -> int:
    command = [binary, document]
    if option is not None:
        command.append(option)
    return subprocess.call(command)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("validate", "replay"):
        command = sub.add_parser(name)
        command.add_argument("document")
        command.add_argument("--binary", default=os.environ.get(BINARY_ENV), help="built kyty_resource_fold_replay executable")
        if name == "replay":
            command.add_argument("--print-output", action="store_true")
    args = parser.parse_args(argv)

    binary = resolve_binary(args.binary)
    if binary is None:
        print("resource-fold-replay: CLI unavailable; pass --binary or set " + BINARY_ENV, file=sys.stderr)
        return EXIT_UNAVAILABLE
    if args.command == "validate":
        return run(binary, args.document, "--validate-only")
    return run(binary, args.document, "--print-output" if args.print_output else None)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
