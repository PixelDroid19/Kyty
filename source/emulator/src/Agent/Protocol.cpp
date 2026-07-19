#include "Emulator/Agent/Protocol.h"

#include "Emulator/Agent/EventRing.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Validation/DomainValidators.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {
namespace {

const char* BringUpModeName(Core::BringUp::Mode mode)
{
	return mode == Core::BringUp::Mode::Unsafe ? "unsafe" : "strict";
}

void AppendFeatureList(const Core::BringUp::Config& config, std::string* out)
{
	*out += '[';
	bool           first           = true;
	const uint32_t not_implemented = 1u << static_cast<uint32_t>(Core::BringUp::Feature::NotImplemented);
	const uint32_t missing_func    = 1u << static_cast<uint32_t>(Core::BringUp::Feature::MissingFunctionImport);
	const uint32_t gfx_perm        = 1u << static_cast<uint32_t>(Core::BringUp::Feature::GraphicsPermissive);
	const uint32_t adj_discover    = 1u << static_cast<uint32_t>(Core::BringUp::Feature::AdjacentModuleDiscovery);

	if ((config.feature_mask & not_implemented) != 0)
	{
		*out += first ? "\"not_implemented\"" : ",\"not_implemented\"";
		first = false;
	}
	if ((config.feature_mask & missing_func) != 0)
	{
		*out += first ? "\"missing_function_import\"" : ",\"missing_function_import\"";
		first = false;
	}
	if ((config.feature_mask & gfx_perm) != 0)
	{
		*out += first ? "\"gfx_permissive\"" : ",\"gfx_permissive\"";
		first = false;
	}
	if ((config.feature_mask & adj_discover) != 0)
	{
		*out += first ? "\"adjacent_module_discovery\"" : ",\"adjacent_module_discovery\"";
	}
	*out += ']';
	(void)first;
}

void AppendSubsystemList(const Core::BringUp::Config& config, std::string* out)
{
	*out += '[';
	bool first = true;
	const struct
	{
		uint32_t    bit;
		const char* name;
	} entries[] = {
	    {1u << static_cast<uint32_t>(Core::BringUp::Subsystem::Core), "core"},
	    {1u << static_cast<uint32_t>(Core::BringUp::Subsystem::Loader), "loader"},
	    {1u << static_cast<uint32_t>(Core::BringUp::Subsystem::Kernel), "kernel"},
	    {1u << static_cast<uint32_t>(Core::BringUp::Subsystem::Graphics), "graphics"},
	    {1u << static_cast<uint32_t>(Core::BringUp::Subsystem::Audio), "audio"},
	    {1u << static_cast<uint32_t>(Core::BringUp::Subsystem::Network), "network"},
	    {1u << static_cast<uint32_t>(Core::BringUp::Subsystem::Hle), "hle"},
	    {1u << static_cast<uint32_t>(Core::BringUp::Subsystem::Other), "other"},
	};
	for (const auto& e: entries)
	{
		if ((config.subsystem_mask & e.bit) == 0)
		{
			continue;
		}
		if (!first)
		{
			*out += ',';
		}
		*out += '"';
		*out += e.name;
		*out += '"';
		first = false;
	}
	*out += ']';
}

std::string FeatureString(Core::BringUp::Feature feature)
{
	switch (feature)
	{
		case Core::BringUp::Feature::NotImplemented: return "not_implemented";
		case Core::BringUp::Feature::MissingFunctionImport: return "missing_function_import";
		case Core::BringUp::Feature::GraphicsPermissive: return "gfx_permissive";
		case Core::BringUp::Feature::AdjacentModuleDiscovery: return "adjacent_module_discovery";
		default: return "unknown";
	}
}

std::string SubsystemString(Core::BringUp::Subsystem subsystem)
{
	switch (subsystem)
	{
		case Core::BringUp::Subsystem::Core: return "core";
		case Core::BringUp::Subsystem::Loader: return "loader";
		case Core::BringUp::Subsystem::Kernel: return "kernel";
		case Core::BringUp::Subsystem::Graphics: return "graphics";
		case Core::BringUp::Subsystem::Audio: return "audio";
		case Core::BringUp::Subsystem::Network: return "network";
		case Core::BringUp::Subsystem::Hle: return "hle";
		case Core::BringUp::Subsystem::Other: return "other";
		default: return "unknown";
	}
}

} // namespace

