#!/usr/bin/env python3
"""Unit tests for the strict playable regression profile (no private titles)."""

from __future__ import annotations

import importlib.util
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import kyty_runner_common as runner
from kyty_scene_checkpoint import SceneCheckpoint


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


class CheckpointFixture:
    """Synthetic native PNGs, never gameplay evidence for an actual workload."""
    def __init__(self, root):
        from PIL import Image

        self.root = root
        self.images = {}
        for role, marker_x in (("before", 8), ("after", 32), ("wrong", 48), ("menu", -1)):
            image = Image.new("RGB", (64, 48))
            for y in range(48):
                for x in range(64):
                    rgb = ((x * 7 + y * 3) % 220, (x * 3 + y * 11) % 220, (x * 11 + y * 7) % 220)
                    if role == "menu":
                        rgb = tuple(220 - c for c in rgb)
                    elif marker_x <= x < marker_x + 8 and 20 <= y < 28:
                        rgb = (240, 180, 10)
                    image.putpixel((x, y), rgb)
            path = root / f"{role}.png"
            image.save(path)
            self.images[role] = path
        self.contract = {
            "schema": "kyty_scene_checkpoint_v1", "fixture": "synthetic-room", "kind": "gameplay",
            "before": self.reference("before"), "after": self.reference("after"),
            "scene_regions": [{"box": [0, 0, 1, 0.25], "max_mean_error": 0}],
            "response": {"box": [0, 0.375, 1, 0.625], "max_mean_error": 0,
                         "pixel_delta": 20, "min_changed_ratio": 0.05},
            "action": {"pad_sequence": [{"tool": "pad_down", "button": "right", "hold_s": 0.2},
                                         {"tool": "pad_up", "button": "right"}],
                       "milestones": {"min_pad_taps": 0, "min_guest_read_state_samples": 0,
                                      "min_guest_read_samples": 1},
                       "min_present_delta": 10, "min_settle_s": 0},
        }
        self.path = root / "checkpoint.json"
        self.write()

    def reference(self, role):
        path = self.images[role]
        return {"image": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}

    def write(self):
        self.path.write_text(json.dumps(self.contract), encoding="utf-8")

    def record(self, role, present):
        return {"path": str(self.images[role]), "file": self.images[role].name,
                "sha256": self.reference(role)["sha256"], "present": present, "source": "kyty_agent.capture"}

    def evaluate(self, after="after", *, consumed=True):
        before_input = {"delivered_taps": 0, "guest_read_state_samples": 0, "guest_read_samples": 0}
        after_input = dict(before_input, guest_read_samples=int(consumed))
        action = {"start_present": 10, "end_present": 20, "settled": True,
                  "input_before": before_input, "input_after": after_input,
                  "delivery_checks": reg.input_delivery_checks(before_input, after_input,
                                      self.contract["action"]["milestones"], True)}
        return SceneCheckpoint(self.path, capture).evaluate(self.record("before", 10), self.record(after, 30), action)


