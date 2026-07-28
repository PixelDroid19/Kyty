#!/usr/bin/env python3
"""Tests for the shared process-boundary runner infrastructure."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import kyty_runner_common as runner


class ChildEnvironmentTests(unittest.TestCase):
    def test_builds_a_minimal_strict_environment(self) -> None:
        base = {
            "PATH": "/usr/bin",
            "DISPLAY": ":0",
            "KYTY_STUB_MISSING": "1",
            "KYTY_AUTO_CROSS": "1",
            "UNRELATED_PARENT_VALUE": "private",
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            env = runner.build_child_environment(
                base,
                guest_root=root / "guest",
                agent_socket=root / "agent.sock",
                capture_directory=root / "captures",
            )

        self.assertEqual(env["PATH"], "/usr/bin")
        self.assertEqual(env["DISPLAY"], ":0")
        self.assertEqual(env["KYTY_PRINTF_DIRECTION"], "Silent")
        self.assertNotIn("UNRELATED_PARENT_VALUE", env)
        self.assertEqual(runner.find_forbidden_environment_keys(env), [])

    def test_preserves_only_explicit_optional_values(self) -> None:
        base = {
            "KYTY_INTERNAL_RESOLUTION_WIDTH": "1920",
            "KYTY_INTERNAL_RESOLUTION_HEIGHT": "1080",
            "KYTY_NATIVE_CAPTURE_MAX_EDGE": "2048",
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            env = runner.build_child_environment(
                base,
                guest_root=root / "guest",
                agent_socket=root / "agent.sock",
                capture_directory=root / "captures",
                optional_values=(
                    "KYTY_INTERNAL_RESOLUTION_WIDTH",
                    "KYTY_INTERNAL_RESOLUTION_HEIGHT",
                ),
                default_values={"KYTY_NATIVE_CAPTURE_MAX_EDGE": "1280"},
            )

        self.assertEqual(env["KYTY_INTERNAL_RESOLUTION_WIDTH"], "1920")
        self.assertEqual(env["KYTY_INTERNAL_RESOLUTION_HEIGHT"], "1080")
        self.assertEqual(env["KYTY_NATIVE_CAPTURE_MAX_EDGE"], "2048")


class ProcessTests(unittest.TestCase):
    def test_launches_an_isolated_process_group_with_canonical_stdio(self) -> None:
        process = object()
        with mock.patch.object(runner.subprocess, "Popen", return_value=process) as popen:
            result = runner.launch_process_group(
                ["fc_script", "scripts/run_guest.lua", "/guest"],
                cwd=Path("/repo"),
                env={"PATH": "/usr/bin"},
                stdout="log",
            )

        self.assertIs(result, process)
        popen.assert_called_once_with(
            ["fc_script", "scripts/run_guest.lua", "/guest"],
            cwd="/repo",
            env={"PATH": "/usr/bin"},
            stdout="log",
            stderr=subprocess.STDOUT,
            start_new_session=(runner.os.name == "posix"),
        )

    def test_reports_a_process_that_survives_sigkill(self) -> None:
        class Process:
            pid = 123

            @staticmethod
            def poll() -> None:
                return None

            @staticmethod
            def wait(timeout: int) -> None:
                raise subprocess.TimeoutExpired("guest", timeout)

        notes: list[str] = []
        with mock.patch.object(runner.os, "killpg"):
            runner.terminate_process_group(Process(), notes)

        self.assertIn("kill_timeout_after_sigkill", notes)


if __name__ == "__main__":
    unittest.main()