Tool ParseTool(const char* name) noexcept
{
	if (name == nullptr)
	{
		return Tool::Unknown;
	}
	struct Entry
	{
		const char* name;
		Tool        tool;
	};
	static constexpr Entry kTools[] = {
	    {"help", Tool::Help},
	    {"ping", Tool::Ping},
	    {"status", Tool::Status},
	    {"diagnostics", Tool::Diagnostics},
	    {"perf_snapshot", Tool::PerfSnapshot},
	    {"sync_waits", Tool::SyncWaits},
	    {"threads", Tool::Threads},
	    {"events", Tool::Events},
	    {"last_error", Tool::LastError},
	    {"capture", Tool::Capture},
	    {"score", Tool::Score},
	    {"pad_down", Tool::PadDown},
	    {"pad_up", Tool::PadUp},
	    {"pad_tap", Tool::PadTap},
	    {"pad_axis", Tool::PadAxis},
	    {"pad_clear", Tool::PadClear},
	    {"wait_present", Tool::WaitPresent},
	    {"wait_frame", Tool::WaitFrame},
	    {"wait_phase", Tool::WaitPhase},
	    {"wait_event", Tool::WaitEvent},
	    {"watch", Tool::Watch},
	};
	for (const auto& entry: kTools)
	{
		if (std::strcmp(name, entry.name) == 0)
		{
			return entry.tool;
		}
	}
	return Tool::Unknown;
}

