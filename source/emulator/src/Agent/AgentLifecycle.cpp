#include "Emulator/Agent/AgentLifecycle.h"

#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/DbgAssert.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent::Lifecycle {
namespace {

std::atomic_bool g_hooks_installed {false};
std::atomic_bool g_graphics_init_emitted {false};
std::atomic_bool g_first_frame_emitted {false};
std::atomic_bool g_first_present_emitted {false};
std::atomic_bool g_input_ready_emitted {false};
std::atomic<uint32_t> g_stencil_frontier_emits {0};
std::atomic<uint32_t> g_storage_frontier_emits {0};

constexpr uint32_t kGraphicsFrontierEmitLimit = 64;

bool IsPathDelimiter(char ch)
{
	return ch == '\0' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '"' || ch == '\'' ||
	       ch == ',' || ch == ';';
}

bool IsWindowsAbsolutePath(const char* value)
{
	return value != nullptr && std::isalpha(static_cast<unsigned char>(value[0])) != 0 && value[1] == ':' &&
	       (value[2] == '/' || value[2] == '\\');
}

bool IsTitleId(const char* value)
{
	if (value == nullptr)
	{
		return false;
	}
	for (int i = 0; i < 4; ++i)
	{
		if (std::isupper(static_cast<unsigned char>(value[i])) == 0)
		{
			return false;
		}
	}
	for (int i = 4; i < 9; ++i)
	{
		if (std::isdigit(static_cast<unsigned char>(value[i])) == 0)
		{
			return false;
		}
	}
	return true;
}

void AppendLiteral(const char* value, char* out, size_t cap, size_t* written)
{
	for (size_t i = 0; value[i] != '\0' && *written + 1 < cap; ++i)
	{
		out[(*written)++] = value[i];
	}
}

// Strip absolute paths and title identifiers so private guest roots never leave the process.
void SanitizeMessage(const char* in, char* out, size_t cap)
{
	if (out == nullptr || cap == 0)
	{
		return;
	}
	if (in == nullptr)
	{
		out[0] = '\0';
		return;
	}
	size_t w = 0;
	for (size_t i = 0; in[i] != '\0' && w + 1 < cap;)
	{
		if (in[i] == '/' || IsWindowsAbsolutePath(in + i))
		{
			AppendLiteral("$HOST_PATH", out, cap, &w);
			while (!IsPathDelimiter(in[i]))
			{
				++i;
			}
			continue;
		}
		if (IsTitleId(in + i))
		{
			AppendLiteral("$TITLE_ID", out, cap, &w);
			i += 9;
			continue;
		}
		out[w++] = in[i++];
	}
	out[w] = '\0';
}

void BringUpDecisionHook(Core::BringUp::Feature feature, Core::BringUp::Subsystem /*subsystem*/,
                         Core::BringUp::Decision decision, const char* identity,
                         bool breaker) noexcept
{
	char msg[kAgentEventMessageMax] {};
	char id_buf[96] {};
	SanitizeMessage(identity != nullptr ? identity : "", id_buf, sizeof(id_buf));
	const char* feat = "feature";
	switch (feature)
	{
		case Core::BringUp::Feature::NotImplemented: feat = "not_implemented"; break;
		case Core::BringUp::Feature::MissingFunctionImport: feat = "missing_function_import"; break;
		case Core::BringUp::Feature::GraphicsPermissive: feat = "gfx_permissive"; break;
		case Core::BringUp::Feature::AdjacentModuleDiscovery: feat = "adjacent_module_discovery"; break;
	}
	if (breaker)
	{
		std::snprintf(msg, sizeof(msg), "breaker feature=%s id=%s", feat, id_buf);
		Emit(EventKind::Error, kCodeBringUpBreaker, msg);
		return;
	}
	if (decision == Core::BringUp::Decision::Continue)
	{
		std::snprintf(msg, sizeof(msg), "continue feature=%s id=%s", feat, id_buf);
		Emit(EventKind::Info, kCodeBringUpContinue, msg);
		return;
	}
	std::snprintf(msg, sizeof(msg), "halt feature=%s id=%s", feat, id_buf);
	Emit(EventKind::Error, kCodeBringUpHalt, msg);
}

} // namespace

