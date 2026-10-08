#!/usr/bin/env python3
"""Tests for the resource-fold replay wrapper and, when available, the CLI.

CLI cases run only when KYTY_RESOURCE_FOLD_REPLAY_BIN names the built tool; they
are skipped with an explicit reason otherwise. The expected canonical output is
always produced by the C++ evaluator itself, never written here.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import kyty_resource_fold_replay as wrapper

BINARY = os.environ.get(wrapper.BINARY_ENV)
CLI_AVAILABLE = wrapper.resolve_binary(BINARY) is not None


def fnv1a64(text: str) -> int:
    value = 14695981039346656037
    for byte in text.encode("ascii"):
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


CANONICAL_BEGIN = b"resource-fold-canonical-begin bytes="
CANONICAL_END = b"resource-fold-canonical-end\n"


def extract_canonical(stdout: bytes) -> str:
    """Take exactly the framed payload bytes; fail unless the frame is complete."""
    begin = stdout.rfind(CANONICAL_BEGIN)
    if begin < 0:
        raise AssertionError("canonical frame begin line missing")
    header_end = stdout.index(b"\n", begin)
    size = int(stdout[begin + len(CANONICAL_BEGIN) : header_end])
    payload_start = header_end + 1
    payload_end = payload_start + size
    if stdout[payload_end : payload_end + len(CANONICAL_END)] != CANONICAL_END:
        raise AssertionError("canonical frame end line missing after the declared payload")
    return stdout[payload_start:payload_end].decode("ascii")


def minimal_document(canonical: str, reads: list[dict] | None = None) -> str:
    """A PS5 document without code or metadata; keys follow the fixed schema order."""
    parse = {
        "guest_platform": 2,
        "user_sgpr_num": 0,
        "user_data_register_base": 0,
        "vertex_resource_types": 0,
        "initial_output": {
            "push_constant_offset": 0,
            "push_constant_size": 0,
            "descriptor_set_slot": 0,
            "vertex_attrib": 0,
            "vertex_attrib_reg": 0,
        },
        "user_sgpr": {"count": 0, "value": [0] * 32, "type": [0] * 32},
        "user_data": {
            "eud_size_dw": 0,
            "srt_size_dw": 0,
            "direct_count": 0,
            "direct_present": 0,
            "direct": [],
            "sharp": [{"count": 0, "present": 0, "entries": []} for _ in range(4)],
        },
        "code_present": 0,
    }
    fingerprint = fnv1a64(canonical)
    document = {
        "schema_version": 3,
        "parse": parse,
        "transcript": {"reads": reads or []},
        "canonical_output": canonical,
        "fingerprint_hi": fingerprint >> 32,
        "fingerprint_lo": fingerprint & 0xFFFFFFFF,
    }
    return json.dumps(document, separators=(",", ":"))


class CanonicalFrameTests(unittest.TestCase):
    def test_takes_exactly_the_framed_payload_after_startup_output(self) -> None:
        payload = b"usage.fetch=0\nbind.push_constant_size=0\n"
        stdout = (
            b"startup diagnostic line\n"
            + CANONICAL_BEGIN
            + str(len(payload)).encode()
            + b"\n"
            + payload
            + CANONICAL_END
            + b"fingerprint=0 output_matches_document=0\n"
        )
        self.assertEqual(extract_canonical(stdout), payload.decode())

    def test_rejects_a_truncated_frame(self) -> None:
        stdout = CANONICAL_BEGIN + b"64\nusage.fetch=0\n" + CANONICAL_END
        with self.assertRaises(AssertionError):
            extract_canonical(stdout)


class WrapperTests(unittest.TestCase):
    def test_reports_unavailable_cli_instead_of_validating(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(wrapper.main(["validate", "document.json"]), wrapper.EXIT_UNAVAILABLE)

    def test_rejects_non_executable_binary(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            fake = Path(scratch) / "tool"
            fake.write_text("not a program")
            fake.chmod(0o600)
            self.assertEqual(wrapper.main(["replay", "document.json", "--binary", str(fake)]), wrapper.EXIT_UNAVAILABLE)

    def test_routes_options_to_the_cli(self) -> None:
        with mock.patch.object(wrapper, "resolve_binary", return_value="/tool"), mock.patch.object(
            wrapper.subprocess, "call", return_value=4
        ) as call:
            self.assertEqual(wrapper.main(["validate", "doc.json", "--binary", "/tool"]), 4)
            call.assert_called_with(["/tool", "doc.json", "--validate-only"])
            wrapper.main(["replay", "doc.json", "--binary", "/tool", "--print-output"])
            call.assert_called_with(["/tool", "doc.json", "--print-output"])


@unittest.skipUnless(CLI_AVAILABLE, "kyty_resource_fold_replay CLI unavailable (set " + wrapper.BINARY_ENV + ")")
class CliTests(unittest.TestCase):
    def setUp(self) -> None:
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)

    def write(self, name: str, text: str) -> str:
        path = Path(self.scratch.name) / name
        path.write_text(text, encoding="ascii")
        return str(path)

    def cli(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([BINARY, *args], capture_output=True, text=True, check=False)

    def evaluator_output(self) -> str:
        """The CLI's length-framed canonical payload; startup diagnostics before it are ignored."""
        path = self.write("probe.json", minimal_document(""))
        result = subprocess.run([BINARY, path, "--print-output"], capture_output=True, check=False)
        self.assertEqual(result.returncode, 4, result.stderr)
        return extract_canonical(result.stdout)

    def test_round_trip_matches_evaluator_output_exactly(self) -> None:
        document = self.write("doc.json", minimal_document(self.evaluator_output()))
        self.assertEqual(self.cli(document, "--validate-only").returncode, 0)
        self.assertEqual(self.cli(document).returncode, 0)
        self.assertEqual(wrapper.main(["replay", document, "--binary", BINARY]), 0)

    def test_hostile_documents_are_malformed(self) -> None:
        valid = minimal_document(self.evaluator_output())
        cases = {
            "trailing": valid + "{}",
            "fractional": valid.replace('"schema_version":3', '"schema_version":3.0', 1),
            "exponent": valid.replace('"schema_version":3', '"schema_version":3e0', 1),
            "duplicate": valid.replace('{"schema_version":3,', '{"schema_version":3,"schema_version":3,', 1),
            "previous_schema": valid.replace('"schema_version":3', '"schema_version":2', 1),
            "missing_initial_output": valid.replace(
                '"initial_output":{"push_constant_offset":0,"push_constant_size":0,"descriptor_set_slot":0,'
                '"vertex_attrib":0,"vertex_attrib_reg":0},',
                "",
                1,
            ),
            "unknown_key": valid.replace('"parse":', '"parsed":', 1),
            "incomplete": valid[:-1],
            "deep": "[" * 10000,
            "platform": valid.replace('"guest_platform":2', '"guest_platform":1', 1),
        }
        for name, text in cases.items():
            with self.subTest(name):
                self.assertEqual(self.cli(self.write(name + ".json", text)).returncode, 2)

    def test_unused_read_fails_the_replay(self) -> None:
        read = {"address_hi": 0, "address_lo": 4096, "dwords": 1, "success": 1, "words": [7]}
        document = self.write("unused.json", minimal_document(self.evaluator_output(), [read]))
        self.assertEqual(self.cli(document).returncode, 3)

    def test_changed_output_is_a_mismatch(self) -> None:
        document = self.write("mismatch.json", minimal_document(self.evaluator_output() + "extra=1\n"))
        self.assertEqual(self.cli(document).returncode, 4)


if __name__ == "__main__":
    unittest.main()
