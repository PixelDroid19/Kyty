#!/usr/bin/env python3
"""Integer instruction oracle: native AMD gfx1030 execution and external comparison.

The native backend compiles one HIP kernel whose five 32-bit integer operations
are AMDGCN inline assembly (v_add_nc_u32, v_mul_lo_u32, v_mul_hi_u32,
v_lshlrev_b32, v_lshrrev_b32). Before the binary is run, the compiler's device
assembly is checked for each mnemonic, so a C++ fallback cannot pass as an ISA
measurement. Only gfx1030 is supported.

The Vulkan endpoint is an external executable that runs the production shader
pipeline. This module only invokes it with a fixed argument list and validates its
protocol. When the executable is absent, the backend is reported as unavailable.

`compare` checks two measured result directories for the same input bytes. The
host arithmetic reference is recorded alongside each measurement, but it is not
the oracle and does not decide the comparison.

Scope: five unsigned 32-bit operations on gfx1030 only. No floating-point,
denormal, graphics or frame behavior is covered.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys


SCHEMA = "kyty_hardware_oracle_v2"
PROBE_SCHEMA = "kyty_hardware_oracle_probe_v1"
ENDPOINT_SCHEMA = "kyty_shader_integer_endpoint_v1"
KFD_DEVICE = Path("/dev/kfd")
HIP_COMPILER = "hipcc"
SUPPORTED_ARCH = "gfx1030"
OPS = ("add", "mul", "mulhi", "shl", "shr")
MODES = ("seeded", "edges")
WAVE_WIDTHS = (32, 64)
MAX_COUNT = 1 << 16
MAX_SEED = (1 << 32) - 1
MAX_META_BYTES = 4096
MAX_VERSION_BYTES = 8192
MAX_ASM_BYTES = 1 << 20
MAX_ENDPOINT_DISPATCHES = 64
MASK32 = 0xFFFFFFFF
MASK64 = (1 << 64) - 1
EDGE_VALUES = (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)
ARCH_PATTERN = re.compile(r"gfx[0-9a-f]{3,6}")
GCN_NAME_PATTERN = re.compile(r"gfx[0-9a-f]{3,6}(:[a-z0-9+-]{1,16})*")
DEVICE_NAME_PATTERN = re.compile(r"[A-Za-z0-9 ._()/+-]{1,128}")
VERSION_LINE_PATTERN = re.compile(r"[ -~]{1,200}")
DEVICE_META_KEYS = {"warp_size", "device_name", "gcn_arch_name", "runtime_version", "driver_version"}
ENDPOINT_META_KEYS = {"schema", "backend", "op", "pairs", "dispatches", "physical_device", "subgroup_size", "measured"}
ENDPOINT_OPTIONAL_KEYS = {"default_subgroup_size"}
INPUT_MD5_PATTERN = re.compile(r"[0-9a-f]{32}")
VULKAN_MAX_PAIRS = 1024          # 16 pairs per dispatch x 64 dispatches
VULKAN_PAIRS_PER_DISPATCH = 16
VULKAN_LOGICAL_WAVE = 64        # the endpoint runs the paired logical wave64 only
ENDPOINT_UNAVAILABLE_EXIT = 77  # the endpoint reports pre-dispatch unavailability with this status
ENDPOINT_REFUSED_EXIT = 2       # the endpoint refused the request as a usage or bound violation
ASM_MNEMONICS = ("v_add_nc_u32", "v_mul_lo_u32", "v_mul_hi_u32", "v_lshlrev_b32", "v_lshrrev_b32")
COMPILE_TIMEOUT_S = 120
RUN_TIMEOUT_S = 60
ENDPOINT_TIMEOUT_S = 120
REPO_ROOT = Path(__file__).resolve().parents[1]
EXIT_OK = 0
EXIT_MISMATCH = 1
EXIT_USAGE = 2
EXIT_UNAVAILABLE = 3
EXIT_ERROR = 4

HIP_SOURCE = r'''// Integer instruction oracle for gfx1030. Each arithmetic result is produced by one
// AMDGCN instruction written as inline assembly. There is no C++ arithmetic fallback.
// Usage: oracle <op-index> <count> <inputs.bin> <outputs.bin> <meta.json>
// inputs.bin holds count little-endian uint32 operands a, then count operands b.
#if !defined(__gfx1030__)
#error "this oracle supports gfx1030 only"
#endif
#include <hip/hip_runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr uint32_t BLOCK_SIZE = 256;
constexpr uint32_t MAX_COUNT = 1u << 16;
constexpr uint32_t OP_SHR = 4;

// Operand order is published per instruction: D = src0 op src1. For shifts, src0 is the
// shift count (y) and src1 is the value (x), so D = x << (y & 31) and D = x >> (y & 31).
__global__ void ArithmeticOracle(const uint32_t* a, const uint32_t* b, uint32_t* out, uint32_t count, uint32_t op)
{
	const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
	if (i >= count) {
		return;
	}
	const uint32_t x = a[i];
	const uint32_t y = b[i];
	uint32_t       r = 0;
	switch (op) {
		case 0: asm volatile("v_add_nc_u32 %0, %1, %2" : "=v"(r) : "v"(x), "v"(y)); break;
		case 1: asm volatile("v_mul_lo_u32 %0, %1, %2" : "=v"(r) : "v"(x), "v"(y)); break;
		case 2: asm volatile("v_mul_hi_u32 %0, %1, %2" : "=v"(r) : "v"(x), "v"(y)); break;
		case 3: asm volatile("v_lshlrev_b32 %0, %1, %2" : "=v"(r) : "v"(y), "v"(x)); break;
		default: asm volatile("v_lshrrev_b32 %0, %1, %2" : "=v"(r) : "v"(y), "v"(x)); break;
	}
	out[i] = r;
}

} // namespace

#define HIP_CHECK(expr)                                                        \
	do {                                                                       \
		const hipError_t status_ = (expr);                                     \
		if (status_ != hipSuccess) {                                           \
			std::fprintf(stderr, "hip error %d at line %d\n", static_cast<int>(status_), __LINE__); \
			return 4;                                                          \
		}                                                                      \
	} while (0)

int main(int argc, char** argv)
{
	if (argc != 6) {
		return 2;
	}
	const long op = std::strtol(argv[1], nullptr, 10);
	const long count_arg = std::strtol(argv[2], nullptr, 10);
	if (op < 0 || op > static_cast<long>(OP_SHR) || count_arg < 1 || count_arg > static_cast<long>(MAX_COUNT)) {
		return 2;
	}
	const uint32_t count = static_cast<uint32_t>(count_arg);

	std::vector<uint32_t> host(2u * count);
	std::FILE* input = std::fopen(argv[3], "rb");
	if (input == nullptr) {
		return 2;
	}
	const size_t read = std::fread(host.data(), sizeof(uint32_t), host.size(), input);
	std::fclose(input);
	if (read != host.size()) {
		return 2;
	}

	hipDeviceProp_t props {};
	HIP_CHECK(hipGetDeviceProperties(&props, 0));
	int runtime_version = 0;
	int driver_version = 0;
	HIP_CHECK(hipRuntimeGetVersion(&runtime_version));
	HIP_CHECK(hipDriverGetVersion(&driver_version));
	const size_t bytes = sizeof(uint32_t) * count;
	uint32_t*    a     = nullptr;
	uint32_t*    b     = nullptr;
	uint32_t*    out   = nullptr;
	HIP_CHECK(hipMalloc(&a, bytes));
	HIP_CHECK(hipMalloc(&b, bytes));
	HIP_CHECK(hipMalloc(&out, bytes));
	HIP_CHECK(hipMemcpy(a, host.data(), bytes, hipMemcpyHostToDevice));
	HIP_CHECK(hipMemcpy(b, host.data() + count, bytes, hipMemcpyHostToDevice));

	const uint32_t blocks = (count + BLOCK_SIZE - 1) / BLOCK_SIZE;
	hipLaunchKernelGGL(ArithmeticOracle, dim3(blocks), dim3(BLOCK_SIZE), 0, 0, a, b, out, count, static_cast<uint32_t>(op));
	HIP_CHECK(hipGetLastError());
	HIP_CHECK(hipDeviceSynchronize());

	std::vector<uint32_t> result(count);
	HIP_CHECK(hipMemcpy(result.data(), out, bytes, hipMemcpyDeviceToHost));
	HIP_CHECK(hipFree(a));
	HIP_CHECK(hipFree(b));
	HIP_CHECK(hipFree(out));

	std::FILE* output = std::fopen(argv[4], "wb");
	if (output == nullptr || std::fwrite(result.data(), sizeof(uint32_t), result.size(), output) != result.size()) {
		return 3;
	}
	std::fclose(output);
	std::FILE* meta = std::fopen(argv[5], "wb");
	if (meta == nullptr) {
		return 3;
	}
	std::fprintf(meta,
	             "{\"warp_size\": %d, \"device_name\": \"%s\", \"gcn_arch_name\": \"%s\", "
	             "\"runtime_version\": %d, \"driver_version\": %d}\n",
	             props.warpSize, props.name, props.gcnArchName, runtime_version, driver_version);
	std::fclose(meta);
	return 0;
}
'''


class HardwareOracleError(ValueError):
    pass


class CommandTimeout(HardwareOracleError):
    pass


def host_reference(op, a, b):
    """Exact unsigned 32-bit results. Shift counts use their low five bits. Ancillary only."""
    if op not in OPS:
        raise HardwareOracleError(f"unknown operation {op!r}")
    if len(a) != len(b):
        raise HardwareOracleError("operand lists must have equal length")
    if op == "add":
        return [(x + y) & MASK32 for x, y in zip(a, b)]
    if op == "mul":
        return [(x * y) & MASK32 for x, y in zip(a, b)]
    if op == "mulhi":
        return [((x * y) >> 32) & MASK32 for x, y in zip(a, b)]
    if op == "shl":
        return [(x << (y & 31)) & MASK32 for x, y in zip(a, b)]
    return [x >> (y & 31) for x, y in zip(a, b)]


def make_inputs(mode, count, seed):
    """Returns operand lists a and b for a generated mode. Both modes are reproducible."""
    if mode == "edges":
        pairs = [(x, y) for x in EDGE_VALUES for y in EDGE_VALUES]
        chosen = [pairs[i % len(pairs)] for i in range(count)]
        return [x for x, _ in chosen], [y for _, y in chosen]
    if mode != "seeded":
        raise HardwareOracleError(f"unknown mode {mode!r}")
    state = seed & MASK64
    draws = []
    for _ in range(2 * count):
        state = (state * 6364136223846793005 + 1442695040888963407) & MASK64
        draws.append(state >> 32)
    return draws[:count], draws[count:]


def _open_regular(path):
    """Opens a file without following symbolic links and without blocking on FIFOs. Returns (handle, size).

    The type is checked on the open descriptor, so a path replaced between the check and the read
    still cannot be a FIFO, device or directory. Non-blocking open has no effect on regular files.
    """
    flags = os.O_RDONLY | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise HardwareOracleError(f"cannot open {Path(path).name}: {error.strerror or error}") from error
    try:
        status = os.fstat(descriptor)
        if not stat.S_ISREG(status.st_mode):
            raise HardwareOracleError(f"{Path(path).name} must be a regular file")
        handle = os.fdopen(descriptor, "rb")
    except BaseException:
        os.close(descriptor)
        raise
    return handle, status.st_size


def read_regular_file(path, limit):
    """Reads a regular file whose size is at most limit bytes. Larger files are refused before reading."""
    handle, size = _open_regular(path)
    with handle:
        if size > limit:
            raise HardwareOracleError(f"{Path(path).name} exceeds the {limit}-byte limit")
        return handle.read(limit + 1)


def read_output_bytes(path, limit):
    """Reads at most limit + 1 bytes of a regular output file, so the caller can compare the exact size."""
    handle, _ = _open_regular(path)
    with handle:
        return handle.read(limit + 1)


def build_inputs(request, max_pairs=MAX_COUNT):
    """Returns the operand pairs and their exact bytes. External files are used verbatim.

    The size bound is checked before the file is read, so an oversized input is refused cheaply.
    """
    if request["input_file"] is not None:
        data = read_regular_file(request["input_file"], 8 * max_pairs)
        if not data or len(data) % 8:
            raise HardwareOracleError("external input must hold a non-zero multiple of 8 bytes (uint32 pairs)")
        count = len(data) // 8
        values = struct.unpack(f"<{2 * count}I", data)
        a, b = list(values[:count]), list(values[count:])
    else:
        a, b = make_inputs(request["mode"], request["count"], request["seed"])
        count = request["count"]
        data = struct.pack(f"<{2 * count}I", *a, *b)
    return {"count": count, "a": a, "b": b, "data": data,
            "md5": hashlib.md5(data, usedforsecurity=False).hexdigest()}


def compare_outputs(expected, actual):
    if len(expected) != len(actual):
        return {"status": "length_mismatch", "mismatch_count": 0, "first_index": None,
                "expected": None, "actual": None}
    mismatches = [i for i, (e, a) in enumerate(zip(expected, actual)) if e != a]
    if not mismatches:
        return {"status": "match", "mismatch_count": 0, "first_index": None, "expected": None, "actual": None}
    first = mismatches[0]
    return {"status": "mismatch", "mismatch_count": len(mismatches), "first_index": first,
            "expected": expected[first], "actual": actual[first]}


def check_device_assembly(text):
    """Returns the expected mnemonics that do not appear as instructions. Comments do not count."""
    return [m for m in ASM_MNEMONICS if re.search(rf"^\s*{m}\s", text, re.MULTILINE) is None]


def validate_device_metadata(meta):
    """Strict schema for the device metadata written by the kernel. Returns the artifact fields."""
    if not isinstance(meta, dict) or set(meta) != DEVICE_META_KEYS:
        raise HardwareOracleError("device metadata has missing or unknown keys")
    warp_size = meta["warp_size"]
    if type(warp_size) is not int or warp_size not in WAVE_WIDTHS:
        raise HardwareOracleError("device warp size must be 32 or 64")
    device_name = meta["device_name"]
    if not isinstance(device_name, str) or DEVICE_NAME_PATTERN.fullmatch(device_name) is None:
        raise HardwareOracleError("device name has an unexpected form")
    gcn_name = meta["gcn_arch_name"]
    if not isinstance(gcn_name, str) or GCN_NAME_PATTERN.fullmatch(gcn_name) is None:
        raise HardwareOracleError("GCN architecture name has an unexpected form")
    versions = {}
    for key in ("runtime_version", "driver_version"):
        value = meta[key]
        if type(value) is not int or not 0 <= value <= (1 << 31) - 1:
            raise HardwareOracleError(f"{key} must be a non-negative 32-bit integer")
        versions[key] = value
    return {"warp_size": warp_size, "device_name": device_name, "gcn_arch_name": gcn_name, **versions}


def validate_endpoint_meta(meta, op, count):
    """Strict schema for the Vulkan endpoint's own report. Measured only when it says so."""
    if not isinstance(meta, dict) or not ENDPOINT_META_KEYS <= set(meta) or \
            set(meta) - ENDPOINT_META_KEYS - ENDPOINT_OPTIONAL_KEYS:
        raise HardwareOracleError("endpoint metadata has missing or unknown keys")
    if meta["schema"] != ENDPOINT_SCHEMA or meta["backend"] != "vulkan":
        raise HardwareOracleError("endpoint metadata schema or backend is not the Vulkan protocol")
    if meta["op"] != op or meta["pairs"] != count:
        raise HardwareOracleError("endpoint metadata does not describe this operation and input")
    dispatches = meta["dispatches"]
    expected_dispatches = -(-count // VULKAN_PAIRS_PER_DISPATCH)
    if type(dispatches) is not int or dispatches != expected_dispatches or dispatches > MAX_ENDPOINT_DISPATCHES:
        raise HardwareOracleError("endpoint dispatch count does not match the pair count and the 16-pair batches")
    subgroup = meta["subgroup_size"]
    if type(subgroup) is not int or subgroup not in (1, 2, 4, 8, 16, 32, 64, 128):
        raise HardwareOracleError("endpoint subgroup size must be a power of two up to 128")
    device = meta["physical_device"]
    if not isinstance(device, str) or DEVICE_NAME_PATTERN.fullmatch(device) is None:
        raise HardwareOracleError("endpoint physical device name has an unexpected form")
    if meta["measured"] is not True and meta["measured"] is not False:
        raise HardwareOracleError("endpoint measured flag must be a boolean")
    if "default_subgroup_size" in meta:
        default_subgroup = meta["default_subgroup_size"]
        if type(default_subgroup) is not int or default_subgroup not in (1, 2, 4, 8, 16, 32, 64, 128):
            raise HardwareOracleError("endpoint default subgroup size must be a power of two up to 128")
    return meta


def read_compiler_version(path):
    """First non-empty line of `hipcc --version`, restricted to printable ASCII."""
    with Path(path).open("rb") as handle:
        text = handle.read(MAX_VERSION_BYTES).decode("utf-8", errors="replace")
    for line in text.splitlines():
        stripped = line.strip()
        if stripped:
            if VERSION_LINE_PATTERN.fullmatch(stripped) is None:
                raise HardwareOracleError("compiler version line has an unexpected form")
            return stripped
    raise HardwareOracleError("compiler reported no version")


def probe(kfd_path=KFD_DEVICE, compiler=HIP_COMPILER):
    """Checks that the device node is accessible and that the HIP compiler resolves. Presence only."""
    kfd_ok = Path(kfd_path).exists() and os.access(kfd_path, os.R_OK | os.W_OK)
    compiler_ok = shutil.which(compiler) is not None
    checks = {"kfd_device": kfd_ok, "hip_compiler": compiler_ok}
    status = "present" if all(checks.values()) else "unavailable"
    return {"schema": PROBE_SCHEMA, "status": status, "checks": checks, "measurement": "not_run"}


def validate_request(op, mode, wave_width, count, seed, arch, out_dir, input_file=None, repo_root=REPO_ROOT):
    if op not in OPS:
        raise HardwareOracleError("operation must be one of " + ", ".join(OPS))
    if input_file is None and mode not in MODES:
        raise HardwareOracleError("mode must be one of " + ", ".join(MODES))
    if input_file is not None and mode is not None:
        raise HardwareOracleError("choose either a generated mode or an external input file, not both")
    if wave_width not in WAVE_WIDTHS:
        raise HardwareOracleError("wave width must be 32 or 64")
    if input_file is not None and count is not None:
        raise HardwareOracleError("--count cannot be combined with --input-file: the file defines the pair count")
    if input_file is None and (type(count) is not int or not 1 <= count <= MAX_COUNT):
        raise HardwareOracleError(f"count must be in [1, {MAX_COUNT}]")
    if type(seed) is not int or not 0 <= seed <= MAX_SEED:
        raise HardwareOracleError(f"seed must be in [0, {MAX_SEED}]")
    if arch != SUPPORTED_ARCH:
        raise HardwareOracleError(f"unsupported architecture {arch!r}: this oracle supports {SUPPORTED_ARCH} only")
    out_path = Path(out_dir)
    if not out_path.is_absolute():
        raise HardwareOracleError("output directory must be absolute")
    resolved = out_path.resolve()
    repo = Path(repo_root).resolve()
    if resolved == repo or repo in resolved.parents:
        raise HardwareOracleError("output directory must be outside the repository")
    if input_file is not None and not Path(input_file).is_absolute():
        raise HardwareOracleError("input file must be an absolute path")
    return {"op": op, "mode": mode if input_file is None else "external", "wave_width": wave_width,
            "count": count if input_file is None else None, "seed": seed, "arch": arch,
            "out_dir": resolved, "input_file": input_file}


def build_asm_argv(source, asm_out, wave_width, arch):
    argv = [HIP_COMPILER, "-O2", "-std=c++17", f"--offload-arch={arch}"]
    if wave_width == 64:
        argv.append("-mwavefrontsize64")
    argv += ["-S", "--cuda-device-only", "-o", str(asm_out), str(source)]
    return argv


def build_compile_argv(source, binary, wave_width, arch):
    argv = [HIP_COMPILER, "-O2", "-std=c++17", f"--offload-arch={arch}"]
    if wave_width == 64:
        argv.append("-mwavefrontsize64")
    argv += ["-o", str(binary), str(source)]
    return argv


def build_version_argv():
    return [HIP_COMPILER, "--version"]


def build_run_argv(binary, op, count, input_path, output_path, meta_path):
    return [str(binary), str(OPS.index(op)), str(count), str(input_path), str(output_path), str(meta_path)]


def build_endpoint_argv(endpoint, op, count, input_path, output_path, meta_path, wave_width, max_dispatches):
    return [str(endpoint), "--op", op, "--count", str(count), "--inputs", str(input_path), "--output", str(output_path),
            "--meta", str(meta_path), "--wave-width", str(wave_width), "--max-dispatches", str(max_dispatches)]


def run_command(argv, timeout, log_path):
    """Runs a fixed argument list without a shell; returns the exit status or raises CommandTimeout."""
    try:
        with Path(log_path).open("wb") as log:
            completed = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT, timeout=timeout, check=False, shell=False)
    except subprocess.TimeoutExpired as error:
        raise CommandTimeout("command timed out") from error
    return completed.returncode


