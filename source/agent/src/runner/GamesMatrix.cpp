#include "Kyty/Agent/Runner/GamesMatrix.h"

#include "Kyty/Agent/Runner/Sha256.h"

#include "RunnerSupport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <tuple>

namespace Kyty::Agent::Runner {
namespace {

namespace fs = std::filesystem;

constexpr const char* kSummarySchema  = "kyty_games_matrix_summary_v1";
constexpr const char* kDiscoverSchema = "kyty_games_matrix_discover_v1";
constexpr const char* kPrimaryName    = "eboot.bin";
constexpr size_t      kMaxLogBytes    = 256u * 1024u * 1024u;

const Environment& DiagnosticBringupEnvironment()
{
	static const Environment env = {
	    {"KYTY_BRINGUP_MODE", "unsafe"},
	    {"KYTY_BRINGUP_ALLOW_DIAGNOSTIC", "1"},
	    {"KYTY_BRINGUP_FEATURES", "not_implemented,missing_function_import,gfx_permissive,adjacent_module_discovery"},
	};
	return env;
}

bool OneOf(const std::string& value, std::initializer_list<const char*> options)
{
	return std::any_of(options.begin(), options.end(), [&](const char* option) { return value == option; });
}

// Python-style str(value or "").
std::string TextOf(const Json& value)
{
	return value.Truthy() ? ValueText(value) : std::string();
}

// First non-empty label; placeholder frontiers such as "none" do not count.
std::string UsableLabel(std::initializer_list<std::string> parts)
{
	for (const std::string& part: parts)
	{
		const std::string label = Strip(part);
		if (!OneOf(Lower(label), {"", "none", "null", "unknown", "n/a", "na"}))
		{
			return label;
		}
	}
	return {};
}

std::string EnvOr(const char* key, const std::string& fallback)
{
	const char* value = std::getenv(key);
	return value != nullptr && *value != '\0' ? std::string(value) : fallback;
}

std::string Resolved(const fs::path& path)
{
	std::error_code ec;
	const fs::path  canonical = fs::weakly_canonical(path, ec);
	return ec ? fs::absolute(path, ec).lexically_normal().string() : canonical.string();
}

bool IsSkippedDirectory(const std::string& name)
{
	return name.empty() || name[0] == '.' || name == "_scratch" || name == "node_modules";
}

// Directory walk that, like a non-following top-down walk, lists symlinked
// directories but never descends into them.
void WalkForPrimaries(const fs::path& directory, int depth, int max_depth, std::set<std::string>* found)
{
	if (depth > max_depth)
	{
		return;
	}
	std::error_code          ec;
	std::vector<fs::path>    subdirectories;
	bool                     has_primary = false;
	for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
	{
		const std::string name = it->path().filename().string();
		std::error_code   type_ec;
		if (!it->is_directory(type_ec))
		{
			has_primary = has_primary || name == kPrimaryName;
			continue;
		}
		if (!IsSkippedDirectory(name) && !it->is_symlink(type_ec))
		{
			subdirectories.push_back(it->path());
		}
	}
	if (has_primary)
	{
		found->insert(Resolved(directory));
	}
	for (const fs::path& subdirectory: subdirectories)
	{
		const std::string name = subdirectory.filename().string();
		// Package bulk never holds another package.
		if (has_primary && OneOf(name, {"sce_module", "modules", "Media", "sce_sys"}))
		{
			continue;
		}
		WalkForPrimaries(subdirectory, depth + 1, max_depth, found);
	}
}

std::vector<std::string> SortedCaseInsensitive(std::vector<std::string> paths)
{
	std::stable_sort(paths.begin(), paths.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
	return paths;
}

bool IsWordByte(unsigned char c)
{
	return std::isalnum(c) != 0 || c == '_' || c >= 0x80;
}

bool EndsPath(unsigned char c, bool windows)
{
	return std::isspace(c) != 0 || (c >= 0x1c && c <= 0x1f) || c == '"' || c == '\'' || c == '<' || c == '>' || (windows && c == '|');
}

// Length of a path starting at `at`, or 0. Windows paths need "X:\" or "X:/".
size_t PathLength(const std::string& text, size_t at, bool windows)
{
	size_t body = at + 1;
	if (windows)
	{
		if (at + 2 >= text.size() || std::isalpha(static_cast<unsigned char>(text[at])) == 0 || text[at + 1] != ':' ||
		    (text[at + 2] != '\\' && text[at + 2] != '/'))
		{
			return 0;
		}
		body = at + 3;
	} else if (text[at] != '/')
	{
		return 0;
	}
	size_t end = body;
	while (end < text.size() && !EndsPath(static_cast<unsigned char>(text[end]), windows))
	{
		++end;
	}
	return end > body ? end - at : 0;
}

// Length of a whole-word PPSA/CUSA identifier starting at `at`, or 0.
size_t TitleIdLength(const std::string& text, size_t at)
{
	if (at + 4 >= text.size())
	{
		return 0;
	}
	const std::string prefix = Lower(text.substr(at, 4));
	if (prefix != "ppsa" && prefix != "cusa")
	{
		return 0;
	}
	size_t end = at + 4;
	while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])) != 0)
	{
		++end;
	}
	if (end == at + 4 || (end < text.size() && IsWordByte(static_cast<unsigned char>(text[end]))))
	{
		return 0;
	}
	return end - at;
}

