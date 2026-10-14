#!/usr/bin/env python3
"""Offline dependency graph over Kyty graphics operation records.

Each draw, dispatch, DMA or resolve record lists explicit resource spans. A read
span is linked to the nearest preceding record whose write span overlaps each
still-unattributed byte, so partially overlapping writers all appear. Bytes with
no earlier writer are external inputs. A record's reads observe the state before
that record, so a read-modify-write span both reads from earlier writers and
publishes its write for later records. Later records never satisfy an earlier read.

The graph is diagnostic evidence about trace order. It is not proof of hardware
synchronization, and it must not be used to justify removing a barrier or wait.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


INPUT_SCHEMA = "kyty_operation_graph_input_v1"
OUTPUT_SCHEMA = "kyty_operation_graph_v1"
RECORD_KINDS = ("draw", "dispatch", "dma", "resolve")
ACCESS_KINDS = ("read", "write")
TOP_LEVEL_KEYS = {"schema", "records"}
RECORD_KEYS = {"id", "kind", "spans"}
SPAN_KEYS = {"resource", "offset", "size", "access"}
MAX_INPUT_BYTES = 4 * 1024 * 1024
MAX_RECORDS = 4096
MAX_SPANS_PER_RECORD = 64
MAX_TOTAL_SPANS = 65536
MAX_EXTENT = 1 << 48
MAX_CANDIDATE_OVERLAPS = 1_000_000
MAX_OUTPUT_ITEMS = 131072
MAX_UNATTRIBUTED_FRAGMENTS = 256
TRACE_META_SCHEMA = "kyty_operation_trace_meta_v1"
INPUT_DIGEST_PATTERN = re.compile(r"[0-9a-f]{32}")
TRACE_META_KEYS = {"schema", "input_md5", "operations_seen", "records_emitted", "operations_dropped", "truncated",
                   "not_traced_kinds", "incomplete_records"}
INCOMPLETE_RECORD_KEYS = {"id", "reasons"}
NOT_TRACED_KINDS = ("auto_draw", "depth_stencil_copy_draw", "dma", "indirect_dispatch", "indirect_draw",
                    "resolve_and_copy")
INCOMPLETE_REASONS = (
    "index_type_unknown",
    "color_attachment_extent_unknown",
    "htile_metadata_not_recorded",
    "stencil_access_not_recorded",
    "texture_extent_unknown",
    "storage_image_extent_unknown",
    "device_address_resources_not_spanned",
    "gds_pointers_not_spanned",
    "storage_usage_unknown",
    "conflicting_write_span",
    "span_limit_reached",
    "invalid_span",
    "operation_without_exact_spans",
    "attachment_clear_not_recorded",
)
MAX_TRACE_INCOMPLETE = MAX_RECORDS
IDENTIFIER = re.compile(r"[A-Za-z0-9_.:-]{1,128}")
DOT_OPERATION_PREFIX = "op:"
DOT_EXTERNAL_PREFIX = "ext:"


class GraphInputError(ValueError):
    pass


def _reject_non_finite(token):
    raise GraphInputError(f"non-finite number {token} is not allowed")


def _reject_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise GraphInputError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def read_bounded_bytes(path, label):
    try:
        with Path(path).open("rb") as handle:
            data = handle.read(MAX_INPUT_BYTES + 1)
    except OSError as error:
        raise GraphInputError(f"cannot read {label} {path}: {error.strerror or error}") from error
    if len(data) > MAX_INPUT_BYTES:
        raise GraphInputError(f"{label} exceeds the {MAX_INPUT_BYTES}-byte limit")
    return data


def parse_json_bytes(data, label):
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys, parse_constant=_reject_non_finite)
    except GraphInputError:
        raise
    except RecursionError as error:
        raise GraphInputError("JSON nesting is too deep") from error
    except ValueError as error:
        raise GraphInputError(f"{label} is not valid UTF-8 JSON: {error}") from error


def load_document(path):
    return parse_json_bytes(read_bounded_bytes(path, "input"), "input")


def _integer(value, name, low, high):
    if type(value) is not int or not low <= value <= high:
        raise GraphInputError(f"{name} must be an integer in [{low}, {high}]")
    return value


def _identifier(value, name):
    if not isinstance(value, str) or IDENTIFIER.fullmatch(value) is None:
        raise GraphInputError(f"{name} must match {IDENTIFIER.pattern} with no surrounding text")
    return value


def _parse_span(raw, record_id):
    if not isinstance(raw, dict) or set(raw) != SPAN_KEYS:
        raise GraphInputError(f"record {record_id!r} has a span with missing or unknown keys")
    offset = _integer(raw["offset"], "span offset", 0, MAX_EXTENT - 1)
    size = _integer(raw["size"], "span size", 1, MAX_EXTENT)
    if offset + size > MAX_EXTENT:
        raise GraphInputError(f"record {record_id!r} span exceeds the address bound")
    if raw["access"] not in ACCESS_KINDS:
        raise GraphInputError(f"record {record_id!r} has an unknown access kind")
    return {
        "resource": _identifier(raw["resource"], "span resource"),
        "offset": offset,
        "size": size,
        "end": offset + size,
        "access": raw["access"],
    }


def parse_trace_meta(document):
    """Validates the producer's sidecar. Records and dropped operations must reconcile with the input."""
    if not isinstance(document, dict) or set(document) != TRACE_META_KEYS:
        raise GraphInputError("trace metadata must contain exactly the producer keys")
    if document["schema"] != TRACE_META_SCHEMA:
        raise GraphInputError(f"unsupported trace metadata schema; expected {TRACE_META_SCHEMA}")
    digest = document["input_md5"]
    if not isinstance(digest, str) or INPUT_DIGEST_PATTERN.fullmatch(digest) is None:
        raise GraphInputError("trace metadata input_md5 must be 32 lowercase hexadecimal digits")
    counts = {key: _integer(document[key], key, 0, 1 << 32)
              for key in ("operations_seen", "records_emitted", "operations_dropped")}
    if counts["records_emitted"] + counts["operations_dropped"] != counts["operations_seen"]:
        raise GraphInputError("trace metadata counts do not add up to operations_seen")
    if type(document["truncated"]) is not bool:
        raise GraphInputError("trace metadata truncated must be a boolean")
    not_traced = document["not_traced_kinds"]
    if not isinstance(not_traced, list) or len(set(not_traced)) != len(not_traced) or \
            any(kind not in NOT_TRACED_KINDS for kind in not_traced):
        raise GraphInputError("trace metadata lists an unknown or repeated untraced kind")
    incomplete_raw = document["incomplete_records"]
    if not isinstance(incomplete_raw, list) or len(incomplete_raw) > MAX_TRACE_INCOMPLETE:
        raise GraphInputError(f"trace metadata lists more than {MAX_TRACE_INCOMPLETE} incomplete records")
    incomplete = []
    seen_ids = set()
    for raw in incomplete_raw:
        if not isinstance(raw, dict) or set(raw) != INCOMPLETE_RECORD_KEYS:
            raise GraphInputError("incomplete record entry must contain exactly id and reasons")
        record_id = _identifier(raw["id"], "incomplete record id")
        reasons = raw["reasons"]
        if record_id in seen_ids or not isinstance(reasons, list) or not reasons or \
                len(set(reasons)) != len(reasons) or any(reason not in INCOMPLETE_REASONS for reason in reasons):
            raise GraphInputError(f"incomplete record {record_id!r} has an unknown or repeated reason")
        seen_ids.add(record_id)
        incomplete.append({"id": record_id, "reasons": sorted(reasons)})
    return {**counts, "input_md5": digest, "truncated": document["truncated"], "not_traced_kinds": sorted(not_traced),
            "incomplete_records": sorted(incomplete, key=lambda item: item["id"])}