def prepare_fresh_dir(out_dir):
    """The output directory must be new or empty, so no earlier result is overwritten."""
    out = Path(out_dir)
    if not out.parent.is_dir():
        raise HardwareOracleError("output directory parent does not exist")
    if out.is_symlink():
        raise HardwareOracleError("output directory must not be a symbolic link")
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise HardwareOracleError("output directory must be new or empty")
    out.mkdir(mode=0o700, exist_ok=True)


def write_exclusive(path, text):
    with Path(path).open("x", encoding="utf-8") as handle:
        handle.write(text)


def _read_bounded_json(path):
    try:
        text = read_regular_file(path, MAX_META_BYTES).decode("utf-8")
        return json.loads(text)
    except UnicodeDecodeError as error:
        raise HardwareOracleError("metadata is not UTF-8 text") from error


def _summary(request, backend, status, exit_code, measured=False, **extra):
    result = {"schema": SCHEMA, "backend": backend, "status": status, "measured": measured, "op": request["op"],
              "mode": request["mode"], "wave_width": request["wave_width"], "count": request["count"],
              "seed": request["seed"], "arch": request["arch"]}
    result.update(extra)
    return result, exit_code


def _host_check(op, inputs, actual):
    """Ancillary: the host arithmetic reference compared with a measured output. Not the oracle."""
    return compare_outputs(host_reference(op, inputs["a"], inputs["b"]), actual)