void Emit(EventKind kind, const char* code, const char* message)
{
	char sanitized[kAgentEventMessageMax] {};
	SanitizeMessage(message, sanitized, sizeof(sanitized));
	EventRing::Instance().Push(kind, code != nullptr ? code : "", sanitized);
}

void EmitStartupConfig(const char* mode_name, bool explicitly_configured)
{
	char msg[96];
	std::snprintf(msg, sizeof(msg), "mode=%s explicit=%s", mode_name != nullptr ? mode_name : "strict",
	              explicitly_configured ? "true" : "false");
	Emit(EventKind::Info, kCodeStartupConfig, msg);
}

void EmitExecutableDiscovered(const char* sanitized_relative_or_basename)
{
	char msg[96];
	std::snprintf(msg, sizeof(msg), "primary=%s",
	              sanitized_relative_or_basename != nullptr ? sanitized_relative_or_basename : "");
	Emit(EventKind::Info, kCodeExecutableDiscovered, msg);
}

void EmitModuleDiscovery(uint32_t entry_count, uint32_t adjacent_count, uint32_t rejections)
{
	char msg[96];
	std::snprintf(msg, sizeof(msg), "entries=%u adjacent=%u rejections=%u", entry_count, adjacent_count,
	              rejections);
	Emit(EventKind::Info, kCodeModuleDiscovery, msg);
}

void EmitModuleLoaded(const char* sanitized_relative_key)
{
	char msg[128];
	std::snprintf(msg, sizeof(msg), "module=%s", sanitized_relative_key != nullptr ? sanitized_relative_key : "");
	Emit(EventKind::Info, kCodeModuleLoaded, msg);
}

void EmitMissingImport(const char* sanitized_identity)
{
	char msg[kAgentEventMessageMax];
	std::snprintf(msg, sizeof(msg), "identity=%s", sanitized_identity != nullptr ? sanitized_identity : "");
	Emit(EventKind::Warn, kCodeMissingImport, msg);
}

void EmitSymbolResolved(const char* sanitized_identity, const char* source)
{
	// Bound volume: first 256 successful resolves per process (ring is also bounded).
	static std::atomic<uint32_t> emits {0};
	if (emits.fetch_add(1, std::memory_order_relaxed) >= 256u)
	{
		return;
	}
	char msg[kAgentEventMessageMax];
	std::snprintf(msg, sizeof(msg), "source=%s identity=%s", source != nullptr ? source : "unknown",
	              sanitized_identity != nullptr ? sanitized_identity : "");
	Emit(EventKind::Info, kCodeSymbolResolved, msg);
}

void EmitRelocationFailure(const char* sanitized_symbol)
{
	char msg[kAgentEventMessageMax];
	std::snprintf(msg, sizeof(msg), "symbol=%s", sanitized_symbol != nullptr ? sanitized_symbol : "");
	Emit(EventKind::Error, kCodeRelocationFailure, msg);
}

void EmitGraphicsInit()
{
	bool expected = false;
	if (g_graphics_init_emitted.compare_exchange_strong(expected, true, std::memory_order_relaxed))
	{
		Emit(EventKind::Info, kCodeGraphicsInit, "graphic_ready");
	}
}

