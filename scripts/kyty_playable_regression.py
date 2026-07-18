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
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional


FORBIDDEN_CHILD_ENV = (
    "KYTY_BRINGUP_MODE",
    "KYTY_BRINGUP_FEATURES",
    "KYTY_BRINGUP_SUBSYSTEMS",
    "KYTY_BRINGUP_BURST_LIMIT",
    "KYTY_BRINGUP_BURST_WINDOW_MS",
    "KYTY_BRINGUP_ALLOW_DIAGNOSTIC",
    "KYTY_STUB_MISSING",
    "KYTY_GFX_PERMISSIVE",
    "KYTY_AUTO_CROSS",
    "KYTY_SKIP_UD2",
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
        "min_pad_taps": 1,
        "min_guest_read_state_samples": 1,
    },
    "pad_sequence": [
        {"tool": "pad_tap", "button": "cross"},
        {"tool": "pad_tap", "button": "cross"},
    ],
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


def clean_child_env(
    base: dict[str, str],
    *,
    guest_root: Path,
    agent_sock: Path,
    capture_dir: Path,
) -> dict[str, str]:
    keep_keys = (
        "PATH",
        "HOME",
        "USER",
        "LOGNAME",
        "LANG",
        "LC_ALL",
        "LC_CTYPE",
        "DISPLAY",
        "WAYLAND_DISPLAY",
        "XDG_RUNTIME_DIR",
        "XAUTHORITY",
        "XDG_SESSION_TYPE",
        "DBUS_SESSION_BUS_ADDRESS",
        "VK_ICD_FILENAMES",
        "VK_DRIVER_FILES",
        "LD_LIBRARY_PATH",
        "LIBGL_DRIVERS_PATH",
        "MESA_LOADER_DRIVER_OVERRIDE",
        "AMD_VULKAN_ICD",
        "DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1",
    )
    env: dict[str, str] = {}
    for k in keep_keys:
        if k in base and base[k]:
            env[k] = base[k]
    env["KYTY_GUEST_ROOT"] = str(guest_root)
    env["KYTY_AGENT_SOCK"] = str(agent_sock)
    env["KYTY_NATIVE_CAPTURE_DIR"] = str(capture_dir)
    env["KYTY_PRINTF_DIRECTION"] = "Silent"
    env["KYTY_SCREEN_WIDTH"] = base.get("KYTY_SCREEN_WIDTH", "1280")
    env["KYTY_SCREEN_HEIGHT"] = base.get("KYTY_SCREEN_HEIGHT", "720")
    env["KYTY_NATIVE_CAPTURE_MAX_EDGE"] = base.get("KYTY_NATIVE_CAPTURE_MAX_EDGE", "1280")
    for k in FORBIDDEN_CHILD_ENV:
        env.pop(k, None)
    return env


def assert_strict_env(env: dict[str, str]) -> list[str]:
    return [k for k in FORBIDDEN_CHILD_ENV if k in env]


def agent_sock_path(run_id: str) -> Path:
    digest = hashlib.sha256(run_id.encode("utf-8")).hexdigest()[:12]
    return Path(f"/tmp/kyty_pr_{digest}.sock")


def sock_is_stale(path: Path) -> bool:
    if not path.exists():
        return False
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.2)
        try:
            s.connect(str(path))
            return False
        except (ConnectionRefusedError, FileNotFoundError, OSError):
            return True
        finally:
            try:
                s.close()
            except OSError:
                pass
    except OSError:
        return True


def remove_stale_socket(path: Path, notes: list[str]) -> None:
    if path.exists() and sock_is_stale(path):
        try:
            path.unlink()
            notes.append("removed_stale_socket")
        except OSError as exc:
            notes.append(f"stale_socket_unlink_failed:{type(exc).__name__}")


def kill_process_group(proc: subprocess.Popen[Any], notes: list[str]) -> None:
    if proc.poll() is not None:
        return
    try:
        if os.name == "posix":
            os.killpg(proc.pid, signal.SIGTERM)
        else:
            proc.terminate()
        try:
            proc.wait(timeout=5)
            notes.append("killed_sigterm")
            return
        except subprocess.TimeoutExpired:
            pass
        if os.name == "posix":
            os.killpg(proc.pid, signal.SIGKILL)
        else:
            proc.kill()
        proc.wait(timeout=5)
        notes.append("killed_sigkill")
    except ProcessLookupError:
        notes.append("process_already_gone")
    except OSError as exc:
        notes.append(f"kill_failed:{type(exc).__name__}")


