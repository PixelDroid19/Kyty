#include "Kyty/Agent/Runner/JsonValue.h"
#include "Kyty/Agent/Runner/Md5.h"
#include "Kyty/Agent/Runner/OperationGraph.h"
#include "Kyty/UnitTest.h"

#include "ScopedTestDirectory.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

UT_BEGIN(AgentOperationGraph);

using namespace Kyty::Agent::Runner; // NOLINT(google-build-using-namespace)
namespace fs = std::filesystem;

namespace {

Json SpanOf(const char* resource, int64_t offset, int64_t size, const char* access)
{
	return Json::MakeObject().Set("resource", resource).Set("offset", offset).Set("size", size).Set("access", access);
}

Json RecordOf(const char* id, const char* kind, std::initializer_list<Json> spans)
{
	Json list = Json::MakeArray();
	for (const Json& span: spans)
	{
		list.Push(span);
	}
	return Json::MakeObject().Set("id", id).Set("kind", kind).Set("spans", std::move(list));
}

Json DocumentOf(std::initializer_list<Json> records)
{
	Json list = Json::MakeArray();
	for (const Json& record: records)
	{
		list.Push(record);
	}
	return Json::MakeObject().Set("schema", "kyty_operation_graph_input_v1").Set("records", std::move(list));
}

Json Build(const Json& document, const Json& trace_meta = {})
{
	Json        graph;
	std::string error;
	EXPECT_TRUE(BuildOperationGraph(document, trace_meta, &graph, &error)) << error;
	return graph;
}

bool Rejected(const Json& document)
{
	Json        graph;
	std::string error;
	return !BuildOperationGraph(document, {}, &graph, &error) && !error.empty();
}

using EdgeRow     = std::tuple<std::string, std::string, std::string, int64_t, int64_t>;
using ExternalRow = std::tuple<std::string, std::string, int64_t, int64_t>;

std::vector<EdgeRow> EdgesOf(const Json& graph)
{
	std::vector<EdgeRow> rows;
	for (const Json& edge: graph["edges"].Items())
	{
		rows.emplace_back(edge["from"].AsString(), edge["to"].AsString(), edge["resource"].AsString(), edge["offset"].AsInt(),
		                  edge["size"].AsInt());
	}
	std::sort(rows.begin(), rows.end());
	return rows;
}

std::vector<ExternalRow> ExternalsOf(const Json& graph)
{
	std::vector<ExternalRow> rows;
	for (const Json& item: graph["external_inputs"].Items())
	{
		rows.emplace_back(item["to"].AsString(), item["resource"].AsString(), item["offset"].AsInt(), item["size"].AsInt());
	}
	std::sort(rows.begin(), rows.end());
	return rows;
}

Json CleanMeta(int64_t records)
{
	Json document = Json::MakeObject()
	                    .Set("schema", "kyty_operation_trace_meta_v1")
	                    .Set("input_md5", std::string(32, '0'))
	                    .Set("operations_seen", records)
	                    .Set("records_emitted", records)
	                    .Set("operations_dropped", 0)
	                    .Set("truncated", false)
	                    .Set("not_traced_kinds", Json::MakeArray().Push("indirect_draw").Push("dma"))
	                    .Set("incomplete_records", Json::MakeArray());
	return document;
}

Json ParsedMeta(const Json& document)
{
	Json        meta;
	std::string error;
	EXPECT_TRUE(ParseTraceMeta(document, &meta, &error)) << error;
	return meta;
}

bool MetaRejected(const Json& document)
{
	Json        meta;
	std::string error;
	return !ParseTraceMeta(document, &meta, &error) && !error.empty();
}

void WriteFile(const fs::path& path, const std::string& text)
{
	std::ofstream out(path, std::ios::binary);
	out << text;
}

std::string ReadFile(const fs::path& path)
{
	std::ifstream      in(path, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

} // namespace

TEST(AgentOperationGraphMd5, MatchesReferenceVectors)
{
	EXPECT_EQ(Md5Hex("", 0), "d41d8cd98f00b204e9800998ecf8427e");
	EXPECT_EQ(Md5Hex("abc", 3), "900150983cd24fb0d6963f7d28e17f72");
	// 56..63 bytes need a second padding block.
	const std::string sixty_two = "12345678901234567890123456789012345678901234567890123456789012";
	EXPECT_EQ(Md5Hex(sixty_two.data(), sixty_two.size()), "a29fba1f76305e4754853afb94525918");
	const std::string eighty = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
	EXPECT_EQ(Md5Hex(eighty.data(), eighty.size()), "57edf4a22be3c955ac49da2e2107b67a");
}

TEST(AgentOperationGraph, NearestWriterShadowsOlderBytesAndKeepsPartialWriters)
{
	const Json graph = Build(DocumentOf({RecordOf("w0", "dma", {SpanOf("buf", 0, 64, "write")}),
	                                     RecordOf("w1", "draw", {SpanOf("buf", 32, 64, "write")}),
	                                     RecordOf("r2", "dispatch", {SpanOf("buf", 0, 128, "read")})}));
	EXPECT_EQ(EdgesOf(graph), (std::vector<EdgeRow> {{"w0", "r2", "buf", 0, 32}, {"w1", "r2", "buf", 32, 64}}));
	EXPECT_EQ(ExternalsOf(graph), (std::vector<ExternalRow> {{"r2", "buf", 96, 32}}));
	EXPECT_EQ(graph["schema"].AsString(), "kyty_operation_graph_v1");
	EXPECT_TRUE(graph["diagnostic_only"].AsBool());
	EXPECT_FALSE(graph["synchronization_proof"].AsBool());
	EXPECT_EQ(graph["completeness"]["metadata"].AsString(), "absent");
}

TEST(AgentOperationGraph, OnlyEarlierWritersOfTheSameResourceSatisfyARead)
{
	const Json graph = Build(DocumentOf({RecordOf("w0", "dma", {SpanOf("buf", 0, 8, "write")}),
	                                     RecordOf("w1", "dma", {SpanOf("buf", 0, 8, "write"), SpanOf("buf", 64, 8, "write")}),
	                                     RecordOf("r2", "resolve", {SpanOf("buf", 0, 8, "read"), SpanOf("other", 0, 4, "read")}),
	                                     RecordOf("w3", "draw", {SpanOf("other", 0, 4, "write")})}));
	EXPECT_EQ(EdgesOf(graph), (std::vector<EdgeRow> {{"w1", "r2", "buf", 0, 8}}));
	EXPECT_EQ(ExternalsOf(graph), (std::vector<ExternalRow> {{"r2", "other", 0, 4}}));
}

TEST(AgentOperationGraph, ReadModifyWriteReadsPriorStateThenPublishes)
{
	const Json chained = Build(DocumentOf({RecordOf("w0", "dma", {SpanOf("buf", 0, 8, "write")}),
	                                       RecordOf("rmw1", "draw", {SpanOf("buf", 0, 8, "read"), SpanOf("buf", 0, 8, "write")}),
	                                       RecordOf("r2", "dispatch", {SpanOf("buf", 0, 8, "read")})}));
	EXPECT_EQ(EdgesOf(chained), (std::vector<EdgeRow> {{"rmw1", "r2", "buf", 0, 8}, {"w0", "rmw1", "buf", 0, 8}}));
	EXPECT_TRUE(ExternalsOf(chained).empty());

	const Json alone = Build(DocumentOf({RecordOf("rmw0", "draw", {SpanOf("buf", 0, 8, "read"), SpanOf("buf", 0, 8, "write")})}));
	EXPECT_TRUE(EdgesOf(alone).empty());
	EXPECT_EQ(ExternalsOf(alone), (std::vector<ExternalRow> {{"rmw0", "buf", 0, 8}}));
}

TEST(AgentOperationGraph, DuplicateReadSpansProduceOneEdgePerWriterPiece)
{
	const Json graph = Build(DocumentOf({RecordOf("w0", "dma", {SpanOf("buf", 0, 8, "write")}),
	                                     RecordOf("r1", "draw", {SpanOf("buf", 0, 8, "read"), SpanOf("buf", 0, 8, "read")})}));
	EXPECT_EQ(EdgesOf(graph), (std::vector<EdgeRow> {{"w0", "r1", "buf", 0, 8}}));
}

TEST(AgentOperationGraph, RejectsMalformedRecords)
{
	const Json good = RecordOf("r0", "draw", {SpanOf("buf", 0, 8, "read")});
	EXPECT_FALSE(Rejected(DocumentOf({good})));

	EXPECT_TRUE(Rejected(Json::MakeObject().Set("schema", "kyty_operation_graph_input_v2").Set("records", Json::MakeArray().Push(good))));
	EXPECT_TRUE(Rejected(DocumentOf({good}).Set("extra", 1)));
	EXPECT_TRUE(Rejected(DocumentOf({})));
	EXPECT_TRUE(Rejected(DocumentOf({good, good})));
	EXPECT_TRUE(Rejected(DocumentOf({RecordOf("r0", "copy", {SpanOf("buf", 0, 8, "read")})})));
	EXPECT_TRUE(Rejected(DocumentOf({RecordOf("r0", "draw", {SpanOf("buf", 0, 8, "modify")})})));
	EXPECT_TRUE(Rejected(DocumentOf({RecordOf("r0", "draw", {SpanOf("buf", 0, 0, "read")})})));
	EXPECT_TRUE(Rejected(DocumentOf({RecordOf("r0", "draw", {SpanOf("buf", -1, 8, "read")})})));
	EXPECT_TRUE(Rejected(DocumentOf({RecordOf("r0", "draw", {SpanOf("buf", int64_t {1} << 48, 1, "read")})})));
	EXPECT_TRUE(Rejected(DocumentOf({RecordOf("r0", "draw", {SpanOf("buf", (int64_t {1} << 48) - 4, 8, "read")})})));
	EXPECT_TRUE(Rejected(DocumentOf({RecordOf("r0", "draw", {})})));
	EXPECT_TRUE(Rejected(DocumentOf({RecordOf("r0\n", "draw", {SpanOf("buf", 0, 8, "read")})})));
	EXPECT_TRUE(Rejected(DocumentOf({RecordOf("r0", "draw", {SpanOf("buf space", 0, 8, "read")})})));
	EXPECT_TRUE(Rejected(DocumentOf({RecordOf("w0", "draw", {SpanOf("b", 0, 8, "write"), SpanOf("b", 4, 8, "write")})})));

	Json float_size = SpanOf("buf", 0, 8, "read");
	float_size.Set("size", 8.0);
	EXPECT_TRUE(Rejected(DocumentOf({RecordOf("r0", "draw", {float_size})})));
	Json bool_offset = SpanOf("buf", 0, 8, "read");
	bool_offset.Set("offset", false);
	EXPECT_TRUE(Rejected(DocumentOf({RecordOf("r0", "draw", {bool_offset})})));
}

TEST(AgentOperationGraph, CompletenessFollowsTheTraceMetadata)
{
	const Json document = DocumentOf({RecordOf("w0", "dma", {SpanOf("buf", 0, 8, "write")})});

	const Json clean = Build(document, ParsedMeta(CleanMeta(1)))["completeness"];
	EXPECT_EQ(clean["metadata"].AsString(), "present");
	EXPECT_TRUE(clean["traced_operations_complete"].AsBool());
	EXPECT_EQ(clean["not_traced_kinds"], Json::MakeArray().Push("dma").Push("indirect_draw"));

	Json dropped = CleanMeta(1);
	dropped.Set("operations_seen", 2).Set("operations_dropped", 1);
	EXPECT_FALSE(Build(document, ParsedMeta(dropped))["completeness"]["traced_operations_complete"].AsBool());

	Json truncated = CleanMeta(1);
	truncated.Set("truncated", true);
	EXPECT_FALSE(Build(document, ParsedMeta(truncated))["completeness"]["traced_operations_complete"].AsBool());

	Json incomplete = CleanMeta(1);
	incomplete.Set("incomplete_records", Json::MakeArray().Push(Json::MakeObject().Set("id", "w0").Set(
	                                         "reasons", Json::MakeArray().Push("span_limit_reached").Push("invalid_span"))));
	const Json listed = Build(document, ParsedMeta(incomplete))["completeness"];
	EXPECT_FALSE(listed["traced_operations_complete"].AsBool());
	EXPECT_EQ(listed["incomplete_records"][0]["reasons"], Json::MakeArray().Push("invalid_span").Push("span_limit_reached"));

	Json        graph;
	std::string error;
	EXPECT_FALSE(BuildOperationGraph(document, ParsedMeta(CleanMeta(2)), &graph, &error));
}

TEST(AgentOperationGraph, TraceMetadataSchemaIsStrict)
{
	EXPECT_FALSE(MetaRejected(CleanMeta(1)));
	EXPECT_TRUE(MetaRejected(CleanMeta(1).Set("extra", true)));
	EXPECT_TRUE(MetaRejected(CleanMeta(1).Set("input_md5", std::string(32, 'A'))));
	EXPECT_TRUE(MetaRejected(CleanMeta(1).Set("input_md5", std::string(31, '0'))));
	EXPECT_TRUE(MetaRejected(CleanMeta(1).Set("operations_seen", 3)));
	EXPECT_TRUE(MetaRejected(CleanMeta(1).Set("truncated", 0)));
	EXPECT_TRUE(MetaRejected(CleanMeta(1).Set("not_traced_kinds", Json::MakeArray().Push("dma").Push("dma"))));
	EXPECT_TRUE(MetaRejected(CleanMeta(1).Set("not_traced_kinds", Json::MakeArray().Push("draw"))));
	const auto with_incomplete = [](Json reasons) {
		return CleanMeta(1).Set("incomplete_records",
		                        Json::MakeArray().Push(Json::MakeObject().Set("id", "w0").Set("reasons", std::move(reasons))));
	};
	EXPECT_TRUE(MetaRejected(with_incomplete(Json::MakeArray())));
	EXPECT_TRUE(MetaRejected(with_incomplete(Json::MakeArray().Push("not_a_reason"))));
	EXPECT_TRUE(MetaRejected(with_incomplete(Json::MakeArray().Push("invalid_span").Push("invalid_span"))));
}

TEST(AgentOperationGraph, DotKeepsOperationAndExternalNamesApart)
{
	// "ext:buf" is a legal operation id; it must not merge with the generated external source.
	const Json graph = Build(DocumentOf({RecordOf("ext:buf", "dma", {SpanOf("buf", 0, 8, "write")}),
	                                     RecordOf("r1", "draw", {SpanOf("buf", 0, 16, "read")})}));
	const std::string dot = OperationGraphDot(graph);
	EXPECT_EQ(dot.rfind("digraph kyty_operation_graph {\n", 0), 0u);
	EXPECT_NE(dot.find("\"op:ext:buf\" -> \"op:r1\" [label=\"buf [0, 8)\"];"), std::string::npos);
	EXPECT_NE(dot.find("\"ext:buf\" -> \"op:r1\" [label=\"buf [8, 16)\", style=dashed];"), std::string::npos);
}

TEST(AgentOperationGraph, CliBindsTheSidecarToTheExactInputBytes)
{
	const ScopedTestDirectory directory("agent-operation-graph");
	ASSERT_FALSE(directory.Path().empty());
	const fs::path input = directory.Path() / "ops.json";
	const fs::path meta  = directory.Path() / "meta.json";
	const fs::path out   = directory.Path() / "graph.json";
	const fs::path dot   = directory.Path() / "graph.dot";

	const std::string bytes =
	    DocumentOf({RecordOf("w0", "dma", {SpanOf("buf", 0, 8, "write")}), RecordOf("r1", "draw", {SpanOf("buf", 0, 8, "read")})}).Dump();
	WriteFile(input, bytes);
	WriteFile(meta, CleanMeta(2).Set("input_md5", Md5Hex(bytes.data(), bytes.size())).Dump());
	ASSERT_EQ(OperationGraphMain({input.string(), "--meta", meta.string(), "--json", out.string(), "--dot", dot.string()}), 0);
	Json        graph;
	std::string error;
	ASSERT_TRUE(ParseJson(ReadFile(out), &graph, &error)) << error;
	EXPECT_EQ(EdgesOf(graph), (std::vector<EdgeRow> {{"w0", "r1", "buf", 0, 8}}));
	EXPECT_TRUE(graph["completeness"]["traced_operations_complete"].AsBool());
	EXPECT_NE(ReadFile(dot).find("\"op:w0\" -> \"op:r1\""), std::string::npos);

	// Same records, different bytes: the sidecar no longer describes this input.
	WriteFile(input, bytes + "\n");
	EXPECT_EQ(OperationGraphMain({input.string(), "--meta", meta.string(), "--json", out.string()}), 1);

	WriteFile(input, "{\"schema\":\"kyty_operation_graph_input_v1\",\"schema\":\"x\",\"records\":[]}");
	EXPECT_EQ(OperationGraphMain({input.string(), "--json", out.string()}), 1);
	EXPECT_EQ(OperationGraphMain({(directory.Path() / "missing.json").string()}), 1);
	EXPECT_EQ(OperationGraphMain({}), 2);
}

UT_END();