template <typename Matcher>
std::string ReplaceWords(const std::string& text, const char* replacement, Matcher match_length)
{
	std::string out;
	size_t      at = 0;
	while (at < text.size())
	{
		const bool   word_start = at == 0 || !IsWordByte(static_cast<unsigned char>(text[at - 1]));
		const size_t length     = word_start ? match_length(at) : 0;
		if (length == 0)
		{
			out += text[at++];
			continue;
		}
		out += replacement;
		at += length;
	}
	return out;
}

int64_t ProgressRank(const std::string& label)
{
	static const std::map<std::string, int64_t> ranks = {{"launch", 0},     {"runtime_started", 1}, {"video_initialized", 2},
	                                                     {"presenting", 3}, {"interactive", 4},     {"unsupported", 5}};
	const auto                                  it    = ranks.find(label);
	return it == ranks.end() ? 0 : it->second;
}

std::string ToHex(uint64_t value)
{
	char buffer[17];
	std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(value));
	return buffer;
}

std::string NewRunId()
{
	const auto  ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	std::random_device random;
	std::string        nonce;
	while (nonce.size() < 12)
	{
		nonce += ToHex(random() & 0xfu);
	}
	return UtcTime("%Y%m%dT%H%M%SZ") + "_" + ToHex(static_cast<uint64_t>(ns)) + "_" + nonce;
}

std::string MatrixSocketPath(const std::string& run_id, const std::string& case_id)
{
	const std::string identity = run_id + ":" + case_id;
	return "/tmp/kyty_mx_" + Sha256Hex(identity.data(), identity.size()).substr(0, 20) + ".sock";
}

MatrixCase FailedLaunch(const std::string& case_id, const std::string& frontier, bool attempted, std::vector<std::string> notes)
{
	MatrixCase result;
	result.case_id        = case_id;
	result.outcome        = "launch_failed";
	result.first_frontier = frontier;
	result.valid_package  = true;
	result.attempted      = attempted;
	result.notes          = std::move(notes);
	return result;
}

MatrixCase InvalidFixture(const std::string& case_id, const std::string& reason)
{
	MatrixCase result;
	result.case_id        = case_id;
	result.outcome        = "invalid_fixture";
	result.first_frontier = reason;
	result.notes          = {reason};
	return result;
}

// Observation loop of one launched case; the child is still owned by the caller.
class MatrixCaseRun
{
public:
	MatrixCaseRun(RunnerHost* host, const MatrixCaseOptions& options, std::string socket)
	    : m_host(host), m_options(options), m_socket(std::move(socket))
	{
	}

	MatrixCase Run()
	{
		const double started        = m_host->Now();
		const double total_deadline = started + m_options.total_deadline_s;
		Observe(started, total_deadline);
		Finish();
		return Classify();
	}

private:
	void Observe(double started, double total_deadline)
	{
		const double startup_deadline = started + m_options.startup_deadline_s;
		const double present_deadline = started + m_options.present_deadline_s;
		while (m_host->Now() < total_deadline)
		{
			if (m_host->PollChild().has_value())
			{
				return;
			}
			const double now = m_host->Now();
			if (!m_agent_ready)
			{
				if (now > startup_deadline)
				{
					m_notes.emplace_back("startup_deadline");
					m_timed_out = true;
					return;
				}
				if (m_host->CallAgent(m_socket, "ping", Json::MakeObject(), 1.0).code == 0)
				{
					m_agent_ready   = true;
					m_best_progress = "runtime_started";
					m_notes.emplace_back("agent_ready");
				}
				m_host->Sleep(0.25);
				continue;
			}
			if (ObserveStatus(now) || ObserveLastError())
			{
				return;
			}
			if (now > present_deadline && OneOf(m_best_progress, {"runtime_started", "video_initialized", "launch"}))
			{
				if (!m_present_deadline_noted)
				{
					m_notes.emplace_back("present_deadline");
					m_present_deadline_noted = true;
				}
				// Video or present progress keeps the case running until the total deadline.
				if (m_best_progress == "runtime_started")
				{
					m_timed_out = true;
					return;
				}
			}
			m_host->Sleep(0.5);
		}
		m_timed_out = true;
		m_notes.emplace_back("total_deadline");
	}

