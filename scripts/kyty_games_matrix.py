#!/usr/bin/env python3
"""
Dynamic multi-game strict integration matrix.

Discovers guest package roots under KYTY_GAMES_ROOT (no hard-coded title names),
launches each valid root once in a clean strict child process, observes progress
via the existing kyty_agent protocol only, and writes:

  - raw evidence under an untracked scratch directory
  - a sanitized summary with anonymous stable case IDs (no private paths)

Usage (from repo root):

  KYTY_GAMES_ROOT=/path/to/Games \\
  KYTY_MATRIX_SCRATCH=/untracked/scratch \\
  python3 scripts/kyty_games_matrix.py \\
    --fc-script _build_linux/fc_script \\
    --kyty-agent _build_linux/agent/kyty_agent

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
import signal
import socket
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Iterable, Optional

# --- Primary executable rules (must match GuestExecutableLocator) ------------

PRIMARY_NAMES = frozenset({"eboot.bin", "main.elf", "eboot.elf"})

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
    return name.lower() in PRIMARY_NAMES


def package_root_from_primary(primary: Path) -> Path:
    return primary.resolve().parent


def discover_game_roots(games_root: Path, max_depth: int = 6) -> list[Path]:
    """
    Recursively discover package roots: any directory that contains a primary
    executable (eboot.bin / main.elf / eboot.elf) at that directory's top level.

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
) -> dict[str, str]:
    """Strict-only child env: no BringUp/unsafe flags."""
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
    return env


def assert_strict_env(env: dict[str, str]) -> list[str]:
    bad = [k for k in FORBIDDEN_CHILD_ENV if k in env]
    return bad


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


def agent_call(agent: Path, sock: Path, tool: str, args: Optional[dict[str, Any]] = None, timeout: float = 5.0) -> tuple[int, dict[str, Any]]:
    """Call agent tool over the unix socket protocol (agent path is reserved for CLI parity)."""
    del agent  # socket protocol is authoritative; CLI path kept for API stability
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
        if s.startswith(" in /") or "/home/" in s or "Documents/" in s:
            continue
        if s.startswith("===") and s.endswith("==="):
            # e.g. === Unexpected non-Func after import validation Ok ===
            inner = s.strip("= ").strip()
            if inner:
                return inner[:160]
        if "EXIT" in s and ":" in s:
            return s[:160]
        if "unsupported" in s.lower() or "assert" in s.lower() or "fatal" in s.lower():
            return s[:160]
    return ""