class StrictEnvTests(unittest.TestCase):
    def test_playable_environment_strips_forbidden(self) -> None:
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
            env = reg.build_playable_environment(
                base,
                guest_root=Path(td) / "g",
                agent_socket=Path(td) / "a.sock",
                capture_directory=Path(td) / "c",
            )
        for k in runner.STRICT_FORBIDDEN_ENVIRONMENT_KEYS:
            self.assertNotIn(k, env)
        self.assertEqual(runner.find_forbidden_environment_keys(env), [])
        self.assertEqual(env["KYTY_PRINTF_DIRECTION"], "Silent")
        self.assertEqual(env["KYTY_CRASH_REPORT"], str(Path(td) / "crash-context.json"))

    def test_preserves_explicit_runtime_and_driver_configuration(self) -> None:
        requested = {
            "KYTY_SHADER_OPTIMIZATION": "None",
            "KYTY_SHADER_VALIDATION": "1",
            "KYTY_WAIT_TIMEOUT_MS": "0",
            "KYTY_SPIRV_CACHE": "/host-cache/spirv",
            "KYTY_VULKAN_PIPELINE_CACHE": "/host-cache/pipelines.bin",
            "MESA_SHADER_CACHE_DIR": "/host-cache/mesa",
            "MESA_SHADER_CACHE_MAX_SIZE": "256M",
            "MESA_SHADER_CACHE_DISABLE": "false",
            "XDG_CACHE_HOME": "/host-cache/xdg",
            "TMPDIR": "/host-cache/tmp",
            "KYTY_SAVEDATA_DIR": "/host-runtime/save",
            "KYTY_SANDBOX_DIR": "/host-runtime/sandbox",
            "shader_spilling_rate": "1",
            "VK_DRIVER_FILES": "/host-driver/icd.json",
            "KYTY_RENDER_RESOLUTION_MODE": "Native",
        }
        env = reg.build_playable_environment(
            dict(requested, KYTY_PRINTF_DIRECTION="Console"),
            guest_root=Path("/fixture/guest"), agent_socket=Path("/fixture/agent.sock"),
            capture_directory=Path("/fixture/captures"),
        )
        for key, value in requested.items():
            with self.subTest(key=key):
                self.assertEqual(env.get(key), value)
        self.assertEqual(env["KYTY_PRINTF_DIRECTION"], "Silent")

    def test_optional_runtime_configuration_is_not_invented(self) -> None:
        env = reg.build_playable_environment(
            {"KYTY_SHADER_OPTIMIZATION": "", "MESA_SHADER_CACHE_DIR": "", "TMPDIR": ""},
            guest_root=Path("/fixture/guest"), agent_socket=Path("/fixture/agent.sock"),
            capture_directory=Path("/fixture/captures"),
        )
        for key in ["KYTY_SHADER_OPTIMIZATION", "MESA_SHADER_CACHE_DIR", "TMPDIR", "shader_spilling_rate"]:
            self.assertNotIn(key, env)

    def test_shader_probes_and_unrelated_driver_overrides_do_not_leak(self) -> None:
        probes = {key: "1" for key in [
            "KYTY_VS_CLIP_PROBE", "KYTY_PS_MRT_PROBE", "KYTY_TRACE_DRAW_PS",
            "KYTY_PAD_SCRIPT", "INTEL_DEBUG", "LD_PRELOAD", "KYTY_WAIT_TIMEOUT_MODE",
        ]}
        env = reg.build_playable_environment(
            probes, guest_root=Path("/fixture/guest"), agent_socket=Path("/fixture/agent.sock"),
            capture_directory=Path("/fixture/captures"),
        )
        self.assertTrue(probes.keys().isdisjoint(env))