	// True when the interactive phase held long enough to stop the bounded case.
	bool ObserveStatus(double now)
	{
		const AgentReply reply = m_host->CallAgent(m_socket, "status", Json::MakeObject(), 2.0);
		if (reply.code != 0)
		{
			return false;
		}
		m_status                   = ExtractResult(reply.body);
		const std::string phase    = TextOf(m_status["phase"]);
		const std::string frontier = TextOf(m_status["frontier"]);
		if (m_status["graphic_ready"].Truthy() && OneOf(m_best_progress, {"launch", "runtime_started"}))
		{
			m_best_progress = "video_initialized";
		}
		if (m_status["present"].AsInt() > 0)
		{
			m_best_progress = "presenting";
			if (!m_presenting_since.has_value())
			{
				m_presenting_since = now;
			}
		}
		if (phase != "interactive" && frontier != "interactive")
		{
			return false;
		}
		m_best_progress = "interactive";
		if (m_presenting_since.has_value() && now - *m_presenting_since >= m_options.stability_s)
		{
			m_notes.emplace_back("stability_reached");
			return true;
		}
		return false;
	}

	// True when the runtime reported a bring-up halt.
	bool ObserveLastError()
	{
		const AgentReply reply = m_host->CallAgent(m_socket, "last_error", Json::MakeObject(), 1.0);
		if (reply.code != 0)
		{
			return false;
		}
		const Json& event = ExtractResult(reply.body)["event"];
		if (!event.IsObject() || !event["code"].Truthy())
		{
			return false;
		}
		m_last_error_code = ValueText(event["code"]);
		if (!OneOf(m_last_error_code, {"bringup_halt", "bringup_breaker"}))
		{
			return false;
		}
		m_best_progress = "unsupported";
		m_notes.emplace_back("unsupported_from_last_error");
		return true;
	}

	void Finish()
	{
		// One last observation while the owned socket is still live.
		if (m_agent_ready)
		{
			const AgentReply reply = m_host->CallAgent(m_socket, "status", Json::MakeObject(), 1.0);
			if (reply.code == 0)
			{
				m_status = ExtractResult(reply.body);
			}
		}
		if (!m_host->PollChild().has_value())
		{
			m_host->TerminateChild(&m_notes);
			m_runner_stopped = true;
		}
		m_exit_code = m_host->PollChild();
		std::vector<std::string> cleanup;
		RemoveStaleUnixSocket(m_socket, &cleanup);
		std::copy_if(cleanup.begin(), cleanup.end(), std::back_inserter(m_notes),
		             [](const std::string& note) { return note != "removed_stale_socket"; });
	}

	MatrixCase Classify()
	{
		auto [outcome, first_frontier] = MapMatrixOutcome(m_exit_code, m_agent_ready, m_status, m_last_error_code, m_timed_out, m_runner_stopped);
		const bool failed              = OneOf(outcome, {"host_crash", "unsupported", "launch_failed", "loader_failed"});
		// Prefer the best observed progress when it is further than the mapped default.
		if (ProgressRank(m_best_progress) >= ProgressRank(first_frontier) && m_best_progress != "launch" && !failed)
		{
			first_frontier = m_best_progress;
		}
		// One actionable host or assert label beats a generic token.
		if (failed)
		{
			const std::string log_label = MatrixActionableFromLog((fs::path(m_options.case_dir) / "child.log").string());
			if (!log_label.empty() &&
			    (outcome == "host_crash" || OneOf(first_frontier, {"", "host_crash", "loader_failed", "launch", "post_agent_exit"})))
			{
				first_frontier = log_label;
			}
		}
		bool progressed = OneOf(outcome, {"runtime_started", "video_initialized", "presenting", "interactive", "controlled_timeout", "guest_exit"}) ||
		                  OneOf(m_best_progress, {"runtime_started", "video_initialized", "presenting", "interactive"});
		if (m_agent_ready && outcome == "launch_failed")
		{
			outcome        = "runtime_started";
			first_frontier = first_frontier.empty() ? "runtime_started" : first_frontier;
			progressed     = true;
		}
		MatrixCase result;
		result.case_id        = m_options.case_id;
		result.outcome        = outcome;
		result.first_frontier = first_frontier.empty() ? m_best_progress : first_frontier;
		result.valid_package  = true;
		result.attempted      = true;
		result.progressed     = progressed;
		result.child_exit     = m_exit_code;
		result.agent_phase    = TextOf(m_status["phase"]);
		result.agent_frontier = TextOf(m_status["frontier"]);
		result.present        = m_status["present"].AsInt();
		result.notes          = m_notes;
		return result;
	}