def load_trace_meta(path):
    return parse_trace_meta(parse_json_bytes(read_bounded_bytes(path, "trace metadata"), "trace metadata"))


def bind_trace_meta(trace_meta, input_bytes):
    """Accepts a sidecar only for the exact input bytes it describes. Pairing by count alone is not enough."""
    if hashlib.md5(input_bytes, usedforsecurity=False).hexdigest() != trace_meta["input_md5"]:
        raise GraphInputError("trace metadata digest does not match the input bytes")
    return trace_meta


def _overlaps(a, b):
    return a["resource"] == b["resource"] and a["offset"] < b["end"] and b["offset"] < a["end"]


def _check_record_conflicts(spans, record_id):
    """Two writes of one record to the same bytes have no defined final value. Read-modify-write is allowed."""
    writes = [span for span in spans if span["access"] == "write"]
    for i, first in enumerate(writes):
        for second in writes[i + 1:]:
            if _overlaps(first, second):
                raise GraphInputError(f"record {record_id!r} writes the same bytes twice")


def _parse_record(raw, ordinal):
    if not isinstance(raw, dict) or set(raw) != RECORD_KEYS:
        raise GraphInputError(f"record {ordinal} has missing or unknown keys")
    record_id = _identifier(raw["id"], "record id")
    if raw["kind"] not in RECORD_KINDS:
        raise GraphInputError(f"record {record_id!r} has an unknown kind")
    spans_raw = raw["spans"]
    if not isinstance(spans_raw, list) or not 1 <= len(spans_raw) <= MAX_SPANS_PER_RECORD:
        raise GraphInputError(f"record {record_id!r} must have 1 to {MAX_SPANS_PER_RECORD} spans")
    spans = [_parse_span(item, record_id) for item in spans_raw]
    _check_record_conflicts(spans, record_id)
    return {"id": record_id, "kind": raw["kind"], "ordinal": ordinal, "spans": spans}