def _measured_outcome(request, backend, inputs, actual, artifact, outputs_name):
    host = _host_check(request["op"], inputs, actual)
    summary, code = _summary(request, backend, "measured", EXIT_OK, measured=True, artifact=artifact,
                             input_md5=inputs["md5"], outputs=outputs_name, host_reference=host)
    # The request's count is None for --input-file; the operand file defines the pair count.
    summary["count"] = inputs["count"]
    return summary, code


def _load_outputs(path, count):
    """Returns (status, values). status is 'measured' only for exactly 4*count bytes of regular file."""
    try:
        raw = read_output_bytes(path, 4 * count)
    except HardwareOracleError:
        return "output_invalid", None
    if len(raw) != 4 * count:
        return "output_size_mismatch", None
    return "measured", list(struct.unpack(f"<{count}I", raw))


def run_oracle(request):
    """Native AMD backend. Returns (summary, exit code)."""
    capability = probe()
    if capability["status"] != "present":
        return _summary(request, "hip", "unavailable", EXIT_UNAVAILABLE, probe=capability)
    out_dir = request["out_dir"]
    prepare_fresh_dir(out_dir)
    build_dir, raw_dir = out_dir / "build", out_dir / "raw"
    for path in (build_dir, raw_dir):
        path.mkdir(mode=0o700)
    inputs = build_inputs(request)
    (raw_dir / "inputs.bin").write_bytes(inputs["data"])
    source = build_dir / "oracle.hip"
    source.write_text(HIP_SOURCE, encoding="utf-8")
    binary, asm_out = build_dir / "oracle", build_dir / "oracle.s"
    output_path, meta_path = raw_dir / "outputs.bin", raw_dir / "meta.json"
    count, wave, arch = inputs["count"], request["wave_width"], request["arch"]
    try:
        if run_command(build_asm_argv(source, asm_out, wave, arch), COMPILE_TIMEOUT_S, build_dir / "asm.log") != 0:
            return _summary(request, "hip", "compile_failed", EXIT_MISMATCH)
        if asm_out.stat().st_size > MAX_ASM_BYTES:
            return _summary(request, "hip", "asm_check_failed", EXIT_MISMATCH, reason="assembly too large")
        missing = check_device_assembly(asm_out.read_text(encoding="utf-8", errors="replace"))
        if missing:
            return _summary(request, "hip", "asm_check_failed", EXIT_MISMATCH, missing_mnemonics=missing)
        if run_command(build_compile_argv(source, binary, wave, arch), COMPILE_TIMEOUT_S, build_dir / "compile.log") != 0:
            return _summary(request, "hip", "compile_failed", EXIT_MISMATCH)
        version_ok = run_command(build_version_argv(), COMPILE_TIMEOUT_S, build_dir / "version.log") == 0
        try:
            compiler_version = read_compiler_version(build_dir / "version.log") if version_ok else None
        except (OSError, HardwareOracleError):
            compiler_version = None
        if compiler_version is None:
            return _summary(request, "hip", "compiler_version_unreadable", EXIT_MISMATCH)
        if run_command(build_run_argv(binary, request["op"], count, raw_dir / "inputs.bin", output_path, meta_path),
                       RUN_TIMEOUT_S, raw_dir / "run.log") != 0:
            return _summary(request, "hip", "device_error", EXIT_MISMATCH)
    except CommandTimeout:
        return _summary(request, "hip", "timeout", EXIT_MISMATCH)

    try:
        device = validate_device_metadata(_read_bounded_json(meta_path))
    except (OSError, ValueError):
        return _summary(request, "hip", "device_metadata_invalid", EXIT_MISMATCH)
    artifact = {"compiler_version": compiler_version, "supported_arch": SUPPORTED_ARCH, **device}
    if device["warp_size"] != wave:
        return _summary(request, "hip", "wave_width_mismatch", EXIT_MISMATCH, artifact=artifact)
    if device["gcn_arch_name"].split(":")[0] != arch:
        return _summary(request, "hip", "arch_mismatch", EXIT_MISMATCH, artifact=artifact)
    output_status, actual = _load_outputs(output_path, count)
    if output_status != "measured":
        return _summary(request, "hip", output_status, EXIT_MISMATCH, artifact=artifact)
    summary, code = _measured_outcome(request, "hip", inputs, actual, artifact, "raw/outputs.bin")
    write_exclusive(out_dir / "result.json", json.dumps(summary, indent=2, sort_keys=True) + "\n")
    return summary, code


