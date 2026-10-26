#include "Kyty/Agent/Runner/OperationGraph.h"

#include "Kyty/Agent/Runner/Md5.h"

#include "RunnerSupport.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace Kyty::Agent::Runner {
namespace {

constexpr const char* kInputSchema     = "kyty_operation_graph_input_v1";
constexpr const char* kOutputSchema    = "kyty_operation_graph_v1";
constexpr const char* kTraceMetaSchema = "kyty_operation_trace_meta_v1";

constexpr size_t  kMaxInputBytes            = 4u * 1024u * 1024u;
constexpr size_t  kMaxRecords               = 4096;
constexpr size_t  kMaxSpansPerRecord        = 64;
constexpr size_t  kMaxTotalSpans            = 65536;
constexpr int64_t kMaxExtent                = int64_t {1} << 48;
constexpr int64_t kMaxCandidateOverlaps     = 1000000;
constexpr size_t  kMaxOutputItems           = 131072;
constexpr size_t  kMaxUnattributedFragments = 256;
constexpr size_t  kMaxTraceIncomplete       = kMaxRecords;

const std::set<std::string>& RecordKinds()
{
	static const std::set<std::string> kinds = {"draw", "dispatch", "dma", "resolve"};
	return kinds;
}

const std::set<std::string>& NotTracedKinds()
{
	static const std::set<std::string> kinds = {"auto_draw",         "depth_stencil_copy_draw", "dma", "indirect_dispatch",
	                                            "indirect_draw",     "resolve_and_copy"};
	return kinds;
}

const std::set<std::string>& IncompleteReasons()
{
	static const std::set<std::string> reasons = {
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
	};
	return reasons;
}

struct Span
{
	std::string resource;
	int64_t     offset = 0;
	int64_t     end    = 0;
	bool        write  = false;
};

struct Record
{
	std::string       id;
	std::string       kind;
	size_t            ordinal = 0;
	std::vector<Span> spans;
};

bool Fail(std::string* error, std::string message)
{
	*error = std::move(message);
	return false;
}

bool HasExactKeys(const Json& object, std::initializer_list<const char*> keys)
{
	if (!object.IsObject() || object.Size() != keys.size())
	{
		return false;
	}
	return std::all_of(keys.begin(), keys.end(), [&](const char* key) { return object.Has(key); });
}

bool IntegerIn(const Json& value, int64_t low, int64_t high, int64_t* out)
{
	if (!value.IsInteger() || value.AsNumber() < static_cast<double>(low) || value.AsNumber() > static_cast<double>(high))
	{
		return false;
	}
	*out = value.AsInt();
	return true;
}

// [A-Za-z0-9_.:-]{1,128} with no surrounding text.
bool IsIdentifier(const Json& value)
{
	if (!value.IsString() || value.AsString().empty() || value.AsString().size() > 128)
	{
		return false;
	}
	return std::all_of(value.AsString().begin(), value.AsString().end(), [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' || c == '-';
	});
}

// Strings from a fixed vocabulary, each at most once.
bool UniqueVocabulary(const Json& list, const std::set<std::string>& vocabulary, std::vector<std::string>* sorted)
{
	if (!list.IsArray())
	{
		return false;
	}
	std::set<std::string> seen;
	for (const Json& item: list.Items())
	{
		if (!item.IsString() || vocabulary.count(item.AsString()) == 0 || !seen.insert(item.AsString()).second)
		{
			return false;
		}
	}
	sorted->assign(seen.begin(), seen.end());
	return true;
}

Json StringArray(const std::vector<std::string>& values)
{
	Json array = Json::MakeArray();
	for (const auto& value: values)
	{
		array.Push(value);
	}
	return array;
}

std::string Quoted(const std::string& id)
{
	return "'" + id + "'";
}

bool ParseSpan(const Json& raw, const std::string& record_id, Span* span, std::string* error)
{
	if (!HasExactKeys(raw, {"resource", "offset", "size", "access"}))
	{
		return Fail(error, "record " + Quoted(record_id) + " has a span with missing or unknown keys");
	}
	int64_t offset = 0;
	int64_t size   = 0;
	if (!IntegerIn(raw["offset"], 0, kMaxExtent - 1, &offset))
	{
		return Fail(error, "span offset must be an integer in [0, " + std::to_string(kMaxExtent - 1) + "]");
	}
	if (!IntegerIn(raw["size"], 1, kMaxExtent, &size))
	{
		return Fail(error, "span size must be an integer in [1, " + std::to_string(kMaxExtent) + "]");
	}
	if (offset + size > kMaxExtent)
	{
		return Fail(error, "record " + Quoted(record_id) + " span exceeds the address bound");
	}
	const Json& access = raw["access"];
	if (!access.IsString() || (access.AsString() != "read" && access.AsString() != "write"))
	{
		return Fail(error, "record " + Quoted(record_id) + " has an unknown access kind");
	}
	if (!IsIdentifier(raw["resource"]))
	{
		return Fail(error, "span resource must match [A-Za-z0-9_.:-]{1,128} with no surrounding text");
	}
	*span = {raw["resource"].AsString(), offset, offset + size, access.AsString() == "write"};
	return true;
}

bool Overlaps(const Span& a, const Span& b)
{
	return a.resource == b.resource && a.offset < b.end && b.offset < a.end;
}

bool ParseRecord(const Json& raw, size_t ordinal, Record* record, std::string* error)
{
	if (!HasExactKeys(raw, {"id", "kind", "spans"}))
	{
		return Fail(error, "record " + std::to_string(ordinal) + " has missing or unknown keys");
	}
	if (!IsIdentifier(raw["id"]))
	{
		return Fail(error, "record id must match [A-Za-z0-9_.:-]{1,128} with no surrounding text");
	}
	record->id      = raw["id"].AsString();
	record->ordinal = ordinal;
	if (!raw["kind"].IsString() || RecordKinds().count(raw["kind"].AsString()) == 0)
	{
		return Fail(error, "record " + Quoted(record->id) + " has an unknown kind");
	}
	record->kind      = raw["kind"].AsString();
	const Json& spans = raw["spans"];
	if (!spans.IsArray() || spans.Size() < 1 || spans.Size() > kMaxSpansPerRecord)
	{
		return Fail(error, "record " + Quoted(record->id) + " must have 1 to " + std::to_string(kMaxSpansPerRecord) + " spans");
	}
	for (const Json& item: spans.Items())
	{
		Span span;
		if (!ParseSpan(item, record->id, &span, error))
		{
			return false;
		}
		record->spans.push_back(std::move(span));
	}
	// Two writes of one record to the same bytes have no defined final value; read-modify-write is allowed.
	for (size_t i = 0; i < record->spans.size(); ++i)
	{
		for (size_t j = i + 1; j < record->spans.size(); ++j)
		{
			const Span& first  = record->spans[i];
			const Span& second = record->spans[j];
			if (first.write && second.write && Overlaps(first, second))
			{
				return Fail(error, "record " + Quoted(record->id) + " writes the same bytes twice");
			}
		}
	}
	return true;
}

bool ParseRecords(const Json& document, std::vector<Record>* records, std::string* error)
{
	if (!HasExactKeys(document, {"schema", "records"}))
	{
		return Fail(error, "input must contain exactly the keys schema and records");
	}
	if (document["schema"] != Json(kInputSchema))
	{
		return Fail(error, std::string("unsupported input schema; expected ") + kInputSchema);
	}
	const Json& raw_records = document["records"];
	if (!raw_records.IsArray() || raw_records.Size() < 1 || raw_records.Size() > kMaxRecords)
	{
		return Fail(error, "records must be a list of 1 to " + std::to_string(kMaxRecords) + " entries");
	}
	std::set<std::string> ids;
	size_t                total_spans = 0;
	for (size_t ordinal = 0; ordinal < raw_records.Size(); ++ordinal)
	{
		Record record;
		if (!ParseRecord(raw_records[ordinal], ordinal, &record, error))
		{
			return false;
		}
		if (!ids.insert(record.id).second)
		{
			return Fail(error, "duplicate record id " + Quoted(record.id));
		}
		total_spans += record.spans.size();
		if (total_spans > kMaxTotalSpans)
		{
			return Fail(error, "input exceeds " + std::to_string(kMaxTotalSpans) + " total spans");
		}
		records->push_back(std::move(record));
	}
	return true;
}

using Interval = std::pair<int64_t, int64_t>;

// Removes [lo, hi) from the uncovered intervals and returns the overlapped pieces.
std::vector<Interval> Take(std::vector<Interval>* uncovered, int64_t lo, int64_t hi)
{
	std::vector<Interval> remaining;
	std::vector<Interval> hits;
	for (const auto& [start, end]: *uncovered)
	{
		const int64_t hit_lo = std::max(start, lo);
		const int64_t hit_hi = std::min(end, hi);
		if (hit_lo >= hit_hi)
		{
			remaining.emplace_back(start, end);
			continue;
		}
		hits.emplace_back(hit_lo, hit_hi);
		if (start < hit_lo)
		{
			remaining.emplace_back(start, hit_lo);
		}
		if (hit_hi < end)
		{
			remaining.emplace_back(hit_hi, end);
		}
	}
	*uncovered = std::move(remaining);
	return hits;
}

struct Dependency
{
	size_t      reader = 0;
	size_t      writer = 0; // unused for external inputs
	std::string resource;
	int64_t     lo = 0;
	int64_t     hi = 0;
};

// Deduplicated edges and external inputs under one explicit item bound.
class GraphOutput
{
public:
	bool AddEdge(size_t writer, size_t reader, const std::string& resource, int64_t lo, int64_t hi, std::string* error)
	{
		return Add(&m_edges, {reader, writer, resource, lo, hi}, true, error);
	}
	bool AddExternal(size_t reader, const std::string& resource, int64_t lo, int64_t hi, std::string* error)
	{
		return Add(&m_external, {reader, 0, resource, lo, hi}, false, error);
	}