def agent_call(sock: Path, tool: str, args: Optional[dict[str, Any]] = None, timeout: float = 8.0) -> tuple[int, dict[str, Any]]:
    req = {"id": 1, "tool": tool, "args": args or {}}
    s: Optional[socket.socket] = None
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(str(sock))
        s.sendall((json.dumps(req, separators=(",", ":")) + "\n").encode("utf-8"))
        data = b""
        while b"\n" not in data and len(data) < 262144:
            chunk = s.recv(8192)
            if not chunk:
                break
            data += chunk
        line = data.decode("utf-8", errors="replace").split("\n", 1)[0]
        obj = json.loads(line) if line else {}
        if not obj.get("ok", False):
            return 1, obj
        return 0, obj
    except FileNotFoundError:
        return 125, {"ok": False, "error": {"code": "transport", "message": "socket_missing"}}
    except (ConnectionRefusedError, TimeoutError, OSError, json.JSONDecodeError) as exc:
        return 125, {"ok": False, "error": {"code": "transport", "message": type(exc).__name__}}
    finally:
        if s is not None:
            try:
                s.close()
            except OSError:
                pass


def extract_result(obj: dict[str, Any]) -> dict[str, Any]:
    if isinstance(obj.get("result"), dict):
        return obj["result"]
    return obj


def first_actionable_error(events: list[dict[str, Any]], last_error: dict[str, Any]) -> str:
    if last_error:
        code = str(last_error.get("code") or last_error.get("message") or "").strip()
        if code and code.lower() not in ("none", "null", ""):
            return code[:160]
    for ev in events:
        if not isinstance(ev, dict):
            continue
        kind = str(ev.get("kind") or "").lower()
        code = str(ev.get("code") or "").strip()
        if kind in ("error", "fatal") or code:
            if code:
                return code[:160]
    return ""


def first_actionable_from_log(log_path: Path) -> str:
    if not log_path.is_file():
        return ""
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    for line in text.splitlines():
        s = line.strip()
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
            "timeline": self.timeline,
            "notes": self.notes,
            "profile_deadlines_s": self.profile.get("deadlines_s", {}),
            "profile_milestones": self.profile.get("milestones", {}),
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