def run_vulkan(request, endpoint):
    """Vulkan endpoint backend. Returns (summary, exit code).

    Admission is checked before any file is read or created: the logical wave must be 64 and
    the pair count must be at most 1024. Missing or non-executable endpoints are unavailable.
    """
    if request["wave_width"] != VULKAN_LOGICAL_WAVE:
        return _summary(request, "vulkan", "unavailable", EXIT_UNAVAILABLE, reason="logical_wave32_unsupported")
    if request["input_file"] is None and request["count"] > VULKAN_MAX_PAIRS:
        raise HardwareOracleError(f"the Vulkan endpoint accepts at most {VULKAN_MAX_PAIRS} pairs per run")
    inputs = build_inputs(request, max_pairs=VULKAN_MAX_PAIRS)
    if inputs["count"] > VULKAN_MAX_PAIRS:
        raise HardwareOracleError(f"the Vulkan endpoint accepts at most {VULKAN_MAX_PAIRS} pairs per run")
    endpoint_path = Path(endpoint)
    if not endpoint_path.exists() or not stat.S_ISREG(endpoint_path.stat().st_mode):
        return _summary(request, "vulkan", "unavailable", EXIT_UNAVAILABLE, reason="endpoint_not_found")
    if not os.access(endpoint_path, os.X_OK):
        return _summary(request, "vulkan", "unavailable", EXIT_UNAVAILABLE, reason="endpoint_not_executable")
    prepare_fresh_dir(request["out_dir"])
    raw_dir = request["out_dir"] / "raw"
    raw_dir.mkdir(mode=0o700)
    (raw_dir / "inputs.bin").write_bytes(inputs["data"])
    output_path, meta_path = raw_dir / "outputs.bin", raw_dir / "endpoint-meta.json"
    argv = build_endpoint_argv(endpoint_path, request["op"], inputs["count"], raw_dir / "inputs.bin", output_path,
                               meta_path, request["wave_width"], MAX_ENDPOINT_DISPATCHES)
    try:
        exit_code = run_command(argv, ENDPOINT_TIMEOUT_S, raw_dir / "endpoint.log")
    except CommandTimeout:
        return _summary(request, "vulkan", "timeout", EXIT_MISMATCH)
    if exit_code == ENDPOINT_UNAVAILABLE_EXIT:
        return _summary(request, "vulkan", "endpoint_unavailable", EXIT_UNAVAILABLE,
                        reason="compute_capability_unavailable_before_dispatch")
    if exit_code == ENDPOINT_REFUSED_EXIT:
        return _summary(request, "vulkan", "endpoint_refused", EXIT_MISMATCH)
    if exit_code != 0:
        return _summary(request, "vulkan", "endpoint_error", EXIT_MISMATCH)
    try:
        meta = validate_endpoint_meta(_read_bounded_json(meta_path), request["op"], inputs["count"])
    except (OSError, ValueError):
        return _summary(request, "vulkan", "endpoint_metadata_invalid", EXIT_MISMATCH)
    if not meta["measured"]:
        return _summary(request, "vulkan", "endpoint_not_measured", EXIT_UNAVAILABLE)
    output_status, actual = _load_outputs(output_path, inputs["count"])
    if output_status != "measured":
        return _summary(request, "vulkan", output_status, EXIT_MISMATCH)
    subgroup_mode = "matched" if meta["subgroup_size"] == request["wave_width"] else "differs_qualified"
    artifact = {"physical_device": meta["physical_device"], "physical_subgroup_size": meta["subgroup_size"],
                "logical_wave_width": request["wave_width"], "subgroup_mode": subgroup_mode,
                "dispatches": meta["dispatches"]}
    if "default_subgroup_size" in meta:
        artifact["default_subgroup_size"] = meta["default_subgroup_size"]
    summary, code = _measured_outcome(request, "vulkan", inputs, actual, artifact, "raw/outputs.bin")
    write_exclusive(request["out_dir"] / "result.json", json.dumps(summary, indent=2, sort_keys=True) + "\n")
    return summary, code