def is_controlled_kill_exit(process_exit: Optional[int]) -> bool:
    """True for SIGTERM/SIGKILL forms returned by Popen after killpg."""
    if process_exit is None:
        return False
    # Python POSIX: -signal; shell-style: 128+signal
    return process_exit in (-15, -9, 15, 9, 143, 137)


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

    When the parent hits a deadline it sets timed_out and killpg's the child;
    poll() then returns -15/-9 (or 143/137). timed_out must win over that exit
    so the outcome is controlled_timeout, not loader_failed.
    """
    del notes  # reserved for future signal weighting
    phase = str(status.get("phase") or "")
    frontier = usable_label(status.get("frontier"))
    present = int(status.get("present") or 0)
    graphic = bool(status.get("graphic_ready"))
    err = usable_label(last_error_code)

    # Host crash exits (dbg_exit 321 → 65 on Unix wait, or 139 SIGSEGV).
    # SIGTERM/SIGKILL from parent killpg are not host crashes.
    if process_exit in (139, 132, 134, 136, 65, 321) and not timed_out:
        return "host_crash", usable_label(err, frontier, "host_crash")

    if err in ("bringup_halt", "bringup_breaker", "unsupported") or frontier == "unsupported":
        return "unsupported", usable_label(frontier, err, "unsupported")

    if not agent_ready:
        if timed_out:
            return "launch_failed", "agent_ready_timeout"
        if process_exit is not None and process_exit != 0:
            return "launch_failed", "launch"
        if process_exit == 0:
            return "guest_exit", "guest_exit_before_agent"
        return "launch_failed", "agent_not_ready"

    # Agent was ready — progress-ranked outcomes. timed_out always wins over
    # non-zero exit from parent killpg (process_exit=-15/-9 after deadline).
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

    # Critical: check timed_out BEFORE treating kill exit as loader_failed.
    if timed_out:
        return "controlled_timeout", "runtime_started"

    if process_exit == 0:
        return "guest_exit", usable_label(frontier, "guest_exit")
    if process_exit is not None and process_exit != 0:
        # Genuine post-agent failure (child died on its own, not parent deadline).
        if err or frontier in ("error", "unsupported", "launch"):
            return "loader_failed", usable_label(frontier, err, "loader_failed")
        return "loader_failed", "post_agent_exit"
    return "runtime_started", "runtime_started"


def agent_sock_path(case_id: str, case_scratch: Path) -> Path:
    """
    AF_UNIX sun_path is ~108 bytes including NUL. Long scratch trees overflow
    that limit and the agent fails to listen. Prefer a short /tmp path keyed by
    case_id; fall back to case_scratch only when it still fits.
    """
    short = Path(f"/tmp/kyty_mx_{case_id}.sock")
    if len(str(short)) < 100:
        return short
    candidate = (case_scratch / "a.sock").resolve()
    if len(str(candidate)) < 100:
        return candidate
    # Last resort: hash-only name under /tmp
    return Path(f"/tmp/kyty_mx_{hashlib.sha256(case_id.encode()).hexdigest()[:16]}.sock")


def run_one_case(
    *,
    case_id: str,
    package_root: Path,
    repo_root: Path,
    fc_script: Path,
    agent: Path,
    case_scratch: Path,
    startup_deadline_s: float,
    present_deadline_s: float,
    total_deadline_s: float,
    stability_s: float,
) -> CaseResult:
    notes: list[str] = []
    case_scratch.mkdir(parents=True, exist_ok=True)
    sock = agent_sock_path(case_id, case_scratch)
    # Record sock path for debugging without embedding guest titles
    (case_scratch / "agent.sock.path").write_text(str(sock) + "\n", encoding="utf-8")
    capture_dir = case_scratch / "captures"
    capture_dir.mkdir(exist_ok=True)
    log_path = case_scratch / "child.log"
    pid_path = case_scratch / "child.pid"
    env_dump = case_scratch / "child_env_keys.txt"

    remove_stale_socket(sock, notes)
    if pid_path.exists():
        try:
            old_pid = int(pid_path.read_text().strip())
            os.kill(old_pid, 0)
            notes.append("stale_pid_alive")
            try:
                os.kill(old_pid, signal.SIGKILL)
                notes.append("killed_stale_pid")
            except OSError:
                pass
        except (ValueError, ProcessLookupError, OSError):
            notes.append("stale_pid_file_cleared")
        try:
            pid_path.unlink()
        except OSError:
            pass

    env = clean_child_env(
        dict(os.environ),
        guest_root=package_root,
        agent_sock=sock,
        capture_dir=capture_dir,
    )
    bad = assert_strict_env(env)
    if bad:
        notes.append("strict_env_violation:" + ",".join(bad))
    # Evidence of strict purity (keys only — no secret values / paths)
    env_dump.write_text("\n".join(sorted(env.keys())) + "\n", encoding="utf-8")
    for k in FORBIDDEN_CHILD_ENV:
        if k in env:
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

    try:
        while time.monotonic() < total_deadline:
            rc = proc.poll()
            if rc is not None:
                break

            now = time.monotonic()
            # Agent ready poll
            if not agent_ready:
                if now > startup_deadline:
                    notes.append("startup_deadline")
                    timed_out = True
                    break
                code, obj = agent_call(agent, sock, "ping", timeout=1.0)
                if code == 0:
                    agent_ready = True
                    best_progress = "runtime_started"
                    notes.append("agent_ready")
                time.sleep(0.25)
                continue

            code, obj = agent_call(agent, sock, "status", timeout=2.0)
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
            ecode, eobj = agent_call(agent, sock, "last_error", timeout=1.0)
            if ecode == 0:
                ev = extract_status(eobj).get("event")
                if isinstance(ev, dict) and ev.get("code"):
                    last_error_code = str(ev.get("code"))
                    if last_error_code in ("bringup_halt", "bringup_breaker"):
                        best_progress = "unsupported"
                        notes.append("unsupported_from_last_error")
                        break

            if now > present_deadline and best_progress in ("runtime_started", "video_initialized", "launch"):
                notes.append("present_deadline")
                # do not break immediately if still making agent progress; mark for classification
                if best_progress == "runtime_started":
                    timed_out = True
                    break

            time.sleep(0.5)
        else:
            timed_out = True
            notes.append("total_deadline")

    finally:
        if proc.poll() is None:
            # Killing after a deadline must keep timed_out=True for classification.
            if any(
                n in notes
                for n in ("startup_deadline", "present_deadline", "total_deadline")
            ):
                timed_out = True
            kill_process_group(proc, notes)
        exit_code = proc.poll()
        try:
            if sock.exists():
                sock.unlink()
        except OSError:
            pass
        try:
            pid_path.unlink()
        except OSError:
            pass

    # Final status sample if possible (socket may be gone)
    if agent_ready and sock.exists() and not sock_is_stale(sock):
        code, obj = agent_call(agent, sock, "status", timeout=1.0)
        if code == 0:
            status = extract_status(obj)

    # If we killed via SIGTERM/SIGKILL after a deadline note, force timeout flag.
    if not timed_out and is_controlled_kill_exit(exit_code):
        if any(
            n in notes
            for n in ("startup_deadline", "present_deadline", "total_deadline", "killed_sigterm", "killed_sigkill")
        ) and any(n.endswith("_deadline") for n in notes):
            timed_out = True

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
        if outcome not in ("host_crash", "unsupported", "launch_failed"):
            first_frontier = best_progress

    # One actionable host/assert label beats a generic host_crash token
    if outcome in ("host_crash", "loader_failed", "unsupported", "launch_failed"):
        log_label = first_actionable_from_log(log_path)
        if log_label and first_frontier in ("", "host_crash", "loader_failed", "launch", "post_agent_exit"):
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


def write_summary(path: Path, results: list[CaseResult], meta: dict[str, Any]) -> None:
    # Progressed = agent-observed runtime or later (including later host_crash).
    before = [r for r in results if not r.progressed]
    after = [r for r in results if r.progressed]
    payload = {
        "schema": "kyty_games_matrix_summary_v1",
        "mode": "strict",
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
    text = json.dumps(payload, indent=2, sort_keys=True)
    private_markers = ("/home/", "Documents/PS5/Games", "PPSA", "CUSA")
    if any(m in text for m in private_markers):
        for bucket in ("all_cases", "failed_before_runtime", "progressed"):
            for r in payload[bucket]:
                r["notes"] = [
                    n
                    for n in r.get("notes", [])
                    if not any(m in n for m in private_markers)
                ]
                for key in ("first_frontier", "agent_frontier", "agent_phase"):
                    if any(m in str(r.get(key, "")) for m in private_markers):
                        r[key] = "redacted"
        text = json.dumps(payload, indent=2, sort_keys=True)
    path.write_text(text + "\n", encoding="utf-8")


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Kyty strict multi-game matrix runner")
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
    parser.add_argument("--kyty-agent", default="_build_linux/agent/kyty_agent")
    parser.add_argument("--repo-root", default="")
    parser.add_argument("--discover-only", action="store_true")
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
    run_id = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    run_dir = scratch / f"run_{run_id}"
    run_dir.mkdir(parents=True, exist_ok=True)

    fc_script = (repo_root / args.fc_script).resolve() if not Path(args.fc_script).is_absolute() else Path(args.fc_script)
    agent = (repo_root / args.kyty_agent).resolve() if not Path(args.kyty_agent).is_absolute() else Path(args.kyty_agent)

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
            "mode": "strict",
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
        text = json.dumps(payload, indent=2, sort_keys=True)
        if "/home/" in text or "Documents/PS5/Games" in text:
            raise SystemExit("error: private path leaked into discover-only summary")
        (run_dir / "summary.json").write_text(text + "\n", encoding="utf-8")
        print(json.dumps(discover_report, indent=2))
        print(f"summary={run_dir / 'summary.json'}", file=sys.stderr)
        return 0

    if not fc_script.is_file() or not agent.is_file():
        print(f"error: missing fc_script or kyty_agent under build", file=sys.stderr)
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
                agent=agent,
                case_scratch=case_dir,
                startup_deadline_s=args.startup_deadline_s,
                present_deadline_s=args.present_deadline_s,
                total_deadline_s=args.total_deadline_s,
                stability_s=args.stability_s,
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
        meta={
            "discover_only": False,
            "valid_count": len(valid_roots),
            "run_id": run_id,
            "startup_deadline_s": args.startup_deadline_s,
            "present_deadline_s": args.present_deadline_s,
            "total_deadline_s": args.total_deadline_s,
            "strict": True,
        },
    )
    # Print only sanitized summary to stdout
    print(summary_path.read_text(encoding="utf-8"))
    print(f"raw_evidence_dir={run_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
