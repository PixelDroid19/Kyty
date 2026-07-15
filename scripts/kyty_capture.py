#!/usr/bin/env python3
"""Reproducible, sanitized Kyty graphics capture runner.

The command intentionally separates three concerns:

* ``capture`` runs the guest and records a bounded set of frame screenshots.
* ``score`` computes deterministic image metrics without starting the guest.
* ``compare`` compares a capture manifest with a checked-in/local baseline.

Capture output is designed for ignored scratch directories.  The manifest never
stores the private guest root or the complete environment; only a redacted
configuration and reproducibility metadata are written.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import time
from collections import Counter
from typing import Any, Callable


SCHEMA_VERSION = 1
DEFAULT_WORLD_CROP = (0.18, 0.08, 0.92, 0.78)
DEFAULT_HUD_CROP = (0.02, 0.82, 0.35, 0.96)
FORBIDDEN_ENV = ("KYTY_STUB_MISSING", "KYTY_GFX_PERMISSIVE")
DIAGNOSTIC_ENV = (
    "KYTY_LIGHTBUF_PROBE",
    "KYTY_SKIP_UD2",
    "KYTY_BLEND_CONSTANT_EVIDENCE",
    "KYTY_VERTEX_EVIDENCE",
    "KYTY_SHADER_PROBE_CRC",
    "KYTY_TEX_PROBE",
    "KYTY_RT_EVIDENCE",
    "KYTY_RT_EVIDENCE_PS",
    "KYTY_FRAMEBUFFER_EVIDENCE",
    "KYTY_VIDEOOUT_EVIDENCE",
    "KYTY_FPS_LOG",
)


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def run_text(command: list[str], *, timeout: float = 10.0) -> str:
    try:
        return subprocess.check_output(
            command, stderr=subprocess.DEVNULL, text=True, timeout=timeout
        ).strip()
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return ""


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def nonnegative_float(value: str) -> float:
    parsed = float(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return parsed


def scheduled_key(value: str) -> tuple[int, str]:
    try:
        frame_text, key = value.split(":", 1)
        frame = positive_int(frame_text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must use FRAME:KEY, for example 700:x") from exc
    key = key.strip()
    if not key:
        raise argparse.ArgumentTypeError("key must not be empty")
    return frame, key


def git_metadata(root: Path) -> dict[str, Any]:
    return {
        "commit": run_text(["git", "rev-parse", "HEAD"], timeout=5),
        "branch": run_text(["git", "branch", "--show-current"], timeout=5),
        "dirty": bool(run_text(["git", "status", "--porcelain"], timeout=5)),
        "diff_stat": run_text(["git", "diff", "--stat"], timeout=5),
    }


def host_metadata() -> dict[str, Any]:
    vulkan_summary = run_text(["vulkaninfo", "--summary"], timeout=15)
    device = ""
    for line in vulkan_summary.splitlines():
        if "deviceName" in line or "GPU id" in line:
            device = line.strip()
            break
    return {
        "platform": platform.platform(),
        "python": platform.python_version(),
        "gpu_summary_line": device,
    }


def ensure_strict_environment(allow_diagnostics: bool, allowed: set[str] | None = None) -> None:
    if allow_diagnostics:
        return
    allowed = allowed or set()
    active = [
        name
        for name in FORBIDDEN_ENV + DIAGNOSTIC_ENV
        if os.environ.get(name) and name not in allowed
    ]
    if active:
        raise RuntimeError(
            "strict capture refuses diagnostic/permissive environment: "
            + ", ".join(active)
        )


class Image:
    def __init__(self, width: int, height: int, pixel: Callable[[int, int], tuple[int, int, int]], backend: str):
        self.width = width
        self.height = height
        self.pixel = pixel
        self.backend = backend


def load_image(path: Path) -> Image:
    try:
        from PIL import Image as PILImage  # type: ignore

        image = PILImage.open(path).convert("RGB")
        pixels = image.load()
        return Image(image.width, image.height, lambda x, y: pixels[x, y], "pillow")
    except ImportError:
        identify = shutil.which("identify")
        convert = shutil.which("convert")
        if not identify or not convert:
            raise RuntimeError("score requires Pillow or ImageMagick (identify + convert)")
        size = run_text([identify, "-format", "%w %h", str(path)])
        try:
            width, height = (int(value) for value in size.split())
        except ValueError as exc:
            raise RuntimeError(f"cannot read image dimensions: {path}") from exc
        raw = subprocess.check_output([convert, str(path), "rgb:-"])

        def pixel(x: int, y: int) -> tuple[int, int, int]:
            offset = (y * width + x) * 3
            return raw[offset], raw[offset + 1], raw[offset + 2]

        return Image(width, height, pixel, "imagemagick")


def crop_bounds(image: Image, crop: tuple[float, float, float, float]) -> tuple[int, int, int, int]:
    x0, y0, x1, y1 = crop
    return (
        max(0, min(image.width - 1, int(image.width * x0))),
        max(0, min(image.height - 1, int(image.height * y0))),
        max(1, min(image.width, int(image.width * x1))),
        max(1, min(image.height, int(image.height * y1))),
    )


def score_image(path: Path) -> dict[str, Any]:
    image = load_image(path)
    x0, y0, x1, y1 = crop_bounds(image, DEFAULT_WORLD_CROP)
    hx0, hy0, hx1, hy1 = crop_bounds(image, DEFAULT_HUD_CROP)
    total = white = saturated = 0
    hist: Counter[tuple[int, int, int]] = Counter()
    col_diff = row_diff = 0.0
    col_samples = row_samples = 0
    green_hud = hud_total = 0

    for y in range(hy0, hy1):
        for x in range(hx0, hx1, 2):
            r, g, b = image.pixel(x, y)
            hud_total += 1
            green_hud += int(g >= 140 and g > r + 40 and g > b + 40)

    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = image.pixel(x, y)
            total += 1
            white += int(r >= 245 and g >= 245 and b >= 245)
            saturated += int(max(r, g, b) >= 245 and min(r, g, b) <= 90)
            hist[(r >> 4, g >> 4, b >> 4)] += 1

    step = 4
    mid_y = (y0 + y1) // 2
    mid_x = (x0 + x1) // 2
    for x in range(x0, max(x0, x1 - step), step):
        a = image.pixel(x, mid_y)
        b = image.pixel(x + step, mid_y)
        col_diff += sum(abs(a[i] - b[i]) for i in range(3))
        col_samples += 1
    for y in range(y0, max(y0, y1 - step), step):
        a = image.pixel(mid_x, y)
        b = image.pixel(mid_x, y + step)
        row_diff += sum(abs(a[i] - b[i]) for i in range(3))
        row_samples += 1

    entropy = 0.0
    for count in hist.values():
        probability = count / max(total, 1)
        entropy -= probability * math.log2(probability)
    avg_col = col_diff / max(col_samples, 1)
    avg_row = row_diff / max(row_samples, 1)
    stripey = avg_col > 40 and avg_row < 8 and avg_col > avg_row * 6
    green_ratio = green_hud / max(hud_total, 1)
    white_ratio = white / max(total, 1)
    saturated_ratio = saturated / max(total, 1)
    ocr = ""
    tesseract = shutil.which("tesseract")
    if tesseract:
        ocr = run_text([tesseract, str(path), "stdout", "--psm", "6"], timeout=20).upper()
    is_loading = bool(re.search(r"LOADING|PRISONERS|QUARTERS", ocr))
    is_jump = bool(re.search(r"\bJUMP\b", ocr)) or green_ratio >= 0.02
    scene_ok = is_jump and not is_loading
    gameplay_like = white_ratio < 0.35 and entropy >= 2.5 and len(hist) >= 80 and not stripey

    return {
        "path": path.name,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "width": image.width,
        "height": image.height,
        "backend": image.backend,
        "world": {
            "white_ratio": round(white_ratio, 6),
            "saturated_ratio": round(saturated_ratio, 6),
            "entropy": round(entropy, 4),
            "unique_quantized_colors": len(hist),
            "avg_column_diff": round(avg_col, 4),
            "avg_row_diff": round(avg_row, 4),
            "stripey": stripey,
            "green_hud_ratio": round(green_ratio, 6),
            "is_loading": is_loading,
            "is_jump": is_jump,
            "scene_ok": scene_ok,
            "gameplay_like": gameplay_like,
        },
    }


def aggregate_captures(captures: list[dict[str, Any]]) -> dict[str, Any]:
    if not captures:
        raise RuntimeError("capture contains no samples")
    worlds = [capture["world"] for capture in captures]
    return {
        "world": {
            "white_ratio": max(world["white_ratio"] for world in worlds),
            "saturated_ratio": max(world["saturated_ratio"] for world in worlds),
            "entropy": min(world["entropy"] for world in worlds),
            "unique_quantized_colors": min(world["unique_quantized_colors"] for world in worlds),
            "avg_column_diff": max(world["avg_column_diff"] for world in worlds),
            "avg_row_diff": min(world["avg_row_diff"] for world in worlds),
            "stripey": any(world["stripey"] for world in worlds),
            "green_hud_ratio": min(world["green_hud_ratio"] for world in worlds),
            "is_loading": any(world.get("is_loading", False) for world in worlds),
            "is_jump": all(world.get("is_jump", False) for world in worlds),
            "scene_ok": all(world.get("scene_ok", False) for world in worlds),
            "gameplay_like": all(world["gameplay_like"] for world in worlds),
        }
    }


def compare_metrics(current: dict[str, Any], baseline: dict[str, Any]) -> dict[str, Any]:
    current_world = current["world"]
    baseline_world = baseline["world"]
    checks = {
        "white_ratio_not_worse": current_world["white_ratio"] <= baseline_world["white_ratio"] + 0.08,
        "entropy_not_collapsed": current_world["entropy"] >= baseline_world["entropy"] - 1.0,
        "colors_not_collapsed": current_world["unique_quantized_colors"] >= max(20, int(baseline_world["unique_quantized_colors"] * 0.55)),
        "not_stripey": not current_world["stripey"],
        "absolute_world_gate": (
            current_world["white_ratio"] < 0.35
            and current_world["entropy"] >= 2.5
            and current_world["unique_quantized_colors"] >= 80
        ),
        "scene_is_gameplay": current_world.get("scene_ok", False),
    }
    return {
        "pass": all(checks.values()),
        "checks": checks,
        "delta": {
            "white_ratio": round(current_world["white_ratio"] - baseline_world["white_ratio"], 6),
            "entropy": round(current_world["entropy"] - baseline_world["entropy"], 4),
            "unique_quantized_colors": current_world["unique_quantized_colors"] - baseline_world["unique_quantized_colors"],
        },
    }


def window_ids() -> list[str]:
    xdotool = shutil.which("xdotool")
    if not xdotool:
        raise RuntimeError("capture requires xdotool")
    return run_text([xdotool, "search", "--name", "fps:"]).splitlines()


def find_window(exclude: set[str] | None = None) -> tuple[str, int] | None:
    xdotool = shutil.which("xdotool")
    if not xdotool:
        raise RuntimeError("capture requires xdotool")
    ids = window_ids()
    excluded = exclude or set()
    candidates: list[tuple[str, int]] = []
    for window_id in ids:
        if window_id in excluded:
            continue
        geometry = run_text([xdotool, "getwindowgeometry", "--shell", window_id])
        values: dict[str, int] = {}
        for line in geometry.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                if value.isdigit():
                    values[key] = int(value)
        area = values.get("WIDTH", 0) * values.get("HEIGHT", 0)
        if area:
            candidates.append((window_id, area))
    return max(candidates, key=lambda item: item[1]) if candidates else None


def window_frame(window_id: str) -> int | None:
    xdotool = shutil.which("xdotool")
    if not xdotool:
        return None
    title = run_text([xdotool, "getwindowname", window_id])
    match = re.search(r"frame:\s*(\d+)", title)
    return int(match.group(1)) if match else None


def capture_window(window_id: str, output: Path) -> None:
    importer = shutil.which("import")
    if not importer:
        raise RuntimeError("capture requires ImageMagick import")
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.check_call([importer, "-window", window_id, str(output)])


def send_scheduled_key(window_id: str, key: str) -> None:
    xdotool = shutil.which("xdotool")
    if not xdotool:
        raise RuntimeError("input scheduling requires xdotool")
    subprocess.check_call([xdotool, "key", "--window", window_id, key], stderr=subprocess.DEVNULL)


def run_capture(args: argparse.Namespace) -> int:
    allowed = set()
    if args.rt_evidence:
        allowed.add("KYTY_RT_EVIDENCE")
    if args.rt_evidence_ps:
        allowed.update(("KYTY_RT_EVIDENCE", "KYTY_RT_EVIDENCE_PS"))
    if args.framebuffer_evidence:
        allowed.add("KYTY_FRAMEBUFFER_EVIDENCE")
    if args.videoout_evidence:
        allowed.add("KYTY_VIDEOOUT_EVIDENCE")
    if args.tex_probe:
        allowed.add("KYTY_TEX_PROBE")
    if args.lightbuf_probe:
        allowed.add("KYTY_LIGHTBUF_PROBE")
    if args.shader_probe_crc:
        allowed.add("KYTY_SHADER_PROBE_CRC")
    ensure_strict_environment(args.allow_diagnostics, allowed)
    root = Path(__file__).resolve().parents[1]
    output = Path(args.out).resolve()
    output.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    log_path = output / f"guest-{stamp}.log"
    manifest_path = output / f"capture-{stamp}.json"
    build = Path(args.build)
    if not build.is_absolute():
        build = root / build
    executable = build / "fc_script"
    if not executable.exists():
        raise RuntimeError(f"missing build executable: {executable}")
    guest_root = Path(args.guest_root).expanduser().resolve()
    if not guest_root.exists():
        raise RuntimeError(f"guest root does not exist: {guest_root}")

    env = os.environ.copy()
    if not args.allow_diagnostics:
        for name in DIAGNOSTIC_ENV:
            env.pop(name, None)
    env.pop("KYTY_STUB_MISSING", None)
    env.pop("KYTY_GFX_PERMISSIVE", None)
    env["KYTY_AUTO_CROSS"] = "1" if args.auto_cross else "0"
    env["SDL_VIDEODRIVER"] = "x11"
    if args.rt_evidence:
        env["KYTY_RT_EVIDENCE"] = "1"
    if args.rt_evidence_ps:
        env["KYTY_RT_EVIDENCE"] = "1"
        env["KYTY_RT_EVIDENCE_PS"] = args.rt_evidence_ps
    if args.framebuffer_evidence:
        env["KYTY_FRAMEBUFFER_EVIDENCE"] = "1"
    if args.videoout_evidence:
        env["KYTY_VIDEOOUT_EVIDENCE"] = "1"
        env["KYTY_VIDEOOUT_EVIDENCE_MIN_FRAME"] = str(args.min_frame)
    if args.tex_probe:
        env["KYTY_TEX_PROBE"] = "1"
    if args.lightbuf_probe:
        env["KYTY_LIGHTBUF_PROBE"] = args.lightbuf_probe
    if args.shader_probe_crc:
        env["KYTY_SHADER_PROBE_CRC"] = args.shader_probe_crc
    env["KYTY_SCREEN_WIDTH"] = str(args.width)
    env["KYTY_SCREEN_HEIGHT"] = str(args.height)
    if args.vulkan_validation:
        env["KYTY_VULKAN_VALIDATION"] = "1"
    if args.shader_validation:
        env["KYTY_SHADER_VALIDATION"] = "1"
    if args.command_buffer_dump:
        env["KYTY_COMMAND_BUFFER_DUMP"] = "1"
        env["KYTY_COMMAND_BUFFER_DUMP_FOLDER"] = str(output / "buffers")
    if args.pipeline_dump:
        env["KYTY_PIPELINE_DUMP"] = "1"
        env["KYTY_PIPELINE_DUMP_FOLDER"] = str(output / "pipelines")

    command = [str(executable), "scripts/run_guest.lua", str(guest_root)]
    started_utc = utc_now()
    started = time.monotonic()
    existing_windows = set(window_ids())
    previous_sigterm = signal.getsignal(signal.SIGTERM)

    def abort(_signum: int, _frame: Any) -> None:
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, abort)
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            command,
            cwd=root,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=(os.name == "posix"),
        )
    captures: list[dict[str, Any]] = []
    capture_error: str | None = None
    scheduled = sorted(args.key_at or [])
    sent_keys: list[dict[str, Any]] = []
    try:
        deadline = time.monotonic() + args.max_wait
        frame = None
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"guest exited early with code {process.returncode}; see {log_path}")
            found = find_window(existing_windows)
            if found:
                frame = window_frame(found[0])
                if frame is not None:
                    while scheduled and frame >= scheduled[0][0]:
                        target_frame, key = scheduled.pop(0)
                        send_scheduled_key(found[0], key)
                        sent_keys.append({"frame": frame, "target_frame": target_frame, "key": key})
                if frame is not None and frame >= args.min_frame:
                    break
            time.sleep(1)
        if frame is None or frame < args.min_frame:
            raise RuntimeError(f"timed out at frame {frame}; see {log_path}")
        for index in range(args.samples):
            if process.poll() is not None:
                raise RuntimeError(f"guest exited during capture with code {process.returncode}; see {log_path}")
            found = find_window(existing_windows)
            if not found:
                raise RuntimeError("game window disappeared during capture")
            window_id = found[0]
            frame = window_frame(window_id)
            image_path = output / f"frame-{stamp}-{index:03d}.png"
            capture_window(window_id, image_path)
            metrics = score_image(image_path)
            metrics["path"] = str(image_path.relative_to(output))
            metrics["frame"] = frame
            captures.append(metrics)
            if index + 1 < args.samples:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        capture_error = "capture interrupted"
    except RuntimeError as exc:
        capture_error = str(exc)
    finally:
        if process.poll() is None:
            if os.name == "posix":
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)
            else:
                process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        signal.signal(signal.SIGTERM, previous_sigterm)

    aggregate = aggregate_captures(captures) if captures else None
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "kind": "kyty.graphics.capture",
        "started_utc": started_utc,
        "duration_seconds": round(time.monotonic() - started, 3),
        "git": git_metadata(root),
        "host": host_metadata(),
        "config": {
            "build": str(build.relative_to(root)) if build.is_relative_to(root) else build.name,
            "screen": [args.width, args.height],
            "min_frame": args.min_frame,
            "samples": args.samples,
            "interval_seconds": args.interval,
            "auto_cross": args.auto_cross,
            "diagnostic_input": args.auto_cross,
            "rt_evidence": args.rt_evidence,
            "rt_evidence_ps": args.rt_evidence_ps,
            "framebuffer_evidence": args.framebuffer_evidence,
            "videoout_evidence": args.videoout_evidence,
            "videoout_evidence_min_frame": args.min_frame if args.videoout_evidence else None,
            "tex_probe": args.tex_probe,
            "lightbuf_probe": args.lightbuf_probe,
            "shader_probe_crc": args.shader_probe_crc,
            "key_at": [{"frame": frame, "key": key} for frame, key in (args.key_at or [])],
            "vulkan_validation": args.vulkan_validation,
            "shader_validation": args.shader_validation,
            "command_buffer_dump": args.command_buffer_dump,
            "pipeline_dump": args.pipeline_dump,
            "strict_environment": not args.allow_diagnostics,
            "strict_compatibility_candidate": (
                not args.allow_diagnostics
                and not args.auto_cross
                and not any(
                    (
                        args.rt_evidence,
                        args.rt_evidence_ps,
                        args.framebuffer_evidence,
                        args.videoout_evidence,
                        args.tex_probe,
                        args.lightbuf_probe,
                        args.shader_probe_crc,
                        args.key_at,
                        args.vulkan_validation,
                        args.shader_validation,
                        args.command_buffer_dump,
                        args.pipeline_dump,
                    )
                )
            ),
        },
        "artifacts": {
            "log": str(log_path.relative_to(output)),
            "screenshots": [str(item["path"]) for item in captures],
        },
        "input": sent_keys,
        "captures": captures,
        "aggregate": aggregate,
        "status": "ok" if capture_error is None else "incomplete",
    }
    if capture_error is not None:
        manifest["error"] = capture_error
    if args.baseline:
        baseline = load_manifest(Path(args.baseline))
        baseline_metrics = baseline.get("aggregate")
        if baseline_metrics is None and baseline.get("captures"):
            baseline_metrics = aggregate_captures(baseline["captures"])
        if baseline_metrics is None:
            raise RuntimeError("baseline manifest contains no captures")
        if aggregate is None:
            manifest["comparison"] = {"pass": False, "checks": {"samples_available": False}, "delta": {}}
        else:
            manifest["comparison"] = compare_metrics(aggregate, baseline_metrics)
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if capture_error is not None:
        raise RuntimeError(f"{capture_error}; partial manifest: {manifest_path}")
    result = {"manifest": str(manifest_path), "captures": len(captures), "frames": [item["frame"] for item in captures]}
    if "comparison" in manifest:
        result["comparison"] = manifest["comparison"]
    print(json.dumps(result))
    return 0 if manifest.get("comparison", {}).get("pass", True) else 1


def load_manifest(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema_version") != SCHEMA_VERSION:
        raise RuntimeError(f"unsupported capture schema: {data.get('schema_version')}")
    return data


def command_score(args: argparse.Namespace) -> int:
    metrics = score_image(Path(args.image))
    print(json.dumps(metrics, indent=2, sort_keys=True))
    return 0 if (not args.gate or (metrics["world"]["gameplay_like"] and metrics["world"]["scene_ok"])) else 1


def command_compare(args: argparse.Namespace) -> int:
    current = load_manifest(Path(args.current))
    baseline = load_manifest(Path(args.baseline))
    current_metrics = current.get("aggregate") or (aggregate_captures(current.get("captures", [])) if current.get("captures") else None)
    baseline_metrics = baseline.get("aggregate") or (aggregate_captures(baseline.get("captures", [])) if baseline.get("captures") else None)
    if current_metrics is None or baseline_metrics is None:
        result = {
            "pass": False,
            "checks": {"samples_available": False},
            "delta": {},
        }
        print(json.dumps(result, indent=2, sort_keys=True))
        return 1
    result = compare_metrics(current_metrics, baseline_metrics)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["pass"] else 1


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)

    capture = commands.add_parser("capture", help="run the guest and capture reproducible frames")
    capture.add_argument("--guest-root", default=os.environ.get("KYTY_GUEST_ROOT"), required=not bool(os.environ.get("KYTY_GUEST_ROOT")))
    capture.add_argument("--build", default=os.environ.get("KYTY_BUILD", "_build_linux"))
    capture.add_argument("--out", default="_scratch_playable/captures")
    capture.add_argument("--min-frame", type=positive_int, default=1250)
    capture.add_argument("--max-wait", type=positive_int, default=240)
    capture.add_argument("--samples", type=positive_int, default=3)
    capture.add_argument("--interval", type=nonnegative_float, default=2.0)
    capture.add_argument("--baseline", help="optional prior manifest; fail on a visual metric regression")
    capture.add_argument("--width", type=positive_int, default=1280)
    capture.add_argument("--height", type=positive_int, default=720)
    capture.add_argument("--auto-cross", action=argparse.BooleanOptionalAction, default=True)
    capture.add_argument("--rt-evidence", action="store_true")
    capture.add_argument(
        "--rt-evidence-ps",
        metavar="CRC32",
        help="limit RT_EVIDENCE pipeline logs to one pixel-shader CRC (hex, diagnostic)",
    )
    capture.add_argument("--framebuffer-evidence", action="store_true")
    capture.add_argument("--videoout-evidence", action="store_true", help="log the final VideoOut-to-swapchain blit (diagnostic)")
    capture.add_argument("--tex-probe", action="store_true")
    capture.add_argument(
        "--lightbuf-probe",
        choices=("1", "compositor"),
        help="arm ordered lighting/compositor RT capture (diagnostic)",
    )
    capture.add_argument(
        "--shader-probe-crc",
        metavar="CRC32",
        help="dump one VS/PS shader IR by CRC32 (diagnostic)",
    )
    capture.add_argument(
        "--key-at",
        action="append",
        type=scheduled_key,
        metavar="FRAME:KEY",
        help="send one key edge when the window reaches FRAME; repeatable",
    )
    capture.add_argument("--vulkan-validation", action="store_true")
    capture.add_argument("--shader-validation", action="store_true")
    capture.add_argument("--command-buffer-dump", action="store_true")
    capture.add_argument("--pipeline-dump", action="store_true")
    capture.add_argument("--allow-diagnostics", action="store_true")
    capture.set_defaults(function=run_capture)

    score = commands.add_parser("score", help="score one screenshot")
    score.add_argument("image")
    score.add_argument("--gate", action="store_true", help="return failure when gameplay-like metrics are not met")
    score.set_defaults(function=command_score)

    compare = commands.add_parser("compare", help="compare the last sample of two manifests")
    compare.add_argument("--current", required=True)
    compare.add_argument("--baseline", required=True)
    compare.set_defaults(function=command_compare)
    return root


def main() -> int:
    args = parser().parse_args()
    try:
        return args.function(args)
    except (OSError, RuntimeError, ValueError, KeyError, IndexError, subprocess.CalledProcessError) as exc:
        print(f"kyty_capture: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
