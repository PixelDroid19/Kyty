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

    def test_default_profile_uses_exactly_three_cross_taps(self) -> None:
        p = reg.load_profile(None)
        self.assertEqual(
            p["pad_sequence"],
            [
                {"tool": "pad_tap", "button": "cross"},
                {"tool": "pad_tap", "button": "cross"},
                {"tool": "pad_tap", "button": "cross"},
            ],
        )
        self.assertTrue(p["post_input"]["require_loading_transition"])
        self.assertEqual(p["post_input"]["min_present_delta"], 240)
        self.assertEqual(p["post_input"]["min_settle_s"], 15)
        self.assertEqual(p["input"]["menu_settle_s"], 5)
        self.assertEqual(p["input"]["inter_tap_settle_s"], 3)
        self.assertEqual(p["milestones"]["min_pad_taps"], 3)

    def test_load_shipped_profile_json(self) -> None:
        path = Path(__file__).resolve().parents[0] / "profiles" / "strict_playable.json"
        if not path.is_file():
            self.skipTest("shipped profile missing")
        p = reg.load_profile(path)
        self.assertEqual(p["schema"], "kyty_playable_regression_profile_v1")
        text = json.dumps(p).lower()
        self.assertNotIn("dead cells", text)
        self.assertNotIn("ppsa", text)

    def test_summary_preserves_input_timing_profile(self) -> None:
        report = reg.RunReport(
            profile={
                "deadlines_s": {"total": 180},
                "milestones": {"min_pad_taps": 3},
                "input": {"menu_settle_s": 5, "inter_tap_settle_s": 3},
                "post_input": {
                    "require_loading_transition": True,
                    "min_present_delta": 240,
                    "min_settle_s": 15,
                },
            }
        )

        summary = report.to_sanitized_dict()

        self.assertEqual(summary["profile_input"]["menu_settle_s"], 5)
        self.assertEqual(summary["profile_input"]["inter_tap_settle_s"], 3)
        self.assertTrue(summary["profile_post_input"]["require_loading_transition"])
        self.assertEqual(summary["profile_post_input"]["min_present_delta"], 240)


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
            milestones={"min_present_after_ready": 1, "present_delta_stable": 15, "min_pad_taps": 2},
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

    def test_input_gate_rejects_partial_startup_sequence(self) -> None:
        gates = reg.evaluate_gates(
            agent_ready=True,
            video_initialized=True,
            first_present=5,
            present_delta=20,
            input_before={"delivered_taps": 0, "guest_read_state_samples": 0},
            input_after={"delivered_taps": 2, "guest_read_state_samples": 100},
            capture_path="frame.bmp",
            compare={"pass": True, "checks": {}},
            baseline_missing=False,
            create_baseline=False,
            shutdown="controlled_kill",
            first_error="",
            milestones={
                "min_present_after_ready": 1,
                "present_delta_stable": 15,
                "min_pad_taps": 3,
                "min_guest_read_state_samples": 1,
            },
            visual_require_baseline=True,
        )

        by = {gate.name: gate for gate in gates}
        self.assertFalse(by["input_delivered"].passed)

    def test_input_gate_rejects_failed_clear_after_three_taps(self) -> None:
        gates = reg.evaluate_gates(
            agent_ready=True,
            video_initialized=True,
            first_present=5,
            present_delta=20,
            input_before={"delivered_taps": 0, "guest_read_state_samples": 0},
            input_after={"delivered_taps": 3, "guest_read_state_samples": 100},
            capture_path="frame.bmp",
            compare={"pass": True, "checks": {}},
            baseline_missing=False,
            create_baseline=False,
            shutdown="controlled_kill",
            first_error="",
            milestones={
                "min_present_after_ready": 1,
                "present_delta_stable": 15,
                "min_pad_taps": 3,
                "min_guest_read_state_samples": 1,
            },
            visual_require_baseline=True,
            input_sequence_ok=False,
        )

        by = {gate.name: gate for gate in gates}
        self.assertFalse(by["input_sequence_complete"].passed)
        self.assertFalse(by["input_delivered"].passed)


