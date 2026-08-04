#!/usr/bin/env python3
"""Check the small, explicit emulator dependency-direction contract."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import tempfile
import unittest
from typing import Iterable, List, Optional, Tuple


@dataclass(frozen=True)
class CheckResult:
    exit_code: int
    diagnostics: Tuple[str, ...]


MAX_SOURCE_BYTES = 4 * 1024 * 1024

# The checker reads only these concrete source files. It intentionally does
# not walk directories because this is a narrow dependency guard, not a broad
# source graph audit.
AUDIO_SOURCE_FILES = (
    "emulator/src/Audio.cpp",
    "emulator/src/AudioHost.cpp",
    "emulator/src/AudioPcm.cpp",
)
KERNEL_SOURCE_FILES = (
    "emulator/src/Kernel/FileSystem.cpp",
    "emulator/src/Kernel/Pthread.cpp",
)
HOST_SOURCE_FILES = (
    "emulator/src/Host/CaptureImageCodec.cpp",
    "emulator/src/Host/Clock.cpp",
    "emulator/src/Host/Platform.cpp",
    "emulator/src/Host/Png.cpp",
)
LIBS_SOURCE_FILES = (
    "emulator/src/Libs/LibC.cpp",
    "emulator/src/Libs/LibSaveData.cpp",
)
LOADER_SOURCE_FILES = (
    "emulator/src/Loader/SystemContent.cpp",
)
RUNTIME_LINKER_SOURCE = "emulator/src/Loader/RuntimeLinker.cpp"
ALLOWED_SOURCE_FILES = (
    *AUDIO_SOURCE_FILES,
    *KERNEL_SOURCE_FILES,
    *HOST_SOURCE_FILES,
    *LIBS_SOURCE_FILES,
    *LOADER_SOURCE_FILES,
    RUNTIME_LINKER_SOURCE,
)

GRAPHICS_INCLUDE_PREFIX = "Emulator/Graphics/"
SOURCE_GRAPHICS_INCLUDE_PREFIX = "emulator/include/Emulator/Graphics/"
SDL_INCLUDE_PREFIX = "SDL"
DEVTOOLS_INCLUDE_PREFIXES = (
    "Emulator/DevTools/",
    "Kyty/DevTools/",
)
SOURCE_DEVTOOLS_INCLUDE_PREFIXES = (
    "emulator/include/Emulator/DevTools/",
    "devtools/include/Kyty/DevTools/",
    "lib/DevTools/include/Kyty/DevTools/",
)
PROFILER_INCLUDE_PATHS = (
    "Emulator/Profiler.h",
    "emulator/include/Emulator/Profiler.h",
)

@dataclass(frozen=True)
class Violation:
    source_path: str
    line_number: int
    rule: Optional[str]
    include_path: Optional[str]
    input_error: Optional[str] = None

    def diagnostic(self) -> str:
        if self.include_path is None:
            return f"{self.source_path}:{self.line_number}: {self.input_error or 'uncheckable nonliteral include'}"
        assert self.rule is not None
        return f"{self.source_path}:{self.line_number}: forbidden include ({self.rule}): {self.include_path}"


@dataclass(frozen=True)
class IncludeDirective:
    line_number: int
    include_path: Optional[str]
    raw_physical_line: str
    source_has_utf8_bom: bool


def _resolve_source_root(source_root: Path) -> Tuple[Optional[Path], Optional[str]]:
    try:
        resolved_root = source_root.resolve(strict=True)
    except (OSError, RuntimeError):
        return None, f"source root is not a directory: {source_root}"
    if not resolved_root.is_dir():
        return None, f"source root is not a directory: {source_root}"
    return resolved_root, None


def _read_allowlisted_source(root: Path, relative_path: str) -> Tuple[Optional[str], Optional[str]]:
    path = root.joinpath(*Path(relative_path).parts)
    try:
        resolved_path = path.resolve(strict=True)
    except FileNotFoundError:
        # Synthetic fixtures may only need one of the explicitly allowlisted
        # files, so a missing member is not itself a dependency violation.
        return None, None
    except (OSError, RuntimeError):
        return None, f"{relative_path}: unable to resolve source file"

    try:
        resolved_path.relative_to(root)
    except ValueError:
        return None, f"{relative_path}: source file escapes supplied root"

    if not resolved_path.is_file():
        return None, f"{relative_path}: source file is not a regular file"

    try:
        with resolved_path.open("rb") as source_file:
            contents = source_file.read(MAX_SOURCE_BYTES + 1)
    except OSError:
        return None, f"{relative_path}: unable to read source file"

    if len(contents) > MAX_SOURCE_BYTES:
        return None, f"{relative_path}: source file exceeds {MAX_SOURCE_BYTES} byte limit"

    try:
        return contents.decode("utf-8"), None
    except UnicodeDecodeError:
        return None, f"{relative_path}: source file is not valid UTF-8"


def _canonical_include_path(include_path: str) -> str:
    """Normalize header separators and lexical segments for rule matching."""

    segments: List[str] = []
    for segment in include_path.replace("\\", "/").split("/"):
        if segment in ("", "."):
            continue
        if segment == ".." and segments and segments[-1] != "..":
            segments.pop()
            continue
        segments.append(segment)
    # Case-fold unconditionally: this catches the real Windows spelling while
    # failing closed for a path that would not compile on a case-sensitive host.
    return "/".join(segments).casefold()


def _is_absolute_include_path(include_path: str) -> bool:
    normalized = include_path.replace("\\", "/")
    return normalized.startswith("/") or (len(normalized) >= 2 and normalized[0].isalpha() and normalized[1] == ":")


def _rule_for_include(relative_path: str, include_path: str) -> Optional[str]:
    canonical_paths = [_canonical_include_path(include_path)]
    if not _is_absolute_include_path(include_path):
        source_parent = relative_path.rsplit("/", 1)[0]
        canonical_paths.append(_canonical_include_path(source_parent + "/" + include_path))

    graphics_prefixes = (GRAPHICS_INCLUDE_PREFIX, SOURCE_GRAPHICS_INCLUDE_PREFIX)
    if relative_path in AUDIO_SOURCE_FILES and any(
        canonical_path.startswith(prefix.casefold()) for canonical_path in canonical_paths for prefix in graphics_prefixes
    ):
        return "Audio -> Graphics"
    if relative_path in KERNEL_SOURCE_FILES and any(
        canonical_path.startswith(prefix.casefold()) for canonical_path in canonical_paths for prefix in graphics_prefixes
    ):
        return "Kernel -> Graphics"
    if relative_path in HOST_SOURCE_FILES and any(
        canonical_path.startswith(prefix.casefold()) for canonical_path in canonical_paths for prefix in graphics_prefixes
    ):
        return "Host -> Graphics"
    if relative_path in LIBS_SOURCE_FILES and any(
        canonical_path.startswith(prefix.casefold()) for canonical_path in canonical_paths for prefix in graphics_prefixes
    ):
        return "Libs -> Graphics"
    if relative_path in LIBS_SOURCE_FILES and any(
        canonical_path == SDL_INCLUDE_PREFIX.casefold()
        or canonical_path.startswith(
            (SDL_INCLUDE_PREFIX + ".").casefold(),
        )
        or canonical_path.startswith(
            (SDL_INCLUDE_PREFIX + "_").casefold(),
        )
        or canonical_path.startswith((SDL_INCLUDE_PREFIX + "2/").casefold())
        for canonical_path in canonical_paths
    ):
        return "Libs -> SDL"
    if relative_path in LOADER_SOURCE_FILES and any(
        canonical_path.startswith(prefix.casefold()) for canonical_path in canonical_paths for prefix in graphics_prefixes
    ):
        return "Loader -> Graphics"
    devtools_prefixes = DEVTOOLS_INCLUDE_PREFIXES + SOURCE_DEVTOOLS_INCLUDE_PREFIXES
    if relative_path == RUNTIME_LINKER_SOURCE and any(
        canonical_path.startswith(prefix.casefold()) for canonical_path in canonical_paths for prefix in devtools_prefixes
    ):
        return "RuntimeLinker -> DevTools"
    if relative_path == RUNTIME_LINKER_SOURCE and any(
        canonical_path == _canonical_include_path(profiler_path)
        for canonical_path in canonical_paths
        for profiler_path in PROFILER_INCLUDE_PATHS
    ):
        return "RuntimeLinker -> Profiler"
    return None


def _splice_escaped_newlines(contents: str) -> Tuple[str, List[int]]:
    """Apply the C/C++ escaped-newline phase and retain physical line numbers."""

    output: List[str] = []
    line_numbers: List[int] = []
    source_line = 1
    index = 0
    while index < len(contents):
        character = contents[index]
        if character == "\\" and index + 1 < len(contents):
            following = contents[index + 1]
            if following == "\n":
                index += 2
                source_line += 1
                continue
            if following == "\r":
                index += 2
                if index < len(contents) and contents[index] == "\n":
                    index += 1
                source_line += 1
                continue

        output.append(character)
        line_numbers.append(source_line)
        if character == "\n":
            source_line += 1
        elif character == "\r" and (index + 1 == len(contents) or contents[index + 1] != "\n"):
            source_line += 1
        index += 1
    return "".join(output), line_numbers


def _physical_source_lines(contents: str) -> List[str]:
    """Split source lines only on CR/LF, preserving all directive whitespace."""

    lines: List[str] = []
    start = 0
    index = 0
    while index < len(contents):
        if contents[index] == "\r":
            lines.append(contents[start:index])
            index += 1
            if index < len(contents) and contents[index] == "\n":
                index += 1
            start = index
            continue
        if contents[index] == "\n":
            lines.append(contents[start:index])
            index += 1
            start = index
            continue
        index += 1
    lines.append(contents[start:])
    return lines


def _skip_quoted_literal(contents: str, start: int) -> int:
    quote = contents[start]
    index = start + 1
    while index < len(contents):
        if contents[index] == "\\" and index + 1 < len(contents):
            index += 2
            continue
        if contents[index] == quote:
            return index + 1
        index += 1
    return index


def _skip_raw_string_literal(contents: str, start: int) -> int:
    """Skip a C++ raw string when its delimiter is structurally valid."""

    delimiter_end = contents.find("(", start + 2, start + 19)
    if delimiter_end == -1:
        return start + 1
    delimiter = contents[start + 2 : delimiter_end]
    if any(character in " ()\\\t\r\n" for character in delimiter):
        return start + 1
    closing = ")" + delimiter + '"'
    closing_start = contents.find(closing, delimiter_end + 1)
    if closing_start == -1:
        return len(contents)
    return closing_start + len(closing)


def _is_horizontal_whitespace(character: str) -> bool:
    return character in " \t\f\v"


def _consume_header_name(contents: str, start: int, terminator: str) -> Tuple[Optional[str], int]:
    index = start + 1
    while index < len(contents):
        character = contents[index]
        if character == terminator:
            return contents[start + 1 : index], index + 1
        if character in "\r\n":
            return None, index
        index += 1
    return None, index


def _preprocessor_includes(contents: str) -> Iterable[IncludeDirective]:
    """Return literal headers or fail-closed nonliteral includes from C++ text."""

    physical_lines = _physical_source_lines(contents)
    source_has_utf8_bom = contents.startswith("\ufeff")
    scan_contents = contents[1:] if source_has_utf8_bom else contents
    spliced_contents, line_numbers = _splice_escaped_newlines(scan_contents)
    directives: List[IncludeDirective] = []

    prefix = 0
    after_hash = 1
    include_keyword = 2
    after_include = 3
    not_directive = 4
    keyword = "include"
    state = prefix
    keyword_index = 0
    include_separator_seen = False
    directive_line = 0
    index = 0

    def reset_line() -> None:
        nonlocal state, keyword_index, include_separator_seen, directive_line
        state = prefix
        keyword_index = 0
        include_separator_seen = False
        directive_line = 0

    def consume_whitespace() -> None:
        nonlocal state, include_separator_seen
        if state == include_keyword:
            state = not_directive
        elif state == after_include:
            include_separator_seen = True

    def consume_newline() -> None:
        nonlocal index
        if spliced_contents[index] == "\r" and index + 1 < len(spliced_contents) and spliced_contents[index + 1] == "\n":
            index += 2
        else:
            index += 1
        reset_line()

    def append_directive(include_path: Optional[str]) -> None:
        raw_physical_line = ""
        if 0 < directive_line <= len(physical_lines):
            raw_physical_line = physical_lines[directive_line - 1]
        directives.append(IncludeDirective(directive_line, include_path, raw_physical_line, source_has_utf8_bom))

    while index < len(spliced_contents):
        character = spliced_contents[index]
        if character in "\r\n":
            consume_newline()
            continue

        if character == "/" and index + 1 < len(spliced_contents):
            next_character = spliced_contents[index + 1]
            if next_character == "/":
                consume_whitespace()
                index += 2
                while index < len(spliced_contents) and spliced_contents[index] not in "\r\n":
                    index += 1
                continue
            if next_character == "*":
                consume_whitespace()
                index += 2
                while index < len(spliced_contents):
                    if index + 1 < len(spliced_contents) and spliced_contents[index] == "*" and spliced_contents[index + 1] == "/":
                        index += 2
                        break
                    if spliced_contents[index] in "\r\n":
                        consume_newline()
                        continue
                    index += 1
                continue

        if state == after_include and character == "<":
            include_path, index = _consume_header_name(spliced_contents, index, ">")
            append_directive(include_path)
            state = not_directive
            continue
        if state == after_include and character == '"':
            include_path, index = _consume_header_name(spliced_contents, index, '"')
            append_directive(include_path)
            state = not_directive
            continue

        if character == "R" and index + 1 < len(spliced_contents) and spliced_contents[index + 1] == '"':
            if state == after_include and include_separator_seen:
                append_directive(None)
            state = not_directive
            index = _skip_raw_string_literal(spliced_contents, index)
            continue
        if character in ('"', "'"):
            if state == after_include and include_separator_seen:
                append_directive(None)
            state = not_directive
            index = _skip_quoted_literal(spliced_contents, index)
            continue

        if _is_horizontal_whitespace(character):
            consume_whitespace()
            index += 1
            continue

        if state == prefix:
            if character == "#":
                state = after_hash
                directive_line = line_numbers[index]
            elif character == "%" and index + 1 < len(spliced_contents) and spliced_contents[index + 1] == ":":
                state = after_hash
                directive_line = line_numbers[index]
                index += 2
                continue
            else:
                state = not_directive
        elif state == after_hash:
            if character == keyword[0]:
                state = include_keyword
                keyword_index = 1
            else:
                state = not_directive
        elif state == include_keyword:
            if keyword_index < len(keyword) and character == keyword[keyword_index]:
                keyword_index += 1
                if keyword_index == len(keyword):
                    state = after_include
                    include_separator_seen = False
            else:
                state = not_directive
        elif state == after_include:
            if include_separator_seen:
                append_directive(None)
            state = not_directive
        index += 1

    return directives


def check_source_root(source_root: Path, strict: bool = False) -> CheckResult:
    """Return bounded, deterministic diagnostics for the explicit file set.

    Include detection only recognizes preprocessing include directives. It
    applies escaped-newline and comment lexical normalization, but does not
    evaluate C++ conditionals or parse declarations. No directory traversal is
    performed, and each allowlisted file is resolved back under source_root
    before at most MAX_SOURCE_BYTES are read as strict UTF-8.
    """

    root, root_error = _resolve_source_root(source_root)
    if root_error is not None:
        return CheckResult(exit_code=1, diagnostics=(root_error,))

    assert root is not None
    input_diagnostics: List[Tuple[str, str]] = []
    violations: List[Violation] = []
    for relative_path in ALLOWED_SOURCE_FILES:
        contents, input_error = _read_allowlisted_source(root, relative_path)
        if input_error is not None:
            input_diagnostics.append((relative_path, input_error))
            continue
        if contents is None:
            continue

        for directive in _preprocessor_includes(contents):
            line_number = directive.line_number
            include_path = directive.include_path
            if include_path is None:
                violations.append(Violation(relative_path, line_number, None, None))
                continue
            if _is_absolute_include_path(include_path):
                violations.append(Violation(relative_path, line_number, None, None, "uncheckable absolute include"))
                continue
            rule = _rule_for_include(relative_path, include_path)
            if rule is None:
                continue
            violations.append(Violation(relative_path, line_number, rule, include_path))

    ordered_input_diagnostics = [diagnostic for _, diagnostic in sorted(input_diagnostics)]
    ordered_violations = sorted(
        violations,
        key=lambda violation: (
            violation.source_path,
            violation.line_number,
            violation.include_path or "",
            violation.rule or "",
        ),
    )
    diagnostics = tuple(ordered_input_diagnostics + [violation.diagnostic() for violation in ordered_violations])
    return CheckResult(exit_code=1 if diagnostics else 0, diagnostics=diagnostics)


class BoundaryCheckerTests(unittest.TestCase):
    def write_fixture(self, root: Path, relative_path: str, contents: str) -> None:
        path = root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")

    def test_allows_unrelated_includes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(root, "emulator/src/AudioHost.cpp", '#include "Emulator/Kernel/Time.h"\n')
            self.write_fixture(root, "emulator/src/Loader/RuntimeLinker.cpp", '#include "Emulator/Loader/Elf.h"\n')

            result = check_source_root(root)

            self.assertEqual(result, CheckResult(exit_code=0, diagnostics=()))

    def test_bounds_raw_string_delimiter_scan(self) -> None:
        class RawDelimiterProbe(str):
            def __new__(cls, value: str) -> "RawDelimiterProbe":
                instance = super().__new__(cls, value)
                instance.search_limits = []
                return instance

            def find(self, substring: str, start: int = 0, end: Optional[int] = None) -> int:
                if substring == "(":
                    self.search_limits.append(end)
                if end is None:
                    return super().find(substring, start)
                return super().find(substring, start, end)

        raw_candidate = RawDelimiterProbe('R"' + "x" * 64)

        self.assertEqual(_skip_raw_string_literal(raw_candidate, 0), 1)
        self.assertEqual(raw_candidate.search_limits, [19])

    def test_rejects_forbidden_include_after_utf8_bom(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(root, "emulator/src/AudioHost.cpp", '\ufeff#include "Emulator/Graphics/Graphics.h"\n')

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=("emulator/src/AudioHost.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Graphics.h",),
                ),
            )

    def test_rejects_forbidden_audio_graphics_include(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(root, "emulator/src/AudioHost.cpp", '#include "Emulator/Graphics/Graphics.h"\n')

            result = check_source_root(root)

            self.assertEqual(result.exit_code, 1)
            self.assertEqual(
                result.diagnostics,
                ("emulator/src/AudioHost.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Graphics.h",),
            )

    def test_rejects_forbidden_kernel_graphics_include(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(root, "emulator/src/Kernel/FileSystem.cpp", '#include "Emulator/Graphics/Objects/GpuMemory.h"\n')

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=(
                        "emulator/src/Kernel/FileSystem.cpp:1: forbidden include (Kernel -> Graphics): Emulator/Graphics/Objects/GpuMemory.h",
                    ),
                ),
            )

    def test_rejects_forbidden_host_graphics_include(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(root, "emulator/src/Host/CaptureImageCodec.cpp", '#include "Emulator/Graphics/Utils.h"\n')

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=(
                        "emulator/src/Host/CaptureImageCodec.cpp:1: forbidden include (Host -> Graphics): Emulator/Graphics/Utils.h",
                    ),
                ),
            )

    def test_rejects_forbidden_libs_graphics_include(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(root, "emulator/src/Libs/LibC.cpp", '#include "Emulator/Graphics/Objects/GpuMemory.h"\n')

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=(
                        "emulator/src/Libs/LibC.cpp:1: forbidden include (Libs -> Graphics): Emulator/Graphics/Objects/GpuMemory.h",
                    ),
                ),
            )

    def test_rejects_direct_sdl_include_from_libs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(root, "emulator/src/Libs/LibSaveData.cpp", '#include "SDL_filesystem.h"\n')

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=(
                        "emulator/src/Libs/LibSaveData.cpp:1: forbidden include (Libs -> SDL): SDL_filesystem.h",
                    ),
                ),
            )

    def test_rejects_forbidden_loader_graphics_include(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(root, "emulator/src/Loader/SystemContent.cpp", '#include "Emulator/Graphics/Image.h"\n')

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=(
                        "emulator/src/Loader/SystemContent.cpp:1: forbidden include (Loader -> Graphics): Emulator/Graphics/Image.h",
                    ),
                ),
            )

    def test_rejects_runtime_linker_profiler_include(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(root, "emulator/src/Loader/RuntimeLinker.cpp", '#include "Emulator/Profiler.h"\n')

            expected = CheckResult(
                exit_code=1,
                diagnostics=(
                    "emulator/src/Loader/RuntimeLinker.cpp:1: forbidden include (RuntimeLinker -> Profiler): Emulator/Profiler.h",
                ),
            )
            self.assertEqual(check_source_root(root), expected)
            self.assertEqual(check_source_root(root, strict=True), expected)

    def test_rejects_forbidden_include_with_preprocessor_whitespace(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(
                root,
                "emulator/src/Audio.cpp",
                '#include"Emulator/Graphics/Shader.h"\n'
                '/**/#include "Emulator/Graphics/Color.h"\n',
            )
            self.write_fixture(root, "emulator/src/AudioHost.cpp", '#/**/include/**/"Emulator/Graphics/Graphics.h"\n')
            self.write_fixture(root, "emulator/src/AudioPcm.cpp", '#include \\\n"Emulator/Graphics/Window.h"\n')

            result = check_source_root(root)

            self.assertEqual(result.exit_code, 1)
            self.assertEqual(
                result.diagnostics,
                (
                    "emulator/src/Audio.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Shader.h",
                    "emulator/src/Audio.cpp:2: forbidden include (Audio -> Graphics): Emulator/Graphics/Color.h",
                    "emulator/src/AudioHost.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Graphics.h",
                    "emulator/src/AudioPcm.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Window.h",
                ),
            )

    def test_ignores_commented_out_include_text(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(
                root,
                "emulator/src/Audio.cpp",
                '/*\n#include "Emulator/Graphics/Graphics.h"\n*/\n'
                '// #include "Emulator/Graphics/Window.h"\n',
            )

            self.assertEqual(check_source_root(root), CheckResult(exit_code=0, diagnostics=()))

    def test_rejects_header_names_and_horizontal_whitespace(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(
                root,
                "emulator/src/Audio.cpp",
                '#include <Emulator/Graphics//Graphics.h>\n'
                '#include\v"Emulator/Graphics/Color.h"\n'
                '#include\f"Emulator/Graphics/Window.h"\n',
            )

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=(
                        "emulator/src/Audio.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics//Graphics.h",
                        "emulator/src/Audio.cpp:2: forbidden include (Audio -> Graphics): Emulator/Graphics/Color.h",
                        "emulator/src/Audio.cpp:3: forbidden include (Audio -> Graphics): Emulator/Graphics/Window.h",
                    ),
                ),
            )

    def test_rejects_canonicalized_forbidden_include_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(
                root,
                "emulator/src/Audio.cpp",
                '#include "Emulator\\Graphics\\Graphics.h"\n'
                '#include "Emulator//Graphics/Color.h"\n'
                '#include "Emulator/Other/../Graphics/Window.h"\n'
                '#include "emulator/graphics/Shader.h"\n'
                '#include "Emulator\\Graphics\\GuestTextureLayout.h"\n',
            )
            self.write_fixture(
                root,
                "emulator/src/Loader/RuntimeLinker.cpp",
                '#include "Kyty\\DevTools\\Telemetry\\Progress.h"\n'
                '#include "Kyty/Tools/../DevTools/Telemetry/WriterRegistry.h"\n',
            )

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=(
                        "emulator/src/Audio.cpp:1: forbidden include (Audio -> Graphics): Emulator\\Graphics\\Graphics.h",
                        "emulator/src/Audio.cpp:2: forbidden include (Audio -> Graphics): Emulator//Graphics/Color.h",
                        "emulator/src/Audio.cpp:3: forbidden include (Audio -> Graphics): Emulator/Other/../Graphics/Window.h",
                        "emulator/src/Audio.cpp:4: forbidden include (Audio -> Graphics): emulator/graphics/Shader.h",
                        "emulator/src/Audio.cpp:5: forbidden include (Audio -> Graphics): Emulator\\Graphics\\GuestTextureLayout.h",
                        "emulator/src/Loader/RuntimeLinker.cpp:1: forbidden include (RuntimeLinker -> DevTools): Kyty\\DevTools\\Telemetry\\Progress.h",
                        "emulator/src/Loader/RuntimeLinker.cpp:2: forbidden include (RuntimeLinker -> DevTools): Kyty/Tools/../DevTools/Telemetry/WriterRegistry.h",
                    ),
                ),
            )

    def test_rejects_digraph_include_directive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(root, "emulator/src/AudioHost.cpp", '%:include "Emulator/Graphics/Graphics.h"\n')

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=("emulator/src/AudioHost.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Graphics.h",),
                ),
            )

    def test_rejects_forbidden_relative_include_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(
                root,
                "emulator/src/AudioHost.cpp",
                '#include "../include/Emulator/Graphics/Graphics.h"\n',
            )
            self.write_fixture(
                root,
                "emulator/src/Loader/RuntimeLinker.cpp",
                '#include "../../../devtools/include/Kyty/DevTools/Telemetry/Progress.h"\n',
            )

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=(
                        "emulator/src/AudioHost.cpp:1: forbidden include (Audio -> Graphics): ../include/Emulator/Graphics/Graphics.h",
                        "emulator/src/Loader/RuntimeLinker.cpp:1: forbidden include (RuntimeLinker -> DevTools): ../../../devtools/include/Kyty/DevTools/Telemetry/Progress.h",
                    ),
                ),
            )

    def test_rejects_absolute_allowlisted_include(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(
                root,
                "emulator/src/AudioPcm.cpp",
                '#include "/host/private/Emulator/Graphics/Graphics.h"\n',
            )

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=("emulator/src/AudioPcm.cpp:1: uncheckable absolute include",),
                ),
            )

    def test_rejects_drive_qualified_allowlisted_include(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(
                root,
                "emulator/src/Audio.cpp",
                '#include "C:..\\..\\source\\emulator\\include\\Emulator\\Graphics\\GuestTextureLayout.h"\n',
            )
            self.write_fixture(
                root,
                "emulator/src/Loader/RuntimeLinker.cpp",
                '#include "D:..\\..\\source\\devtools\\include\\Kyty\\DevTools\\Telemetry\\Progress.h"\n',
            )

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=(
                        "emulator/src/Audio.cpp:1: uncheckable absolute include",
                        "emulator/src/Loader/RuntimeLinker.cpp:1: uncheckable absolute include",
                    ),
                ),
            )

    def test_rejects_nonliteral_allowlisted_include(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(
                root,
                "emulator/src/Audio.cpp",
                '#define GFX "Emulator/Graphics/Graphics.h"\n'
                "#include GFX\n",
            )

            self.assertEqual(
                check_source_root(root),
                CheckResult(
                    exit_code=1,
                    diagnostics=("emulator/src/Audio.cpp:2: uncheckable nonliteral include",),
                ),
            )

    def test_rejects_missing_source_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            missing_root = Path(directory) / "missing-source"

            result = check_source_root(missing_root)

            self.assertEqual(result.exit_code, 1)
            self.assertEqual(result.diagnostics, (f"source root is not a directory: {missing_root}",))

    def test_sorts_forbidden_diagnostics_by_source_and_line(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(root, "emulator/src/AudioHost.cpp", '#include "Emulator/Graphics/Window.h"\n')
            self.write_fixture(
                root,
                "emulator/src/Loader/RuntimeLinker.cpp",
                '\n#include "Kyty/DevTools/Telemetry/Progress.h"\n',
            )
            self.write_fixture(root, "emulator/src/Audio.cpp", '#include "Emulator/Graphics/Graphics.h"\n')

            result = check_source_root(root)

            self.assertEqual(result.exit_code, 1)
            self.assertEqual(
                result.diagnostics,
                (
                    "emulator/src/Audio.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Graphics.h",
                    "emulator/src/AudioHost.cpp:1: forbidden include (Audio -> Graphics): Emulator/Graphics/Window.h",
                    "emulator/src/Loader/RuntimeLinker.cpp:2: forbidden include (RuntimeLinker -> DevTools): Kyty/DevTools/Telemetry/Progress.h",
                ),
            )

    def test_normal_and_strict_modes_reject_audio_graphics_includes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_fixture(
                root,
                "emulator/src/Audio.cpp",
                "\n" * 10
                + '#include "Emulator/Graphics/GuestTextureLayout.h"\n'
                '#include "Emulator/Graphics/Objects/GpuMemory.h"\n',
            )

            expected = CheckResult(
                exit_code=1,
                diagnostics=(
                    "emulator/src/Audio.cpp:11: forbidden include (Audio -> Graphics): Emulator/Graphics/GuestTextureLayout.h",
                    "emulator/src/Audio.cpp:12: forbidden include (Audio -> Graphics): Emulator/Graphics/Objects/GpuMemory.h",
                ),
            )
            self.assertEqual(check_source_root(root), expected)
            self.assertEqual(check_source_root(root, strict=True), expected)

def parse_arguments(argv: Optional[Iterable[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_root", nargs="?", type=Path, help="path to the source root")
    parser.add_argument("--strict", action="store_true", help="compatibility spelling; the normal policy already rejects every guarded edge")
    parser.add_argument("--self-test", action="store_true", help="run checker fixture tests")
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        if arguments.source_root is not None or arguments.strict:
            parser.error("--self-test cannot be combined with source_root or --strict")
    elif arguments.source_root is None:
        parser.error("source_root is required unless --self-test is used")
    return arguments


def main(argv: Optional[Iterable[str]] = None) -> int:
    arguments = parse_arguments(argv)
    if arguments.self_test:
        result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(BoundaryCheckerTests))
        return 0 if result.wasSuccessful() else 1

    result = check_source_root(arguments.source_root, strict=arguments.strict)
    for diagnostic in result.diagnostics:
        print(diagnostic)
    return result.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