	RunnerHost*              m_host;
	const MatrixCaseOptions& m_options;
	std::string              m_socket;
	std::vector<std::string> m_notes;
	bool                     m_agent_ready = false;
	bool                     m_timed_out   = false;
	bool                     m_runner_stopped = false;
	bool                     m_present_deadline_noted = false;
	Json                     m_status                 = Json::MakeObject();
	std::string              m_last_error_code;
	std::string              m_best_progress = "launch";
	std::optional<double>    m_presenting_since;
	std::optional<int>       m_exit_code;
};

Json CaseList(const std::vector<MatrixCase>& cases)
{
	Json list = Json::MakeArray();
	for (const MatrixCase& item: cases)
	{
		list.Push(item.ToJson());
	}
	return list;
}

Json CountByOutcome(const std::vector<MatrixCase>& cases)
{
	std::map<std::string, int64_t> counts;
	for (const MatrixCase& item: cases)
	{
		++counts[item.outcome];
	}
	Json by = Json::MakeObject();
	for (const auto& [outcome, count]: counts)
	{
		by.Set(outcome, count);
	}
	return by;
}

std::vector<MatrixCase> Filter(const std::vector<MatrixCase>& cases, bool progressed)
{
	std::vector<MatrixCase> out;
	std::copy_if(cases.begin(), cases.end(), std::back_inserter(out), [&](const MatrixCase& item) { return item.progressed == progressed; });
	return out;
}

struct MatrixArgs
{
	std::string games_root;
	std::string repo_root;
	std::string scratch;
	std::string fc_script;
	std::string mode = "strict";
	bool        discover_only = false;
	int64_t     max_depth     = 6;
	int64_t     max_cases     = 0;
	double      startup_s     = 45.0;
	double      present_s     = 90.0;
	double      total_s       = 120.0;
	double      stability_s   = 10.0;
};

bool ParseMatrixArgs(const std::vector<std::string>& args, MatrixArgs* out, std::string* error)
{
	ParsedOptions options;
	if (!ParseOptions(args, {"--discover-only"},
	                  {"--games-root", "--scratch", "--fc-script", "--repo-root", "--mode", "--max-depth", "--startup-deadline-s",
	                   "--present-deadline-s", "--total-deadline-s", "--stability-s", "--max-cases"},
	                  &options, error) ||
	    !options.positionals.empty())
	{
		*error = error->empty() ? "unexpected argument" : *error;
		return false;
	}
	std::error_code ec;
	out->repo_root     = ResolvePath(OptionValue(options, "--repo-root", fs::current_path(ec).string()));
	out->games_root    = OptionValue(options, "--games-root", EnvOr("KYTY_GAMES_ROOT", ""));
	out->scratch       = OptionValue(options, "--scratch", EnvOr("KYTY_MATRIX_SCRATCH", (fs::path(out->repo_root) / "_matrix_scratch").string()));
	out->fc_script     = ResolvePath(OptionValue(options, "--fc-script", "_build_linux/fc_script"), out->repo_root);
	out->mode          = OptionValue(options, "--mode", "strict");
	out->discover_only = HasOption(options, "--discover-only");
	if (out->mode != "strict" && out->mode != "bringup")
	{
		*error = "--mode must be strict or bringup";
		return false;
	}
	const std::pair<const char*, int64_t*> integers[] = {{"--max-depth", &out->max_depth}, {"--max-cases", &out->max_cases}};
	for (const auto& [name, value]: integers)
	{
		if (HasOption(options, name) && !ParseInt64(OptionValue(options, name), value))
		{
			*error = std::string(name) + " must be an integer";
			return false;
		}
	}
	const std::tuple<const char*, const char*, double*, const char*> deadlines[] = {
	    {"--startup-deadline-s", "KYTY_MATRIX_STARTUP_S", &out->startup_s, "45"},
	    {"--present-deadline-s", "KYTY_MATRIX_PRESENT_S", &out->present_s, "90"},
	    {"--total-deadline-s", "KYTY_MATRIX_TOTAL_S", &out->total_s, "120"},
	    {"--stability-s", "KYTY_MATRIX_STABILITY_S", &out->stability_s, "10"},
	};
	for (const auto& [name, env_key, value, fallback]: deadlines)
	{
		if (!ParseDouble(OptionValue(options, name, EnvOr(env_key, fallback)), value))
		{
			*error = std::string(name) + " must be a number";
			return false;
		}
	}
	return true;
}