def parse_records(document):
    if not isinstance(document, dict) or set(document) != TOP_LEVEL_KEYS:
        raise GraphInputError("input must contain exactly the keys schema and records")
    if document["schema"] != INPUT_SCHEMA:
        raise GraphInputError(f"unsupported input schema; expected {INPUT_SCHEMA}")
    raw_records = document["records"]
    if not isinstance(raw_records, list) or not 1 <= len(raw_records) <= MAX_RECORDS:
        raise GraphInputError(f"records must be a list of 1 to {MAX_RECORDS} entries")
    records = []
    ids = set()
    total_spans = 0
    for ordinal, raw in enumerate(raw_records):
        record = _parse_record(raw, ordinal)
        if record["id"] in ids:
            raise GraphInputError(f"duplicate record id {record['id']!r}")
        ids.add(record["id"])
        total_spans += len(record["spans"])
        if total_spans > MAX_TOTAL_SPANS:
            raise GraphInputError(f"input exceeds {MAX_TOTAL_SPANS} total spans")
        records.append(record)
    return records


def _take(uncovered, lo, hi):
    """Removes [lo, hi) from the uncovered intervals and returns the overlapped pieces."""
    remaining = []
    hits = []
    for start, end in uncovered:
        hit_lo = max(start, lo)
        hit_hi = min(end, hi)
        if hit_lo >= hit_hi:
            remaining.append((start, end))
            continue
        hits.append((hit_lo, hit_hi))
        if start < hit_lo:
            remaining.append((start, hit_lo))
        if hit_hi < end:
            remaining.append((hit_hi, end))
    return remaining, hits


class _GraphOutput:
    """Collects deduplicated edges and external inputs under one explicit item bound."""

    def __init__(self):
        self.edges = []
        self.external_inputs = []
        self._keys = set()

    def _add(self, key, item, target):
        if key in self._keys:
            return
        if len(self.edges) + len(self.external_inputs) >= MAX_OUTPUT_ITEMS:
            raise GraphInputError(f"operation graph exceeds {MAX_OUTPUT_ITEMS} output items")
        self._keys.add(key)
        target.append(item)

    def add_edge(self, writer, reader, resource, lo, hi):
        key = ("edge", writer["ordinal"], reader["ordinal"], resource, lo, hi)
        self._add(key, {"from": writer["id"], "to": reader["id"], "resource": resource,
                        "offset": lo, "size": hi - lo, "_reader": reader["ordinal"], "_writer": writer["ordinal"]},
                  self.edges)

    def add_external(self, reader, resource, lo, hi):
        key = ("external", reader["ordinal"], resource, lo, hi)
        self._add(key, {"to": reader["id"], "resource": resource, "offset": lo, "size": hi - lo,
                        "_reader": reader["ordinal"]}, self.external_inputs)


