#!/usr/bin/env python3
"""
Strict playable regression profile (orchestration only).

Launches the standard guest path (fc_script + scripts/run_guest.lua), observes
via existing kyty_agent tools, delivers diagnostic pad edges, captures a native
frame, and compares metrics against an untracked local baseline.

Does not alter emulator behavior. Guest root comes only from env/CLI
(KYTY_GUEST_ROOT). Tracked outputs never embed absolute private guest paths or
title identifiers.

Exit codes:
  0  — all gates passed
  1  — playable gate failed (blocker for subsequent runtime work)
  2  — usage / missing tooling / missing guest root (when required)
  3  — baseline missing and --create-baseline not set (visual gate incomplete)
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

from kyty_scene_checkpoint import SceneCheckpoint

from kyty_runner_common import (
    build_child_environment,
    call_agent,
    find_forbidden_environment_keys,
    launch_process_group,
    remove_stale_unix_socket,
    terminate_process_group,
)


DEFAULT_PROFILE: dict[str, Any] = {
    "schema": "kyty_playable_regression_profile_v1",
    "mode": "strict",
    "deadlines_s": {
        "agent_ready": 60,
        "first_present": 120,
        "present_stability": 25,
        "input_window": 30,
        "total": 180,
    },
    "milestones": {
        "min_present_after_ready": 1,
        "present_delta_stable": 15,
        "min_pad_taps": 3,
        "min_guest_read_state_samples": 1,
    },
    "pad_sequence": [
        {"tool": "pad_tap", "button": "cross"},
        {"tool": "pad_tap", "button": "cross"},
        {"tool": "pad_tap", "button": "cross"},
    ],
    "input": {"menu_settle_s": 5, "inter_tap_settle_s": 3},
    "post_input": {
        # Legacy phase hint retained in profiles for diagnostics only.
        "require_loading_transition": True,
        "min_present_delta": 240,
        "min_settle_s": 15,
    },
    "visual": {"require_baseline": True, "require_capture": True},
}

MATERIAL_VISUAL_CHECKS = (
    "white_ratio_not_worse",
    "entropy_not_collapsed",
    "colors_not_collapsed",
    "not_stripey",
    "absolute_world_gate",
)


def load_capture_module(repo_root: Path) -> Any:
    path = repo_root / "scripts" / "kyty_capture.py"
    spec = importlib.util.spec_from_file_location("kyty_capture", path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules["kyty_capture"] = mod
    spec.loader.exec_module(mod)
    return mod


def load_profile(path: Optional[Path]) -> dict[str, Any]:
    if path is None:
        return json.loads(json.dumps(DEFAULT_PROFILE))
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("profile must be a JSON object")
    # Merge over defaults so partial profiles work
    out = json.loads(json.dumps(DEFAULT_PROFILE))
    for key, val in data.items():
        if isinstance(val, dict) and isinstance(out.get(key), dict):
            out[key].update(val)
        else:
            out[key] = val
    return out


def build_playable_environment(
    base: dict[str, str],
    *,
    guest_root: Path,
    agent_socket: Path,
    capture_directory: Path,
) -> dict[str, str]:
    """Build the strict playable child policy on shared runner primitives."""
    env = build_child_environment(
        base,
        guest_root=guest_root,
        agent_socket=agent_socket,
        capture_directory=capture_directory,
        optional_values=(
            "KYTY_RENDER_RESOLUTION_MODE",
            "KYTY_RENDER_RESOLUTION_WIDTH",
            "KYTY_RENDER_RESOLUTION_HEIGHT",
            "KYTY_PRESENTATION_FILTER",
            # Preserve explicit strict runtime and writable-path choices; do
            # not silently switch optimizer, driver cache or filesystem.
            "KYTY_SHADER_OPTIMIZATION",
            "KYTY_SHADER_VALIDATION",
            "KYTY_WAIT_TIMEOUT_MS",
            "KYTY_SPIRV_CACHE",
            "KYTY_VULKAN_PIPELINE_CACHE",
            "KYTY_SAVEDATA_DIR",
            "KYTY_SANDBOX_DIR",
            "XDG_CACHE_HOME",
            "TMPDIR",
            "MESA_SHADER_CACHE_DIR",
            "MESA_SHADER_CACHE_MAX_SIZE",
            "MESA_SHADER_CACHE_DISABLE",
            "shader_spilling_rate",
        ),
        default_values={"KYTY_NATIVE_CAPTURE_MAX_EDGE": "1280"},
    )
    env["KYTY_CRASH_REPORT"] = str(capture_directory.parent / "crash-context.json")
    return env


def agent_sock_path(run_id: str) -> Path:
    digest = hashlib.sha256(run_id.encode("utf-8")).hexdigest()[:12]
    return Path(f"/tmp/kyty_pr_{digest}.sock")


def resolve_runtime_launch_paths(
    repo_root: Path,
    *,
    runtime_cwd: Optional[Path] = None,
    guest_script: Optional[Path] = None,
) -> tuple[Path, Path]:
    """Keep the Lua input independent of the writable runtime directory."""
    repo_root = repo_root.resolve()

    def resolve(path: Path) -> Path:
        path = path.expanduser()
        return (path if path.is_absolute() else repo_root / path).resolve()

    cwd = resolve(runtime_cwd if runtime_cwd is not None else repo_root)
    script = resolve(guest_script if guest_script is not None else Path("scripts/run_guest.lua"))
    if not cwd.is_dir():
        raise ValueError("runtime cwd is not a directory")
    if not script.is_file():
        raise ValueError("guest script is not a file")
    return cwd, script




def extract_result(obj: dict[str, Any]) -> dict[str, Any]:
    if isinstance(obj.get("result"), dict):
        return obj["result"]
    return obj


def first_actionable_error(events: list[dict[str, Any]], last_error: dict[str, Any]) -> str:
    if last_error:
        code = str(last_error.get("code") or last_error.get("message") or "").strip()
        if code and code.lower() not in ("none", "null", ""):
            return code[:160]
        if str(last_error.get("kind") or "").lower() in ("error", "fatal"):
            return str(last_error.get("message") or last_error["kind"])[:160]
    for ev in events:
        if not isinstance(ev, dict):
            continue
        kind = str(ev.get("kind") or "").lower()
        code = str(ev.get("code") or "").strip()
        if kind in ("error", "fatal"):
            return (code or str(ev.get("message") or kind))[:160]
    return ""


def first_actionable_from_log(log_path: Path) -> str:
    if not log_path.is_file():
        return ""
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        s = line.strip()
        if s in ("--- Error ---", "--- Fatal Error ---"):
            for diagnostic in lines[index + 1 :]:
                diagnostic = diagnostic.strip()
                if not diagnostic or diagnostic.startswith("---") or diagnostic.startswith(" in /"):
                    continue
                return diagnostic[:160]
        if not s or s.startswith("---") or s.startswith("["):
            continue
        if "/home/" in s or "Documents/" in s or s.startswith(" in /"):
            continue
        if s.startswith("===") and s.endswith("==="):
            return s.strip("= ").strip()[:160]
        if "assert" in s.lower() or "fatal" in s.lower() or "unpatched" in s.lower():
            return s[:160]
    return ""


def sanitize_text(text: str, guest_root: Optional[Path] = None) -> str:
    out = text
    if guest_root is not None:
        out = out.replace(str(guest_root), "$KYTY_GUEST_ROOT")
        try:
            out = out.replace(str(guest_root.resolve()), "$KYTY_GUEST_ROOT")
        except OSError:
            pass
    for marker in ("/home/", "Documents/PS5", "PPSA", "CUSA"):
        if marker in out:
            # blunt redaction of absolute homes and title-ish tokens in free text
            out = out.replace(marker, "<redacted>")
    return out


def sanitize_obj(obj: Any, guest_root: Optional[Path] = None) -> Any:
    if isinstance(obj, dict):
        return {k: sanitize_obj(v, guest_root) for k, v in obj.items()}
    if isinstance(obj, list):
        return [sanitize_obj(v, guest_root) for v in obj]
    if isinstance(obj, str):
        return sanitize_text(obj, guest_root)
    return obj


@dataclass
class GateResult:
    name: str
    passed: bool
    detail: str = ""


@dataclass
class PostInputWaitState:
    loading_seen: bool = False
    interactive_since: Optional[float] = None
    interactive_start_present: Optional[int] = None


@dataclass
class RunReport:
    gates: list[GateResult] = field(default_factory=list)
    timeline: list[dict[str, Any]] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    first_error: str = ""
    first_present: Optional[int] = None
    present_delta: int = 0
    input_before: dict[str, Any] = field(default_factory=dict)
    input_after: dict[str, Any] = field(default_factory=dict)
    capture_path: str = ""
    compare: dict[str, Any] = field(default_factory=dict)
    child_exit: Optional[int] = None
    shutdown: str = ""
    timed_out: bool = False
    stop_reason: str = ""
    coordinator_stop_requested: bool = False
    checkpoint: dict[str, Any] = field(default_factory=dict)
    profile: dict[str, Any] = field(default_factory=dict)

    def all_passed(self) -> bool:
        return bool(self.gates) and all(g.passed for g in self.gates)

    def to_sanitized_dict(self, guest_root: Optional[Path] = None) -> dict[str, Any]:
        payload = {
            "schema": "kyty_playable_regression_summary_v1",
            "mode": "strict",
            "protocol": "kyty_agent",
            "passed": self.all_passed(),
            "gates": [{"name": g.name, "passed": g.passed, "detail": g.detail} for g in self.gates],
            "first_error": self.first_error,
            "first_present": self.first_present,
            "present_delta": self.present_delta,
            "input_before": self.input_before,
            "input_after": self.input_after,
            "capture": self.capture_path,
            "compare": self.compare,
            "child_exit": self.child_exit,
            "shutdown": self.shutdown,
            "timed_out": self.timed_out,
            "stop_reason": self.stop_reason,
            "coordinator_stop_requested": self.coordinator_stop_requested,
            "checkpoint": self.checkpoint,
            "timeline": self.timeline,
            "notes": self.notes,
            "profile_deadlines_s": self.profile.get("deadlines_s", {}),
            "profile_milestones": self.profile.get("milestones", {}),
            "profile_input": self.profile.get("input", {}),
            "profile_post_input": self.profile.get("post_input", {}),
        }
        return sanitize_obj(payload, guest_root)


def pad_counters(status: dict[str, Any]) -> dict[str, Any]:
    pad = status.get("pad") if isinstance(status.get("pad"), dict) else {}
    return {
        "delivered_taps": int(pad.get("delivered_taps") or 0),
        "guest_read_state_samples": int(pad.get("guest_read_state_samples") or 0),
        "guest_read_samples": int(pad.get("guest_read_samples") or 0),
        "tap_pending": bool(pad.get("tap_pending")),
    }


def deadline_timeout(deadline: float, cap: float, *, clock: Any = None) -> float:
    clock = clock or time.monotonic
    return max(0.0, min(cap, deadline - clock()))


def deliver_pad_sequence(
    sock: Path,
    steps: list[dict[str, Any]],
    *,
    call: Any = None,
    pause: Any = None,
    deadline: Optional[float] = None,
    clock: Any = None,
    inter_tap_settle_s: float = 0.0,
    status_observer: Any = None,
) -> tuple[bool, list[dict[str, Any]]]:
    call = call or call_agent
    pause = pause or time.sleep
    clock = clock or time.monotonic
    events: list[dict[str, Any]] = []
    success = True
    supported = frozenset(("pad_tap", "pad_down", "pad_up"))
    action_deadline = deadline
    if deadline is not None:
        initial_budget = max(0.0, deadline - clock())
        clear_reserve = min(2.0, max(0.1, initial_budget * 0.2), initial_budget)
        action_deadline = deadline - clear_reserve

    def action_timeout(cap: float) -> float:
        if action_deadline is None:
            return cap
        return deadline_timeout(action_deadline, cap, clock=clock)

    try:
        for step_index, step in enumerate(steps):
            if action_deadline is not None and clock() >= action_deadline:
                success = False
                break
            tool = str(step.get("tool") or "pad_tap")
            button = str(step.get("button") or "cross")
            if tool not in supported:
                events.append({"event": tool, "button": button, "ok": False})
                success = False
                break

            hold_s = float(step.get("hold_s", 0.0))
            if not 0.0 <= hold_s <= 30.0 or (hold_s and tool != "pad_down"):
                events.append({"event": tool, "button": button, "ok": False, "error": "invalid_hold"})
                success = False
                break
            hold_before = None
            if hold_s:
                status_code, status_obj = call(sock, "status", {}, timeout=max(0.001, action_timeout(2.0)))
                if status_code != 0:
                    success = False
                    break
                hold_before = pad_counters(extract_result(status_obj))
                if status_observer is not None:
                    status_observer(extract_result(status_obj))
            budget = action_timeout(2.0)
            if budget <= 0:
                success = False
                break
            code, _obj = call(sock, tool, {"button": button}, timeout=budget)
            events.append({"event": tool, "button": button, "ok": code == 0})
            if code != 0:
                success = False
                break

            # A down/up pair without an observed hold can be cleared before a
            # guest ever polls it. Keep the requested overlay active while
            # observing guest input counters; the delivery gate checks them.
            hold_until = clock() + hold_s
            hold_after = hold_before
            while clock() < hold_until:
                if action_deadline is not None and clock() >= action_deadline:
                    success = False
                    break
                wait_s = min(0.05, hold_until - clock())
                if action_deadline is not None:
                    wait_s = min(wait_s, max(0.0, action_deadline - clock()))
                pause(wait_s)
                status_code, status_obj = call(sock, "status", {}, timeout=max(0.001, action_timeout(2.0)))
                if status_code != 0:
                    success = False
                    break
                if status_observer is not None:
                    status_observer(extract_result(status_obj))
                hold_after = pad_counters(extract_result(status_obj))
            if hold_before is not None and hold_after is not None:
                consumed = (hold_after["guest_read_state_samples"] + hold_after["guest_read_samples"]
                            > hold_before["guest_read_state_samples"] + hold_before["guest_read_samples"])
                events.append({"event": "hold_observed", "button": button, "hold_s": hold_s,
                               "input_before": hold_before, "input_after": hold_after, "ok": success and consumed})
                success = success and consumed
            if action_deadline is not None and clock() > action_deadline:
                success = False
            if not success:
                break

            if tool == "pad_tap":
                tap_completed = False
                for _attempt in range(60):
                    if action_deadline is not None and clock() >= action_deadline:
                        break
                    status_code, status_obj = call(sock, "status", {}, timeout=action_timeout(2.0))
                    if status_code != 0:
                        success = False
                        break
                    observed_status = extract_result(status_obj)
                    if status_observer is not None:
                        status_observer(observed_status)
                    if not pad_counters(observed_status)["tap_pending"]:
                        tap_completed = True
                        break
                    wait_s = (
                        0.05
                        if action_deadline is None
                        else min(0.05, max(0.0, action_deadline - clock()))
                    )
                    if wait_s > 0.0:
                        pause(wait_s)
                if not tap_completed:
                    success = False
                    break
                if step_index + 1 < len(steps) and inter_tap_settle_s > 0.0:
                    settle_until = clock() + inter_tap_settle_s
                    if action_deadline is not None:
                        settle_until = min(settle_until, action_deadline)
                    while clock() < settle_until:
                        pause(min(0.25, settle_until - clock()))
                        status_code, status_obj = call(sock, "status", {}, timeout=action_timeout(2.0))
                        if status_code != 0:
                            success = False
                            break
                        if status_observer is not None:
                            status_observer(extract_result(status_obj))
                    if not success:
                        break
                    if action_deadline is not None and clock() >= action_deadline:
                        success = False
                        break
    finally:
        clear_timeout = 2.0 if deadline is None else deadline_timeout(deadline, 2.0, clock=clock)
        clear_code, _clear_obj = call(sock, "pad_clear", {}, timeout=max(0.001, clear_timeout))
        events.append({"event": "pad_clear", "ok": clear_code == 0})
        success = success and clear_code == 0

    return success, events


def advance_post_input_wait(
    state: PostInputWaitState,
    require_loading_transition: bool,
    phase: str,
    present: int,
    now: float,
    min_present_delta: int,
    min_settle_s: float,
) -> bool:
    # Agent phase is derived from host FPS, not guest scene identity. Keep it
    # diagnostic only; explicit captures establish the fixture checkpoint.
    del require_loading_transition, phase
    if state.interactive_start_present is not None and present < state.interactive_start_present:
        state.interactive_since = None
        state.interactive_start_present = None
    if state.interactive_since is None or state.interactive_start_present is None:
        state.interactive_since = now
        state.interactive_start_present = present

    present_delta = max(0, present - state.interactive_start_present)
    elapsed_s = max(0.0, now - state.interactive_since)
    return present_delta >= min_present_delta and elapsed_s >= min_settle_s


def record_input_phase_observation(state: PostInputWaitState, phase: str) -> bool:
    """Record a phase seen while the input sequence is still in progress.

    Inter-tap observations may prove that the guest entered loading, but they
    must not start the post-input interactive settle window before the final
    input has been delivered.
    """
    if phase != "loading":
        return False

    newly_seen = not state.loading_seen
    state.loading_seen = True
    state.interactive_since = None
    state.interactive_start_present = None
    return newly_seen


def can_start_pad_sequence(
    agent_ready: bool,
    first_present: Optional[int],
    phase: str,
    presents_stable: bool,
    elapsed: float,
    stability_fallback_at: float,
    menu_ready_at: float = 0.0,
) -> bool:
    return (
        agent_ready
        and first_present is not None
        and elapsed >= menu_ready_at
        and (presents_stable or elapsed >= stability_fallback_at)
    )


def classify_exit(exit_code: Optional[int], timed_out: bool, *, coordinator_stop: bool = False) -> str:
    """A deadline/stop request never overrides an actual abnormal return."""
    if exit_code is None:
        return "unknown"
    if coordinator_stop and exit_code in (0, -15, -9, 143, 137):
        return "controlled_timeout" if timed_out else "coordinator_stop"
    if exit_code == 0:
        return "guest_exit_ok"
    if exit_code < 0 or exit_code in (139, 132, 134, 136, 65, 321):
        return "host_crash"
    return f"exit_{exit_code}"


def input_delivery_checks(before: dict[str, Any], after: dict[str, Any],
                          milestones: dict[str, Any], sequence_ok: bool) -> dict[str, bool]:
    taps = int(after.get("delivered_taps", 0)) - int(before.get("delivered_taps", 0))
    states = int(after.get("guest_read_state_samples", 0)) - int(before.get("guest_read_state_samples", 0))
    reads = int(after.get("guest_read_samples", 0)) - int(before.get("guest_read_samples", 0))
    return {
        "sequence_and_clear": sequence_ok,
        "tap_count": taps == int(milestones.get("min_pad_taps", 1)),
        "read_state_samples": states >= int(milestones.get("min_guest_read_state_samples", 1)),
        "read_samples": reads >= int(milestones.get("min_guest_read_samples", 0)),
        "guest_consumed_input": states >= 0 and reads >= 0 and states + reads > 0,
    }


def evaluate_gates(
    *,
    agent_ready: bool,
    video_initialized: bool,
    first_present: Optional[int],
    present_delta: int,
    input_before: dict[str, Any],
    input_after: dict[str, Any],
    capture_path: str,
    compare: dict[str, Any],
    baseline_missing: bool,
    create_baseline: bool,
    shutdown: str,
    first_error: str,
    milestones: dict[str, Any],
    visual_require_baseline: bool,
    input_sequence_ok: bool = True,
    checkpoint: Optional[dict[str, Any]] = None,
    timed_out: bool = False,
    final_error_observed: bool = True,
) -> list[GateResult]:
    gates: list[GateResult] = []
    gates.append(GateResult("agent_ready", agent_ready, "agent_ready" if agent_ready else "agent_not_ready"))
    gates.append(
        GateResult(
            "video_initialized",
            video_initialized,
            "graphic_ready" if video_initialized else "graphic_not_ready",
        )
    )
    min_present = int(milestones.get("min_present_after_ready", 1))
    presents_ok = first_present is not None and first_present >= min_present
    gates.append(
        GateResult(
            "first_present",
            presents_ok,
            f"first_present={first_present}",
        )
    )
    need_delta = int(milestones.get("present_delta_stable", 15))
    gates.append(
        GateResult(
            "present_advancing",
            present_delta >= need_delta,
            f"present_delta={present_delta} need>={need_delta}",
        )
    )
    delivery = input_delivery_checks(input_before, input_after, milestones, input_sequence_ok)
    gates.append(
        GateResult(
            "input_sequence_complete",
            input_sequence_ok,
            "sequence_and_clear_ok" if input_sequence_ok else "tap_or_clear_failed",
        )
    )
    gates.append(
        GateResult(
            "input_delivered",
            all(delivery.values()),
            json.dumps(delivery, sort_keys=True),
        )
    )
    gates.append(
        GateResult(
            "native_capture",
            bool(capture_path),
            capture_path or "no_capture",
        )
    )
    if baseline_missing and create_baseline:
        gates.append(
            GateResult(
                "visual_compare",
                bool(compare.get("pass")),
                "material_baseline_candidate" if compare.get("pass") else json.dumps(compare.get("checks") or compare, sort_keys=True)[:200],
            )
        )
    elif baseline_missing and visual_require_baseline:
        gates.append(GateResult("visual_compare", False, "baseline_missing"))
    else:
        gates.append(
            GateResult(
                "visual_compare",
                bool(compare.get("pass")),
                json.dumps(compare.get("checks") or compare, sort_keys=True)[:200],
            )
        )
    crash = shutdown == "host_crash"
    gates.append(GateResult("no_host_crash", not crash, shutdown))
    gates.append(GateResult("expected_exit", shutdown in ("guest_exit_ok", "coordinator_stop"), shutdown))
    gates.append(GateResult("within_deadline", not timed_out and shutdown != "controlled_timeout", shutdown))
    gates.append(GateResult("no_runtime_failure", not first_error, first_error or "none"))
    gates.append(GateResult("final_error_observed", final_error_observed, "observed" if final_error_observed else "unknown"))
    checks = (checkpoint or {}).get("checks") or {}
    scene = all(checks.get(key, False) for key in ("gameplay_checkpoint", "before_scene", "after_scene"))
    response = all(checks.get(key, False) for key in
                   ("capture_input_order", "action_delivered", "action_settled", "expected_response"))
    gates.append(GateResult("scene_checkpoint", scene, json.dumps(checkpoint or {"error": "checkpoint_missing"})[:300]))
    gates.append(GateResult("action_response", response, json.dumps(checks, sort_keys=True)))
    return gates


def material_visual_checks(raw_compare: dict[str, Any]) -> dict[str, bool]:
    checks = raw_compare.get("checks") if isinstance(raw_compare.get("checks"), dict) else {}
    return {key: bool(checks.get(key, False)) for key in MATERIAL_VISUAL_CHECKS}


def compare_playable_visual_metrics(capture_mod: Any, metrics: dict[str, Any], baseline: dict[str, Any]) -> dict[str, Any]:
    raw_compare = capture_mod.compare_metrics(metrics, baseline)
    material = material_visual_checks(raw_compare)
    return {
        "pass": all(material.values()),
        "assessment": "material_health_only",
        "checks": material,
        "delta": raw_compare.get("delta") or {},
        "raw_checks": raw_compare.get("checks") or {},
    }


def visual_floor_from_metrics(metrics: dict[str, Any]) -> dict[str, Any]:
    world = metrics.get("world") if isinstance(metrics.get("world"), dict) else {}
    white_ratio = float(world.get("white_ratio") or 0.0)
    entropy = float(world.get("entropy") or 0.0)
    unique_colors = int(world.get("unique_quantized_colors") or 0)
    checks = {
        "absolute_world_gate": white_ratio < 0.35 and entropy >= 2.5 and unique_colors >= 80,
        "not_stripey": not bool(world.get("stripey")),
    }
    return {
        "pass": all(checks.values()),
        "assessment": "material_health_only",
        "checks": checks,
        "world": {
            "white_ratio": white_ratio,
            "entropy": entropy,
            "unique_quantized_colors": unique_colors,
        },
    }


def retain_native_capture(result: dict[str, Any], capture_dir: Path, label: str,
                          min_present: int) -> dict[str, Any]:
    """Bind evidence to the fresh native response and this child's output tree."""
    path = Path(result.get("path") or "").resolve()
    present = int(result.get("present") or 0)
    if (result.get("image_format") != "PNG" or path.suffix.lower() != ".png"
            or not path.is_relative_to(capture_dir.resolve()) or not path.is_file()
            or present <= min_present or path.stat().st_size > 32 * 1024 * 1024):
        raise ValueError("invalid_or_stale_native_capture")
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("native_capture_not_png")
    dest = capture_dir / f"{label}_present_{present}.png"
    dest.write_bytes(data)
    return {"path": str(dest), "file": dest.name, "sha256": hashlib.sha256(data).hexdigest(),
            "present": present, "frame": result.get("frame"), "source": "kyty_agent.capture"}