void WriteSanitized(const fs::path& path, const Json& payload)
{
	std::string error;
	if (!WriteTextFile(path.string(), SanitizeMatrixJson(payload).Dump(2) + "\n", &error))
	{
		std::fprintf(stderr, "kyty_agent: error: %s\n", error.c_str());
	}
}

Json DiscoverOnlySummary(const MatrixArgs& args, const std::string& run_id, const std::vector<MatrixCase>& invalid,
                         const std::vector<std::string>& pending_ids)
{
	Json pending   = Json::MakeArray();
	Json all_cases = CaseList(invalid);
	Json ids       = Json::MakeArray();
	for (const std::string& id: pending_ids)
	{
		const Json item = Json::MakeObject()
		                      .Set("case_id", id)
		                      .Set("valid_package", true)
		                      .Set("attempted", false)
		                      .Set("progressed", false)
		                      .Set("first_frontier", "discover_only")
		                      .Set("notes", Json::MakeArray().Push("discover_only_pending"));
		pending.Push(item);
		all_cases.Push(item);
		ids.Push(id);
	}
	const auto valid  = static_cast<int64_t>(pending_ids.size());
	const auto failed = Filter(invalid, false);
	return Json::MakeObject()
	    .Set("schema", kSummarySchema)
	    .Set("mode", args.mode)
	    .Set("protocol", "kyty_agent")
	    .Set("meta", Json::MakeObject().Set("discover_only", true).Set("valid_count", valid).Set("pending_case_ids", ids).Set("run_id", run_id))
	    .Set("counts", Json::MakeObject()
	                       .Set("total", static_cast<int64_t>(invalid.size()) + valid)
	                       .Set("valid_packages", valid)
	                       .Set("attempted", 0)
	                       .Set("failed_before_runtime", static_cast<int64_t>(failed.size()))
	                       .Set("progressed", 0)
	                       .Set("pending_discover_only", valid)
	                       .Set("by_outcome", CountByOutcome(invalid)))
	    .Set("failed_before_runtime", CaseList(failed))
	    .Set("progressed", Json::MakeArray())
	    .Set("pending", std::move(pending))
	    .Set("all_cases", std::move(all_cases));
}

Json RunSummary(const MatrixArgs& args, const std::string& run_id, const std::vector<MatrixCase>& results, int64_t valid_count)
{
	const auto before   = Filter(results, false);
	const auto after    = Filter(results, true);
	const auto valid    = std::count_if(results.begin(), results.end(), [](const MatrixCase& item) { return item.valid_package; });
	const auto attempts = std::count_if(results.begin(), results.end(), [](const MatrixCase& item) { return item.attempted; });
	return Json::MakeObject()
	    .Set("schema", kSummarySchema)
	    .Set("mode", args.mode)
	    .Set("protocol", "kyty_agent")
	    .Set("meta", Json::MakeObject()
	                     .Set("discover_only", false)
	                     .Set("valid_count", valid_count)
	                     .Set("run_id", run_id)
	                     .Set("startup_deadline_s", args.startup_s)
	                     .Set("present_deadline_s", args.present_s)
	                     .Set("total_deadline_s", args.total_s)
	                     .Set("strict", args.mode == "strict"))
	    .Set("counts", Json::MakeObject()
	                       .Set("total", static_cast<int64_t>(results.size()))
	                       .Set("valid_packages", static_cast<int64_t>(valid))
	                       .Set("attempted", static_cast<int64_t>(attempts))
	                       .Set("failed_before_runtime", static_cast<int64_t>(before.size()))
	                       .Set("progressed", static_cast<int64_t>(after.size()))
	                       .Set("by_outcome", CountByOutcome(results)))
	    .Set("failed_before_runtime", CaseList(before))
	    .Set("progressed", CaseList(after))
	    .Set("all_cases", CaseList(results));
}

} // namespace

Json MatrixCase::ToJson() const
{
	Json note_list = Json::MakeArray();
	for (const std::string& note: notes)
	{
		note_list.Push(note);
	}
	return Json::MakeObject()
	    .Set("case_id", case_id)
	    .Set("outcome", outcome)
	    .Set("first_frontier", first_frontier)
	    .Set("valid_package", valid_package)
	    .Set("attempted", attempted)
	    .Set("progressed", progressed)
	    .Set("child_exit", child_exit.has_value() ? Json(*child_exit) : Json())
	    .Set("agent_phase", agent_phase)
	    .Set("agent_frontier", agent_frontier)
	    .Set("present", present)
	    .Set("notes", std::move(note_list));
}

std::vector<std::string> DiscoverGameRoots(const std::string& games_root, int max_depth)
{
	std::error_code ec;
	const fs::path  root = Resolved(games_root);
	if (!fs::is_directory(root, ec))
	{
		return {};
	}
	std::set<std::string> found;
	WalkForPrimaries(root, 0, max_depth, &found);
	return SortedCaseInsensitive({found.begin(), found.end()});
}

std::string InvalidPackageReason(const std::string& root)
{
	std::error_code ec;
	if (!fs::is_directory(root, ec))
	{
		return "not_a_directory";
	}
	return fs::is_regular_file(fs::path(root) / kPrimaryName, ec) ? std::string() : "missing_primary_executable";
}

std::string AnonymousCaseId(const std::string& games_root, const std::string& package_root)
{
	const fs::path root     = Resolved(games_root);
	const fs::path package  = Resolved(package_root);
	const fs::path relative = package.lexically_relative(root);
	const bool     inside   = !relative.empty() && *relative.begin() != "..";
	const std::string key   = inside ? relative.generic_string() : package.filename().string();
	return "case_" + Sha256Hex(key.data(), key.size()).substr(0, 12);
}

std::pair<std::string, std::string> MapMatrixOutcome(std::optional<int> process_exit, bool agent_ready, const Json& status,
                                                     const std::string& last_error_code, bool timed_out, bool runner_stopped)
{
	const std::string phase    = TextOf(status["phase"]);
	const std::string frontier = UsableLabel({TextOf(status["frontier"])});
	const int64_t     present  = status["present"].AsInt();
	const bool        graphic  = status["graphic_ready"].Truthy();
	const std::string error    = UsableLabel({last_error_code});

	if (OneOf(error, {"bringup_halt", "bringup_breaker", "unsupported"}) || frontier == "unsupported")
	{
		return {"unsupported", UsableLabel({frontier, error, "unsupported"})};
	}
	// Signal-style exits are host crashes. Exit 65 is also used by strict
	// unsupported paths and therefore requires explicit crash evidence.
	const bool crash_exit = process_exit.has_value() && std::set<int> {139, 132, 134, 136, 321}.count(*process_exit) != 0;
	if (crash_exit || OneOf(error, {"host_crash", "host_fault"}))
	{
		return {"host_crash", UsableLabel({error, frontier, "host_crash"})};
	}
	// A deadline or a stable interactive phase ends with the runner terminating
	// the child's process group. That signal exit after the agent was ready is
	// the runner's own stop, classified by the observed frontier below.
	if (process_exit.has_value() && *process_exit != 0 && !((timed_out || runner_stopped) && agent_ready))
	{
		if (!agent_ready)
		{
			return {"launch_failed", "launch"};
		}
		return {"loader_failed", UsableLabel({frontier, error, "post_agent_exit"})};
	}
	if (!agent_ready)
	{
		if (process_exit == 0)
		{
			return {"guest_exit", "guest_exit_before_agent"};
		}
		return {"launch_failed", timed_out ? "agent_ready_timeout" : "agent_not_ready"};
	}
	if (phase == "interactive" || frontier == "interactive")
	{
		return {timed_out ? "controlled_timeout" : "interactive", "interactive"};
	}
	if (present > 0)
	{
		return {timed_out ? "controlled_timeout" : "presenting", "presenting"};
	}
	if (graphic || frontier == "graphics")
	{
		return {timed_out ? "controlled_timeout" : "video_initialized", "video_initialized"};
	}
	if (process_exit == 0)
	{
		return {"guest_exit", UsableLabel({frontier, "guest_exit"})};
	}
	return {timed_out ? "controlled_timeout" : "runtime_started", "runtime_started"};
}

std::string SanitizeMatrixText(const std::string& text)
{
	std::string out = ReplaceWords(text, "[redacted-path]", [&text](size_t at) { return PathLength(text, at, true); });
	out             = ReplaceWords(out, "[redacted-path]", [&out](size_t at) { return PathLength(out, at, false); });
	return ReplaceWords(out, "[redacted-title-id]", [&out](size_t at) { return TitleIdLength(out, at); });
}

Json SanitizeMatrixJson(const Json& value)
{
	if (value.IsString())
	{
		return SanitizeMatrixText(value.AsString());
	}
	if (value.IsArray())
	{
		Json out = Json::MakeArray();
		for (const Json& item: value.Items())
		{
			out.Push(SanitizeMatrixJson(item));
		}
		return out;
	}
	if (value.IsObject())
	{
		Json out = Json::MakeObject();
		for (const auto& [key, item]: value.Members())
		{
			out.Set(key, SanitizeMatrixJson(item));
		}
		return out;
	}
	return value;
}

