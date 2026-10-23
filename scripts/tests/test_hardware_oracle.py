#!/usr/bin/env python3
"""Contract tests for the integer instruction oracle: native gfx1030 HIP, Vulkan endpoint protocol, and compare.

No test here measures hardware or runs the Vulkan endpoint. Device and endpoint
runners are replaced by labeled fakes that only write files, so these tests check
argument construction, the assembly gate, protocol validation, file handling and
comparison rules.
"""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "kyty_hardware_oracle.py"
SPEC = importlib.util.spec_from_file_location("kyty_hardware_oracle", MODULE_PATH)
assert SPEC and SPEC.loader
oracle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(oracle)

MASK32 = 0xFFFFFFFF
AVAILABLE = {"schema": oracle.PROBE_SCHEMA, "status": "present", "checks": {}, "measurement": "not_run"}
UNAVAILABLE = {"schema": oracle.PROBE_SCHEMA, "status": "unavailable", "checks": {}, "measurement": "not_run"}
FAKE_META = {"warp_size": 64, "device_name": "Fake Test Device", "gcn_arch_name": "gfx1030",
             "runtime_version": 60200000, "driver_version": 60200000}
FAKE_ASM = "\n".join(f"\t{m} v0, v1, v2" for m in oracle.ASM_MNEMONICS) + "\n"
FAKE_ENDPOINT_META = {"schema": oracle.ENDPOINT_SCHEMA, "backend": "vulkan", "op": "add", "pairs": 4,
                      "dispatches": 1, "physical_device": "Fake Test Device", "subgroup_size": 64, "measured": True}


def request_for(**overrides):
    values = {"op": "add", "mode": "seeded", "wave_width": 64, "count": 4, "seed": 2, "arch": "gfx1030",
              "out_dir": None, "input_file": None}
    values.update(overrides)
    return values


class HostReferenceTests(unittest.TestCase):
    def test_arithmetic_wraps_at_thirty_two_bits(self):
        self.assertEqual(oracle.host_reference("add", [0xFFFFFFFF], [2]), [1])
        self.assertEqual(oracle.host_reference("mul", [0x80000000], [2]), [0])
        self.assertEqual(oracle.host_reference("mulhi", [0xFFFFFFFF], [0xFFFFFFFF]), [0xFFFFFFFE])

    def test_shift_counts_use_the_low_five_bits_and_the_value_is_x(self):
        self.assertEqual(oracle.host_reference("shl", [1], [33]), [2])
        self.assertEqual(oracle.host_reference("shl", [1], [32]), [1])
        self.assertEqual(oracle.host_reference("shr", [0x80000000], [31]), [1])

    def test_unknown_operations_are_rejected(self):
        with self.assertRaises(oracle.HardwareOracleError):
            oracle.host_reference("div", [1], [1])