void EmitStencilFrontier(const StencilFrontierContext& context)
{
	if (g_stencil_frontier_emits.fetch_add(1, std::memory_order_relaxed) >= kGraphicsFrontierEmitLimit)
	{
		return;
	}
	char msg[kAgentEventMessageMax] {};
	std::snprintf(msg, sizeof(msg),
	              "test=%u clear=%u htile=%u zdecomp=%u sdecomp=%u resum=%u copyc=%u copys=%u ro=%u rb=%u wb=%u",
	              context.stencil_enable ? 1u : 0u, context.clear_enable ? 1u : 0u,
	              context.htile ? 1u : 0u, context.depth_decompress ? 1u : 0u,
	              context.stencil_decompress ? 1u : 0u, context.resummarize ? 1u : 0u,
	              context.copy_centroid ? 1u : 0u, static_cast<unsigned>(context.copy_sample),
	              context.read_only ? 1u : 0u,
	              context.read_base_present ? 1u : 0u, context.write_base_present ? 1u : 0u);
	Emit(EventKind::Error, kCodeGraphicsStencilFrontier, msg);
}

static void FormatStorageFrontier(const StorageFrontierContext& context, char* message, size_t message_size)
{
	const char* access = "unknown";
	switch (context.access)
	{
		case StorageAccessClass::Unknown: break;
		case StorageAccessClass::Raw: access = "raw"; break;
		case StorageAccessClass::Typed: access = "typed"; break;
		case StorageAccessClass::Mixed: access = "mixed"; break;
	}
	const char* source = context.source == StorageBindingSource::Metadata ? "metadata" : "direct";
	const char* reason = "none";
	switch (context.unknown_reason)
	{
		case StorageUnknownReason::None: break;
		case StorageUnknownReason::CodeUnavailable: reason = "code_unavailable"; break;
		case StorageUnknownReason::NoMatchingInstruction: reason = "no_matching_instruction"; break;
		case StorageUnknownReason::RegisterBaseMismatch: reason = "register_base_mismatch"; break;
		case StorageUnknownReason::MetadataOnlyBinding: reason = "metadata_only"; break;
	}
	std::snprintf(message, message_size,
	              "access=%s source=%s reason=%s code=%u exact=%u unbased=%u decoded=%u indirect=%u "
	              "idx=%d sgpr=%d slot=%d usage=%u stride=%u fmt=%u "
	              "dst=0x%03x tid=%u swz=%u",
	              access, source, reason, context.code_available ? 1u : 0u, context.exact_match ? 1u : 0u,
	              context.unbased_match ? 1u : 0u, context.decoded_unknown ? 1u : 0u, context.indirect_use ? 1u : 0u,
	              context.resource_index, context.sgpr, context.slot,
	              context.usage, context.stride, context.format, context.dst_sel, context.add_tid ? 1u : 0u,
	              context.swizzle ? 1u : 0u);
}

void EmitStorageFrontier(const StorageFrontierContext& context)
{
	if (g_storage_frontier_emits.fetch_add(1, std::memory_order_relaxed) >= kGraphicsFrontierEmitLimit)
	{
		return;
	}
	char msg[kAgentEventMessageMax] {};
	FormatStorageFrontier(context, msg, sizeof(msg));
	Emit(EventKind::Error, kCodeGraphicsStorageFrontier, msg);
}

void EmitStorageFrontierFatal(const StorageFrontierContext& context)
{
	char msg[kAgentEventMessageMax] {};
	FormatStorageFrontier(context, msg, sizeof(msg));
	EmitStorageFrontier(context);
	std::fprintf(stderr, "KYTY_AGENT_FRONTIER code=%s %s\n", kCodeGraphicsStorageFrontier, msg);
	std::fflush(stderr);
}

void EmitFirstFrame(int frame)
{
	bool expected = false;
	if (g_first_frame_emitted.compare_exchange_strong(expected, true, std::memory_order_relaxed))
	{
		char msg[32];
		std::snprintf(msg, sizeof(msg), "frame=%d", frame);
		Emit(EventKind::Info, kCodeFirstFrame, msg);
	}
}

void EmitFirstPresent(uint64_t present)
{
	bool expected = false;
	if (g_first_present_emitted.compare_exchange_strong(expected, true, std::memory_order_relaxed))
	{
		char msg[48];
		std::snprintf(msg, sizeof(msg), "present=%llu", static_cast<unsigned long long>(present));
		Emit(EventKind::Present, kCodeFirstPresent, msg);
	}
}