std::string MatrixActionableFromLog(const std::string& log_path)
{
	std::string text;
	std::string error;
	if (!ReadTextFile(log_path, kMaxLogBytes, &text, &error))
	{
		return {};
	}
	for (const std::string& raw: SplitLines(text))
	{
		const std::string line = Strip(raw);
		if (line.empty() || line.rfind("---", 0) == 0 || line[0] == '[')
		{
			continue;
		}
		// Structured assert banners, e.g. "=== Unexpected import ===".
		if (line.size() >= 3 && line.rfind("===", 0) == 0 && line.compare(line.size() - 3, 3, "===") == 0)
		{
			const std::string inner = Strip(Strip(line, "= "));
			if (!inner.empty())
			{
				return TruncateCodePoints(inner, 160);
			}
		}
		const std::string lowered   = Lower(line);
		const bool        exit_line = line.find("EXIT") != std::string::npos && line.find(':') != std::string::npos;
		if (exit_line || lowered.find("unsupported") != std::string::npos || lowered.find("not implemented") != std::string::npos ||
		    lowered.find("assert") != std::string::npos || lowered.find("fatal") != std::string::npos ||
		    lowered.find("unknown ") != std::string::npos)
		{
			return TruncateCodePoints(SanitizeMatrixText(line.substr(0, line.find(" in /"))), 160);
		}
	}
	return {};
}

MatrixCase RunMatrixCase(RunnerHost* host, const MatrixCaseOptions& options)
{
	std::error_code ec;
	const fs::path  case_dir = options.case_dir;
	fs::create_directories(case_dir / "captures", ec);
	const std::string socket = MatrixSocketPath(options.run_id, options.case_id);
	std::string       error;
	(void)WriteTextFile((case_dir / "agent.sock.path").string(), socket + "\n", &error);
	if (fs::exists(socket, ec))
	{
		return FailedLaunch(options.case_id, "socket_path_collision", true, {"socket_path_collision"});
	}

	ChildEnvironmentOptions env_options;
	env_options.guest_root        = options.package_root;
	env_options.capture_directory = (case_dir / "captures").string();
	env_options.agent_endpoint    = socket;
	env_options.optional_values   = {"KYTY_PRESENT_FPS_LIMIT"};
	Environment env               = BuildChildEnvironment(options.base_environment, env_options);
	if (options.mode == "bringup")
	{
		for (const auto& [key, value]: DiagnosticBringupEnvironment())
		{
			env[key] = value;
		}
	}
	// Evidence of strict purity: keys only, never values.
	std::string keys;
	for (const auto& [key, value]: env)
	{
		keys += key + "\n";
	}
	(void)WriteTextFile((case_dir / "child_env_keys.txt").string(), keys, &error);
	if (options.mode == "strict")
	{
		const auto bad = FindActiveEnvironmentKeys(env, StrictForbiddenEnvironmentKeys());
		if (!bad.empty())
		{
			std::string joined;
			for (const auto& key: bad)
			{
				joined += (joined.empty() ? "" : ",") + key;
			}
			return FailedLaunch(options.case_id, "strict_env_violation", false, {"strict_env_violation:" + joined});
		}
	}

	LaunchSpec spec;
	spec.argv     = {options.fc_script, "scripts/run_guest.lua", options.package_root};
	spec.cwd      = options.repo_root;
	spec.env      = std::move(env);
	spec.log_path = (case_dir / "child.log").string();
	if (!host->Launch(spec, &error))
	{
		return FailedLaunch(options.case_id, "parent_exception", true, {"launch_error"});
	}
	return MatrixCaseRun(host, options, socket).Run();
}

int MatrixExitCode(const std::vector<MatrixCase>& results)
{
	const bool failed = std::any_of(results.begin(), results.end(), [](const MatrixCase& item) {
		return item.attempted && OneOf(item.outcome, {"launch_failed", "loader_failed", "unsupported", "host_crash"});
	});
	return failed ? 1 : 0;
}

