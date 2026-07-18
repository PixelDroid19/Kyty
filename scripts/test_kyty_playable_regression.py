#!/usr/bin/env python3
"""Unit tests for the strict playable regression profile (no private titles)."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("kyty_playable_regression.py")
SPEC = importlib.util.spec_from_file_location("kyty_playable_regression", MODULE_PATH)
assert SPEC and SPEC.loader
reg = importlib.util.module_from_spec(SPEC)
sys.modules["kyty_playable_regression"] = reg
SPEC.loader.exec_module(reg)

CAPTURE_PATH = Path(__file__).with_name("kyty_capture.py")
CSPEC = importlib.util.spec_from_file_location("kyty_capture", CAPTURE_PATH)
assert CSPEC and CSPEC.loader
capture = importlib.util.module_from_spec(CSPEC)
sys.modules["kyty_capture"] = capture
CSPEC.loader.exec_module(capture)


class StrictEnvTests(unittest.TestCase):
    def test_clean_child_env_strips_forbidden(self) -> None:
        base = {
            "PATH": "/usr/bin",
            "HOME": "/tmp",
            "DISPLAY": ":0",
            "KYTY_STUB_MISSING": "1",
            "KYTY_GFX_PERMISSIVE": "1",
            "KYTY_BRINGUP_MODE": "unsafe",
            "KYTY_AUTO_CROSS": "1",
            "KYTY_SKIP_UD2": "1",
        }
        with tempfile.TemporaryDirectory() as td:
            env = reg.clean_child_env(
                base,
                guest_root=Path(td) / "g",
                agent_sock=Path(td) / "a.sock",
                capture_dir=Path(td) / "c",
            )
        for k in reg.FORBIDDEN_CHILD_ENV:
            self.assertNotIn(k, env)
        self.assertEqual(reg.assert_strict_env(env), [])
        self.assertEqual(env["KYTY_PRINTF_DIRECTION"], "Silent")


class ProfileLoadTests(unittest.TestCase):
    def test_default_profile_has_deadlines_only(self) -> None:
        p = reg.load_profile(None)
        self.assertEqual(p["mode"], "strict")
        self.assertIn("agent_ready", p["deadlines_s"])
        self.assertNotIn("title", p)
        self.assertNotIn("guest_root", p)

    def test_load_shipped_profile_json(self) -> None:
        path = Path(__file__).resolve().parents[0] / "profiles" / "strict_playable.json"
        if not path.is_file():
            self.skipTest("shipped profile missing")
        p = reg.load_profile(path)
        self.assertEqual(p["schema"], "kyty_playable_regression_profile_v1")
        text = json.dumps(p).lower()
        self.assertNotIn("dead cells", text)
        self.assertNotIn("ppsa", text)


class GateClassificationTests(unittest.TestCase):
    def test_all_gates_pass_happy_path(self) -> None:
        gates = reg.evaluate_gates(
            agent_ready=True,
            video_initialized=True,
            first_present=10,
            present_delta=20,
            input_before={"delivered_taps": 0, "guest_read_state_samples": 0},
            input_after={"delivered_taps": 2, "guest_read_state_samples": 5},
            capture_path="frame.bmp",
            compare={"pass": True, "checks": {}},
            baseline_missing=False,
            create_baseline=False,
            shutdown="controlled_timeout",
            first_error="",
            milestones={"min_present_after_ready": 1, "present_delta_stable": 15, "min_pad_taps": 1},
            visual_require_baseline=True,
        )
        self.assertTrue(all(g.passed for g in gates))

    def test_host_crash_fails_gate(self) -> None:
        gates = reg.evaluate_gates(
            agent_ready=True,
            video_initialized=False,
            first_present=None,
            present_delta=0,
            input_before={},
            input_after={},
            capture_path="",
            compare={},
            baseline_missing=True,
            create_baseline=False,
            shutdown="host_crash",
            first_error="Unpatched non-Func import!!!",
            milestones={"min_present_after_ready": 1, "present_delta_stable": 15, "min_pad_taps": 1},
            visual_require_baseline=True,
        )
        by = {g.name: g for g in gates}
        self.assertFalse(by["no_host_crash"].passed)
        self.assertFalse(by["first_present"].passed)
        self.assertFalse(by["no_early_loader_failure"].passed)

    def test_baseline_missing_fails_visual(self) -> None:
        gates = reg.evaluate_gates(
            agent_ready=True,
            video_initialized=True,
            first_present=5,
            present_delta=20,
            input_before={"delivered_taps": 0},
            input_after={"delivered_taps": 1},
            capture_path="x.bmp",
            compare={},
            baseline_missing=True,
            create_baseline=False,
            shutdown="controlled_kill",
            first_error="",
            milestones={"min_present_after_ready": 1, "present_delta_stable": 15, "min_pad_taps": 1},
            visual_require_baseline=True,
        )
        by = {g.name: g for g in gates}
        self.assertFalse(by["visual_compare"].passed)


class CompareWiringTests(unittest.TestCase):
    def test_uses_shipped_compare_metrics(self) -> None:
        current = {
            "world": {
                "white_ratio": 0.05,
                "entropy": 6.5,
                "unique_quantized_colors": 900,
                "stripey": False,
                "scene_ok": False,
            }
        }
        baseline = {
            "world": {
                "white_ratio": 0.04,
                "entropy": 6.8,
                "unique_quantized_colors": 1000,
                "stripey": False,
                "scene_ok": True,
            }
        }
        raw = capture.compare_metrics(current, baseline)
        # Profile material gates: ignore OCR scene_ok
        material = {
            "white_ratio_not_worse": raw["checks"]["white_ratio_not_worse"],
            "entropy_not_collapsed": raw["checks"]["entropy_not_collapsed"],
            "colors_not_collapsed": raw["checks"]["colors_not_collapsed"],
            "not_stripey": raw["checks"]["not_stripey"],
        }
        self.assertTrue(all(material.values()))


class SanitizeTests(unittest.TestCase):
    def test_summary_redacts_guest_root(self) -> None:
        report = reg.RunReport()
        report.gates = [reg.GateResult("agent_ready", True, "ok")]
        report.notes = ["/home/user/secret/game"]
        report.first_error = ""
        d = report.to_sanitized_dict(Path("/home/user/secret/game"))
        text = json.dumps(d)
        self.assertNotIn("/home/user/secret/game", text)
        self.assertNotIn("/home/", text)


class MissingGuestCliTests(unittest.TestCase):
    def test_allow_missing_guest_skip(self) -> None:
        import os

        with tempfile.TemporaryDirectory() as td:
            old = os.environ.pop("KYTY_GUEST_ROOT", None)
            try:
                rc = reg.main(
                    [
                        "--scratch",
                        td,
                        "--allow-missing-guest",
                        "--guest-root",
                        "",
                    ]
                )
            finally:
                if old is not None:
                    os.environ["KYTY_GUEST_ROOT"] = old
            self.assertEqual(rc, 0)
            skip = Path(td) / "summary_skip.json"
            self.assertTrue(skip.is_file())
            data = json.loads(skip.read_text(encoding="utf-8"))
            self.assertTrue(data.get("skipped"))


if __name__ == "__main__":
    unittest.main()