const char* SkipWs(const char* p)
{
	while (p != nullptr && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
	{
		++p;
	}
	return p;
}

bool MatchKey(const char* p, const char* key, const char** after_colon)
{
	p = SkipWs(p);
	if (p == nullptr || *p != '"')
	{
		return false;
	}
	++p;
	const size_t key_len = std::strlen(key);
	if (std::strncmp(p, key, key_len) != 0 || p[key_len] != '"')
	{
		return false;
	}
	p = SkipWs(p + key_len + 1);
	if (*p != ':')
	{
		return false;
	}
	*after_colon = SkipWs(p + 1);
	return true;
}

bool ParseStringValue(const char* p, std::string* out, const char** end)
{
	p = SkipWs(p);
	if (p == nullptr || *p != '"')
	{
		return false;
	}
	++p;
	std::string value;
	while (*p != '\0' && *p != '"')
	{
		if (*p == '\\' && p[1] != '\0')
		{
			++p;
			switch (*p)
			{
				case '"':
				case '\\':
				case '/': value.push_back(*p); break;
				case 'n': value.push_back('\n'); break;
				case 't': value.push_back('\t'); break;
				default: value.push_back(*p); break;
			}
		} else
		{
			value.push_back(*p);
		}
		++p;
	}
	if (*p != '"')
	{
		return false;
	}
	*out = value;
	*end = p + 1;
	return true;
}

bool ParseU64Value(const char* p, uint64_t* out, const char** end)
{
	p = SkipWs(p);
	if (p == nullptr || !std::isdigit(static_cast<unsigned char>(*p)))
	{
		return false;
	}
	char*      parse_end = nullptr;
	const auto value     = std::strtoull(p, &parse_end, 10);
	if (parse_end == p)
	{
		return false;
	}
	*out = value;
	*end = parse_end;
	return true;
}

bool SkipJsonString(const char** cursor)
{
	const char* p = *cursor;
	if (p == nullptr || *p != '"')
	{
		return false;
	}
	for (++p; *p != '\0'; ++p)
	{
		const auto ch = static_cast<unsigned char>(*p);
		if (ch < 0x20)
		{
			return false;
		}
		if (*p == '"')
		{
			*cursor = p + 1;
			return true;
		}
		if (*p != '\\')
		{
			continue;
		}
		++p;
		if (*p == '\0')
		{
			return false;
		}
		if (std::strchr("\"\\/bfnrt", *p) != nullptr)
		{
			continue;
		}
		if (*p != 'u')
		{
			return false;
		}
		for (int i = 0; i < 4; ++i)
		{
			if (!std::isxdigit(static_cast<unsigned char>(p[1 + i])))
			{
				return false;
			}
		}
		p += 4;
	}
	return false;
}

bool SkipJsonValue(const char** cursor, uint32_t depth);

bool SkipJsonComposite(const char** cursor, char open, char close, uint32_t depth)
{
	const char* p = SkipWs(*cursor);
	if (p == nullptr || *p != open || depth > 32u)
	{
		return false;
	}
	p = SkipWs(p + 1);
	if (*p == close)
	{
		*cursor = p + 1;
		return true;
	}
	for (;;)
	{
		if (open == '{')
		{
			if (!SkipJsonString(&p))
			{
				return false;
			}
			p = SkipWs(p);
			if (*p != ':')
			{
				return false;
			}
			++p;
		}
		if (!SkipJsonValue(&p, depth + 1u))
		{
			return false;
		}
		p = SkipWs(p);
		if (*p == close)
		{
			*cursor = p + 1;
			return true;
		}
		if (*p != ',')
		{
			return false;
		}
		p = SkipWs(p + 1);
	}
}

bool SkipJsonNumber(const char** cursor)
{
	const char* p = *cursor;
	if (*p == '-')
	{
		++p;
	}
	if (*p == '0')
	{
		++p;
	} else
	{
		if (!std::isdigit(static_cast<unsigned char>(*p)))
		{
			return false;
		}
		while (std::isdigit(static_cast<unsigned char>(*p)))
		{
			++p;
		}
	}
	if (*p == '.')
	{
		++p;
		if (!std::isdigit(static_cast<unsigned char>(*p)))
		{
			return false;
		}
		while (std::isdigit(static_cast<unsigned char>(*p)))
		{
			++p;
		}
	}
	if (*p == 'e' || *p == 'E')
	{
		++p;
		if (*p == '+' || *p == '-')
		{
			++p;
		}
		if (!std::isdigit(static_cast<unsigned char>(*p)))
		{
			return false;
		}
		while (std::isdigit(static_cast<unsigned char>(*p)))
		{
			++p;
		}
	}
	*cursor = p;
	return true;
}

bool SkipJsonValue(const char** cursor, uint32_t depth)
{
	const char* p = SkipWs(*cursor);
	if (p == nullptr)
	{
		return false;
	}
	if (*p == '"')
	{
		const bool ok = SkipJsonString(&p);
		*cursor       = p;
		return ok;
	}
	if (*p == '{' || *p == '[')
	{
		const char open  = *p;
		const char close = open == '{' ? '}' : ']';
		const bool ok    = SkipJsonComposite(&p, open, close, depth);
		*cursor          = p;
		return ok;
	}
	const struct
	{
		const char* text;
		size_t      length;
	} literals[] = {{"true", 4}, {"false", 5}, {"null", 4}};
	for (const auto& literal: literals)
	{
		if (std::strncmp(p, literal.text, literal.length) == 0)
		{
			*cursor = p + literal.length;
			return true;
		}
	}
	if (*p == '-' || std::isdigit(static_cast<unsigned char>(*p)))
	{
		const bool ok = SkipJsonNumber(&p);
		*cursor       = p;
		return ok;
	}
	return false;
}

bool IsCompleteJsonObject(const char* json)
{
	const char* p = SkipWs(json);
	if (p == nullptr || *p != '{' || !SkipJsonValue(&p, 0))
	{
		return false;
	}
	return *SkipWs(p) == '\0';
}

bool FindObjectField(const char* json, const char* key, const char** value_start)
{
	if (json == nullptr || key == nullptr || value_start == nullptr)
	{
		return false;
	}
	const char* p = SkipWs(json);
	if (*p != '{')
	{
		return false;
	}
	++p;
	while (*p != '\0' && *p != '}')
	{
		const char* after = nullptr;
		if (MatchKey(p, key, &after))
		{
			*value_start = after;
			return true;
		}
		// Skip this field roughly: advance to next comma at depth 1 or end.
		int  depth      = 0;
		bool in_str     = false;
		bool escape     = false;
		bool seen_colon = false;
		for (; *p != '\0'; ++p)
		{
			const char c = *p;
			if (in_str)
			{
				if (escape)
				{
					escape = false;
				} else if (c == '\\')
				{
					escape = true;
				} else if (c == '"')
				{
					in_str = false;
				}
				continue;
			}
			if (c == '"')
			{
				in_str = true;
				continue;
			}
			if (c == '{' || c == '[')
			{
				++depth;
				continue;
			}
			if (c == '}' || c == ']')
			{
				if (depth == 0)
				{
					return false;
				}
				--depth;
				continue;
			}
			if (c == ':' && depth == 0)
			{
				seen_colon = true;
				continue;
			}
			if (seen_colon && depth == 0 && c == ',')
			{
				++p;
				break;
			}
			if (seen_colon && depth == 0 && c == '}')
			{
				return false;
			}
		}
	}
	return false;
}

std::string JsonEscape(const char* value)
{
	std::string out;
	if (value == nullptr)
	{
		return out;
	}
	for (const char* p = value; *p != '\0'; ++p)
	{
		const auto ch = static_cast<unsigned char>(*p);
		switch (ch)
		{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (ch < 0x20)
				{
					char escaped[7];
					std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
					out += escaped;
				} else
				{
					out.push_back(*p);
				}
				break;
		}
	}
	return out;
}

std::string JsonString(const char* value)
{
	return std::string("\"") + JsonEscape(value) + "\"";
}

std::string BuildDiagnosticsResult(const Core::BringUp::Config& config, const Core::BringUp::Diagnostics& bringup,
                                   const Loader::MissingImportDiagnostics& imports, const Loader::ModuleLoadPlanDiagnostics& load_plan)
{
	// Prefer diagnostics snapshot's embedded config (immutable effective view).
	// Fall back to the explicit config argument when the snapshot is still default
	// and the caller passed a live GetConfig() copy.
	const Core::BringUp::Config& eff =
	    (bringup.config.mode == Core::BringUp::Mode::Unsafe || bringup.config.feature_mask != 0 || bringup.config.explicitly_configured)
	        ? bringup.config
	        : config;

	std::string out;
	out.reserve(1536);
	out += '{';
	out += "\"protocol_version\":" + std::to_string(Kyty::Agent::kProtocolVersion);
	out += ",\"diagnostic_input\":true";
	out += ",\"bring_up\":{";
	out += "\"mode\":" + JsonString(BringUpModeName(eff.mode));
	out += ",\"feature_mask\":" + std::to_string(eff.feature_mask);
	out += ",\"subsystem_mask\":" + std::to_string(eff.subsystem_mask);
	out += ",\"burst_limit\":" + std::to_string(eff.burst_limit);
	out += ",\"burst_window_ms\":" + std::to_string(eff.burst_window_ms);
	out += ",\"explicitly_configured\":" + std::string(eff.explicitly_configured ? "true" : "false");
	out += ",\"enabled_features\":";
	std::string feature_json;
	AppendFeatureList(eff, &feature_json);
	out += feature_json;
	out += ",\"enabled_subsystems\":";
	std::string subsystem_json;
	AppendSubsystemList(eff, &subsystem_json);
	out += subsystem_json;
	out += ",\"diagnostics\":{";
	out += "\"unique_sites\":" + std::to_string(bringup.unique_sites);
	out += ",\"total_continuations\":" + std::to_string(bringup.total_continuations);
	out += ",\"total_halts\":" + std::to_string(bringup.total_halts);
	out += ",\"breaker_trips\":" + std::to_string(bringup.breaker_trips);
	out += ",\"table_overflows\":" + std::to_string(bringup.table_overflows);
	out += ",\"config_rejections\":" + std::to_string(bringup.config_rejections);
	out += ",\"last_breaker_key\":" + std::to_string(bringup.last_breaker_key);
	out += ",\"last_breaker_feature\":" + JsonString(FeatureString(bringup.last_breaker_feature).c_str());
	out += ",\"last_breaker_subsystem\":" + JsonString(SubsystemString(bringup.last_breaker_subsystem).c_str());
	out += ",\"continues_by_feature\":[";
	for (uint32_t i = 0; i < Core::BringUp::kFeatureCount; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		out += std::to_string(bringup.continues_by_feature[i]);
	}
	out += "],\"halts_by_feature\":[";
	for (uint32_t i = 0; i < Core::BringUp::kFeatureCount; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		out += std::to_string(bringup.halts_by_feature[i]);
	}
	out += "],\"continues_by_subsystem\":[";
	for (uint32_t i = 0; i < Core::BringUp::kSubsystemCount; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		out += std::to_string(bringup.continues_by_subsystem[i]);
	}
	out += "],\"halts_by_subsystem\":[";
	for (uint32_t i = 0; i < Core::BringUp::kSubsystemCount; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		out += std::to_string(bringup.halts_by_subsystem[i]);
	}
	// Close diagnostics object, then bring_up object — event_ring is top-level.
	out += "]}"; // end diagnostics
	out += "}";  // end bring_up

	// Top-level event_ring so consumers use result.event_ring.dropped (not nested
	// under bring_up). Overflow remains visible when history was overwritten.
	const auto ring = Emulator::Agent::EventRing::Instance().GetStats();
	out += ",\"event_ring\":{";
	out += "\"capacity\":" + std::to_string(ring.capacity);
	out += ",\"size\":" + std::to_string(ring.size);
	out += ",\"next_seq\":" + std::to_string(ring.next_seq);
	out += ",\"total_pushed\":" + std::to_string(ring.total_pushed);
	out += ",\"dropped\":" + std::to_string(ring.dropped);
	out += ",\"overflowed\":" + std::string(ring.overflowed ? "true" : "false");
	out += "}";

	const auto performance = Libs::Graphics::DebugStatsGetPerformanceSnapshot(false);
	char       performance_json[8192];
	std::snprintf(
	    performance_json, sizeof(performance_json),
	    ",\"performance\":{\"interval_ms\":%llu,\"draws\":%llu,\"dispatches\":%llu,"
	    "\"alloc_bytes\":%llu,\"free_bytes\":%llu,\"creates\":%llu,\"frees\":%llu,\"flips\":%llu,"
	    "\"buffer_flushes\":%llu,\"command_buffers\":%llu,\"submits\":%llu,"
	    "\"fence_waits\":%llu,\"fence_wait_ns\":%llu,\"fence_wait_max_ns\":%llu,"
	    "\"acquires\":%llu,\"acquire_ns\":%llu,\"acquire_max_ns\":%llu,"
	    "\"presents\":%llu,\"present_ns\":%llu,\"present_max_ns\":%llu,"
	    "\"wait_reg_mem\":%llu,\"wait_reg_mem_ns\":%llu,\"wait_reg_mem_max_ns\":%llu,"
	    "\"wait_flip_done\":%llu,\"wait_flip_done_ns\":%llu,\"wait_flip_done_max_ns\":%llu,"
	    "\"in_flight_current\":%llu,\"in_flight_max\":%llu,"
	    "\"frame_samples\":%llu,\"frame_time_p50_us\":%llu,\"frame_time_p95_us\":%llu,"
	    "\"frame_time_p99_us\":%llu,\"frame_time_max_us\":%llu,"
	    "\"frames_over_50ms\":%llu,\"frames_over_100ms\":%llu,\"frames_over_250ms\":%llu,"
	    "\"hash_calls\":%llu,\"hash_bytes\":%llu,\"hash_ns\":%llu,\"hash_max_ns\":%llu,"
	    "\"detile_calls\":%llu,\"detile_bytes\":%llu,\"detile_ns\":%llu,\"detile_max_ns\":%llu,"
	    "\"upload_calls\":%llu,\"upload_bytes\":%llu,\"upload_ns\":%llu,\"upload_max_ns\":%llu,"
	    "\"writeback_calls\":%llu,\"writeback_bytes\":%llu,\"writeback_ns\":%llu,\"writeback_max_ns\":%llu,"
	    "\"gfx_pipeline_lookup_hits\":%llu,\"gfx_pipeline_lookup_misses\":%llu,"
	    "\"gfx_pipeline_lookup_ns\":%llu,\"gfx_pipeline_lookup_max_ns\":%llu,"
	    "\"compute_pipeline_lookup_hits\":%llu,\"compute_pipeline_lookup_misses\":%llu,"
	    "\"compute_pipeline_lookup_ns\":%llu,\"compute_pipeline_lookup_max_ns\":%llu,"
	    "\"pipeline_evictions\":%llu,"
	    "\"gfx_pipeline_miss_count\":%llu,\"gfx_pipeline_miss_ns\":%llu,\"gfx_pipeline_miss_max_ns\":%llu,"
	    "\"compute_pipeline_miss_count\":%llu,\"compute_pipeline_miss_ns\":%llu,\"compute_pipeline_miss_max_ns\":%llu,"
	    "\"shader_ir_input_analysis_count\":%llu,\"shader_ir_input_analysis_ns\":%llu,"
	    "\"shader_ir_input_analysis_max_ns\":%llu,"
	    "\"shader_ir_pipeline_miss_count\":%llu,\"shader_ir_pipeline_miss_ns\":%llu,"
	    "\"shader_ir_pipeline_miss_max_ns\":%llu,"
	    "\"spirv_source_count\":%llu,\"spirv_source_ns\":%llu,\"spirv_source_max_ns\":%llu,"
	    "\"spirv_compile_count\":%llu,\"spirv_compile_ns\":%llu,\"spirv_compile_max_ns\":%llu,"
	    "\"vk_graphics_pipeline_create_count\":%llu,\"vk_graphics_pipeline_create_ns\":%llu,"
	    "\"vk_graphics_pipeline_create_max_ns\":%llu,"
	    "\"vk_compute_pipeline_create_count\":%llu,\"vk_compute_pipeline_create_ns\":%llu,"
	    "\"vk_compute_pipeline_create_max_ns\":%llu,"
	    "\"shader_translation_cache_hits\":%llu,\"shader_translation_cache_misses\":%llu,"
	    "\"shader_translation_cache_evictions\":%llu,"
	    "\"live_objects\":%llu,\"fps\":%.3f,\"frame_time_ms\":%.3f",
	    static_cast<unsigned long long>(performance.interval_ms), static_cast<unsigned long long>(performance.draws),
	    static_cast<unsigned long long>(performance.dispatches), static_cast<unsigned long long>(performance.alloc_bytes),
	    static_cast<unsigned long long>(performance.free_bytes), static_cast<unsigned long long>(performance.creates),
	    static_cast<unsigned long long>(performance.frees), static_cast<unsigned long long>(performance.flips),
	    static_cast<unsigned long long>(performance.buffer_flushes), static_cast<unsigned long long>(performance.command_buffers),
	    static_cast<unsigned long long>(performance.submits), static_cast<unsigned long long>(performance.fence_waits),
	    static_cast<unsigned long long>(performance.fence_wait_ns), static_cast<unsigned long long>(performance.fence_wait_max_ns),
	    static_cast<unsigned long long>(performance.acquires), static_cast<unsigned long long>(performance.acquire_ns),
	    static_cast<unsigned long long>(performance.acquire_max_ns), static_cast<unsigned long long>(performance.presents),
	    static_cast<unsigned long long>(performance.present_ns), static_cast<unsigned long long>(performance.present_max_ns),
	    static_cast<unsigned long long>(performance.wait_reg_mem), static_cast<unsigned long long>(performance.wait_reg_mem_ns),
	    static_cast<unsigned long long>(performance.wait_reg_mem_max_ns), static_cast<unsigned long long>(performance.wait_flip_done),
	    static_cast<unsigned long long>(performance.wait_flip_done_ns), static_cast<unsigned long long>(performance.wait_flip_done_max_ns),
	    static_cast<unsigned long long>(performance.in_flight_current), static_cast<unsigned long long>(performance.in_flight_max),
	    static_cast<unsigned long long>(performance.frame_samples), static_cast<unsigned long long>(performance.frame_time_p50_us),
	    static_cast<unsigned long long>(performance.frame_time_p95_us), static_cast<unsigned long long>(performance.frame_time_p99_us),
	    static_cast<unsigned long long>(performance.frame_time_max_us), static_cast<unsigned long long>(performance.frames_over_50ms),
	    static_cast<unsigned long long>(performance.frames_over_100ms), static_cast<unsigned long long>(performance.frames_over_250ms),
	    static_cast<unsigned long long>(performance.hash_calls), static_cast<unsigned long long>(performance.hash_bytes),
	    static_cast<unsigned long long>(performance.hash_ns), static_cast<unsigned long long>(performance.hash_max_ns),
	    static_cast<unsigned long long>(performance.detile_calls), static_cast<unsigned long long>(performance.detile_bytes),
	    static_cast<unsigned long long>(performance.detile_ns), static_cast<unsigned long long>(performance.detile_max_ns),
	    static_cast<unsigned long long>(performance.upload_calls), static_cast<unsigned long long>(performance.upload_bytes),
	    static_cast<unsigned long long>(performance.upload_ns), static_cast<unsigned long long>(performance.upload_max_ns),
	    static_cast<unsigned long long>(performance.writeback_calls), static_cast<unsigned long long>(performance.writeback_bytes),
	    static_cast<unsigned long long>(performance.writeback_ns), static_cast<unsigned long long>(performance.writeback_max_ns),
	    static_cast<unsigned long long>(performance.gfx_pipeline_lookup_hits),
	    static_cast<unsigned long long>(performance.gfx_pipeline_lookup_misses),
	    static_cast<unsigned long long>(performance.gfx_pipeline_lookup_ns),
	    static_cast<unsigned long long>(performance.gfx_pipeline_lookup_max_ns),
	    static_cast<unsigned long long>(performance.compute_pipeline_lookup_hits),
	    static_cast<unsigned long long>(performance.compute_pipeline_lookup_misses),
	    static_cast<unsigned long long>(performance.compute_pipeline_lookup_ns),
	    static_cast<unsigned long long>(performance.compute_pipeline_lookup_max_ns),
	    static_cast<unsigned long long>(performance.pipeline_evictions),
	    static_cast<unsigned long long>(performance.gfx_pipeline_miss_count),
	    static_cast<unsigned long long>(performance.gfx_pipeline_miss_ns),
	    static_cast<unsigned long long>(performance.gfx_pipeline_miss_max_ns),
	    static_cast<unsigned long long>(performance.compute_pipeline_miss_count),
	    static_cast<unsigned long long>(performance.compute_pipeline_miss_ns),
	    static_cast<unsigned long long>(performance.compute_pipeline_miss_max_ns),
	    static_cast<unsigned long long>(performance.shader_ir_input_analysis_count),
	    static_cast<unsigned long long>(performance.shader_ir_input_analysis_ns),
	    static_cast<unsigned long long>(performance.shader_ir_input_analysis_max_ns),
	    static_cast<unsigned long long>(performance.shader_ir_pipeline_miss_count),
	    static_cast<unsigned long long>(performance.shader_ir_pipeline_miss_ns),
	    static_cast<unsigned long long>(performance.shader_ir_pipeline_miss_max_ns),
	    static_cast<unsigned long long>(performance.spirv_source_count),
	    static_cast<unsigned long long>(performance.spirv_source_ns),
	    static_cast<unsigned long long>(performance.spirv_source_max_ns),
	    static_cast<unsigned long long>(performance.spirv_compile_count),
	    static_cast<unsigned long long>(performance.spirv_compile_ns),
	    static_cast<unsigned long long>(performance.spirv_compile_max_ns),
	    static_cast<unsigned long long>(performance.vk_graphics_pipeline_create_count),
	    static_cast<unsigned long long>(performance.vk_graphics_pipeline_create_ns),
	    static_cast<unsigned long long>(performance.vk_graphics_pipeline_create_max_ns),
	    static_cast<unsigned long long>(performance.vk_compute_pipeline_create_count),
	    static_cast<unsigned long long>(performance.vk_compute_pipeline_create_ns),
	    static_cast<unsigned long long>(performance.vk_compute_pipeline_create_max_ns),
	    static_cast<unsigned long long>(performance.shader_translation_cache_hits),
	    static_cast<unsigned long long>(performance.shader_translation_cache_misses),
	    static_cast<unsigned long long>(performance.shader_translation_cache_evictions),
	    static_cast<unsigned long long>(performance.live_objects), performance.fps, performance.frame_time_ms);
	out += performance_json;
	out += ",\"gpu_memory\":{\"create_calls\":" + std::to_string(performance.gpu_memory_create_calls);
	out += ",\"create_ns\":" + std::to_string(performance.gpu_memory_create_ns);
	out += ",\"create_max_ns\":" + std::to_string(performance.gpu_memory_create_max_ns);
	out += ",\"types\":[";
	constexpr const char* gpu_memory_type_names[Libs::Graphics::kDebugStatsGpuMemoryTypeCount] = {
	    "video_out_buffer", "depth_stencil_buffer", "label",   "index_buffer",  "vertex_buffer",
	    "storage_buffer",   "texture",              "render_texture", "storage_texture"};
	for (uint32_t i = 0; i < Libs::Graphics::kDebugStatsGpuMemoryTypeCount; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		const auto& type = performance.gpu_memory_types[i];
		out += "{\"type\":\"";
		out += gpu_memory_type_names[i];
		out += "\",\"fast_reuse\":" + std::to_string(type.fast_reuse);
		out += ",\"exact_reuse\":" + std::to_string(type.exact_reuse);
		out += ",\"new_standalone\":" + std::to_string(type.new_standalone);
		out += ",\"new_linked\":" + std::to_string(type.new_linked);
		out += ",\"new_from_objects\":" + std::to_string(type.new_from_objects);
		out += ",\"reclaim_new\":" + std::to_string(type.reclaim_new);
		out += ",\"logical_free\":" + std::to_string(type.logical_free);
		out += ",\"live\":" + std::to_string(type.live);
		out += '}';
	}
	out += "]}}";

	out += ",\"missing_imports\":{";
	out += "\"resolution_attempts\":" + std::to_string(imports.resolution_attempts);
	out += ",\"unique_symbols\":" + std::to_string(imports.unique_symbols);
	out += ",\"used\":" + std::to_string(imports.used);
	out += ",\"table_overflows\":" + std::to_string(imports.table_overflows);
	out += ",\"table_capacity\":" + std::to_string(imports.table_capacity);
	out += "}";

	// Sanitized load plan only (relative keys / counts — never host paths or title IDs).
	constexpr uint32_t kDisplayedEntries    = 8;
	constexpr uint32_t kDisplayedRejections = 4;
	constexpr uint32_t kDisplayedConflicts  = 4;
	const bool         truncated = load_plan.entry_count > kDisplayedEntries || load_plan.rejection_count > kDisplayedRejections ||
	                               load_plan.export_conflict_count > kDisplayedConflicts;
	out += ",\"load_plan\":{";
	out += "\"discovery_enabled\":" + std::string(load_plan.discovery_enabled ? "true" : "false");
	out += ",\"discovery_attempted\":" + std::string(load_plan.discovery_attempted ? "true" : "false");
	out += ",\"entry_count\":" + std::to_string(load_plan.entry_count);
	out += ",\"adjacent_count\":" + std::to_string(load_plan.adjacent_count);
	out += ",\"rejection_count\":" + std::to_string(load_plan.rejection_count);
	out += ",\"applied_count\":" + std::to_string(load_plan.applied_count);
	out += ",\"export_conflict_count\":" + std::to_string(load_plan.export_conflict_count);
	out += ",\"truncated\":" + std::string(truncated ? "true" : "false");
	out += ",\"primary_identity\":" + JsonString(load_plan.primary_identity);
	out += ",\"entries\":[";
	for (uint32_t i = 0; i < load_plan.entry_count && i < kDisplayedEntries; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		out += JsonString(load_plan.entries[i]);
	}
	out += "],\"rejections\":[";
	for (uint32_t i = 0; i < load_plan.rejection_count && i < kDisplayedRejections; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		out += JsonString(load_plan.rejections[i]);
	}
	out += "],\"export_conflicts\":[";
	for (uint32_t i = 0; i < load_plan.export_conflict_count && i < kDisplayedConflicts; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		out += JsonString(load_plan.export_conflicts[i]);
	}
	out += "]}"; // end load_plan
	out += '}';  // end root
	return out;
}

std::string FormatOk(uint64_t id, const std::string& result_json_object)
{
	// Every response envelope carries the same protocol_version (current state of wire).
	char prefix[96];
	std::snprintf(prefix, sizeof(prefix),
	              "{\"id\":%llu,\"ok\":true,\"protocol_version\":%u,\"result\":", static_cast<unsigned long long>(id),
	              Kyty::Agent::kProtocolVersion);
	return std::string(prefix) + result_json_object + "}";
}

std::string FormatErr(uint64_t id, const char* code, const char* message)
{
	char buf[Kyty::Agent::kRequestLineMax];
	std::snprintf(buf, sizeof(buf), "{\"id\":%llu,\"ok\":false,\"protocol_version\":%u,\"error\":{\"code\":%s,\"message\":%s}}",
	              static_cast<unsigned long long>(id), Kyty::Agent::kProtocolVersion, JsonString(code != nullptr ? code : "error").c_str(),
	              JsonString(message != nullptr ? message : "").c_str());
	return std::string(buf);
}

Core::Domain::ValidationResult ParseAndValidateRequestLine(const char* line, Request* out)
{
	using Core::Domain::Fail;
	using Core::Domain::Ok;

	if (line == nullptr || out == nullptr)
	{
		return Fail("malformed", "agent", "parse_request", "null line or out");
	}
	*out = Request {};
	if (!IsCompleteJsonObject(line))
	{
		return Fail("malformed", "agent", "parse_request", "request must be one complete JSON object");
	}

	const char* id_value = nullptr;
	if (!FindObjectField(line, "id", &id_value))
	{
		return Fail("malformed", "agent", "parse_request", "missing id");
	}
	const char* end = nullptr;
	if (!ParseU64Value(id_value, &out->id, &end))
	{
		return Fail("malformed", "agent", "parse_request", "id must be an integer");
	}

	const char* tool_value = nullptr;
	if (!FindObjectField(line, "tool", &tool_value) || !ParseStringValue(tool_value, &out->tool, &end))
	{
		return Fail("malformed", "agent", "parse_request", "missing tool");
	}
	out->kind = ParseTool(out->tool.c_str());

	const char* args_value = nullptr;
	if (FindObjectField(line, "args", &args_value))
	{
		args_value = SkipWs(args_value);
		if (*args_value != '{')
		{
			return Fail("malformed", "agent", "parse_request", "args must be an object");
		}
		int         depth  = 0;
		bool        in_str = false;
		bool        escape = false;
		const char* start  = args_value;
		const char* p      = args_value;
		for (; *p != '\0'; ++p)
		{
			const char c = *p;
			if (in_str)
			{
				if (escape)
				{
					escape = false;
				} else if (c == '\\')
				{
					escape = true;
				} else if (c == '"')
				{
					in_str = false;
				}
				continue;
			}
			if (c == '"')
			{
				in_str = true;
				continue;
			}
			if (c == '{')
			{
				++depth;
				continue;
			}
			if (c == '}')
			{
				--depth;
				if (depth == 0)
				{
					out->args_json.assign(start, static_cast<size_t>(p - start + 1));
					break;
				}
			}
		}
		if (out->args_json.empty())
		{
			return Fail("malformed", "agent", "parse_request", "args object truncated");
		}
	} else
	{
		out->args_json = "{}";
	}

	// Domain policy on the parsed draft (known tools, pad button, etc.).
	Validation::AgentRequestDraft draft {};
	draft.id              = out->id;
	draft.tool            = out->tool.c_str();
	draft.known_tool      = out->kind != Tool::Unknown;
	draft.requires_button = out->kind == Tool::PadDown || out->kind == Tool::PadUp || out->kind == Tool::PadTap;
	std::string button;
	draft.has_string_button = ArgsGetString(out->args_json, "button", &button);
	return Validation::ValidateAgentRequest(draft);
}

bool ParseRequestLine(const char* line, Request* out, ErrorInfo* error)
{
	if (error == nullptr)
	{
		return false;
	}
	*error            = ErrorInfo {};
	const auto result = ParseAndValidateRequestLine(line, out);
	if (result.Ok())
	{
		return true;
	}
	error->code    = result.error.code;
	error->message = result.error.reason;
	return false;
}

bool ArgsGetString(const std::string& args_json, const char* key, std::string* out)
{
	const char* value = nullptr;
	const char* end   = nullptr;
	if (!FindObjectField(args_json.c_str(), key, &value) || !ParseStringValue(value, out, &end))
	{
		return false;
	}
	return true;
}

bool ArgsGetU64(const std::string& args_json, const char* key, uint64_t* out)
{
	const char* value = nullptr;
	const char* end   = nullptr;
	if (!FindObjectField(args_json.c_str(), key, &value) || !ParseU64Value(value, out, &end))
	{
		return false;
	}
	return true;
}

bool ArgsGetU32(const std::string& args_json, const char* key, uint32_t* out)
{
	uint64_t value = 0;
	if (!ArgsGetU64(args_json, key, &value) || value > UINT32_MAX)
	{
		return false;
	}
	*out = static_cast<uint32_t>(value);
	return true;
}

bool ArgsGetBool(const std::string& args_json, const char* key, bool* out)
{
	const char* value = nullptr;
	if (!FindObjectField(args_json.c_str(), key, &value))
	{
		return false;
	}
	value = SkipWs(value);
	if (std::strncmp(value, "true", 4) == 0)
	{
		*out = true;
		return true;
	}
	if (std::strncmp(value, "false", 5) == 0)
	{
		*out = false;
		return true;
	}
	return false;
}

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED
