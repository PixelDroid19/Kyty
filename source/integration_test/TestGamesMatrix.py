#!/usr/bin/env python3
"""Process-boundary integration checks for the multi-game matrix runner."""

from __future__ import annotations

import importlib.util
import json
import os
import socket
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock


import sys

MODULE_PATH = Path(__file__).resolve().parents[2] / "scripts" / "kyty_games_matrix.py"
SPEC = importlib.util.spec_from_file_location("kyty_games_matrix", MODULE_PATH)
assert SPEC and SPEC.loader
matrix = importlib.util.module_from_spec(SPEC)
sys.modules["kyty_games_matrix"] = matrix
SPEC.loader.exec_module(matrix)


def _touch(path: Path, data: bytes = b"\x7fELF") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class DiscoveryTests(unittest.TestCase):
    def test_discovers_only_eboot_bin_used_by_run_guest(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _touch(root / "pkg_a" / "eboot.bin")
            _touch(root / "nested" / "pkg_b" / "main.elf")
            _touch(root / "nested" / "pkg_c" / "EBOOT.ELF")  # case-insensitive
            _touch(root / "noise" / "readme.txt", b"hi")
            _touch(root / "pkg_a" / "sce_module" / "libSceLibcInternal.prx")
            found = matrix.discover_game_roots(root)
            self.assertEqual(len(found), 1)
            names = {p.name for p in found}
            self.assertEqual(names, {"pkg_a"})

    def test_anonymous_case_id_stable_and_pathless(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            games = Path(td)
            pkg = games / "Some Private Title PPSA00000"
            _touch(pkg / "eboot.bin")
            a = matrix.anonymous_case_id(games, pkg)
            b = matrix.anonymous_case_id(games, pkg)
            self.assertEqual(a, b)
            self.assertTrue(a.startswith("case_"))
            self.assertNotIn("Private", a)
            self.assertNotIn("PPSA", a)
            self.assertNotIn(str(pkg), a)

    def test_classify_invalid_without_primary(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td) / "empty"
            d.mkdir()
            ok, reason = matrix.classify_candidate(d)
            self.assertFalse(ok)
            self.assertEqual(reason, "missing_primary_executable")

    def test_discover_only_cli_sanitized_summary(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            games = td_path / "games"
            _touch(games / "alpha" / "eboot.bin")
            _touch(games / "beta" / "main.elf")
            (games / "junk").mkdir(parents=True)
            (games / "junk" / "note.txt").write_text("x", encoding="utf-8")
            scratch = td_path / "scratch"
            rc = matrix.main(
                [
                    "--discover-only",
                    "--games-root",
                    str(games),
                    "--scratch",
                    str(scratch),
                ]
            )
            self.assertEqual(rc, 0)
            runs = list(scratch.glob("run_*"))
            self.assertEqual(len(runs), 1)
            summary = json.loads((runs[0] / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["meta"]["discover_only"], True)
            self.assertEqual(summary["counts"]["valid_packages"], 1)
            self.assertEqual(summary["counts"]["attempted"], 0)
            self.assertEqual(summary["counts"]["pending_discover_only"], 1)
            text = json.dumps(summary)
            self.assertNotIn(str(games), text)
            self.assertNotIn("alpha", text)
            self.assertNotIn("beta", text)
            self.assertNotIn("/home/", text)


class StrictEnvTests(unittest.TestCase):
    def test_clean_child_env_strips_forbidden_keys(self) -> None:
        base = {
            "PATH": "/usr/bin",
            "HOME": "/tmp",
            "KYTY_STUB_MISSING": "1",
            "KYTY_GFX_PERMISSIVE": "1",
            "KYTY_BRINGUP_MODE": "unsafe",
            "KYTY_BRINGUP_FEATURES": "x",
            "DISPLAY": ":0",
        }
        with tempfile.TemporaryDirectory() as td:
            guest = Path(td) / "g"
            guest.mkdir()
            env = matrix.clean_child_env(
                base,
                guest_root=guest,
                agent_sock=Path(td) / "a.sock",
                capture_dir=Path(td) / "c",
            )
        for k in matrix.FORBIDDEN_CHILD_ENV:
            self.assertNotIn(k, env)
        self.assertEqual(matrix.assert_strict_env(env), [])
        self.assertEqual(env["KYTY_PRINTF_DIRECTION"], "Silent")
        self.assertIn("KYTY_GUEST_ROOT", env)
        self.assertIn("KYTY_AGENT_SOCK", env)

    def test_bringup_child_env_uses_only_central_policy(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            guest = Path(td) / "g"
            guest.mkdir()
            env = matrix.clean_child_env(
                {"PATH": "/usr/bin"},
                guest_root=guest,
                agent_sock=Path(td) / "a.sock",
                capture_dir=Path(td) / "c",
                mode="bringup",
            )
        self.assertEqual(env["KYTY_BRINGUP_MODE"], "unsafe")
        self.assertEqual(env["KYTY_BRINGUP_ALLOW_DIAGNOSTIC"], "1")
        self.assertIn("missing_function_import", env["KYTY_BRINGUP_FEATURES"])
        self.assertIn("gfx_permissive", env["KYTY_BRINGUP_FEATURES"])
        self.assertNotIn("KYTY_STUB_MISSING", env)
        self.assertNotIn("KYTY_GFX_PERMISSIVE", env)


class StaleSocketTests(unittest.TestCase):
    def test_sock_is_stale_for_orphan_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "dead.sock"
            p.write_text("not a socket", encoding="utf-8")
            self.assertTrue(matrix.sock_is_stale(p))
            notes: list[str] = []
            matrix.remove_stale_socket(p, notes)
            self.assertFalse(p.exists())
            self.assertIn("removed_stale_socket", notes)

    def test_sock_is_live_when_listener_accepts(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "live.sock"
            if p.exists():
                p.unlink()
            srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            srv.bind(str(p))
            srv.listen(1)
            stop = threading.Event()

            def accept_loop() -> None:
                srv.settimeout(0.3)
                while not stop.is_set():
                    try:
                        conn, _ = srv.accept()
                        conn.close()
                    except socket.timeout:
                        continue
                    except OSError:
                        break

            t = threading.Thread(target=accept_loop, daemon=True)
            t.start()
            try:
                self.assertFalse(matrix.sock_is_stale(p))
            finally:
                stop.set()
                srv.close()
                if p.exists():
                    p.unlink()


class StalePidTests(unittest.TestCase):
    def test_pid_file_is_removed_without_signalling_recorded_pid(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            pid_path = Path(td) / "child.pid"
            pid_path.write_text(str(os.getpid()), encoding="utf-8")
            notes: list[str] = []
            with mock.patch.object(matrix.os, "kill") as kill:
                matrix.clear_untrusted_pid_file(pid_path, notes)
            kill.assert_not_called()
            self.assertFalse(pid_path.exists())
            self.assertIn("untrusted_stale_pid_file_removed", notes)


class OutcomeMapTests(unittest.TestCase):
    def test_host_crash_exit(self) -> None:
        oc, fr = matrix.map_outcome(
            process_exit=139,
            agent_ready=True,
            status={"phase": "booting", "frontier": "loader", "present": 0},
            last_error_code="",
            timed_out=False,
            notes=[],
        )
        self.assertEqual(oc, "host_crash")
        self.assertEqual(fr, "loader")

    def test_exit_65_with_bringup_halt_is_unsupported(self) -> None:
        oc, fr = matrix.map_outcome(
            process_exit=65,
            agent_ready=True,
            status={"phase": "booting", "frontier": "none", "present": 0},
            last_error_code="bringup_halt",
            timed_out=False,
            notes=[],
        )
        self.assertEqual(oc, "unsupported")
        self.assertEqual(fr, "bringup_halt")

    def test_exit_65_without_crash_evidence_is_not_host_crash(self) -> None:
        oc, _ = matrix.map_outcome(
            process_exit=65,
            agent_ready=True,
            status={"phase": "error", "frontier": "error", "present": 0},
            last_error_code="",
            timed_out=False,
            notes=[],
        )
        self.assertEqual(oc, "loader_failed")

    def test_strict_abort_cannot_be_reported_as_video_initialized(self) -> None:
        oc, fr = matrix.map_outcome(
            process_exit=65,
            agent_ready=True,
            status={"phase": "booting", "frontier": "none", "present": 0, "graphic_ready": True},
            last_error_code="",
            timed_out=False,
            notes=[],
        )
        self.assertEqual(oc, "loader_failed")
        self.assertEqual(fr, "post_agent_exit")


class FrontierExtractionTests(unittest.TestCase):
    def test_fatal_log_replaces_stale_interactive_frontier_for_host_crash(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "child.log"
            log.write_text(
                "normal startup\nFATAL-ACCESS-VIOLATION 0x0000000000001234 0x0000000000005678\n",
                encoding="utf-8",
            )
            label = matrix.first_actionable_from_log(log)
        self.assertEqual(label, "FATAL-ACCESS-VIOLATION 0x0000000000001234 0x0000000000005678")

    def test_unknown_decoder_line_is_actionable(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "child.log"
            log.write_text(
                "unknown mimg format for opcode: 0x27 at addr 0x00000a68, dmask: 0x1\n",
                encoding="utf-8",
            )
            label = matrix.first_actionable_from_log(log)
        self.assertEqual(label, "unknown mimg format for opcode: 0x27 at addr 0x00000a68, dmask: 0x1")

    def test_unsupported_from_last_error(self) -> None:
        oc, fr = matrix.map_outcome(
            process_exit=None,
            agent_ready=True,
            status={"phase": "error", "frontier": "unsupported", "present": 0},
            last_error_code="bringup_halt",
            timed_out=False,
            notes=[],
        )
        self.assertEqual(oc, "unsupported")

    def test_presenting_and_controlled_timeout(self) -> None:
        oc, fr = matrix.map_outcome(
            process_exit=None,
            agent_ready=True,
            status={"phase": "loading", "frontier": "presenting", "present": 12, "graphic_ready": True},
            last_error_code="",
            timed_out=True,
            notes=[],
        )
        self.assertEqual(oc, "controlled_timeout")
        self.assertEqual(fr, "presenting")

    def test_launch_failed_before_agent(self) -> None:
        oc, fr = matrix.map_outcome(
            process_exit=1,
            agent_ready=False,
            status={},
            last_error_code="",
            timed_out=False,
            notes=[],
        )
        self.assertEqual(oc, "launch_failed")

    def test_runtime_started_agent_ready(self) -> None:
        oc, fr = matrix.map_outcome(
            process_exit=None,
            agent_ready=True,
            status={"phase": "booting", "frontier": "startup", "present": 0, "graphic_ready": False},
            last_error_code="",
            timed_out=False,
            notes=[],
        )
        self.assertEqual(oc, "runtime_started")

    def test_loader_failed_after_agent(self) -> None:
        oc, fr = matrix.map_outcome(
            process_exit=2,
            agent_ready=True,
            status={"phase": "error", "frontier": "error", "present": 0},
            last_error_code="load_failed",
            timed_out=False,
            notes=[],
        )
        self.assertEqual(oc, "loader_failed")


class ContinueOnFailTests(unittest.TestCase):
    def test_matrix_continues_after_one_parent_exception(self) -> None:
        """Parent exception on case N must not abort case N+1."""
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            games = td_path / "games"
            _touch(games / "one" / "eboot.bin")
            _touch(games / "two" / "eboot.bin")
            scratch = td_path / "scratch"
            # Point fc_script at a process that exits immediately. Each case
            # must still be recorded instead of aborting the matrix.
            fake = td_path / "fake_fc"
            fake.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
            fake.chmod(0o755)
            rc = matrix.main(
                [
                    "--games-root",
                    str(games),
                    "--scratch",
                    str(scratch),
                    "--fc-script",
                    str(fake),
                    "--startup-deadline-s",
                    "2",
                    "--present-deadline-s",
                    "2",
                    "--total-deadline-s",
                    "3",
                ]
            )
            self.assertNotEqual(rc, 0)
            runs = list(scratch.glob("run_*"))
            self.assertEqual(len(runs), 1)
            summary = json.loads((runs[0] / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["counts"]["attempted"], 2)
            self.assertEqual(summary["counts"]["valid_packages"], 2)
            # Both attempted exactly once
            case_ids = [c["case_id"] for c in summary["all_cases"] if c.get("valid_package")]
            self.assertEqual(len(case_ids), 2)
            self.assertEqual(len(set(case_ids)), 2)
            for c in summary["all_cases"]:
                if c.get("valid_package") and c.get("attempted"):
                    self.assertIn(c["outcome"], matrix.OUTCOMES)
            text = json.dumps(summary)
            self.assertNotIn("one", text)
            self.assertNotIn("two", text)


class AgentSockPathTests(unittest.TestCase):
    def test_agent_sock_path_fits_unix_limit(self) -> None:
        with tempfile.TemporaryDirectory(prefix="very_long_matrix_scratch_dir_name_") as td:
            # Nest to force a long case_scratch path
            deep = Path(td)
            for i in range(6):
                deep = deep / f"layer_{i}_with_extra_padding_name"
            deep.mkdir(parents=True)
            sock = matrix.agent_sock_path("case_abcdef123456", deep, "run_a")
            self.assertLess(len(str(sock)), 100)
            self.assertTrue(str(sock).startswith("/tmp/kyty_mx_"))

    def test_same_case_uses_distinct_socket_for_each_run(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            scratch = Path(td)
            first = matrix.agent_sock_path("case_abcdef123456", scratch, "run_a")
            second = matrix.agent_sock_path("case_abcdef123456", scratch, "run_b")
            self.assertNotEqual(first, second)

    def test_cleanup_does_not_unlink_replaced_socket(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "agent.sock"
            path.write_text("owned", encoding="utf-8")
            identity = matrix.path_identity(path)
            path.unlink()
            path.write_text("replacement", encoding="utf-8")
            notes: list[str] = []
            matrix.unlink_owned_socket(path, identity, notes)
            self.assertTrue(path.exists())
            self.assertIn("socket_identity_changed", notes)


class CleanupTests(unittest.TestCase):
    def test_second_wait_timeout_is_caught(self) -> None:
        proc = mock.Mock()
        proc.poll.return_value = None
        proc.pid = 1234
        proc.wait.side_effect = [
            subprocess.TimeoutExpired(cmd="guest", timeout=5),
            subprocess.TimeoutExpired(cmd="guest", timeout=5),
        ]
        notes: list[str] = []
        with mock.patch.object(matrix.os, "killpg"):
            matrix.kill_process_group(proc, notes)
        self.assertIn("kill_timeout_after_sigkill", notes)


class FinalStatusTests(unittest.TestCase):
    def test_final_status_is_sampled_while_owned_socket_is_live(self) -> None:
        status = {"phase": "loading"}
        response = {"ok": True, "result": {"phase": "interactive", "present": 7}}
        with mock.patch.object(matrix, "sock_is_stale", return_value=False), mock.patch.object(
            matrix, "agent_call", return_value=(0, response)
        ) as call:
            sampled = matrix.sample_final_status(
                Path("/tmp/owned.sock"),
                agent_ready=True,
                current=status,
            )
        call.assert_called_once()
        self.assertEqual(sampled["phase"], "interactive")
        self.assertEqual(sampled["present"], 7)


class SanitizationTests(unittest.TestCase):
    def test_recursive_sanitization_removes_paths_and_title_identifiers(self) -> None:
        private = {
            "notes": [
                "failed at /mnt/private/Games/Secret/game.bin",
                r"failed at C:\Games\Secret\game.bin",
                "title PPSA12345 and CUSA99999",
            ],
            "nested": {"path": "/Volumes/Games/Secret", "safe": "runtime_started"},
        }
        sanitized = matrix.sanitize_summary_value(private)
        text = json.dumps(sanitized)
        for marker in ("/mnt/", "/Volumes/", "C:\\", "PPSA12345", "CUSA99999", "Secret"):
            self.assertNotIn(marker, text)
        self.assertIn("runtime_started", text)


class RunIdentityTests(unittest.TestCase):
    def test_run_ids_are_unique_even_with_same_clock_value(self) -> None:
        with mock.patch.object(matrix.time, "time_ns", return_value=123):
            first = matrix.new_run_id()
            second = matrix.new_run_id()
        self.assertNotEqual(first, second)


class ExitGateTests(unittest.TestCase):
    def test_failed_attempted_game_returns_nonzero(self) -> None:
        result = matrix.CaseResult(
            case_id="case_x",
            outcome="loader_failed",
            first_frontier="loader",
            valid_package=True,
            attempted=True,
            progressed=False,
        )
        self.assertNotEqual(matrix.matrix_exit_code([result], discover_only=False), 0)

    def test_successful_attempted_game_returns_zero(self) -> None:
        result = matrix.CaseResult(
            case_id="case_x",
            outcome="presenting",
            first_frontier="presenting",
            valid_package=True,
            attempted=True,
            progressed=True,
        )
        self.assertEqual(matrix.matrix_exit_code([result], discover_only=False), 0)

    def test_discover_only_is_always_zero(self) -> None:
        result = matrix.CaseResult(
            case_id="case_x",
            outcome="invalid_fixture",
            first_frontier="missing_primary_executable",
            valid_package=False,
            attempted=False,
            progressed=False,
        )
        self.assertEqual(matrix.matrix_exit_code([result], discover_only=True), 0)


class LogFrontierTests(unittest.TestCase):
    def test_first_actionable_from_assert_banner(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "child.log"
            p.write_text(
                "\n".join(
                    [
                        "KYTY_AGENT listening on /tmp/x.sock",
                        "--- Stack Trace ---",
                        "=== Unexpected non-Func after import validation Ok ===",
                        "[10]\tsome_nid type=2",
                        " in /home/user/Kyty/source/emulator/src/Loader/RuntimeLinker.cpp:1270",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            label = matrix.first_actionable_from_log(p)
            self.assertEqual(label, "Unexpected non-Func after import validation Ok")

    def test_first_actionable_from_not_implemented_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "child.log"
            log.write_text(
                "--- Stack Trace ---\n"
                "--- Fatal Error ---\n"
                "Not implemented (offen == 1) in /home/private/ShaderParse.cpp:2770\n",
                encoding="utf-8",
            )
            label = matrix.first_actionable_from_log(log)
            self.assertEqual(label, "Not implemented (offen == 1)")
            self.assertNotIn("/home/", label)


class OutcomeVocabularyTests(unittest.TestCase):
    def test_required_outcomes_present(self) -> None:
        required = {
            "invalid_fixture",
            "launch_failed",
            "loader_failed",
            "unsupported",
            "runtime_started",
            "video_initialized",
            "presenting",
            "interactive",
            "controlled_timeout",
            "guest_exit",
            "host_crash",
        }
        self.assertTrue(required.issubset(matrix.OUTCOMES))


if __name__ == "__main__":
    unittest.main()
