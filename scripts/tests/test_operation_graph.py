#!/usr/bin/env python3
"""Deterministic contract tests for the offline operation dependency graph."""

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "kyty_operation_graph.py"
SPEC = importlib.util.spec_from_file_location("kyty_operation_graph", MODULE_PATH)
assert SPEC and SPEC.loader
graph = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(graph)


def span(resource, offset, size, access):
    return {"resource": resource, "offset": offset, "size": size, "access": access}


def record(rid, kind, *spans):
    return {"id": rid, "kind": kind, "spans": list(spans)}


def document(*records):
    return {"schema": graph.INPUT_SCHEMA, "records": list(records)}


def edges_of(result):
    return sorted((e["from"], e["to"], e["resource"], e["offset"], e["size"]) for e in result["edges"])


def externals_of(result):
    return sorted((e["to"], e["resource"], e["offset"], e["size"]) for e in result["external_inputs"])


class NearestWriterTests(unittest.TestCase):
    def test_nearest_writer_shadows_older_bytes_and_keeps_partial_interval_writers(self):
        doc = document(
            record("w0", "dma", span("buf", 0, 64, "write")),
            record("w1", "draw", span("buf", 32, 64, "write")),
            record("r2", "dispatch", span("buf", 0, 128, "read")),
        )
        result = graph.build_graph(doc)
        self.assertEqual(edges_of(result), [("w0", "r2", "buf", 0, 32), ("w1", "r2", "buf", 32, 64)])
        self.assertEqual(externals_of(result), [("r2", "buf", 96, 32)])

    def test_older_full_writer_is_superseded_by_nearer_writer_of_same_bytes(self):
        doc = document(
            record("w0", "dma", span("buf", 0, 8, "write")),
            record("w1", "dma", span("buf", 0, 8, "write")),
            record("r2", "resolve", span("buf", 0, 8, "read")),
        )
        result = graph.build_graph(doc)
        self.assertEqual(edges_of(result), [("w1", "r2", "buf", 0, 8)])
        self.assertEqual(externals_of(result), [])

    def test_future_writers_never_satisfy_an_earlier_read(self):
        doc = document(
            record("r0", "draw", span("buf", 0, 16, "read")),
            record("w1", "dma", span("buf", 0, 16, "write")),
        )
        result = graph.build_graph(doc)
        self.assertEqual(edges_of(result), [])
        self.assertEqual(externals_of(result), [("r0", "buf", 0, 16)])

    def test_unproduced_bytes_and_other_resources_are_external_inputs(self):
        doc = document(
            record("w0", "dma", span("a", 0, 8, "write")),
            record("r1", "draw", span("b", 0, 8, "read")),
        )
        result = graph.build_graph(doc)
        self.assertEqual(edges_of(result), [])
        self.assertEqual(externals_of(result), [("r1", "b", 0, 8)])

    def test_disjoint_writes_in_the_same_resource_do_not_create_edges(self):
        doc = document(
            record("w0", "dma", span("buf", 0, 8, "write")),
            record("r1", "draw", span("buf", 8, 8, "read")),
        )
        result = graph.build_graph(doc)
        self.assertEqual(edges_of(result), [])
        self.assertEqual(externals_of(result), [("r1", "buf", 8, 8)])

    def test_multiple_disjoint_pieces_of_one_read_resolve_to_their_own_writers(self):
        doc = document(
            record("w0", "dma", span("buf", 0, 16, "write")),
            record("w1", "dma", span("buf", 32, 16, "write")),
            record("r2", "dispatch", span("buf", 8, 32, "read")),
        )
        result = graph.build_graph(doc)
        self.assertEqual(edges_of(result), [("w0", "r2", "buf", 8, 8), ("w1", "r2", "buf", 32, 8)])
        self.assertEqual(externals_of(result), [("r2", "buf", 16, 16)])


