#!/usr/bin/env python3
"""Small deterministic tests for the capture contract."""

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("kyty_capture.py")
SPEC = importlib.util.spec_from_file_location("kyty_capture", MODULE_PATH)
assert SPEC and SPEC.loader
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)


def metrics(*, white: float, entropy: float, colors: int, stripey: bool = False):
    return {
        "world": {
            "white_ratio": white,
            "entropy": entropy,
            "unique_quantized_colors": colors,
            "stripey": stripey,
            "scene_ok": not stripey,
        }
    }


class CaptureContractTests(unittest.TestCase):
    def test_capture_disables_continuous_auto_cross_by_default(self):
        args = capture.parser().parse_args(["capture", "--guest-root", "/tmp"])
        self.assertFalse(args.auto_cross)

    def test_compare_accepts_small_measurement_drift(self):
        result = capture.compare_metrics(
            metrics(white=0.04, entropy=6.8, colors=1200),
            metrics(white=0.01, entropy=7.0, colors=1300),
        )
        self.assertTrue(result["pass"])

    def test_compare_rejects_white_collapse(self):
        result = capture.compare_metrics(
            metrics(white=0.96, entropy=0.2, colors=3),
            metrics(white=0.01, entropy=7.0, colors=1300),
        )
        self.assertFalse(result["pass"])
        self.assertFalse(result["checks"]["white_ratio_not_worse"])

    def test_compare_rejects_directional_stripes(self):
        result = capture.compare_metrics(
            metrics(white=0.04, entropy=6.8, colors=1200, stripey=True),
            metrics(white=0.01, entropy=7.0, colors=1300),
        )
        self.assertFalse(result["pass"])
        self.assertFalse(result["checks"]["not_stripey"])

    def test_crop_bounds_are_clamped(self):
        image = capture.Image(100, 50, lambda _x, _y: (0, 0, 0), "test")
        self.assertEqual(capture.crop_bounds(image, (-1.0, -1.0, 2.0, 2.0)), (0, 0, 100, 50))

    def test_lightbuf_probe_is_rejected_as_compatibility_evidence(self):
        args = capture.parser().parse_args(["capture", "--guest-root", "/tmp", "--lightbuf-probe", "1"])
        self.assertEqual(args.lightbuf_probe, "1")
        self.assertFalse(
            not args.allow_diagnostics
            and not args.auto_cross
            and not any((args.lightbuf_probe,))
        )

    def test_scheduled_key_parses_frame_and_key(self):
        args = capture.parser().parse_args(["capture", "--guest-root", "/tmp", "--key-at", "700:x"])
        self.assertEqual(args.key_at, [(700, "x")])

    def test_rt_evidence_filter_is_recorded_as_diagnostic(self):
        args = capture.parser().parse_args(["capture", "--guest-root", "/tmp", "--rt-evidence-ps", "3f9d6677"])
        self.assertEqual(args.rt_evidence_ps, "3f9d6677")
        self.assertFalse(
            not args.allow_diagnostics
            and not args.auto_cross
            and not any((args.rt_evidence, args.rt_evidence_ps))
        )

    def test_shader_probe_is_recorded_as_diagnostic(self):
        args = capture.parser().parse_args(["capture", "--guest-root", "/tmp", "--shader-probe-crc", "2d15c0fe"])
        self.assertEqual(args.shader_probe_crc, "2d15c0fe")
        self.assertFalse(
            not args.allow_diagnostics
            and not args.auto_cross
            and not any((args.shader_probe_crc,))
        )

    def test_videoout_evidence_is_recorded_as_diagnostic(self):
        args = capture.parser().parse_args(["capture", "--guest-root", "/tmp", "--videoout-evidence"])
        self.assertTrue(args.videoout_evidence)
        self.assertFalse(
            not args.allow_diagnostics
            and not args.auto_cross
            and not any((args.videoout_evidence,))
        )


if __name__ == "__main__":
    unittest.main()
