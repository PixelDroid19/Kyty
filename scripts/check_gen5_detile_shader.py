#!/usr/bin/env python3
"""Regenerate or verify the checked-in Gen5 detile SPIR-V blob.

The normal CMake build consumes the checked-in include and deliberately does
not require a host shader compiler. This developer command uses glslc -O,
validates both binaries with spirv-val, and compares the generated words with
the include exactly.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = REPO_ROOT / "source/emulator/src/Graphics/host_shaders/gen5_detile.comp"
INCLUDE = REPO_ROOT / "source/emulator/src/Graphics/host_shaders/gen5_detile_comp.inc"
ARRAY_NAME = "kGen5DetileCompSpirv"
WORDS_PER_LINE = 6


def fail(message: str) -> None:
    raise ValueError(message)


def read_words(path: pathlib.Path) -> list[int]:
    try:
        data = path.read_bytes()
    except OSError as error:
        fail(f"cannot read SPIR-V {path}: {error}")
    if not data or len(data) % 4 != 0:
        fail(f"SPIR-V must be a non-empty multiple of four bytes: {path}")
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def parse_include(path: pathlib.Path) -> list[int]:
    try:
        contents = path.read_text(encoding="utf-8")
    except OSError as error:
        fail(f"cannot read include {path}: {error}")
    match = re.search(
        rf"constexpr\s+uint32_t\s+{ARRAY_NAME}\[(\d+)\]\s*=\s*\{{(.*?)\}};",
        contents,
        flags=re.DOTALL,
    )
    if match is None:
        fail(f"cannot find {ARRAY_NAME} in {path}")
    declared_count = int(match.group(1))
    words = [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]+)u", match.group(2))]
    if len(words) != declared_count:
        fail(f"{path}: declared {declared_count} words but parsed {len(words)}")
    if not words or words[0] != 0x07230203:
        fail(f"{path}: invalid SPIR-V magic")
    return words


def write_include(path: pathlib.Path, words: list[int]) -> None:
    lines = [
        "// Auto-generated from gen5_detile.comp with scripts/check_gen5_detile_shader.py --write.",
        "// Do not edit by hand.",
        f"constexpr uint32_t {ARRAY_NAME}[{len(words)}] = {{",
    ]
    for index in range(0, len(words), WORDS_PER_LINE):
        line = ", ".join(f"0x{word:08x}u" for word in words[index : index + WORDS_PER_LINE])
        lines.append(f"\t{line},")
    lines.append("};")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def executable(name: str) -> str:
    resolved = shutil.which(name)
    if resolved is None:
        fail(f"required tool not found: {name}")
    return resolved


def run(command: list[str]) -> None:
    try:
        subprocess.run(command, check=True)
    except OSError as error:
        fail(f"cannot run {' '.join(command)}: {error}")
    except subprocess.CalledProcessError as error:
        fail(f"command failed ({error.returncode}): {' '.join(command)}")


def compile_shader(compiler: str, source: pathlib.Path, output: pathlib.Path) -> list[int]:
    run([compiler, "-fshader-stage=compute", "-O", "-o", str(output), str(source)])
    return read_words(output)


def validate(validator: str, path: pathlib.Path) -> None:
    run([validator, "--target-env", "vulkan1.0", str(path)])


def write_spirv(path: pathlib.Path, words: list[int]) -> None:
    path.write_bytes(struct.pack(f"<{len(words)}I", *words))


def first_difference(generated: list[int], checked_in: list[int]) -> str:
    for index, (left, right) in enumerate(zip(generated, checked_in)):
        if left != right:
            return f"word {index}: generated 0x{left:08x}, checked-in 0x{right:08x}"
    if len(generated) != len(checked_in):
        return f"word count: generated {len(generated)}, checked-in {len(checked_in)}"
    return "unknown difference"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="verify source, SPIR-V validity, and checked-in words (default)")
    mode.add_argument("--write", action="store_true", help="regenerate the checked-in include from the shader source")
    parser.add_argument("--compiler", default="glslc", help="glslc executable to use (default: %(default)s)")
    parser.add_argument("--validator", default="spirv-val", help="spirv-val executable to use (default: %(default)s)")
    args = parser.parse_args()

    try:
        compiler = executable(args.compiler)
        validator = executable(args.validator)
        if not SOURCE.is_file():
            fail(f"missing shader source: {SOURCE}")
        with tempfile.TemporaryDirectory(prefix="kyty-gen5-detile-") as directory:
            root = pathlib.Path(directory)
            generated_path = root / "gen5_detile.spv"
            generated_words = compile_shader(compiler, SOURCE, generated_path)
            validate(validator, generated_path)

            if args.write:
                write_include(INCLUDE, generated_words)
                checked_in_words = generated_words
            else:
                checked_in_words = parse_include(INCLUDE)

            checked_in_path = root / "checked_in.spv"
            write_spirv(checked_in_path, checked_in_words)
            validate(validator, checked_in_path)

            if generated_words != checked_in_words:
                fail(f"source/blob mismatch: {first_difference(generated_words, checked_in_words)}")
    except ValueError as error:
        print(f"gen5 detile shader check failed: {error}", file=sys.stderr)
        return 1

    action = "regenerated" if args.write else "verified"
    print(f"gen5 detile shader {action}: {len(generated_words)} SPIR-V words")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