class ResultContractTests(unittest.TestCase):
    def test_result_is_diagnostic_and_never_claims_synchronization(self):
        result = graph.build_graph(document(record("w0", "dma", span("buf", 0, 8, "write"))))
        self.assertEqual(result["schema"], graph.OUTPUT_SCHEMA)
        self.assertTrue(result["diagnostic_only"])
        self.assertFalse(result["synchronization_proof"])
        self.assertEqual(result["nodes"], [{"id": "w0", "kind": "dma", "ordinal": 0}])

    def test_identical_input_produces_identical_output(self):
        doc = document(
            record("w0", "dma", span("buf", 0, 64, "write")),
            record("w1", "draw", span("buf", 32, 64, "write")),
            record("r2", "dispatch", span("buf", 0, 128, "read")),
        )
        self.assertEqual(json.dumps(graph.build_graph(doc), sort_keys=True), json.dumps(graph.build_graph(doc), sort_keys=True))

    def test_dot_output_contains_the_writer_to_reader_edge(self):
        doc = document(
            record("w0", "dma", span("buf", 0, 8, "write")),
            record("r1", "draw", span("buf", 0, 8, "read")),
        )
        dot = graph.to_dot(graph.build_graph(doc))
        self.assertTrue(dot.startswith("digraph"))
        self.assertIn('"op:w0" -> "op:r1"', dot)


class MalformedInputTests(unittest.TestCase):
    def assertRejected(self, doc):
        with self.assertRaises(graph.GraphInputError):
            graph.build_graph(doc)

    def test_rejects_unknown_schema_and_extra_top_level_keys(self):
        self.assertRejected({"schema": "other_v1", "records": [record("w0", "dma", span("b", 0, 8, "write"))]})
        doc = document(record("w0", "dma", span("b", 0, 8, "write")))
        doc["note"] = "extra"
        self.assertRejected(doc)

    def test_rejects_duplicate_record_ids(self):
        self.assertRejected(document(record("x", "dma", span("b", 0, 8, "write")), record("x", "draw", span("b", 0, 8, "read"))))

    def test_rejects_unknown_kind_and_access(self):
        self.assertRejected(document(record("w0", "compute", span("b", 0, 8, "write"))))
        self.assertRejected(document(record("w0", "dma", span("b", 0, 8, "readwrite"))))

    def test_rejects_zero_negative_boolean_and_float_numbers(self):
        self.assertRejected(document(record("w0", "dma", span("b", 0, 0, "write"))))
        self.assertRejected(document(record("w0", "dma", span("b", -1, 8, "write"))))
        self.assertRejected(document(record("w0", "dma", span("b", True, 8, "write"))))
        self.assertRejected(document(record("w0", "dma", span("b", 0, 4.0, "write"))))

    def test_rejects_extents_beyond_the_bounded_address_space(self):
        self.assertRejected(document(record("w0", "dma", span("b", graph.MAX_EXTENT - 4, 8, "write"))))

    def test_rejects_empty_and_oversized_span_lists(self):
        self.assertRejected(document(record("w0", "dma")))
        too_many = [span("b", i * 8, 8, "write") for i in range(graph.MAX_SPANS_PER_RECORD + 1)]
        self.assertRejected(document(record("w0", "dma", *too_many)))

    def test_rejects_too_many_records(self):
        records = [record(f"r{i}", "dma", span("b", 0, 8, "write")) for i in range(graph.MAX_RECORDS + 1)]
        self.assertRejected(document(*records))

    def test_rejects_invalid_resource_ids_and_missing_or_extra_record_keys(self):
        self.assertRejected(document(record("w0", "dma", span("", 0, 8, "write"))))
        self.assertRejected(document(record("w0", "dma", span("has space", 0, 8, "write"))))
        self.assertRejected(document({"id": "w0", "kind": "dma"}))
        extra = record("w0", "dma", span("b", 0, 8, "write"))
        extra["note"] = "x"
        self.assertRejected(document(extra))

    def test_rejects_overlapping_writes_inside_one_record(self):
        self.assertRejected(document(record("w0", "draw", span("b", 0, 8, "write"), span("b", 4, 8, "write"))))



