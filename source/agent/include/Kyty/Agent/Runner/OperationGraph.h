#ifndef KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_OPERATIONGRAPH_H_
#define KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_OPERATIONGRAPH_H_

#include "Kyty/Agent/Runner/JsonValue.h"

#include <string>
#include <vector>

namespace Kyty::Agent::Runner {

// Offline dependency graph over graphics operation records
// (kyty_operation_graph_input_v1 -> kyty_operation_graph_v1).
//
// Each draw, dispatch, DMA or resolve record lists explicit resource spans. A
// read span is linked to the nearest preceding record whose write span overlaps
// each still-unattributed byte, so partially overlapping writers all appear.
// Bytes with no earlier writer are external inputs. A record's reads observe the
// state before that record, so a read-modify-write span both reads from earlier
// writers and publishes its write for later records.
//
// The graph is diagnostic evidence about trace order. It is not proof of
// hardware synchronization and must not justify removing a barrier or wait.

// Validates the producer's kyty_operation_trace_meta_v1 sidecar and returns it
// normalized (sorted kinds, incomplete records and reasons).
[[nodiscard]] bool ParseTraceMeta(const Json& document, Json* meta, std::string* error);

// `trace_meta` is a ParseTraceMeta result already bound to the input bytes, or null.
[[nodiscard]] bool BuildOperationGraph(const Json& document, const Json& trace_meta, Json* graph, std::string* error);

// Graphviz rendering; operation and external source names never collide.
[[nodiscard]] std::string OperationGraphDot(const Json& graph);

// `kyty_agent operation-graph INPUT [--json PATH] [--dot PATH] [--meta PATH]`
[[nodiscard]] int OperationGraphMain(const std::vector<std::string>& args);

} // namespace Kyty::Agent::Runner

#endif /* KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_OPERATIONGRAPH_H_ */