class PadSequenceTests(unittest.TestCase):
    def test_sequence_delivers_three_taps_then_always_clears(self) -> None:
        calls: list[tuple[str, dict[str, object]]] = []

        def call(_sock: Path, tool: str, args: dict[str, object], timeout: float):
            del timeout
            calls.append((tool, args))
            if tool == "status":
                return 0, {"ok": True, "result": {"pad": {"tap_pending": False}}}
            return 0, {"ok": True}

        ok, events = reg.deliver_pad_sequence(
            Path("/tmp/test.sock"),
            [
                {"tool": "pad_tap", "button": "cross"},
                {"tool": "pad_tap", "button": "cross"},
                {"tool": "pad_tap", "button": "cross"},
            ],
            call=call,
            pause=lambda _seconds: None,
        )

        self.assertTrue(ok)
        mutating = [tool for tool, _args in calls if tool.startswith("pad_")]
        self.assertEqual(mutating, ["pad_tap", "pad_tap", "pad_tap", "pad_clear"])
        self.assertEqual(events[-1]["event"], "pad_clear")

    def test_sequence_spaces_taps_for_ui_transitions(self) -> None:
        now = [0.0]
        tap_times: list[float] = []

        def call(_sock: Path, tool: str, _args: dict[str, object], timeout: float):
            del timeout
            if tool == "pad_tap":
                tap_times.append(now[0])
            if tool == "status":
                return 0, {"ok": True, "result": {"pad": {"tap_pending": False}}}
            return 0, {"ok": True}

        ok, _events = reg.deliver_pad_sequence(
            Path("/tmp/test.sock"),
            [
                {"tool": "pad_tap", "button": "cross"},
                {"tool": "pad_tap", "button": "cross"},
                {"tool": "pad_tap", "button": "cross"},
            ],
            call=call,
            pause=lambda seconds: now.__setitem__(0, now[0] + seconds),
            clock=lambda: now[0],
            inter_tap_settle_s=1.0,
        )

        self.assertTrue(ok)
        self.assertEqual(tap_times, [0.0, 1.0, 2.0])

    def test_sequence_clears_even_when_a_tap_fails(self) -> None:
        calls: list[str] = []

        def call(_sock: Path, tool: str, _args: dict[str, object], timeout: float):
            del timeout
            calls.append(tool)
            return (1 if tool == "pad_tap" else 0), {"ok": tool != "pad_tap"}

        ok, _events = reg.deliver_pad_sequence(
            Path("/tmp/test.sock"),
            [{"tool": "pad_tap", "button": "cross"}],
            call=call,
            pause=lambda _seconds: None,
        )

        self.assertFalse(ok)
        self.assertEqual(calls, ["pad_tap", "pad_clear"])

    def test_sequence_stops_polling_at_its_deadline_and_clears(self) -> None:
        now = [0.0]
        calls: list[tuple[str, float]] = []

        def call(_sock: Path, tool: str, _args: dict[str, object], timeout: float):
            calls.append((tool, timeout))
            if tool == "status":
                now[0] += timeout
                return 0, {"ok": True, "result": {"pad": {"tap_pending": True}}}
            return 0, {"ok": True}

        ok, _events = reg.deliver_pad_sequence(
            Path("/tmp/test.sock"),
            [{"tool": "pad_tap", "button": "cross"}],
            call=call,
            pause=lambda _seconds: None,
            deadline=1.0,
            clock=lambda: now[0],
        )

        self.assertFalse(ok)
        self.assertEqual([tool for tool, _timeout in calls], ["pad_tap", "status", "pad_clear"])
        self.assertLessEqual(calls[1][1], 1.0)
        self.assertGreaterEqual(calls[-1][1], 0.1)

    def test_deadline_timeout_never_exceeds_remaining_budget(self) -> None:
        self.assertEqual(reg.deadline_timeout(10.0, 2.0, clock=lambda: 9.5), 0.5)
        self.assertEqual(reg.deadline_timeout(10.0, 2.0, clock=lambda: 10.0), 0.0)
        self.assertEqual(reg.deadline_timeout(10.0, 2.0, clock=lambda: 11.0), 0.0)

    def test_startup_input_waits_for_interactive_phase(self) -> None:
        self.assertFalse(reg.can_start_pad_sequence(True, 1, "loading", True, 5.0, 10.0, 5.0))
        self.assertFalse(reg.can_start_pad_sequence(True, 1, "booting", True, 5.0, 10.0, 5.0))
        self.assertFalse(reg.can_start_pad_sequence(True, 1, "interactive", True, 4.9, 10.0, 5.0))
        self.assertTrue(reg.can_start_pad_sequence(True, 1, "interactive", True, 5.0, 10.0, 5.0))

    def test_post_input_wait_can_use_bounded_settle_when_loading_label_is_missed(self) -> None:
        seen, ready = reg.advance_post_input_wait(False, False, "interactive", 119, 5.0, 120, 5.0)
        self.assertFalse(seen)
        self.assertFalse(ready)

        seen, ready = reg.advance_post_input_wait(False, seen, "interactive", 120, 4.9, 120, 5.0)
        self.assertFalse(seen)
        self.assertFalse(ready)

        seen, ready = reg.advance_post_input_wait(False, seen, "interactive", 120, 5.0, 120, 5.0)
        self.assertFalse(seen)
        self.assertTrue(ready)

    def test_post_input_wait_honors_optional_loading_transition(self) -> None:
        seen, ready = reg.advance_post_input_wait(True, False, "interactive", 200, 10.0, 120, 5.0)
        self.assertFalse(ready)

        seen, ready = reg.advance_post_input_wait(True, seen, "loading", 201, 10.1, 120, 5.0)
        self.assertTrue(seen)
        self.assertFalse(ready)

        seen, ready = reg.advance_post_input_wait(True, seen, "interactive", 202, 10.2, 120, 5.0)
        self.assertTrue(ready)


