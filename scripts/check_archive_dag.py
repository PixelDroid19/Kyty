#!/usr/bin/env python3
"""Verify the emulator archive graph is acyclic and each archive owns its objects.

Run against a configured build directory:

    python3 scripts/check_archive_dag.py <build_dir> [<source_dir>]

The source directory is optional; when provided the script also verifies that
every object in each kyty_* archive maps to a source file owned by that
domain (the CMake-side projection of the domain boundaries).
"""

import argparse
import pathlib
import re
import subprocess
import sys
from collections import defaultdict, deque

# Domain -> globs under source/emulator, mirrored from emulator/CMakeLists.txt.
DOMAIN_SOURCE_GLOBS = {
    "kyty_runtime_core": (
        "src/Log.cpp",
        "src/Config.cpp",
        "src/GuestRuntimePort.cpp",
        "src/GuestMemory.cpp",
        "src/GpuMemoryFault.cpp",
        "src/AtomicFile.cpp",
        "src/Validation/DomainValidators.cpp",
        "src/SystemContentPort.cpp",
        "src/PresentationStats.cpp",
        "src/VideoFrameMemory.cpp",
        "src/Ports/*.cpp",
    ),
    "kyty_kernel": ("src/Kernel/*.cpp",),
    "kyty_diagnostics": ("src/Agent/EventRing.cpp", "src/Agent/AgentLifecycle.cpp"),
    "kyty_loader": (
        "src/Loader/*.cpp",
    ),
    "kyty_host": ("src/Host/*.cpp",),
    "kyty_graphics": (
        "src/Graphics/*.cpp",
        "src/Graphics/Objects/*.cpp",
        "src/Hle/*.cpp",
    ),
    "kyty_hle": (
        "src/Libs/*.cpp",
        "src/Network.cpp",
        "src/NetworkHttpUri.cpp",
        "src/Audio.cpp",
        "src/Audio3d.cpp",
        "src/AudioAjm.cpp",
        "src/AudioAvPlayer.cpp",
        "src/AudioHost.cpp",
        "src/AudioNgs2.cpp",
        "src/Dialog.cpp",
        "src/Controller.cpp",
    ),
    "kyty_audio_video_backend": ("src/AudioVideoBackend.cpp",),
    "kyty_audio_pcm": ("src/AudioPcm.cpp",),
    "emulator": (
        "src/Kyty.cpp",
        "src/Emulator.cpp",
        "src/Profiler.cpp",
        "src/Agent/AgentServer.cpp",
        "src/Agent/AgentSubsystem.cpp",
        "src/Agent/StallWatch.cpp",
        "src/Agent/FrameScore.cpp",
        "src/Agent/DebugSnapshot.cpp",
        "src/Agent/Protocol.cpp",
    ),
}

# Objects compiled from outside source/emulator (third-party backends).
FOREIGN_OBJECT_ALLOWLIST = {"imgui_impl_sdl2.cpp.o": "kyty_host"}


def archive_path(build_dir: pathlib.Path, archive: str) -> pathlib.Path:
    candidates = [
        build_dir / "emulator" / f"lib{archive}.a",
        build_dir / "audio_pcm" / f"lib{archive}.a",
        build_dir / "lib" / f"lib{archive}.a",
        build_dir / f"lib{archive}.a",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"cannot locate lib{archive}.a under {build_dir}")


def member_symbols(archive: pathlib.Path, defined_only: bool) -> dict:
    args = ["nm", "-A"] + (["--defined-only"] if defined_only else ["-u"]) + [str(archive)]
    out = subprocess.run(args, capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError(f"nm failed on {archive}: {out.stderr.strip()}")
    symbols = defaultdict(set)
    for line in out.stdout.splitlines():
        parts = line.split(":")
        if len(parts) < 3:
            continue
        obj = parts[1].split("(")[-1].rstrip(")")
        symbols[obj].add(parts[2].split()[-1])
    return symbols


def owned_sources(source_dir: pathlib.Path, globs) -> set:
    owned = set()
    for pattern in globs:
        for path in (source_dir / "emulator").glob(pattern):
            owned.add(path.name)
    return owned


def check_dag(build_dir: pathlib.Path) -> list:
    diagnostics = []
    archives = list(DOMAIN_SOURCE_GLOBS)
    defined = {}
    undefined = {}
    for name in archives:
        try:
            path = archive_path(build_dir, name)
        except FileNotFoundError as error:
            diagnostics.append(f"missing archive: {error}")
            continue
        defined[name] = set()
        for symbols in member_symbols(path, True).values():
            defined[name] |= symbols
        undefined[name] = set()
        for symbols in member_symbols(path, False).values():
            undefined[name] |= symbols

    edges = defaultdict(set)
    for target in defined:
        for dependency in defined:
            if target == dependency:
                continue
            if undefined[target] & defined[dependency]:
                edges[target].add(dependency)

    # Topological sort; a cycle leaves some archive unprocessed.
    remaining = set(defined)
    order = []
    while remaining:
        ready = [name for name in remaining if not (edges[name] & remaining)]
        if not ready:
            cycle_members = ", ".join(sorted(remaining))
            diagnostics.append(f"archive link cycle among: {cycle_members}")
            return diagnostics
        for name in ready:
            remaining.discard(name)
            order.append(name)
        for name in remaining:
            edges[name].discard(set(ready))

    diagnostics.append(f"archive order (consumers first): {' -> '.join(reversed(order))}")
    return diagnostics


def check_ownership(build_dir: pathlib.Path, source_dir: pathlib.Path) -> list:
    diagnostics = []
    expected = {name: owned_sources(source_dir, globs) for name, globs in DOMAIN_SOURCE_GLOBS.items()}
    for name in DOMAIN_SOURCE_GLOBS:
        try:
            path = archive_path(build_dir, name)
        except FileNotFoundError:
            continue
        members = subprocess.run(["ar", "t", str(path)], capture_output=True, text=True).stdout.splitlines()
        expected_members = {source + ".o" for source in expected[name]}
        for member in members:
            if member in expected_members:
                continue
            if FOREIGN_OBJECT_ALLOWLIST.get(member) == name:
                continue
            diagnostics.append(f"{name} owns unexpected object {member}")
        for source in expected[name]:
            expected_member = source + ".o"
            if expected_member == "imgui_impl_sdl2.cpp.o":
                continue
            if expected_member not in members:
                diagnostics.append(f"{name} is missing object {expected_member}")
    return diagnostics


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=pathlib.Path)
    parser.add_argument("source_dir", nargs="?", type=pathlib.Path, default=None)
    args = parser.parse_args()

    diagnostics = check_dag(args.build_dir)
    if args.source_dir is not None:
        diagnostics += check_ownership(args.build_dir, args.source_dir)

    for diagnostic in diagnostics:
        print(diagnostic)
    return 1 if any("cycle" in d or "unexpected" in d or "missing" in d or "cannot locate" in d for d in diagnostics) else 0


if __name__ == "__main__":
    sys.exit(main())