class InputTests(unittest.TestCase):
    def test_seeded_inputs_are_reproducible_bounded_and_seed_dependent(self):
        first = oracle.make_inputs("seeded", 1000, 7)
        self.assertEqual(first, oracle.make_inputs("seeded", 1000, 7))
        self.assertNotEqual(first, oracle.make_inputs("seeded", 1000, 8))
        for values in first:
            self.assertTrue(all(0 <= v <= MASK32 for v in values))

    def test_external_pairs_are_read_in_order_with_their_digest(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "pairs.bin"
            data = struct.pack("<4I", 1, 2, 3, 4) + struct.pack("<4I", 10, 20, 30, 40)
            path.write_bytes(data)
            inputs = oracle.build_inputs(request_for(mode=None, count=None, input_file=str(path)))
        self.assertEqual((inputs["count"], inputs["a"], inputs["b"]), (4, [1, 2, 3, 4], [10, 20, 30, 40]))
        self.assertEqual(inputs["md5"], hashlib.md5(data).hexdigest())

    def test_external_input_size_and_kind_are_bounded(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for name, payload in (("empty.bin", b""), ("odd.bin", b"\x00" * 12)):
                (root / name).write_bytes(payload)
                with self.subTest(name=name), self.assertRaises(oracle.HardwareOracleError):
                    oracle.build_inputs(request_for(mode=None, count=None, input_file=str(root / name)))
            (root / "large.bin").write_bytes(b"\x00" * (8 * oracle.MAX_COUNT + 8))
            with self.assertRaises(oracle.HardwareOracleError):
                oracle.build_inputs(request_for(mode=None, count=None, input_file=str(root / "large.bin")))
            with self.assertRaises(oracle.HardwareOracleError):
                oracle.build_inputs(request_for(mode=None, count=None, input_file=str(root)))


class RequestValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.outside = str(self.root / "private-run")

    def tearDown(self):
        self.temp.cleanup()

    def validate(self, **overrides):
        values = {"op": "add", "mode": "seeded", "wave_width": 64, "count": 16, "seed": 1, "arch": "gfx1030",
                  "out_dir": self.outside, "input_file": None, "repo_root": self.repo}
        values.update(overrides)
        return oracle.validate_request(**values)

    def test_accepts_supported_architecture_outside_the_repository(self):
        request = self.validate()
        self.assertEqual(request["out_dir"], Path(self.outside).resolve())

    def test_only_gfx1030_is_accepted_and_the_error_says_so(self):
        for arch in ("gfx90a", "gfx1100", "gfx1030:xnack+"):
            with self.subTest(arch=arch), self.assertRaisesRegex(oracle.HardwareOracleError, "gfx1030 only"):
                self.validate(arch=arch)

    def test_generated_mode_and_external_file_are_mutually_exclusive(self):
        with self.assertRaises(oracle.HardwareOracleError):
            self.validate(input_file=str(self.root / "pairs.bin"))

    def test_rejects_unknown_operation_wave_width_counts_and_seeds(self):
        for overrides in ({"op": "div"}, {"mode": "random"}, {"wave_width": 16}, {"count": 0},
                          {"count": oracle.MAX_COUNT + 1}, {"seed": -1}, {"seed": 1 << 32}):
            with self.subTest(overrides=overrides), self.assertRaises(oracle.HardwareOracleError):
                self.validate(**overrides)

    def test_rejects_relative_and_in_repository_output_directories(self):
        with self.assertRaises(oracle.HardwareOracleError):
            self.validate(out_dir="relative/dir")
        with self.assertRaises(oracle.HardwareOracleError):
            self.validate(out_dir=str(self.repo / "raw"))


class OutputDirectoryTests(unittest.TestCase):
    def test_new_or_empty_directories_are_accepted_and_nonempty_ones_are_refused(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            oracle.prepare_fresh_dir(root / "new")
            self.assertTrue((root / "new").is_dir())
            (root / "used").mkdir()
            oracle.prepare_fresh_dir(root / "used")
            (root / "used" / "result.json").write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(oracle.HardwareOracleError, "new or empty"):
                oracle.prepare_fresh_dir(root / "used")

    def test_a_file_in_the_output_location_is_refused(self):
        with tempfile.TemporaryDirectory() as td:
            blocker = Path(td) / "file"
            blocker.write_text("x", encoding="utf-8")
            with self.assertRaises(oracle.HardwareOracleError):
                oracle.prepare_fresh_dir(blocker)

    def test_result_files_are_created_exclusively(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "result.json"
            oracle.write_exclusive(path, "{}")
            with self.assertRaises(FileExistsError):
                oracle.write_exclusive(path, "{}")


class CapabilityProbeTests(unittest.TestCase):
    def test_missing_kfd_device_reports_unavailable_and_measures_nothing(self):
        with tempfile.TemporaryDirectory() as td:
            with mock.patch.object(oracle.shutil, "which", return_value="/usr/bin/hipcc"):
                result = oracle.probe(kfd_path=Path(td) / "kfd")
        self.assertEqual(result["status"], "unavailable")
        self.assertEqual(result["measurement"], "not_run")

    def test_present_capability_is_still_only_a_presence_check(self):
        with tempfile.TemporaryDirectory() as td:
            kfd = Path(td) / "kfd"
            kfd.write_bytes(b"")
            with mock.patch.object(oracle.shutil, "which", return_value="/usr/bin/hipcc"):
                result = oracle.probe(kfd_path=kfd)
        self.assertEqual(result["status"], "present")
        self.assertEqual(result["measurement"], "not_run")

    def test_native_run_without_hardware_is_unavailable_with_no_measurement_fields(self):
        with tempfile.TemporaryDirectory() as td:
            out_dir = Path(td) / "private-run"
            with mock.patch.object(oracle, "probe", return_value=UNAVAILABLE), \
                 mock.patch.object(oracle, "run_command", side_effect=AssertionError("no command may run")):
                with contextlib.redirect_stdout(io.StringIO()) as printed:
                    code = oracle.main(["run", "--op", "add", "--mode", "edges", "--wave-width", "64",
                                        "--count", "64", "--out-dir", str(out_dir)])
            self.assertFalse(out_dir.exists())
            summary = json.loads(printed.getvalue())
        self.assertEqual(code, oracle.EXIT_UNAVAILABLE)
        self.assertEqual((summary["backend"], summary["status"], summary["measured"]), ("hip", "unavailable", False))
        for key in ("comparison", "artifact", "host_reference", "outputs"):
            self.assertNotIn(key, summary)


class CommandShapeTests(unittest.TestCase):
    def test_assembly_compile_names_the_architecture_and_requests_device_assembly(self):
        argv = oracle.build_asm_argv(Path("/a/oracle.hip"), Path("/a/oracle.s"), 64, "gfx1030")
        self.assertEqual(argv[0], "hipcc")
        self.assertIn("--offload-arch=gfx1030", argv)
        self.assertIn("-S", argv)
        self.assertIn("-mwavefrontsize64", argv)
        self.assertNotIn("-mwavefrontsize64", oracle.build_asm_argv(Path("/a.hip"), Path("/a.s"), 32, "gfx1030"))

    def test_binary_version_and_run_argv_are_fixed_lists(self):
        self.assertIn("--offload-arch=gfx1030", oracle.build_compile_argv(Path("/a.hip"), Path("/a"), 64, "gfx1030"))
        self.assertEqual(oracle.build_version_argv(), ["hipcc", "--version"])
        argv = oracle.build_run_argv(Path("/a/oracle"), "mulhi", 4096, Path("/r/in.bin"), Path("/r/out.bin"),
                                     Path("/r/meta.json"))
        self.assertEqual(argv[1:], ["2", "4096", "/r/in.bin", "/r/out.bin", "/r/meta.json"])
        for argv in (oracle.build_asm_argv(Path("/a.hip"), Path("/a.s"), 64, "gfx1030"), argv):
            self.assertTrue(all(isinstance(part, str) for part in argv))

    def test_endpoint_argv_passes_fixed_flags_and_the_dispatch_cap(self):
        argv = oracle.build_endpoint_argv(Path("/e/endpoint"), "shl", 8, Path("/r/in.bin"), Path("/r/out.bin"),
                                          Path("/r/meta.json"), 64, 16)
        self.assertEqual(argv[:3], ["/e/endpoint", "--op", "shl"])
        self.assertEqual(argv[argv.index("--max-dispatches") + 1], "16")

    def test_commands_run_without_a_shell_with_a_timeout_and_timeouts_are_typed(self):
        completed = subprocess.CompletedProcess(args=["x"], returncode=0)
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "log.txt"
            with mock.patch.object(oracle.subprocess, "run", return_value=completed) as run:
                self.assertEqual(oracle.run_command(["x", "--a"], 30, log), 0)
            self.assertIs(run.call_args.kwargs["shell"], False)
            self.assertEqual(run.call_args.kwargs["timeout"], 30)
            with mock.patch.object(oracle.subprocess, "run", side_effect=subprocess.TimeoutExpired("x", 1)):
                with self.assertRaises(oracle.CommandTimeout):
                    oracle.run_command(["x"], 1, log)


class AssemblyGateTests(unittest.TestCase):
    def test_all_five_instructions_present_passes(self):
        self.assertEqual(oracle.check_device_assembly(FAKE_ASM), [])

    def test_missing_instruction_is_reported_by_mnemonic(self):
        text = FAKE_ASM.replace("v_mul_hi_u32", "v_mul_u32_u24")
        self.assertEqual(oracle.check_device_assembly(text), ["v_mul_hi_u32"])

    def test_a_mnemonic_in_a_comment_does_not_satisfy_the_gate(self):
        text = FAKE_ASM.replace("\tv_lshrrev_b32 v0, v1, v2", "; v_lshrrev_b32 v0, v1, v2")
        self.assertEqual(oracle.check_device_assembly(text), ["v_lshrrev_b32"])


class DeviceMetadataTests(unittest.TestCase):
    def test_valid_metadata_is_returned_as_artifact_fields(self):
        self.assertEqual(oracle.validate_device_metadata(dict(FAKE_META)), FAKE_META)

    def test_wrong_shapes_and_values_are_rejected(self):
        with self.assertRaises(oracle.HardwareOracleError):
            oracle.validate_device_metadata({k: v for k, v in FAKE_META.items() if k != "driver_version"})
        for key, value in (("warp_size", True), ("warp_size", 16), ("runtime_version", -1),
                           ("device_name", "Name \"quoted\""), ("gcn_arch_name", "sm_80")):
            with self.subTest(key=key, value=value), self.assertRaises(oracle.HardwareOracleError):
                oracle.validate_device_metadata(dict(FAKE_META, **{key: value}))


class EndpointProtocolTests(unittest.TestCase):
    def test_valid_endpoint_report_is_accepted(self):
        meta = oracle.validate_endpoint_meta(dict(FAKE_ENDPOINT_META), "add", 4)
        self.assertTrue(meta["measured"])

    def test_reports_for_another_backend_operation_or_input_are_rejected(self):
        cases = (
            dict(FAKE_ENDPOINT_META, backend="hip"),
            dict(FAKE_ENDPOINT_META, op="mul"),
            dict(FAKE_ENDPOINT_META, pairs=8),
            dict(FAKE_ENDPOINT_META, dispatches=0),
            dict(FAKE_ENDPOINT_META, dispatches=oracle.MAX_ENDPOINT_DISPATCHES + 1),
            dict(FAKE_ENDPOINT_META, subgroup_size=3),
            dict(FAKE_ENDPOINT_META, measured="yes"),
            dict(FAKE_ENDPOINT_META, note="extra"),
        )
        for meta in cases:
            with self.subTest(meta=meta), self.assertRaises(oracle.HardwareOracleError):
                oracle.validate_endpoint_meta(meta, "add", 4)


class EndpointDiscoveryTests(unittest.TestCase):
    def test_missing_endpoint_is_unavailable_and_nothing_runs(self):
        with tempfile.TemporaryDirectory() as td:
            request = request_for(out_dir=Path(td) / "run")
            with mock.patch.object(oracle, "run_command", side_effect=AssertionError("no command may run")):
                summary, code = oracle.run_vulkan(request, str(Path(td) / "absent-endpoint"))
            self.assertFalse((Path(td) / "run").exists())
        self.assertEqual((summary["backend"], summary["status"], summary["reason"]),
                         ("vulkan", "unavailable", "endpoint_not_found"))
        self.assertEqual(code, oracle.EXIT_UNAVAILABLE)

    def test_non_executable_endpoint_file_is_unavailable(self):
        with tempfile.TemporaryDirectory() as td:
            endpoint = Path(td) / "endpoint"
            endpoint.write_bytes(b"")
            os.chmod(endpoint, 0o644)
            summary, code = oracle.run_vulkan(request_for(out_dir=Path(td) / "run"), str(endpoint))
        self.assertEqual(summary["reason"], "endpoint_not_executable")
        self.assertEqual(code, oracle.EXIT_UNAVAILABLE)


def fake_hip_runner(meta=None, asm=FAKE_ASM, binary_fails=False):
    meta = FAKE_META if meta is None else meta

    def runner(argv, timeout, log_path):
        if "--version" in argv:
            Path(log_path).write_text("HIP version: fake-test-version\n", encoding="utf-8")
            return 0
        if "-S" in argv:
            Path(argv[argv.index("-o") + 1]).write_text(asm, encoding="utf-8")
            return 0
        if argv[0] == "hipcc":
            if binary_fails:
                return 1
            Path(argv[argv.index("-o") + 1]).write_bytes(b"fake-binary")
            return 0
        input_path, output_path, meta_path = (Path(p) for p in argv[3:6])
        raw = input_path.read_bytes()
        count = len(raw) // 8
        a, b = struct.unpack(f"<{2 * count}I", raw)[:count], struct.unpack(f"<{2 * count}I", raw)[count:]
        output_path.write_bytes(struct.pack(f"<{count}I", *oracle.host_reference("add", list(a), list(b))))
        meta_path.write_text(json.dumps(meta), encoding="utf-8")
        return 0

    return runner


def fake_endpoint_runner(meta, outputs=None):
    def runner(argv, timeout, log_path):
        count = int(argv[argv.index("--count") + 1])
        op = argv[argv.index("--op") + 1]
        out = Path(argv[argv.index("--output") + 1])
        meta_path = Path(argv[argv.index("--meta") + 1])
        values = outputs if outputs is not None else [0] * count
        out.write_bytes(struct.pack(f"<{len(values)}I", *values))
        meta_path.write_text(json.dumps(dict(meta, op=op, pairs=count)), encoding="utf-8")
        return 0

    return runner


class HipGlueTests(unittest.TestCase):
    def run_hip(self, runner, tmpdir, **request):
        values = request_for(out_dir=Path(tmpdir) / "run")
        values.update(request)
        with mock.patch.object(oracle, "probe", return_value=AVAILABLE), \
             mock.patch.object(oracle, "run_command", side_effect=runner):
            return oracle.run_oracle(values)

    def test_measured_native_run_records_artifact_and_ancillary_host_reference(self):
        with tempfile.TemporaryDirectory() as td:
            summary, code = self.run_hip(fake_hip_runner(), td)
        self.assertEqual((summary["status"], summary["measured"], code), ("measured", True, oracle.EXIT_OK))
        self.assertEqual(summary["artifact"]["compiler_version"], "HIP version: fake-test-version")
        self.assertEqual(summary["artifact"]["gcn_arch_name"], "gfx1030")
        self.assertEqual(summary["host_reference"]["status"], "match")

    def test_missing_instruction_in_device_assembly_blocks_the_measurement(self):
        with tempfile.TemporaryDirectory() as td:
            summary, code = self.run_hip(fake_hip_runner(asm="\tv_add_nc_u32 v0, v1, v2\n"), td)
        self.assertEqual((summary["status"], code), ("asm_check_failed", oracle.EXIT_MISMATCH))
        self.assertIn("v_mul_lo_u32", summary["missing_mnemonics"])
        self.assertFalse(summary["measured"])

    def test_device_on_another_architecture_is_reported_not_measured(self):
        with tempfile.TemporaryDirectory() as td:
            summary, code = self.run_hip(fake_hip_runner(meta=dict(FAKE_META, gcn_arch_name="gfx90a")), td)
        self.assertEqual((summary["status"], code), ("arch_mismatch", oracle.EXIT_MISMATCH))
        self.assertFalse(summary["measured"])
        self.assertNotIn("host_reference", summary)

    def test_wave_width_disagreement_is_reported_before_any_output_is_trusted(self):
        with tempfile.TemporaryDirectory() as td:
            summary, _ = self.run_hip(fake_hip_runner(meta=dict(FAKE_META, warp_size=32)), td, wave_width=64)
        self.assertEqual(summary["status"], "wave_width_mismatch")
        self.assertFalse(summary["measured"])

    def test_binary_build_failure_is_compile_failed(self):
        with tempfile.TemporaryDirectory() as td:
            summary, code = self.run_hip(fake_hip_runner(binary_fails=True), td)
        self.assertEqual((summary["status"], code), ("compile_failed", oracle.EXIT_MISMATCH))


class VulkanGlueTests(unittest.TestCase):
    """Protocol tests with a labeled fake endpoint. They do not exercise the Vulkan endpoint itself."""

    def run_vulkan(self, runner, tmpdir, wave_width=64, outputs=None):
        endpoint = Path(tmpdir) / "endpoint"
        endpoint.write_bytes(b"")
        os.chmod(endpoint, stat.S_IRWXU)
        request = request_for(out_dir=Path(tmpdir) / "run", wave_width=wave_width)
        with mock.patch.object(oracle, "run_command", side_effect=runner):
            return oracle.run_vulkan(request, str(endpoint))

    def test_protocol_report_yields_a_measured_vulkan_result_and_qualifies_subgroup_mode(self):
        with tempfile.TemporaryDirectory() as td:
            meta = dict(FAKE_ENDPOINT_META, subgroup_size=32)
            summary, code = self.run_vulkan(fake_endpoint_runner(meta, outputs=[0, 0, 0, 0]), td)
        self.assertEqual((summary["status"], summary["measured"], code), ("measured", True, oracle.EXIT_OK))
        self.assertEqual(summary["artifact"]["logical_wave_width"], 64)
        self.assertEqual(summary["artifact"]["subgroup_mode"], "differs_qualified")

    def test_endpoint_that_reports_not_measured_is_unavailable_not_success(self):
        with tempfile.TemporaryDirectory() as td:
            summary, code = self.run_vulkan(fake_endpoint_runner(dict(FAKE_ENDPOINT_META, measured=False)), td)
        self.assertEqual((summary["status"], code), ("endpoint_not_measured", oracle.EXIT_UNAVAILABLE))
        self.assertFalse(summary["measured"])

    def test_output_of_the_wrong_length_is_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            runner = fake_endpoint_runner(FAKE_ENDPOINT_META, outputs=[0, 0])
            summary, code = self.run_vulkan(runner, td)
        self.assertEqual((summary["status"], code), ("output_size_mismatch", oracle.EXIT_MISMATCH))


def write_result(directory, backend, values, *, measured=True, op="add", input_md5="a" * 32):
    directory.mkdir(parents=True)
    (directory / "raw").mkdir()
    (directory / "raw" / "outputs.bin").write_bytes(struct.pack(f"<{len(values)}I", *values))
    result = {"schema": oracle.SCHEMA, "backend": backend, "status": "measured" if measured else "endpoint_error",
              "measured": measured, "op": op, "count": len(values), "input_md5": input_md5,
              "outputs": "raw/outputs.bin", "artifact": {"backend": backend}, "wave_width": 64, "arch": "gfx1030"}
    (directory / "result.json").write_text(json.dumps(result), encoding="utf-8")


class CompareTests(unittest.TestCase):
    def test_equal_measured_outputs_match(self):
        with tempfile.TemporaryDirectory() as td:
            hip, vulkan = Path(td) / "hip", Path(td) / "vulkan"
            write_result(hip, "hip", [1, 2, 3])
            write_result(vulkan, "vulkan", [1, 2, 3])
            summary, code = oracle.compare_results(hip, vulkan)
        self.assertEqual((summary["status"], code), ("match", oracle.EXIT_OK))

    def test_first_disagreeing_lane_is_reported(self):
        with tempfile.TemporaryDirectory() as td:
            hip, vulkan = Path(td) / "hip", Path(td) / "vulkan"
            write_result(hip, "hip", [1, 2, 3])
            write_result(vulkan, "vulkan", [1, 9, 3])
            summary, code = oracle.compare_results(hip, vulkan)
        self.assertEqual((summary["status"], code, summary["comparison"]["first_index"]), ("mismatch", oracle.EXIT_MISMATCH, 1))

    def test_an_unmeasured_side_makes_the_comparison_unavailable_and_names_the_side(self):
        with tempfile.TemporaryDirectory() as td:
            hip, vulkan = Path(td) / "hip", Path(td) / "vulkan"
            write_result(hip, "hip", [1, 2], measured=False)
            write_result(vulkan, "vulkan", [1, 2])
            summary, code = oracle.compare_results(hip, vulkan)
        self.assertEqual((summary["status"], summary["unavailable_side"], code), ("unavailable", "hip", oracle.EXIT_UNAVAILABLE))

    def test_results_from_different_inputs_or_operations_are_never_compared(self):
        with tempfile.TemporaryDirectory() as td:
            hip, vulkan = Path(td) / "hip", Path(td) / "vulkan"
            write_result(hip, "hip", [1, 2], input_md5="a" * 32)
            write_result(vulkan, "vulkan", [1, 2], input_md5="b" * 32)
            with self.assertRaisesRegex(oracle.HardwareOracleError, "same input"):
                oracle.compare_results(hip, vulkan)
            write_result(Path(td) / "other", "vulkan", [1, 2], op="mul", input_md5="a" * 32)
            with self.assertRaisesRegex(oracle.HardwareOracleError, "op"):
                oracle.compare_results(hip, Path(td) / "other")

    def test_a_result_from_the_wrong_backend_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as td:
            hip, wrong = Path(td) / "hip", Path(td) / "wrong"
            write_result(hip, "hip", [1])
            write_result(wrong, "hip", [1])
            with self.assertRaises(oracle.HardwareOracleError):
                oracle.compare_results(hip, wrong)


def exit_code_runner(code):
    """Labeled fake endpoint: reports the given exit status and writes no output or metadata."""
    def runner(argv, timeout, log_path):
        return code

    return runner


class VulkanEndpointStatusTests(unittest.TestCase):
    """Exit-status mapping of the endpoint. Each case runs through run_vulkan with a fake exit status."""

    def run_with_exit(self, code, tmpdir, **overrides):
        endpoint = Path(tmpdir) / "endpoint"
        endpoint.write_bytes(b"")
        os.chmod(endpoint, stat.S_IRWXU)
        request = request_for(out_dir=Path(tmpdir) / "run", **overrides)
        with mock.patch.object(oracle, "run_command", side_effect=exit_code_runner(code)):
            return oracle.run_vulkan(request, str(endpoint))

    def test_exit_77_is_pre_dispatch_unavailability_not_an_endpoint_error(self):
        with tempfile.TemporaryDirectory() as td:
            summary, code = self.run_with_exit(77, td)
            self.assertFalse((Path(td) / "run" / "raw" / "outputs.bin").exists())
        self.assertEqual((summary["status"], code), ("endpoint_unavailable", oracle.EXIT_UNAVAILABLE))
        self.assertFalse(summary["measured"])
        self.assertNotIn("artifact", summary)

    def test_exit_2_is_a_refusal_and_other_failures_are_endpoint_errors(self):
        with tempfile.TemporaryDirectory() as td:
            refused, refused_code = self.run_with_exit(2, td)
        with tempfile.TemporaryDirectory() as td:
            failed, failed_code = self.run_with_exit(1, td)
        self.assertEqual((refused["status"], refused_code), ("endpoint_refused", oracle.EXIT_MISMATCH))
        self.assertEqual((failed["status"], failed_code), ("endpoint_error", oracle.EXIT_MISMATCH))
        self.assertFalse(refused["measured"] or failed["measured"])

    def test_logical_wave32_is_unavailable_and_no_endpoint_is_launched(self):
        with tempfile.TemporaryDirectory() as td:
            endpoint = Path(td) / "endpoint"
            endpoint.write_bytes(b"")
            os.chmod(endpoint, stat.S_IRWXU)
            request = request_for(out_dir=Path(td) / "run", wave_width=32)
            with mock.patch.object(oracle, "run_command", side_effect=AssertionError("no launch")):
                summary, code = oracle.run_vulkan(request, str(endpoint))
            self.assertFalse((Path(td) / "run").exists())
        self.assertEqual((summary["status"], summary["reason"], code),
                         ("unavailable", "logical_wave32_unsupported", oracle.EXIT_UNAVAILABLE))


class VulkanAdmissionTests(unittest.TestCase):
    def test_pair_cap_is_1024_and_is_refused_before_any_launch_or_file_creation(self):
        with tempfile.TemporaryDirectory() as td:
            endpoint = Path(td) / "endpoint"
            endpoint.write_bytes(b"")
            os.chmod(endpoint, stat.S_IRWXU)
            request = request_for(out_dir=Path(td) / "run", count=oracle.VULKAN_MAX_PAIRS + 1)
            with mock.patch.object(oracle, "run_command", side_effect=AssertionError("no launch")), \
                 self.assertRaisesRegex(oracle.HardwareOracleError, "at most 1024"):
                oracle.run_vulkan(request, str(endpoint))
            self.assertFalse((Path(td) / "run").exists())

    def test_oversized_external_input_is_refused_by_size_before_it_is_read(self):
        with tempfile.TemporaryDirectory() as td:
            large = Path(td) / "large.bin"
            large.write_bytes(b"\x00" * (8 * oracle.VULKAN_MAX_PAIRS + 8))
            request = request_for(mode=None, count=None, input_file=str(large), out_dir=Path(td) / "run")
            with mock.patch.object(oracle, "run_command", side_effect=AssertionError("no launch")), \
                 self.assertRaisesRegex(oracle.HardwareOracleError, "exceeds"):
                oracle.run_vulkan(request, str(large))
            self.assertFalse((Path(td) / "run").exists())

    def test_sixty_four_dispatches_are_the_maximum_and_consistency_is_required(self):
        expected = {"pairs": 1024, "dispatches": 64}
        self.assertEqual(oracle.validate_endpoint_meta(dict(FAKE_ENDPOINT_META, pairs=1024, dispatches=64), "add", 1024)["dispatches"],
                         expected["dispatches"])
        for pairs, dispatches in ((1024, 63), (17, 1), (16, 2)):
            with self.subTest(pairs=pairs, dispatches=dispatches), self.assertRaises(oracle.HardwareOracleError):
                oracle.validate_endpoint_meta(dict(FAKE_ENDPOINT_META, pairs=pairs, dispatches=dispatches), "add", pairs)


class VulkanOptionalFieldTests(unittest.TestCase):
    def test_default_subgroup_size_is_optional_and_must_be_a_power_of_two(self):
        meta = oracle.validate_endpoint_meta(dict(FAKE_ENDPOINT_META, default_subgroup_size=64), "add", 4)
        self.assertEqual(meta["default_subgroup_size"], 64)
        self.assertNotIn("default_subgroup_size", oracle.validate_endpoint_meta(dict(FAKE_ENDPOINT_META), "add", 4))
        with self.assertRaises(oracle.HardwareOracleError):
            oracle.validate_endpoint_meta(dict(FAKE_ENDPOINT_META, default_subgroup_size=24), "add", 4)

    def test_measured_result_records_the_default_subgroup_separately_from_the_paired_subgroup(self):
        with tempfile.TemporaryDirectory() as td:
            endpoint = Path(td) / "endpoint"
            endpoint.write_bytes(b"")
            os.chmod(endpoint, stat.S_IRWXU)
            meta = dict(FAKE_ENDPOINT_META, subgroup_size=32, default_subgroup_size=64)
            with mock.patch.object(oracle, "run_command", side_effect=fake_endpoint_runner(meta, outputs=[0, 0, 0, 0])):
                summary, code = oracle.run_vulkan(request_for(out_dir=Path(td) / "run"), str(endpoint))
        self.assertEqual((summary["status"], code), ("measured", oracle.EXIT_OK))
        self.assertEqual(summary["artifact"]["physical_subgroup_size"], 32)
        self.assertEqual(summary["artifact"]["default_subgroup_size"], 64)


def valid_result_pair(root, values=(1, 2, 3)):
    hip, vulkan = root / "hip", root / "vulkan"
    write_result(hip, "hip", list(values))
    write_result(vulkan, "vulkan", list(values))
    return hip, vulkan


def edit_result(directory, **changes):
    path = directory / "result.json"
    result = json.loads(path.read_text(encoding="utf-8"))
    for key, value in changes.items():
        if value is oracle_missing:
            result.pop(key, None)
        else:
            result[key] = value
    path.write_text(json.dumps(result), encoding="utf-8")


oracle_missing = object()


class MalformedResultTests(unittest.TestCase):
    """Result directories are untrusted input. Each malformed field must fail with a typed error."""

    def assert_refused(self, **changes):
        with tempfile.TemporaryDirectory() as td:
            hip, vulkan = valid_result_pair(Path(td))
            edit_result(vulkan, **changes)
            with self.assertRaises(oracle.HardwareOracleError):
                oracle.compare_results(hip, vulkan)

    def test_control_pair_compares_and_matches(self):
        with tempfile.TemporaryDirectory() as td:
            hip, vulkan = valid_result_pair(Path(td))
            summary, code = oracle.compare_results(hip, vulkan)
        self.assertEqual((summary["status"], code), ("match", oracle.EXIT_OK))

    def test_output_field_cannot_name_another_location(self):
        for outputs in ("../outputs.bin", "/elsewhere/outputs.bin", "raw/../outputs.bin", "raw/other.bin", 7):
            with self.subTest(outputs=outputs):
                self.assert_refused(outputs=outputs)

    def test_missing_required_fields_are_typed_errors_not_key_errors(self):
        for key in ("count", "op", "input_md5", "wave_width", "arch", "artifact", "outputs"):
            with self.subTest(key=key):
                self.assert_refused(**{key: oracle_missing})

    def test_wrong_types_and_ranges_are_refused(self):
        cases = (
            {"count": "4"}, {"count": 0}, {"count": True}, {"count": 2000},
            {"op": "div"}, {"op": ["add"]}, {"input_md5": "A" * 32}, {"input_md5": "a" * 31},
            {"wave_width": 16}, {"wave_width": "64"}, {"arch": "gfx90a"}, {"artifact": []},
            {"status": "endpoint_error"}, {"schema": "other_v1"},
        )
        for changes in cases:
            with self.subTest(changes=changes):
                self.assert_refused(**changes)

    def test_measured_must_be_the_boolean_true(self):
        self.assert_refused(measured="true")
        self.assert_refused(measured=1)

    def test_a_measured_string_flag_is_not_an_unavailable_report(self):
        with tempfile.TemporaryDirectory() as td:
            hip, vulkan = valid_result_pair(Path(td))
            edit_result(vulkan, measured=False, status="endpoint_error")
            summary, code = oracle.compare_results(hip, vulkan)
        self.assertEqual((summary["status"], summary["unavailable_side"], code),
                         ("unavailable", "vulkan", oracle.EXIT_UNAVAILABLE))


class RawDirectoryContainmentTests(unittest.TestCase):
    @unittest.skipUnless(hasattr(os, "symlink"), "symbolic links are POSIX-specific here")
    def test_raw_directory_symlink_outside_is_refused_before_the_output_is_read(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            hip, vulkan = valid_result_pair(root)
            outside = root / "outside"
            outside.mkdir()
            (outside / "outputs.bin").write_bytes(struct.pack("<3I", 1, 2, 3))
            (vulkan / "raw" / "outputs.bin").unlink()
            (vulkan / "raw").rmdir()
            os.symlink(outside, vulkan / "raw")
            real_read = oracle.read_output_bytes

            def guarded_read(path, limit):
                if "vulkan" in str(path):
                    raise AssertionError("the contained Vulkan output must not be read")
                return real_read(path, limit)

            with mock.patch.object(oracle, "read_output_bytes", side_effect=guarded_read), \
                 self.assertRaisesRegex(oracle.HardwareOracleError, "raw must be a real directory"):
                oracle.compare_results(hip, vulkan)


class OutputFileBoundTests(unittest.TestCase):
    def test_output_longer_or_shorter_than_the_count_is_refused(self):
        for values in ((1, 2, 3, 4), (1, 2)):
            with self.subTest(length=len(values)), tempfile.TemporaryDirectory() as td:
                hip, vulkan = valid_result_pair(Path(td))
                (vulkan / "raw" / "outputs.bin").write_bytes(struct.pack(f"<{len(values)}I", *values))
                with self.assertRaisesRegex(oracle.HardwareOracleError, "do not match"):
                    oracle.compare_results(hip, vulkan)

    def test_an_output_far_larger_than_the_bound_is_refused_without_reading_it_all(self):
        with tempfile.TemporaryDirectory() as td:
            hip, vulkan = valid_result_pair(Path(td))
            (vulkan / "raw" / "outputs.bin").write_bytes(b"\x00" * (4 * 3 + 1_000_000))
            with self.assertRaisesRegex(oracle.HardwareOracleError, "exceeds|do not match"):
                oracle.compare_results(hip, vulkan)

    @unittest.skipUnless(hasattr(os, "symlink"), "symbolic links are platform-specific")
    def test_output_symbolic_link_is_refused(self):
        with tempfile.TemporaryDirectory() as td:
            hip, vulkan = valid_result_pair(Path(td))
            target = Path(td) / "elsewhere.bin"
            target.write_bytes(struct.pack("<3I", 1, 2, 3))
            (vulkan / "raw" / "outputs.bin").unlink()
            os.symlink(target, vulkan / "raw" / "outputs.bin")
            with self.assertRaises(oracle.HardwareOracleError):
                oracle.compare_results(hip, vulkan)

    @unittest.skipUnless(hasattr(os, "mkfifo"), "FIFOs are POSIX-specific")
    def test_output_fifo_is_refused_promptly_and_never_opened_for_blocking_read(self):
        with tempfile.TemporaryDirectory() as td:
            hip, vulkan = valid_result_pair(Path(td))
            (vulkan / "raw" / "outputs.bin").unlink()
            os.mkfifo(vulkan / "raw" / "outputs.bin")
            with self.assertRaises(oracle.HardwareOracleError):
                oracle.compare_results(hip, vulkan)


class EndpointOutputValidationTests(unittest.TestCase):
    def run_endpoint_writing(self, tmpdir, writer, count=4):
        endpoint = Path(tmpdir) / "endpoint"
        endpoint.write_bytes(b"")
        os.chmod(endpoint, stat.S_IRWXU)
        meta = dict(FAKE_ENDPOINT_META)

        def runner(argv, timeout, log_path):
            out = Path(argv[argv.index("--output") + 1])
            meta_path = Path(argv[argv.index("--meta") + 1])
            writer(out)
            meta_path.write_text(json.dumps(dict(meta, op="add", pairs=count)), encoding="utf-8")
            return 0

        request = request_for(out_dir=Path(tmpdir) / "run", count=count)
        with mock.patch.object(oracle, "run_command", side_effect=runner):
            return oracle.run_vulkan(request, str(endpoint))

    def test_oversized_endpoint_output_is_a_size_mismatch(self):
        with tempfile.TemporaryDirectory() as td:
            summary, code = self.run_endpoint_writing(td, lambda out: out.write_bytes(b"\x00" * 100_000))
        self.assertEqual((summary["status"], code), ("output_size_mismatch", oracle.EXIT_MISMATCH))
        self.assertFalse(summary["measured"])

    @unittest.skipUnless(hasattr(os, "mkfifo"), "FIFOs are POSIX-specific")
    def test_endpoint_output_that_is_a_fifo_is_invalid_and_does_not_block(self):
        with tempfile.TemporaryDirectory() as td:
            summary, code = self.run_endpoint_writing(td, lambda out: os.mkfifo(out))
        self.assertEqual((summary["status"], code), ("output_invalid", oracle.EXIT_MISMATCH))
        self.assertFalse(summary["measured"])

    def test_external_input_result_records_the_pair_count_from_the_file(self):
        with tempfile.TemporaryDirectory() as td:
            pairs = Path(td) / "pairs.bin"
            pairs.write_bytes(struct.pack("<4I", 1, 2, 3, 4) + struct.pack("<4I", 5, 6, 7, 8))
            endpoint = Path(td) / "endpoint"
            endpoint.write_bytes(b"")
            os.chmod(endpoint, stat.S_IRWXU)

            def runner(argv, timeout, log_path):
                Path(argv[argv.index("--output") + 1]).write_bytes(struct.pack("<4I", 0, 0, 0, 0))
                Path(argv[argv.index("--meta") + 1]).write_text(
                    json.dumps(dict(FAKE_ENDPOINT_META, op="add", pairs=4)), encoding="utf-8")
                return 0

            request = request_for(mode=None, count=None, input_file=str(pairs), out_dir=Path(td) / "run")
            with mock.patch.object(oracle, "run_command", side_effect=runner):
                summary, _ = oracle.run_vulkan(request, str(endpoint))
        self.assertEqual((summary["status"], summary["count"]), ("measured", 4))


class CountAndInputAdmissionTests(unittest.TestCase):
    def test_count_with_input_file_is_refused_at_request_admission(self):
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaisesRegex(oracle.HardwareOracleError, "cannot be combined"):
                oracle.validate_request("add", None, 64, 4, 1, "gfx1030", str(Path(td) / "run"),
                                        input_file=str(Path(td) / "pairs.bin"), repo_root=Path(td))

    def test_count_with_input_file_is_refused_by_the_command_line(self):
        with tempfile.TemporaryDirectory() as td:
            pairs = Path(td) / "pairs.bin"
            pairs.write_bytes(struct.pack("<2I", 1, 2) + struct.pack("<2I", 3, 4))
            with contextlib.redirect_stderr(io.StringIO()):
                code = oracle.main(["run", "--op", "add", "--input-file", str(pairs), "--count", "2",
                                    "--wave-width", "64", "--out-dir", str(Path(td) / "run")])
        self.assertEqual(code, oracle.EXIT_USAGE)


if __name__ == "__main__":
    unittest.main()
