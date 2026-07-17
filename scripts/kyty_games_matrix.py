#!/usr/bin/env python3
"""
Dynamic multi-game integration matrix.

Discovers guest package roots under KYTY_GAMES_ROOT (no hard-coded title names),
launches each valid root once in strict or explicitly diagnostic bring-up mode,
observes progress through the existing kyty_agent socket protocol, and writes:

  - raw evidence under an untracked scratch directory
  - a sanitized summary with anonymous stable case IDs (no private paths)

Usage (from repo root):

  KYTY_GAMES_ROOT=/path/to/Games \\
  KYTY_MATRIX_SCRATCH=/untracked/scratch \\
  python3 scripts/kyty_games_matrix.py \\
    --fc-script _build_linux/fc_script

  # Discovery only (no launches):
  python3 scripts/kyty_games_matrix.py --discover-only --games-root /tmp/synth

Do not commit raw evidence or private paths. The summary never embeds absolute
guest roots or title identifiers.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Iterable, Optional

# --- Primary executable rules (must match GuestExecutableLocator) ------------

PRIMARY_NAMES = frozenset({"eboot.bin"})

FAILED_ATTEMPT_OUTCOMES = frozenset(
    {
        "launch_failed",
        "loader_failed",
        "unsupported",
        "host_crash",
    }
)

_TITLE_ID_RE = re.compile(r"\b(?:PPSA|CUSA)\d+\b", re.IGNORECASE)
_WINDOWS_PATH_RE = re.compile(r"(?<![\w])(?:[A-Za-z]:[\\/])(?:[^\s\"'<>|]+)")
_POSIX_PATH_RE = re.compile(r"(?<![\w])/(?:[^\s\"'<>]+)")

# Outcome vocabulary (required)
OUTCOMES = frozenset(
    {
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
)

# Env keys that must NOT appear in a strict matrix child.
FORBIDDEN_CHILD_ENV = (
    "KYTY_BRINGUP_MODE",
    "KYTY_BRINGUP_FEATURES",
    "KYTY_BRINGUP_SUBSYSTEMS",
    "KYTY_BRINGUP_BURST_LIMIT",
    "KYTY_BRINGUP_BURST_WINDOW_MS",
    "KYTY_BRINGUP_ALLOW_DIAGNOSTIC",
    "KYTY_STUB_MISSING",
    "KYTY_GFX_PERMISSIVE",
)


def is_primary_executable_name(name: str) -> bool:
    return name in PRIMARY_NAMES


def package_root_from_primary(primary: Path) -> Path:
    return primary.resolve().parent


def discover_game_roots(games_root: Path, max_depth: int = 6) -> list[Path]:
    """
    Recursively discover package roots: any directory that contains a primary
    executable (eboot.bin) at that directory's top level.

    Does not hardcode title names. Nested packages under an already-selected
    root are still found if they contain their own primary (walk continues
    only into non-package dirs? — we record every dir with a primary and do
    not descend into package contents for additional eboots one level down
    only if they're the same package). Policy: record the directory that
    *directly* contains the primary file.
    """
    games_root = games_root.resolve()
    if not games_root.is_dir():
        return []

    found: list[Path] = []
    # Walk top-down; when we find a primary in a dir, record it and still
    # continue (nested packages are rare but allowed).
    for dirpath, dirnames, filenames in os.walk(games_root):
        rel = Path(dirpath).resolve().relative_to(games_root)
        depth = 0 if str(rel) == "." else len(rel.parts)
        if depth > max_depth:
            dirnames[:] = []
            continue
        # Skip hidden / VCS noise
        dirnames[:] = [d for d in dirnames if not d.startswith(".") and d not in (".git", "_scratch", "node_modules")]
        primaries = [f for f in filenames if is_primary_executable_name(f)]
        if primaries:
            found.append(Path(dirpath).resolve())
            # Do not descend into known package bulk (optional prune for speed)
            for prune in ("sce_module", "modules", "Media", "sce_sys"):
                if prune in dirnames:
                    dirnames.remove(prune)
    # Stable order by path string (deterministic discovery)
    found = sorted(set(found), key=lambda p: str(p).lower())
    return found


def classify_candidate(root: Path) -> tuple[bool, str]:
    """Return (is_valid_package, reason). Invalid → invalid_fixture."""
    if not root.is_dir():
        return False, "not_a_directory"
    primaries = [p for p in root.iterdir() if p.is_file() and is_primary_executable_name(p.name)]
    if not primaries:
        return False, "missing_primary_executable"
    # Soft package signal: sce_sys optional but preferred
    return True, "ok"


def anonymous_case_id(games_root: Path, package_root: Path) -> str:
    """Stable anonymous ID: sha256 of relative path, never the path itself."""
    try:
        rel = package_root.resolve().relative_to(games_root.resolve()).as_posix()
    except ValueError:
        rel = package_root.name
    digest = hashlib.sha256(rel.encode("utf-8")).hexdigest()
    return f"case_{digest[:12]}"


@dataclass
class CaseResult:
    case_id: str
    outcome: str
    first_frontier: str
    valid_package: bool
    attempted: bool
    progressed: bool  # reached runtime_started or later
    child_exit: Optional[int] = None
    agent_phase: str = ""
    agent_frontier: str = ""
    present: int = 0
    notes: list[str] = field(default_factory=list)

    def to_sanitized_dict(self) -> dict[str, Any]:
        return {
            "case_id": self.case_id,
            "outcome": self.outcome,
            "first_frontier": self.first_frontier,
            "valid_package": self.valid_package,
            "attempted": self.attempted,
            "progressed": self.progressed,
            "child_exit": self.child_exit,
            "agent_phase": self.agent_phase,
            "agent_frontier": self.agent_frontier,
            "present": self.present,
            "notes": list(self.notes),
        }


def clean_child_env(
    base: dict[str, str],
    *,
    guest_root: Path,
    agent_sock: Path,
    capture_dir: Path,
    mode: str = "strict",
) -> dict[str, str]:
    """Build an isolated strict or explicitly diagnostic bring-up environment."""
    # Start from a minimal host set needed for Vulkan/display, not a full parent clone.
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
    # Explicitly ensure forbidden keys are absent
    for k in FORBIDDEN_CHILD_ENV:
        env.pop(k, None)
    if mode == "bringup":
        env["KYTY_BRINGUP_MODE"] = "unsafe"
        env["KYTY_BRINGUP_ALLOW_DIAGNOSTIC"] = "1"
        env["KYTY_BRINGUP_FEATURES"] = (
            "not_implemented,missing_function_import,gfx_permissive,adjacent_module_discovery"
        )
    return env


def assert_strict_env(env: dict[str, str]) -> list[str]:
    bad = [k for k in FORBIDDEN_CHILD_ENV if k in env]
    return bad


def clear_untrusted_pid_file(path: Path, notes: list[str]) -> None:
    """Remove stale metadata without ever signalling a PID we did not create."""
    if not path.exists():
        return
    try:
        path.unlink()
        notes.append("untrusted_stale_pid_file_removed")
    except OSError as exc:
        notes.append(f"stale_pid_unlink_failed:{type(exc).__name__}")


def sock_is_stale(path: Path) -> bool:
    if not path.exists():
        return False
    # Connect attempt: refused/not-a-socket → stale leftover
    try:
        if not path.is_socket() and not path.is_file():
            return True
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.2)
        try:
            s.connect(str(path))
            s.close()
            return False  # live
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


def path_identity(path: Path) -> Optional[tuple[int, int]]:
    try:
        stat = path.stat()
    except OSError:
        return None
    return stat.st_dev, stat.st_ino


def unlink_owned_socket(
    path: Path,
    expected_identity: Optional[tuple[int, int]],
    notes: list[str],
) -> None:
    if expected_identity is None or not path.exists():
        return
    if path_identity(path) != expected_identity:
        notes.append("socket_identity_changed")
        return
    try:
        path.unlink()
    except OSError as exc:
        notes.append(f"socket_unlink_failed:{type(exc).__name__}")


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
        try:
            proc.wait(timeout=5)
            notes.append("killed_sigkill")
        except subprocess.TimeoutExpired:
            notes.append("kill_timeout_after_sigkill")
    except ProcessLookupError:
        notes.append("process_already_gone")
    except OSError as exc:
        notes.append(f"kill_failed:{type(exc).__name__}")


def agent_call(
    sock: Path,
    tool: str,
    args: Optional[dict[str, Any]] = None,
    timeout: float = 5.0,
) -> tuple[int, dict[str, Any]]:
    """Call a diagnostic tool through the authoritative agent socket protocol."""
    req = {"id": 1, "tool": tool, "args": args or {}}
    s: Optional[socket.socket] = None
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(str(sock))
        payload = json.dumps(req, separators=(",", ":")) + "\n"
        s.sendall(payload.encode("utf-8"))
        data = b""
        while b"\n" not in data and len(data) < 65536:
            chunk = s.recv(4096)
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


def extract_status(result_obj: dict[str, Any]) -> dict[str, Any]:
    if "result" in result_obj and isinstance(result_obj["result"], dict):
        return result_obj["result"]
    return result_obj


def usable_label(*parts: Any) -> str:
    """First non-empty actionable label; ignore placeholder frontiers like 'none'."""
    placeholders = {"", "none", "null", "unknown", "n/a", "na"}
    for part in parts:
        s = str(part or "").strip()
        if s.lower() not in placeholders:
            return s
    return ""


def sanitize_summary_text(value: str) -> str:
    """Remove host paths and common private title identifiers from one string."""
    sanitized = _WINDOWS_PATH_RE.sub("[redacted-path]", value)
    sanitized = _POSIX_PATH_RE.sub("[redacted-path]", sanitized)
    return _TITLE_ID_RE.sub("[redacted-title-id]", sanitized)


def sanitize_summary_value(value: Any) -> Any:
    """Recursively sanitize every string in a summary-compatible value."""
    if isinstance(value, str):
        return sanitize_summary_text(value)
    if isinstance(value, list):
        return [sanitize_summary_value(item) for item in value]
    if isinstance(value, tuple):
        return [sanitize_summary_value(item) for item in value]
    if isinstance(value, dict):
        return {str(key): sanitize_summary_value(item) for key, item in value.items()}
    return value


def first_actionable_from_log(log_path: Path) -> str:
    """
    Extract a single short host/guest error label from a child log.
    Skips stack dumps and absolute paths; returns '' when nothing useful is found.
    """
    if not log_path.is_file():
        return ""
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    # Prefer structured assert banners over noise
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("---") or s.startswith("["):
            continue
        if s.startswith(" in /"):
            continue
        if s.startswith("===") and s.endswith("==="):
            # e.g. === Unexpected non-Func after import validation Ok ===
            inner = s.strip("= ").strip()
            if inner:
                return inner[:160]
        if "EXIT" in s and ":" in s:
            message = s.split(" in /", 1)[0]
            return sanitize_summary_text(message)[:160]
        lower = s.lower()
        if (
            "unsupported" in lower
            or "not implemented" in lower
            or "assert" in lower
            or "fatal" in lower
            or "unknown " in lower
        ):
            message = s.split(" in /", 1)[0]
            return sanitize_summary_text(message)[:160]
    return ""


def map_outcome(
    *,
    process_exit: Optional[int],
    agent_ready: bool,
    status: dict[str, Any],
    last_error_code: str,
    timed_out: bool,
    notes: list[str],
) -> tuple[str, str]:
    """
    Map process + agent observation to (outcome, first_frontier).
    first_frontier is a short actionable label, not a flood of events.
    """
    del notes  # reserved for future signal weighting
    phase = str(status.get("phase") or "")
    frontier = usable_label(status.get("frontier"))
    present = int(status.get("present") or 0)
    graphic = bool(status.get("graphic_ready"))
    err = usable_label(last_error_code)

    if err in ("bringup_halt", "bringup_breaker", "unsupported") or frontier == "unsupported":
        return "unsupported", usable_label(frontier, err, "unsupported")

    # Signal-style exits are host crashes. Exit 65 is also used by strict
    # unsupported paths and therefore requires explicit crash evidence.
    if process_exit in (139, 132, 134, 136, 321) or err in ("host_crash", "host_fault"):
        return "host_crash", usable_label(err, frontier, "host_crash")

    if process_exit is not None and process_exit != 0:
        if not agent_ready:
            return "launch_failed", "launch"
        return "loader_failed", usable_label(frontier, err, "post_agent_exit")

    if not agent_ready:
        if process_exit == 0:
            return "guest_exit", "guest_exit_before_agent"
        if timed_out:
            return "launch_failed", "agent_ready_timeout"
        return "launch_failed", "agent_not_ready"

    # Agent was ready at some point
    if phase == "interactive" or frontier == "interactive":
        oc = "controlled_timeout" if timed_out else "interactive"
        return oc, "interactive"
    if present > 0:
        if timed_out:
            return "controlled_timeout", "presenting"
        return "presenting", "presenting"
    if graphic or frontier == "graphics":
        if timed_out:
            return "controlled_timeout", "video_initialized"
        return "video_initialized", "video_initialized"
    if agent_ready:
        if process_exit == 0:
            return "guest_exit", usable_label(frontier, "guest_exit")
        if timed_out:
            return "controlled_timeout", "runtime_started"
        return "runtime_started", "runtime_started"

    if timed_out:
        return "controlled_timeout", usable_label(frontier, "timeout")
    return "launch_failed", usable_label(frontier, "unknown")


def agent_sock_path(case_id: str, case_scratch: Path, run_nonce: str) -> Path:
    """
    AF_UNIX sun_path is ~108 bytes including NUL. Long scratch trees overflow
    that limit and the agent fails to listen. Prefer a short /tmp path keyed by
    case and run identity; fall back to case_scratch only when it still fits.
    """
    identity = f"{run_nonce}:{case_id}"
    digest = hashlib.sha256(identity.encode("utf-8")).hexdigest()[:20]
    short = Path(f"/tmp/kyty_mx_{digest}.sock")
    if len(str(short)) < 100:
        return short
    candidate = (case_scratch / "a.sock").resolve()
    if len(str(candidate)) < 100:
        return candidate
    # Last resort: hash-only name under /tmp
    return Path(f"/tmp/kyty_mx_{digest}.sock")


def sample_final_status(
    sock: Path,
    *,
    agent_ready: bool,
    current: dict[str, Any],
) -> dict[str, Any]:
    """Best-effort final observation performed before process teardown."""
    if not agent_ready or sock_is_stale(sock):
        return current
    code, obj = agent_call(sock, "status", timeout=1.0)
    if code != 0:
        return current
    return extract_status(obj)


def run_one_case(
    *,
    case_id: str,
    package_root: Path,
    repo_root: Path,
    fc_script: Path,
    case_scratch: Path,
    startup_deadline_s: float,
    present_deadline_s: float,
    total_deadline_s: float,
    stability_s: float,
    run_nonce: str,
    mode: str,
) -> CaseResult:
    notes: list[str] = []
    case_scratch.mkdir(parents=True, exist_ok=True)
    sock = agent_sock_path(case_id, case_scratch, run_nonce)
    # Record sock path for debugging without embedding guest titles
    (case_scratch / "agent.sock.path").write_text(str(sock) + "\n", encoding="utf-8")
    capture_dir = case_scratch / "captures"
    capture_dir.mkdir(exist_ok=True)
    log_path = case_scratch / "child.log"
    pid_path = case_scratch / "child.pid"
    env_dump = case_scratch / "child_env_keys.txt"

    if sock.exists():
        return CaseResult(
            case_id=case_id,
            outcome="launch_failed",
            first_frontier="socket_path_collision",
            valid_package=True,
            attempted=True,
            progressed=False,
            notes=["socket_path_collision"],
        )
    clear_untrusted_pid_file(pid_path, notes)

    env = clean_child_env(
        dict(os.environ),
        guest_root=package_root,
        agent_sock=sock,
        capture_dir=capture_dir,
        mode=mode,
    )
    bad = assert_strict_env(env) if mode == "strict" else []
    if bad:
        notes.append("strict_env_violation:" + ",".join(bad))
    # Evidence of strict purity (keys only — no secret values / paths)
    env_dump.write_text("\n".join(sorted(env.keys())) + "\n", encoding="utf-8")
    for k in FORBIDDEN_CHILD_ENV:
        if mode == "strict" and k in env:
            return CaseResult(
                case_id=case_id,
                outcome="launch_failed",
                first_frontier="strict_env_violation",
                valid_package=True,
                attempted=False,
                progressed=False,
                notes=notes,
            )

    cmd = [str(fc_script), "scripts/run_guest.lua", str(package_root)]
    started = time.monotonic()
    total_deadline = started + total_deadline_s
    startup_deadline = started + startup_deadline_s
    present_deadline = started + present_deadline_s

    with log_path.open("w", encoding="utf-8") as log:
        proc = subprocess.Popen(
            cmd,
            cwd=str(repo_root),
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=(os.name == "posix"),
        )
    pid_path.write_text(str(proc.pid), encoding="utf-8")

    agent_ready = False
    status: dict[str, Any] = {}
    last_error_code = ""
    best_progress = "launch"
    presenting_since: Optional[float] = None
    timed_out = False
    present_deadline_noted = False
    owned_socket_identity: Optional[tuple[int, int]] = None

    try:
        while time.monotonic() < total_deadline:
            rc = proc.poll()
            if rc is not None:
                break

            now = time.monotonic()
            if owned_socket_identity is None:
                owned_socket_identity = path_identity(sock)
            # Agent ready poll
            if not agent_ready:
                if now > startup_deadline:
                    notes.append("startup_deadline")
                    timed_out = True
                    break
                code, obj = agent_call(sock, "ping", timeout=1.0)
                if code == 0:
                    agent_ready = True
                    best_progress = "runtime_started"
                    notes.append("agent_ready")
                time.sleep(0.25)
                continue

            code, obj = agent_call(sock, "status", timeout=2.0)
            if code == 0:
                status = extract_status(obj)
                phase = str(status.get("phase") or "")
                frontier = str(status.get("frontier") or "")
                present = int(status.get("present") or 0)
                graphic = bool(status.get("graphic_ready"))
                if graphic and best_progress in ("launch", "runtime_started"):
                    best_progress = "video_initialized"
                if present > 0:
                    best_progress = "presenting"
                    if presenting_since is None:
                        presenting_since = now
                if phase == "interactive" or frontier == "interactive":
                    best_progress = "interactive"
                    # Optional short stability then stop (matrix is bounded)
                    if presenting_since is not None and (now - presenting_since) >= stability_s:
                        notes.append("stability_reached")
                        break

            # last_error for unsupported
            ecode, eobj = agent_call(sock, "last_error", timeout=1.0)
            if ecode == 0:
                ev = extract_status(eobj).get("event")
                if isinstance(ev, dict) and ev.get("code"):
                    last_error_code = str(ev.get("code"))
                    if last_error_code in ("bringup_halt", "bringup_breaker"):
                        best_progress = "unsupported"
                        notes.append("unsupported_from_last_error")
                        break

            if now > present_deadline and best_progress in ("runtime_started", "video_initialized", "launch"):
                if not present_deadline_noted:
                    notes.append("present_deadline")
                    present_deadline_noted = True
                # do not break immediately if still making agent progress; mark for classification
                if best_progress == "runtime_started":
                    timed_out = True
                    break

            time.sleep(0.5)
        else:
            timed_out = True
            notes.append("total_deadline")

    finally:
        if owned_socket_identity is None:
            owned_socket_identity = path_identity(sock)
        status = sample_final_status(
            sock,
            agent_ready=agent_ready,
            current=status,
        )
        if proc.poll() is None:
            kill_process_group(proc, notes)
        exit_code = proc.poll()
        unlink_owned_socket(sock, owned_socket_identity, notes)
        try:
            pid_path.unlink()
        except OSError:
            pass

    outcome, first_frontier = map_outcome(
        process_exit=exit_code,
        agent_ready=agent_ready,
        status=status,
        last_error_code=last_error_code,
        timed_out=timed_out,
        notes=notes,
    )
    # Prefer best progress as frontier when more advanced than map default
    progress_rank = {
        "launch": 0,
        "runtime_started": 1,
        "video_initialized": 2,
        "presenting": 3,
        "interactive": 4,
        "unsupported": 5,
    }
    if progress_rank.get(best_progress, 0) >= progress_rank.get(first_frontier, 0) and best_progress != "launch":
        if outcome not in ("host_crash", "unsupported", "launch_failed", "loader_failed"):
            first_frontier = best_progress

    # One actionable host/assert label beats a generic host_crash token
    if outcome in ("host_crash", "loader_failed", "unsupported", "launch_failed"):
        log_label = first_actionable_from_log(log_path)
        if log_label and (
            outcome == "host_crash"
            or first_frontier in ("", "host_crash", "loader_failed", "launch", "post_agent_exit")
        ):
            first_frontier = log_label

    progressed = outcome in (
        "runtime_started",
        "video_initialized",
        "presenting",
        "interactive",
        "controlled_timeout",
        "guest_exit",
    ) or best_progress in ("runtime_started", "video_initialized", "presenting", "interactive")

    # If we only got agent ready, ensure outcome reflects runtime_started min
    if agent_ready and outcome == "launch_failed":
        outcome = "runtime_started"
        first_frontier = first_frontier or "runtime_started"
        progressed = True

    return CaseResult(
        case_id=case_id,
        outcome=outcome,
        first_frontier=first_frontier or best_progress,
        valid_package=True,
        attempted=True,
        progressed=progressed,
        child_exit=exit_code,
        agent_phase=str(status.get("phase") or ""),
        agent_frontier=str(status.get("frontier") or ""),
        present=int(status.get("present") or 0),
        notes=notes,
    )


def write_summary(path: Path, results: list[CaseResult], meta: dict[str, Any], *, mode: str) -> None:
    # Progressed = agent-observed runtime or later (including later host_crash).
    before = [r for r in results if not r.progressed]
    after = [r for r in results if r.progressed]
    payload = {
        "schema": "kyty_games_matrix_summary_v1",
        "mode": mode,
        "protocol": "kyty_agent",
        "meta": meta,
        "counts": {
            "total": len(results),
            "valid_packages": sum(1 for r in results if r.valid_package),
            "attempted": sum(1 for r in results if r.attempted),
            "failed_before_runtime": len(before),
            "progressed": len(after),
            "by_outcome": {},
        },
        "failed_before_runtime": [r.to_sanitized_dict() for r in before],
        "progressed": [r.to_sanitized_dict() for r in after],
        "all_cases": [r.to_sanitized_dict() for r in results],
    }
    by: dict[str, int] = {}
    for r in results:
        by[r.outcome] = by.get(r.outcome, 0) + 1
    payload["counts"]["by_outcome"] = by
    # Ensure no absolute private paths leaked into summary text
    text = json.dumps(sanitize_summary_value(payload), indent=2, sort_keys=True)
    path.write_text(text + "\n", encoding="utf-8")


def new_run_id() -> str:
    timestamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    nonce = uuid.uuid4().hex[:12]
    return f"{timestamp}_{time.time_ns():x}_{nonce}"


def matrix_exit_code(results: list[CaseResult], *, discover_only: bool) -> int:
    if discover_only:
        return 0
    attempted = [result for result in results if result.attempted]
    if any(result.outcome in FAILED_ATTEMPT_OUTCOMES for result in attempted):
        return 1
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Kyty multi-game integration matrix runner")
    parser.add_argument(
        "--games-root",
        default=os.environ.get("KYTY_GAMES_ROOT", ""),
        help="Root to scan (or set KYTY_GAMES_ROOT). Never written into tracked summaries as absolute paths.",
    )
    parser.add_argument(
        "--scratch",
        default=os.environ.get("KYTY_MATRIX_SCRATCH", ""),
        help="Untracked scratch for raw logs (default: <repo>/_matrix_scratch)",
    )
    parser.add_argument("--fc-script", default="_build_linux/fc_script")
    parser.add_argument("--repo-root", default="")
    parser.add_argument("--discover-only", action="store_true")
    parser.add_argument("--mode", choices=("strict", "bringup"), default="strict")
    parser.add_argument("--max-depth", type=int, default=6)
    parser.add_argument("--startup-deadline-s", type=float, default=float(os.environ.get("KYTY_MATRIX_STARTUP_S", "45")))
    parser.add_argument("--present-deadline-s", type=float, default=float(os.environ.get("KYTY_MATRIX_PRESENT_S", "90")))
    parser.add_argument("--total-deadline-s", type=float, default=float(os.environ.get("KYTY_MATRIX_TOTAL_S", "120")))
    parser.add_argument("--stability-s", type=float, default=float(os.environ.get("KYTY_MATRIX_STABILITY_S", "10")))
    parser.add_argument("--max-cases", type=int, default=0, help="0 = all discovered")
    args = parser.parse_args(argv)

    repo_root = Path(args.repo_root or Path(__file__).resolve().parents[1]).resolve()
    games_root_s = args.games_root or os.environ.get("KYTY_GAMES_ROOT", "")
    if not games_root_s:
        print("error: set --games-root or KYTY_GAMES_ROOT", file=sys.stderr)
        return 2
    games_root = Path(games_root_s).expanduser().resolve()

    scratch = Path(args.scratch or (repo_root / "_matrix_scratch")).expanduser().resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    run_id = new_run_id()
    run_dir = scratch / f"run_{run_id}"
    run_dir.mkdir(parents=True, exist_ok=True)

    fc_script = (repo_root / args.fc_script).resolve() if not Path(args.fc_script).is_absolute() else Path(args.fc_script)

    roots = discover_game_roots(games_root, max_depth=args.max_depth)
    # Also scan immediate children that are invalid (no primary) for classification
    # so the summary can show invalid_fixture counts from top-level entries.
    top_level = [p for p in games_root.iterdir() if p.is_dir() and not p.name.startswith(".")] if games_root.is_dir() else []

    results: list[CaseResult] = []
    seen_ids: set[str] = set()

    # Map discovered roots
    valid_roots = []
    for root in roots:
        ok, reason = classify_candidate(root)
        cid = anonymous_case_id(games_root, root)
        if cid in seen_ids:
            continue
        seen_ids.add(cid)
        if not ok:
            results.append(
                CaseResult(
                    case_id=cid,
                    outcome="invalid_fixture",
                    first_frontier=reason,
                    valid_package=False,
                    attempted=False,
                    progressed=False,
                    notes=[reason],
                )
            )
        else:
            valid_roots.append((cid, root))

    # Top-level dirs without primary → invalid_fixture (not double-counting valid)
    valid_set = {r.resolve() for _, r in valid_roots}
    for d in sorted(top_level, key=lambda p: p.name.lower()):
        if d.resolve() in valid_set:
            continue
        # If a nested valid package exists under d, still report d as non-package container
        ok, reason = classify_candidate(d)
        if ok:
            continue
        cid = anonymous_case_id(games_root, d)
        if cid in seen_ids:
            continue
        seen_ids.add(cid)
        results.append(
            CaseResult(
                case_id=cid,
                outcome="invalid_fixture",
                first_frontier=reason,
                valid_package=False,
                attempted=False,
                progressed=False,
                notes=[reason],
            )
        )

    discover_report = {
        "schema": "kyty_games_matrix_discover_v1",
        "valid_count": len(valid_roots),
        "invalid_count": sum(1 for r in results if r.outcome == "invalid_fixture"),
        "case_ids": [cid for cid, _ in valid_roots] + [r.case_id for r in results if r.outcome == "invalid_fixture"],
    }
    (run_dir / "discover.json").write_text(json.dumps(discover_report, indent=2) + "\n", encoding="utf-8")

    if args.discover_only:
        # Discover-only never launches. Valid packages are pending (not failures).
        pending_ids = [cid for cid, _ in valid_roots]
        sanitized_pending = [
            {
                "case_id": cid,
                "valid_package": True,
                "attempted": False,
                "progressed": False,
                "first_frontier": "discover_only",
                "notes": ["discover_only_pending"],
            }
            for cid in pending_ids
        ]
        by: dict[str, int] = {}
        for r in results:
            by[r.outcome] = by.get(r.outcome, 0) + 1
        payload = {
            "schema": "kyty_games_matrix_summary_v1",
            "mode": args.mode,
            "protocol": "kyty_agent",
            "meta": {
                "discover_only": True,
                "valid_count": len(valid_roots),
                "pending_case_ids": pending_ids,
                "run_id": run_id,
            },
            "counts": {
                "total": len(results) + len(valid_roots),
                "valid_packages": len(valid_roots),
                "attempted": 0,
                "failed_before_runtime": sum(1 for r in results if not r.progressed),
                "progressed": 0,
                "pending_discover_only": len(valid_roots),
                "by_outcome": by,
            },
            "failed_before_runtime": [r.to_sanitized_dict() for r in results if not r.progressed],
            "progressed": [],
            "pending": sanitized_pending,
            "all_cases": [r.to_sanitized_dict() for r in results] + sanitized_pending,
        }
        text = json.dumps(sanitize_summary_value(payload), indent=2, sort_keys=True)
        (run_dir / "summary.json").write_text(text + "\n", encoding="utf-8")
        print(json.dumps(discover_report, indent=2))
        print(f"summary={run_dir / 'summary.json'}", file=sys.stderr)
        return 0

    if not fc_script.is_file():
        print("error: missing fc_script under build", file=sys.stderr)
        return 2

    if args.max_cases > 0:
        valid_roots = valid_roots[: args.max_cases]

    for cid, root in valid_roots:
        case_dir = run_dir / cid
        try:
            result = run_one_case(
                case_id=cid,
                package_root=root,
                repo_root=repo_root,
                fc_script=fc_script,
                case_scratch=case_dir,
                startup_deadline_s=args.startup_deadline_s,
                present_deadline_s=args.present_deadline_s,
                total_deadline_s=args.total_deadline_s,
                stability_s=args.stability_s,
                run_nonce=run_id,
                mode=args.mode,
            )
        except Exception as exc:  # continue matrix after unexpected parent errors
            result = CaseResult(
                case_id=cid,
                outcome="launch_failed",
                first_frontier="parent_exception",
                valid_package=True,
                attempted=True,
                progressed=False,
                notes=[type(exc).__name__],
            )
        results.append(result)
        # Per-case sanitized mini log (no paths)
        (case_dir / "result.json").write_text(
            json.dumps(result.to_sanitized_dict(), indent=2) + "\n", encoding="utf-8"
        )

    summary_path = run_dir / "summary.json"
    write_summary(
        summary_path,
        results,
        mode=args.mode,
        meta={
            "discover_only": False,
            "valid_count": len(valid_roots),
            "run_id": run_id,
            "startup_deadline_s": args.startup_deadline_s,
            "present_deadline_s": args.present_deadline_s,
            "total_deadline_s": args.total_deadline_s,
            "strict": args.mode == "strict",
        },
    )
    # Print only sanitized summary to stdout
    print(summary_path.read_text(encoding="utf-8"))
    print(f"raw_evidence_dir={run_dir}", file=sys.stderr)
    return matrix_exit_code(results, discover_only=False)


if __name__ == "__main__":
    sys.exit(main())