int GamesMatrixMain(const std::vector<std::string>& args)
{
	MatrixArgs  matrix;
	std::string error;
	if (!ParseMatrixArgs(args, &matrix, &error))
	{
		std::fprintf(stderr, "kyty_agent: %s\n", error.c_str());
		return 2;
	}
	if (matrix.games_root.empty())
	{
		std::fprintf(stderr, "error: set --games-root or KYTY_GAMES_ROOT\n");
		return 2;
	}
	const std::string games_root = ResolvePath(matrix.games_root);
	const std::string run_id     = NewRunId();
	const fs::path    run_dir    = fs::path(ResolvePath(matrix.scratch)) / ("run_" + run_id);
	std::error_code   ec;
	fs::create_directories(run_dir, ec);

	std::vector<MatrixCase>                          invalid;
	std::vector<std::pair<std::string, std::string>> valid_roots; // (case id, root)
	std::set<std::string>                            seen_ids;
	for (const std::string& root: DiscoverGameRoots(games_root, static_cast<int>(matrix.max_depth)))
	{
		const std::string id = AnonymousCaseId(games_root, root);
		if (!seen_ids.insert(id).second)
		{
			continue;
		}
		const std::string reason = InvalidPackageReason(root);
		if (reason.empty())
		{
			valid_roots.emplace_back(id, root);
		} else
		{
			invalid.push_back(InvalidFixture(id, reason));
		}
	}
	// Top-level directories without a primary are reported as invalid fixtures.
	std::vector<std::string> top_level;
	for (fs::directory_iterator it(games_root, ec), end; !ec && it != end; it.increment(ec))
	{
		std::error_code type_ec;
		if (it->is_directory(type_ec) && it->path().filename().string().rfind('.', 0) != 0)
		{
			top_level.push_back(it->path().string());
		}
	}
	std::stable_sort(top_level.begin(), top_level.end(), [](const std::string& a, const std::string& b) {
		return Lower(fs::path(a).filename().string()) < Lower(fs::path(b).filename().string());
	});
	for (const std::string& directory: top_level)
	{
		const std::string reason = InvalidPackageReason(directory);
		if (reason.empty())
		{
			continue;
		}
		const std::string id = AnonymousCaseId(games_root, directory);
		if (seen_ids.insert(id).second)
		{
			invalid.push_back(InvalidFixture(id, reason));
		}
	}

	Json case_ids = Json::MakeArray();
	for (const auto& [id, root]: valid_roots)
	{
		case_ids.Push(id);
	}
	for (const MatrixCase& item: invalid)
	{
		case_ids.Push(item.case_id);
	}
	const Json discover = Json::MakeObject()
	                          .Set("schema", kDiscoverSchema)
	                          .Set("valid_count", static_cast<int64_t>(valid_roots.size()))
	                          .Set("invalid_count", static_cast<int64_t>(invalid.size()))
	                          .Set("case_ids", case_ids);
	(void)WriteTextFile((run_dir / "discover.json").string(), discover.Dump(2) + "\n", &error);

	if (matrix.discover_only)
	{
		std::vector<std::string> pending_ids;
		for (const auto& [id, root]: valid_roots)
		{
			pending_ids.push_back(id);
		}
		WriteSanitized(run_dir / "summary.json", DiscoverOnlySummary(matrix, run_id, invalid, pending_ids));
		std::printf("%s\n", discover.Dump(2).c_str());
		std::fprintf(stderr, "summary=%s\n", (run_dir / "summary.json").string().c_str());
		return 0;
	}
	if (!fs::is_regular_file(matrix.fc_script, ec))
	{
		std::fprintf(stderr, "error: missing fc_script under build\n");
		return 2;
	}
	if (CreateSystemRunnerHost() == nullptr)
	{
		std::fprintf(stderr, "kyty_agent: games-matrix is not supported on this host\n");
		return 2;
	}
	if (matrix.max_cases > 0 && valid_roots.size() > static_cast<size_t>(matrix.max_cases))
	{
		valid_roots.resize(static_cast<size_t>(matrix.max_cases));
	}

	std::vector<MatrixCase> results = invalid;
	const Environment       base    = CurrentEnvironment();
	for (const auto& [id, root]: valid_roots)
	{
		MatrixCaseOptions options;
		options.case_id            = id;
		options.package_root       = root;
		options.repo_root          = matrix.repo_root;
		options.fc_script          = matrix.fc_script;
		options.case_dir           = (run_dir / id).string();
		options.run_id             = run_id;
		options.mode               = matrix.mode;
		options.startup_deadline_s = matrix.startup_s;
		options.present_deadline_s = matrix.present_s;
		options.total_deadline_s   = matrix.total_s;
		options.stability_s        = matrix.stability_s;
		options.base_environment   = base;
		// Each case gets its own host so one case's child never leaks into the next.
		const std::unique_ptr<RunnerHost> host = CreateSystemRunnerHost();
		results.push_back(RunMatrixCase(host.get(), options));
		(void)WriteTextFile((run_dir / id / "result.json").string(), results.back().ToJson().Dump(2) + "\n", &error);
	}

	const fs::path summary_path = run_dir / "summary.json";
	WriteSanitized(summary_path, RunSummary(matrix, run_id, results, static_cast<int64_t>(valid_roots.size())));
	std::string summary_text;
	(void)ReadTextFile(summary_path.string(), kMaxLogBytes, &summary_text, &error);
	std::printf("%s\n", summary_text.c_str());
	std::fprintf(stderr, "raw_evidence_dir=%s\n", run_dir.string().c_str());
	return MatrixExitCode(results);
}

} // namespace Kyty::Agent::Runner