class InputFileTests(unittest.TestCase):
    def test_duplicate_json_keys_are_rejected_before_graph_construction(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "dup.json"
            path.write_text('{"schema": "kyty_operation_graph_input_v1", "schema": "x", "records": []}', encoding="utf-8")
            with self.assertRaises(graph.GraphInputError):
                graph.load_document(path)

    def test_input_file_size_is_bounded(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "large.json"
            path.write_bytes(b" " * (graph.MAX_INPUT_BYTES + 1))
            with self.assertRaises(graph.GraphInputError):
                graph.load_document(path)

    def test_cli_writes_json_and_dot_for_valid_input_and_returns_one_for_malformed_input(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            good = root / "good.json"
            good.write_text(json.dumps(document(
                record("w0", "dma", span("buf", 0, 8, "write")),
                record("r1", "draw", span("buf", 0, 8, "read")),
            )), encoding="utf-8")
            out_json = root / "graph.json"
            out_dot = root / "graph.dot"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(graph.main([str(good), "--json", str(out_json), "--dot", str(out_dot)]), 0)
            self.assertEqual(json.loads(out_json.read_text(encoding="utf-8"))["schema"], graph.OUTPUT_SCHEMA)
            self.assertIn('"op:w0" -> "op:r1"', out_dot.read_text(encoding="utf-8"))

            bad = root / "bad.json"
            bad.write_text('{"schema": "kyty_operation_graph_input_v1", "records": [{"id": "x"}]}', encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(graph.main([str(bad)]), 1)


class BudgetTests(unittest.TestCase):
    def test_analysis_budget_rejects_pathological_candidate_counts(self):
        # Disjoint writers never satisfy the reads, so every preceding write is examined.
        writers = [record(f"w{i}", "dma", span("b", i * 8, 8, "write")) for i in range(600)]
        readers = [record(f"r{i}", "draw", span("b", 1 << 20, 8, "read")) for i in range(600)]
        original = graph.MAX_CANDIDATE_OVERLAPS
        graph.MAX_CANDIDATE_OVERLAPS = 1000
        try:
            with self.assertRaises(graph.GraphInputError):
                graph.build_graph(document(*writers, *readers))
        finally:
            graph.MAX_CANDIDATE_OVERLAPS = original


@contextlib.contextmanager
def bound(name, value):
    original = getattr(graph, name)
    setattr(graph, name, value)
    try:
        yield
    finally:
        setattr(graph, name, original)


class IdentifierAnchoringTests(unittest.TestCase):
    def test_identifiers_must_match_the_whole_string_so_trailing_newlines_fail(self):
        for doc in (
            document(record("w0\n", "dma", span("buf", 0, 8, "write"))),
            document(record("w0", "dma", span("buf\n", 0, 8, "write"))),
            document(record(" w0", "dma", span("buf", 0, 8, "write"))),
        ):
            with self.subTest(doc=doc):
                with self.assertRaises(graph.GraphInputError):
                    graph.build_graph(doc)


class DotNamespaceTests(unittest.TestCase):
    def test_generated_external_sources_never_share_a_name_with_an_operation_id(self):
        # Operation id "external:vb" is a valid identifier and must stay distinct from the external source for "vb".
        doc = document(
            record("external:vb", "dma", span("vb", 0, 8, "write")),
            record("r1", "draw", span("vb", 0, 16, "read")),
        )
        dot = graph.to_dot(graph.build_graph(doc))
        self.assertIn('"op:external:vb" -> "op:r1"', dot)
        self.assertIn('"ext:vb" -> "op:r1"', dot)
        declared = [line.split(" [")[0].strip() for line in dot.splitlines() if line.startswith("  \"")
                    and " -> " not in line]
        self.assertEqual(len(declared), len(set(declared)))


class ReadModifyWriteTests(unittest.TestCase):
    def test_read_modify_write_reads_prior_state_and_then_publishes_its_write(self):
        doc = document(
            record("w0", "dma", span("buf", 0, 8, "write")),
            record("rmw1", "draw", span("buf", 0, 8, "read"), span("buf", 0, 8, "write")),
            record("r2", "dispatch", span("buf", 0, 8, "read")),
        )
        result = graph.build_graph(doc)
        self.assertEqual(edges_of(result), sorted([("w0", "rmw1", "buf", 0, 8), ("rmw1", "r2", "buf", 0, 8)]))
        self.assertEqual(externals_of(result), [])

    def test_read_modify_write_without_a_prior_writer_is_an_external_input_not_self_supplied(self):
        result = graph.build_graph(document(record("rmw0", "draw", span("buf", 0, 8, "read"), span("buf", 0, 8, "write"))))
        self.assertEqual(edges_of(result), [])
        self.assertEqual(externals_of(result), [("rmw0", "buf", 0, 8)])

    def test_two_writes_of_one_record_to_the_same_bytes_are_still_rejected(self):
        with self.assertRaises(graph.GraphInputError):
            graph.build_graph(document(record("w0", "draw", span("b", 0, 8, "write"), span("b", 4, 8, "write"))))

    def test_duplicate_read_spans_produce_one_edge_per_writer_piece(self):
        doc = document(
            record("w0", "dma", span("buf", 0, 8, "write")),
            record("r1", "draw", span("buf", 0, 8, "read"), span("buf", 0, 8, "read")),
        )
        result = graph.build_graph(doc)
        self.assertEqual(edges_of(result), [("w0", "r1", "buf", 0, 8)])


class OutputBoundTests(unittest.TestCase):
    def test_output_items_are_bounded_and_fail_with_an_actionable_error(self):
        reads = [span("buf", i * 16, 8, "read") for i in range(64)]
        doc = document(record("r0", "draw", *reads))
        with bound("MAX_OUTPUT_ITEMS", 10):
            with self.assertRaisesRegex(graph.GraphInputError, "output items"):
                graph.build_graph(doc)
        self.assertEqual(len(graph.build_graph(doc)["external_inputs"]), 64)

    def test_fragmentation_of_an_unattributed_read_is_bounded(self):
        writers = [record(f"w{i}", "dma", span("buf", 2 * i, 1, "write")) for i in range(1, 21)]
        doc = document(*writers, record("r", "draw", span("buf", 0, 64, "read")))
        with bound("MAX_UNATTRIBUTED_FRAGMENTS", 8):
            with self.assertRaisesRegex(graph.GraphInputError, "fragments"):
                graph.build_graph(doc)
        self.assertEqual(len(graph.build_graph(doc)["edges"]), 20)


class CliErrorTests(unittest.TestCase):
    def run_cli(self, *args):
        with contextlib.redirect_stderr(io.StringIO()) as err:
            code = graph.main(list(args))
        return code, err.getvalue()

    def test_missing_input_file_is_an_actionable_error(self):
        with tempfile.TemporaryDirectory() as td:
            code, message = self.run_cli(str(Path(td) / "absent.json"))
        self.assertEqual(code, 1)
        self.assertIn("cannot read input", message)

    def test_deeply_nested_json_is_rejected_without_a_traceback(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "deep.json"
            path.write_text("[" * 200000, encoding="utf-8")
            code, message = self.run_cli(str(path))
        self.assertEqual(code, 1)
        self.assertIn("error:", message)

    def test_unwritable_output_path_is_an_actionable_error(self):
        with tempfile.TemporaryDirectory() as td:
            good = Path(td) / "good.json"
            good.write_text(json.dumps(document(record("w0", "dma", span("b", 0, 8, "write")))), encoding="utf-8")
            code, message = self.run_cli(str(good), "--json", str(Path(td) / "missing-dir" / "out.json"))
        self.assertEqual(code, 1)
        self.assertIn("cannot write output", message)


# Golden contract shared with the C++ producer unit test (UnitTestEmulatorGraphicsOperationTrace.cpp).
# Keep these literals byte-identical to the producer's encoding.
GOLDEN_INPUT = (
    '{"schema":"kyty_operation_graph_input_v1","records":['
    '{"id":"op0","kind":"draw","spans":['
    '{"resource":"guest","offset":4096,"size":12,"access":"read"},'
    '{"resource":"guest","offset":8192,"size":64,"access":"read"},'
    '{"resource":"guest","offset":12288,"size":32,"access":"read"},'
    '{"resource":"guest","offset":12288,"size":32,"access":"write"},'
    '{"resource":"guest","offset":16384,"size":256,"access":"read"},'
    '{"resource":"guest","offset":16384,"size":256,"access":"write"}]},'
    '{"id":"op1","kind":"dispatch","spans":['
    '{"resource":"guest","offset":12288,"size":32,"access":"read"}]}]}'
)
GOLDEN_META = (
    '{"schema":"kyty_operation_trace_meta_v1","input_md5":"862d69cca795f7de144947efea015b54",'
    '"operations_seen":2,"records_emitted":2,'
    '"operations_dropped":0,"truncated":false,'
    '"not_traced_kinds":["auto_draw","depth_stencil_copy_draw","dma","indirect_dispatch","indirect_draw","resolve_and_copy"],'
    '"incomplete_records":[{"id":"op0","reasons":["color_attachment_extent_unknown"]}]}'
)


EMPTY_DIGEST = "d41d8cd98f00b204e9800998ecf8427e"


def digest_of(data):
    return hashlib.md5(data, usedforsecurity=False).hexdigest()


def trace_meta(**overrides):
    values = {
        "schema": graph.TRACE_META_SCHEMA,
        "input_md5": EMPTY_DIGEST,
        "operations_seen": 2,
        "records_emitted": 2,
        "operations_dropped": 0,
        "truncated": False,
        "not_traced_kinds": ["dma"],
        "incomplete_records": [],
    }
    values.update(overrides)
    return values


class TraceMetadataTests(unittest.TestCase):
    def two_record_document(self):
        return document(
            record("w0", "draw", span("buf", 0, 8, "write")),
            record("r1", "dispatch", span("buf", 0, 8, "read")),
        )

    def test_without_metadata_completeness_is_reported_as_absent_not_as_complete(self):
        result = graph.build_graph(self.two_record_document())
        self.assertEqual(result["completeness"], {"metadata": "absent"})

    def test_incomplete_or_dropped_operations_make_traced_operations_incomplete(self):
        meta = graph.parse_trace_meta(trace_meta(
            incomplete_records=[{"id": "w0", "reasons": ["color_attachment_extent_unknown"]}]))
        result = graph.build_graph(self.two_record_document(), trace_meta=meta)
        self.assertFalse(result["completeness"]["traced_operations_complete"])
        self.assertEqual(result["completeness"]["incomplete_records"],
                         [{"id": "w0", "reasons": ["color_attachment_extent_unknown"]}])
        dropped = graph.parse_trace_meta(trace_meta(operations_seen=3, operations_dropped=1))
        result = graph.build_graph(self.two_record_document(), trace_meta=dropped)
        self.assertFalse(result["completeness"]["traced_operations_complete"])

    def test_clean_metadata_reports_complete_while_listing_untraced_kinds(self):
        result = graph.build_graph(self.two_record_document(), trace_meta=graph.parse_trace_meta(trace_meta()))
        self.assertTrue(result["completeness"]["traced_operations_complete"])
        self.assertEqual(result["completeness"]["not_traced_kinds"], ["dma"])

    def test_truncated_trace_is_never_reported_complete(self):
        meta = graph.parse_trace_meta(trace_meta(truncated=True, operations_seen=3, operations_dropped=1))
        result = graph.build_graph(self.two_record_document(), trace_meta=meta)
        self.assertFalse(result["completeness"]["traced_operations_complete"])
        self.assertTrue(result["completeness"]["truncated"])

    def test_metadata_must_agree_with_the_number_of_emitted_records(self):
        meta = graph.parse_trace_meta(trace_meta(records_emitted=3, operations_seen=3))
        with self.assertRaisesRegex(graph.GraphInputError, "does not match"):
            graph.build_graph(self.two_record_document(), trace_meta=meta)

    def test_metadata_schema_is_strict(self):
        cases = [
            trace_meta(schema="other_v1"),
            dict(trace_meta(), note="extra"),
            trace_meta(operations_seen=5),
            trace_meta(truncated="no"),
            trace_meta(not_traced_kinds=["teleport"]),
            trace_meta(incomplete_records=[{"id": "w0", "reasons": ["made_up_reason"]}]),
            trace_meta(incomplete_records=[{"id": "w0", "reasons": []}]),
            trace_meta(incomplete_records=[{"id": "w0\n", "reasons": ["texture_extent_unknown"]}]),
            trace_meta(incomplete_records=[{"id": "w0", "reasons": ["texture_extent_unknown"], "x": 1}]),
        ]
        for values in cases:
            with self.subTest(values=values):
                with self.assertRaises(graph.GraphInputError):
                    graph.parse_trace_meta(values)

    def test_incomplete_record_list_is_bounded(self):
        many = [{"id": f"op{i}", "reasons": ["span_limit_reached"]} for i in range(graph.MAX_TRACE_INCOMPLETE + 1)]
        with self.assertRaises(graph.GraphInputError):
            graph.parse_trace_meta(trace_meta(operations_seen=len(many), records_emitted=0,
                                              operations_dropped=len(many), incomplete_records=many))

    def test_cli_accepts_metadata_and_rejects_malformed_metadata(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            ops = root / "ops.json"
            ops_bytes = json.dumps(self.two_record_document()).encode("utf-8")
            ops.write_bytes(ops_bytes)
            meta = root / "meta.json"
            meta.write_text(json.dumps(trace_meta(input_md5=digest_of(ops_bytes))), encoding="utf-8")
            out = root / "graph.json"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(graph.main([str(ops), "--meta", str(meta), "--json", str(out)]), 0)
            self.assertTrue(json.loads(out.read_text(encoding="utf-8"))["completeness"]["traced_operations_complete"])
            meta.write_text('{"schema": "kyty_operation_trace_meta_v1"', encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(graph.main([str(ops), "--meta", str(meta)]), 1)


class DigestBindingTests(unittest.TestCase):
    """The sidecar describes one exact input. Matching record counts are not evidence of pairing."""

    def two_records(self, rid_a="w0", rid_b="r1"):
        return json.dumps(document(
            record(rid_a, "draw", span("buf", 0, 8, "write")),
            record(rid_b, "dispatch", span("buf", 0, 8, "read")),
        )).encode("utf-8")

    def test_sidecar_is_accepted_only_for_the_exact_input_bytes(self):
        raw = self.two_records()
        meta = graph.parse_trace_meta(trace_meta(input_md5=digest_of(raw)))
        self.assertIs(graph.bind_trace_meta(meta, raw), meta)

    def test_same_count_sidecar_from_another_trace_is_rejected(self):
        other_trace = self.two_records()
        this_trace = self.two_records(rid_a="x0", rid_b="x1")
        meta = graph.parse_trace_meta(trace_meta(input_md5=digest_of(other_trace)))
        with self.assertRaisesRegex(graph.GraphInputError, "digest"):
            graph.bind_trace_meta(meta, this_trace)

    def test_whitespace_change_is_rejected_unless_the_digest_describes_it(self):
        raw = self.two_records()
        changed = raw.replace(b",", b", ", 1)
        meta = graph.parse_trace_meta(trace_meta(input_md5=digest_of(raw)))
        with self.assertRaisesRegex(graph.GraphInputError, "digest"):
            graph.bind_trace_meta(meta, changed)
        rebound = graph.parse_trace_meta(trace_meta(input_md5=digest_of(changed)))
        self.assertIs(graph.bind_trace_meta(rebound, changed), rebound)

    def test_digest_field_must_be_32_lowercase_hex_digits(self):
        for bad in ("862D69CCA795F7DE144947EFEA015B54", "862d69cca795f7de144947efea015b5", "g" * 32, "", 0):
            with self.subTest(digest=bad):
                with self.assertRaises(graph.GraphInputError):
                    graph.parse_trace_meta(trace_meta(input_md5=bad))
        values = trace_meta()
        del values["input_md5"]
        with self.assertRaises(graph.GraphInputError):
            graph.parse_trace_meta(values)

    def test_cli_exact_producer_pair_passes_and_altered_or_foreign_inputs_fail(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            meta = root / "meta.json"
            meta.write_text(GOLDEN_META, encoding="utf-8")
            ops = root / "ops.json"
            ops.write_bytes(GOLDEN_INPUT.encode("utf-8"))
            out = root / "graph.json"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(graph.main([str(ops), "--meta", str(meta), "--json", str(out)]), 0)
            ops.write_bytes(GOLDEN_INPUT.replace(",", ", ", 1).encode("utf-8"))
            with contextlib.redirect_stderr(io.StringIO()) as err:
                self.assertEqual(graph.main([str(ops), "--meta", str(meta)]), 1)
            self.assertIn("digest", err.getvalue())
            foreign = root / "foreign.json"
            foreign.write_bytes(self.two_records(rid_a="x0", rid_b="x1"))
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(graph.main([str(foreign), "--meta", str(meta)]), 1)


class ProducerContractTests(unittest.TestCase):
    """Golden producer output: the same bytes are asserted by the C++ producer unit test."""

    def test_golden_producer_document_builds_the_expected_cross_draw_edge(self):
        doc = json.loads(GOLDEN_INPUT)
        meta = graph.parse_trace_meta(json.loads(GOLDEN_META))
        result = graph.build_graph(doc, trace_meta=meta)
        self.assertEqual(edges_of(result), [("op0", "op1", "guest", 12288, 32)])
        self.assertFalse(result["completeness"]["traced_operations_complete"])
        self.assertEqual(result["completeness"]["not_traced_kinds"][0], "auto_draw")

    def test_golden_metadata_round_trips_through_the_cli(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            ops = root / "ops.json"
            ops.write_text(GOLDEN_INPUT, encoding="utf-8")
            meta = root / "meta.json"
            meta.write_text(GOLDEN_META, encoding="utf-8")
            out = root / "graph.json"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(graph.main([str(ops), "--meta", str(meta), "--json", str(out)]), 0)
            result = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(result["completeness"]["incomplete_records"][0]["id"], "op0")


if __name__ == "__main__":
    unittest.main()