void EmitInputReady()
{
	bool expected = false;
	if (g_input_ready_emitted.compare_exchange_strong(expected, true, std::memory_order_relaxed))
	{
		Emit(EventKind::Input, kCodeInputReady, "pad_overlay_ready");
	}
}

void EmitGuestExit(int status)
{
	char msg[48];
	std::snprintf(msg, sizeof(msg), "status=%d", status);
	Emit(EventKind::Info, kCodeGuestExit, msg);
}

void EmitHostCrash(const char* sanitized_hint)
{
	Emit(EventKind::Fatal, kCodeHostCrash, sanitized_hint != nullptr ? sanitized_hint : "host_fault");
}

const char* ClassifyHostFaultCode(const char* host_fault_code)
{
	if (host_fault_code != nullptr && std::strcmp(host_fault_code, "not_implemented_halt") == 0)
	{
		return kCodeBringUpHalt;
	}
	return kCodeHostCrash;
}

void InstallHooks()
{
	bool expected = false;
	if (!g_hooks_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
	{
		return;
	}
	Core::BringUp::SetDecisionHook(+[](Core::BringUp::Feature feature, Core::BringUp::Subsystem subsystem,
	                                   Core::BringUp::Decision decision, const char* identity,
	                                   bool breaker) noexcept {
		BringUpDecisionHook(feature, subsystem, decision, identity, breaker);
	});
	// Sync host-fault path (assert / EXIT / EXIT_IF / dbg_exit). Not used from
	// async-signal-only FatalFault (that path stays signal-safe).
	Core::SetHostFaultHook(+[](const char* code, const char* message) noexcept {
		char msg[kAgentEventMessageMax] {};
		std::snprintf(msg, sizeof(msg), "code=%s %s", code != nullptr ? code : "host_fault",
		              message != nullptr ? message : "");
		const char* event_code = ClassifyHostFaultCode(code);
		Emit(std::strcmp(event_code, kCodeHostCrash) == 0 ? EventKind::Fatal : EventKind::Error, event_code, msg);
	});
}

const char* ClassifyFrontier(const char* phase_name, bool has_last_error, const char* last_error_code,
                             bool graphic_ready, uint64_t present)
{
	if (has_last_error && last_error_code != nullptr)
	{
		if (std::strcmp(last_error_code, kCodeBringUpHalt) == 0 ||
		    std::strcmp(last_error_code, kCodeBringUpBreaker) == 0 ||
		    std::strcmp(last_error_code, "unsupported") == 0)
		{
			return "unsupported";
		}
		if (std::strcmp(last_error_code, kCodeHostCrash) == 0 || std::strcmp(last_error_code, "fatal") == 0)
		{
			return "launch";
		}
		if (phase_name != nullptr && std::strcmp(phase_name, "interactive") == 0)
		{
			return "interactive";
		}
		if (graphic_ready && present > 0 && std::strstr(last_error_code, "stall") == nullptr &&
		    std::strcmp(last_error_code, "timeout") != 0)
		{
			return "graphics";
		}
		if (std::strstr(last_error_code, "stall") != nullptr || std::strcmp(last_error_code, "timeout") == 0)
		{
			return "stall";
		}
		return "error";
	}
	if (phase_name != nullptr && std::strcmp(phase_name, "stalled") == 0)
	{
		return "stall";
	}
	if (phase_name != nullptr && std::strcmp(phase_name, "interactive") == 0)
	{
		return "interactive";
	}
	if (graphic_ready && present > 0)
	{
		return "graphics";
	}
	if (!graphic_ready)
	{
		return "launch";
	}
	return "none";
}

} // namespace Kyty::Emulator::Agent::Lifecycle

#endif // KYTY_EMU_ENABLED