	std::vector<Dependency> SortedEdges() const
	{
		auto edges = m_edges;
		std::stable_sort(edges.begin(), edges.end(), [](const Dependency& a, const Dependency& b) {
			return std::tie(a.reader, a.resource, a.lo, a.writer) < std::tie(b.reader, b.resource, b.lo, b.writer);
		});
		return edges;
	}
	std::vector<Dependency> SortedExternal() const
	{
		auto external = m_external;
		std::stable_sort(external.begin(), external.end(), [](const Dependency& a, const Dependency& b) {
			return std::tie(a.reader, a.resource, a.lo) < std::tie(b.reader, b.resource, b.lo);
		});
		return external;
	}

private:
	bool Add(std::vector<Dependency>* target, Dependency item, bool edge, std::string* error)
	{
		auto key = std::make_tuple(edge, item.writer, item.reader, item.resource, item.lo, item.hi);
		if (m_keys.count(key) != 0)
		{
			return true;
		}
		if (m_edges.size() + m_external.size() >= kMaxOutputItems)
		{
			return Fail(error, "operation graph exceeds " + std::to_string(kMaxOutputItems) + " output items");
		}
		m_keys.insert(std::move(key));
		target->push_back(std::move(item));
		return true;
	}

	std::vector<Dependency>                                                 m_edges;
	std::vector<Dependency>                                                 m_external;
	std::set<std::tuple<bool, size_t, size_t, std::string, int64_t, int64_t>> m_keys;
};

// Links each read span to its nearest earlier writers; leftovers are external inputs.
bool LinkReads(const std::vector<Record>& records, GraphOutput* output, std::string* error)
{
	std::map<std::string, std::vector<std::pair<size_t, const Span*>>> writers_by_resource;
	int64_t                                                             candidates = 0;
	for (const Record& reader: records)
	{
		for (const Span& read: reader.spans)
		{
			if (read.write)
			{
				continue;
			}
			std::vector<Interval> uncovered = {{read.offset, read.end}};
			const auto&           writers   = writers_by_resource[read.resource];
			// Nearest first; only records before this reader are listed.
			for (auto it = writers.rbegin(); it != writers.rend() && !uncovered.empty(); ++it)
			{
				if (++candidates > kMaxCandidateOverlaps)
				{
					return Fail(error, "operation graph exceeds the analysis budget");
				}
				const auto hits = Take(&uncovered, it->second->offset, it->second->end);
				if (uncovered.size() > kMaxUnattributedFragments)
				{
					return Fail(error, "read span fragments into more than " + std::to_string(kMaxUnattributedFragments) + " pieces");
				}
				for (const auto& [lo, hi]: hits)
				{
					if (!output->AddEdge(it->first, reader.ordinal, read.resource, lo, hi, error))
					{
						return false;
					}
				}
			}
			for (const auto& [lo, hi]: uncovered)
			{
				if (!output->AddExternal(reader.ordinal, read.resource, lo, hi, error))
				{
					return false;
				}
			}
		}
		// Publish writes only after this record's reads, so a read-modify-write is not its own writer.
		for (const Span& write: reader.spans)
		{
			if (write.write)
			{
				writers_by_resource[write.resource].emplace_back(reader.ordinal, &write);
			}
		}
	}
	return true;
}

// Traced operations are complete only when nothing was dropped, truncated or left incomplete.
Json Completeness(const Json& trace_meta)
{
	if (trace_meta.IsNull())
	{
		return Json::MakeObject().Set("metadata", "absent");
	}
	const bool complete = !trace_meta["truncated"].AsBool() && trace_meta["operations_dropped"].AsInt() == 0 &&
	                      trace_meta["incomplete_records"].Size() == 0;
	return Json::MakeObject()
	    .Set("metadata", "present")
	    .Set("traced_operations_complete", complete)
	    .Set("truncated", trace_meta["truncated"])
	    .Set("operations_seen", trace_meta["operations_seen"])
	    .Set("operations_dropped", trace_meta["operations_dropped"])
	    .Set("not_traced_kinds", trace_meta["not_traced_kinds"])
	    .Set("incomplete_records", trace_meta["incomplete_records"]);
}

Json Render(const std::vector<Record>& records, const GraphOutput& output, Json completeness)
{
	Json nodes = Json::MakeArray();
	for (const Record& record: records)
	{
		nodes.Push(Json::MakeObject().Set("id", record.id).Set("kind", record.kind).Set("ordinal", static_cast<int64_t>(record.ordinal)));
	}
	Json edges = Json::MakeArray();
	for (const Dependency& edge: output.SortedEdges())
	{
		edges.Push(Json::MakeObject()
		               .Set("from", records[edge.writer].id)
		               .Set("to", records[edge.reader].id)
		               .Set("resource", edge.resource)
		               .Set("offset", edge.lo)
		               .Set("size", edge.hi - edge.lo));
	}
	Json external = Json::MakeArray();
	for (const Dependency& item: output.SortedExternal())
	{
		external.Push(Json::MakeObject()
		                  .Set("to", records[item.reader].id)
		                  .Set("resource", item.resource)
		                  .Set("offset", item.lo)
		                  .Set("size", item.hi - item.lo));
	}
	return Json::MakeObject()
	    .Set("schema", kOutputSchema)
	    .Set("diagnostic_only", true)
	    .Set("synchronization_proof", false)
	    .Set("nodes", std::move(nodes))
	    .Set("edges", std::move(edges))
	    .Set("external_inputs", std::move(external))
	    .Set("completeness", std::move(completeness));
}

bool ParseIncompleteRecords(const Json& raw_list, Json* sorted, std::string* error)
{
	if (!raw_list.IsArray() || raw_list.Size() > kMaxTraceIncomplete)
	{
		return Fail(error, "trace metadata lists more than " + std::to_string(kMaxTraceIncomplete) + " incomplete records");
	}
	std::map<std::string, std::vector<std::string>> by_id;
	for (const Json& raw: raw_list.Items())
	{
		if (!HasExactKeys(raw, {"id", "reasons"}))
		{
			return Fail(error, "incomplete record entry must contain exactly id and reasons");
		}
		if (!IsIdentifier(raw["id"]))
		{
			return Fail(error, "incomplete record id must match [A-Za-z0-9_.:-]{1,128} with no surrounding text");
		}
		const std::string&       id = raw["id"].AsString();
		std::vector<std::string> reasons;
		if (by_id.count(id) != 0 || raw["reasons"].Size() == 0 || !UniqueVocabulary(raw["reasons"], IncompleteReasons(), &reasons))
		{
			return Fail(error, "incomplete record " + Quoted(id) + " has an unknown or repeated reason");
		}
		by_id[id] = std::move(reasons);
	}
	*sorted = Json::MakeArray();
	for (const auto& [id, reasons]: by_id)
	{
		sorted->Push(Json::MakeObject().Set("id", id).Set("reasons", StringArray(reasons)));
	}
	return true;
}

bool IsLowerHexDigest(const Json& value)
{
	return value.IsString() && value.AsString().size() == 32 &&
	       std::all_of(value.AsString().begin(), value.AsString().end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool ReadBounded(const std::string& path, const char* label, std::string* bytes, std::string* error)
{
	std::string read_error;
	if (!ReadTextFile(path, kMaxInputBytes, bytes, &read_error))
	{
		return Fail(error, std::string("cannot read ") + label + ": " + read_error);
	}
	return true;
}

bool ParseBounded(const std::string& bytes, const char* label, Json* document, std::string* error)
{
	std::string parse_error;
	if (!ParseJson(bytes, document, &parse_error))
	{
		return Fail(error, std::string(label) + " is not valid JSON: " + parse_error);
	}
	return true;
}

// Accepts a sidecar only for the exact input bytes it describes; pairing by counts alone is not enough.
bool LoadBoundTraceMeta(const std::string& path, const std::string& input_bytes, Json* trace_meta, std::string* error)
{
	std::string bytes;
	Json        document;
	if (!ReadBounded(path, "trace metadata", &bytes, error) || !ParseBounded(bytes, "trace metadata", &document, error) ||
	    !ParseTraceMeta(document, trace_meta, error))
	{
		return false;
	}
	if (Md5Hex(input_bytes.data(), input_bytes.size()) != (*trace_meta)["input_md5"].AsString())
	{
		return Fail(error, "trace metadata digest does not match the input bytes");
	}
	return true;
}

bool WriteGraphOutputs(const ParsedOptions& options, const Json& graph, std::string* error)
{
	const std::string text = graph.Dump(2) + "\n";
	std::string       write_error;
	if (HasOption(options, "--json"))
	{
		if (!WriteTextFile(OptionValue(options, "--json"), text, &write_error))
		{
			return Fail(error, "cannot write output: " + write_error);
		}
	} else
	{
		std::fputs(text.c_str(), stdout);
	}
	if (HasOption(options, "--dot") && !WriteTextFile(OptionValue(options, "--dot"), OperationGraphDot(graph), &write_error))
	{
		return Fail(error, "cannot write output: " + write_error);
	}
	return true;
}

} // namespace

bool ParseTraceMeta(const Json& document, Json* meta, std::string* error)
{
	if (!HasExactKeys(document, {"schema", "input_md5", "operations_seen", "records_emitted", "operations_dropped", "truncated",
	                             "not_traced_kinds", "incomplete_records"}))
	{
		return Fail(error, "trace metadata must contain exactly the producer keys");
	}
	if (document["schema"] != Json(kTraceMetaSchema))
	{
		return Fail(error, std::string("unsupported trace metadata schema; expected ") + kTraceMetaSchema);
	}
	if (!IsLowerHexDigest(document["input_md5"]))
	{
		return Fail(error, "trace metadata input_md5 must be 32 lowercase hexadecimal digits");
	}
	int64_t seen    = 0;
	int64_t emitted = 0;
	int64_t dropped = 0;
	const std::pair<const char*, int64_t*> counts[] = {{"operations_seen", &seen}, {"records_emitted", &emitted}, {"operations_dropped", &dropped}};
	for (const auto& [key, value]: counts)
	{
		if (!IntegerIn(document[key], 0, int64_t {1} << 32, value))
		{
			return Fail(error, std::string(key) + " must be an integer in [0, 4294967296]");
		}
	}
	if (emitted + dropped != seen)
	{
		return Fail(error, "trace metadata counts do not add up to operations_seen");
	}
	if (!document["truncated"].IsBool())
	{
		return Fail(error, "trace metadata truncated must be a boolean");
	}
	std::vector<std::string> not_traced;
	if (!UniqueVocabulary(document["not_traced_kinds"], NotTracedKinds(), &not_traced))
	{
		return Fail(error, "trace metadata lists an unknown or repeated untraced kind");
	}
	Json incomplete;
	if (!ParseIncompleteRecords(document["incomplete_records"], &incomplete, error))
	{
		return false;
	}
	*meta = Json::MakeObject()
	            .Set("input_md5", document["input_md5"])
	            .Set("operations_seen", seen)
	            .Set("records_emitted", emitted)
	            .Set("operations_dropped", dropped)
	            .Set("truncated", document["truncated"])
	            .Set("not_traced_kinds", StringArray(not_traced))
	            .Set("incomplete_records", std::move(incomplete));
	return true;
}

bool BuildOperationGraph(const Json& document, const Json& trace_meta, Json* graph, std::string* error)
{
	std::vector<Record> records;
	if (!ParseRecords(document, &records, error))
	{
		return false;
	}
	if (!trace_meta.IsNull() && trace_meta["records_emitted"].AsInt() != static_cast<int64_t>(records.size()))
	{
		return Fail(error, "trace metadata does not match the operation records");
	}
	GraphOutput output;
	if (!LinkReads(records, &output, error))
	{
		return false;
	}
	*graph = Render(records, output, Completeness(trace_meta));
	return true;
}

std::string OperationGraphDot(const Json& graph)
{
	const auto range = [](const Json& item) {
		return item["resource"].AsString() + " [" + std::to_string(item["offset"].AsInt()) + ", " +
		       std::to_string(item["offset"].AsInt() + item["size"].AsInt()) + ")";
	};
	std::string           dot = "digraph kyty_operation_graph {\n";
	for (const Json& node: graph["nodes"].Items())
	{
		dot += "  \"op:" + node["id"].AsString() + "\" [label=\"" + node["id"].AsString() + " " + node["kind"].AsString() + "\"];\n";
	}
	std::set<std::string> declared;
	for (const Json& item: graph["external_inputs"].Items())
	{
		const std::string source = "ext:" + item["resource"].AsString();
		if (declared.insert(source).second)
		{
			dot += "  \"" + source + "\" [shape=box];\n";
		}
		dot += "  \"" + source + "\" -> \"op:" + item["to"].AsString() + "\" [label=\"" + range(item) + "\", style=dashed];\n";
	}
	for (const Json& edge: graph["edges"].Items())
	{
		dot += "  \"op:" + edge["from"].AsString() + "\" -> \"op:" + edge["to"].AsString() + "\" [label=\"" + range(edge) + "\"];\n";
	}
	return dot + "}\n";
}

int OperationGraphMain(const std::vector<std::string>& args)
{
	ParsedOptions options;
	std::string   error;
	if (!ParseOptions(args, {}, {"--json", "--dot", "--meta"}, &options, &error) || options.positionals.size() != 1)
	{
		std::fprintf(stderr, "usage: kyty_agent operation-graph INPUT [--json PATH] [--dot PATH] [--meta PATH]\n");
		return 2;
	}
	std::string input_bytes;
	Json        trace_meta;
	Json        document;
	Json        graph;
	const bool  ok = ReadBounded(options.positionals[0], "input", &input_bytes, &error) &&
	                (!HasOption(options, "--meta") || LoadBoundTraceMeta(OptionValue(options, "--meta"), input_bytes, &trace_meta, &error)) &&
	                ParseBounded(input_bytes, "input", &document, &error) && BuildOperationGraph(document, trace_meta, &graph, &error) &&
	                WriteGraphOutputs(options, graph, &error);
	if (!ok)
	{
		std::fprintf(stderr, "error: %s\n", error.c_str());
		return 1;
	}
	return 0;
}

} // namespace Kyty::Agent::Runner