MEASURED_RESULT_KEYS = ("schema", "backend", "status", "measured", "op", "count", "input_md5", "outputs",
                        "artifact", "wave_width", "arch")
FIXED_OUTPUTS_NAME = "raw/outputs.bin"


def _admitted_output_path(directory):
    """Resolves the result directory once and admits only a real raw/ child, so no parent link escapes it.

    The leaf file is still opened with O_NOFOLLOW by the reader. A symbolic link at raw/ would redirect
    the leaf outside the result directory, so it is refused here by lstat before any output is opened.
    """
    try:
        base = Path(directory).resolve(strict=True)
        raw_status = os.lstat(base / "raw")
    except OSError as error:
        raise HardwareOracleError(f"result directory has no readable raw child: {error.strerror or error}") from error
    if not stat.S_ISDIR(raw_status.st_mode):
        raise HardwareOracleError("raw must be a real directory inside the result directory")
    return base / "raw" / FIXED_OUTPUTS_NAME.split("/")[-1]


def _load_result(directory, label):
    """Loads and validates one result directory. Returns (result, None) when measured, or (None, status)."""
    result = _read_bounded_json(Path(directory) / "result.json")
    if not isinstance(result, dict) or result.get("schema") != SCHEMA or result.get("backend") != label:
        raise HardwareOracleError(f"{label} directory does not hold a {label} result of this schema")
    if result.get("measured") is False:
        status = result.get("status")
        if not isinstance(status, str) or not status:
            raise HardwareOracleError(f"{label} result has no status")
        return None, status
    if result.get("measured") is not True or not set(MEASURED_RESULT_KEYS) <= set(result):
        raise HardwareOracleError(f"{label} result is missing measured fields")
    if result["status"] != "measured" or result["op"] not in OPS:
        raise HardwareOracleError(f"{label} result has an unknown status or operation")
    limit = VULKAN_MAX_PAIRS if label == "vulkan" else MAX_COUNT
    count = result["count"]
    if type(count) is not int or not 1 <= count <= limit:
        raise HardwareOracleError(f"{label} result has an invalid pair count")
    if not isinstance(result["input_md5"], str) or INPUT_MD5_PATTERN.fullmatch(result["input_md5"]) is None:
        raise HardwareOracleError(f"{label} result has an invalid input digest")
    if result["outputs"] != FIXED_OUTPUTS_NAME:
        raise HardwareOracleError(f"{label} result names an output location other than the fixed one")
    if type(result["wave_width"]) is not int or result["wave_width"] not in WAVE_WIDTHS:
        raise HardwareOracleError(f"{label} result has an invalid wave width")
    if result["arch"] != SUPPORTED_ARCH or not isinstance(result["artifact"], dict):
        raise HardwareOracleError(f"{label} result has an unsupported architecture or artifact")
    values = read_output_bytes(_admitted_output_path(directory), 4 * count)
    if len(values) != 4 * count:
        raise HardwareOracleError(f"{label} outputs do not match their recorded count")
    result["values"] = list(struct.unpack(f"<{count}I", values))
    return result, None