class CompareWiringTests(unittest.TestCase):
    def test_playable_compare_preserves_raw_checks(self) -> None:
        result = reg.compare_playable_visual_metrics(
            capture,
            metrics={"world": {"white_ratio": 0.04, "entropy": 6.8, "unique_quantized_colors": 900, "stripey": False}},
            baseline={"world": {"white_ratio": 0.04, "entropy": 6.8, "unique_quantized_colors": 900, "stripey": False}},
        )
        self.assertTrue(result["pass"])
        self.assertIn("absolute_world_gate", result["checks"])
        self.assertIn("white_ratio_not_worse", result["raw_checks"])

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
        # Profile material gates: ignore OCR scene_ok, but still require a
        # non-collapsed frame through absolute_world_gate.
        material = reg.material_visual_checks(raw)
        self.assertTrue(all(material.values()))

    def test_material_gates_reject_absolute_black_even_if_relative_checks_pass(self) -> None:
        current = {
            "world": {
                "white_ratio": 0.0,
                "entropy": 0.0,
                "unique_quantized_colors": 200,
                "stripey": False,
                "scene_ok": False,
            }
        }
        baseline = {
            "world": {
                "white_ratio": 0.0,
                "entropy": 0.0,
                "unique_quantized_colors": 200,
                "stripey": False,
                "scene_ok": False,
            }
        }
        material = reg.material_visual_checks(capture.compare_metrics(current, baseline))
        self.assertFalse(material["absolute_world_gate"])
        self.assertFalse(all(material.values()))

    def test_visual_floor_rejects_solid_baseline_candidate(self) -> None:
        result = reg.visual_floor_from_metrics(
            {
                "world": {
                    "white_ratio": 0.0,
                    "entropy": 0.0,
                    "unique_quantized_colors": 1,
                    "stripey": False,
                }
            }
        )
        self.assertFalse(result["pass"])
        self.assertFalse(result["checks"]["absolute_world_gate"])

    def test_visual_floor_accepts_material_baseline_candidate(self) -> None:
        result = reg.visual_floor_from_metrics(
            {
                "world": {
                    "white_ratio": 0.04,
                    "entropy": 6.8,
                    "unique_quantized_colors": 1000,
                    "stripey": False,
                }
            }
        )
        self.assertTrue(result["pass"])


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
