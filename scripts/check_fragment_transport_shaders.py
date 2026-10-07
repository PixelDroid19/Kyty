#!/usr/bin/env python3
"""Generate or verify the public host wave-transport kernels for Vulkan 1.4."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import tempfile

import check_gen5_detile_shader as blobs


ROOT = pathlib.Path(__file__).resolve().parent.parent
SHADERS = ROOT / "source/emulator/src/Graphics/host_shaders"
KERNELS = ("scan", "pack")


def process(name: str, scratch: pathlib.Path, write: bool, compiler: str, validator: str) -> int:
    stem = f"fragment_wave_{name}"
    source = SHADERS / f"{stem}.comp"
    include = SHADERS / f"{stem}_comp.inc"
    generated = scratch / f"{stem}.spv"
    blobs.ARRAY_NAME = f"kFragmentWave{name.title()}Spirv"
    words = blobs.compile_shader(compiler, source, generated)
    blobs.validate(validator, generated)
    if write:
        blobs.write_include(include, words)
        text = include.read_text(encoding="utf-8")
        text = text.replace("gen5_detile.comp", source.name).replace("check_gen5_detile_shader.py", "check_fragment_transport_shaders.py")
        include.write_text(text, encoding="utf-8")
    checked = blobs.parse_include(include)
    checked_path = scratch / f"{stem}_checked.spv"
    blobs.write_spirv(checked_path, checked)
    blobs.validate(validator, checked_path)
    if words != checked:
        raise ValueError(f"{stem}: {blobs.first_difference(words, checked)}")
    return len(words)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--compiler", default="glslc")
    parser.add_argument("--validator", default="spirv-val")
    parser.add_argument("--disassembler", default="spirv-dis")
    parser.add_argument("--scratch", required=True, type=pathlib.Path, help="Owned build scratch directory, outside the repo")
    args = parser.parse_args()
    try:
        args.scratch.mkdir(parents=True, exist_ok=True)
        compiler = blobs.executable(args.compiler)
        validator = blobs.executable(args.validator)
        with tempfile.TemporaryDirectory(prefix="fragment-kernels-", dir=args.scratch) as directory:
            for name in KERNELS:
                count = process(name, pathlib.Path(directory), args.write, compiler, validator)
                print(f"fragment {name}: {count} validated words")
            check_capture_core(pathlib.Path(directory), args.write, compiler, validator, blobs.executable(args.disassembler))
            check_resolve_core(pathlib.Path(directory), args.write, compiler, validator, blobs.executable(args.disassembler))
    except (ValueError, OSError) as error:
        print(f"fragment transport shader check failed: {error}")
        return 1
    return 0


def check_capture_core(scratch: pathlib.Path, write: bool, compiler: str, validator: str, disassembler: str) -> None:
    binary = scratch / "capture.spv"
    blobs.run([compiler, "--target-env=vulkan1.4", "-O0", "-o", str(binary), str(SHADERS / "fragment_wave_capture.frag")])
    blobs.validate(validator, binary)
    text = subprocess.run([disassembler, str(binary)], check=True, capture_output=True, text=True).stdout
    reader = re.search(r"(?m)^%read_lane_word_u1_ = OpFunction.*?OpFunctionEnd\n", text, re.S)
    if reader is None:
        raise ValueError("capture core reader function missing")
    signature = reader.group().splitlines()[:2]
    text = text[:reader.start()] + "<capture_reader>\n" + text[reader.end():]
    text = re.sub(r"(?m)^.*(?:OpName %capture_stub|OpDecorate %capture_stub|%capture_stub = OpVariable).*\n", "", text)
    text = text.replace(" %capture_stub ", " <capture_interface> ", 1)
    types = re.search(r"(?m)^\s*%void = OpTypeVoid", text)
    if types is None:
        raise ValueError("capture core types missing")
    text = text[:types.start()] + "<capture_annotations>\n" + text[types.start():]
    anchor = re.search(r"(?m)^\s*%main = OpFunction", text)
    if anchor is None:
        raise ValueError("capture core entry missing")
    text = text[:anchor.start()] + "<capture_declarations>\n" + text[anchor.start():]
    output = "// Generated from the public capture kernel. Do not edit by hand.\n"
    output += 'constexpr const char* kFragmentCaptureReaderSignature = R"CORE(' + "\n".join(signature) + '\n)CORE";\n'
    output += 'constexpr const char* kFragmentCaptureCore = R"CORE(' + text + ')CORE";\n'
    include = SHADERS / "fragment_wave_capture_asm.inc"
    if write:
        include.write_text(output, encoding="utf-8")
    elif include.read_text(encoding="utf-8") != output:
        raise ValueError("capture core/source mismatch")
    print("fragment capture core: validated and verified")


def check_resolve_core(scratch: pathlib.Path, write: bool, compiler: str, validator: str, disassembler: str) -> None:
    binary = scratch / "resolve.spv"
    blobs.run([compiler, "--target-env=vulkan1.4", "-O0", "-o", str(binary), str(SHADERS / "fragment_wave_resolve.frag")])
    blobs.validate(validator, binary)
    text = subprocess.run([disassembler, str(binary)], check=True, capture_output=True, text=True).stdout
    writer = re.search(r"(?m)^%write_colors_u1_ = OpFunction.*?OpFunctionEnd\n", text, re.S)
    if writer is None:
        raise ValueError("resolve core writer function missing")
    signature = writer.group().splitlines()[:2]
    text = text[:writer.start()] + "<resolve_writer>\n" + text[writer.end():]
    text = re.sub(r"(?m)^.*(?:OpName %resolve_stub|OpDecorate %resolve_stub|%resolve_stub = OpVariable).*\n", "", text)
    text = text.replace(" %resolve_stub ", " <resolve_interface> ", 1)
    text = re.sub(r"(?m)^.*OpDecorate %required_components SpecId.*\n", "", text)
    text = text.replace("%required_components = OpSpecConstant %uint 15", "%required_components = OpConstant %uint <resolve_mask>")
    types = re.search(r"(?m)^\s*%void = OpTypeVoid", text)
    if types is None:
        raise ValueError("resolve core types missing")
    text = text[:types.start()] + "<resolve_annotations>\n" + text[types.start():]
    anchor = re.search(r"(?m)^\s*%main = OpFunction", text)
    if anchor is None:
        raise ValueError("resolve core entry missing")
    text = text[:anchor.start()] + "<resolve_declarations>\n" + text[anchor.start():]
    output = "// Generated from the public resolve kernel. Do not edit by hand.\n"
    output += 'constexpr const char* kFragmentResolveWriterSignature = R"CORE(' + "\n".join(signature) + '\n)CORE";\n'
    output += 'constexpr const char* kFragmentResolveCore = R"CORE(' + text + ')CORE";\n'
    include = SHADERS / "fragment_wave_resolve_asm.inc"
    if write:
        include.write_text(output, encoding="utf-8")
    elif include.read_text(encoding="utf-8") != output:
        raise ValueError("resolve core/source mismatch")
    print("fragment resolve core: validated and verified")


if __name__ == "__main__":
    raise SystemExit(main())