class RuntimePathsTests(unittest.TestCase):
    def make_repo(self, base: Path) -> Path:
        repo = base / "repo"
        (repo / "scripts").mkdir(parents=True)
        (repo / "scripts" / "run_guest.lua").write_text("-- fixture\n", encoding="utf-8")
        return repo

    def test_default_launch_paths_are_absolute(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = self.make_repo(Path(td))
            cwd, script = reg.resolve_runtime_launch_paths(repo)
            self.assertEqual(cwd, repo.resolve())
            self.assertEqual(script, (repo / "scripts" / "run_guest.lua").resolve())

    def test_runtime_and_lua_can_be_outside_repo(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            repo = self.make_repo(base)
            runtime = base / "runtime"
            runtime.mkdir()
            lua = runtime / "preserved.lua"
            lua.write_text("-- preserved\n", encoding="utf-8")
            cwd, script = reg.resolve_runtime_launch_paths(repo, runtime_cwd=runtime, guest_script=lua)
            self.assertEqual(cwd, runtime.resolve())
            self.assertEqual(script, lua.resolve())

    def test_relative_script_stays_repo_relative_with_external_cwd(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            repo = self.make_repo(base)
            runtime = base / "runtime"
            runtime.mkdir()
            cwd, script = reg.resolve_runtime_launch_paths(
                repo, runtime_cwd=runtime, guest_script=Path("scripts/run_guest.lua")
            )
            self.assertEqual(cwd, runtime.resolve())
            self.assertEqual(script, (repo / "scripts" / "run_guest.lua").resolve())

    def test_missing_runtime_directory_is_rejected_without_creation(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = self.make_repo(Path(td))
            runtime = Path(td) / "missing"
            with self.assertRaisesRegex(ValueError, "runtime cwd"):
                reg.resolve_runtime_launch_paths(repo, runtime_cwd=runtime)
            self.assertFalse(runtime.exists())

    def test_missing_lua_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = self.make_repo(Path(td))
            with self.assertRaisesRegex(ValueError, "guest script"):
                reg.resolve_runtime_launch_paths(repo, guest_script=repo / "missing.lua")

    def test_run_session_uses_selected_paths_without_guest_launch(self) -> None:
        class LaunchObserved(Exception):
            pass

        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            repo = self.make_repo(base)
            runtime = base / "runtime"
            runtime.mkdir()
            lua = runtime / "preserved.lua"
            lua.write_text("-- fixture\n", encoding="utf-8")
            fc = runtime / "versioned-fc"
            fc.write_bytes(b"fixture binary")
            with mock.patch.object(reg, "load_capture_module"), mock.patch.object(reg, "remove_stale_unix_socket"), \
                 mock.patch.object(reg, "launch_process_group", side_effect=LaunchObserved) as launch:
                with self.assertRaises(LaunchObserved):
                    reg.run_session(
                        repo_root=repo, guest_root=base / "guest", fc_script=fc, profile=reg.load_profile(None),
                        scratch=base / "scratch", baseline_path=base / "baseline.json", create_baseline=False,
                        runtime_cwd=runtime, guest_script=lua,
                    )
            self.assertEqual(launch.call_args.args[0], [str(fc.resolve()), str(lua.resolve()), str(base / "guest")])
            self.assertEqual(launch.call_args.kwargs["cwd"], runtime.resolve())
            self.assertEqual(launch.call_args.kwargs["env"]["KYTY_PRINTF_DIRECTION"], "Silent")


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
    def test_sigabrt_is_a_host_crash(self) -> None:
        self.assertEqual(reg.classify_exit(-6, False), "host_crash")

    def test_fatal_marker_reports_following_diagnostic(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "child.log"
            log.write_text(
                "--- Fatal Error ---\nNot implemented (shader format != 4)\n in /tmp/source.cpp:1\n",
                encoding="utf-8",
            )
            self.assertEqual(
                reg.first_actionable_from_log(log),
                "Not implemented (shader format != 4)",
            )

    def test_kill_timeout_is_reported_without_raising(self) -> None:
        class Process:
            pid = 123

            @staticmethod
            def poll():
                return None

            @staticmethod
            def wait(timeout: int):
                raise subprocess.TimeoutExpired("guest", timeout)

        notes: list[str] = []
        with mock.patch.object(reg.os, "killpg"):
            runner.terminate_process_group(Process(), notes)
        self.assertIn("kill_timeout_after_sigkill", notes)

    def test_material_delivery_without_checkpoint_is_incomplete(self) -> None:
        gates = reg.evaluate_gates(
            agent_ready=True,
            video_initialized=True,
            first_present=10,
            present_delta=20,
            input_before={"delivered_taps": 0, "guest_read_state_samples": 0},
            input_after={"delivered_taps": 2, "guest_read_state_samples": 5},
            capture_path="frame.png",
            compare={"pass": True, "checks": {}},
            baseline_missing=False,
            create_baseline=False,
            shutdown="coordinator_stop",
            first_error="",
            milestones={"min_present_after_ready": 1, "present_delta_stable": 15, "min_pad_taps": 2},
            visual_require_baseline=True,
        )
        # Good health/delivery is deliberately incomplete without a fixture.
        by = {g.name: g for g in gates}
        self.assertTrue(all(g.passed for g in gates if g.name not in ("scene_checkpoint", "action_response")))
        self.assertFalse(by["scene_checkpoint"].passed)
        self.assertFalse(by["action_response"].passed)

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
        self.assertFalse(by["no_runtime_failure"].passed)

    def test_baseline_missing_fails_visual(self) -> None:
        gates = reg.evaluate_gates(
            agent_ready=True,
            video_initialized=True,
            first_present=5,
            present_delta=20,
            input_before={"delivered_taps": 0},
            input_after={"delivered_taps": 1},
            capture_path="x.png",
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
            capture_path="frame.png",
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
            capture_path="frame.png",
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
    def test_each_hold_requires_its_own_consumed_guest_sample(self):
        now = [0.0]
        down_count = [0]
        reads = [0]
        def call(_sock, tool, _args, timeout):
            self.assertGreater(timeout, 0)
            if tool == "pad_down":
                down_count[0] += 1
            if tool == "status":
                if down_count[0] == 1:
                    reads[0] += 1
                return 0, {"result": {"pad": {"guest_read_samples": reads[0]}}}
            return 0, {"ok": True}
        ok, events = reg.deliver_pad_sequence(Path("test.sock"), [
            {"tool": "pad_down", "button": "right", "hold_s": 0.1},
            {"tool": "pad_up", "button": "right"},
            {"tool": "pad_down", "button": "left", "hold_s": 0.1}],
            call=call, clock=lambda: now[0], pause=lambda seconds: now.__setitem__(0, now[0] + seconds))
        self.assertFalse(ok)
        holds = [event for event in events if event["event"] == "hold_observed"]
        self.assertEqual([event["ok"] for event in holds], [True, False])
        self.assertEqual(events[-1]["event"], "pad_clear")

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

    def test_sequence_observes_loading_during_inter_tap_settle(self) -> None:
        now = [0.0]
        observed_phases: list[str] = []
        post_input = reg.PostInputWaitState()

        def call(_sock: Path, tool: str, _args: dict[str, object], timeout: float):
            del timeout
            if tool == "status":
                phase = "loading" if now[0] >= 0.25 else "interactive"
                return 0, {
                    "ok": True,
                    "result": {
                        "phase": phase,
                        "present": int(now[0] * 100),
                        "pad": {"tap_pending": False},
                    },
                }
            return 0, {"ok": True}

        ok, _events = reg.deliver_pad_sequence(
            Path("/tmp/test.sock"),
            [
                {"tool": "pad_tap", "button": "cross"},
                {"tool": "pad_tap", "button": "cross"},
            ],
            call=call,
            pause=lambda seconds: now.__setitem__(0, now[0] + seconds),
            clock=lambda: now[0],
            inter_tap_settle_s=0.5,
            status_observer=lambda status: (
                observed_phases.append(str(status.get("phase") or "")),
                reg.record_input_phase_observation(post_input, str(status.get("phase") or "")),
            ),
        )

        self.assertTrue(ok)
        self.assertIn("loading", observed_phases)
        self.assertTrue(post_input.loading_seen)
        self.assertIsNone(post_input.interactive_since)
        self.assertIsNone(post_input.interactive_start_present)
        self.assertFalse(
            reg.advance_post_input_wait(post_input, True, "interactive", 1000, now[0], 240, 15.0)
        )

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

    def test_startup_input_uses_progress_not_fps_phase(self) -> None:
        self.assertTrue(reg.can_start_pad_sequence(True, 1, "loading", True, 5.0, 10.0, 5.0))
        self.assertTrue(reg.can_start_pad_sequence(True, 1, "booting", True, 5.0, 10.0, 5.0))
        self.assertFalse(reg.can_start_pad_sequence(True, 1, "interactive", True, 4.9, 10.0, 5.0))
        self.assertTrue(reg.can_start_pad_sequence(True, 1, "interactive", True, 5.0, 10.0, 5.0))

    def test_post_input_wait_can_use_bounded_settle_when_loading_label_is_missed(self) -> None:
        state = reg.PostInputWaitState()
        ready = reg.advance_post_input_wait(state, False, "interactive", 100, 0.0, 120, 5.0)
        self.assertFalse(ready)

        ready = reg.advance_post_input_wait(state, False, "interactive", 219, 5.0, 120, 5.0)
        self.assertFalse(ready)

        ready = reg.advance_post_input_wait(state, False, "interactive", 220, 5.0, 120, 5.0)
        self.assertTrue(ready)

    def test_post_input_wait_does_not_treat_fps_loading_as_scene_identity(self) -> None:
        state = reg.PostInputWaitState()
        ready = reg.advance_post_input_wait(state, True, "interactive", 200, 10.0, 120, 5.0)
        self.assertFalse(ready)

        ready = reg.advance_post_input_wait(state, True, "loading", 201, 10.1, 120, 5.0)
        self.assertFalse(state.loading_seen)
        self.assertFalse(ready)

        ready = reg.advance_post_input_wait(state, True, "interactive", 202, 10.2, 120, 5.0)
        self.assertFalse(ready)

        ready = reg.advance_post_input_wait(state, True, "interactive", 319, 15.2, 120, 5.0)
        self.assertFalse(ready)

        ready = reg.advance_post_input_wait(state, True, "interactive", 322, 15.2, 120, 5.0)
        self.assertTrue(ready)

    def test_post_input_wait_ignores_fps_fluctuations_but_resets_on_counter_reset(self) -> None:
        state = reg.PostInputWaitState(loading_seen=True)
        self.assertFalse(reg.advance_post_input_wait(state, True, "interactive", 100, 1.0, 10, 2.0))
        self.assertFalse(reg.advance_post_input_wait(state, True, "loading", 109, 2.9, 10, 2.0))
        self.assertTrue(reg.advance_post_input_wait(state, True, "loading", 110, 3.0, 10, 2.0))
        self.assertFalse(reg.advance_post_input_wait(state, True, "interactive", 1, 4.0, 10, 2.0))
        self.assertTrue(reg.advance_post_input_wait(state, True, "loading", 11, 6.0, 10, 2.0))


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


class AcceptanceDefectTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.fixture = CheckpointFixture(Path(self.tmp.name))
        self.base = dict(agent_ready=True, video_initialized=True, first_present=1, present_delta=300,
                         input_before={"delivered_taps": 0, "guest_read_state_samples": 0},
                         input_after={"delivered_taps": 3, "guest_read_state_samples": 6},
                         capture_path="synthetic.png", compare={"pass": True}, baseline_missing=False,
                         create_baseline=False, first_error="", milestones=reg.DEFAULT_PROFILE["milestones"],
                         visual_require_baseline=True, checkpoint=self.fixture.evaluate(), shutdown="guest_exit_ok")

    def gates(self, **overrides):
        return {g.name: g for g in reg.evaluate_gates(**dict(self.base, **overrides))}

    def test_retained_audit_negative_controls_with_valid_independent_scene(self):
        # Same pure inputs as audit-F, plus an actually computed scene oracle.
        self.assertTrue(all(g.passed for g in self.gates().values()))
        for rc in (-6, -11, -4, -8, -7, -15, -9, 139, 132, 134, 136):
            for timed_out in (False, True):
                with self.subTest(rc=rc, timed_out=timed_out):
                    gates = self.gates(shutdown=reg.classify_exit(rc, timed_out), timed_out=timed_out)
                    self.assertFalse(gates["no_host_crash"].passed)
                    self.assertFalse(gates["expected_exit"].passed)
        gates = self.gates(first_error="VK_ERROR_DEVICE_LOST after presentation")
        self.assertFalse(gates["no_runtime_failure"].passed)

    def test_unexpected_returns_unknown_and_timeout_fail(self):
        for rc in (1, 7, 143, 137, None):
            self.assertFalse(self.gates(shutdown=reg.classify_exit(rc, False))["expected_exit"].passed)
        self.assertEqual(reg.classify_exit(0, False, coordinator_stop=True), "coordinator_stop")
        self.assertEqual(reg.classify_exit(-15, True, coordinator_stop=True), "controlled_timeout")
        self.assertFalse(self.gates(shutdown="controlled_timeout")["within_deadline"].passed)
        self.assertFalse(self.gates(timed_out=True)["within_deadline"].passed)
        self.assertFalse(self.gates(final_error_observed=False)["final_error_observed"].passed)
        for rc in (-11, -4, -8, 7):
            self.assertNotEqual(reg.classify_exit(rc, True, coordinator_stop=True), "controlled_timeout")

    def test_healthy_menu_and_accepted_flags_cannot_self_certify_gameplay(self):
        for checkpoint in (None, {"accepted": True, "pass": True}):
            gates = self.gates(checkpoint=checkpoint, baseline_missing=True, create_baseline=True)
            self.assertTrue(gates["visual_compare"].passed)
            self.assertFalse(gates["scene_checkpoint"].passed)
            self.assertFalse(gates["action_response"].passed)
        self.fixture.contract["kind"] = "menu"
        self.fixture.write()
        self.assertFalse(self.gates(checkpoint=self.fixture.evaluate())["scene_checkpoint"].passed)

    def test_reference_hashes_and_distinct_expected_response_are_required(self):
        self.fixture.contract["before"]["sha256"] = "0" * 64
        self.fixture.write()
        with self.assertRaisesRegex(ValueError, "hash"):
            SceneCheckpoint(self.fixture.path, capture)
        self.fixture.contract["before"] = self.fixture.reference("before")
        self.fixture.contract["after"] = self.fixture.reference("before")
        self.fixture.write()
        with self.assertRaisesRegex(ValueError, "distinguish"):
            SceneCheckpoint(self.fixture.path, capture)

    def test_changed_image_without_expected_response_or_input_does_not_pass(self):
        for role in ("before", "wrong", "menu"):
            self.assertFalse(self.gates(checkpoint=self.fixture.evaluate(role))["action_response"].passed)
        self.assertFalse(self.gates(checkpoint=self.fixture.evaluate(consumed=False))["action_response"].passed)

    def test_capture_binding_rejects_stale_hash_and_out_of_order_input(self):
        checkpoint = SceneCheckpoint(self.fixture.path, capture)
        good = self.fixture.evaluate()
        before = self.fixture.record("before", 10)
        after = self.fixture.record("after", 30)
        before["sha256"] = "0" * 64
        self.assertFalse(checkpoint.evaluate(before, after, good["action"])["checks"]["before_scene"])
        before = self.fixture.record("before", 10)
        good["action"]["start_present"] = 5
        self.assertFalse(checkpoint.evaluate(before, after, good["action"])["checks"]["capture_input_order"])

    def test_native_response_must_be_fresh_png_inside_this_run(self):
        capture_dir = self.fixture.root / "captures"
        capture_dir.mkdir()
        local = capture_dir / "native.png"
        local.write_bytes(self.fixture.images["before"].read_bytes())
        result = {"path": str(local), "present": 11, "image_format": "PNG"}
        self.assertEqual(reg.retain_native_capture(result, capture_dir, "before", 10)["present"], 11)
        for changed in ({"present": 10}, {"path": str(self.fixture.images["before"])}, {"image_format": "JPEG"}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                reg.retain_native_capture(dict(result, **changed), capture_dir, "invalid", 10)

    def test_zero_taps_and_zero_state_reads_allow_consumed_read_holds(self):
        milestones = {"min_pad_taps": 0, "min_guest_read_state_samples": 0, "min_guest_read_samples": 1}
        self.assertTrue(all(reg.input_delivery_checks({}, {"guest_read_samples": 3}, milestones, True).values()))
        self.assertFalse(all(reg.input_delivery_checks({}, {}, milestones, True).values()))
        self.assertFalse(all(reg.input_delivery_checks({}, {"guest_read_samples": 3, "delivered_taps": 1},
                                                      milestones, True).values()))
        self.assertFalse(all(reg.input_delivery_checks({"guest_read_samples": 4}, {"guest_read_samples": 3},
                                                      milestones, True).values()))

    def test_error_events_without_code_are_actionable_but_info_codes_are_not(self):
        self.assertEqual(reg.first_actionable_error([{"kind": "fatal", "message": "device lost"}], {}), "device lost")
        self.assertEqual(reg.first_actionable_error([{"kind": "info", "code": "first_present"}], {}), "")


class SessionOrchestrationTests(unittest.TestCase):
    """Exercise run_session and real process-boundary wiring with fake Popen/agent."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.fixture = CheckpointFixture(self.root)
        self.binary = self.root / "fake-fc"
        self.binary.write_bytes(b"not executable: Popen is replaced")
        self.baseline = self.root / "baseline.json"
        self.now = 0.0
        self.present = 0
        self.taps = self.states = self.reads = self.captures = self.clears = 0
        self.held = False
        self.consume_hold = True
        self.stop_return = 0
        self.crash_on_capture = None
        self.timeout_on_capture = False
        self.error_stage = ""
        self.error = None
        self.events_only = False
        self.unavailable_after_capture = False
        self.fail_action_clear = False
        self.natural_exit = False
        self.final_event_calls = 0
        self.before_role = "before"
        self.after_role = "after"
        self.proc = mock.Mock(pid=12345, returncode=None)
        self.proc.poll.side_effect = lambda: self.proc.returncode
        self.proc.wait.side_effect = lambda timeout: self.proc.returncode
        self.profile = reg.load_profile(None)
        self.profile["deadlines_s"].update(total=30, input_window=5)
        self.profile["milestones"]["present_delta_stable"] = 10
        self.profile["input"] = {"menu_settle_s": 0, "inter_tap_settle_s": 0}
        self.profile["post_input"] = {"require_loading_transition": True, "min_present_delta": 0, "min_settle_s": 0}

    def pause(self, seconds):
        self.now += seconds

    def launch(self, *args, **kwargs):
        self.capture_dir = Path(kwargs["env"]["KYTY_NATIVE_CAPTURE_DIR"])
        self.child_log = Path(kwargs["stdout"].name)
        return self.proc

    def stop(self, _pid, _signal):
        self.proc.returncode = self.stop_return

    def agent(self, _sock, tool, args=None, timeout=1):
        self.assertGreater(timeout, 0)
        result = {}
        if self.proc.returncode is not None:
            return 125, {"ok": False}
        if tool == "status":
            self.present += 10
            if self.held and self.consume_hold:
                self.reads += 1
            # Deliberately low-FPS loading throughout: it is not a scene oracle.
            result = {"present": self.present, "frame": self.present, "graphic_ready": True,
                      "phase": "loading", "fps": 1,
                      "pad": {"delivered_taps": self.taps, "guest_read_state_samples": self.states,
                              "guest_read_samples": self.reads, "tap_pending": False}}
        elif tool == "pad_tap":
            self.taps += 1
            self.states += 2
        elif tool == "pad_down":
            self.held = True
        elif tool in ("pad_up", "pad_clear"):
            self.held = False
            if tool == "pad_clear":
                self.clears += 1
                if self.error_stage == "post_input":
                    self.error = {"kind": "fatal", "code": "late_input_failure"}
                if self.fail_action_clear and self.clears == 2:
                    return 1, {"ok": False}
        elif tool == "capture":
            self.captures += 1
            self.present += 1
            path = self.capture_dir / f"native-{self.captures}.png"
            role = self.before_role if self.captures == 1 else self.after_role
            path.write_bytes(self.fixture.images[role].read_bytes())
            result = {"path": str(path), "present": self.present, "frame": self.present, "image_format": "PNG"}
            if self.captures == 2 and self.error_stage == "post_capture":
                self.error = {"kind": "fatal", "code": "late_capture_failure"}
            if self.crash_on_capture is not None:
                self.proc.returncode = self.crash_on_capture
            if self.timeout_on_capture:
                self.now = 31
        elif tool in ("last_error", "events"):
            if self.captures == 2 and self.unavailable_after_capture:
                return 125, {"ok": False}
            if tool == "last_error":
                result = {"event": None if self.events_only else self.error}
            else:
                result = {"events": [self.error] if self.error else [{"kind": "info", "code": "first_present"}]}
                if self.captures == 2:
                    self.final_event_calls += 1
                    if self.natural_exit and self.final_event_calls == 2:
                        self.proc.returncode = 0
        return 0, {"ok": True, "result": result}

    def run_fake(self, *, checkpoint=True, score=None, create_baseline=True):
        with mock.patch.object(runner.subprocess, "Popen", side_effect=self.launch) as popen, \
             mock.patch.object(runner.os, "killpg", side_effect=self.stop), \
             mock.patch.object(reg, "call_agent", side_effect=self.agent), \
             mock.patch.object(reg.time, "monotonic", side_effect=lambda: self.now), \
             mock.patch.object(reg.time, "sleep", side_effect=self.pause), \
             mock.patch.object(reg, "agent_sock_path", return_value=self.root / "agent.sock"), \
             mock.patch.object(reg, "load_capture_module", return_value=capture), \
             mock.patch.object(capture, "score_image", side_effect=score or capture.score_image):
            rc, report = reg.run_session(repo_root=Path(__file__).resolve().parents[1], guest_root=self.root / "guest",
                fc_script=self.binary, profile=self.profile, scratch=self.root / "scratch", baseline_path=self.baseline,
                create_baseline=create_baseline, scene_checkpoint_path=self.fixture.path if checkpoint else None)
        self.assertEqual(popen.call_count, 1)
        self.assertTrue(popen.call_args.kwargs["start_new_session"])
        self.assertEqual(report.child_exit, self.proc.returncode)
        summary_path = next((self.root / "scratch").glob("run_*/summary.json"))
        self.assertEqual(json.loads(summary_path.read_text())["child_exit"], self.proc.returncode)
        return rc, report

    def test_native_scene_action_and_coordinator_zero_return_are_not_natural_exit(self):
        rc, report = self.run_fake()
        self.assertEqual(rc, 0, report.to_sanitized_dict())
        self.assertEqual(report.shutdown, "coordinator_stop")
        self.assertTrue(report.coordinator_stop_requested)
        self.assertEqual(self.captures, 2)
        self.assertEqual(self.clears, 2)
        self.assertTrue(all(report.checkpoint["checks"].values()))
        self.assertTrue(self.baseline.is_file())

    def test_existing_material_baseline_still_requires_an_independent_checkpoint(self):
        metrics = capture.score_image(self.fixture.images["after"])
        retained = json.dumps({"world": metrics["world"]})
        self.baseline.write_text(retained, encoding="utf-8")
        rc, report = self.run_fake(checkpoint=False, create_baseline=False)
        self.assertEqual(rc, 1)
        self.assertTrue(next(g for g in report.gates if g.name == "visual_compare").passed)
        self.assertFalse(next(g for g in report.gates if g.name == "scene_checkpoint").passed)
        self.assertEqual(self.baseline.read_text(), retained)

    def test_existing_baseline_with_scene_and_action_can_pass(self):
        metrics = capture.score_image(self.fixture.images["after"])
        retained = json.dumps({"world": metrics["world"]})
        self.baseline.write_text(retained, encoding="utf-8")
        rc, report = self.run_fake(create_baseline=False)
        self.assertEqual(rc, 0, report.to_sanitized_dict())
        self.assertIn("visual_compare_done", report.notes)
        self.assertEqual(self.baseline.read_text(), retained)

    def test_zero_tap_startup_hold_is_accepted_only_with_guest_reads(self):
        self.profile["pad_sequence"] = self.fixture.contract["action"]["pad_sequence"]
        self.profile["milestones"].update(min_pad_taps=0, min_guest_read_state_samples=0, min_guest_read_samples=1)
        rc, report = self.run_fake()
        self.assertEqual(rc, 0, report.to_sanitized_dict())
        self.assertEqual(report.input_after["delivered_taps"], 0)
        self.assertGreater(report.input_after["guest_read_samples"], report.input_before["guest_read_samples"])

    def test_natural_exit_is_retained_separately(self):
        self.natural_exit = True
        rc, report = self.run_fake()
        self.assertEqual(rc, 0, report.to_sanitized_dict())
        self.assertEqual(report.shutdown, "guest_exit_ok")
        self.assertFalse(report.coordinator_stop_requested)
        self.assertEqual(report.stop_reason, "natural_exit")

    def test_coordinator_sigterm_is_recorded_with_the_actual_negative_return(self):
        self.stop_return = -15
        rc, report = self.run_fake()
        self.assertEqual(rc, 0, report.to_sanitized_dict())
        self.assertEqual(report.shutdown, "coordinator_stop")
        self.assertEqual(report.child_exit, -15)

    def test_post_input_error_survives_later_capture_and_does_not_replace_baseline(self):
        self.error_stage = "post_input"
        self.baseline.write_text("retained baseline", encoding="utf-8")
        rc, report = self.run_fake()
        self.assertEqual(rc, 1)
        self.assertEqual(report.first_error, "late_input_failure")
        self.assertEqual(self.captures, 2)
        self.assertEqual(self.baseline.read_text(), "retained baseline")

    def test_error_only_in_post_capture_events_is_retained(self):
        self.error_stage = "post_capture"
        self.events_only = True
        rc, report = self.run_fake()
        self.assertEqual(rc, 1)
        self.assertEqual(report.first_error, "late_capture_failure")
        self.assertFalse(self.baseline.exists())

    def test_error_after_scoring_is_collected_at_final_observation(self):
        real_score = capture.score_image
        def score(path):
            result = real_score(path)
            self.error = {"kind": "fatal", "code": "after_score_failure"}
            return result
        rc, report = self.run_fake(score=score)
        self.assertEqual(rc, 1)
        self.assertEqual(report.first_error, "after_score_failure")

    def test_late_log_error_is_retained_when_agent_has_no_error(self):
        real_score = capture.score_image
        def score(path):
            result = real_score(path)
            self.child_log.write_text("--- Fatal Error ---\nlate log failure\n", encoding="utf-8")
            return result
        rc, report = self.run_fake(score=score)
        self.assertEqual(rc, 1)
        self.assertEqual(report.first_error, "late log failure")

    def test_deadline_does_not_mask_real_child_signal(self):
        self.timeout_on_capture = True
        self.crash_on_capture = -11
        rc, report = self.run_fake()
        self.assertEqual(rc, 1)
        self.assertTrue(report.timed_out)
        self.assertEqual(report.shutdown, "host_crash")
        self.assertEqual(report.child_exit, -11)
        self.assertFalse(report.coordinator_stop_requested)

    def test_timeout_with_coordinator_zero_return_is_still_a_failed_timeout(self):
        self.timeout_on_capture = True
        rc, report = self.run_fake()
        self.assertEqual(rc, 1)
        self.assertTrue(report.timed_out)
        self.assertEqual(report.child_exit, 0)
        self.assertEqual(report.shutdown, "controlled_timeout")
        self.assertEqual(report.stop_reason, "deadline")

    def test_coordinator_shutdown_cannot_hide_racing_crash_or_nonzero_exit(self):
        self.stop_return = -8
        rc, report = self.run_fake()
        self.assertEqual(rc, 1)
        self.assertEqual(report.shutdown, "host_crash")
        self.assertFalse(self.baseline.exists())

    def test_unexpected_exit_after_capture_rejects_healthy_observation(self):
        self.stop_return = 7
        rc, report = self.run_fake()
        self.assertEqual(rc, 1)
        self.assertEqual(report.shutdown, "exit_7")

    def test_missing_final_error_observation_is_not_clean(self):
        self.unavailable_after_capture = True
        rc, report = self.run_fake()
        self.assertEqual(rc, 1)
        self.assertFalse(next(g for g in report.gates if g.name == "final_error_observed").passed)

    def test_no_checkpoint_menu_baseline_is_rejected(self):
        self.before_role = "menu"
        rc, report = self.run_fake(checkpoint=False)
        self.assertEqual(rc, 1)
        self.assertTrue(next(g for g in report.gates if g.name == "visual_compare").passed)
        self.assertFalse(next(g for g in report.gates if g.name == "scene_checkpoint").passed)
        self.assertFalse(self.baseline.exists())

    def test_wrong_before_scene_does_not_run_fixture_action(self):
        self.before_role = "menu"
        rc, report = self.run_fake()
        self.assertEqual(rc, 1)
        self.assertEqual(self.clears, 1)
        self.assertFalse(report.checkpoint["checks"]["before_scene"])

    def test_invalid_reference_hash_never_becomes_a_baseline(self):
        self.fixture.contract["after"]["sha256"] = "0" * 64
        self.fixture.write()
        rc, report = self.run_fake()
        self.assertEqual(rc, 1)
        self.assertIn("checkpoint_invalid", report.notes)
        self.assertFalse(self.baseline.exists())

    def test_scene_change_does_not_hide_failed_action_clear(self):
        self.fail_action_clear = True
        rc, report = self.run_fake()
        self.assertEqual(rc, 1)
        self.assertFalse(report.checkpoint["checks"]["action_delivered"])

    def test_unconsumed_hold_cannot_be_certified_by_later_image_change(self):
        self.consume_hold = False
        rc, report = self.run_fake()
        self.assertEqual(rc, 1)
        self.assertTrue(report.checkpoint["checks"]["expected_response"])
        self.assertFalse(report.checkpoint["checks"]["action_delivered"])

    def test_animation_other_than_expected_response_fails(self):
        self.after_role = "wrong"
        rc, report = self.run_fake()
        self.assertEqual(rc, 1)
        self.assertFalse(report.checkpoint["checks"]["expected_response"])


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