def run_session(
    *,
    repo_root: Path,
    guest_root: Path,
    fc_script: Path,
    profile: dict[str, Any],
    scratch: Path,
    baseline_path: Path,
    create_baseline: bool,
    runtime_cwd: Optional[Path] = None,
    guest_script: Optional[Path] = None,
    scene_checkpoint_path: Optional[Path] = None,
) -> tuple[int, RunReport]:
    runtime_cwd, guest_script = resolve_runtime_launch_paths(
        repo_root, runtime_cwd=runtime_cwd, guest_script=guest_script
    )
    fc_script = fc_script.expanduser().resolve()
    capture_mod = load_capture_module(repo_root)
    report = RunReport(profile=profile)
    notes = report.notes
    run_id = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    scratch.mkdir(parents=True, exist_ok=True)
    run_dir = Path(tempfile.mkdtemp(prefix=f"run_{run_id}_", dir=scratch))
    capture_dir = run_dir / "captures"
    capture_dir.mkdir(exist_ok=True)
    log_path = run_dir / "child.log"
    env_keys_path = run_dir / "child_env_keys.txt"
    sock = agent_sock_path(run_dir.name)
    remove_stale_unix_socket(sock, notes)

    env = build_playable_environment(
        dict(os.environ),
        guest_root=guest_root,
        agent_socket=sock,
        capture_directory=capture_dir,
    )
    bad = find_forbidden_environment_keys(env)
    if bad:
        notes.append("strict_env_violation:" + ",".join(bad))
        report.gates = [GateResult("strict_env", False, ",".join(bad))]
        (run_dir / "summary.json").write_text(
            json.dumps(report.to_sanitized_dict(guest_root), indent=2) + "\n", encoding="utf-8"
        )
        return 1, report
    env_keys_path.write_text("\n".join(sorted(env.keys())) + "\n", encoding="utf-8")

    deadlines = profile.get("deadlines_s") or {}
    milestones = profile.get("milestones") or {}
    total_s = float(deadlines.get("total") or 180)
    ready_s = float(deadlines.get("agent_ready") or 60)
    first_present_s = float(deadlines.get("first_present") or 120)
    stability_s = float(deadlines.get("present_stability") or 25)
    input_s = float(deadlines.get("input_window") or 30)

    checkpoint = None
    if scene_checkpoint_path is not None:
        try:
            checkpoint = SceneCheckpoint(scene_checkpoint_path, capture_mod)
        except (OSError, ValueError, KeyError, TypeError, RuntimeError) as exc:
            report.checkpoint = {"error": f"checkpoint_invalid:{type(exc).__name__}"}
            notes.append("checkpoint_invalid")
    else:
        report.checkpoint = {"error": "checkpoint_missing"}

    cmd = [str(fc_script), str(guest_script), str(guest_root)]
    (run_dir / "launch-config.json").write_text(
        json.dumps({
            "command": cmd,
            "cwd": str(runtime_cwd),
            "binary_sha256": hashlib.sha256(fc_script.read_bytes()).hexdigest(),
            "guest_script_sha256": hashlib.sha256(guest_script.read_bytes()).hexdigest(),
        }, indent=2) + "\n", encoding="utf-8"
    )
    started = time.monotonic()
    deadline_total = started + total_s
    with log_path.open("w", encoding="utf-8") as log:
        proc = launch_process_group(
            cmd,
            cwd=runtime_cwd,
            env=env,
            stdout=log,
        )
    (run_dir / "child.pid").write_text(str(proc.pid), encoding="utf-8")

    agent_ready = False
    video_initialized = False
    first_present: Optional[int] = None
    present_at_stable_start: Optional[int] = None
    present_delta = 0
    input_before: dict[str, Any] = {}
    input_after: dict[str, Any] = {}
    capture_rel = ""
    compare: dict[str, Any] = {}
    baseline_missing = not baseline_path.is_file()
    timed_out = False
    coordinator_stop = False
    final_error_observed = False
    baseline_candidate: dict[str, Any] = {}
    status: dict[str, Any] = {}
    phase_input_done = False
    phase_stable_done = False
    phase_capture_done = False
    input_sequence_ok = False
    post_input_config = profile.get("post_input") or {}
    input_config = profile.get("input") or {}
    menu_settle_s = float(input_config.get("menu_settle_s") or 0.0)
    inter_tap_settle_s = float(input_config.get("inter_tap_settle_s") or 0.0)
    require_loading_transition = bool(post_input_config.get("require_loading_transition", True))
    post_input_min_present_delta = int(post_input_config.get("min_present_delta", 240))
    post_input_min_settle_s = float(post_input_config.get("min_settle_s", 15))
    post_input_wait = PostInputWaitState()
    post_input_ready = False
    interactive_since: Optional[float] = None

    def observe_errors(stage: str) -> bool:
        observed = True
        for tool, args in (("last_error", {}), ("events", {"last": 100})):
            budget = deadline_timeout(deadline_total, 1.0)
            if budget <= 0:
                observed = False
                continue
            code, obj = call_agent(sock, tool, args, timeout=budget)
            if code != 0:
                observed = False
                continue
            result = extract_result(obj)
            error = first_actionable_error(result.get("events") or [], result.get("event") or {})
            if error and not report.first_error:
                report.first_error = error
                report.timeline.append({"t": round(time.monotonic() - started, 3),
                                        "event": "runtime_error", "stage": stage, "error": error})
        if not observed:
            notes.append(f"error_observation_incomplete:{stage}")
        return observed

    def take_capture(label: str, min_present: int) -> dict[str, Any]:
        nonlocal timed_out
        budget = deadline_timeout(deadline_total, 20.0)
        if budget < 2.0:
            timed_out = True
            notes.append("capture_budget_exhausted")
            return {}
        code, obj = call_agent(sock, "capture",
                               {"timeout_ms": min(15000, int((budget - 0.25) * 1000)), "score": True},
                               timeout=budget)
        observe_errors("post_capture")
        if code != 0:
            notes.append("capture_failed")
            return {}
        try:
            record = retain_native_capture(extract_result(obj), capture_dir, label, min_present)
        except (OSError, ValueError, TypeError) as exc:
            notes.append(f"capture_invalid:{type(exc).__name__}")
            return {}
        report.timeline.append({"t": round(time.monotonic() - started, 3), "event": "capture",
                                **{k: v for k, v in record.items() if k != "path"}})
        return record

    def exercise_checkpoint(before: dict[str, Any]) -> dict[str, Any]:
        assert checkpoint is not None
        action: dict[str, Any] = {}
        after: dict[str, Any] = {}
        if before and checkpoint.match(Path(before["path"]), "before")["matches"]:
            config = checkpoint.contract["action"]
            budget = deadline_timeout(deadline_total, 2.0)
            code, obj = call_agent(sock, "status", timeout=max(0.001, budget))
            if code == 0 and budget > 0:
                pre = extract_result(obj)
                action["start_present"] = int(pre.get("present") or 0)
                action["input_before"] = pad_counters(pre)
                action_ok, events = deliver_pad_sequence(
                    sock, config["pad_sequence"], deadline=min(deadline_total, time.monotonic() + input_s),
                    inter_tap_settle_s=inter_tap_settle_s,
                    status_observer=lambda _status: observe_errors("checkpoint_input"),
                )
                action["events"] = events
                observe_errors("post_checkpoint_input")
                budget = deadline_timeout(deadline_total, 2.0)
                code, obj = call_agent(sock, "status", timeout=max(0.001, budget))
                if code == 0 and budget > 0:
                    post = extract_result(obj)
                    action["end_present"] = int(post.get("present") or 0)
                    action["input_after"] = pad_counters(post)
                    action["delivery_checks"] = input_delivery_checks(
                        action["input_before"], action["input_after"], config["milestones"], action_ok)
                    wait = PostInputWaitState(interactive_since=time.monotonic(),
                                              interactive_start_present=action["end_present"])
                    while time.monotonic() < deadline_total and proc.poll() is None:
                        budget = deadline_timeout(deadline_total, 2.0)
                        code, obj = call_agent(sock, "status", timeout=max(0.001, budget))
                        observe_errors("checkpoint_settle")
                        if code != 0:
                            break
                        present = int(extract_result(obj).get("present") or 0)
                        if advance_post_input_wait(wait, False, "", present, time.monotonic(),
                                                   int(config["min_present_delta"]), float(config["min_settle_s"])):
                            action["settled"] = True
                            after = take_capture("checkpoint_after", present)
                            break
                        time.sleep(min(0.1, max(0.0, deadline_total - time.monotonic())))
        report.checkpoint = checkpoint.evaluate(before, after, action)
        return after

    try:
        while time.monotonic() < deadline_total:
            if proc.poll() is not None:
                notes.append("child_exited_early")
                break
            now = time.monotonic()
            elapsed = now - started

            # Agent ready
            if not agent_ready:
                if elapsed > ready_s:
                    notes.append("agent_ready_deadline")
                    timed_out = True
                    break
                call_timeout = deadline_timeout(deadline_total, 1.0)
                if call_timeout <= 0.0:
                    timed_out = True
                    notes.append("total_deadline")
                    break
                code, obj = call_agent(sock, "ping", timeout=call_timeout)
                if code == 0:
                    agent_ready = True
                    notes.append("agent_ready")
                    report.timeline.append({"t": round(elapsed, 3), "event": "agent_ready"})
                time.sleep(min(0.25, max(0.0, deadline_total - time.monotonic())))
                continue

            phase = ""
            call_timeout = deadline_timeout(deadline_total, 2.0)
            if call_timeout <= 0.0:
                timed_out = True
                notes.append("total_deadline")
                break
            code, obj = call_agent(sock, "status", timeout=call_timeout)
            if code == 0:
                status = extract_result(obj)
                present = int(status.get("present") or 0)
                frame = int(status.get("frame") or 0)
                graphic = bool(status.get("graphic_ready"))
                phase = str(status.get("phase") or "")
                if not phase_input_done:
                    if graphic and present > 0:
                        if interactive_since is None:
                            interactive_since = elapsed
                    else:
                        interactive_since = None
                if phase_input_done:
                    input_after = pad_counters(status)
                report.timeline.append(
                    {
                        "t": round(elapsed, 3),
                        "event": "status",
                        "present": present,
                        "frame": frame,
                        "phase": phase,
                        "graphic_ready": graphic,
                        "fps": status.get("fps"),
                    }
                )
                if graphic and not video_initialized:
                    video_initialized = True
                    report.timeline.append({"t": round(elapsed, 3), "event": "video_initialized"})
                if present >= 1 and first_present is None:
                    first_present = present
                    report.timeline.append({"t": round(elapsed, 3), "event": "first_present", "present": present})
                    present_at_stable_start = present

                if phase_input_done and not post_input_ready:
                    was_loading_seen = post_input_wait.loading_seen
                    post_input_ready = advance_post_input_wait(
                        post_input_wait,
                        require_loading_transition,
                        phase,
                        present,
                        now,
                        post_input_min_present_delta,
                        post_input_min_settle_s,
                    )
                    if post_input_wait.loading_seen and not was_loading_seen:
                        notes.append("post_input_loading_seen")
                        report.timeline.append({"t": round(elapsed, 3), "event": "post_input_loading"})
                    if post_input_ready:
                        stable_present_delta = max(
                            0,
                            present
                            - int(
                                post_input_wait.interactive_start_present
                                if post_input_wait.interactive_start_present is not None
                                else present
                            ),
                        )
                        stable_settle_s = max(
                            0.0,
                            now
                            - float(
                                post_input_wait.interactive_since
                                if post_input_wait.interactive_since is not None
                                else now
                            ),
                        )
                        notes.append("post_input_settled")
                        report.timeline.append(
                            {
                                "t": round(elapsed, 3),
                                "event": "post_input_settled",
                                "present_delta": stable_present_delta,
                                "settle_s": round(stable_settle_s, 3),
                            }
                        )
            elif not phase_input_done:
                interactive_since = None

            observe_errors("poll")

            if first_present is None and elapsed > first_present_s:
                notes.append("first_present_deadline")
                timed_out = True
                break

            # Stability: accumulate present delta
            if first_present is not None and not phase_stable_done:
                if present_at_stable_start is None:
                    present_at_stable_start = int(status.get("present") or 0)
                present_now = int(status.get("present") or 0)
                present_delta = max(0, present_now - int(present_at_stable_start))
                need = int(milestones.get("present_delta_stable", 15))
                if present_delta >= need:
                    phase_stable_done = True
                    notes.append("present_stability_met")
                    report.timeline.append(
                        {"t": round(elapsed, 3), "event": "present_stable", "delta": present_delta}
                    )
                elif elapsed > first_present_s + stability_s:
                    notes.append("present_stability_deadline")
                    # continue to try input/capture with whatever we have

            # Startup routing uses bounded presentation progress. The FPS phase
            # remains diagnostic; only the fixture can identify a scene.
            if not phase_input_done and can_start_pad_sequence(
                agent_ready,
                first_present,
                phase,
                phase_stable_done,
                elapsed,
                first_present_s + stability_s * 0.5,
                float(interactive_since if interactive_since is not None else elapsed) + menu_settle_s,
            ):
                call_timeout = deadline_timeout(deadline_total, 2.0)
                if call_timeout <= 0.0:
                    timed_out = True
                    notes.append("total_deadline")
                    break
                code, sobj = call_agent(sock, "status", timeout=call_timeout)
                pre_input_status = extract_result(sobj) if code == 0 else {}
                if code != 0 or not pre_input_status.get("graphic_ready"):
                    interactive_since = None
                else:
                    input_before = pad_counters(pre_input_status)

                    def observe_input_status(observed_status: dict[str, Any]) -> None:
                        observe_errors("startup_input")
                        observed_phase = str(observed_status.get("phase") or "")
                        observed_now = time.monotonic()
                        if record_input_phase_observation(post_input_wait, observed_phase):
                            notes.append("post_input_loading_seen")
                            report.timeline.append(
                                {
                                    "t": round(observed_now - started, 3),
                                    "event": "post_input_loading",
                                }
                            )

                    input_sequence_ok, input_events = deliver_pad_sequence(
                        sock,
                        profile.get("pad_sequence") or [],
                        deadline=min(deadline_total, time.monotonic() + input_s),
                        inter_tap_settle_s=inter_tap_settle_s,
                        status_observer=observe_input_status,
                    )
                    for event in input_events:
                        report.timeline.append(
                            {"t": round(time.monotonic() - started, 3), **event}
                        )
                    if not input_sequence_ok:
                        notes.append("input_sequence_failed")
                    observe_errors("post_input")
                    fresh_timeout = deadline_timeout(deadline_total, 2.0)
                    fresh_code, fresh_obj = call_agent(sock, "status", timeout=max(0.001, fresh_timeout))
                    if fresh_code == 0:
                        status = extract_result(fresh_obj)
                        input_after = pad_counters(status)
                        fresh_now = time.monotonic()
                        fresh_phase = str(status.get("phase") or "")
                        was_loading_seen = post_input_wait.loading_seen
                        post_input_ready = advance_post_input_wait(
                            post_input_wait,
                            require_loading_transition,
                            fresh_phase,
                            int(status.get("present") or 0),
                            fresh_now,
                            post_input_min_present_delta,
                            post_input_min_settle_s,
                        )
                        if post_input_wait.loading_seen and not was_loading_seen:
                            notes.append("post_input_loading_seen")
                            report.timeline.append(
                                {
                                    "t": round(time.monotonic() - started, 3),
                                    "event": "post_input_loading",
                                }
                            )
                    else:
                        input_sequence_ok = False
                        notes.append("post_input_status_failed")
                    phase_input_done = True
                    notes.append("input_sequence_done")

            # First capture is the fixture's pre-action checkpoint. Material
            # health alone cannot promote it to a playable baseline.
            if agent_ready and first_present is not None and not phase_capture_done:
                if phase_input_done and post_input_ready:
                    before = take_capture("checkpoint_before", int(status.get("present") or 0))
                    selected = before
                    try:
                        if checkpoint is not None:
                            selected = exercise_checkpoint(before) or before
                        if selected:
                            capture_rel = selected["file"]
                            metrics = capture_mod.score_image(Path(selected["path"]))
                            (run_dir / "capture_metrics.json").write_text(
                                json.dumps(sanitize_obj(metrics, guest_root), indent=2) + "\n", encoding="utf-8")
                            if create_baseline:
                                compare = visual_floor_from_metrics(metrics)
                                baseline_candidate = {
                                    "schema": "kyty_playable_visual_baseline_v1", "world": metrics["world"],
                                    "checkpoint_contract_sha256": checkpoint.sha256 if checkpoint else None,
                                    "capture_sha256": selected["sha256"],
                                    "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                                }
                            elif baseline_path.is_file():
                                bas = json.loads(baseline_path.read_text(encoding="utf-8"))
                                baseline_metrics = bas if "world" in bas else {"world": bas}
                                compare = compare_playable_visual_metrics(capture_mod, metrics, baseline_metrics)
                                notes.append("visual_compare_done")
                    except Exception as exc:  # noqa: BLE001 — keep failed checkpoint evidence
                        notes.append(f"checkpoint_or_metrics_error:{type(exc).__name__}")
                        report.checkpoint = {"error": type(exc).__name__}
                        compare = {"pass": False, "error": type(exc).__name__}
                    phase_capture_done = True

            # Done when core phases complete
            if phase_stable_done and phase_input_done and phase_capture_done:
                notes.append("session_milestones_complete")
                break

            time.sleep(min(0.35, max(0.0, deadline_total - time.monotonic())))
        else:
            timed_out = True
            notes.append("total_deadline")

    finally:
        # Observe failures after input, capture and scoring, before stopping the
        # child. A lost final observation is unknown, never an invented clean
        # error state. The child return is collected after coordinator cleanup.
        if agent_ready:
            final_error_observed = observe_errors("final")
        if time.monotonic() >= deadline_total:
            timed_out = True
        report.stop_reason = "deadline" if timed_out else (
            "milestones_complete" if phase_stable_done and phase_input_done and phase_capture_done else "incomplete")
        if proc.poll() is None:
            report.coordinator_stop_requested = True
            stop_notes_start = len(notes)
            terminate_process_group(proc, notes)
            coordinator_stop = any(n in ("killed_sigterm", "killed_sigkill", "kill_timeout_after_sigkill")
                                   for n in notes[stop_notes_start:])
        exit_code = proc.poll()
        if not coordinator_stop:
            report.stop_reason = ("natural_exit" if exit_code == 0 else
                                  "abnormal_exit" if exit_code is not None else "unknown")
        try:
            if sock.exists():
                sock.unlink()
        except OSError:
            pass

    report.child_exit = exit_code
    report.timed_out = timed_out
    report.shutdown = classify_exit(exit_code, timed_out, coordinator_stop=coordinator_stop)
    report.first_present = first_present
    report.present_delta = present_delta
    report.input_before = input_before
    report.input_after = input_after
    report.capture_path = capture_rel
    report.compare = compare

    if not report.first_error:
        report.first_error = first_actionable_from_log(log_path)

    visual_req = bool((profile.get("visual") or {}).get("require_baseline", True))
    report.gates = evaluate_gates(
        agent_ready=agent_ready,
        video_initialized=video_initialized,
        first_present=first_present,
        present_delta=present_delta,
        input_before=input_before,
        input_after=input_after,
        capture_path=capture_rel,
        compare=compare,
        baseline_missing=baseline_missing,
        create_baseline=create_baseline and bool(capture_rel),
        shutdown=report.shutdown,
        first_error=report.first_error,
        milestones=milestones,
        visual_require_baseline=visual_req,
        input_sequence_ok=input_sequence_ok,
        checkpoint=report.checkpoint,
        timed_out=timed_out,
        final_error_observed=final_error_observed,
    )

    # Do not overwrite a retained baseline with a menu, failed action, late
    # runtime error, or crash, even when the image-health floor was satisfied.
    if create_baseline and baseline_candidate and report.all_passed():
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_path.write_text(json.dumps(baseline_candidate, indent=2) + "\n", encoding="utf-8")
        notes.append("baseline_created")
        baseline_missing = False
    elif create_baseline:
        notes.append("baseline_rejected_gates")

    summary = report.to_sanitized_dict(guest_root)
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    (run_dir / "timeline.json").write_text(
        json.dumps(sanitize_obj(report.timeline, guest_root), indent=2) + "\n", encoding="utf-8"
    )

    if baseline_missing and visual_req and not create_baseline:
        return 3, report
    if report.all_passed():
        return 0, report
    return 1, report


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Kyty strict playable regression profile")
    parser.add_argument("--guest-root", default=os.environ.get("KYTY_GUEST_ROOT", ""))
    parser.add_argument(
        "--scratch",
        default=os.environ.get("KYTY_REGRESSION_SCRATCH", ""),
        help="Untracked scratch (default: <repo>/_scratch_playable/playable_regression)",
    )
    parser.add_argument(
        "--baseline",
        default=os.environ.get("KYTY_REGRESSION_BASELINE", ""),
        help="Untracked baseline metrics JSON (local only)",
    )
    parser.add_argument("--create-baseline", action="store_true")
    parser.add_argument("--profile", default="", help="JSON profile with bounded routing/deadlines/milestones")
    parser.add_argument("--scene-checkpoint", type=Path, help="External reviewed fixture scene/action contract JSON")
    parser.add_argument("--fc-script", default="_build_linux/fc_script")
    parser.add_argument("--runtime-cwd", default="", help="Existing writable runtime directory (default: repo root)")
    parser.add_argument("--guest-script", default="", help="Lua input (default: scripts/run_guest.lua; relative to repo root)")
    parser.add_argument("--repo-root", default="")
    parser.add_argument("--allow-missing-guest", action="store_true", help="Exit 0 with skip summary if guest unset")
    args = parser.parse_args(argv)

    repo_root = Path(args.repo_root or Path(__file__).resolve().parents[1]).resolve()
    profile_path = Path(args.profile) if args.profile else (repo_root / "scripts" / "profiles" / "strict_playable.json")
    if profile_path.is_file():
        profile = load_profile(profile_path)
    else:
        profile = load_profile(None)

    scratch = Path(
        args.scratch
        or os.environ.get("KYTY_REGRESSION_SCRATCH")
        or (repo_root / "_scratch_playable" / "playable_regression")
    ).expanduser().resolve()
    scratch.mkdir(parents=True, exist_ok=True)

    baseline = Path(
        args.baseline
        or os.environ.get("KYTY_REGRESSION_BASELINE")
        or (scratch / "visual_baseline.json")
    ).expanduser().resolve()

    guest_s = args.guest_root or os.environ.get("KYTY_GUEST_ROOT", "")
    if not guest_s:
        skip = {
            "schema": "kyty_playable_regression_summary_v1",
            "mode": "strict",
            "passed": False,
            "skipped": True,
            "reason": "KYTY_GUEST_ROOT unset",
            "gates": [],
            "notes": ["honest_skip_no_guest_root"],
        }
        (scratch / "summary_skip.json").write_text(json.dumps(skip, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(skip, indent=2))
        return 0 if args.allow_missing_guest else 2

    guest_root = Path(guest_s).expanduser().resolve()
    if not guest_root.is_dir():
        print("error: guest root is not a directory", file=sys.stderr)
        return 2

    fc = Path(args.fc_script)
    if not fc.is_absolute():
        fc = (repo_root / fc).resolve()
    if not fc.is_file():
        print("error: fc_script missing", file=sys.stderr)
        return 2

    try:
        runtime_cwd, guest_script = resolve_runtime_launch_paths(
            repo_root,
            runtime_cwd=Path(args.runtime_cwd) if args.runtime_cwd else None,
            guest_script=Path(args.guest_script) if args.guest_script else None,
        )
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    rc, report = run_session(
        repo_root=repo_root,
        guest_root=guest_root,
        fc_script=fc,
        profile=profile,
        scratch=scratch,
        baseline_path=baseline,
        create_baseline=args.create_baseline,
        runtime_cwd=runtime_cwd,
        guest_script=guest_script,
        scene_checkpoint_path=args.scene_checkpoint,
    )
    summary = report.to_sanitized_dict(guest_root)
    # Ensure no absolute private path leaked
    text = json.dumps(summary)
    if str(guest_root) in text or "/home/" in text:
        summary = sanitize_obj(summary, guest_root)
        text = json.dumps(summary)
    print(text)
    print(f"scratch={scratch}", file=sys.stderr)
    print(f"exit={rc} passed={summary.get('passed')}", file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