def compare_results(hip_dir, vulkan_dir):
    """Compares two measured result directories for the same operation and input bytes."""
    hip, hip_status = _load_result(hip_dir, "hip")
    vulkan, vulkan_status = _load_result(vulkan_dir, "vulkan")
    for label, result, status in (("hip", hip, hip_status), ("vulkan", vulkan, vulkan_status)):
        if result is None:
            return {"schema": SCHEMA, "status": "unavailable", "unavailable_side": label,
                    "reported_status": status}, EXIT_UNAVAILABLE
    for key in ("op", "count", "input_md5"):
        if hip[key] != vulkan[key]:
            raise HardwareOracleError(f"results differ in {key}; they were not produced from the same input")
    comparison = compare_outputs(hip["values"], vulkan["values"])
    status = "match" if comparison["status"] == "match" else "mismatch"
    code = EXIT_OK if status == "match" else EXIT_MISMATCH
    return {"schema": SCHEMA, "status": status, "op": hip["op"], "count": hip["count"],
            "input_md5": hip["input_md5"], "comparison": comparison,
            "hip_artifact": hip["artifact"], "vulkan_artifact": vulkan["artifact"]}, code


def main(argv=None):
    parser = argparse.ArgumentParser(description="Integer instruction oracle: native gfx1030 and Vulkan endpoint.")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("probe", help="check HIP device and compiler presence only")
    for name, text in (("run", "run the native gfx1030 HIP oracle"), ("vulkan-run", "run the external Vulkan endpoint")):
        sub = commands.add_parser(name, help=text)
        sub.add_argument("--op", required=True, choices=OPS)
        source = sub.add_mutually_exclusive_group(required=True)
        source.add_argument("--mode", choices=MODES)
        source.add_argument("--input-file", help="absolute external file of uint32 pairs (a..., then b...)")
        sub.add_argument("--wave-width", type=int, required=True, choices=WAVE_WIDTHS)
        sub.add_argument("--count", type=int)
        sub.add_argument("--seed", type=int, default=1)
        sub.add_argument("--arch", default=SUPPORTED_ARCH)
        sub.add_argument("--out-dir", required=True, help="absolute new or empty directory outside the repository")
        if name == "vulkan-run":
            sub.add_argument("--endpoint", required=True, help="absolute path of the Vulkan endpoint executable")
    compare = commands.add_parser("compare", help="compare a native result with a Vulkan result for the same input")
    compare.add_argument("--hip", required=True, type=Path)
    compare.add_argument("--vulkan", required=True, type=Path)
    args = parser.parse_args(argv)

    if args.command == "probe":
        result = probe()
        print(json.dumps(result, indent=2, sort_keys=True))
        return EXIT_OK if result["status"] == "present" else EXIT_UNAVAILABLE
    try:
        if args.command == "compare":
            summary, code = compare_results(args.hip, args.vulkan)
            print(json.dumps(summary, indent=2, sort_keys=True))
            return code
        if args.input_file is None and args.count is None:
            raise HardwareOracleError("generated modes need --count")
        request = validate_request(args.op, args.mode, args.wave_width, args.count, args.seed, args.arch, args.out_dir,
                                   input_file=args.input_file)
        if args.command == "vulkan-run":
            if not Path(args.endpoint).is_absolute():
                raise HardwareOracleError("endpoint must be an absolute path")
            summary, code = run_vulkan(request, args.endpoint)
        else:
            summary, code = run_oracle(request)
    except HardwareOracleError as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_USAGE
    except OSError as error:
        print(f"error: cannot prepare or read output: {error.strerror or error}", file=sys.stderr)
        return EXIT_ERROR
    print(json.dumps(summary, indent=2, sort_keys=True))
    return code


if __name__ == "__main__":
    sys.exit(main())