def classify_exit(exit_code: Optional[int], timed_out: bool) -> str:
    if timed_out:
        return "controlled_timeout"
    if exit_code is None:
        return "unknown"
    if exit_code == 0:
        return "guest_exit_ok"
    if exit_code in (139, 132, 134, 136, 65, 321):
        return "host_crash"
    if exit_code in (-15, -9, 143, 137):
        return "controlled_kill"
    return f"exit_{exit_code}"


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
    min_present = int(milestones.get("min_present_after_ready") or 1)
    presents_ok = first_present is not None and first_present >= min_present
    gates.append(
        GateResult(
            "first_present",
            presents_ok,
            f"first_present={first_present}",
        )
    )
    need_delta = int(milestones.get("present_delta_stable") or 15)
    gates.append(
        GateResult(
            "present_advancing",
            present_delta >= need_delta,
            f"present_delta={present_delta} need>={need_delta}",
        )
    )
    min_taps = int(milestones.get("min_pad_taps") or 1)
    taps_before = int(input_before.get("delivered_taps") or 0)
    taps_after = int(input_after.get("delivered_taps") or 0)
    reads_before = int(input_before.get("guest_read_state_samples") or 0)
    reads_after = int(input_after.get("guest_read_state_samples") or 0)
    min_reads = int(milestones.get("min_guest_read_state_samples") or 1)
    taps_ok = (taps_after - taps_before) >= min_taps
    # Prefer delivered_taps; fall back to guest actually sampling pad state after our taps.
    reads_ok = (reads_after - reads_before) >= min_reads
    input_ok = taps_ok or (taps_after > taps_before) or reads_ok
    gates.append(
        GateResult(
            "input_delivered",
            input_ok,
            f"taps {taps_before}->{taps_after} reads {reads_before}->{reads_after}",
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
                "baseline_created" if compare.get("pass") else json.dumps(compare.get("checks") or compare, sort_keys=True)[:200],
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
    # Loader/assert style first error after agent is a blocker if we never presented.
    if first_error and not presents_ok:
        gates.append(GateResult("no_early_loader_failure", False, first_error))
    else:
        gates.append(GateResult("no_early_loader_failure", True, first_error or "none"))
    return gates


def material_visual_checks(raw_compare: dict[str, Any]) -> dict[str, bool]:
    checks = raw_compare.get("checks") if isinstance(raw_compare.get("checks"), dict) else {}
    return {key: bool(checks.get(key, False)) for key in MATERIAL_VISUAL_CHECKS}


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
        "checks": checks,
        "world": {
            "white_ratio": white_ratio,
            "entropy": entropy,
            "unique_quantized_colors": unique_colors,
        },
    }


def run_session(
    *,
    repo_root: Path,
    guest_root: Path,
    fc_script: Path,
    profile: dict[str, Any],
    scratch: Path,
    baseline_path: Path,
    create_baseline: bool,
) -> tuple[int, RunReport]:
    capture_mod = load_capture_module(repo_root)
    report = RunReport(profile=profile)
    notes = report.notes
    run_id = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    run_dir = scratch / f"run_{run_id}"
    run_dir.mkdir(parents=True, exist_ok=True)
    capture_dir = run_dir / "captures"
    capture_dir.mkdir(exist_ok=True)
    log_path = run_dir / "child.log"
    env_keys_path = run_dir / "child_env_keys.txt"
    sock = agent_sock_path(run_id)
    remove_stale_socket(sock, notes)

    env = clean_child_env(
        dict(os.environ),
        guest_root=guest_root,
        agent_sock=sock,
        capture_dir=capture_dir,
    )
    bad = assert_strict_env(env)
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

    cmd = [str(fc_script), "scripts/run_guest.lua", str(guest_root)]
    started = time.monotonic()
    deadline_total = started + total_s
    with log_path.open("w", encoding="utf-8") as log:
        proc = subprocess.Popen(
            cmd,
            cwd=str(repo_root),
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=(os.name == "posix"),
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
    status: dict[str, Any] = {}
    phase_input_done = False
    phase_stable_done = False
    phase_capture_done = False

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
                code, obj = agent_call(sock, "ping", timeout=1.0)
                if code == 0:
                    agent_ready = True
                    notes.append("agent_ready")
                    report.timeline.append({"t": round(elapsed, 3), "event": "agent_ready"})
                time.sleep(0.25)
                continue

            code, obj = agent_call(sock, "status", timeout=2.0)
            if code == 0:
                status = extract_result(obj)
                present = int(status.get("present") or 0)
                frame = int(status.get("frame") or 0)
                graphic = bool(status.get("graphic_ready"))
                phase = str(status.get("phase") or "")
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

            # last_error snapshot
            ecode, eobj = agent_call(sock, "last_error", timeout=1.0)
            if ecode == 0:
                ev = extract_result(eobj).get("event")
                if isinstance(ev, dict) and not report.first_error:
                    report.first_error = first_actionable_error([], ev)

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
                need = int(milestones.get("present_delta_stable") or 15)
                if present_delta >= need:
                    phase_stable_done = True
                    notes.append("present_stability_met")
                    report.timeline.append(
                        {"t": round(elapsed, 3), "event": "present_stable", "delta": present_delta}
                    )
                elif elapsed > first_present_s + stability_s:
                    notes.append("present_stability_deadline")
                    # continue to try input/capture with whatever we have

            # Input phase once we have some presents (or stability deadline passed)
            if agent_ready and first_present is not None and not phase_input_done:
                if phase_stable_done or elapsed > first_present_s + stability_s * 0.5:
                    code, sobj = agent_call(sock, "status", timeout=2.0)
                    if code == 0:
                        input_before = pad_counters(extract_result(sobj))
                    for step in profile.get("pad_sequence") or []:
                        button = str(step.get("button") or "cross")
                        tool = str(step.get("tool") or "pad_tap")
                        if tool == "pad_tap":
                            agent_call(sock, "pad_tap", {"button": button}, timeout=2.0)
                        elif tool == "pad_down":
                            agent_call(sock, "pad_down", {"button": button}, timeout=2.0)
                        elif tool == "pad_up":
                            agent_call(sock, "pad_up", {"button": button}, timeout=2.0)
                        report.timeline.append(
                            {"t": round(time.monotonic() - started, 3), "event": tool, "button": button}
                        )
                        time.sleep(0.15)
                    # Wait a few presents for guest to sample overlay
                    agent_call(sock, "wait_present", {"delta": 5, "timeout_ms": int(input_s * 1000)}, timeout=input_s + 2)
                    code, sobj = agent_call(sock, "status", timeout=2.0)
                    if code == 0:
                        input_after = pad_counters(extract_result(sobj))
                        st = extract_result(sobj)
                        present_now = int(st.get("present") or 0)
                        if present_at_stable_start is not None:
                            present_delta = max(present_delta, present_now - int(present_at_stable_start))
                    phase_input_done = True
                    notes.append("input_sequence_done")

            # Capture once input attempted or presents stable
            if agent_ready and first_present is not None and not phase_capture_done:
                if phase_input_done or phase_stable_done:
                    ccode, cobj = agent_call(
                        sock, "capture", {"timeout_ms": 15000, "score": True}, timeout=20.0
                    )
                    if ccode == 0:
                        cres = extract_result(cobj)
                        path = str(cres.get("path") or "")
                        if path:
                            # Prefer anonymous basename in summary (no title IDs).
                            cap_src = Path(path)
                            if cap_src.is_file():
                                present_tag = int(cres.get("present") or 0)
                                dest = capture_dir / f"capture_present_{present_tag}.bmp"
                                try:
                                    dest.write_bytes(cap_src.read_bytes())
                                except OSError:
                                    dest = cap_src
                                capture_rel = dest.name
                                report.timeline.append(
                                    {
                                        "t": round(time.monotonic() - started, 3),
                                        "event": "capture",
                                        "file": capture_rel,
                                        "present": cres.get("present"),
                                    }
                                )
                                # Visual metrics via shipped kyty_capture.score_image
                                try:
                                    metrics = capture_mod.score_image(dest if dest.is_file() else cap_src)
                                    (run_dir / "capture_metrics.json").write_text(
                                        json.dumps(sanitize_obj(metrics, guest_root), indent=2) + "\n",
                                        encoding="utf-8",
                                    )
                                    if create_baseline or baseline_missing:
                                        if create_baseline:
                                            floor = visual_floor_from_metrics(metrics)
                                            compare = {
                                                "pass": bool(floor.get("pass")),
                                                "checks": floor.get("checks") or {},
                                                "delta": {},
                                            }
                                            if floor.get("pass"):
                                                baseline_path.parent.mkdir(parents=True, exist_ok=True)
                                                # Store metrics only (no private paths)
                                                baseline_payload = {
                                                    "schema": "kyty_playable_visual_baseline_v1",
                                                    "world": metrics["world"],
                                                    "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                                                }
                                                baseline_path.write_text(
                                                    json.dumps(baseline_payload, indent=2) + "\n", encoding="utf-8"
                                                )
                                                notes.append("baseline_created")
                                                baseline_missing = False
                                                compare["checks"] = {**compare["checks"], "baseline_created": True}
                                            else:
                                                notes.append("baseline_rejected_visual_floor")
                                    if baseline_path.is_file() and not create_baseline:
                                        bas = json.loads(baseline_path.read_text(encoding="utf-8"))
                                        # Support both raw score_image and wrapped baseline
                                        baseline_metrics = bas if "world" in bas else {"world": bas}
                                        current = {"world": metrics["world"]}
                                        compare = capture_mod.compare_metrics(current, baseline_metrics)
                                        # Playable regression: do not require scene_is_gameplay OCR
                                        # (title-specific OCR is diagnostic). Material regress gates only.
                                        material = material_visual_checks(compare)
                                        compare = {
                                            "pass": all(material.values()),
                                            "checks": material,
                                            "delta": compare.get("delta") or {},
                                            "raw_checks": checks,
                                        }
                                        notes.append("visual_compare_done")
                                except Exception as exc:  # noqa: BLE001 — record and fail visual gate
                                    notes.append(f"visual_metrics_error:{type(exc).__name__}")
                                    compare = {"pass": False, "error": type(exc).__name__}
                    else:
                        notes.append("capture_failed")
                    phase_capture_done = True

            # Done when core phases complete
            if phase_stable_done and phase_input_done and phase_capture_done:
                notes.append("session_milestones_complete")
                break

            time.sleep(0.35)
        else:
            timed_out = True
            notes.append("total_deadline")

    finally:
        if proc.poll() is None:
            kill_process_group(proc, notes)
            if timed_out or any(n.endswith("_deadline") for n in notes):
                timed_out = True
        exit_code = proc.poll()
        try:
            if sock.exists():
                sock.unlink()
        except OSError:
            pass

    report.child_exit = exit_code
    report.shutdown = classify_exit(exit_code, timed_out)
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
        baseline_missing=baseline_missing and not create_baseline,
        create_baseline=create_baseline and bool(capture_rel),
        shutdown=report.shutdown,
        first_error=report.first_error,
        milestones=milestones,
        visual_require_baseline=visual_req,
    )

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
    parser.add_argument("--profile", default="", help="JSON profile with deadlines/milestones only")
    parser.add_argument("--fc-script", default="_build_linux/fc_script")
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

    rc, report = run_session(
        repo_root=repo_root,
        guest_root=guest_root,
        fc_script=fc,
        profile=profile,
        scratch=scratch,
        baseline_path=baseline,
        create_baseline=args.create_baseline,
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