def build_graph(document, trace_meta=None):
    records = parse_records(document)
    if trace_meta is not None and trace_meta["records_emitted"] != len(records):
        raise GraphInputError("trace metadata does not match the operation records")
    writers_by_resource = {}
    output = _GraphOutput()
    candidates = 0
    for reader in records:
        for read in (span for span in reader["spans"] if span["access"] == "read"):
            uncovered = [(read["offset"], read["end"])]
            # Walk writers nearest-first; only records before this reader are listed.
            for writer_ordinal, write in reversed(writers_by_resource.get(read["resource"], [])):
                candidates += 1
                if candidates > MAX_CANDIDATE_OVERLAPS:
                    raise GraphInputError("operation graph exceeds the analysis budget")
                uncovered, hits = _take(uncovered, write["offset"], write["end"])
                if len(uncovered) > MAX_UNATTRIBUTED_FRAGMENTS:
                    raise GraphInputError(f"read span fragments into more than {MAX_UNATTRIBUTED_FRAGMENTS} pieces")
                for lo, hi in hits:
                    output.add_edge(records[writer_ordinal], reader, read["resource"], lo, hi)
                if not uncovered:
                    break
            for lo, hi in uncovered:
                output.add_external(reader, read["resource"], lo, hi)
        # Publish writes only after this record's reads, so a read-modify-write is not its own writer.
        for write in (span for span in reader["spans"] if span["access"] == "write"):
            writers_by_resource.setdefault(write["resource"], []).append((reader["ordinal"], write))
    return _render(records, output, _completeness(trace_meta))


def _completeness(trace_meta):
    """Traced operations are complete only when nothing was dropped, truncated or left incomplete."""
    if trace_meta is None:
        return {"metadata": "absent"}
    complete = (not trace_meta["truncated"] and trace_meta["operations_dropped"] == 0
                and not trace_meta["incomplete_records"])
    return {
        "metadata": "present",
        "traced_operations_complete": complete,
        "truncated": trace_meta["truncated"],
        "operations_seen": trace_meta["operations_seen"],
        "operations_dropped": trace_meta["operations_dropped"],
        "not_traced_kinds": trace_meta["not_traced_kinds"],
        "incomplete_records": trace_meta["incomplete_records"],
    }


def _render(records, output, completeness):
    edges = sorted(output.edges, key=lambda e: (e["_reader"], e["resource"], e["offset"], e["_writer"]))
    external_inputs = sorted(output.external_inputs, key=lambda e: (e["_reader"], e["resource"], e["offset"]))
    return {
        "schema": OUTPUT_SCHEMA,
        "diagnostic_only": True,
        "synchronization_proof": False,
        "nodes": [{"id": r["id"], "kind": r["kind"], "ordinal": r["ordinal"]} for r in records],
        "edges": [_public(edge) for edge in edges],
        "external_inputs": [_public(item) for item in external_inputs],
        "completeness": completeness,
    }


def _public(item):
    return {key: value for key, value in item.items() if not key.startswith("_")}


def to_dot(graph):
    """Operation nodes and generated external sources live in disjoint name prefixes."""
    lines = ["digraph kyty_operation_graph {"]
    for node in graph["nodes"]:
        lines.append(f'  "{DOT_OPERATION_PREFIX}{node["id"]}" [label="{node["id"]} {node["kind"]}"];')
    declared = set()
    for item in graph["external_inputs"]:
        source = f'{DOT_EXTERNAL_PREFIX}{item["resource"]}'
        if source not in declared:
            declared.add(source)
            lines.append(f'  "{source}" [shape=box];')
        label = f'{item["resource"]} [{item["offset"]}, {item["offset"] + item["size"]})'
        lines.append(f'  "{source}" -> "{DOT_OPERATION_PREFIX}{item["to"]}" [label="{label}", style=dashed];')
    for edge in graph["edges"]:
        label = f'{edge["resource"]} [{edge["offset"]}, {edge["offset"] + edge["size"]})'
        lines.append(f'  "{DOT_OPERATION_PREFIX}{edge["from"]}" -> "{DOT_OPERATION_PREFIX}{edge["to"]}" [label="{label}"];')
    lines.append("}")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description="Build a diagnostic dependency graph from operation records.")
    parser.add_argument("input", type=Path, help="kyty_operation_graph_input_v1 JSON file")
    parser.add_argument("--json", type=Path, help="write the graph JSON here instead of stdout")
    parser.add_argument("--dot", type=Path, help="also write a Graphviz DOT rendering")
    parser.add_argument("--meta", type=Path, help="kyty_operation_trace_meta_v1 sidecar from the live producer")
    args = parser.parse_args(argv)
    try:
        input_bytes = read_bounded_bytes(args.input, "input")
        trace_meta = bind_trace_meta(load_trace_meta(args.meta), input_bytes) if args.meta else None
        graph = build_graph(parse_json_bytes(input_bytes, "input"), trace_meta=trace_meta)
        text = json.dumps(graph, indent=2, sort_keys=True) + "\n"
        if args.json:
            args.json.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        if args.dot:
            args.dot.write_text(to_dot(graph), encoding="utf-8")
    except GraphInputError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"error: cannot write output: {error.strerror or error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
