#!/usr/bin/env python3
"""Shared process-boundary infrastructure for Kyty integration runners.

This module owns the strict child environment, the line-delimited agent
protocol, Unix-socket lifecycle checks, and process-group termination.  Runner
policy remains in each caller; transport and host-process behavior do not.
"""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any, Optional


HOST_ENVIRONMENT_KEYS = (
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

# Every permissive or automation-only switch is forbidden in strict children.
# Callers that intentionally run a diagnostic policy must add that policy only
# after constructing this clean environment.
STRICT_FORBIDDEN_ENVIRONMENT_KEYS = (
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


def build_child_environment(
    base: Mapping[str, str],
    *,
    guest_root: Path,
    capture_directory: Path,
    agent_socket: Optional[Path] = None,
    optional_values: Sequence[str] = (),
    default_values: Optional[Mapping[str, str]] = None,
) -> dict[str, str]:
    """Build the canonical minimal environment for a strict Kyty child.

    ``optional_values`` copies named non-empty values explicitly required by a
    runner. ``default_values`` copies the parent value when present and uses the
    supplied default otherwise. No unrelated parent state crosses the boundary.
    """
    env = {key: base[key] for key in HOST_ENVIRONMENT_KEYS if base.get(key)}
    env.update(
        {
            "KYTY_GUEST_ROOT": str(guest_root),
            "KYTY_NATIVE_CAPTURE_DIR": str(capture_directory),
            "KYTY_PRINTF_DIRECTION": "Silent",
            "KYTY_SCREEN_WIDTH": base.get("KYTY_SCREEN_WIDTH", "1280"),
            "KYTY_SCREEN_HEIGHT": base.get("KYTY_SCREEN_HEIGHT", "720"),
        }
    )
    if agent_socket is not None:
        env["KYTY_AGENT_ENDPOINT"] = str(agent_socket)
    for key in optional_values:
        if base.get(key):
            env[key] = base[key]
    for key, default in (default_values or {}).items():
        env[key] = base.get(key) or default
    for key in STRICT_FORBIDDEN_ENVIRONMENT_KEYS:
        env.pop(key, None)
    return env


def find_forbidden_environment_keys(env: Mapping[str, str]) -> list[str]:
    """Return strict-policy violations in stable declaration order."""
    return find_active_environment_keys(env, STRICT_FORBIDDEN_ENVIRONMENT_KEYS)


def find_active_environment_keys(
    env: Mapping[str, str],
    keys: Sequence[str],
    allowed: Sequence[str] = (),
) -> list[str]:
    """Return active policy keys, excluding an explicit allow-list."""
    allowed_keys = frozenset(allowed)
    return [key for key in keys if env.get(key) and key not in allowed_keys]


def is_stale_unix_socket(path: Path) -> bool:
    """Return whether an existing path is not backed by a live Unix listener."""
    if not path.exists():
        return False
    if not path.is_socket():
        return True
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(0.2)
            client.connect(str(path))
        return False
    except (ConnectionRefusedError, FileNotFoundError, OSError):
        return True


def remove_stale_unix_socket(path: Path, notes: list[str]) -> None:
    """Remove a stale Unix-socket path and record the observable outcome."""
    if not path.exists() or not is_stale_unix_socket(path):
        return
    try:
        path.unlink()
        notes.append("removed_stale_socket")
    except OSError as exc:
        notes.append(f"stale_socket_unlink_failed:{type(exc).__name__}")


def path_identity(path: Path) -> Optional[tuple[int, int]]:
    """Return a stable device/inode identity, or ``None`` if unavailable."""
    try:
        path_stat = path.stat()
    except OSError:
        return None
    return path_stat.st_dev, path_stat.st_ino


def unlink_owned_socket(
    path: Path,
    expected_identity: Optional[tuple[int, int]],
    notes: list[str],
) -> None:
    """Unlink only the exact socket instance previously created by the child."""
    if expected_identity is None or not path.exists():
        return
    if path_identity(path) != expected_identity:
        notes.append("socket_identity_changed")
        return
    try:
        path.unlink()
    except OSError as exc:
        notes.append(f"socket_unlink_failed:{type(exc).__name__}")


def launch_process_group(
    command: Sequence[str],
    *,
    cwd: Path,
    env: Mapping[str, str],
    stdout: Any,
) -> subprocess.Popen[Any]:
    """Launch one isolated child process group with the canonical stdio policy."""
    return subprocess.Popen(
        list(command),
        cwd=str(cwd),
        env=dict(env),
        stdout=stdout,
        stderr=subprocess.STDOUT,
        start_new_session=(os.name == "posix"),
    )


def terminate_process_group(proc: subprocess.Popen[Any], notes: list[str]) -> None:
    """Terminate a child process group, escalating once to SIGKILL."""
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


def call_agent(
    agent_socket: Path,
    tool: str,
    args: Optional[dict[str, Any]] = None,
    timeout: float = 8.0,
    *,
    max_response_bytes: int = 262144,
) -> tuple[int, dict[str, Any]]:
    """Call one tool through Kyty's line-delimited Unix-socket protocol."""
    request = {"id": 1, "tool": tool, "args": args or {}}
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(timeout)
            client.connect(str(agent_socket))
            payload = json.dumps(request, separators=(",", ":")) + "\n"
            client.sendall(payload.encode("utf-8"))

            response = bytearray()
            while b"\n" not in response:
                chunk = client.recv(min(8192, max_response_bytes - len(response)))
                if not chunk:
                    break
                response.extend(chunk)
                if len(response) >= max_response_bytes and b"\n" not in response:
                    return 125, {
                        "ok": False,
                        "error": {"code": "transport", "message": "response_too_large"},
                    }

        line = bytes(response).decode("utf-8", errors="replace").split("\n", 1)[0]
        if not line:
            return 125, {"ok": False, "error": {"code": "transport", "message": "empty_response"}}
        result = json.loads(line)
        if not isinstance(result, dict):
            return 125, {"ok": False, "error": {"code": "transport", "message": "invalid_response"}}
        if not result.get("ok", False):
            return 1, result
        return 0, result
    except FileNotFoundError:
        return 125, {"ok": False, "error": {"code": "transport", "message": "socket_missing"}}
    except (ConnectionRefusedError, TimeoutError, OSError, json.JSONDecodeError) as exc:
        return 125, {"ok": False, "error": {"code": "transport", "message": type(exc).__name__}}
