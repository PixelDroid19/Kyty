#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/Math/MathAll.h"

#include "Emulator/Agent/AgentLifecycle.h"
#include "Emulator/Agent/AgentServer.h"
#include "Emulator/Agent/EventRing.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/GpuWriteHistory.h"
#include "Emulator/Graphics/NativeCapture.h"
#include "Emulator/Graphics/Objects/DepthMeta.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/Texture.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Graphics/ShaderTranslationCache.h"

#include "../../emulator/src/Graphics/GraphicsRenderInternal.h"
#include "../../emulator/src/Graphics/GraphicsRunInternal.h"
#include "../../emulator/src/Graphics/Objects/GpuMemoryInternal.h"
#include "Emulator/Log.h"

#include "spirv-tools/libspirv.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

using namespace Kyty::Emulator::Agent;
using namespace Kyty::Libs::Graphics;

namespace {

[[noreturn]] void Die(const char* message)
{
	std::fprintf(stderr, "graphics diagnostics integration failure: %s\n", message);
	std::_Exit(1);
}

void Expect(bool condition, const char* message)
{
	if (!condition)
	{
		Die(message);
	}
}

void ExpectValidSpirv(const Kyty::Core::String8& source, const char* message);
void ExpectValidSpirv(const Kyty::Vector<uint32_t>& binary, const char* message);

void TestSetEnvironment(const char* name, const char* value)
{
#if defined(_WIN32)
	Expect(_putenv_s(name, value) == 0, "test environment variable must be set");
#else
	Expect(::setenv(name, value, 1) == 0, "test environment variable must be set");
#endif
}

void TestUnsetEnvironment(const char* name)
{
#if defined(_WIN32)
	Expect(_putenv_s(name, "") == 0, "test environment variable must be cleared");
#else
	Expect(::unsetenv(name) == 0, "test environment variable must be cleared");
#endif
}

void VerifyRenderTargetLifetimeAgentArmGate()
{
	using ArmState  = RenderTargetLifetimeAgentArmState;
	using ArmResult = RenderTargetLifetimeAgentArmRequestResult;

	std::atomic<ArmState> disabled {ArmState::Disabled};
	Expect(RenderTargetLifetimeAgentArmGateOpen(&disabled, true),
	       "missing agent opt-in preserves the existing lifetime trace gate");
	Expect(RenderTargetLifetimeAgentArmRequest(&disabled) == ArmResult::Disabled,
	       "disabled agent arming fails closed without changing the existing trace mode");

	std::atomic<ArmState> gate {ArmState::Idle};
	Expect(!RenderTargetLifetimeAgentArmGateOpen(&gate, true),
	       "agent opt-in keeps lifetime tracing closed before an explicit request");
	Expect(RenderTargetLifetimeAgentArmRequest(&gate) == ArmResult::Armed &&
	           RenderTargetLifetimeAgentArmRequest(&gate) == ArmResult::AlreadyPending,
	       "one explicit request owns the pending state");
	Expect(!RenderTargetLifetimeAgentArmGateOpen(&gate, false) && gate.load(std::memory_order_acquire) == ArmState::Pending,
	       "an ineligible minimum-present boundary does not consume the pending request");
	Expect(RenderTargetLifetimeAgentArmGateOpen(&gate, true) && gate.load(std::memory_order_acquire) == ArmState::Open &&
	           RenderTargetLifetimeAgentArmRequest(&gate) == ArmResult::AlreadyOpen,
	       "eligible render activity atomically consumes pending into the terminal open state");

	for (uint32_t iteration = 0; iteration < 256u; ++iteration)
	{
		std::atomic<ArmState> raced {ArmState::Idle};
		Expect(RenderTargetLifetimeAgentArmRequest(&raced) == ArmResult::Armed,
		       "race fixture starts with one pending request");
		std::atomic_bool start {false};
		bool             opened = false;
		ArmResult        duplicate = ArmResult::Armed;
		std::thread render([&]() {
			while (!start.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			opened = RenderTargetLifetimeAgentArmGateOpen(&raced, true);
		});
		std::thread agent([&]() {
			while (!start.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			duplicate = RenderTargetLifetimeAgentArmRequest(&raced);
		});
		start.store(true, std::memory_order_release);
		render.join();
		agent.join();
		Expect(opened && raced.load(std::memory_order_acquire) == ArmState::Open &&
		           (duplicate == ArmResult::AlreadyPending || duplicate == ArmResult::AlreadyOpen),
		       "agent/render interleaving never accepts a second one-shot request");
	}
}

void VerifyRenderTargetLifetimeAgentArmServerPublication()
{
	TestUnsetEnvironment("KYTY_TRACE_RT_LIFETIME");
	TestUnsetEnvironment("KYTY_AGENT_TRACE_RT_LIFETIME_ARM");
	const std::string before_render = Kyty::Emulator::Agent::Internal::DispatchLine(
	    R"({"id":31,"tool":"trace_rt_lifetime_arm","args":{}})");
	Expect(before_render.find("\"code\":\"not_ready\"") != std::string::npos,
	       "agent arm is retryable only before the render thread publishes immutable trace configuration");

	GraphicsInitializeRenderTargetLifetimeTraceOnRenderThread();
	const std::string trace_disabled = Kyty::Emulator::Agent::Internal::DispatchLine(
	    R"({"id":32,"tool":"trace_rt_lifetime_arm","args":{}})");
	Expect(trace_disabled.find("\"code\":\"trace_disabled\"") != std::string::npos,
	       "render-owned initialization reports a missing master trace opt-in as disabled, not perpetually not-ready");
	const std::string invalid_args = Kyty::Emulator::Agent::Internal::DispatchLine(
	    R"({"id":33,"tool":"trace_rt_lifetime_arm","args":{"extra":1}})");
	Expect(invalid_args.find("\"code\":\"invalid_args\"") != std::string::npos,
	       "agent arm rejects non-empty arguments before touching graphics state");
}

void VerifyRenderTargetLifetimeDepthFilter()
{
	constexpr uint64_t address_a = 0x000000001234abc0ull;
	constexpr uint64_t address_b = 0x000000005678def0ull;
	constexpr uint32_t width     = 320u;
	constexpr uint32_t height    = 240u;
	constexpr uint32_t format    = 130u;
	std::array<uint32_t, 8> zero_meta {};
	std::array<uint32_t, 8> clear_meta {};
	clear_meta.fill(0xfffffff0u);
	DepthMetaPatternSnapshot meta_snapshot {};
	Expect(DepthMetaInspectPattern(zero_meta.data(), sizeof(zero_meta), &meta_snapshot) &&
	           meta_snapshot.kind == DepthMetaPatternKind::UniformZero && meta_snapshot.first_word == 0u &&
	           meta_snapshot.word_count == zero_meta.size(),
	       "lifetime diagnostics classify a complete uniform-zero HTILE plane without treating it as the legacy clear pattern");
	Expect(DepthMetaInspectPattern(clear_meta.data(), sizeof(clear_meta), &meta_snapshot) &&
	           meta_snapshot.kind == DepthMetaPatternKind::RecognizedClear && meta_snapshot.first_word == 0xfffffff0u &&
	           meta_snapshot.word_count == clear_meta.size(),
	       "lifetime diagnostics identify the exact complete HTILE pattern used by the existing clear tracker");
	clear_meta[3] = 0u;
	Expect(DepthMetaInspectPattern(clear_meta.data(), sizeof(clear_meta), &meta_snapshot) &&
	           meta_snapshot.kind == DepthMetaPatternKind::Mixed && meta_snapshot.first_word == 0xfffffff0u,
	       "lifetime diagnostics distinguish a mixed HTILE plane from a complete recognized clear");
	constexpr uint64_t meta_address = 0x0000000067890000ull;
	DepthMetaMarkClear(meta_address, DepthMetaClearSource::SyntheticImageCreate);
	DepthMetaClearEvent clear_event {};
	Expect(DepthMetaConsumeClear(meta_address, &clear_event) && clear_event.valid &&
	           clear_event.source == DepthMetaClearSource::SyntheticImageCreate,
	       "lifetime diagnostics retain the synthetic source of a consumed image-creation clear");
	DepthMetaTraceSnapshot trace_snapshot {};
	Expect(DepthMetaQueryTraceState(meta_address, &trace_snapshot) && !trace_snapshot.pending &&
	           trace_snapshot.has_last_consumed && trace_snapshot.last_consumed.sequence == clear_event.sequence &&
	           trace_snapshot.last_consumed.source == DepthMetaClearSource::SyntheticImageCreate,
	       "lifetime diagnostics expose the last consumed clear after the one-shot pending state is erased");
	DepthMetaStorageIdentity producer_identity {
	    .address = meta_address + 0x1000u,
	    .size = sizeof(zero_meta),
	    .logical_generation = 7u,
	    .backing_generation = 11u,
	    .producer_or_consumer_submit = 41u,
	};
	Expect(DepthMetaPublishComputeFill(producer_identity, 0u),
	       "a complete compute metadata fill publishes an identity-carrying pending event");
	auto wrong_generation = producer_identity;
	wrong_generation.logical_generation++;
	wrong_generation.producer_or_consumer_submit = 42u;
	Expect(!DepthMetaConsumeClear(wrong_generation),
	       "an address-reused storage generation cannot consume a compute metadata fill");
	Expect(DepthMetaPublishComputeFill(producer_identity, 0u),
	       "the exact producer can republish after a rejected address-reuse candidate");
	auto consumer_identity = producer_identity;
	consumer_identity.producer_or_consumer_submit = 42u;
	DepthMetaClearEvent compute_event {};
	Expect(DepthMetaConsumeClear(consumer_identity, &compute_event) && compute_event.valid &&
	           compute_event.source == DepthMetaClearSource::ComputeMetadataFill &&
	           compute_event.address == producer_identity.address && compute_event.size == producer_identity.size &&
	           compute_event.logical_generation == producer_identity.logical_generation &&
	           compute_event.backing_generation == producer_identity.backing_generation &&
	           compute_event.producer_submit == producer_identity.producer_or_consumer_submit &&
	           compute_event.pattern.kind == DepthMetaPatternKind::UniformZero && compute_event.pattern.first_word == 0u &&
	           !DepthMetaConsumeClear(consumer_identity),
	       "the exact later consumer receives the complete zero fill once with producer ordering intact");
	Expect(DepthMetaPublishComputeFill(producer_identity, 0u),
	       "a compute metadata fill can be republished for semantic-rejection coverage");
	DepthMetaTraceSnapshot rejected_snapshot {};
	Expect(DepthMetaQueryTraceState(producer_identity.address, &rejected_snapshot) && rejected_snapshot.pending &&
	           DepthMetaDiscardComputeFill(consumer_identity, rejected_snapshot.pending_event.sequence) &&
	           !DepthMetaConsumeClear(consumer_identity),
	       "an exact compute fill rejected by the first attachment use cannot clear a later use");
	RenderTargetLifetimeDepthFilter filter {};

	Expect(ParseRenderTargetLifetimeDepthFilter(nullptr, nullptr, nullptr, &filter) && !filter.enabled,
	       "missing render-target lifetime depth filters preserve unfiltered tracing");
	Expect(ParseRenderTargetLifetimeDepthFilter(nullptr, "320x240", "130", &filter) && filter.enabled &&
	           filter.extent_enabled && filter.width == width && filter.height == height && filter.format_enabled &&
	           filter.format == format && RenderTargetLifetimeDepthFilterMatches(filter, address_a, width, height, format) &&
	           !RenderTargetLifetimeDepthFilterMatches(filter, address_b, width + 1u, height, format) &&
	           !RenderTargetLifetimeDepthFilterMatches(filter, address_a, width, height, format + 1u),
	       "render-target lifetime depth extent and format filters parse and reject B");
	RenderTargetLifetimeDepthAddressTraceState trace {};
	constexpr uint64_t host_id = 7u;
	const auto first_a = RenderTargetLifetimeDepthAddressTraceArm(filter, address_a, width, height, format, host_id, 10, &trace);
	Expect(first_a.accepted && first_a.newly_armed && !first_a.remapped && trace.guest_addr == address_a &&
	           trace.host_id == host_id && trace.armed_present == 10,
	       "first selected depth address arms one lifetime slot and epoch");
	const auto filtered_b =
	    RenderTargetLifetimeDepthAddressTraceArm(filter, address_b, width + 1u, height, format, host_id + 1u, 11, &trace);
	Expect(!filtered_b.accepted && !filtered_b.newly_armed && !filtered_b.remapped && trace.guest_addr == address_a &&
	           trace.host_id == host_id && trace.armed_present == 10,
	       "filtered B consumes no lifetime slot budget or armed epoch");
	const auto later_a = RenderTargetLifetimeDepthAddressTraceArm(filter, address_a, width, height, format, host_id, 12, &trace);
	Expect(later_a.accepted && !later_a.newly_armed && !later_a.remapped && trace.armed_present == 10 &&
	           RenderTargetLifetimeDepthAddressTraceUseEligible(filter, address_a, width, height, format, host_id, 12, trace) &&
	           !RenderTargetLifetimeDepthAddressTraceUseEligible(filter, address_a, width, height, format, host_id, 10, trace),
	       "later selected A can emit depth use while its armed present remains guarded");
	Expect(ParseRenderTargetLifetimeDepthFilter("0x000000001234abc0", "320x240", nullptr, &filter) &&
	           RenderTargetLifetimeDepthFilterMatches(filter, address_a, width, height, format) &&
	           !RenderTargetLifetimeDepthFilterMatches(filter, address_b, width, height, format) &&
	           !RenderTargetLifetimeDepthFilterMatches(filter, address_a, width + 1u, height, format),
	       "render-target lifetime depth address and extent filters combine with AND");
	Expect(!ParseRenderTargetLifetimeDepthFilter("0", nullptr, nullptr, &filter) && !filter.enabled,
	       "render-target lifetime depth address filter rejects zero");
	Expect(!ParseRenderTargetLifetimeDepthFilter(nullptr, "4294967296x240", nullptr, &filter) && !filter.enabled,
	       "render-target lifetime depth extent filter rejects overflow");
	Expect(!ParseRenderTargetLifetimeDepthFilter(nullptr, "320x240junk", nullptr, &filter) && !filter.enabled,
	       "render-target lifetime depth extent filter rejects trailing input");
	Expect(!ParseRenderTargetLifetimeDepthFilter(nullptr, nullptr, "4294967296", &filter) && !filter.enabled,
	       "render-target lifetime depth format filter rejects overflow");
	Expect(!ParseRenderTargetLifetimeDepthFilter(nullptr, nullptr, "0", &filter) && !filter.enabled,
	       "render-target lifetime depth format filter rejects zero");
	RenderTargetLifetimeColorAddressFilter color_filter {};
	Expect(ParseRenderTargetLifetimeColorAddressFilter(nullptr, &color_filter) && !color_filter.enabled,
	       "missing render-target lifetime color address preserves unfiltered tracing");
	Expect(ParseRenderTargetLifetimeColorAddressFilter("0x1234abc0", &color_filter) && color_filter.enabled &&
	           color_filter.base_addr == address_a && RenderTargetLifetimeColorAddressFilterMatches(color_filter, address_a) &&
	           !RenderTargetLifetimeColorAddressFilterMatches(color_filter, address_b),
	       "render-target lifetime color address accepts hexadecimal and rejects other attachments");
	Expect(ParseRenderTargetLifetimeColorAddressFilter("305441728", &color_filter) && color_filter.enabled &&
	           color_filter.base_addr == address_a && RenderTargetLifetimeColorAddressFilterMatches(color_filter, address_a),
	       "render-target lifetime color address accepts decimal guest provenance");
	Expect(!ParseRenderTargetLifetimeColorAddressFilter("0", &color_filter) && !color_filter.enabled,
	       "render-target lifetime color address rejects zero");
	Expect(!ParseRenderTargetLifetimeColorAddressFilter("0x", &color_filter) && !color_filter.enabled,
	       "render-target lifetime color address rejects an empty hexadecimal value");
	Expect(!ParseRenderTargetLifetimeColorAddressFilter("0x10000000000000000", &color_filter) && !color_filter.enabled,
	       "render-target lifetime color address rejects overflow");
	Expect(!ParseRenderTargetLifetimeColorAddressFilter("305441728junk", &color_filter) && !color_filter.enabled,
	       "render-target lifetime color address rejects trailing input");
	RenderTargetLifetimeColorFormatFilter color_format_filter {};
	Expect(ParseRenderTargetLifetimeColorFormatFilter(nullptr, &color_format_filter) && !color_format_filter.enabled,
	       "missing render-target lifetime color format preserves unfiltered tracing");
	Expect(ParseRenderTargetLifetimeColorFormatFilter("122", &color_format_filter) && color_format_filter.enabled &&
	           color_format_filter.format == 122u && RenderTargetLifetimeColorFormatFilterMatches(color_format_filter, 122u) &&
	           !RenderTargetLifetimeColorFormatFilterMatches(color_format_filter, 50u),
	       "render-target lifetime color format accepts decimal and rejects other formats");
	Expect(ParseRenderTargetLifetimeColorFormatFilter("0x7a", &color_format_filter) && color_format_filter.enabled &&
	           color_format_filter.format == 122u && RenderTargetLifetimeColorFormatFilterMatches(color_format_filter, 122u),
	       "render-target lifetime color format accepts hexadecimal");
	Expect(ParseRenderTargetLifetimeColorAddressFilter("0x1234abc0", &color_filter),
	       "render-target lifetime color selector restores its address fixture");
	Expect(RenderTargetLifetimeColorSelectorEnabled(color_filter, color_format_filter) &&
	           RenderTargetLifetimeColorSelectorMatches(color_filter, color_format_filter, address_a, 122u) &&
	           !RenderTargetLifetimeColorSelectorMatches(color_filter, color_format_filter, address_b, 122u) &&
	           !RenderTargetLifetimeColorSelectorMatches(color_filter, color_format_filter, address_a, 50u),
	       "render-target lifetime color selector applies address and format conjunctively to arm and promotion");
	RenderTargetLifetimeDepthFilter no_depth_filter {};
	RenderTargetLifetimeDepthFilter selected_depth_filter {};
	Expect(ParseRenderTargetLifetimeDepthFilter(nullptr, nullptr, "130", &selected_depth_filter),
	       "render-target lifetime selector restores its depth fixture");
	Expect(RenderTargetLifetimeTraceSelectorsCompatible(no_depth_filter, color_filter, color_format_filter, false) &&
	           !RenderTargetLifetimeTraceSelectorsCompatible(selected_depth_filter, color_filter, color_format_filter, false) &&
	           !RenderTargetLifetimeTraceSelectorsCompatible(no_depth_filter, color_filter, color_format_filter, true),
	       "render-target lifetime color selection excludes depth and MRT probe tracing");
	Expect(!ParseRenderTargetLifetimeColorFormatFilter("0", &color_format_filter) && !color_format_filter.enabled,
	       "render-target lifetime color format rejects zero");
	Expect(!ParseRenderTargetLifetimeColorFormatFilter("0x", &color_format_filter) && !color_format_filter.enabled,
	       "render-target lifetime color format rejects an empty hexadecimal value");
	Expect(!ParseRenderTargetLifetimeColorFormatFilter("0x100000000", &color_format_filter) && !color_format_filter.enabled,
	       "render-target lifetime color format rejects hexadecimal overflow");
	Expect(!ParseRenderTargetLifetimeColorFormatFilter("0x7g", &color_format_filter) && !color_format_filter.enabled,
	       "render-target lifetime color format rejects malformed hexadecimal input");
	Expect(!ParseRenderTargetLifetimeColorFormatFilter("4294967296", &color_format_filter) && !color_format_filter.enabled,
	       "render-target lifetime color format rejects overflow");
	Expect(!ParseRenderTargetLifetimeColorFormatFilter("122junk", &color_format_filter) && !color_format_filter.enabled,
	       "render-target lifetime color format rejects trailing input");
	uint32_t after_capture_ordinal = 0;
	Expect(ParseRenderTargetLifetimeAfterCapture(nullptr, &after_capture_ordinal) && after_capture_ordinal == 0u &&
	           ParseRenderTargetLifetimeAfterCapture("4", &after_capture_ordinal) && after_capture_ordinal == 4u &&
	           RenderTargetLifetimeAfterCaptureEligible(after_capture_ordinal, 7u, 11u) &&
	           !RenderTargetLifetimeAfterCaptureEligible(after_capture_ordinal, 7u, 10u) &&
	           !ParseRenderTargetLifetimeAfterCapture("0", &after_capture_ordinal) && after_capture_ordinal == 0u &&
	           !ParseRenderTargetLifetimeAfterCapture("4294967296", &after_capture_ordinal) &&
	           after_capture_ordinal == 0u && !ParseRenderTargetLifetimeAfterCapture("true", &after_capture_ordinal) &&
	           after_capture_ordinal == 0u,
	       "render-target lifetime after-capture gate opens on the selected successful manual capture ordinal");
	Expect(NativeCapturePublishesManualGate(NativeCaptureMilestone::Manual, true, 1u) &&
	           !NativeCapturePublishesManualGate(NativeCaptureMilestone::FirstPresent, true, 1u) &&
	           !NativeCapturePublishesManualGate(NativeCaptureMilestone::Interval, true, 1u) &&
	           !NativeCapturePublishesManualGate(NativeCaptureMilestone::Manual, false, 1u) &&
	           !NativeCapturePublishesManualGate(NativeCaptureMilestone::Manual, true, 0u),
	       "only a successful explicit manual capture advances the lifetime trace gate generation");
	Expect(NativeCaptureSelectMilestone(true, true, true, true) == NativeCaptureMilestone::Manual &&
	           NativeCaptureSelectMilestone(false, true, true, true) == NativeCaptureMilestone::FirstPresent &&
	           NativeCaptureSelectMilestone(false, false, true, true) == NativeCaptureMilestone::Interval &&
	           NativeCaptureSelectMilestone(false, false, false, true) == NativeCaptureMilestone::Manual,
	       "a pending explicit capture cannot be consumed by automatic first-present or interval policy");
	const auto automatic_selection =
	    NativeCaptureSelectedManualRequestId(NativeCaptureMilestone::Interval, false, 0u);
	const auto manual_selection = NativeCaptureSelectedManualRequestId(NativeCaptureMilestone::Manual, true, 7u);
	Expect(automatic_selection == 0u && manual_selection == 7u &&
	           !NativeCaptureOwnsManualRequest(automatic_selection, true, 8u) &&
	           NativeCaptureOwnsManualRequest(manual_selection, true, 7u) &&
	           !NativeCaptureOwnsManualRequest(manual_selection, false, 7u) &&
	           !NativeCaptureOwnsManualRequest(manual_selection, true, 8u),
	       "automatic selection cannot consume a later request and manual publication preserves exact request ownership");
}

struct AliasContractGpuObject final: public GpuObject
{
	explicit AliasContractGpuObject(GpuMemoryObjectType object_type, bool is_read_only)
	{
		type      = object_type;
		params[0] = 0x414c494153000000ull ^ static_cast<uint64_t>(object_type);
		read_only = is_read_only;
	}

	bool Equal(const uint64_t* other) const override { return other != nullptr && other[0] == params[0]; }

	create_func_t GetCreateFunc() const override
	{
		return [](GraphicContext* /*ctx*/, const uint64_t* params, const uint64_t* /*vaddr*/, const uint64_t* /*size*/,
		          int /*vaddr_num*/, VulkanMemory* /*mem*/) -> void*
		{ return reinterpret_cast<void*>(params[0]); };
	}

	create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; }
	write_back_func_t          GetWriteBackFunc() const override { return nullptr; }
	delete_func_t              GetDeleteFunc() const override
	{
		return [](GraphicContext* /*ctx*/, void* /*obj*/, VulkanMemory* /*mem*/) {};
	}
	update_func_t GetUpdateFunc() const override { return nullptr; }
};

struct ProvenanceContractStorageGpuObject final: public GpuObject
{
	ProvenanceContractStorageGpuObject()
	{
		type      = GpuMemoryObjectType::StorageBuffer;
		params[0] = 0x50524f56454e414eull;
		read_only = false;
	}

	bool Equal(const uint64_t* other) const override { return other != nullptr && other[0] == params[0]; }

	create_func_t GetCreateFunc() const override
	{
		return [](GraphicContext* /*ctx*/, const uint64_t* params, const uint64_t* /*vaddr*/, const uint64_t* /*size*/,
		          int /*vaddr_num*/, VulkanMemory* /*mem*/) -> void*
		{ return reinterpret_cast<void*>(params[0]); };
	}

	create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; }
	write_back_func_t          GetWriteBackFunc() const override
	{
		return [](GraphicContext* /*ctx*/, const uint64_t* /*params*/, void* /*obj*/, const uint64_t* /*vaddr*/,
		          const uint64_t* size, int vaddr_num)
		{
			Expect(size != nullptr && vaddr_num == 1, "provenance fixture receives one write-back range");
			return GpuWritebackResult {.changed_pages = 1u, .copied_bytes = size[0], .content_changed = true};
		};
	}
	delete_func_t GetDeleteFunc() const override
	{
		return [](GraphicContext* /*ctx*/, void* /*obj*/, VulkanMemory* /*mem*/) {};
	}
	update_func_t GetUpdateFunc() const override { return nullptr; }
};

void VerifyRenderTargetIndexAliasContract()
{
	GpuWriteHistoryConfigureForTesting(0x4000u, 0x100u);
	GpuWriteHistoryRecord(GpuWriteHistoryKind::DmaData, 0x2000u, 0x20u, 1u, 0u, 0u);
	GpuWriteHistoryRecord(GpuWriteHistoryKind::WriteData, 0x4040u, 0x20u, 2u, 0u, 0u);
	GpuWriteHistoryRecord(GpuWriteHistoryKind::StorageWriteBack, 0x4080u, 0x40u, 3u,
	                     static_cast<uint32_t>(GpuMemoryObjectType::StorageBuffer), 7u);
	GpuWriteHistorySnapshot write_history {};
	Expect(GpuWriteHistoryQuery(0x4000u, 0x100u, &write_history) && write_history.enabled &&
	           write_history.entry_count == 2u && write_history.matching_count == 2u && write_history.covers_query &&
	           !write_history.entries_truncated &&
	           write_history.retained == 2u && write_history.dropped == 0u &&
	           write_history.total_by_kind[static_cast<uint32_t>(GpuWriteHistoryKind::DmaData)] == 1u &&
	           write_history.total_by_kind[static_cast<uint32_t>(GpuWriteHistoryKind::WriteData)] == 1u &&
	           write_history.total_by_kind[static_cast<uint32_t>(GpuWriteHistoryKind::StorageWriteBack)] == 1u &&
	           write_history.entries[0].sequence < write_history.entries[1].sequence &&
	           write_history.entries[1].content_sequence == 7u,
	       "bounded writer history distinguishes recorder activity from exact-range overlaps");
	GpuWriteHistoryConfigureForTesting(0u, 0u, true);
	GpuWriteHistoryRecord(GpuWriteHistoryKind::DmaData, 0x9000u, 0x20u, 4u, 0u, 0u);
	GpuWriteHistoryRecord(GpuWriteHistoryKind::WriteData, 0xa000u, 0x20u, 5u, 0u, 0u);
	GpuWriteHistorySnapshot automatic_history {};
	Expect(GpuWriteHistoryQuery(0xa000u, 0x20u, &automatic_history) && automatic_history.enabled &&
	           automatic_history.capture_all && automatic_history.covers_query && automatic_history.retained == 2u &&
	           automatic_history.matching_count == 1u && automatic_history.entry_count == 1u &&
	           automatic_history.entries[0].guest_addr == 0xa000u,
	       "automatic writer history retains bounded relocatable events and queries the draw's eventual guest range");
	GpuWriteHistoryConfigureForTesting(0u, 0u, false);

	GpuMap2 bounded_index;
	for (int id = 0; id < 18; ++id)
	{
		bounded_index.Insert(0x200000u, 0x1000u, id);
	}
	uint32_t bounded_visits = 0;
	const bool bounded_complete = bounded_index.VisitCandidatesBounded(
	    0x200000u, 0x1000u, 64u, 64u,
	    [&bounded_visits](int /*id*/)
	    {
		    bounded_visits++;
		    return bounded_visits <= GpuMemoryRangeProvenance::ENTRIES_MAX;
	    });
	Expect(!bounded_complete && bounded_visits == GpuMemoryRangeProvenance::ENTRIES_MAX + 1u,
	       "provenance candidate traversal stops after observing one entry beyond its output capacity");
	Expect(GpuMemoryCreationContentOrigin(GpuMemoryObjectType::VertexBuffer, false, false) ==
	               GpuMemoryContentOrigin::CpuUpload &&
	           GpuMemoryCreationContentOrigin(GpuMemoryObjectType::Texture, true, false) ==
	               GpuMemoryContentOrigin::GpuAliasMaterialization &&
	           GpuMemoryCreationContentOrigin(GpuMemoryObjectType::Texture, true, true) ==
	               GpuMemoryContentOrigin::CpuUpload &&
	           GpuMemoryCreationContentOrigin(GpuMemoryObjectType::RenderTexture, false, false) ==
	               GpuMemoryContentOrigin::Unknown,
	       "creation provenance distinguishes guest upload, GPU alias materialization, fallback, and unknown origins");

	Expect(!GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::IndexBuffer, GpuMemoryOverlapType::Crosses,
	                                                GpuMemoryObjectType::RenderTexture),
	       "the index-to-render-target alias contract does not admit an unobserved partial overlap");
	Expect(!GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::IndexBuffer, GpuMemoryOverlapType::Contains,
	                                                GpuMemoryObjectType::RenderTexture) &&
	           !GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::IndexBuffer, GpuMemoryOverlapType::Equals,
	                                                    GpuMemoryObjectType::RenderTexture),
	       "the index-to-render-target alias contract keeps reverse containment and equality strict");

	GpuMemoryInit();
	GraphicContext       ctx {};
	std::vector<uint8_t> guest(0x20000u);
	const uint64_t       heap_addr  = reinterpret_cast<uint64_t>(guest.data());
	const uint64_t       index_addr = heap_addr + 0x4800u;
	const uint64_t       index_size = 0x100u;
	const uint64_t       target_addr = heap_addr + 0x4000u;
	const uint64_t       target_size = 0x10000u;
	GpuMemorySetAllocatedRange(heap_addr, guest.size());

	Expect(GpuMemoryCreateObject(1u, &ctx, nullptr, index_addr, index_size,
	                             AliasContractGpuObject(GpuMemoryObjectType::IndexBuffer, true)) != nullptr,
	       "the integration fixture creates the contained index view first");
	Expect(GpuMemoryCreateObject(2u, &ctx, nullptr, target_addr, target_size,
	                             AliasContractGpuObject(GpuMemoryObjectType::RenderTexture, false)) != nullptr,
	       "the covering render target links without reclaiming the existing index view");
	Expect(GpuMemoryFindObjects(index_addr, index_size, GpuMemoryObjectType::IndexBuffer, true, false).Size() == 1u &&
	           GpuMemoryFindObjects(target_addr, target_size, GpuMemoryObjectType::RenderTexture, true, false).Size() == 1u,
	       "both typed views remain live after the covering render target is created");
	Expect(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::IndexBuffer,
	                                               GpuMemoryOverlapType::IsContainedWithin,
	                                               GpuMemoryObjectType::RenderTexture),
	       "the derived contained-index relation is the only accepted render-target alias form");

	GpuMemoryOverlapSnapshot overlaps {};
	Expect(GpuMemoryQueryOverlaps(&target_addr, &target_size, 1, &overlaps) && !overlaps.truncated && overlaps.total_count == 2u,
	       "the real range query retains both members of the alias topology");
	bool found_index_relation = false;
	bool found_target_relation = false;
	for (uint32_t i = 0; i < overlaps.entry_count; ++i)
	{
		const auto& entry = overlaps.entries[i];
		found_index_relation = found_index_relation ||
		                       (entry.type == GpuMemoryObjectType::IndexBuffer &&
		                        entry.relation == GpuMemoryOverlapType::IsContainedWithin && entry.count == 1u);
		found_target_relation = found_target_relation ||
		                        (entry.type == GpuMemoryObjectType::RenderTexture &&
		                         entry.relation == GpuMemoryOverlapType::Equals && entry.count == 1u);
	}
	Expect(found_index_relation && found_target_relation,
	       "range classification derives contained-index and exact-target members from their addresses");

	const uint64_t provenance_parent_addr = heap_addr + 0x16000u;
	const uint64_t provenance_parent_size = 0x3000u;
	const uint64_t provenance_addr        = provenance_parent_addr + 0x400u;
	const uint64_t provenance_size        = 0x1000u;
	Expect(GpuMemoryCreateObject(3u, &ctx, nullptr, provenance_parent_addr, provenance_parent_size,
	                             AliasContractGpuObject(GpuMemoryObjectType::IndexBuffer, true)) != nullptr,
	       "the provenance integration fixture creates a covering read-only index view");
	Expect(GpuMemoryCreateObject(4u, &ctx, nullptr, provenance_addr, provenance_size,
	                             ProvenanceContractStorageGpuObject()) != nullptr,
	       "the provenance integration fixture links a writable storage view");

	GpuMemoryRangeProvenance before {};
	const bool provenance_query_ok = GpuMemoryQueryRangeProvenance(provenance_addr, provenance_size, &before);
	Expect(provenance_query_ok && !before.truncated &&
	           before.total_count == 2u && before.entry_count == 2u,
	       "provenance query retains both exact alias identities");
	const GpuMemoryRangeProvenanceEntry* parent_before = nullptr;
	const GpuMemoryRangeProvenanceEntry* storage_before = nullptr;
	for (uint32_t i = 0; i < before.entry_count; ++i)
	{
		const auto& entry = before.entries[i];
		if (entry.type == GpuMemoryObjectType::IndexBuffer)
		{
			parent_before = &entry;
		} else if (entry.type == GpuMemoryObjectType::StorageBuffer)
		{
			storage_before = &entry;
		}
	}
	Expect(parent_before != nullptr && storage_before != nullptr && parent_before->heap_id == storage_before->heap_id &&
	           parent_before->object_id != storage_before->object_id && parent_before->logical_generation != 0u &&
	           storage_before->logical_generation != 0u && parent_before->relation == GpuMemoryOverlapType::Contains &&
	           storage_before->relation == GpuMemoryOverlapType::Equals,
	       "provenance entries expose distinct live identities and their actual overlap relations");
	Expect(parent_before->read_only && !parent_before->write_back_capable && storage_before->write_back_capable &&
	           !storage_before->read_only && storage_before->in_use &&
	           parent_before->content_origin == GpuMemoryContentOrigin::CpuUpload &&
	           storage_before->content_origin == GpuMemoryContentOrigin::CpuUpload &&
	           parent_before->content_sequence != 0u && storage_before->content_sequence != 0u,
	       "new buffer views report CPU upload provenance and write-back capability");
	GpuMemoryFree(&ctx, heap_addr, guest.size());
}

void VerifyVertexClipProbeContract()
{
	ShaderInstruction clamped_add {};
	clamped_add.type              = ShaderInstructionType::VAddF32;
	clamped_add.format            = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	clamped_add.dst               = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	clamped_add.dst.clamp         = true;
	clamped_add.src[0]            = {.type = ShaderOperandType::Vgpr, .register_id = 1, .size = 1};
	clamped_add.src[1]            = {.type = ShaderOperandType::Vgpr, .register_id = 2, .size = 1};
	clamped_add.src_num           = 2;
	ShaderInstruction unclamped_add = clamped_add;
	unclamped_add.dst.clamp         = false;
	ShaderInstruction clamp_nop {};
	clamp_nop.type   = ShaderInstructionType::VNop;
	clamp_nop.format = ShaderInstructionFormat::Empty;
	ShaderInstruction clamp_end {};
	clamp_end.type   = ShaderInstructionType::SEndpgm;
	clamp_end.format = ShaderInstructionFormat::Empty;
	ShaderCode clamped_code {};
	clamped_code.SetType(ShaderType::Vertex);
	clamped_code.GetInstructions().Add(clamped_add);
	clamped_code.GetInstructions().Add(clamp_nop);
	clamped_code.GetInstructions().Add(clamp_end);
	ShaderCode unclamped_code {};
	unclamped_code.SetType(ShaderType::Vertex);
	unclamped_code.GetInstructions().Add(unclamped_add);
	unclamped_code.GetInstructions().Add(clamp_nop);
	unclamped_code.GetInstructions().Add(clamp_end);
	ShaderVertexInputInfo dx10_input {};
	dx10_input.dx10_clamp = true;
	const auto dx10_source = SpirvGenerateSource(clamped_code, &dx10_input, nullptr, nullptr);
	Expect(dx10_source.FindIndex("%dx10_clamp_nan_0 = OpIsNan %bool %c197_0") != Kyty::Core::STRING8_INVALID_INDEX &&
	           dx10_source.FindIndex("%c200_0 = OpSelect %float %dx10_clamp_nan_0 %float_0_000000 %dx10_clamp_numeric_0") !=
	               Kyty::Core::STRING8_INVALID_INDEX,
	       "DX10 mode maps a NaN clamp-modifier input to positive zero");
	ShaderVertexInputInfo passthrough_input {};
	passthrough_input.dx10_clamp = false;
	const auto passthrough_source = SpirvGenerateSource(clamped_code, &passthrough_input, nullptr, nullptr);
	Expect(passthrough_source.FindIndex("%dx10_clamp_nan_0 = OpIsNan %bool %c197_0") != Kyty::Core::STRING8_INVALID_INDEX &&
	           passthrough_source.FindIndex("%c200_0 = OpSelect %float %dx10_clamp_nan_0 %c197_0 %dx10_clamp_numeric_0") !=
	               Kyty::Core::STRING8_INVALID_INDEX,
	       "disabled DX10 mode preserves a NaN through the clamp modifier");
	const auto unclamped_source = SpirvGenerateSource(unclamped_code, &dx10_input, nullptr, nullptr);
	Expect(unclamped_source.FindIndex("dx10_clamp_nan_0") == Kyty::Core::STRING8_INVALID_INDEX,
	       "DX10 mode does not sanitize an ordinary unclamped VALU result");
	ShaderVertexInputInfo ieee_input = dx10_input;
	ieee_input.ieee_mode             = true;
	const auto ieee_source = SpirvGenerateSource(clamped_code, &ieee_input, nullptr, nullptr);
	Expect(ieee_source.FindIndex("dx10_clamp_nan_0") == Kyty::Core::STRING8_INVALID_INDEX &&
	           ieee_source.FindIndex("FClamp") == Kyty::Core::STRING8_INVALID_INDEX,
	       "IEEE mode ignores the floating output clamp modifier");
	ExpectValidSpirv(dx10_source, "DX10 clamp-modifier source validates");
	ExpectValidSpirv(passthrough_source, "NaN-preserving clamp-modifier source validates");
	ExpectValidSpirv(unclamped_source, "ordinary unclamped VALU source validates");
	ExpectValidSpirv(ieee_source, "IEEE-mode clamp-modifier source validates");
	HW::VertexShaderInfo vertex_mode_regs {};
	vertex_mode_regs.es_regs.data_addr = 0x1000u;
	vertex_mode_regs.gs_regs.chksum    = 0x0123456789abcdefull;
	ShaderVertexInputInfo vertex_mode_off {};
	ShaderVertexInputInfo vertex_mode_on = vertex_mode_off;
	vertex_mode_on.dx10_clamp             = true;
	Expect(ShaderGetIdVS(&vertex_mode_regs, &vertex_mode_off) != ShaderGetIdVS(&vertex_mode_regs, &vertex_mode_on),
	       "vertex shader identity separates DX10 clamp modes");
	HW::PixelShaderInfo pixel_mode_regs {};
	pixel_mode_regs.ps_regs.data_addr = 0x2000u;
	pixel_mode_regs.ps_regs.chksum    = 0x0fedcba987654321ull;
	ShaderPixelInputInfo pixel_mode_off {};
	ShaderPixelInputInfo pixel_mode_on = pixel_mode_off;
	pixel_mode_on.ieee_mode             = true;
	Expect(ShaderGetIdPS(&pixel_mode_regs, &pixel_mode_off) != ShaderGetIdPS(&pixel_mode_regs, &pixel_mode_on),
	       "pixel shader identity separates IEEE output-modifier modes");

	HW::ClipControl clip_control {};
	auto            depth_clip = State::ResolveDepthClipState(clip_control, true, true);
	Expect(depth_clip.depth_clip_enable && !depth_clip.depth_clamp_enable && depth_clip.exact,
	       "default guest Z clipping enables native Vulkan depth clipping");
	depth_clip = State::ResolveDepthClipState(clip_control, false, false);
	Expect(depth_clip.depth_clip_enable && !depth_clip.depth_clamp_enable && depth_clip.exact,
	       "default guest Z clipping uses core Vulkan clipping without the extension");
	clip_control.min_z_clip_disable = true;
	depth_clip                     = State::ResolveDepthClipState(clip_control, true, true);
	Expect(!depth_clip.depth_clip_enable && !depth_clip.depth_clamp_enable && !depth_clip.exact,
	       "guest near-plane disable reaches the native depth-clip extension");
	depth_clip = State::ResolveDepthClipState(clip_control, false, true);
	Expect(!depth_clip.depth_clip_enable && depth_clip.depth_clamp_enable && !depth_clip.exact,
	       "guest near-plane disable falls back to core depth clamp");
	depth_clip = State::ResolveDepthClipState(clip_control, false, false);
	Expect(depth_clip.depth_clip_enable && !depth_clip.depth_clamp_enable && !depth_clip.exact,
	       "missing host depth capabilities preserve valid core clipping");
	clip_control.min_z_clip_disable = false;
	clip_control.max_z_clip_disable = true;
	depth_clip                     = State::ResolveDepthClipState(clip_control, true, true);
	Expect(!depth_clip.depth_clip_enable && !depth_clip.depth_clamp_enable && !depth_clip.exact,
	       "guest far-plane disable cannot retain paired host Z clipping");
	clip_control.min_z_clip_disable = true;
	depth_clip = State::ResolveDepthClipState(clip_control, true, true);
	Expect(!depth_clip.depth_clip_enable && !depth_clip.depth_clamp_enable && depth_clip.exact,
	       "paired guest Z disable is exact with the native extension");
	depth_clip = State::ResolveDepthClipState(clip_control, false, true);
	Expect(!depth_clip.depth_clip_enable && depth_clip.depth_clamp_enable && depth_clip.exact,
	       "paired guest Z disable is exact with core depth clamp");
	depth_clip = State::ResolveDepthClipState(clip_control, false, false);
	Expect(depth_clip.depth_clip_enable && !depth_clip.depth_clamp_enable && !depth_clip.exact,
	       "paired guest Z disable remains valid but inexact without either host capability");

	PipelineStaticParameters clipped_pipeline {};
	PipelineStaticParameters unclipped_pipeline = clipped_pipeline;
	unclipped_pipeline.depth_clip_enable         = false;
	Expect(!(clipped_pipeline == unclipped_pipeline),
	       "depth clipping participates in graphics pipeline cache identity");
	PipelineStaticParameters clamped_pipeline = unclipped_pipeline;
	clamped_pipeline.depth_clamp_enable        = true;
	Expect(!(unclipped_pipeline == clamped_pipeline),
	       "depth clamping participates in graphics pipeline cache identity");

	constexpr uint64_t code_id = 0x0123456789abcdefull;
	const auto          set_identity_0 = VertexClipProbeDiagnosticIdentity(0u);
	const auto          set_identity_1 = VertexClipProbeDiagnosticIdentity(1u);
	const auto          set_identity_2 = VertexClipProbeDiagnosticIdentity(2u);
	Expect(set_identity_0 != 0u && set_identity_1 != 0u && set_identity_2 != 0u &&
	           set_identity_0 != set_identity_1 && set_identity_0 != set_identity_2 && set_identity_1 != set_identity_2,
	       "vertex clip probe set identities are nonzero and distinct");
	Expect(ShaderVertexClipProbeEligible(true, false), "next-generation non-embedded vertex shaders are probe eligible");
	Expect(!ShaderVertexClipProbeEligible(true, true), "embedded vertex shaders are probe ineligible");
	Expect(!ShaderVertexClipProbeEligible(false, false), "legacy vertex shaders are probe ineligible");

	TestUnsetEnvironment("KYTY_VS_CLIP_PROBE");
	TestUnsetEnvironment("KYTY_VS_CLIP_PROBE_DRAW");
	TestUnsetEnvironment("KYTY_VS_CLIP_PROBE_MIN_PRESENT");
	TestUnsetEnvironment("KYTY_PS_INPUT0_PROBE");
	TestUnsetEnvironment("KYTY_PS_INPUT0_PROBE_DRAW");
	TestUnsetEnvironment("KYTY_PS_INPUT0_PROBE_MIN_PRESENT");
	TestUnsetEnvironment("KYTY_PS_SAMPLE_PROBE");
	TestUnsetEnvironment("KYTY_PS_SAMPLE_PROBE_DRAW");
	TestUnsetEnvironment("KYTY_PS_SAMPLE_PROBE_MIN_PRESENT");
	TestUnsetEnvironment("KYTY_PS_SAMPLE_PROBE_MATCH_ORDINAL");
	TestUnsetEnvironment("KYTY_PS_MRT_PROBE");
	TestUnsetEnvironment("KYTY_PS_MRT_PROBE_DRAW");
	TestUnsetEnvironment("KYTY_PS_MRT_PROBE_MIN_PRESENT");
	TestUnsetEnvironment("KYTY_PS_MRT_PROBE_MATCH_ORDINAL");
	TestUnsetEnvironment("KYTY_PS_MRT_ATTACHMENT_PROBE");
	TestUnsetEnvironment("KYTY_PS_MRT_ATTACHMENT_MIN_INVOCATIONS");

	const auto disabled = ShaderResolveVertexClipProbeConfig(code_id, true, 3564u);
	Expect(!disabled.enabled && !disabled.draw_scoped && disabled.diagnostic_identity == 0,
	       "vertex clip probe defaults to disabled without selectors");

	TestSetEnvironment("KYTY_VS_CLIP_PROBE", "0123456789abcdef");
	const auto no_draw_selector = ShaderResolveVertexClipProbeConfig(code_id, true, 3564u);
	Expect(!no_draw_selector.enabled && !no_draw_selector.draw_scoped && no_draw_selector.diagnostic_identity == 0,
	       "vertex clip probe requires an exact draw selector");

	TestSetEnvironment("KYTY_VS_CLIP_PROBE_DRAW", "indexed:3564");
	const auto selected = ShaderResolveVertexClipProbeConfig(code_id, true, 3564u);
	const auto wrong_count = ShaderResolveVertexClipProbeConfig(code_id, true, 3563u);
	const auto wrong_kind  = ShaderResolveVertexClipProbeConfig(code_id, false, 3564u);
	const auto wrong_code  = ShaderResolveVertexClipProbeConfig(0x1111111111111111ull, true, 3564u);
	Expect(selected.enabled && selected.draw_scoped && selected.diagnostic_identity != 0,
	       "exact checksum and indexed draw select the vertex clip probe");
	Expect(!wrong_count.enabled && wrong_count.draw_scoped && wrong_count.diagnostic_identity == 0,
	       "vertex clip probe rejects an indexed count mismatch");
	Expect(!wrong_kind.enabled && wrong_kind.draw_scoped && wrong_kind.diagnostic_identity == 0,
	       "vertex clip probe rejects an indexed-versus-auto mismatch");
	Expect(!wrong_code.enabled && !wrong_code.draw_scoped && wrong_code.diagnostic_identity == 0,
	       "vertex clip probe rejects a checksum mismatch");
	Expect(selected.min_present == 0u && VertexClipProbeCanReserveAtPresent(selected, 0u),
	       "vertex clip probe defaults to immediate reservation");
	TestSetEnvironment("KYTY_VS_CLIP_PROBE_MIN_PRESENT", "8500");
	const auto delayed = ShaderResolveVertexClipProbeConfig(code_id, true, 3564u);
	Expect(delayed.enabled && delayed.min_present == 8500u &&
	           !VertexClipProbeCanReserveAtPresent(delayed, 8499u) && VertexClipProbeCanReserveAtPresent(delayed, 8500u),
	       "vertex clip probe preserves its one-shot reservation until the requested present");
	TestSetEnvironment("KYTY_VS_CLIP_PROBE_MIN_PRESENT", "8500trailing");
	const auto malformed_delay = ShaderResolveVertexClipProbeConfig(code_id, true, 3564u);
	Expect(!malformed_delay.enabled && malformed_delay.draw_scoped,
	       "vertex clip probe fails closed on a malformed minimum present");
	TestUnsetEnvironment("KYTY_VS_CLIP_PROBE_MIN_PRESENT");

	TestSetEnvironment("KYTY_VS_CLIP_PROBE_DRAW", "auto:3564");
	const auto selected_auto = ShaderResolveVertexClipProbeConfig(code_id, false, 3564u);
	Expect(selected_auto.enabled && selected_auto.draw_scoped && selected_auto.diagnostic_identity != 0,
	       "exact auto draw selects the vertex clip probe");
	VertexClipProbeLifecycle clear_only_lifecycle {};
	Expect(!VertexClipProbeCanReserveAutoDraw(selected_auto, true) &&
	           clear_only_lifecycle.GetState() == VertexClipProbeState::Idle &&
	           VertexClipProbeCanReserveAutoDraw(selected_auto, false) && clear_only_lifecycle.Reserve(),
	       "an auto clear-only match leaves the one-shot probe idle for a later real draw");

	TestSetEnvironment("KYTY_VS_CLIP_PROBE", "0123456789abcdeff");
	const auto trailing_checksum = ShaderResolveVertexClipProbeConfig(code_id, false, 3564u);
	Expect(!trailing_checksum.enabled && trailing_checksum.diagnostic_identity == 0,
	       "vertex clip probe rejects a checksum with trailing input");
	TestSetEnvironment("KYTY_VS_CLIP_PROBE", "123456789abcdef");
	const auto short_checksum = ShaderResolveVertexClipProbeConfig(code_id, false, 3564u);
	Expect(!short_checksum.enabled && short_checksum.diagnostic_identity == 0,
	       "vertex clip probe rejects a checksum shorter than sixteen hex digits");
	TestSetEnvironment("KYTY_VS_CLIP_PROBE", "0x0123456789abcdef");
	const auto prefixed_checksum = ShaderResolveVertexClipProbeConfig(code_id, false, 3564u);
	Expect(!prefixed_checksum.enabled && prefixed_checksum.diagnostic_identity == 0,
	       "vertex clip probe rejects a checksum with a hexadecimal prefix");
	TestSetEnvironment("KYTY_VS_CLIP_PROBE", "+0123456789abcdef");
	const auto signed_checksum = ShaderResolveVertexClipProbeConfig(code_id, false, 3564u);
	Expect(!signed_checksum.enabled && signed_checksum.diagnostic_identity == 0,
	       "vertex clip probe rejects a checksum with a leading sign");
	TestSetEnvironment("KYTY_VS_CLIP_PROBE", " 0123456789abcdef");
	const auto spaced_checksum = ShaderResolveVertexClipProbeConfig(code_id, false, 3564u);
	Expect(!spaced_checksum.enabled && spaced_checksum.diagnostic_identity == 0,
	       "vertex clip probe rejects a checksum with leading whitespace");
	TestSetEnvironment("KYTY_VS_CLIP_PROBE", "0123456789ABCDEF");
	const auto uppercase_checksum = ShaderResolveVertexClipProbeConfig(code_id, false, 3564u);
	Expect(uppercase_checksum.enabled && uppercase_checksum.draw_scoped && uppercase_checksum.diagnostic_identity != 0,
	       "vertex clip probe accepts uppercase hexadecimal checksums");
	TestSetEnvironment("KYTY_VS_CLIP_PROBE", "0123456789abcdef");
	TestSetEnvironment("KYTY_VS_CLIP_PROBE_DRAW", "indexed:4294967296");
	const auto overflow_count = ShaderResolveVertexClipProbeConfig(code_id, true, 3564u);
	Expect(!overflow_count.enabled && overflow_count.diagnostic_identity == 0,
	       "vertex clip probe rejects an overflowing draw count");
	TestSetEnvironment("KYTY_VS_CLIP_PROBE_DRAW", "indexed:3564trailing");
	const auto trailing_count = ShaderResolveVertexClipProbeConfig(code_id, true, 3564u);
	Expect(!trailing_count.enabled && trailing_count.diagnostic_identity == 0,
	       "vertex clip probe rejects trailing draw-count input");

	Expect(ShaderPixelInput0ProbeEligible(true, false), "next-generation non-embedded pixel shaders are input-zero probe eligible");
	Expect(!ShaderPixelInput0ProbeEligible(true, true), "embedded pixel shaders are input-zero probe ineligible");
	Expect(!ShaderPixelInput0ProbeEligible(false, false), "legacy pixel shaders are input-zero probe ineligible");
	const auto pixel_disabled = ShaderResolvePixelInput0ProbeConfig(code_id, true, 3564u, true);
	Expect(!pixel_disabled.enabled && !pixel_disabled.draw_scoped && pixel_disabled.diagnostic_identity == 0,
	       "pixel input-zero probe defaults to disabled without selectors");
	TestSetEnvironment("KYTY_PS_INPUT0_PROBE", "0123456789abcdef");
	const auto pixel_without_draw = ShaderResolvePixelInput0ProbeConfig(code_id, true, 3564u, true);
	Expect(!pixel_without_draw.enabled && !pixel_without_draw.draw_scoped && pixel_without_draw.diagnostic_identity == 0,
	       "pixel input-zero probe requires an exact draw selector");
	TestSetEnvironment("KYTY_PS_INPUT0_PROBE_DRAW", "indexed:3564");
	const auto pixel_selected       = ShaderResolvePixelInput0ProbeConfig(code_id, true, 3564u, true);
	const auto pixel_stage_disabled = ShaderResolvePixelInput0ProbeConfig(code_id, true, 3564u, false);
	const auto pixel_wrong_count    = ShaderResolvePixelInput0ProbeConfig(code_id, true, 3563u, true);
	const auto pixel_wrong_kind     = ShaderResolvePixelInput0ProbeConfig(code_id, false, 3564u, true);
	const auto pixel_wrong_code     = ShaderResolvePixelInput0ProbeConfig(0x1111111111111111ull, true, 3564u, true);
	Expect(pixel_selected.enabled && pixel_selected.draw_scoped && pixel_selected.diagnostic_identity != 0,
	       "exact checksum and indexed draw select the pixel input-zero probe");
	Expect(pixel_selected.min_present == 0u && PixelInput0ProbeCanReserveAtPresent(pixel_selected, 0u),
	       "pixel input-zero probe defaults to immediate reservation");
	TestSetEnvironment("KYTY_PS_INPUT0_PROBE_MIN_PRESENT", "9000");
	const auto delayed_pixel = ShaderResolvePixelInput0ProbeConfig(code_id, true, 3564u, true);
	Expect(delayed_pixel.enabled && delayed_pixel.min_present == 9000u &&
	           !VertexClipProbeStagesCanReserveAtPresent(delayed, delayed_pixel, 8999u) &&
	           VertexClipProbeStagesCanReserveAtPresent(delayed, delayed_pixel, 9000u),
	       "the shared one-shot waits for the later threshold when both stages are selected");
	TestUnsetEnvironment("KYTY_PS_INPUT0_PROBE_MIN_PRESENT");
	Expect(!pixel_stage_disabled.enabled && !pixel_stage_disabled.draw_scoped && pixel_stage_disabled.diagnostic_identity == 0,
	       "a disabled pixel stage cannot select or poison an ordinary PS-off pipeline");
	Expect(!pixel_wrong_count.enabled && pixel_wrong_count.draw_scoped && pixel_wrong_count.diagnostic_identity == 0,
	       "pixel input-zero probe rejects an indexed count mismatch");
	Expect(!pixel_wrong_kind.enabled && pixel_wrong_kind.draw_scoped && pixel_wrong_kind.diagnostic_identity == 0,
	       "pixel input-zero probe rejects an indexed-versus-auto mismatch");
	Expect(!pixel_wrong_code.enabled && !pixel_wrong_code.draw_scoped && pixel_wrong_code.diagnostic_identity == 0,
	       "pixel input-zero probe rejects a checksum mismatch");
	TestUnsetEnvironment("KYTY_PS_INPUT0_PROBE");
	TestUnsetEnvironment("KYTY_PS_INPUT0_PROBE_DRAW");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE", "0123456789abcdef:@2");
	const auto sample_without_draw = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!sample_without_draw.enabled && !sample_without_draw.draw_scoped &&
	           sample_without_draw.kind == ShaderPixelProbeKind::None,
	       "pixel sample probe requires an exact draw selector");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE_DRAW", "indexed:3564");
	const auto sample_selected       = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	const auto sample_stage_disabled = ShaderResolvePixelProbeConfig(code_id, true, 3564u, false);
	Expect(sample_selected.enabled && sample_selected.draw_scoped &&
	           sample_selected.kind == ShaderPixelProbeKind::SampleResult && sample_selected.sample_ordinal == 2u &&
	           sample_selected.match_ordinal == 0u && sample_selected.diagnostic_identity != 0u,
	       "exact checksum, draw, and ImageSampleB ordinal select the pixel sample-result probe");
	Expect(!sample_stage_disabled.enabled && sample_stage_disabled.kind == ShaderPixelProbeKind::None,
	       "a disabled pixel stage cannot select the sample-result probe");
	TestSetEnvironment("KYTY_PS_MRT_ATTACHMENT_PROBE", "1");
	const auto sample_with_attachment_request = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(sample_with_attachment_request.enabled &&
	           sample_with_attachment_request.kind == ShaderPixelProbeKind::SampleResult &&
	           !sample_with_attachment_request.attachment_readback &&
	           sample_with_attachment_request.diagnostic_identity == sample_selected.diagnostic_identity,
	       "an MRT attachment request does not alter an unrelated sample-result probe");
	TestUnsetEnvironment("KYTY_PS_MRT_ATTACHMENT_PROBE");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE_SPARSE", "1");
	const auto sparse_sample_selected = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(sparse_sample_selected.enabled &&
	           sparse_sample_selected.diagnostic_identity != sample_selected.diagnostic_identity,
	       "sparse pixel sample observation selects a distinct diagnostic module");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE_SPARSE", "true");
	const auto malformed_sparse_sample = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!malformed_sparse_sample.enabled && malformed_sparse_sample.draw_scoped,
	       "pixel sample probe fails closed on malformed sparse observation input");
	TestUnsetEnvironment("KYTY_PS_SAMPLE_PROBE_SPARSE");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE_MIN_PRESENT", "9001");
	const auto delayed_sample = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(delayed_sample.enabled && delayed_sample.min_present == 9001u &&
	           !PixelInput0ProbeCanReserveAtPresent(delayed_sample, 9000u) &&
	           PixelInput0ProbeCanReserveAtPresent(delayed_sample, 9001u),
	       "pixel sample probe preserves the optional one-shot minimum present");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE_MIN_PRESENT", "9001junk");
	const auto malformed_sample_delay = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!malformed_sample_delay.enabled && malformed_sample_delay.draw_scoped,
	       "pixel sample probe fails closed on malformed minimum present input");
	TestUnsetEnvironment("KYTY_PS_SAMPLE_PROBE_MIN_PRESENT");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE_MATCH_ORDINAL", "1");
	const auto second_matching_sample = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(second_matching_sample.enabled && second_matching_sample.match_ordinal == 1u,
	       "pixel sample probe can select the second exact match after its present threshold");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE_MATCH_ORDINAL", "1junk");
	const auto malformed_match_ordinal = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!malformed_match_ordinal.enabled && malformed_match_ordinal.draw_scoped,
	       "pixel sample probe fails closed on a malformed match ordinal");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE_MATCH_ORDINAL", "4294967296");
	const auto overflowing_match_ordinal = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!overflowing_match_ordinal.enabled && overflowing_match_ordinal.draw_scoped,
	       "pixel sample probe fails closed on an overflowing match ordinal");
	TestUnsetEnvironment("KYTY_PS_SAMPLE_PROBE_MATCH_ORDINAL");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE", "0123456789abcdef:@2junk");
	const auto malformed_sample_selector = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!malformed_sample_selector.enabled && malformed_sample_selector.kind == ShaderPixelProbeKind::None,
	       "pixel sample probe rejects a malformed ordinal selector");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE", "0123456789abcdef:@2");
	TestSetEnvironment("KYTY_PS_INPUT0_PROBE", "0123456789abcdef");
	TestSetEnvironment("KYTY_PS_INPUT0_PROBE_DRAW", "indexed:3564");
	const auto ambiguous_pixel_probe = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!ambiguous_pixel_probe.enabled && !ambiguous_pixel_probe.draw_scoped &&
	           ambiguous_pixel_probe.kind == ShaderPixelProbeKind::None,
	       "simultaneous coordinate and sample-result selectors fail closed");
	TestUnsetEnvironment("KYTY_PS_INPUT0_PROBE");
	TestUnsetEnvironment("KYTY_PS_INPUT0_PROBE_DRAW");
	TestUnsetEnvironment("KYTY_PS_SAMPLE_PROBE");
	TestUnsetEnvironment("KYTY_PS_SAMPLE_PROBE_DRAW");
	TestSetEnvironment("KYTY_PS_MRT_PROBE", "0123456789abcdef:mrt0@229");
	const auto mrt_without_draw = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!mrt_without_draw.enabled && !mrt_without_draw.draw_scoped &&
	           mrt_without_draw.kind == ShaderPixelProbeKind::None,
	       "pixel MRT probe requires an exact draw selector");
	TestSetEnvironment("KYTY_PS_MRT_PROBE_DRAW", "indexed:3564");
	TestSetEnvironment("KYTY_PS_MRT_PROBE_MIN_PRESENT", "8090");
	TestSetEnvironment("KYTY_PS_MRT_PROBE_MATCH_ORDINAL", "1");
	const auto mrt_selected = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(mrt_selected.enabled && mrt_selected.draw_scoped &&
	           mrt_selected.kind == ShaderPixelProbeKind::FinalMrtResult && mrt_selected.mrt_target == 0u &&
	           mrt_selected.export_ordinal == 229u && mrt_selected.min_present == 8090u &&
	           mrt_selected.match_ordinal == 1u && !mrt_selected.attachment_readback &&
	           mrt_selected.diagnostic_identity != 0u,
	       "exact checksum, MRT target, export ordinal, draw, threshold, and occurrence select the final-output probe");
	TestSetEnvironment("KYTY_PS_MRT_ATTACHMENT_PROBE", "0");
	const auto mrt_attachment_disabled = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(mrt_attachment_disabled.enabled && !mrt_attachment_disabled.attachment_readback &&
	           mrt_attachment_disabled.diagnostic_identity == mrt_selected.diagnostic_identity,
	       "an absent or zero MRT attachment request leaves the final-output probe unchanged");
	TestSetEnvironment("KYTY_PS_MRT_ATTACHMENT_PROBE", "1");
	const auto mrt_attachment_enabled = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(mrt_attachment_enabled.enabled && mrt_attachment_enabled.kind == ShaderPixelProbeKind::FinalMrtResult &&
	           mrt_attachment_enabled.attachment_readback &&
	           mrt_attachment_enabled.attachment_min_invocations == 1u &&
	           mrt_attachment_enabled.diagnostic_identity == mrt_selected.diagnostic_identity,
	       "the strict MRT attachment request enables only host readback and preserves shader identity");
	TestSetEnvironment("KYTY_PS_MRT_ATTACHMENT_MIN_INVOCATIONS", "10000");
	const auto mrt_attachment_significant = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(mrt_attachment_significant.enabled && mrt_attachment_significant.attachment_readback &&
	           mrt_attachment_significant.attachment_min_invocations == 10000u &&
	           mrt_attachment_significant.diagnostic_identity == mrt_selected.diagnostic_identity,
	       "the MRT attachment probe accepts a positive minimum invocation threshold without changing shader identity");
	TestSetEnvironment("KYTY_PS_MRT_ATTACHMENT_MIN_INVOCATIONS", "0");
	const auto invalid_mrt_attachment_minimum = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!invalid_mrt_attachment_minimum.enabled && invalid_mrt_attachment_minimum.draw_scoped,
	       "the MRT attachment probe rejects a zero minimum invocation threshold");
	TestUnsetEnvironment("KYTY_PS_MRT_ATTACHMENT_MIN_INVOCATIONS");
	TestSetEnvironment("KYTY_PS_MRT_ATTACHMENT_PROBE", "true");
	const auto malformed_mrt_attachment = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!malformed_mrt_attachment.enabled && malformed_mrt_attachment.draw_scoped &&
	           !malformed_mrt_attachment.attachment_readback,
	       "a malformed MRT attachment request fails the selected diagnostic closed");
	TestUnsetEnvironment("KYTY_PS_MRT_ATTACHMENT_PROBE");
	TestSetEnvironment("KYTY_PS_MRT_PROBE", "0123456789abcdef:mrt4@229");
	const auto bad_mrt_target = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!bad_mrt_target.enabled, "pixel MRT probe rejects unsupported targets");
	TestSetEnvironment("KYTY_PS_MRT_PROBE", "0123456789abcdef:mrt0@229junk");
	const auto bad_mrt_ordinal = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!bad_mrt_ordinal.enabled, "pixel MRT probe rejects trailing export-ordinal input");
	TestSetEnvironment("KYTY_PS_MRT_PROBE", "0123456789abcdef:mrt0@229");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE", "0123456789abcdef:@2");
	TestSetEnvironment("KYTY_PS_SAMPLE_PROBE_DRAW", "indexed:3564");
	const auto ambiguous_mrt_sample = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!ambiguous_mrt_sample.enabled && ambiguous_mrt_sample.kind == ShaderPixelProbeKind::None,
	       "simultaneous MRT and sample-result selectors fail closed");
	TestUnsetEnvironment("KYTY_PS_SAMPLE_PROBE");
	TestUnsetEnvironment("KYTY_PS_SAMPLE_PROBE_DRAW");
	TestSetEnvironment("KYTY_FS_TAP", "0123456789abcdef:@7");
	const auto ambiguous_mrt_tap = ShaderResolvePixelProbeConfig(code_id, true, 3564u, true);
	Expect(!ambiguous_mrt_tap.enabled && ambiguous_mrt_tap.kind == ShaderPixelProbeKind::None,
	       "MRT observation cannot coexist with destructive fragment-tap output");
	TestUnsetEnvironment("KYTY_FS_TAP");
	TestUnsetEnvironment("KYTY_PS_MRT_PROBE");
	TestUnsetEnvironment("KYTY_PS_MRT_PROBE_DRAW");
	TestUnsetEnvironment("KYTY_PS_MRT_PROBE_MIN_PRESENT");
	TestUnsetEnvironment("KYTY_PS_MRT_PROBE_MATCH_ORDINAL");

	ShaderId shader_id {};
	shader_id.hash0 = 1u;
	shader_id.crc32 = 2u;
	shader_id.ids.Add(3u);
	const auto ordinary = ShaderModuleKey::Create(shader_id, ShaderModuleStage::Vertex,
	                                              Kyty::Config::ShaderOptimizationType::Performance, true, false, 0);
	const auto selected_set_0 = ShaderModuleKey::Create(shader_id, ShaderModuleStage::Vertex,
	                                                    Kyty::Config::ShaderOptimizationType::Performance, true, false,
	                                                    set_identity_0);
	const auto selected_set_1 = ShaderModuleKey::Create(shader_id, ShaderModuleStage::Vertex,
	                                                    Kyty::Config::ShaderOptimizationType::Performance, true, false,
	                                                    set_identity_1);
	const auto selected_set_2 = ShaderModuleKey::Create(shader_id, ShaderModuleStage::Vertex,
	                                                    Kyty::Config::ShaderOptimizationType::Performance, true, false,
	                                                    set_identity_2);
	Expect(ordinary == ShaderModuleKey::Create(shader_id, ShaderModuleStage::Vertex,
	                                           Kyty::Config::ShaderOptimizationType::Performance, true, false, 0),
	       "ordinary vertex shader cache identity remains unchanged");
	Expect(ordinary != selected_set_0 && selected_set_0 != selected_set_1 && selected_set_0 != selected_set_2 &&
	           selected_set_1 != selected_set_2,
	       "selected vertex clip probe descriptor sets have distinct module identities");
	constexpr uint64_t legacy_vcprobe4 = 0x564350524f424534ull;
	constexpr uint64_t legacy_vcprobe5 = 0x564350524f423500ull;
	for (uint32_t current_set = 0; current_set < 3u; ++current_set)
	{
		for (uint32_t legacy_set = 0; legacy_set < 3u; ++legacy_set)
		{
			Expect(VertexClipProbeDiagnosticIdentity(current_set) != legacy_vcprobe4 + legacy_set,
			       "current diagnostic descriptor variants cannot alias persisted VCPROBE4 modules");
			Expect(VertexClipProbeDiagnosticIdentity(current_set) != legacy_vcprobe5 + legacy_set,
			       "current diagnostic descriptor variants cannot alias persisted VCPROBE5 modules");
		}
	}
	const auto ordinary_pixel = ShaderModuleKey::Create(shader_id, ShaderModuleStage::Pixel,
	                                                    Kyty::Config::ShaderOptimizationType::Performance, true, false, 0);
	const auto selected_pixel = ShaderModuleKey::Create(shader_id, ShaderModuleStage::Pixel,
	                                                    Kyty::Config::ShaderOptimizationType::Performance, true, false,
	                                                    pixel_selected.diagnostic_identity);
	const auto selected_sample_pixel = ShaderModuleKey::Create(shader_id, ShaderModuleStage::Pixel,
	                                                           Kyty::Config::ShaderOptimizationType::Performance, true, false,
	                                                           sample_selected.diagnostic_identity);
	Expect(ordinary_pixel != selected_pixel, "selected pixel input-zero probe has a distinct module identity");
	Expect(ordinary_pixel != selected_sample_pixel && selected_pixel != selected_sample_pixel &&
	           sample_selected.diagnostic_identity == PixelSampleProbeDiagnosticIdentity(0u, 2u) &&
	           sample_selected.diagnostic_identity != PixelSampleProbeDiagnosticIdentity(0u, 1u),
	       "pixel sample-result ordinals have identities distinct from ordinary and coordinate-input modules");
	Expect(mrt_selected.diagnostic_identity == PixelMrtProbeDiagnosticIdentity(0u, 0u, 229u) &&
	           mrt_selected.diagnostic_identity != sample_selected.diagnostic_identity &&
	           PixelMrtProbeDiagnosticIdentity(0u, 0u, 229u) != PixelMrtProbeDiagnosticIdentity(0u, 1u, 229u) &&
	           PixelMrtProbeDiagnosticIdentity(0u, 0u, 229u) != PixelMrtProbeDiagnosticIdentity(0u, 0u, 228u),
	       "pixel MRT target and export ordinal have a distinct diagnostic identity");

	static_assert(sizeof(VertexClipProbeRawStats) == sizeof(uint32_t) * 51u);
	static_assert(offsetof(VertexClipProbeRawStats, min_pixel_frag_x) == sizeof(uint32_t) * 47u);
	static_assert(offsetof(VertexClipProbeRawStats, max_pixel_frag_x) == sizeof(uint32_t) * 48u);
	static_assert(offsetof(VertexClipProbeRawStats, min_pixel_frag_y) == sizeof(uint32_t) * 49u);
	static_assert(offsetof(VertexClipProbeRawStats, max_pixel_frag_y) == sizeof(uint32_t) * 50u);
	const auto initial_stats = VertexClipProbeInitialRawStats();
	Expect(initial_stats.invocations == 0u && initial_stats.nonfinite == 0u && initial_stats.max_w == 0u &&
	           initial_stats.max_x_w == 0u && initial_stats.max_y_w == 0u && initial_stats.max_z_w == 0u &&
	           initial_stats.param0_exports == 0u && initial_stats.param0_nonfinite == 0u &&
	           initial_stats.max_param0_x == 0u && initial_stats.max_param0_y == 0u &&
	           initial_stats.pixel_input0_samples == 0u && initial_stats.pixel_input0_nonfinite == 0u &&
	           initial_stats.max_pixel_input0_x == 0u && initial_stats.max_pixel_input0_y == 0u &&
	           initial_stats.pixel_sample_invocations == 0u && initial_stats.pixel_sample_nonfinite == 0u &&
	           initial_stats.max_pixel_sample_r == 0u && initial_stats.max_pixel_sample_g == 0u &&
	           initial_stats.max_pixel_sample_b == 0u && initial_stats.max_pixel_sample_a == 0u &&
	           initial_stats.max_pixel_frag_x == 0u && initial_stats.max_pixel_frag_y == 0u &&
	           initial_stats.resolver_claimed == 0u && initial_stats.resolver_instruction_pc == 0u &&
	           initial_stats.resolver_access_width == 0u && initial_stats.resolver_desc0 == 0u &&
	           initial_stats.resolver_desc1 == 0u && initial_stats.resolver_raw_byte_offset == 0u &&
	           initial_stats.resolver_valid == 0u && initial_stats.resolver_final_slot == 0u &&
	           initial_stats.resolver_final_byte_offset == 0u && initial_stats.clip_w_nonpositive == 0u &&
	           initial_stats.clip_xy_outside == 0u && initial_stats.clip_z_outside_zero_to_one == 0u &&
	           initial_stats.clip_inside_zero_to_one == 0u &&
	           initial_stats.clip_z_outside_negative_one_to_one == 0u &&
	           initial_stats.clip_inside_negative_one_to_one == 0u &&
	           initial_stats.min_w == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_x_w == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_y_w == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_z_w == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_param0_x == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_param0_y == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_pixel_input0_x == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_pixel_input0_y == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_pixel_sample_r == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_pixel_sample_g == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_pixel_sample_b == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_pixel_sample_a == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_pixel_frag_x == std::numeric_limits<uint32_t>::max() &&
	           initial_stats.min_pixel_frag_y == std::numeric_limits<uint32_t>::max(),
	       "vertex clip probe raw initialization preserves counter and extrema sentinels");
	VertexClipProbeLifecycle lifecycle {};
	Expect(lifecycle.GetState() == VertexClipProbeState::Idle && lifecycle.Reserve() && !lifecycle.Reserve() &&
	           lifecycle.BeginRecording() && lifecycle.MarkPendingFence() && lifecycle.Complete() &&
	           lifecycle.GetState() == VertexClipProbeState::Completed && !lifecycle.Reserve(),
	       "vertex clip probe lifecycle reserves once and completes only after its pending fence");
	VertexClipProbeLifecycle second_match_lifecycle {};
	Expect(!second_match_lifecycle.Reserve(1u) && second_match_lifecycle.GetState() == VertexClipProbeState::Idle &&
	           second_match_lifecycle.Reserve(1u) && second_match_lifecycle.GetState() == VertexClipProbeState::Reserved &&
	           !second_match_lifecycle.Reserve(1u),
	       "vertex clip probe skips one exact match without consuming its one-shot lifecycle");
	VertexClipProbeLifecycle retry_empty_lifecycle {};
	Expect(!retry_empty_lifecycle.Reserve(1u) && retry_empty_lifecycle.Reserve(1u) &&
	           retry_empty_lifecycle.BeginRecording() && retry_empty_lifecycle.MarkPendingFence() &&
	           retry_empty_lifecycle.RetryAfterFence() && retry_empty_lifecycle.GetState() == VertexClipProbeState::Idle &&
	           retry_empty_lifecycle.Reserve(1u) && retry_empty_lifecycle.GetState() == VertexClipProbeState::Reserved,
	       "an empty fenced attachment observation can re-arm the next exact match without re-skipping the ordinal");
	VertexClipProbeRawStats small_attachment_result {};
	small_attachment_result.pixel_sample_invocations = 6u;
	Expect(VertexClipProbeAttachmentShouldRetry(small_attachment_result, 10000u, 5u) &&
	           !VertexClipProbeAttachmentShouldRetry(small_attachment_result, 6u, 5u) &&
	           !VertexClipProbeAttachmentShouldRetry(small_attachment_result, 10000u,
	                                                 kVertexClipProbeAttachmentMaxEmptyRetries),
	       "the minimum invocation gate rejects small fenced matches without exceeding the retry bound");
	ShaderVertexClipProbeConfig paired_vertex_config {};
	paired_vertex_config.enabled = true;
	ShaderPixelInput0ProbeConfig paired_pixel_config {};
	paired_pixel_config.enabled = true;
	paired_pixel_config.kind    = ShaderPixelProbeKind::SampleResult;
	Expect(!VertexClipProbeValidatePairedPixelInstruction(false, &paired_vertex_config, &paired_pixel_config) &&
	           !paired_vertex_config.enabled && !paired_pixel_config.enabled &&
	           paired_pixel_config.kind == ShaderPixelProbeKind::None,
	       "an invalid selected pixel instruction rejects both peers before one-shot reservation");
	const auto negative      = VertexClipProbeEncodeOrderedFloat(-2.0f);
	const auto negative_zero = VertexClipProbeEncodeOrderedFloat(-0.0f);
	const auto positive_zero = VertexClipProbeEncodeOrderedFloat(0.0f);
	const auto positive      = VertexClipProbeEncodeOrderedFloat(2.0f);
	Expect(negative < negative_zero && negative_zero < positive_zero && positive_zero < positive,
	       "ordered vertex clip probe float encoding preserves numeric and signed-zero order");
	Expect(VertexClipProbeDecodeOrderedFloat(negative) == -2.0f && VertexClipProbeDecodeOrderedFloat(positive) == 2.0f,
	       "ordered vertex clip probe float encoding round-trips finite values");
	const auto decoded_negative_zero = VertexClipProbeDecodeOrderedFloat(negative_zero);
	Expect(decoded_negative_zero == 0.0f && std::signbit(decoded_negative_zero),
	       "ordered vertex clip probe float encoding preserves negative zero");

	VertexClipProbeRawStats empty {};
	Expect(!VertexClipProbeHasFiniteExtrema(empty), "empty vertex clip probe stats retain finite-extrema sentinels");
	VertexClipProbeRawStats finite {};
	finite.min_w   = negative;
	finite.max_w   = positive;
	finite.min_x_w = negative_zero;
	finite.max_x_w = positive_zero;
	finite.min_y_w = negative;
	finite.max_y_w = positive;
	finite.min_z_w = negative;
	finite.max_z_w = positive;
	Expect(VertexClipProbeHasFiniteExtrema(finite), "finite vertex clip probe extrema are recognized");
	Expect(!VertexClipProbeHasFiniteParam0Extrema(finite),
	       "position-only vertex probe stats retain empty PARAM0 extrema");
	finite.param0_exports  = 1u;
	finite.min_param0_x   = negative;
	finite.max_param0_x   = positive;
	finite.min_param0_y   = negative_zero;
	finite.max_param0_y   = positive_zero;
	Expect(VertexClipProbeHasFiniteParam0Extrema(finite), "finite PARAM0 export extrema are recognized");
	Expect(!VertexClipProbeHasFinitePixelInput0Extrema(finite),
	       "vertex-only probe stats retain empty pixel input-zero extrema");
	finite.pixel_input0_samples = 1u;
	finite.min_pixel_input0_x   = negative;
	finite.max_pixel_input0_x   = positive;
	finite.min_pixel_input0_y   = negative_zero;
	finite.max_pixel_input0_y   = positive_zero;
	Expect(VertexClipProbeHasFinitePixelInput0Extrema(finite), "finite pixel input-zero extrema are recognized");
	finite.pixel_sample_invocations = 7u;
	finite.min_pixel_sample_r       = negative;
	finite.max_pixel_sample_r       = positive;
	finite.min_pixel_sample_g       = negative_zero;
	finite.max_pixel_sample_g       = positive_zero;
	finite.min_pixel_sample_b       = negative;
	finite.max_pixel_sample_b       = positive;
	finite.min_pixel_sample_a       = negative;
	finite.max_pixel_sample_a       = positive;
	Expect(VertexClipProbeHasFinitePixelSampleExtrema(finite) &&
	           VertexClipProbeDecodeOrderedFloat(finite.min_pixel_sample_r) == -2.0f &&
	           VertexClipProbeDecodeOrderedFloat(finite.max_pixel_sample_a) == 2.0f,
	       "pixel sample-result extrema use the ordered-float round trip for all RGBA channels");
	VertexClipProbeResultInfo result_info {};
	result_info.checksum       = code_id;
	result_info.indexed        = true;
	result_info.guest_count    = 3564u;
	result_info.descriptor_set = 2u;
	VertexClipProbeRawStats bounded_finite = finite;
	const auto               bounded_min    = VertexClipProbeEncodeOrderedFloat(-std::numeric_limits<float>::max());
	const auto               bounded_max    = VertexClipProbeEncodeOrderedFloat(std::numeric_limits<float>::max());
	bounded_finite.min_w = bounded_finite.min_x_w = bounded_finite.min_y_w = bounded_finite.min_z_w = bounded_min;
	bounded_finite.max_w = bounded_finite.max_x_w = bounded_finite.max_y_w = bounded_finite.max_z_w = bounded_max;
	bounded_finite.invocations = std::numeric_limits<uint32_t>::max();
	bounded_finite.nonfinite   = std::numeric_limits<uint32_t>::max();
	bounded_finite.clip_w_nonpositive                 = 1u;
	bounded_finite.clip_xy_outside                    = 2u;
	bounded_finite.clip_z_outside_zero_to_one         = 3u;
	bounded_finite.clip_inside_zero_to_one            = 4u;
	bounded_finite.clip_z_outside_negative_one_to_one = 5u;
	bounded_finite.clip_inside_negative_one_to_one    = 6u;
	char result_message[kAgentEventMessageMax] {};
	Expect(VertexClipProbeFormatResultMessage(result_info, bounded_finite, result_message, sizeof(result_message)) &&
	           std::strlen(result_message) < sizeof(result_message) &&
	           std::strstr(result_message, "cs=0123456789abcdef k=i n=3564 s=2 inv=4294967295 nf=4294967295 fin=1 w=") != nullptr &&
	           std::strstr(result_message, " x=") != nullptr && std::strstr(result_message, " y=") != nullptr &&
	           std::strstr(result_message, " z=") != nullptr,
	       "vertex clip probe result serialization stays compact and includes finite ranges");
	bounded_finite.clip_w_nonpositive = bounded_finite.clip_xy_outside =
	    bounded_finite.clip_z_outside_zero_to_one = bounded_finite.clip_inside_zero_to_one =
	        bounded_finite.clip_z_outside_negative_one_to_one = bounded_finite.clip_inside_negative_one_to_one =
	            std::numeric_limits<uint32_t>::max();
	Expect(VertexClipProbeFormatPopulationResultMessage(result_info, bounded_finite, result_message, sizeof(result_message)) &&
	           std::strlen(result_message) < sizeof(result_message) &&
	           std::strstr(result_message,
	                       "cs=0123456789abcdef k=i n=3564 s=2 wnp=4294967295 oxy=4294967295 oz01=4294967295"
	                       " in01=4294967295 ozn=4294967295 inn=4294967295") != nullptr,
	       "vertex clip population serialization stays bounded at maximum counter values");
	bounded_finite.param0_exports   = std::numeric_limits<uint32_t>::max();
	bounded_finite.param0_nonfinite = std::numeric_limits<uint32_t>::max();
	bounded_finite.min_param0_x = bounded_finite.min_param0_y = bounded_min;
	bounded_finite.max_param0_x = bounded_finite.max_param0_y = bounded_max;
	Expect(VertexClipProbeFormatParam0ResultMessage(result_info, bounded_finite, result_message, sizeof(result_message)) &&
	           std::strlen(result_message) < sizeof(result_message) &&
	           std::strstr(result_message,
	                       "cs=0123456789abcdef k=i n=3564 s=2 p0n=4294967295 p0nf=4294967295 p0fin=1") != nullptr &&
	           std::strstr(result_message, " p0x=") != nullptr && std::strstr(result_message, " p0y=") != nullptr,
	       "vertex PARAM0 result serialization stays compact and includes finite X/Y ranges");
	result_info.pixel_checksum                  = code_id;
	bounded_finite.pixel_input0_samples         = std::numeric_limits<uint32_t>::max();
	bounded_finite.pixel_input0_nonfinite       = std::numeric_limits<uint32_t>::max();
	bounded_finite.min_pixel_input0_x = bounded_finite.min_pixel_input0_y = bounded_min;
	bounded_finite.max_pixel_input0_x = bounded_finite.max_pixel_input0_y = bounded_max;
	Expect(VertexClipProbeFormatPixelInput0ResultMessage(result_info, bounded_finite, result_message, sizeof(result_message)) &&
	           std::strlen(result_message) < sizeof(result_message) &&
	           std::strstr(result_message,
	                       "ps=0123456789abcdef k=i n=3564 s=2 i0n=4294967295 i0nf=4294967295 i0fin=1") != nullptr &&
	           std::strstr(result_message, " i0x=") != nullptr && std::strstr(result_message, " i0y=") != nullptr,
	       "pixel input-zero result serialization stays compact and includes finite X/Y ranges");
	bounded_finite.pixel_sample_invocations = std::numeric_limits<uint32_t>::max();
	bounded_finite.pixel_sample_nonfinite   = std::numeric_limits<uint32_t>::max();
	bounded_finite.min_pixel_sample_r = bounded_finite.min_pixel_sample_g = bounded_finite.min_pixel_sample_b =
	    bounded_finite.min_pixel_sample_a = bounded_min;
	bounded_finite.max_pixel_sample_r = bounded_finite.max_pixel_sample_g = bounded_finite.max_pixel_sample_b =
	    bounded_finite.max_pixel_sample_a = bounded_max;
	Expect(VertexClipProbeFormatPixelSampleResultMessage(result_info, 1u, bounded_finite, result_message, sizeof(result_message)) &&
	           std::strlen(result_message) < sizeof(result_message) &&
	           std::strstr(result_message,
	                       "ps=0123456789abcdef k=i n=3564 s=2 ord=1 sn=4294967295 snf=4294967295 sfin=1") != nullptr &&
	           std::strstr(result_message, " r=") != nullptr && std::strstr(result_message, " g=") != nullptr &&
	           std::strstr(result_message, " b=") != nullptr && std::strstr(result_message, " a=") != nullptr,
	       "pixel sample-result event serialization remains bounded and includes ordinal plus RGBA extrema");
	bounded_finite.min_pixel_frag_x = bounded_finite.min_pixel_frag_y = VertexClipProbeEncodeOrderedFloat(0.5f);
	bounded_finite.max_pixel_frag_x = VertexClipProbeEncodeOrderedFloat(1279.5f);
	bounded_finite.max_pixel_frag_y = VertexClipProbeEncodeOrderedFloat(719.5f);
	Expect(VertexClipProbeFormatPixelMrtCoverageResultMessage(result_info, 0u, 1u, bounded_finite, result_message,
	                                                          sizeof(result_message)) &&
	           std::strlen(result_message) < sizeof(result_message) &&
	           std::strstr(result_message, "mrt=0 ord=1 cfin=1 x=0.5:1279.5 y=0.5:719.5") != nullptr,
	       "pixel MRT result serializes the finite host fragment coverage bounds");
	const uint8_t attachment_rgba8[] = {
	    0x01u, 0x00u, 0x00u, 0xffu, // RGB-nonzero
	    0x00u, 0x00u, 0x00u, 0x7fu, // alpha-only must not count
	    0x00u, 0x00u, 0x00u, 0x00u,
	    0x00u, 0x02u, 0x00u, 0x00u, // RGB-nonzero outside the first row
	};
	VertexClipProbeRawStats attachment_coverage {};
	attachment_coverage.min_pixel_frag_x = VertexClipProbeEncodeOrderedFloat(0.5f);
	attachment_coverage.max_pixel_frag_x = VertexClipProbeEncodeOrderedFloat(5.5f);
	attachment_coverage.min_pixel_frag_y = VertexClipProbeEncodeOrderedFloat(0.5f);
	attachment_coverage.max_pixel_frag_y = VertexClipProbeEncodeOrderedFloat(0.5f);
	VertexClipProbeAttachmentReadbackStats attachment_stats {};
	Expect(VertexClipProbeAggregateAttachmentReadback(VertexClipProbeAttachmentFormat::Rgba8, attachment_rgba8,
	                                                  sizeof(attachment_rgba8), 2u, 2u, attachment_coverage,
	                                                  &attachment_stats) &&
	           attachment_stats.width == 2u && attachment_stats.height == 2u &&
	           attachment_stats.bytes == sizeof(attachment_rgba8) && attachment_stats.rgb_nonzero_pixels == 2u &&
	           attachment_stats.rgb_nonzero_coverage_pixels == 1u && attachment_stats.coverage_bounds_available &&
	           attachment_stats.fnv1a64 != 0u,
	       "MRT attachment aggregation hashes raw bytes, ignores alpha-only RGBA8 pixels, and clamps host coverage");
	VertexClipProbeAttachmentReadbackStats repeated_attachment_stats {};
	Expect(VertexClipProbeAggregateAttachmentReadback(VertexClipProbeAttachmentFormat::Rgba8, attachment_rgba8,
	                                                  sizeof(attachment_rgba8), 2u, 2u, attachment_coverage,
	                                                  &repeated_attachment_stats) &&
	           repeated_attachment_stats.fnv1a64 == attachment_stats.fnv1a64,
	       "MRT attachment aggregation produces a stable raw-byte hash");
	uint8_t changed_attachment_rgba8[sizeof(attachment_rgba8)] {};
	std::memcpy(changed_attachment_rgba8, attachment_rgba8, sizeof(attachment_rgba8));
	changed_attachment_rgba8[3] = 0x00u;
	VertexClipProbeAttachmentReadbackStats changed_attachment_stats {};
	Expect(VertexClipProbeAggregateAttachmentReadback(VertexClipProbeAttachmentFormat::Rgba8, changed_attachment_rgba8,
	                                                  sizeof(changed_attachment_rgba8), 2u, 2u, attachment_coverage,
	                                                  &changed_attachment_stats) &&
	           changed_attachment_stats.rgb_nonzero_pixels == attachment_stats.rgb_nonzero_pixels &&
	           changed_attachment_stats.fnv1a64 != attachment_stats.fnv1a64,
	       "MRT attachment hashing observes raw alpha bytes without treating alpha as RGB coverage");
	const uint8_t attachment_before_rgba8[] = {
	    0x00u, 0x00u, 0x00u, 0xffu,
	    0x01u, 0x00u, 0x00u, 0xffu,
	    0x00u, 0x02u, 0x00u, 0xffu,
	    0x00u, 0x00u, 0x03u, 0x7fu,
	};
	const uint8_t attachment_after_rgba8[] = {
	    0x04u, 0x00u, 0x00u, 0xffu, // black -> RGB-nonzero inside coverage
	    0x00u, 0x00u, 0x00u, 0xffu, // RGB-nonzero -> black inside coverage
	    0x00u, 0x02u, 0x00u, 0xffu, // unchanged outside coverage
	    0x00u, 0x00u, 0x03u, 0xffu, // alpha-only change must not count
	};
	VertexClipProbeAttachmentDeltaStats attachment_delta {};
	Expect(VertexClipProbeAggregateAttachmentDelta(
	           VertexClipProbeAttachmentFormat::Rgba8, attachment_before_rgba8, attachment_after_rgba8,
	           sizeof(attachment_before_rgba8), 2u, 2u, attachment_coverage, &attachment_delta) &&
	           attachment_delta.rgb_changed_pixels == 2u && attachment_delta.rgb_changed_coverage_pixels == 2u &&
	           attachment_delta.rgb_zero_to_nonzero_coverage_pixels == 1u &&
	           attachment_delta.rgb_nonzero_to_zero_coverage_pixels == 1u &&
	           attachment_delta.coverage_bounds_available,
	       "MRT attachment delta attributes RGB changes inside the selected draw coverage without counting alpha-only bytes");
	Expect(VertexClipProbeFormatPixelMrtAttachmentDeltaMessage(
	           result_info, 0u, 229u, 0x1234abc0ull, 1u, 4u, VK_ATTACHMENT_LOAD_OP_LOAD,
	           VertexClipProbeAttachmentStatus::Ok,
	           &attachment_delta, result_message, sizeof(result_message)) &&
	           std::strstr(result_message, "mrt=0 ord=229 ga=00001234abc0 l=0 d=2 b=1 in=2 up=1 dn=1 m=1 r=4") != nullptr,
	       "MRT attachment delta event records selected guest-address provenance and bounded directional RGB changes");
	Expect(VertexClipProbeFormatPixelMrtAttachmentResultMessage(
	           result_info, 0u, 229u, 1u, 0u, VertexClipProbeAttachmentStatus::Ok, &attachment_stats, result_message,
	           sizeof(result_message)) &&
	           std::strlen(result_message) < sizeof(result_message) &&
	           std::strstr(result_message, "mrt=0 ord=229 f=rgba8 e=2x2 nz=2 b=1 in=1") != nullptr &&
	           std::strstr(result_message, "ok=1 m=1 r=0") != nullptr,
	       "MRT attachment event preserves exact selector provenance and bounded raw-byte statistics");
	VertexClipProbeResultInfo maximum_result = result_info;
	maximum_result.pixel_checksum            = std::numeric_limits<uint64_t>::max();
	maximum_result.guest_count               = std::numeric_limits<uint32_t>::max();
	maximum_result.descriptor_set            = 2u;
	VertexClipProbeAttachmentReadbackStats maximum_attachment_stats {};
	maximum_attachment_stats.format                      = VertexClipProbeAttachmentFormat::B10G11R11Ufloat;
	maximum_attachment_stats.width                       = std::numeric_limits<uint32_t>::max();
	maximum_attachment_stats.height                      = std::numeric_limits<uint32_t>::max();
	maximum_attachment_stats.bytes                       = std::numeric_limits<uint64_t>::max();
	maximum_attachment_stats.rgb_nonzero_pixels          = std::numeric_limits<uint64_t>::max();
	maximum_attachment_stats.rgb_nonzero_coverage_pixels = std::numeric_limits<uint64_t>::max();
	maximum_attachment_stats.fnv1a64                     = std::numeric_limits<uint64_t>::max();
	maximum_attachment_stats.coverage_bounds_available   = true;
	Expect(VertexClipProbeFormatPixelMrtAttachmentResultMessage(
	           maximum_result, 3u, std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(),
	           kVertexClipProbeAttachmentMaxEmptyRetries, VertexClipProbeAttachmentStatus::Ok,
	           &maximum_attachment_stats, result_message, sizeof(result_message)) &&
	           std::strlen(result_message) < sizeof(result_message),
	       "MRT attachment event remains below the agent limit for maximum accepted field widths");
	VertexClipProbeAttachmentDeltaStats maximum_delta {};
	constexpr uint64_t maximum_attachment_pixels = 64ull * 1024ull * 1024ull / 4ull;
	maximum_delta.rgb_changed_pixels                     = maximum_attachment_pixels;
	maximum_delta.rgb_changed_coverage_pixels            = maximum_attachment_pixels;
	maximum_delta.rgb_zero_to_nonzero_coverage_pixels    = maximum_attachment_pixels;
	maximum_delta.rgb_nonzero_to_zero_coverage_pixels    = maximum_attachment_pixels;
	maximum_delta.coverage_bounds_available              = true;
	Expect(VertexClipProbeFormatPixelMrtAttachmentDeltaMessage(
	           maximum_result, 3u, std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint64_t>::max(),
	           std::numeric_limits<uint32_t>::max(), kVertexClipProbeAttachmentMaxEmptyRetries, VK_ATTACHMENT_LOAD_OP_LOAD,
	           VertexClipProbeAttachmentStatus::Ok, &maximum_delta, result_message, sizeof(result_message)) &&
	           std::strlen(result_message) < sizeof(result_message),
	       "MRT attachment delta event remains below the agent limit with maximum bounded counts and guest-address width");
	Expect(VertexClipProbeFormatPixelMrtAttachmentResultMessage(
	           result_info, 0u, 229u, 1u, 0u, VertexClipProbeAttachmentStatus::UnsupportedFormat, nullptr, result_message,
	           sizeof(result_message)) &&
	           std::strstr(result_message, "status=skip_format m=1 r=0") != nullptr,
	       "MRT attachment event reports a bounded unsupported-format skip status");
	VertexClipProbeRawStats resolver_sample = initial_stats;
	Expect(VertexClipProbeFormatResolverResultMessage(result_info, resolver_sample, result_message, sizeof(result_message)) &&
	           std::strstr(result_message, "cs=0123456789abcdef k=i n=3564 s=2 c=0") != nullptr,
	       "unreached live resolver emits a bounded explicit no-sample record");
	resolver_sample.resolver_desc0 = 1u;
	Expect(!VertexClipProbeFormatResolverResultMessage(result_info, resolver_sample, result_message, sizeof(result_message)),
	       "unreached live resolver rejects a partially written payload");
	resolver_sample.resolver_desc0 = 0u;
	resolver_sample.resolver_claimed           = 1u;
	resolver_sample.resolver_instruction_pc    = 0x40u;
	resolver_sample.resolver_access_width      = 12u;
	resolver_sample.resolver_desc0             = 0x00102030u;
	resolver_sample.resolver_desc1             = 0x00000047u;
	resolver_sample.resolver_raw_byte_offset   = 24u;
	resolver_sample.resolver_valid             = 1u;
	resolver_sample.resolver_final_slot        = 3u;
	resolver_sample.resolver_final_byte_offset = 48u;
	Expect(VertexClipProbeFormatResolverResultMessage(result_info, resolver_sample, result_message, sizeof(result_message)) &&
	           std::strlen(result_message) < sizeof(result_message) &&
	           std::strstr(result_message, "c=1 p=00000040 w=12 d0=00102030 d1=00000047 ro=24 v=1 fs=3 fo=48") != nullptr,
	       "claimed live resolver sample serializes the causal address decision compactly");
	resolver_sample.resolver_valid             = 0u;
	resolver_sample.resolver_final_byte_offset = 48u;
	Expect(!VertexClipProbeFormatResolverResultMessage(result_info, resolver_sample, result_message, sizeof(result_message)),
	       "invalid live resolver decision rejects a nonzero final address");
	resolver_sample.resolver_valid             = 1u;
	resolver_sample.resolver_final_byte_offset = 48u;
	resolver_sample.resolver_claimed = 2u;
	Expect(!VertexClipProbeFormatResolverResultMessage(result_info, resolver_sample, result_message, sizeof(result_message)),
	       "live resolver serialization rejects an impossible multi-claim state");
	result_info.descriptor_set = 3u;
	Expect(!VertexClipProbeFormatResultMessage(result_info, bounded_finite, result_message, sizeof(result_message)),
	       "vertex clip probe result serialization rejects an invalid descriptor set");
	VertexClipProbeRawStats infinity_rejected {};
	infinity_rejected.invocations = 1;
	infinity_rejected.nonfinite   = 1;
	Expect(!VertexClipProbeHasFiniteExtrema(infinity_rejected),
	       "nonfinite-only vertex clip probe samples retain finite-extrema sentinels");

	// Parsed s_nop; EXP PARAM0; EXP POS0, v0, v1, v2, v3; s_endpgm. The leading no-op
	// preserves the generator's established terminal-instruction context while
	// keeping the focused source fixture minimal. PARAM0 uses the same sources
	// so the test can prove observation without changing the guest export.
	const uint32_t vertex_shader[] = {0xbf800000u, 0xf800020fu, 0x03020100u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
	ShaderCode     vertex_code {};
	vertex_code.SetType(ShaderType::Vertex);
	ShaderParse(vertex_shader, &vertex_code);
	Expect(vertex_code.GetInstructions().Size() == 4u,
	       "synthetic vertex output probe shader parses no-op, PARAM0, POS0, and endpgm");
	Expect(vertex_code.GetInstructions().At(1).type == ShaderInstructionType::Exp &&
	           vertex_code.GetInstructions().At(1).format == ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3,
	       "synthetic vertex output probe shader exports PARAM0");
	Expect(vertex_code.GetInstructions().At(2).type == ShaderInstructionType::Exp &&
	           vertex_code.GetInstructions().At(2).format == ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done,
	       "synthetic vertex clip probe shader exports Pos0");

	ShaderVertexInputInfo ordinary_vertex_input {};
	Expect(ordinary_vertex_input.clip_probe_descriptor_set == kVertexClipProbeInvalidDescriptorSet,
	       "ordinary vertex input retains the invalid host-only clip probe set");
	const auto ordinary_source = SpirvGenerateSource(vertex_code, &ordinary_vertex_input, nullptr, nullptr);
	Expect(ordinary_source.FindIndex("vertex_clip_probe") == Kyty::Core::STRING8_INVALID_INDEX,
	       "ordinary vertex source does not contain clip probe symbols");
	ExpectValidSpirv(ordinary_source, "ordinary synthetic vertex source validates before clip probe injection");

	ShaderVertexInputInfo probe_vertex_input {};
	probe_vertex_input.clip_probe.enabled             = true;
	probe_vertex_input.clip_probe.draw_scoped         = true;
	probe_vertex_input.clip_probe.diagnostic_identity = set_identity_0;
	probe_vertex_input.clip_probe_descriptor_set      = 0u;
	const auto probe_source = SpirvGenerateSource(vertex_code, &probe_vertex_input, nullptr, nullptr);
	Expect(probe_source.FindIndex("OpStore %param0 %t4_1") != Kyty::Core::STRING8_INVALID_INDEX,
	       "vertex PARAM0 probe retains the existing varying store");
	Expect(probe_source.FindIndex("OpStore %t5_2 %t4_2") != Kyty::Core::STRING8_INVALID_INDEX,
	       "vertex clip probe retains the existing gl_Position store");
	Expect(probe_source.FindIndex("OpDecorate %vertex_clip_probe DescriptorSet 0") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source.FindIndex("OpDecorate %vertex_clip_probe Binding 0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "vertex clip probe declares its host-only descriptor at the requested set and binding");
	for (uint32_t member = 0; member < 51u; member++)
	{
		const auto decoration = Kyty::Core::String8::FromPrintf(
		    "OpMemberDecorate %%VertexClipProbeRawStats %u Offset %u", member,
		    static_cast<uint32_t>(member * sizeof(uint32_t)));
		Expect(probe_source.FindIndex(decoration) != Kyty::Core::STRING8_INVALID_INDEX,
		       "vertex clip probe lays out every raw-stat uint at its explicit byte offset");
	}
	Expect(probe_source.FindIndex("OpAtomicIAdd") != Kyty::Core::STRING8_INVALID_INDEX,
	       "vertex clip probe counts invocations and nonfinite observations with uint atomics");
	Expect(probe_source.FindIndex("vertex_clip_probe_w_nonpositive_ptr_2") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source.FindIndex("vertex_clip_probe_xy_outside_ptr_2") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source.FindIndex("vertex_clip_probe_z01_outside_ptr_2") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source.FindIndex("vertex_clip_probe_inside01_ptr_2") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source.FindIndex("vertex_clip_probe_zn11_outside_ptr_2") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source.FindIndex("vertex_clip_probe_insiden11_ptr_2") != Kyty::Core::STRING8_INVALID_INDEX,
	       "vertex clip probe records bounded population classes for both Vulkan clip conventions");
	const auto nonpositive_branch = probe_source.FindIndex(
	    "OpBranchConditional %vertex_clip_probe_w_nonpositive_2 %vertex_clip_probe_w_nonpositive_block_2"
	    " %vertex_clip_probe_w_positive_block_2");
	const auto raw_clip_compare =
	    probe_source.FindIndex("%vertex_clip_probe_x_inside_low_2 = OpFOrdGreaterThanEqual %bool %vertex_clip_probe_x_2"
	                           " %vertex_clip_probe_neg_w_2");
	const auto first_divide = probe_source.FindIndex("%vertex_clip_probe_x_over_w_2 = OpFDiv %float");
	Expect(nonpositive_branch != Kyty::Core::STRING8_INVALID_INDEX &&
	           raw_clip_compare != Kyty::Core::STRING8_INVALID_INDEX && first_divide != Kyty::Core::STRING8_INVALID_INDEX &&
	           raw_clip_compare < first_divide &&
	           probe_source.FindIndex("vertex_clip_probe_nonfinite_ratio_ptr_2") == Kyty::Core::STRING8_INVALID_INDEX,
	       "zero and negative W terminate in one class while positive finite positions classify against raw clip coordinates"
	       " before any potentially overflowing perspective divide");
	Expect(probe_source.FindIndex("OpAtomicUMin") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source.FindIndex("OpAtomicUMax") != Kyty::Core::STRING8_INVALID_INDEX,
	       "vertex clip probe updates extrema with uint atomics");
	Expect(probe_source.FindIndex("OpIsNan") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source.FindIndex("OpIsInf") != Kyty::Core::STRING8_INVALID_INDEX,
	       "vertex clip probe guards extrema with NaN and infinity checks");
	Expect(probe_source.FindIndex("vertex_param0_probe_exports_ptr_1") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source.FindIndex("vertex_param0_probe_nonfinite_ptr_1") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source.FindIndex("vertex_param0_probe_min_x_ptr_1") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source.FindIndex("vertex_param0_probe_max_y_ptr_1") != Kyty::Core::STRING8_INVALID_INDEX,
	       "selected PARAM0 export records count, nonfinite observations, and X/Y extrema");
	Expect(probe_source.FindIndex("OpAtomicFMinEXT") == Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source.FindIndex("OpAtomicFMaxEXT") == Kyty::Core::STRING8_INVALID_INDEX,
	       "vertex clip probe does not require floating-point atomic extensions");
	ExpectValidSpirv(probe_source, "vertex clip probe synthetic source validates");

	ShaderInstruction resolver_load {};
	resolver_load.type                = ShaderInstructionType::BufferLoadFormatXyz;
	resolver_load.format              = ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen;
	resolver_load.pc                  = 0x40u;
	resolver_load.dst                 = {.type = ShaderOperandType::Vgpr, .register_id = 4, .size = 3};
	resolver_load.src[0]              = {.type = ShaderOperandType::Vgpr, .register_id = 1, .size = 1};
	resolver_load.src[1]              = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 4};
	resolver_load.src[2].type         = ShaderOperandType::IntegerInlineConstant;
	resolver_load.src[2].constant.u   = 0u;
	resolver_load.src_num             = 3;
	resolver_load.buffer_idxen        = true;
	ShaderCode resolver_code {};
	resolver_code.SetType(ShaderType::Vertex);
	resolver_code.GetInstructions().Add(resolver_load);
	for (const auto& instruction: vertex_code.GetInstructions())
	{
		resolver_code.GetInstructions().Add(instruction);
	}
	ShaderVertexInputInfo resolver_input = probe_vertex_input;
	resolver_input.clip_probe.diagnostic_identity = set_identity_1;
	resolver_input.clip_probe_descriptor_set      = 1u;
	resolver_input.fetch_embedded                 = true;
	resolver_input.buffers_num                    = 1;
	resolver_input.buffers[0].addr                = 0x00102030u;
	resolver_input.buffers[0].stride              = 24u;
	resolver_input.buffers[0].num_records         = 64u;
	resolver_input.buffers[0].storage_slot        = 0;
	resolver_input.bind.storage_buffers.buffers_num       = 1;
	resolver_input.bind.storage_buffers.start_register[0] = 0;
	resolver_input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::ReadOnly;
	resolver_input.bind.push_constant_size                = 16u;
	ShaderVertexInputInfo ordinary_resolver_input = resolver_input;
	ordinary_resolver_input.clip_probe                    = {};
	ordinary_resolver_input.clip_probe_descriptor_set     = kVertexClipProbeInvalidDescriptorSet;
	const auto ordinary_resolver_source = SpirvGenerateSource(resolver_code, &ordinary_resolver_input, nullptr, nullptr);
	Expect(ordinary_resolver_source.FindIndex("%buf_addr_valid_c_0_0 = OpLogicalAnd %bool") !=
	               Kyty::Core::STRING8_INVALID_INDEX &&
	           ordinary_resolver_source.FindIndex("vertex_resolver_") == Kyty::Core::STRING8_INVALID_INDEX &&
	           ordinary_resolver_source.FindIndex("vertex_clip_probe") == Kyty::Core::STRING8_INVALID_INDEX,
	       "ordinary embedded MUBUF retains its existing resolver without host diagnostic storage or stores");
	ExpectValidSpirv(ordinary_resolver_source, "ordinary embedded MUBUF source validates without live-resolver probe");
	const auto resolver_source = SpirvGenerateSource(resolver_code, &resolver_input, nullptr, nullptr);
	const auto resolver_decision = resolver_source.FindIndex("%buf_addr_valid_c_0_0 = OpLogicalAnd %bool");
	const auto resolver_claim = resolver_source.FindIndex(
	    "%vertex_resolver_claim_prior_0_0 = OpAtomicCompareExchange %uint");
	Expect(resolver_decision != Kyty::Core::STRING8_INVALID_INDEX &&
	           resolver_claim != Kyty::Core::STRING8_INVALID_INDEX && resolver_decision < resolver_claim,
	       "selected embedded MUBUF records only after the final live-address decision");
	Expect(resolver_source.FindIndex("OpStore %vertex_resolver_desc0_ptr_0_0 %buf_addr_desc0_0_0") !=
	               Kyty::Core::STRING8_INVALID_INDEX &&
	           resolver_source.FindIndex("OpStore %vertex_resolver_slot_ptr_0_0 %buf_addr_slot_c_0_0") !=
	               Kyty::Core::STRING8_INVALID_INDEX &&
	           resolver_source.FindIndex("OpStore %vertex_resolver_offset_ptr_0_0 %buf_addr_off_c_0_0") !=
	               Kyty::Core::STRING8_INVALID_INDEX,
	       "selected embedded MUBUF captures descriptor and final SSBO address without changing the load");
	ExpectValidSpirv(resolver_source, "selected embedded MUBUF live-resolver source validates");
	const uint32_t xy_param0_shader[] = {
	    0xbf800000u, 0xf8000203u, 0x03020100u, 0xf80008cfu, 0x03020100u, 0xbf810000u,
	};
	ShaderCode xy_param0_code {};
	xy_param0_code.SetType(ShaderType::Vertex);
	ShaderParse(xy_param0_shader, &xy_param0_code);
	Expect(xy_param0_code.GetInstructions().At(1).format == ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3 &&
	           xy_param0_code.GetInstructions().At(1).exp_enable_mask == 0x03u,
	       "XY-only PARAM0 fixture reaches the established full-format fallback while retaining its channel mask");
	const auto xy_param0_source = SpirvGenerateSource(xy_param0_code, &probe_vertex_input, nullptr, nullptr);
	Expect(xy_param0_source.FindIndex("OpStore %param0 %t4_1") != Kyty::Core::STRING8_INVALID_INDEX &&
	           xy_param0_source.FindIndex("vertex_param0_probe_exports_ptr_1") != Kyty::Core::STRING8_INVALID_INDEX,
	       "XY-only PARAM0 preserves the export and populates the complete X/Y diagnostic contract");
	ExpectValidSpirv(xy_param0_source, "XY-only PARAM0 selected source validates with PARAM0 probe atomics");
	const uint32_t x_only_param0_shader[] = {
	    0xbf800000u, 0xf8000201u, 0x03020100u, 0xf80008cfu, 0x03020100u, 0xbf810000u,
	};
	ShaderCode x_only_param0_code {};
	x_only_param0_code.SetType(ShaderType::Vertex);
	ShaderParse(x_only_param0_shader, &x_only_param0_code);
	const auto x_only_param0_source = SpirvGenerateSource(x_only_param0_code, &probe_vertex_input, nullptr, nullptr);
	Expect(x_only_param0_source.FindIndex("OpStore %param0 %t4_1") != Kyty::Core::STRING8_INVALID_INDEX &&
	           x_only_param0_source.FindIndex("vertex_param0_probe_") == Kyty::Core::STRING8_INVALID_INDEX,
	       "PARAM0 without Y preserves the existing export but cannot populate the X/Y diagnostic contract");
	ExpectValidSpirv(x_only_param0_source, "X-only PARAM0 selected source validates without PARAM0 probe atomics");
	ShaderVertexInputInfo probe_vertex_input_set_1 = probe_vertex_input;
	probe_vertex_input_set_1.clip_probe.diagnostic_identity = set_identity_1;
	probe_vertex_input_set_1.clip_probe_descriptor_set      = 1u;
	const auto probe_source_set_1 = SpirvGenerateSource(vertex_code, &probe_vertex_input_set_1, nullptr, nullptr);
	Expect(probe_source_set_1.FindIndex("OpDecorate %vertex_clip_probe DescriptorSet 1") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_source_set_1.FindIndex("OpDecorate %vertex_clip_probe DescriptorSet 0") == Kyty::Core::STRING8_INVALID_INDEX,
	       "vertex clip probe selected set changes the generated descriptor decoration");
	ExpectValidSpirv(probe_source_set_1, "vertex clip probe set-one synthetic source validates");
	const auto probe_binary = ShaderRecompileVS(vertex_code, &probe_vertex_input);
	Expect(!probe_binary.IsEmpty(), "vertex clip probe synthetic source recompiles");
	ExpectValidSpirv(probe_binary, "vertex clip probe synthetic binary validates");

	ShaderInstruction interp_x {};
	interp_x.type                = ShaderInstructionType::VInterpP2F32;
	interp_x.format              = ShaderInstructionFormat::VdstVsrcAttrChan;
	interp_x.dst                 = {.type = ShaderOperandType::Vgpr, .register_id = 4, .size = 1};
	interp_x.src[0]              = {.type = ShaderOperandType::Vgpr, .register_id = 1, .size = 1};
	interp_x.src[1].type         = ShaderOperandType::IntegerInlineConstant;
	interp_x.src[1].constant.u   = 0;
	interp_x.src[2].type         = ShaderOperandType::IntegerInlineConstant;
	interp_x.src[2].constant.u   = 0;
	interp_x.src_num             = 3;
	ShaderInstruction interp_y  = interp_x;
	interp_y.dst.register_id     = 5;
	interp_y.src[2].constant.u   = 1;
	ShaderInstruction sample {};
	sample.type                  = ShaderInstructionType::ImageSampleB;
	sample.format                = ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3;
	sample.dst                   = {.type = ShaderOperandType::Vgpr, .register_id = 8, .size = 2};
	sample.src[0]                = {.type = ShaderOperandType::Vgpr, .register_id = 3, .size = 3};
	sample.src[1]                = {.type = ShaderOperandType::Sgpr, .register_id = 8, .size = 8};
	sample.src[2]                = {.type = ShaderOperandType::Sgpr, .register_id = 20, .size = 4};
	sample.src_num               = 3;
	sample.mimg_dimension        = 1;
	sample.mimg_dmask            = 0x3;
	ShaderInstruction nop {};
	nop.type                     = ShaderInstructionType::VNop;
	nop.format                   = ShaderInstructionFormat::Empty;
	ShaderInstruction end {};
	end.type                     = ShaderInstructionType::SEndpgm;
	end.format                   = ShaderInstructionFormat::Empty;
	ShaderCode pixel_code {};
	pixel_code.SetType(ShaderType::Pixel);
	pixel_code.GetInstructions().Add(interp_x);
	pixel_code.GetInstructions().Add(interp_y);
	pixel_code.GetInstructions().Add(sample);
	pixel_code.GetInstructions().Add(sample);
	pixel_code.GetInstructions().Add(nop);
	pixel_code.GetInstructions().Add(end);
	ShaderPixelInputInfo ordinary_pixel_input {};
	ordinary_pixel_input.input_num                              = 1;
	ordinary_pixel_input.bind.push_constant_size                = 48;
	ordinary_pixel_input.bind.textures2D.textures_num           = 1;
	ordinary_pixel_input.bind.textures2D.textures2d_sampled_num = 1;
	ordinary_pixel_input.bind.textures2D.desc[0].start_register = 8;
	ordinary_pixel_input.bind.textures2D.desc[0].usage          = ShaderTextureUsage::ReadOnly;
	ordinary_pixel_input.bind.textures2D.desc[0].texture.fields[1] = 22u << 20u;
	ordinary_pixel_input.bind.textures2D.desc[0].texture.fields[3] = 9u << 28u;
	ordinary_pixel_input.bind.samplers.samplers_num                = 1;
	ordinary_pixel_input.bind.samplers.start_register[0]           = 20;
	ordinary_pixel_input.ps_early_z                                = true;
	ShaderCalcBindingIndices(&ordinary_pixel_input.bind);

	ShaderInstruction scalar_to_vgpr {};
	scalar_to_vgpr.type     = ShaderInstructionType::VCvtI32F32;
	scalar_to_vgpr.format   = ShaderInstructionFormat::SVdstSVsrc0;
	scalar_to_vgpr.dst      = {.type = ShaderOperandType::Vgpr, .register_id = 2, .size = 1};
	scalar_to_vgpr.src[0]   = {.type = ShaderOperandType::VccLo, .size = 1};
	scalar_to_vgpr.src_num  = 1;
	ShaderInstruction read_first_lane {};
	read_first_lane.type    = ShaderInstructionType::VReadfirstlaneB32;
	read_first_lane.format  = ShaderInstructionFormat::SVdstSVsrc0;
	read_first_lane.dst     = {.type = ShaderOperandType::VccLo, .size = 1};
	read_first_lane.src[0]  = {.type = ShaderOperandType::Vgpr, .register_id = 2, .size = 1};
	read_first_lane.src_num = 1;
	ShaderInstruction scalar_vcc_select {};
	scalar_vcc_select.type     = ShaderInstructionType::SCselectB32;
	scalar_vcc_select.format   = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	scalar_vcc_select.dst      = {.type = ShaderOperandType::VccLo, .size = 1};
	scalar_vcc_select.src[0]   = {.type = ShaderOperandType::Sgpr, .register_id = 4, .size = 1};
	scalar_vcc_select.src[1]   = {.type = ShaderOperandType::Sgpr, .register_id = 5, .size = 1};
	scalar_vcc_select.src_num  = 2;
	ShaderCode uniform_readfirstlane_code {};
	uniform_readfirstlane_code.SetType(ShaderType::Pixel);
	uniform_readfirstlane_code.GetInstructions().Add(scalar_vcc_select);
	uniform_readfirstlane_code.GetInstructions().Add(scalar_to_vgpr);
	uniform_readfirstlane_code.GetInstructions().Add(read_first_lane);
	uniform_readfirstlane_code.GetInstructions().Add(end);
	const auto uniform_readfirstlane_source =
	    SpirvGenerateSource(uniform_readfirstlane_code, nullptr, &ordinary_pixel_input, nullptr);
	Expect(ShaderReadfirstlaneCanUseUniformCopy(uniform_readfirstlane_code, 2u),
	       "readfirstlane uniformity analysis follows scalar-derived VGPR input");
	Expect(uniform_readfirstlane_source.FindIndex("OpGroupNonUniformBallot") == Kyty::Core::STRING8_INVALID_INDEX,
	       "wave-uniform readfirstlane emits no native subgroup ballot");
	Expect(uniform_readfirstlane_source.FindIndex("BuiltIn SubgroupLocalInvocationId") == Kyty::Core::STRING8_INVALID_INDEX,
	       "wave-uniform readfirstlane emits no subgroup invocation builtin");
	Expect(uniform_readfirstlane_source.FindIndex("OpStore %vcc_lo %t0_2") != Kyty::Core::STRING8_INVALID_INDEX,
	       "wave-uniform readfirstlane stores the scalar-derived source directly");
	ExpectValidSpirv(uniform_readfirstlane_source, "wave-uniform readfirstlane source validates");

	ShaderInstruction bypass_branch {};
	bypass_branch.pc              = 0;
	bypass_branch.type            = ShaderInstructionType::SCbranchScc1;
	bypass_branch.format          = ShaderInstructionFormat::Label;
	bypass_branch.src[0]          = {.type = ShaderOperandType::LiteralConstant};
	bypass_branch.src[0].constant = {.i = 8};
	bypass_branch.src_num         = 1;
	ShaderCode bypassed_readfirstlane_code {};
	bypassed_readfirstlane_code.SetType(ShaderType::Pixel);
	bypassed_readfirstlane_code.GetInstructions().Add(bypass_branch);
	scalar_vcc_select.pc = 4;
	scalar_to_vgpr.pc    = 8;
	read_first_lane.pc   = 12;
	end.pc               = 16;
	bypassed_readfirstlane_code.GetInstructions().Add(scalar_vcc_select);
	bypassed_readfirstlane_code.GetInstructions().Add(scalar_to_vgpr);
	bypassed_readfirstlane_code.GetInstructions().Add(read_first_lane);
	bypassed_readfirstlane_code.GetInstructions().Add(end);
	bypassed_readfirstlane_code.GetLabels().Add(ShaderLabel(read_first_lane.pc, bypass_branch.pc));
	Expect(!ShaderReadfirstlaneCanUseUniformCopy(bypassed_readfirstlane_code, 3u),
	       "readfirstlane rejects a producer that an incoming edge can bypass");

	ShaderInstruction divergent_vcc {};
	divergent_vcc.type     = ShaderInstructionType::VCmpEqF32;
	divergent_vcc.format   = ShaderInstructionFormat::SmaskVsrc0Vsrc1;
	divergent_vcc.dst      = {.type = ShaderOperandType::VccLo, .size = 2};
	divergent_vcc.src[0]   = {.type = ShaderOperandType::Vgpr, .register_id = 6, .size = 1};
	divergent_vcc.src[1]   = {.type = ShaderOperandType::Vgpr, .register_id = 7, .size = 1};
	divergent_vcc.src_num  = 2;
	ShaderCode divergent_vcc_readfirstlane_code {};
	divergent_vcc_readfirstlane_code.SetType(ShaderType::Pixel);
	divergent_vcc_readfirstlane_code.GetInstructions().Add(divergent_vcc);
	divergent_vcc_readfirstlane_code.GetInstructions().Add(scalar_to_vgpr);
	divergent_vcc_readfirstlane_code.GetInstructions().Add(read_first_lane);
	divergent_vcc_readfirstlane_code.GetInstructions().Add(end);
	Expect(!ShaderReadfirstlaneCanUseUniformCopy(divergent_vcc_readfirstlane_code, 2u),
	       "readfirstlane rejects a VGPR derived from lane-divergent VCC");

	ShaderInstruction covering_write {};
	covering_write.type = ShaderInstructionType::BufferLoadDwordx3;
	covering_write.dst  = {.type = ShaderOperandType::Vgpr, .register_id = 1, .size = 3};
	ShaderCode covered_vgpr_readfirstlane_code {};
	covered_vgpr_readfirstlane_code.SetType(ShaderType::Pixel);
	covered_vgpr_readfirstlane_code.GetInstructions().Add(scalar_vcc_select);
	covered_vgpr_readfirstlane_code.GetInstructions().Add(scalar_to_vgpr);
	covered_vgpr_readfirstlane_code.GetInstructions().Add(covering_write);
	covered_vgpr_readfirstlane_code.GetInstructions().Add(read_first_lane);
	covered_vgpr_readfirstlane_code.GetInstructions().Add(end);
	Expect(!ShaderReadfirstlaneCanUseUniformCopy(covered_vgpr_readfirstlane_code, 3u),
	       "readfirstlane rejects a later multi-register write covering its VGPR source");

	ShaderInstruction cmpx = divergent_vcc;
	cmpx.type               = ShaderInstructionType::VCmpxEqF32;
	ShaderCode cmpx_readfirstlane_code {};
	cmpx_readfirstlane_code.SetType(ShaderType::Pixel);
	cmpx_readfirstlane_code.GetInstructions().Add(scalar_vcc_select);
	cmpx_readfirstlane_code.GetInstructions().Add(scalar_to_vgpr);
	cmpx_readfirstlane_code.GetInstructions().Add(cmpx);
	cmpx_readfirstlane_code.GetInstructions().Add(read_first_lane);
	cmpx_readfirstlane_code.GetInstructions().Add(end);
	Expect(!ShaderReadfirstlaneCanUseUniformCopy(cmpx_readfirstlane_code, 3u),
	       "readfirstlane rejects a producer slice crossed by implicit VCMPX EXEC mutation");

	ShaderInstruction clear_exec {};
	clear_exec.type     = ShaderInstructionType::SMovB64;
	clear_exec.format   = ShaderInstructionFormat::Sdst2Ssrc02;
	clear_exec.dst      = {.type = ShaderOperandType::ExecLo, .size = 2};
	clear_exec.src[0]   = {.type = ShaderOperandType::IntegerInlineConstant, .size = 2};
	clear_exec.src_num  = 1;
	ShaderCode zero_exec_readfirstlane_code {};
	zero_exec_readfirstlane_code.SetType(ShaderType::Pixel);
	zero_exec_readfirstlane_code.GetInstructions().Add(scalar_vcc_select);
	zero_exec_readfirstlane_code.GetInstructions().Add(scalar_to_vgpr);
	zero_exec_readfirstlane_code.GetInstructions().Add(clear_exec);
	zero_exec_readfirstlane_code.GetInstructions().Add(read_first_lane);
	zero_exec_readfirstlane_code.GetInstructions().Add(end);
	Expect(!ShaderReadfirstlaneCanUseUniformCopy(zero_exec_readfirstlane_code, 3u),
	       "readfirstlane preserves the native empty-EXEC result after an explicit mask write");

	ShaderInstruction save_exec {};
	save_exec.type     = ShaderInstructionType::SAndSaveexecB64;
	save_exec.format   = ShaderInstructionFormat::Sdst2Ssrc02;
	save_exec.dst      = {.type = ShaderOperandType::Sgpr, .register_id = 8, .size = 2};
	save_exec.src[0]   = {.type = ShaderOperandType::VccLo, .size = 2};
	save_exec.src_num  = 1;
	ShaderCode save_exec_readfirstlane_code {};
	save_exec_readfirstlane_code.SetType(ShaderType::Pixel);
	save_exec_readfirstlane_code.GetInstructions().Add(scalar_vcc_select);
	save_exec_readfirstlane_code.GetInstructions().Add(scalar_to_vgpr);
	save_exec_readfirstlane_code.GetInstructions().Add(save_exec);
	save_exec_readfirstlane_code.GetInstructions().Add(read_first_lane);
	save_exec_readfirstlane_code.GetInstructions().Add(end);
	Expect(!ShaderReadfirstlaneCanUseUniformCopy(save_exec_readfirstlane_code, 3u),
	       "readfirstlane rejects a producer slice crossed by implicit saveexec mutation");

	ShaderInstruction nonuniform_value {};
	nonuniform_value.type     = ShaderInstructionType::VMovB32;
	nonuniform_value.format   = ShaderInstructionFormat::SVdstSVsrc0;
	nonuniform_value.dst      = {.type = ShaderOperandType::Vgpr, .register_id = 2, .size = 1};
	nonuniform_value.src[0]   = {.type = ShaderOperandType::Vgpr, .register_id = 3, .size = 1};
	nonuniform_value.src_num  = 1;
	ShaderCode nonuniform_readfirstlane_code {};
	nonuniform_readfirstlane_code.SetType(ShaderType::Pixel);
	nonuniform_readfirstlane_code.GetInstructions().Add(nonuniform_value);
	nonuniform_readfirstlane_code.GetInstructions().Add(read_first_lane);
	nonuniform_readfirstlane_code.GetInstructions().Add(end);
	const auto nonuniform_readfirstlane_source =
	    SpirvGenerateSource(nonuniform_readfirstlane_code, nullptr, &ordinary_pixel_input, nullptr);
	Expect(nonuniform_readfirstlane_source.FindIndex("OpGroupNonUniformBallot") != Kyty::Core::STRING8_INVALID_INDEX &&
	           nonuniform_readfirstlane_source.FindIndex("BuiltIn SubgroupLocalInvocationId") != Kyty::Core::STRING8_INVALID_INDEX,
	       "nonuniform readfirstlane retains native subgroup exchange");
	ExpectValidSpirv(nonuniform_readfirstlane_source, "nonuniform readfirstlane source validates");

	const auto ordinary_pixel_source = SpirvGenerateSource(pixel_code, nullptr, &ordinary_pixel_input, nullptr);
	Expect(ordinary_pixel_source.FindIndex("pixel_input0_probe") == Kyty::Core::STRING8_INVALID_INDEX,
	       "ordinary pixel source contains no input-zero probe symbols");
	Expect(ordinary_pixel_source.FindIndex("OpExecutionMode %main EarlyFragmentTests") != Kyty::Core::STRING8_INVALID_INDEX,
	       "ordinary opaque early-Z pixel source preserves early fragment tests");
	ExpectValidSpirv(ordinary_pixel_source, "ordinary synthetic pixel source validates without input-zero probe");
	ShaderPixelInputInfo probe_pixel_input = ordinary_pixel_input;
	probe_pixel_input.input0_probe.enabled             = true;
	probe_pixel_input.input0_probe.draw_scoped         = true;
	probe_pixel_input.input0_probe.diagnostic_identity = set_identity_0;
	probe_pixel_input.input0_probe_descriptor_set      = 0u;
	const auto probe_pixel_source = SpirvGenerateSource(pixel_code, nullptr, &probe_pixel_input, nullptr);
	const auto probe_observation = probe_pixel_source.FindIndex("pixel_input0_probe_count_prior_2 = OpAtomicIAdd");
	const auto first_sample      = probe_pixel_source.FindIndex("OpImageSampleImplicitLod %v4float %image_sampled_image_2");
	Expect(probe_pixel_source.FindIndex("OpDecorate %vertex_clip_probe DescriptorSet 0") != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_observation != Kyty::Core::STRING8_INVALID_INDEX && first_sample != Kyty::Core::STRING8_INVALID_INDEX &&
	           probe_observation < first_sample,
	       "selected pixel source observes consumed X/Y immediately before the first implicit sample");
	Expect(probe_pixel_source.FindIndex("pixel_input0_probe_count_ptr_3") == Kyty::Core::STRING8_INVALID_INDEX,
	       "selected pixel source instruments only the first IMAGE_SAMPLE_B");
	Expect(probe_pixel_source.FindIndex("OpExecutionMode %main EarlyFragmentTests") == Kyty::Core::STRING8_INVALID_INDEX,
	       "selected pixel probe observes covered fragments before the ordinary depth test");
	ExpectValidSpirv(probe_pixel_source, "pixel input-zero selected source validates");
	const auto probe_pixel_binary = ShaderRecompilePS(pixel_code, &probe_pixel_input);
	Expect(!probe_pixel_binary.IsEmpty(), "pixel input-zero selected source recompiles");
	ExpectValidSpirv(probe_pixel_binary, "pixel input-zero selected binary validates");

	ShaderPixelInputInfo sample_pixel_input = ordinary_pixel_input;
	sample_pixel_input.input0_probe.enabled             = true;
	sample_pixel_input.input0_probe.draw_scoped         = true;
	sample_pixel_input.input0_probe.kind                = ShaderPixelProbeKind::SampleResult;
	sample_pixel_input.input0_probe.sample_ordinal      = 2u;
	sample_pixel_input.input0_probe.diagnostic_identity = PixelSampleProbeDiagnosticIdentity(0u, 2u);
	sample_pixel_input.input0_probe_descriptor_set      = 0u;
	Expect(ShaderPixelSampleProbeMatchesInstruction(pixel_code, sample_pixel_input.input0_probe),
	       "pixel sample probe accepts an absolute instruction ordinal that names IMAGE_SAMPLE_B");
	auto invalid_sample_config = sample_pixel_input.input0_probe;
	invalid_sample_config.sample_ordinal = 1u;
	Expect(!ShaderPixelSampleProbeMatchesInstruction(pixel_code, invalid_sample_config),
	       "pixel sample probe rejects an absolute instruction ordinal that names another opcode");
	const auto sample_pixel_source = SpirvGenerateSource(pixel_code, nullptr, &sample_pixel_input, nullptr);
	const auto sampled_value = sample_pixel_source.FindIndex("OpImageSampleImplicitLod %v4float %image_sampled_image_2");
	const auto sample_observation = sample_pixel_source.FindIndex("pixel_sample_probe_count_prior_2 = OpAtomicIAdd");
	const auto destination_component =
	    sample_pixel_source.FindIndex("%image_sample_component_2_0 = OpCompositeExtract %float %image_sample_value_2 0");
	Expect(sampled_value != Kyty::Core::STRING8_INVALID_INDEX,
	       "pixel sample probe retains the selected image-sample operation");
	Expect(sample_observation != Kyty::Core::STRING8_INVALID_INDEX,
	       "pixel sample probe emits the selected aggregate");
	Expect(destination_component != Kyty::Core::STRING8_INVALID_INDEX,
	       "pixel sample probe retains selected sample destination materialization");
	Expect(sampled_value < sample_observation && sample_observation < destination_component,
	       "pixel sample probe observes the selected result after sampling and before destination materialization");
	Expect(sample_pixel_source.FindIndex("pixel_sample_probe_count_ptr_3") == Kyty::Core::STRING8_INVALID_INDEX,
	       "pixel sample probe instruments only its absolute instruction ordinal");
	Expect(sample_pixel_source.FindIndex("OpExecutionMode %main EarlyFragmentTests") != Kyty::Core::STRING8_INVALID_INDEX &&
	           sample_pixel_source.FindIndex("%fs_tap_") == Kyty::Core::STRING8_INVALID_INDEX,
	       "pixel sample probe preserves guest early tests and does not activate destructive MRT visualization");
	ExpectValidSpirv(sample_pixel_source, "pixel sample-result selected source validates");
	const auto sample_pixel_binary = ShaderRecompilePS(pixel_code, &sample_pixel_input);
	Expect(!sample_pixel_binary.IsEmpty(), "pixel sample-result selected source recompiles");
	ExpectValidSpirv(sample_pixel_binary, "pixel sample-result selected binary validates");

	ShaderPixelInputInfo sparse_sample_pixel_input = sample_pixel_input;
	sparse_sample_pixel_input.input0_probe = sparse_sample_selected;
	sparse_sample_pixel_input.input0_probe_descriptor_set = 0u;
	const auto sparse_sample_pixel_source = SpirvGenerateSource(pixel_code, nullptr, &sparse_sample_pixel_input, nullptr);
	Expect(sparse_sample_pixel_source.FindIndex("OpCapability GroupNonUniform") != Kyty::Core::STRING8_INVALID_INDEX &&
	           sparse_sample_pixel_source.FindIndex("OpGroupNonUniformElect") != Kyty::Core::STRING8_INVALID_INDEX,
	       "sparse pixel sample observation records one elected lane per subgroup");
	ExpectValidSpirv(sparse_sample_pixel_source, "sparse pixel sample-result selected source validates");

	ShaderInstruction mrt_export {};
	mrt_export.type     = ShaderInstructionType::Exp;
	mrt_export.format   = ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone;
	mrt_export.src[0]   = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	mrt_export.src[1]   = {.type = ShaderOperandType::Vgpr, .register_id = 1, .size = 1};
	mrt_export.src_num  = 2;
	mrt_export.exp_enable_mask = 0xf;
	ShaderCode mrt_pixel_code {};
	mrt_pixel_code.SetType(ShaderType::Pixel);
	mrt_pixel_code.GetInstructions().Add(nop);
	mrt_pixel_code.GetInstructions().Add(mrt_export);
	mrt_pixel_code.GetInstructions().Add(end);
	ShaderPixelInputInfo mrt_pixel_input {};
	mrt_pixel_input.target_output_mode[0]                 = 4;
	mrt_pixel_input.ps_early_z                            = true;
	mrt_pixel_input.input0_probe.enabled                  = true;
	mrt_pixel_input.input0_probe.draw_scoped              = true;
	mrt_pixel_input.input0_probe.kind                     = ShaderPixelProbeKind::FinalMrtResult;
	mrt_pixel_input.input0_probe.mrt_target               = 0u;
	mrt_pixel_input.input0_probe.export_ordinal           = 1u;
	mrt_pixel_input.input0_probe.diagnostic_identity      = PixelMrtProbeDiagnosticIdentity(0u, 0u, 1u);
	mrt_pixel_input.input0_probe_descriptor_set           = 0u;
	Expect(ShaderPixelMrtProbeMatchesInstruction(mrt_pixel_code, mrt_pixel_input, mrt_pixel_input.input0_probe),
	       "pixel MRT probe accepts an active export with the selected target and absolute ordinal");
	auto wrong_mrt_target = mrt_pixel_input.input0_probe;
	wrong_mrt_target.mrt_target = 1u;
	Expect(!ShaderPixelMrtProbeMatchesInstruction(mrt_pixel_code, mrt_pixel_input, wrong_mrt_target),
	       "pixel MRT probe rejects an export for a different target");
	auto wrong_mrt_ordinal = mrt_pixel_input.input0_probe;
	wrong_mrt_ordinal.export_ordinal = 0u;
	Expect(!ShaderPixelMrtProbeMatchesInstruction(mrt_pixel_code, mrt_pixel_input, wrong_mrt_ordinal),
	       "pixel MRT probe rejects a non-export absolute ordinal");
	auto inactive_mrt_input = mrt_pixel_input;
	inactive_mrt_input.target_output_mode[0] = 0;
	Expect(!ShaderPixelMrtProbeMatchesInstruction(mrt_pixel_code, inactive_mrt_input, inactive_mrt_input.input0_probe),
	       "pixel MRT probe rejects a target whose shader color-output mode is inactive");
	auto masked_mrt_export = mrt_export;
	masked_mrt_export.exp_enable_mask = 0u;
	ShaderCode masked_mrt_code {};
	masked_mrt_code.SetType(ShaderType::Pixel);
	masked_mrt_code.GetInstructions().Add(nop);
	masked_mrt_code.GetInstructions().Add(masked_mrt_export);
	masked_mrt_code.GetInstructions().Add(end);
	Expect(!ShaderPixelMrtProbeMatchesInstruction(masked_mrt_code, mrt_pixel_input, mrt_pixel_input.input0_probe),
	       "pixel MRT probe rejects a zero-channel export");
	const auto mrt_pixel_source = SpirvGenerateSource(mrt_pixel_code, nullptr, &mrt_pixel_input, nullptr);
	const auto mrt_value = mrt_pixel_source.FindIndex("%t11_1 = OpCompositeConstruct %v4float");
	const auto mrt_observation = mrt_pixel_source.FindIndex("pixel_mrt_probe_count_prior_1 = OpAtomicIAdd");
	const auto mrt_coverage = mrt_pixel_source.FindIndex("pixel_mrt_probe_min_x_prior_1 = OpAtomicUMin");
	const auto mrt_store = mrt_pixel_source.FindIndex("OpStore %outColor %t11_1");
	Expect(mrt_value != Kyty::Core::STRING8_INVALID_INDEX &&
	           mrt_observation != Kyty::Core::STRING8_INVALID_INDEX && mrt_store != Kyty::Core::STRING8_INVALID_INDEX &&
	           mrt_value < mrt_observation && mrt_observation < mrt_store,
	       "pixel MRT probe observes the final assembled value immediately before the unchanged output store");
	Expect(mrt_pixel_source.FindIndex("OpDecorate %gl_FragCoord BuiltIn FragCoord") != Kyty::Core::STRING8_INVALID_INDEX &&
	           mrt_pixel_source.FindIndex("pixel_mrt_probe_frag_coord_1 = OpLoad %v4float %gl_FragCoord") !=
	               Kyty::Core::STRING8_INVALID_INDEX &&
	           mrt_coverage != Kyty::Core::STRING8_INVALID_INDEX && mrt_observation < mrt_coverage && mrt_coverage < mrt_store,
	       "pixel MRT probe records the rasterized fragment coverage before the unchanged output store");
	Expect(mrt_pixel_source.FindIndex("pixel_sample_probe") == Kyty::Core::STRING8_INVALID_INDEX &&
	           mrt_pixel_source.FindIndex("%fs_tap_") == Kyty::Core::STRING8_INVALID_INDEX &&
	           mrt_pixel_source.FindIndex("OpExecutionMode %main EarlyFragmentTests") != Kyty::Core::STRING8_INVALID_INDEX,
	       "pixel MRT probe is output-preserving and retains guest early fragment tests");
	ExpectValidSpirv(mrt_pixel_source, "pixel MRT selected source validates");
	const auto mrt_pixel_binary = ShaderRecompilePS(mrt_pixel_code, &mrt_pixel_input);
	Expect(!mrt_pixel_binary.IsEmpty(), "pixel MRT selected source recompiles");
	ExpectValidSpirv(mrt_pixel_binary, "pixel MRT selected binary validates");

	// Parsed s_nop; EXP target 8 (pixel Z), v0; s_endpgm. This is the
	// captured single-source depth form: done=1, compr=0, vm=1, en=0x1.
	const uint32_t pixel_depth_shader[] = {0xbf800000u, 0xf8001881u, 0x03020100u, 0xbf810000u};
	ShaderCode     pixel_depth_code {};
	pixel_depth_code.SetType(ShaderType::Pixel);
	ShaderParse(pixel_depth_shader, &pixel_depth_code);
	Expect(pixel_depth_code.GetInstructions().Size() == 3u,
	       "synthetic pixel-depth shader parses no-op, exact target-8 export, and endpgm");
	const auto& pixel_depth_export = pixel_depth_code.GetInstructions().At(1);
	Expect(pixel_depth_export.type == ShaderInstructionType::Exp &&
	           pixel_depth_export.format == ShaderInstructionFormat::PixelZVsrc0VmDone && pixel_depth_export.src_num == 1u &&
	           pixel_depth_export.exp_enable_mask == 0x01u && pixel_depth_export.src[0].type == ShaderOperandType::Vgpr &&
	           pixel_depth_export.src[0].register_id == 0,
	       "exact target-8 export decodes as a one-source pixel-depth IR instruction");
	const auto pixel_depth_debug = ShaderCode::DbgInstructionToStr(pixel_depth_export);
	Expect(pixel_depth_debug.FindIndex("PixelZVsrc0VmDone") != Kyty::Core::STRING8_INVALID_INDEX &&
	           pixel_depth_debug.FindIndex("pixel_z") != Kyty::Core::STRING8_INVALID_INDEX &&
	           pixel_depth_debug.FindIndex("????") == Kyty::Core::STRING8_INVALID_INDEX,
	       "pixel-depth export has a complete debug representation");
	ShaderPixelInputInfo pixel_depth_input {};
	const auto pixel_depth_source = SpirvGenerateSource(pixel_depth_code, nullptr, &pixel_depth_input, nullptr);
	const auto pixel_depth_exec_guard = pixel_depth_source.FindIndex("OpBranchConditional %exp_exec_b_1 %exp_store_1 %exp_kill_1");
	const auto pixel_depth_kill       = pixel_depth_source.FindIndex("%exp_kill_1 = OpLabel\n               OpKill");
	const auto pixel_depth_store      = pixel_depth_source.FindIndex("OpStore %fragDepth %t0_1");
	Expect(pixel_depth_source.FindIndex("OpDecorate %fragDepth BuiltIn FragDepth") != Kyty::Core::STRING8_INVALID_INDEX &&
	           pixel_depth_source.FindIndex("OpExecutionMode %main DepthReplacing") != Kyty::Core::STRING8_INVALID_INDEX &&
	           pixel_depth_source.FindIndex("%fragDepth = OpVariable %_ptr_Output_float Output") != Kyty::Core::STRING8_INVALID_INDEX &&
	           pixel_depth_source.FindIndex("OpStore %outColor") == Kyty::Core::STRING8_INVALID_INDEX &&
	           pixel_depth_exec_guard != Kyty::Core::STRING8_INVALID_INDEX && pixel_depth_kill != Kyty::Core::STRING8_INVALID_INDEX &&
	           pixel_depth_store != Kyty::Core::STRING8_INVALID_INDEX && pixel_depth_exec_guard < pixel_depth_kill &&
	           pixel_depth_kill < pixel_depth_store,
	       "exact target-8 export declares and conditionally stores FragDepth without a color export");
	ExpectValidSpirv(pixel_depth_source, "pixel-depth selected source validates");
	const auto pixel_depth_binary = ShaderRecompilePS(pixel_depth_code, &pixel_depth_input);
	Expect(!pixel_depth_binary.IsEmpty(), "pixel-depth selected source recompiles");
	ExpectValidSpirv(pixel_depth_binary, "pixel-depth selected binary validates");
	ShaderPixelInputInfo pixel_depth_early_input = pixel_depth_input;
	pixel_depth_early_input.ps_early_z            = true;
	const auto pixel_depth_early_source = SpirvGenerateSource(pixel_depth_code, nullptr, &pixel_depth_early_input, nullptr);
	Expect(pixel_depth_early_source.FindIndex("OpExecutionMode %main EarlyFragmentTests") == Kyty::Core::STRING8_INVALID_INDEX &&
	           pixel_depth_early_source.FindIndex("OpExecutionMode %main DepthReplacing") != Kyty::Core::STRING8_INVALID_INDEX,
	       "pixel-depth export suppresses early fragment tests without a conservative-depth proof while retaining DepthReplacing");
	ExpectValidSpirv(pixel_depth_early_source, "pixel-depth early-Z source validates");

	// An unconditional branch to s_endpgm is an early return that bypasses the
	// exact depth export. The depth contract must reject this unsupported CFG
	// rather than declare DepthReplacing for a path that has no FragDepth store.
	ShaderInstruction pixel_depth_bypass_branch {};
	pixel_depth_bypass_branch.pc              = 0;
	pixel_depth_bypass_branch.type            = ShaderInstructionType::SBranch;
	pixel_depth_bypass_branch.format          = ShaderInstructionFormat::Label;
	pixel_depth_bypass_branch.src[0]          = {.type = ShaderOperandType::LiteralConstant};
	pixel_depth_bypass_branch.src[0].constant = {.i = 4}; // pc 0 + 4 + 4 = pc 8 (s_endpgm)
	pixel_depth_bypass_branch.src_num         = 1;
	auto pixel_depth_bypass_export = pixel_depth_export;
	pixel_depth_bypass_export.pc   = 4;
	ShaderInstruction pixel_depth_bypass_end {};
	pixel_depth_bypass_end.pc     = 8;
	pixel_depth_bypass_end.type   = ShaderInstructionType::SEndpgm;
	pixel_depth_bypass_end.format = ShaderInstructionFormat::Empty;
	ShaderCode pixel_depth_bypass_code {};
	pixel_depth_bypass_code.SetType(ShaderType::Pixel);
	pixel_depth_bypass_code.GetInstructions().Add(pixel_depth_bypass_branch);
	pixel_depth_bypass_code.GetInstructions().Add(pixel_depth_bypass_export);
	pixel_depth_bypass_code.GetInstructions().Add(pixel_depth_bypass_end);
	pixel_depth_bypass_code.GetLabels().Add(ShaderLabel(pixel_depth_bypass_end.pc, pixel_depth_bypass_branch.pc));
	const auto pixel_depth_bypass_source = SpirvGenerateSource(pixel_depth_bypass_code, nullptr, &pixel_depth_input, nullptr);
	Expect(pixel_depth_bypass_source.FindIndex("OpKytyPixelDepthControlFlowRejected") != Kyty::Core::STRING8_INVALID_INDEX,
	       "pixel-depth export rejects an early-return branch that bypasses the FragDepth store");
	const auto pixel_depth_bypass_binary = ShaderRecompilePS(pixel_depth_bypass_code, &pixel_depth_input);
	Expect(pixel_depth_bypass_binary.IsEmpty(),
	       "pixel-depth early-return bypass remains rejected through pixel shader recompilation");

	ShaderInstruction guest_atomic {};
	guest_atomic.type              = ShaderInstructionType::BufferAtomicAdd;
	guest_atomic.format            = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
	guest_atomic.dst               = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	guest_atomic.src[0]            = {.type = ShaderOperandType::Vgpr, .register_id = 1, .size = 1};
	guest_atomic.src[1]            = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 4};
	guest_atomic.src[2].type       = ShaderOperandType::IntegerInlineConstant;
	guest_atomic.src[2].constant.u = 0;
	guest_atomic.src_num           = 3;
	guest_atomic.buffer_idxen      = true;
	ShaderCode side_effect_pixel_code {};
	side_effect_pixel_code.SetType(ShaderType::Pixel);
	side_effect_pixel_code.GetInstructions().Add(interp_x);
	side_effect_pixel_code.GetInstructions().Add(interp_y);
	side_effect_pixel_code.GetInstructions().Add(sample);
	side_effect_pixel_code.GetInstructions().Add(guest_atomic);
	side_effect_pixel_code.GetInstructions().Add(end);
	ShaderPixelInputInfo side_effect_pixel_input = ordinary_pixel_input;
	side_effect_pixel_input.input0_probe.enabled             = true;
	side_effect_pixel_input.input0_probe.draw_scoped         = true;
	side_effect_pixel_input.input0_probe.diagnostic_identity = set_identity_2;
	side_effect_pixel_input.input0_probe_descriptor_set      = 2u;
	side_effect_pixel_input.bind.storage_buffers.buffers_num          = 1;
	side_effect_pixel_input.bind.storage_buffers.start_register[0]    = 0;
	side_effect_pixel_input.bind.storage_buffers.usages[0]            = ShaderStorageUsage::ReadWrite;
	side_effect_pixel_input.bind.storage_buffers.accesses[0]          = ShaderStorageAccess::Raw;
	side_effect_pixel_input.bind.storage_buffers.slots[0]             = 0;
	side_effect_pixel_input.bind.storage_buffers.buffers[0].fields[1] = 16u << 16u;
	side_effect_pixel_input.bind.storage_buffers.buffers[0].fields[2] = 4u;
	ShaderCalcBindingIndices(&side_effect_pixel_input.bind);
	const auto side_effect_pixel_source = SpirvGenerateSource(side_effect_pixel_code, nullptr, &side_effect_pixel_input, nullptr);
	Expect(side_effect_pixel_source.FindIndex("OpExecutionMode %main EarlyFragmentTests") != Kyty::Core::STRING8_INVALID_INDEX,
	       "pixel probe preserves guest early tests when the selected shader has storage side effects");
	TestUnsetEnvironment("KYTY_PS_INPUT0_PROBE");
	TestUnsetEnvironment("KYTY_PS_INPUT0_PROBE_DRAW");
}

struct TestCommandBuffer
{
	using Callback = KYTY_SYSV_ABI bool (*)(Gen5::CommandBuffer*, uint32_t, void*);

	uint32_t* bottom      = nullptr;
	uint32_t* top         = nullptr;
	uint32_t* cursor_up   = nullptr;
	uint32_t* cursor_down = nullptr;
	Callback  callback    = nullptr;
	void*     user_data   = nullptr;
	uint32_t  reserved_dw = 0;
	uint32_t  pad         = 0;
};

static_assert(offsetof(TestCommandBuffer, callback) == 0x20);
static_assert(offsetof(TestCommandBuffer, user_data) == 0x28);
static_assert(offsetof(TestCommandBuffer, reserved_dw) == 0x30);

void VerifyTextureBlockDumpSpecContract()
{
	constexpr uint64_t address = 0x1234abcdull;
	Expect(TextureBlockDumpSpecMatches("1024x1024", 1024u, 1024u, address), "size-only block dump spec remains supported");
	Expect(TextureBlockDumpSpecMatches("1024x1024@1234abcd", 1024u, 1024u, address),
	       "bare hexadecimal block dump address matches");
	Expect(TextureBlockDumpSpecMatches("1024x1024@0x1234abcd", 1024u, 1024u, address),
	       "prefixed hexadecimal block dump address matches");
	Expect(!TextureBlockDumpSpecMatches(nullptr, 1024u, 1024u, address), "null block dump spec is rejected");
	Expect(!TextureBlockDumpSpecMatches("", 1024u, 1024u, address), "empty block dump spec is rejected");
	Expect(!TextureBlockDumpSpecMatches("512x1024@1234abcd", 1024u, 1024u, address), "block dump width mismatch is rejected");
	Expect(!TextureBlockDumpSpecMatches("1024x512@1234abcd", 1024u, 1024u, address), "block dump height mismatch is rejected");
	Expect(!TextureBlockDumpSpecMatches("1024x1024@1234abce", 1024u, 1024u, address), "block dump address mismatch is rejected");
	Expect(!TextureBlockDumpSpecMatches(" 1024x1024@1234abcd", 1024u, 1024u, address), "leading whitespace is rejected");
	Expect(!TextureBlockDumpSpecMatches("+1024x1024@1234abcd", 1024u, 1024u, address), "signed width is rejected");
	Expect(!TextureBlockDumpSpecMatches("1024x1024@+1234abcd", 1024u, 1024u, address), "signed address is rejected");
	Expect(!TextureBlockDumpSpecMatches("1024x1024@1234abcdjunk", 1024u, 1024u, address), "trailing address text is rejected");
	Expect(!TextureBlockDumpSpecMatches("4294967296x1024@1234abcd", 1024u, 1024u, address), "width overflow is rejected");
	Expect(!TextureBlockDumpSpecMatches("1024x4294967296@1234abcd", 1024u, 1024u, address), "height overflow is rejected");
	Expect(!TextureBlockDumpSpecMatches("1024x1024@10000000000000000", 1024u, 1024u, address), "address overflow is rejected");
}

void VerifyEventWritePacketContract()
{
	uint32_t storage[8] = {};
	TestCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 8;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 8;

	auto* addressed = Gen5::GraphicsDcbEventWrite(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0x39u,
	                                                reinterpret_cast<const volatile void*>(0x124e80f48ull));
	Expect(addressed == storage, "addressed EVENT_WRITE starts at the command cursor");
	Expect(cb.cursor_up == storage + 4, "addressed EVENT_WRITE consumes four dwords");
	Expect(addressed[0] == KYTY_PM4(4, Pm4::IT_EVENT_WRITE, 0u), "addressed EVENT_WRITE has a four-dword header");
	Expect(addressed[1] == 0x139u, "occlusion EVENT_WRITE selects event index one");
	Expect(addressed[2] == 0x24e80f48u && addressed[3] == 0x1u, "occlusion EVENT_WRITE preserves its aligned address");

	auto* ordinary = Gen5::GraphicsDcbEventWrite(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0x07u, nullptr);
	Expect(ordinary == storage + 4, "ordinary EVENT_WRITE follows the addressed packet");
	Expect(cb.cursor_up == storage + 6, "ordinary EVENT_WRITE remains two dwords");
	Expect(ordinary[0] == KYTY_PM4(2, Pm4::IT_EVENT_WRITE, 0u) && ordinary[1] == 0x07u,
	       "ordinary EVENT_WRITE preserves its short encoding");

	auto* unsupported_address = Gen5::GraphicsDcbEventWrite(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0x38u,
	                                                         reinterpret_cast<const volatile void*>(0x124e80f48ull));
	Expect(unsupported_address == storage + 6, "unsupported addressed event follows the ordinary packet");
	Expect(cb.cursor_up == storage + 8, "event 0x38 remains short without a matching consumer contract");
	Expect(unsupported_address[0] == KYTY_PM4(2, Pm4::IT_EVENT_WRITE, 0u) && unsupported_address[1] == 0x38u,
	       "event 0x38 does not invent an addressed packet form");
}

std::vector<uint32_t> AssembleValidSpirv(const Kyty::Core::String8& source, const char* message)
{
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
	std::vector<uint32_t> binary;
	tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* detail)
	{
		std::fprintf(stderr, "SPIR-V validation at %zu:%zu: %s\n", position.line, position.column, detail);
	});
	Expect(tools.Assemble(source.GetDataConst(), source.Size(), &binary), message);
	Expect(tools.Validate(binary), message);
	return binary;
}

void ExpectValidSpirv(const Kyty::Core::String8& source, const char* message)
{
	AssembleValidSpirv(source, message);
}

void ExpectValidSpirv(const Kyty::Vector<uint32_t>& binary, const char* message)
{
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
	tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* detail)
	{
		std::fprintf(stderr, "SPIR-V validation at %zu:%zu: %s\n", position.line, position.column, detail);
	});
	const std::vector<uint32_t> validation_binary(binary.GetDataConst(), binary.GetDataConst() + binary.Size());
	Expect(tools.Validate(validation_binary), message);
}

struct UnsignedMad64Result
{
	uint64_t value;
	uint32_t carry;
};

UnsignedMad64Result ReferenceUnsignedMad64(uint32_t multiplier_a, uint32_t multiplier_b, uint64_t addend)
{
	const uint64_t product = static_cast<uint64_t>(multiplier_a) * multiplier_b;
	const uint64_t value   = product + addend;
	return {value, value < product ? 1u : 0u};
}

EventRecord LastEvent()
{
	EventRecord event {};
	Expect(EventRing::Instance().CopySince(0, &event, 1) == 1, "expected one event");
	return event;
}

void VerifyStencilFrontier()
{
	EventRing::Instance().ResetForTests();
	Lifecycle::StencilFrontierContext context {};
	context.stencil_enable     = true;
	context.clear_enable       = false;
	context.htile              = true;
	context.depth_decompress   = false;
	context.stencil_decompress = true;
	context.resummarize        = false;
	context.copy_centroid      = true;
	context.copy_sample        = 3;
	context.read_only          = true;
	context.read_base_present  = false;
	context.write_base_present = false;
	Lifecycle::EmitStencilFrontier(context);

	const auto event = LastEvent();
	Expect(std::strcmp(event.code, Lifecycle::kCodeGraphicsStencilFrontier) == 0, "stencil event code");
	Expect(std::strstr(event.message, "test=1") != nullptr, "stencil enable serialized");
	Expect(std::strstr(event.message, "clear=0") != nullptr, "clear serialized");
	Expect(std::strstr(event.message, "htile=1 zdecomp=0 sdecomp=1") != nullptr, "decompression predicate serialized");
	Expect(std::strstr(event.message, "copyc=1 copys=3") != nullptr, "copy indicators serialized");
	Expect(std::strstr(event.message, "ro=1") != nullptr, "read-only serialized");
	Expect(std::strstr(event.message, "rb=0 wb=0") != nullptr, "base presence serialized");
	Expect(std::strlen(event.message) < kAgentEventMessageMax, "stencil message bounded");

	for (uint32_t i = 0; i < 100; ++i)
	{
		Lifecycle::EmitStencilFrontier(context);
	}
	Expect(EventRing::Instance().GetStats().total_pushed == 64, "stencil frontier volume bounded");
}

void VerifyDepthStencilAttachmentAccess(bool load_store_op_none_supported)
{
	RenderDepthInfo depth {};
	depth.format = VK_FORMAT_D32_SFLOAT;
	Expect(ResolveDepthStencilAttachmentAccess(depth, false, false) == DepthStencilAttachmentAccess::Writable,
	       "depth attachment without a sampled alias keeps the writable path");
	Expect(ResolveDepthStencilAttachmentAccess(depth, true, false) == DepthStencilAttachmentAccess::Unsupported,
	       "sampled depth alias requires store-op-none support");
	if (!load_store_op_none_supported)
	{
		return;
	}
	Expect(ResolveDepthStencilAttachmentAccess(depth, true, true) == DepthStencilAttachmentAccess::ReadOnly,
	       "non-writing depth attachment uses the read-only path");

	depth.depth_write_enable = true;
	Expect(ResolveDepthStencilAttachmentAccess(depth, false, true) == DepthStencilAttachmentAccess::Writable,
	       "depth write without a sampled alias keeps the writable path");
	Expect(ResolveDepthStencilAttachmentAccess(depth, true, true) == DepthStencilAttachmentAccess::Unsupported,
	       "sampled depth alias rejects simultaneous depth writes");
	depth.suppress_depth_write = true;
	Expect(ResolveDepthStencilAttachmentAccess(depth, true, true) == DepthStencilAttachmentAccess::ReadOnly,
	       "suppressed depth write stays read-only");

	depth.depth_clear_enable = true;
	Expect(ResolveDepthStencilAttachmentAccess(depth, true, true) == DepthStencilAttachmentAccess::Unsupported,
	       "sampled depth alias rejects simultaneous depth clear");
	depth.depth_clear_enable = false;

	depth.stencil_test_enable               = true;
	depth.stencil_dynamic_front.writeMask   = 0xffu;
	depth.stencil_static_front.passOp       = VK_STENCIL_OP_REPLACE;
	Expect(ResolveDepthStencilAttachmentAccess(depth, true, true) == DepthStencilAttachmentAccess::Unsupported,
	       "sampled depth alias rejects simultaneous stencil writes");
	depth.stencil_dynamic_front.writeMask = 0u;
	Expect(ResolveDepthStencilAttachmentAccess(depth, true, true) == DepthStencilAttachmentAccess::ReadOnly,
	       "masked stencil operation stays read-only");

	depth.stencil_clear_enable = true;
	Expect(ResolveDepthStencilAttachmentAccess(depth, true, true) == DepthStencilAttachmentAccess::Unsupported,
	       "sampled depth alias rejects simultaneous stencil clear");
}

void VerifyImageCopyNormalization()
{
	VulkanImage source(VulkanImageType::StorageTexture);
	VulkanImage destination(VulkanImageType::Texture);
	source.SetNativeExtent(256u, 128u);
	source.SetHostExtent(128u, 64u);
	source.physical_extent = {128u, 64u, 1u};
	source.mip_levels      = 4u;
	source.array_layers    = 4u;
	destination.SetNativeExtent(200u, 160u);
	destination.SetHostExtent(100u, 80u);
	destination.physical_extent = {100u, 80u, 1u};
	destination.mip_levels      = 3u;
	destination.array_layers    = 3u;

	ImageImageCopy requested {};
	requested.src_image       = &source;
	requested.src_level       = 1u;
	requested.dst_level       = 1u;
	requested.width           = 32u;
	requested.height          = 16u;
	requested.src_x           = 60;
	requested.src_y           = 24;
	requested.dst_x           = 45;
	requested.dst_y           = 30;
	requested.src_array_layer = 1u;
	requested.dst_array_layer = 0u;
	requested.layer_count     = 2u;
	ImageImageCopy normalized {};
	Expect(NormalizeImageImageCopy(requested, &destination, &normalized), "intersecting image copy remains valid");
	Expect(normalized.width == 4u && normalized.height == 8u, "image copy uses the real host mip intersection");
	Expect(normalized.src_x == requested.src_x && normalized.dst_x == requested.dst_x,
	       "image copy intersection preserves exact offsets");
	Expect(normalized.layer_count == 2u, "image copy preserves a valid layer range");

	requested.src_level = source.mip_levels;
	Expect(!NormalizeImageImageCopy(requested, &destination, &normalized), "source mip outside the image is rejected");
	requested.src_level       = 1u;
	requested.dst_array_layer = 2u;
	Expect(!NormalizeImageImageCopy(requested, &destination, &normalized), "destination layer overflow is rejected");
	requested.dst_array_layer = 0u;
	requested.src_x           = 64;
	Expect(!NormalizeImageImageCopy(requested, &destination, &normalized), "empty source intersection is rejected");

	VulkanImage atlas(VulkanImageType::StorageTexture);
	atlas.SetNativeExtent(256u, 128u);
	atlas.physical_extent = {256u, 192u, 1u};
	atlas.format          = VK_FORMAT_R8G8B8A8_UNORM;
	VulkanImage mip_destination(VulkanImageType::Texture);
	mip_destination.SetNativeExtent(256u, 128u);
	mip_destination.physical_extent = {256u, 128u, 1u};
	mip_destination.mip_levels      = 2u;
	mip_destination.format          = atlas.format;
	ImageImageCopy atlas_lod {};
	atlas_lod.src_image = &atlas;
	atlas_lod.src_level = 0u;
	atlas_lod.dst_level = 1u;
	atlas_lod.width     = 128u;
	atlas_lod.height    = 64u;
	atlas_lod.src_x     = 0;
	atlas_lod.src_y     = 128;
	atlas_lod.dst_x     = 0;
	atlas_lod.dst_y     = 0;
	Expect(NormalizeImageImageCopy(atlas_lod, &mip_destination, &normalized), "packed storage atlas LOD remains valid");
	Expect(normalized.width == atlas_lod.width && normalized.height == atlas_lod.height,
	       "packed storage atlas LOD is not cropped");

	VulkanImage block_source(VulkanImageType::StorageTexture);
	block_source.physical_extent = {29u, 30u, 1u};
	block_source.format          = VK_FORMAT_R32G32B32A32_UINT;
	VulkanImage block_destination(VulkanImageType::Texture);
	block_destination.physical_extent = {116u, 120u, 1u};
	block_destination.format          = VK_FORMAT_BC3_UNORM_BLOCK;
	ImageImageCopy block_copy {};
	block_copy.src_image = &block_source;
	block_copy.width     = 29u;
	block_copy.height    = 30u;
	block_copy.src_x     = 0;
	block_copy.src_y     = 0;
	block_copy.dst_x     = 0;
	block_copy.dst_y     = 0;
	Expect(NormalizeImageImageCopy(block_copy, &block_destination, &normalized),
	       "compatible uncompressed-to-block copy uses source coordinates");
	block_destination.physical_extent.width = 115u;
	Expect(!NormalizeImageImageCopy(block_copy, &block_destination, &normalized),
	       "block-adjusted destination overflow is rejected");
}

void VerifyBoundedShaderDecode()
{
	ShaderInit();
	static const uint32_t terminated[] = {0xbf810000u, 0xffffffffu};
	ShaderCode     code;
	code.SetType(ShaderType::Compute);
	Expect(ShaderTryParseBounded(terminated, sizeof(uint32_t), &code), "bounded shader accepts an in-range terminator");
	Expect(code.GetInstructions().Size() == 1u, "bounded shader ignores words outside the mapped code range");

	const uint32_t unterminated[] = {0xbf800000u};
	code.SetType(ShaderType::Compute);
	Expect(!ShaderTryParseBounded(unterminated, sizeof(unterminated), &code),
	       "bounded shader rejects a range without a reachable terminator");
	const uint32_t truncated_literals[] = {0x8000ffffu, 0xbf810000u};
	code.SetType(ShaderType::Compute);
	Expect(!ShaderTryParseBounded(truncated_literals, sizeof(truncated_literals), &code),
	       "bounded shader rejects an instruction whose literal words cross the range");
	Expect(!ShaderTryParseBounded(terminated, sizeof(uint32_t) - 1u, &code), "bounded shader rejects a partial dword range");

	static const uint32_t terminal_setpc[] = {0xbe802000u};
	code.SetType(ShaderType::Vertex);
	Expect(!ShaderTryParseBounded(terminal_setpc, sizeof(terminal_setpc), &code),
	       "ordinary bounded shader rejects an unregistered indirect terminator");

	constexpr uint32_t code_end = 0xbf9f0000u;
	const uint32_t padded_control_flow[] = {
	    0xbf850001u,
	    0xbf810000u,
	    0xbf82ffffu,
	    code_end,
	    code_end,
	    code_end,
	    code_end,
	    code_end,
	};
	code.SetType(ShaderType::Compute);
	Expect(ShaderTryParseBounded(padded_control_flow, sizeof(padded_control_flow), &code),
	       "bounded shader accepts an unreachable five-word code-end padding marker");
	Expect(code.GetInstructions().Size() == 3u && code.GetInstructions().At(2).type == ShaderInstructionType::SBranch,
	       "code-end padding remains outside the decoded instruction stream");
	const uint32_t terminated_padding[] = {0xbf810000u, code_end, code_end, code_end, code_end, code_end};
	code.SetType(ShaderType::Compute);
	Expect(ShaderTryParseBounded(terminated_padding, sizeof(terminated_padding), &code),
	       "bounded shader validates a five-word code-end tail after its terminator");
	const uint32_t short_terminated_padding[] = {0xbf810000u, code_end};
	code.SetType(ShaderType::Compute);
	Expect(ShaderTryParseBounded(short_terminated_padding, sizeof(short_terminated_padding), &code) &&
	           code.GetInstructions().Size() == 1u && code.GetInstructions().At(0).type == ShaderInstructionType::SEndpgm,
	       "bounded shader accepts a terminal endpgm independently of unreachable code-end padding");
	const uint32_t short_padding[] = {
	    0xbf850001u,
	    0xbf810000u,
	    0xbf82ffffu,
	    code_end,
	    code_end,
	    code_end,
	    code_end,
	};
	code.SetType(ShaderType::Compute);
	Expect(!ShaderTryParseBounded(short_padding, sizeof(short_padding), &code),
	       "bounded shader rejects an ambiguous code-end run shorter than five words");
	const uint32_t reachable_padding[] = {
	    0xbf820001u,
	    0xbf810000u,
	    code_end,
	    code_end,
	    code_end,
	    code_end,
	    code_end,
	};
	code.SetType(ShaderType::Compute);
	Expect(!ShaderTryParseBounded(reachable_padding, sizeof(reachable_padding), &code),
	       "bounded shader rejects a branch into code-end padding");
	const uint32_t later_reachable_padding[] = {
	    0xbf820006u,
	    0xbf810000u,
	    code_end,
	    code_end,
	    code_end,
	    code_end,
	    code_end,
	    code_end,
	};
	code.SetType(ShaderType::Compute);
	Expect(!ShaderTryParseBounded(later_reachable_padding, sizeof(later_reachable_padding), &code),
	       "bounded shader rejects a branch into a later word of code-end padding");
	const uint32_t fallthrough_padding[] = {
	    0xbf850001u,
	    0xbf810000u,
	    0xbf800000u,
	    code_end,
	    code_end,
	    code_end,
	    code_end,
	    code_end,
	};
	code.SetType(ShaderType::Compute);
	Expect(!ShaderTryParseBounded(fallthrough_padding, sizeof(fallthrough_padding), &code),
	       "bounded shader rejects code-end padding reachable by fallthrough");
	const uint32_t padding_only[] = {code_end, code_end, code_end, code_end, code_end};
	code.SetType(ShaderType::Compute);
	Expect(!ShaderTryParseBounded(padding_only, sizeof(padding_only), &code),
	       "bounded shader rejects code-end padding without a decoded program terminator");

	ShaderMappedData terminal_front_map {};
	terminal_front_map.code_size_bytes = sizeof(terminal_setpc);
	ShaderMappedData terminal_back_map {};
	terminal_back_map.code_size_bytes = sizeof(terminated);
	ShaderMapUserData(reinterpret_cast<uint64_t>(terminal_setpc), terminal_front_map);
	ShaderMapUserData(reinterpret_cast<uint64_t>(terminated), terminal_back_map);
	Expect(ShaderRegisterContinuation(reinterpret_cast<uint64_t>(terminal_setpc), reinterpret_cast<uint64_t>(terminated)),
	       "fused continuation registration requires bounded front and back mappings");
	ShaderParseFusedFront(terminal_setpc, sizeof(terminal_setpc), &code);
	Expect(code.GetInstructions().Size() == 1u && code.GetInstructions().At(0).type == ShaderInstructionType::SSetpcB64,
	       "registered fused front accepts an exact-range indirect terminator");
	static const uint32_t setpc_then_endpgm[] = {0xbe802000u, 0xbf810000u};
	ShaderMappedData branched_front_map {};
	branched_front_map.code_size_bytes = sizeof(setpc_then_endpgm);
	ShaderMapUserData(reinterpret_cast<uint64_t>(setpc_then_endpgm), branched_front_map);
	Expect(ShaderRegisterContinuation(reinterpret_cast<uint64_t>(setpc_then_endpgm), reinterpret_cast<uint64_t>(terminated)),
	       "mapped front with in-range control flow registers its continuation");
	ShaderParseFusedFront(setpc_then_endpgm, sizeof(setpc_then_endpgm), &code);
	Expect(code.GetInstructions().Size() == 2u && code.GetInstructions().At(1).type == ShaderInstructionType::SEndpgm,
	       "in-range code after setpc remains available to other control-flow paths");
	Expect(!ShaderHasTerminalSetpc(code), "non-terminal setpc does not authorize continuation linearization");
	ShaderMappedData replacement {};
	replacement.code_size_bytes = sizeof(terminal_setpc);
	ShaderMapUserData(reinterpret_cast<uint64_t>(terminal_setpc), replacement);
	Expect(ShaderLookupContinuation(reinterpret_cast<uint64_t>(terminal_setpc)) == 0u,
	       "new front mapping invalidates an address-reused continuation");
	ShaderMapUserData(reinterpret_cast<uint64_t>(terminated), terminal_back_map);
	Expect(ShaderLookupContinuation(reinterpret_cast<uint64_t>(setpc_then_endpgm)) == 0u,
	       "new back mapping invalidates continuations that target the reused address");
}

void VerifyScalarConditionalMoves()
{
	const uint32_t code_words[] = {
	    0xbe800000u | (4u << 16u) | (0x05u << 8u) | 2u,
	    0xbe800000u | (6u << 16u) | (0x06u << 8u) | 8u,
	    0xbe800000u | (126u << 16u) | (0x05u << 8u) | 10u,
	    0xbe800000u | (127u << 16u) | (0x05u << 8u) | 11u,
	    0xbe800000u | (12u << 16u) | (0x05u << 8u) | 253u,
	    0xbe800000u | (13u << 16u) | (0x05u << 8u) | 125u,
	    0xbe800000u | (125u << 16u) | (0x05u << 8u) | 14u,
	    0xbe800000u | (16u << 16u) | (0x06u << 8u) | 125u,
	    0xbe800000u | (125u << 16u) | (0x06u << 8u) | 18u,
	    0xbe800000u | (20u << 16u) | (0x06u << 8u) | 253u,
	    0xbe800000u | (22u << 16u) | (0x06u << 8u) | 252u,
	    0xbe800000u | (24u << 16u) | (0x06u << 8u) | 251u,
	    0xbf810000u,
	};
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	Expect(ShaderTryParseBounded(code_words, sizeof(code_words), &code), "scalar conditional moves decode in a bounded shader");
	Expect(code.GetInstructions().Size() == 13u, "scalar conditional move shader keeps its operations and terminator");
	const auto& move32 = code.GetInstructions().At(0);
	const auto& move64 = code.GetInstructions().At(1);
	const auto& exec_lo_move = code.GetInstructions().At(2);
	const auto& exec_hi_move = code.GetInstructions().At(3);
	Expect(move32.type == ShaderInstructionType::SCmovB32 && move32.src_num == 2 && move32.src[1] == move32.dst,
	       "32-bit conditional move preserves its destination when SCC is clear");
	Expect(move64.type == ShaderInstructionType::SCmovB64 && move64.src_num == 2 && move64.src[1] == move64.dst &&
	           move64.dst.size == 2 && move64.src[0].size == 2,
	       "64-bit conditional move preserves both destination dwords when SCC is clear");
	Expect(exec_lo_move.dst.type == ShaderOperandType::ExecLo && exec_hi_move.dst.type == ShaderOperandType::ExecHi,
	       "32-bit conditional moves retain individual EXEC destinations");
	Expect(code.GetInstructions().At(4).src[0].type == ShaderOperandType::Scc,
	       "32-bit conditional move accepts SCC as a scalar source");
	Expect(code.GetInstructions().At(5).src[0].type == ShaderOperandType::Null &&
	           code.GetInstructions().At(6).dst.type == ShaderOperandType::Null &&
	           code.GetInstructions().At(7).src[0].type == ShaderOperandType::Null &&
	           code.GetInstructions().At(8).dst.type == ShaderOperandType::Null,
	       "scalar conditional moves retain NULL source and destination operands");
	Expect(code.GetInstructions().At(9).src[0].type == ShaderOperandType::Scc &&
	           code.GetInstructions().At(10).src[0].type == ShaderOperandType::ExecZ &&
	           code.GetInstructions().At(11).src[0].type == ShaderOperandType::VccZ,
	       "64-bit conditional moves retain scalar status sources");
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 1;
	input.threads_num[1] = 1;
	input.threads_num[2] = 1;
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	Expect(source.FindIndex("OpLoad %uint %scc") != Kyty::Core::STRING8_INVALID_INDEX,
	       "scalar conditional moves read SCC during translation");
	Expect(source.FindIndex("OpSelect %uint") != Kyty::Core::STRING8_INVALID_INDEX,
	       "scalar conditional moves select between source and prior destination");
	Expect(source.FindIndex("OpStore %exec_lo") != Kyty::Core::STRING8_INVALID_INDEX &&
	           source.FindIndex("OpStore %exec_hi") != Kyty::Core::STRING8_INVALID_INDEX,
	       "32-bit conditional moves write both individual EXEC destinations");
	Expect(source.FindIndex("%z191_2 = OpLoad %uint %exec_lo") != Kyty::Core::STRING8_INVALID_INDEX &&
	           source.FindIndex("OpStore %execz %z196_2") != Kyty::Core::STRING8_INVALID_INDEX &&
	           source.FindIndex("%z191_3 = OpLoad %uint %exec_lo") != Kyty::Core::STRING8_INVALID_INDEX &&
	           source.FindIndex("OpStore %execz %z196_3") != Kyty::Core::STRING8_INVALID_INDEX,
	       "32-bit conditional moves refresh EXECZ after each individual EXEC write");
	Expect(source.FindIndex("%snz") == Kyty::Core::STRING8_INVALID_INDEX,
	       "scalar conditional moves preserve SCC instead of deriving it from their result");
	Expect(source.FindIndex("OpCopyObject %uint %uint_0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "NULL scalar sources translate to zero");
	ExpectValidSpirv(source, "scalar conditional moves emit valid SPIR-V for SCC, NULL, and EXEC operands");
}

void VerifyGuestReadVisitSerializesProtection()
{
	const uint64_t page_size = Kyty::Core::VirtualMemory::GetPageSize();
	Expect(page_size != 0u, "guest read visit requires a host page size");
	const uint64_t address = Kyty::Core::VirtualMemory::Alloc(0, page_size, Kyty::Core::VirtualMemory::Mode::ReadWrite);
	Expect(address != 0u, "guest read visit allocates a guest-owned page");
	const uint32_t marker = 0x5a17c0deu;
	Expect(Kyty::Core::VirtualMemory::CopyToGuest(address, &marker, sizeof(marker)), "guest read visit initializes the range");

	struct VisitSync
	{
		std::mutex              mutex;
		std::condition_variable changed;
		bool                    visitor_entered = false;
		bool                    release_visitor = false;
		bool                    protector_started = false;
		bool                    protector_finished = false;
		bool                    marker_valid = false;
	} sync;
	bool visit_result   = false;
	bool protect_result = false;
	std::thread visitor(
	    [&]
	    {
		    visit_result = Kyty::Core::VirtualMemory::VisitReadableGuestRange(
		        address, sizeof(marker),
		        [](const void* data, uint64_t size, void* opaque)
		        {
			        auto* state = static_cast<VisitSync*>(opaque);
			        std::unique_lock lock(state->mutex);
			        state->marker_valid   = size == sizeof(uint32_t) && *static_cast<const uint32_t*>(data) == 0x5a17c0deu;
			        state->visitor_entered = true;
			        state->changed.notify_all();
			        state->changed.wait(lock, [&] { return state->release_visitor; });
			        return state->marker_valid;
		        },
		        &sync);
	    });
	{
		std::unique_lock lock(sync.mutex);
		Expect(sync.changed.wait_for(lock, std::chrono::seconds(1), [&] { return sync.visitor_entered; }),
		       "guest read visitor enters before protection changes");
	}
	std::thread protector(
	    [&]
	    {
		    {
			    std::lock_guard lock(sync.mutex);
			    sync.protector_started = true;
			    sync.changed.notify_all();
		    }
		    protect_result = Kyty::Core::VirtualMemory::ProtectGuest(address, page_size, Kyty::Core::VirtualMemory::Mode::NoAccess);
		    {
			    std::lock_guard lock(sync.mutex);
			    sync.protector_finished = true;
			    sync.changed.notify_all();
		    }
	    });
	{
		std::unique_lock lock(sync.mutex);
		Expect(sync.changed.wait_for(lock, std::chrono::seconds(1), [&] { return sync.protector_started; }),
		       "guest protection attempt starts while visitor is active");
		Expect(!sync.changed.wait_for(lock, std::chrono::milliseconds(25), [&] { return sync.protector_finished; }),
		       "guest protection waits for the active read visitor");
		sync.release_visitor = true;
		sync.changed.notify_all();
	}
	visitor.join();
	protector.join();
	Expect(visit_result && protect_result, "guest read visit and deferred protection both complete");
	Expect(Kyty::Core::VirtualMemory::ProtectGuest(address, page_size, Kyty::Core::VirtualMemory::Mode::ReadWrite),
	       "guest read visit restores page access for cleanup");
	Expect(Kyty::Core::VirtualMemory::Free(address), "guest read visit releases its guest page");
}

void VerifyFusedShaderUsesEffectiveBackEntry()
{
	alignas(256) static uint32_t front_code[64] = {};
	alignas(256) static uint32_t back_code[128] = {};
	const uint64_t               back_entry     = reinterpret_cast<uint64_t>(back_code) + 256u;
	ShaderRegister               back_registers[2] {};
	back_registers[0].offset = Pm4::SPI_SHADER_PGM_LO_GS;
	back_registers[0].value  = static_cast<uint32_t>((back_entry >> 8u) & 0xffffffffu);
	back_registers[1].offset = Pm4::SPI_SHADER_PGM_LO_GS + 1u;
	back_registers[1].value  = static_cast<uint32_t>((back_entry >> 40u) & 0xffu);

	Shader front {};
	front.type = 4u;
	front.code = front_code;
	Shader back {};
	back.type             = 6u;
	back.code             = back_code;
	back.sh_registers     = back_registers;
	back.num_sh_registers = 2u;
	Shader         fused {};
	ShaderRegister scratch[2] {};
	ShaderMappedData front_map {};
	front_map.code_size_bytes = sizeof(front_code);
	ShaderMappedData back_map {};
	back_map.code_size_bytes = sizeof(back_code);
	ShaderMapUserData(reinterpret_cast<uint64_t>(front_code), front_map);
	ShaderMapUserData(reinterpret_cast<uint64_t>(back_code), back_map);
	Expect(Gen5::GraphicsUnknownFuseShaderHalves(&fused, &front, &back, scratch) == 0,
	       "valid fused shader halves resolve their back program address");
	Expect(ShaderLookupContinuation(reinterpret_cast<uint64_t>(front_code)) == back_entry,
	       "fused shader continuation uses the effective back entry rather than the allocation base");
	ShaderMappedData smaller_back_map {};
	smaller_back_map.code_size_bytes = 128u;
	ShaderMapUserData(reinterpret_cast<uint64_t>(back_code), smaller_back_map);
	Expect(ShaderLookupContinuation(reinterpret_cast<uint64_t>(front_code)) == 0u,
	       "reusing a back allocation invalidates continuations into its previous range");
	Expect(!ShaderRegisterContinuation(reinterpret_cast<uint64_t>(front_code), back_entry),
	       "continuation registration rejects an entry outside the replacement owner range");
	alignas(256) static uint32_t unmapped_back_code[64] = {};
	const uint64_t               unmapped_back_entry    = reinterpret_cast<uint64_t>(unmapped_back_code);
	ShaderRegister               unmapped_registers[2] {};
	unmapped_registers[0].offset = Pm4::SPI_SHADER_PGM_LO_GS;
	unmapped_registers[0].value  = static_cast<uint32_t>((unmapped_back_entry >> 8u) & 0xffffffffu);
	unmapped_registers[1].offset = Pm4::SPI_SHADER_PGM_LO_GS + 1u;
	unmapped_registers[1].value  = static_cast<uint32_t>((unmapped_back_entry >> 40u) & 0xffu);
	Shader unmapped_back          = back;
	unmapped_back.code            = unmapped_back_code;
	unmapped_back.sh_registers    = unmapped_registers;
	Expect(Gen5::GraphicsUnknownFuseShaderHalves(&fused, &front, &unmapped_back, scratch) != 0,
	       "fused shader rejects an unmapped back entry");
}

void VerifyStorageFrontier()
{
	EventRing::Instance().ResetForTests();
	Lifecycle::StorageFrontierContext context {};
	context.binding.access          = Lifecycle::StorageAccessClass::Mixed;
	context.binding.source          = Lifecycle::StorageBindingSource::Metadata;
	context.binding.unknown_reason  = Lifecycle::StorageUnknownReason::RegisterBaseMismatch;
	context.binding.code_available  = true;
	context.binding.exact_match     = false;
	context.unbased_match   = true;
	context.decoded_unknown = false;
	context.binding.indirect_use    = true;
	context.binding.raw_vmem_oob_guarded = true;
	context.binding.raw_smem_use         = false;
	context.binding.raw_tbuffer_use      = false;
	context.resource_index  = 15;
	context.sgpr            = 31;
	context.slot            = 65535;
	context.usage           = 3;
	context.stride          = 16383;
	context.format          = 127;
	context.dst_sel         = 0xfff;
	context.add_tid         = true;
	context.swizzle         = true;
	Lifecycle::EmitStorageFrontier(context);

	const auto event = LastEvent();
	Expect(std::strcmp(event.code, Lifecycle::kCodeGraphicsStorageFrontier) == 0, "storage event code");
	Expect(std::strstr(event.message, "a=mixed") != nullptr, "access class serialized");
	Expect(std::strstr(event.message, "src=metadata r=register_base_mismatch") != nullptr,
	       "binding source and unknown reason serialized");
	Expect(std::strstr(event.message, "c=1 x=0 ub=1") != nullptr, "instruction availability and register matches serialized");
	Expect(std::strstr(event.message, "d=0 i=1 vm=1 sm=0 tb=0") != nullptr,
	       "decode completeness and raw-consumer evidence serialized");
	Expect(std::strstr(event.message, "n=15 sg=31 sl=65535") != nullptr, "resource identity serialized");
	Expect(std::strstr(event.message, "u=3 st=16383 f=127") != nullptr, "descriptor geometry serialized");
	Expect(std::strstr(event.message, "ds=fff tid=1 sw=1") != nullptr, "descriptor controls serialized");
	Expect(std::strlen(event.message) < kAgentEventMessageMax, "storage message bounded");
}

void VerifyStorageRange()
{
	EventRing::Instance().ResetForTests();
	Lifecycle::StorageRangeContext context {};
	context.binding.access          = Lifecycle::StorageAccessClass::Raw;
	context.binding.source          = Lifecycle::StorageBindingSource::Direct;
	context.binding.unknown_reason  = Lifecycle::StorageUnknownReason::None;
	context.binding.code_available  = true;
	context.binding.exact_match     = true;
	context.binding.indirect_use    = false;
	context.binding.raw_vmem_oob_guarded = true;
	context.binding.raw_smem_use         = false;
	context.binding.raw_tbuffer_use      = false;
	context.backing                 = Lifecycle::StorageRangeBacking::Flexible;
	context.backing_size            = 4096;
	context.resource_index    = 7;
	context.sgpr              = 36;
	context.slot              = 7;
	context.usage             = 1;
	context.stride            = 255;
	context.base              = 0x0000f00000006000ull;
	context.declared_size     = 0x649b00000ull;
	context.materialized_size = 0x06500000ull;
	context.descriptor_words[0] = 0x11223344u;
	context.descriptor_words[1] = 0x55667788u;
	context.descriptor_words[2] = 0x99aabbccu;
	context.descriptor_words[3] = 0xddeeff00u;
	Lifecycle::EmitStorageRange(context);

	const auto event = LastEvent();
	Expect(std::strcmp(event.code, Lifecycle::kCodeGraphicsStorageRange) == 0, "storage range event code");
	Expect(std::strstr(event.message, "a=raw src=direct r=none c=1 x=1 i=0 vm=1 sm=0 tb=0 k=flexible ks=4096") != nullptr,
	       "storage provenance serialized");
	Expect(std::strstr(event.message, "idx=7 sgpr=36 slot=7 u=1 st=255") != nullptr, "storage range identity serialized");
	Expect(std::strstr(event.message, "b=0xf00000006000 d=27006074880 m=105906176") != nullptr,
	       "storage range sizes serialized");
	Expect(std::strstr(event.message, "w=11223344,55667788,99aabbcc,ddeeff00") != nullptr,
	       "storage descriptor words serialized");
	Expect(std::strlen(event.message) < kAgentEventMessageMax, "storage range message bounded");
}

void VerifyStorageUnknownReasonResolution()
{
	auto evidence = ResolveShaderStorageAccessEvidence(false, ShaderStorageBindingSource::MetadataSharp, ShaderStorageAccess::Unknown,
	                                                   ShaderStorageAccess::Unknown, false, false);
	Expect(evidence.reason == ShaderStorageUnknownReason::CodeUnavailable, "missing shader code reason");

	evidence = ResolveShaderStorageAccessEvidence(true, ShaderStorageBindingSource::DirectResource, ShaderStorageAccess::Unknown,
	                                              ShaderStorageAccess::Raw, false, false);
	Expect(evidence.reason == ShaderStorageUnknownReason::RegisterBaseMismatch, "register base mismatch reason");

	evidence = ResolveShaderStorageAccessEvidence(true, ShaderStorageBindingSource::MetadataSharp, ShaderStorageAccess::Unknown,
	                                              ShaderStorageAccess::Unknown, false, false);
	Expect(evidence.access == ShaderStorageAccess::UnusedMetadata, "proven unused metadata classification");
	Expect(evidence.reason == ShaderStorageUnknownReason::None, "unused metadata is not unknown");

	evidence = ResolveShaderStorageAccessEvidence(true, ShaderStorageBindingSource::MetadataSharp, ShaderStorageAccess::Unknown,
	                                              ShaderStorageAccess::Unknown, true, false);
	Expect(evidence.access == ShaderStorageAccess::Unknown, "unknown decoded instruction remains strict");
	Expect(evidence.reason == ShaderStorageUnknownReason::MetadataOnlyBinding, "unknown metadata reason preserved");

	evidence = ResolveShaderStorageAccessEvidence(true, ShaderStorageBindingSource::MetadataSharp, ShaderStorageAccess::Unknown,
	                                              ShaderStorageAccess::Unknown, false, true);
	Expect(evidence.access == ShaderStorageAccess::Unknown, "indirect descriptor use remains strict");

	evidence = ResolveShaderStorageAccessEvidence(true, ShaderStorageBindingSource::DirectResource, ShaderStorageAccess::Unknown,
	                                              ShaderStorageAccess::Unknown, false, false);
	Expect(evidence.access == ShaderStorageAccess::Unknown, "unmatched direct resource remains strict");
	Expect(evidence.reason == ShaderStorageUnknownReason::NoMatchingInstruction, "unmatched direct resource preserves its reason");
}

void VerifyRenderColorArrayBackingGrouping()
{
	RenderColorInfo color {};
	color.targets_num = 2;
	for (auto& attachment: color.attachment)
	{
		attachment.type                  = RenderColorType::RenderTexture;
		attachment.base_addr             = 0x1000u;
		attachment.render_texture_format = RenderTextureFormat::R8G8B8A8Unorm;
		attachment.width                 = 128u;
		attachment.height                = 64u;
		attachment.pitch                 = 128u;
		attachment.tile                  = true;
	}
	color.attachment[0].image_layers    = 1u;
	color.attachment[0].base_array_layer = 0u;
	color.attachment[0].size            = 0x20000u;
	color.attachment[1].image_layers    = 2u;
	color.attachment[1].base_array_layer = 1u;
	color.attachment[1].size            = 0x40000u;

	NormalizeRenderColorArrayBackings(&color);
	Expect(color.attachment[0].image_layers == 2u && color.attachment[1].image_layers == 2u,
	       "MRT slices share the largest array backing");
	Expect(color.attachment[0].size == 0x40000u && color.attachment[1].size == 0x40000u,
	       "MRT slices share the full backing size");
	Expect(color.attachment[0].base_array_layer == 0u && color.attachment[1].base_array_layer == 1u,
	       "MRT slice views remain distinct");

	RenderTextureVulkanImage render_array;
	render_array.usage                               = VK_IMAGE_USAGE_STORAGE_BIT;
	render_array.image_view[VulkanImage::VIEW_ARRAY] = reinterpret_cast<VkImageView>(0x1);
	int storage_view = -1;
	Expect(VulkanResolveStorageImageView(&render_array, false, true, &storage_view),
	       "render-target arrays expose a storage-compatible view");
	Expect(storage_view == VulkanImage::VIEW_ARRAY, "render-target storage arrays use the canonical array view");
}

ShaderOperand Sgpr(int register_id, int size)
{
	ShaderOperand operand {};
	operand.type        = ShaderOperandType::Sgpr;
	operand.register_id = register_id;
	operand.size        = size;
	return operand;
}

void VerifyStorageConsumerAnalysis()
{
	ShaderCode        code;
	ShaderInstruction end {};
	end.type = ShaderInstructionType::SEndpgm;
	code.GetInstructions().Add(end);

	auto evidence = AnalyzeShaderStorageUse(code, 32);
	Expect(!evidence.decoded_unknown, "fully decoded shader has no unknown instruction");
	Expect(!evidence.indirect_descriptor_use, "unread descriptor range is not indirectly consumed");
	Expect(evidence.access == ShaderStorageAccess::Unknown, "unread descriptor has no direct access class");

	ShaderInstruction indirect {};
	indirect.type    = ShaderInstructionType::SMovB32;
	indirect.src_num = 1;
	indirect.src[0]  = Sgpr(35, 1);
	code.GetInstructions().Add(indirect);
	evidence = AnalyzeShaderStorageUse(code, 32);
	Expect(evidence.indirect_descriptor_use, "overlapping SGPR read blocks unused classification");

	ShaderCode unknown_code;
	unknown_code.GetInstructions().Add(ShaderInstruction {});
	evidence = AnalyzeShaderStorageUse(unknown_code, 32);
	Expect(evidence.decoded_unknown, "unknown decoded instruction blocks unused classification");
}

void VerifyUnusedMetadataExclusionPreservesActiveOrdering()
{
	ShaderStorageResources resources {};
	resources.buffers_num       = 3;
	resources.slots[0]          = 2;
	resources.slots[1]          = 7;
	resources.slots[2]          = 9;
	resources.start_register[0] = 16;
	resources.start_register[1] = 32;
	resources.start_register[2] = 48;
	resources.accesses[0]       = ShaderStorageAccess::Raw;
	resources.accesses[1]       = ShaderStorageAccess::UnusedMetadata;
	resources.accesses[2]       = ShaderStorageAccess::Typed;

	ExcludeUnusedMetadataStorage(&resources);

	Expect(resources.buffers_num == 2, "unused metadata excluded before binding");
	Expect(resources.slots[0] == 2 && resources.slots[1] == 9, "active slot ordering preserved");
	Expect(resources.start_register[0] == 16 && resources.start_register[1] == 48, "active resource identity preserved");
	Expect(resources.accesses[0] == ShaderStorageAccess::Raw && resources.accesses[1] == ShaderStorageAccess::Typed,
	       "active access classifications preserved");
}

void VerifyResidualStencilPm4Boundary()
{
	HW::Context context;
	State::ApplyDepthStencilPlaneRegisters(context, 1u << Pm4::DB_STENCIL_INFO_FORMAT_SHIFT, 0, 0);
	State::SetRenderControl(context, 0);
	State::SetDepthControl(context, 0);

	Expect(State::ValidateStencilPlane(context.GetDepthRenderTarget(), context.GetRenderControl(), context.GetDepthControl()) ==
	           State::StencilPlaneValidation::Inactive,
	       "inactive residual stencil must not be consumed");
}

void VerifyActiveStencilPm4BoundaryRejectsMissingBases()
{
	constexpr uint32_t kStencilFormat = 1u << Pm4::DB_STENCIL_INFO_FORMAT_SHIFT;
	constexpr uint32_t kStencilEnable = 1u << Pm4::DB_DEPTH_CONTROL_STENCIL_ENABLE_SHIFT;
	constexpr uint32_t kStencilClear  = 1u << Pm4::DB_RENDER_CONTROL_STENCIL_CLEAR_ENABLE_SHIFT;

	HW::Context context;
	State::ApplyDepthStencilPlaneRegisters(context, kStencilFormat, 0, 0);
	State::SetRenderControl(context, 0);
	State::SetDepthControl(context, kStencilEnable);
	Expect(State::ValidateStencilPlane(context.GetDepthRenderTarget(), context.GetRenderControl(), context.GetDepthControl()) ==
	           State::StencilPlaneValidation::MissingReadBase,
	       "active stencil test must reject missing read base");

	State::SetDepthControl(context, 0);
	State::SetRenderControl(context, kStencilClear);
	Expect(State::ValidateStencilPlane(context.GetDepthRenderTarget(), context.GetRenderControl(), context.GetDepthControl()) ==
	           State::StencilPlaneValidation::MissingWriteBase,
	       "active stencil clear must reject missing write base");

	const uint32_t active_render_controls[] = {
	    1u << Pm4::DB_RENDER_CONTROL_RESUMMARIZE_ENABLE_SHIFT,
	    1u << Pm4::DB_RENDER_CONTROL_COPY_CENTROID_SHIFT,
	    1u << Pm4::DB_RENDER_CONTROL_COPY_SAMPLE_SHIFT,
	};
	for (const auto control: active_render_controls)
	{
		State::SetRenderControl(context, control);
		Expect(State::ValidateStencilPlane(context.GetDepthRenderTarget(), context.GetRenderControl(), context.GetDepthControl()) !=
		           State::StencilPlaneValidation::Inactive,
		       "active stencil copy/resummarize operation must reject missing bases");
	}

	auto target                       = context.GetDepthRenderTarget();
	target.z_info.tile_surface_enable = true;
	context.SetDepthRenderTarget(target);
	State::SetRenderControl(context, 1u << Pm4::DB_RENDER_CONTROL_STENCIL_COMPRESS_DISABLE_SHIFT);
	Expect(State::ValidateStencilPlane(context.GetDepthRenderTarget(), context.GetRenderControl(), context.GetDepthControl()) !=
	           State::StencilPlaneValidation::Inactive,
	       "active stencil decompression must reject missing bases");
}

void VerifyRawGen5StorageDescriptorContract()
{
	ShaderBufferResource resource {};
	resource.fields[1] = 128u << 16u;
	resource.fields[2] = 2;
	resource.fields[3] = (5u << 12u) | DstSel(4, 0, 0, 1);

	Expect(ShaderGen5StorageDescriptorSupported(resource, ShaderStorageAccess::Raw),
	       "raw dword access accepts byte-addressed 128-byte records");
	Expect(!ShaderGen5StorageDescriptorSupported(resource, ShaderStorageAccess::Typed), "typed format 5 remains strict");
	Expect(!ShaderGen5StorageDescriptorSupported(resource, ShaderStorageAccess::Mixed), "mixed access remains strict");
	Expect(!ShaderGen5StorageDescriptorSupported(resource, ShaderStorageAccess::Unknown), "unknown access remains strict");

	resource.fields[1] = 126u << 16u;
	Expect(!ShaderGen5StorageDescriptorSupported(resource, ShaderStorageAccess::Raw),
	       "raw access rejects a byte-addressed stride that is not dword-aligned");
}

ShaderCode ParseUnsignedExecLessThan(bool vop3)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	if (vop3)
	{
		const uint32_t words[] = {
		    (0x35u << 26u) | (0xd1u << 16u) | 106u,
		    256u | (257u << 9u),
		    (0x35u << 26u) | (0xd1u << 16u) | 106u,
		    256u | (257u << 9u),
		    0xbf810000u,
		};
		ShaderParse(words, &code);
	} else
	{
		const uint32_t words[] = {
		    (0x3eu << 25u) | (0xd1u << 17u) | (1u << 9u) | 256u,
		    (0x3eu << 25u) | (0xd1u << 17u) | (1u << 9u) | 256u,
		    0xbf810000u,
		};
		ShaderParse(words, &code);
	}
	return code;
}

void VerifyUnsignedExecLessThanComparison()
{
	for (const bool vop3: {false, true})
	{
		auto code = ParseUnsignedExecLessThan(vop3);
		Expect(code.GetInstructions().Size() == 3, "comparison encoding parsed with endpgm");
		const auto& compare = code.GetInstructions().At(0);
		Expect(compare.type == ShaderInstructionType::VCmpxLtU32, "unsigned exec comparison decoded");
		Expect(compare.format == ShaderInstructionFormat::SmaskVsrc0Vsrc1, "comparison uses shared mask format");
		Expect(compare.dst.type == ShaderOperandType::VccLo, "comparison updates VCC/EXEC mask");
	}

	auto source = SpirvGenerateSource(ParseUnsignedExecLessThan(false), nullptr, nullptr, nullptr);
	Expect(std::strstr(source.c_str(), "OpULessThan") != nullptr, "unsigned less-than emits unsigned SPIR-V compare");
	Expect(std::strstr(source.c_str(), "OpStore %exec_lo") != nullptr, "exec comparison updates exec low mask");
	Expect(std::strstr(source.c_str(), "OpStore %exec_hi %uint_0") != nullptr, "exec comparison clears exec high mask");
}

ShaderCode ParseFloatExecNotLessEqual(bool vop3)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	if (vop3)
	{
		const uint32_t words[] = {
		    (0x35u << 26u) | (0x1cu << 16u) | 106u,
		    256u | (257u << 9u),
		    (0x35u << 26u) | (0x1cu << 16u) | 106u,
		    256u | (257u << 9u),
		    0xbf810000u,
		};
		ShaderParse(words, &code);
	} else
	{
		const uint32_t words[] = {
		    (0x3eu << 25u) | (0x1cu << 17u) | (1u << 9u) | 256u,
		    (0x3eu << 25u) | (0x1cu << 17u) | (1u << 9u) | 256u,
		    0xbf810000u,
		};
		ShaderParse(words, &code);
	}
	return code;
}

void VerifyFloatExecNotLessEqualComparison()
{
	for (const bool vop3: {false, true})
	{
		auto code = ParseFloatExecNotLessEqual(vop3);
		Expect(code.GetInstructions().Size() == 3, "float comparison encoding parsed with endpgm");
		const auto& compare = code.GetInstructions().At(0);
		Expect(compare.type == ShaderInstructionType::VCmpxNleF32, "float exec comparison decoded");
		Expect(compare.format == ShaderInstructionFormat::SmaskVsrc0Vsrc1, "float comparison uses shared mask format");
		Expect(compare.dst.type == ShaderOperandType::VccLo, "float comparison updates VCC/EXEC mask");
	}

	auto source = SpirvGenerateSource(ParseFloatExecNotLessEqual(false), nullptr, nullptr, nullptr);
	Expect(std::strstr(source.c_str(), "OpFUnordGreaterThan") != nullptr,
	       "not-less-equal uses unordered-greater-than for IEEE NaN semantics");
	Expect(std::strstr(source.c_str(), "OpStore %exec_lo") != nullptr, "float exec comparison updates exec low mask");
	Expect(std::strstr(source.c_str(), "OpStore %exec_hi %uint_0") != nullptr, "float exec comparison clears exec high mask");
}

void VerifyGen5FloatExecNotLessThanSdwa()
{
	// Captured Gen5 SDWA VOPC at normalized PC 0x1194:
	// v_cmpx_nlt_f32 v100, 0 with DWORD selectors and neutral modifiers.
	// AMD RDNA2 ISA, Table 23:
	// https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture
	const uint32_t shader[] = {0x7c3d00f9u, 0x86060064u, 0x7c3d00f9u, 0x86060064u, 0xbf810000u};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	Expect(code.GetInstructions().Size() == 3, "captured Gen5 SDWA comparison parses with endpgm");
	const auto& compare = code.GetInstructions().At(0);
	Expect(compare.type == ShaderInstructionType::VCmpxNltF32, "Gen5 VOPC opcode 0x1e decodes as v_cmpx_nlt_f32");
	Expect(compare.format == ShaderInstructionFormat::SmaskVsrc0Vsrc1, "Gen5 SDWA comparison uses the shared mask format");
	Expect(compare.dst.type == ShaderOperandType::VccLo && compare.dst.size == 2, "Gen5 SDWA comparison targets the VCC/EXEC mask");
	Expect(compare.src_num == 2, "Gen5 SDWA comparison consumes two sources");
	Expect(compare.src[0].type == ShaderOperandType::Vgpr && compare.src[0].register_id == 100,
	       "Gen5 SDWA comparison decodes the vector source");
	Expect(compare.src[1].type == ShaderOperandType::IntegerInlineConstant && compare.src[1].constant.i == 0,
	       "Gen5 SDWA comparison decodes the inline zero source");
	Expect(compare.src[0].swizzle == 6 && compare.src[1].swizzle == 6, "captured comparison uses DWORD selectors");
	Expect(!compare.src[0].negate && !compare.src[0].absolute && !compare.src[1].negate && !compare.src[1].absolute,
	       "captured comparison has neutral source modifiers");

	const uint32_t vop3_shader[] = {
	    (0x35u << 26u) | (0x1eu << 16u) | 106u,
	    256u | (257u << 9u),
	    (0x35u << 26u) | (0x1eu << 16u) | 106u,
	    256u | (257u << 9u),
	    0xbf810000u,
	};
	ShaderCode vop3_code;
	vop3_code.SetType(ShaderType::Compute);
	ShaderParse(vop3_shader, &vop3_code);
	Expect(vop3_code.GetInstructions().At(0).type == ShaderInstructionType::VCmpxNltF32,
	       "VOP3 opcode 0x1e shares the v_cmpx_nlt_f32 semantic decoder");
	Expect(vop3_code.GetInstructions().At(0).format == ShaderInstructionFormat::SmaskVsrc0Vsrc1,
	       "VOP3 v_cmpx_nlt_f32 uses the shared mask format");

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, nullptr);
	Expect(source.FindIndex("OpFUnordGreaterThanEqual") != Kyty::Core::STRING8_INVALID_INDEX,
	       "not-less-than preserves unordered-or-greater-equal NaN semantics");
	Expect(source.FindIndex("%texec_0 = OpLoad %uint %exec_lo") != Kyty::Core::STRING8_INVALID_INDEX,
	       "CMPX reads the prior execution mask");
	Expect(source.FindIndex("%tmasked_0 = OpBitwiseAnd %uint %t3_0 %texec_0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "CMPX cannot reactivate an inactive lane");
	Expect(source.FindIndex("OpStore %exec_lo") != Kyty::Core::STRING8_INVALID_INDEX,
	       "Gen5 CMPX updates the low execution mask");
	Expect(source.FindIndex("OpStore %exec_hi %uint_0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "Gen5 CMPX clears the unused high execution mask");
}

ShaderCode ParseUnsignedByteBufferLoad()
{
	const uint32_t word0    = (0x38u << 26u) | (0x08u << 18u) | (1u << 13u) | 3u;
	const uint32_t word1    = (128u << 24u) | (2u << 16u) | (30u << 8u) | 43u;
	const uint32_t shader[] = {word0, word1, 0xbf800000u, 0xbf810000u};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);
	return code;
}

void VerifyUnsignedByteBufferLoad()
{
	auto code = ParseUnsignedByteBufferLoad();
	Expect(code.GetInstructions().Size() == 3, "byte buffer load encoding parsed with endpgm");
	const auto& load = code.GetInstructions().At(0);
	Expect(load.type == ShaderInstructionType::BufferLoadUbyte, "unsigned byte buffer load decoded");
	Expect(load.format == ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen, "byte buffer load uses the direct MUBUF address format");
	Expect(load.dst.type == ShaderOperandType::Vgpr && load.dst.register_id == 30, "byte buffer load destination decoded");
	Expect(load.src[0].type == ShaderOperandType::Vgpr && load.src[0].register_id == 43, "byte buffer load address decoded");
	Expect(load.src[1].type == ShaderOperandType::Sgpr && load.src[1].register_id == 8 && load.src[1].size == 4,
	       "byte buffer load descriptor decoded");
	Expect(load.buffer_imm_offset == 3u, "byte buffer load immediate offset preserved separately");
	Expect(load.src[2].type == ShaderOperandType::IntegerInlineConstant && load.src[2].constant.i == 0,
	       "byte buffer load keeps the scalar offset outside descriptor addressing");

	Expect(ShaderGetDirectStorageUsage(code, 8) == ShaderStorageUsage::ReadOnly,
	       "unsigned byte load remains an active read-only storage use");
	auto usage = AnalyzeShaderStorageUse(code, 8);
	Expect(usage.access == ShaderStorageAccess::Raw && !usage.decoded_unknown && !usage.indirect_descriptor_use,
	       "unsigned byte load remains a known raw storage consumer");

	ShaderComputeInputInfo input {};
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.start_register[0] = 8;
	input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::ReadOnly;
	input.bind.push_constant_size                = 16;

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	Expect(source.FindIndex("OpShiftRightLogical %uint") != Kyty::Core::STRING8_INVALID_INDEX,
	       "byte load extracts the selected byte from the loaded dword");
	Expect(source.FindIndex("OpBitwiseAnd %uint") != Kyty::Core::STRING8_INVALID_INDEX, "byte load masks to an unsigned byte");
	Expect(source.FindIndex("%uint_255") != Kyty::Core::STRING8_INVALID_INDEX, "byte load zero-extension mask is 0xff");
	Expect(source.FindIndex("OpBitcast %float") != Kyty::Core::STRING8_INVALID_INDEX, "unsigned byte result is stored as VGPR bits");
	Expect(source.FindIndex("%buffer_load_float1") == Kyty::Core::STRING8_INVALID_INDEX,
	       "unsigned byte load does not coerce to dword float loading");
	ExpectValidSpirv(source, "unsigned byte load emits valid SPIR-V");
}

ShaderCode ParseGen5BufferLoadDwordOffenIdxen()
{
	// Captured MUBUF at PC 0x35c:
	// buffer_load_dword v80, v[4:5], s[8:11], 0 offen idxen offset:0x90.
	const uint32_t shader[] = {0xe0303090u, 0x80025004u, 0xbf800000u, 0xbf810000u};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);
	return code;
}

ShaderCode ParseGen5BufferLoadDwordIdxen()
{
	// Captured MUBUF at normalized PC 0x58:
	// buffer_load_dword v2, v43, s[8:11], 0 idxen offset:0x38.
	const uint32_t shader[] = {0xe0302038u, 0x8002022bu, 0xbf800000u, 0xbf810000u};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);
	return code;
}

void VerifyGen5BufferLoadDwordIdxen()
{
	auto code = ParseGen5BufferLoadDwordIdxen();
	Expect(code.GetInstructions().Size() == 3, "idxen buffer load encoding parsed with endpgm");

	const auto& load = code.GetInstructions().At(0);
	Expect(load.type == ShaderInstructionType::BufferLoadDword, "idxen MUBUF opcode 0xc decodes as buffer_load_dword");
	Expect(load.format == ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen, "idxen buffer load preserves the scalar VGPR address format");
	Expect(load.dst.type == ShaderOperandType::Vgpr && load.dst.register_id == 2 && load.dst.size == 1,
	       "idxen buffer load destination decoded");
	Expect(load.src[0].type == ShaderOperandType::Vgpr && load.src[0].register_id == 43 && load.src[0].size == 1,
	       "idxen buffer load preserves its single index VGPR");
	Expect(load.src[1].type == ShaderOperandType::Sgpr && load.src[1].register_id == 8 && load.src[1].size == 4,
	       "idxen buffer load descriptor decoded");
	Expect(load.buffer_imm_offset == 0x38u, "idxen buffer load preserves the instruction byte offset");
	Expect(load.src[2].type == ShaderOperandType::IntegerInlineConstant && load.src[2].constant.i == 0,
	       "idxen buffer load keeps scalar offset independent from the instruction byte offset");

	ShaderComputeInputInfo input {};
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.start_register[0] = 8;
	input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::ReadOnly;
	input.bind.push_constant_size                = 16;

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	Expect(source.FindIndex("OpLoad %float %v43") != Kyty::Core::STRING8_INVALID_INDEX,
	       "idxen consumes its scalar address VGPR as the structured index");
	Expect(source.FindIndex("OpLoad %float %v44") == Kyty::Core::STRING8_INVALID_INDEX, "idxen does not consume a nonexistent offset VGPR");
	Expect(source.FindIndex("OpFunctionCall %void %buffer_load_float1") != Kyty::Core::STRING8_INVALID_INDEX,
	       "idxen buffer_load_dword uses the existing raw dword load contract");
}

void VerifyGen5BufferLoadDwordOffenIdxen()
{
	auto code = ParseGen5BufferLoadDwordOffenIdxen();
	Expect(code.GetInstructions().Size() == 3, "offen buffer load encoding parsed with endpgm");

	const auto& load = code.GetInstructions().At(0);
	Expect(load.type == ShaderInstructionType::BufferLoadDword, "offen MUBUF opcode 0xc decodes as buffer_load_dword");
	Expect(load.format == ShaderInstructionFormat::Vdata1Vaddr2SvSoffsOffenIdxen,
	       "offen and idxen preserve the paired VGPR address format");
	Expect(load.dst.type == ShaderOperandType::Vgpr && load.dst.register_id == 80, "offen buffer load destination decoded");
	Expect(load.src[0].type == ShaderOperandType::Vgpr && load.src[0].register_id == 4 && load.src[0].size == 2,
	       "offen buffer load preserves the offset and index VGPR pair");
	Expect(load.src[1].type == ShaderOperandType::Sgpr && load.src[1].register_id == 8 && load.src[1].size == 4,
	       "offen buffer load descriptor decoded");
	Expect(load.buffer_imm_offset == 0x90u, "offen buffer load preserves the instruction byte offset");
	Expect(load.src[2].type == ShaderOperandType::IntegerInlineConstant && load.src[2].constant.i == 0,
	       "offen buffer load keeps scalar offset independent from the instruction byte offset");

	ShaderComputeInputInfo input {};
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.start_register[0] = 8;
	input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::ReadOnly;
	input.bind.push_constant_size                = 16;

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	Expect(source.FindIndex("OpLoad %float %v4") != Kyty::Core::STRING8_INVALID_INDEX,
	       "offen consumes the first address VGPR as a per-lane byte offset");
	Expect(source.FindIndex("OpLoad %float %v5") != Kyty::Core::STRING8_INVALID_INDEX,
	       "idxen consumes the second address VGPR as the structured index");
	Expect(source.FindIndex("OpIAdd %int") != Kyty::Core::STRING8_INVALID_INDEX,
	       "offen adds the per-lane byte offset to the scalar and instruction offset");
	Expect(source.FindIndex("OpFunctionCall %void %buffer_load_float1") != Kyty::Core::STRING8_INVALID_INDEX,
	       "offen buffer_load_dword uses the existing raw dword load contract");
}

ShaderCode ParseGen5UnsignedSub(bool vop3)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);

	if (vop3)
	{
		const uint32_t shader[] = {
		    (0x35u << 26u) | (0x126u << 16u) | 80u,
		    128u | ((256u + 156u) << 9u),
		    0xbf800000u,
		    0xbf810000u,
		};
		ShaderParse(shader, &code);
	} else
	{
		const uint32_t shader[] = {
		    (0x26u << 25u) | (80u << 17u) | (156u << 9u) | 128u,
		    0xbf800000u,
		    0xbf810000u,
		};
		ShaderParse(shader, &code);
	}

	return code;
}

void VerifyGen5UnsignedSub()
{
	for (const bool vop3: {false, true})
	{
		auto code = ParseGen5UnsignedSub(vop3);
		Expect(code.GetInstructions().Size() == 3, "unsigned subtraction encoding parsed with endpgm");

		const auto& instruction = code.GetInstructions().At(0);
		Expect(instruction.type == ShaderInstructionType::VSubI32, "Gen5 opcode 0x26 decodes as unsigned subtraction");
		Expect(instruction.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1, "unsigned subtraction has no carry destination");
		Expect(instruction.dst.register_id == 80, "unsigned subtraction destination decoded");
		Expect(instruction.src[0].type == ShaderOperandType::IntegerInlineConstant && instruction.src[0].constant.i == 0,
		       "unsigned subtraction source zero decoded");
		Expect(instruction.src[1].type == ShaderOperandType::Vgpr && instruction.src[1].register_id == 156,
		       "captured unsigned subtraction VGPR source decoded");
		Expect(instruction.dst.multiplier == 1.0f && !instruction.dst.clamp && !instruction.src[0].negate && !instruction.src[0].absolute &&
		           !instruction.src[1].negate && !instruction.src[1].absolute,
		       "captured subtraction has neutral modifiers");
	}

	const auto source = SpirvGenerateSource(ParseGen5UnsignedSub(false), nullptr, nullptr, nullptr);
	Expect(std::strstr(source.c_str(), "OpISub %uint") != nullptr, "unsigned subtraction emits modular uint subtraction");
	Expect(std::strstr(source.c_str(), "OpISub %int") == nullptr, "unsigned subtraction does not use signed arithmetic");
}

ShaderCode ParseGen5AddCarryIn(bool overflow_case)
{
	const uint32_t word0 = (0x35u << 26u) | (0x128u << 16u) | (106u << 8u) | 80u;
	const uint32_t word1 = (overflow_case ? 255u : 128u) | (128u << 9u) | (8u << 18u);

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	if (overflow_case)
	{
		const uint32_t shader[] = {word0, word1, 0xffffffffu, 0xbf800000u, 0xbf810000u};
		ShaderParse(shader, &code);
	} else
	{
		const uint32_t shader[] = {word0, word1, 0xbf800000u, 0xbf810000u};
		ShaderParse(shader, &code);
	}
	return code;
}

ShaderCode ParseGen5AddCarryInVop2()
{
	const uint32_t shader[] = {
	    (0x28u << 25u) | (80u << 17u) | (80u << 9u) | 128u,
	    0xbf800000u,
	    0xbf810000u,
	};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);
	return code;
}

void VerifyGen5AddCarryIn()
{
	for (const bool overflow_case: {false, true})
	{
		auto code = ParseGen5AddCarryIn(overflow_case);
		Expect(code.GetInstructions().Size() == 3, "carry-in encoding parsed with endpgm");

		const auto& instruction = code.GetInstructions().At(0);
		Expect(instruction.type == ShaderInstructionType::VAddCoCiU32, "Gen5 VOP3 opcode 0x128 decodes as v_add_co_ci_u32");
		Expect(instruction.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2,
		       "carry-in arithmetic preserves vector result, carry destination, and SGPR pair source");
		Expect(instruction.dst.type == ShaderOperandType::Vgpr && instruction.dst.register_id == 80, "carry-in vector destination decoded");
		Expect(instruction.dst2.type == ShaderOperandType::VccLo && instruction.dst2.register_id == 0 && instruction.dst2.size == 2,
		       "carry-out destination decodes as VCC pair");
		Expect(instruction.src[0].type == (overflow_case ? ShaderOperandType::LiteralConstant : ShaderOperandType::IntegerInlineConstant),
		       "carry-in first operand encoding preserved");
		Expect(!overflow_case || instruction.src[0].constant.u == 0xffffffffu, "carry-in overflow operand preserves all-one bits");
		Expect(instruction.src[1].type == ShaderOperandType::IntegerInlineConstant && instruction.src[1].constant.i == 0,
		       "carry-in second operand decoded");
		Expect(instruction.src[2].type == ShaderOperandType::Sgpr && instruction.src[2].register_id == 8 && instruction.src[2].size == 2,
		       "carry-in source decodes as SGPR8:9");
		Expect(instruction.dst.multiplier == 1.0f && !instruction.dst.clamp && !instruction.src[0].negate && !instruction.src[0].absolute &&
		           !instruction.src[1].negate && !instruction.src[1].absolute && !instruction.src[2].negate && !instruction.src[2].absolute,
		       "captured carry-in encoding has neutral supported modifiers");
	}

	const auto source = SpirvGenerateSource(ParseGen5AddCarryIn(false), nullptr, nullptr, nullptr);
	Expect(std::strstr(source.c_str(), "%addc = OpFunction %v2uint") != nullptr, "carry-in uses the shared modular add-with-carry helper");
	Expect(std::strstr(source.c_str(), "OpFunctionCall %v2uint %addc %t0_0 %t1_0 %t2_0") != nullptr,
	       "carry-in passes both operands and the SGPR carry source to shared IR");
	Expect(std::strstr(source.c_str(), "OpIAddCarry %ResTypeU") != nullptr, "carry-in helper preserves unsigned carry computation");
	Expect(std::strstr(source.c_str(), "OpIAdd %uint") != nullptr, "carry-in helper adds the incoming carry before storing the result");
	Expect(std::strstr(source.c_str(), "OpLoad %uint %s8") != nullptr, "carry-in loads the low dword of the SGPR carry pair");

	const auto overflow_source = SpirvGenerateSource(ParseGen5AddCarryIn(true), nullptr, nullptr, nullptr);
	Expect(std::strstr(overflow_source.c_str(), "OpFunctionCall %v2uint %addc %t0_0 %t1_0 %t2_0") != nullptr,
	       "overflow case keeps the shared carry-in arithmetic path");
	Expect(std::strstr(overflow_source.c_str(), "%uint_0xffffffff") != nullptr,
	       "overflow case preserves the all-one unsigned operand in SPIR-V");

	auto vop2_code = ParseGen5AddCarryInVop2();
	Expect(vop2_code.GetInstructions().Size() == 3, "direct carry-in encoding parsed with endpgm");
	const auto& vop2_instruction = vop2_code.GetInstructions().At(0);
	Expect(vop2_instruction.type == ShaderInstructionType::VAddCoCiU32, "direct Gen5 VOP2 opcode 0x28 shares the carry-in IR contract");
	Expect(vop2_instruction.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2,
	       "direct carry-in encoding normalizes to the shared five-operand format");
	Expect(vop2_instruction.src[2].type == ShaderOperandType::VccLo && vop2_instruction.src[2].size == 2 &&
	           vop2_instruction.dst2.type == ShaderOperandType::VccLo && vop2_instruction.dst2.size == 2,
	       "direct carry-in uses implicit VCC for both carry input and output");
	const auto vop2_source = SpirvGenerateSource(vop2_code, nullptr, nullptr, nullptr);
	Expect(std::strstr(vop2_source.c_str(), "OpFunctionCall %v2uint %addc %t0_0 %t1_0 %t2_0") != nullptr,
	       "direct carry-in reuses the shared SPIR-V helper");
}

void VerifyGen5XnorVop2()
{
	// Captured Gen5 VOP2 at normalized PC 0xdf4:
	// v_xnor_b32 v96, v141, v96.
	// AMD RDNA2 ISA, Table 77 (VOP2 opcode 30):
	// https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture
	const uint32_t shader[] = {0x3cc0c18du, 0xbf800000u, 0xbf810000u};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	Expect(code.GetInstructions().Size() == 3, "Gen5 VOP2 XNOR parses with endpgm");
	const auto& instruction = code.GetInstructions().At(0);
	Expect(instruction.type == ShaderInstructionType::VXnorB32, "Gen5 VOP2 opcode 0x1e decodes as v_xnor_b32");
	Expect(instruction.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1, "XNOR keeps the two-source vector format");
	Expect(instruction.dst.type == ShaderOperandType::Vgpr && instruction.dst.register_id == 96, "XNOR destination decoded");
	Expect(instruction.src[0].type == ShaderOperandType::Vgpr && instruction.src[0].register_id == 141, "XNOR first source decoded");
	Expect(instruction.src[1].type == ShaderOperandType::Vgpr && instruction.src[1].register_id == 96, "XNOR second source decoded");

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, nullptr);
	Expect(source.FindIndex("OpBitwiseXor %uint %t0_0 %t1_0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "XNOR computes the bitwise XOR of both operands");
	Expect(source.FindIndex("%t_0 = OpNot %uint %tx_0") != Kyty::Core::STRING8_INVALID_INDEX, "XNOR complements the XOR result");

	Kyty::Config::SetNextGen(false);
	ShaderCode legacy_code;
	legacy_code.SetType(ShaderType::Compute);
	ShaderParse(shader, &legacy_code);
	Kyty::Config::SetNextGen(true);

	Expect(legacy_code.GetInstructions().At(0).type == ShaderInstructionType::VBfmB32, "legacy VOP2 opcode 0x1e remains v_bfm_b32");
	const auto legacy_source = SpirvGenerateSource(legacy_code, nullptr, nullptr, nullptr);
	Expect(legacy_source.FindIndex("OpBitFieldInsert %uint") != Kyty::Core::STRING8_INVALID_INDEX,
	       "legacy VOP2 opcode 0x1e retains bit-field-mask semantics");
}

void VerifyGen5BitCountVop3()
{
	// Captured Gen5 VOP3 at normalized PC 0xe04:
	// v_bcnt_u32_b32 v98, v97, 0.
	// AMD RDNA2 ISA, Table 83 (VOP3A opcode 868):
	// https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture
	const uint32_t shader[] = {0xd7640062u, 0x00010161u, 0xbf800000u, 0xbf810000u};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	Expect(code.GetInstructions().Size() == 3, "Gen5 VOP3 bit count parses with endpgm");
	const auto& instruction = code.GetInstructions().At(0);
	Expect(instruction.type == ShaderInstructionType::VBcntU32B32, "Gen5 VOP3 opcode 0x364 decodes as v_bcnt_u32_b32");
	Expect(instruction.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1, "bit count normalizes to the shared two-source format");
	Expect(instruction.src_num == 2, "bit count ignores the unused VOP3 src2 field");
	Expect(instruction.dst.type == ShaderOperandType::Vgpr && instruction.dst.register_id == 98, "bit count destination decoded");
	Expect(instruction.src[0].type == ShaderOperandType::Vgpr && instruction.src[0].register_id == 97, "bit count value source decoded");
	Expect(instruction.src[1].type == ShaderOperandType::IntegerInlineConstant && instruction.src[1].constant.i == 0,
	       "bit count accumulator source decoded");
	Expect(instruction.dst.multiplier == 1.0f && !instruction.dst.clamp && !instruction.src[0].negate && !instruction.src[0].absolute &&
	           !instruction.src[1].negate && !instruction.src[1].absolute,
	       "captured bit count encoding has neutral supported modifiers");

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, nullptr);
	Expect(source.FindIndex("OpBitCount %int %t0_0") != Kyty::Core::STRING8_INVALID_INDEX, "bit count uses the shared population-count IR");
	Expect(source.FindIndex("OpIAdd %uint %tbu_0 %t1_0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "bit count adds the explicit accumulator source");
}

void VerifyGen5ShiftLeftOrVop3()
{
	// Captured Gen5 VOP3 at normalized PC 0xe1c:
	// v_lshl_or_b32 v96, v98, 19, v96.
	// AMD RDNA2 ISA, Table 83 (VOP3A opcode 879):
	// https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture
	const uint32_t shader[] = {0xd76f0060u, 0x05812762u, 0xbf800000u, 0xbf810000u};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	Expect(code.GetInstructions().Size() == 3, "Gen5 VOP3 shift-or parses with endpgm");
	const auto& instruction = code.GetInstructions().At(0);
	Expect(instruction.type == ShaderInstructionType::VLshlOrB32, "Gen5 VOP3 opcode 0x36f decodes as v_lshl_or_b32");
	Expect(instruction.format == ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2, "shift-or keeps the three-source vector format");
	Expect(instruction.src_num == 3, "shift-or consumes all three VOP3 sources");
	Expect(instruction.dst.type == ShaderOperandType::Vgpr && instruction.dst.register_id == 96, "shift-or destination decoded");
	Expect(instruction.src[0].type == ShaderOperandType::Vgpr && instruction.src[0].register_id == 98, "shift-or value source decoded");
	Expect(instruction.src[1].type == ShaderOperandType::IntegerInlineConstant && instruction.src[1].constant.i == 19,
	       "shift-or count source decoded");
	Expect(instruction.src[2].type == ShaderOperandType::Vgpr && instruction.src[2].register_id == 96, "shift-or OR source decoded");
	Expect(instruction.dst.multiplier == 1.0f && !instruction.dst.clamp && !instruction.src[0].negate && !instruction.src[0].absolute &&
	           !instruction.src[1].negate && !instruction.src[1].absolute && !instruction.src[2].negate && !instruction.src[2].absolute,
	       "captured shift-or encoding has neutral supported modifiers");

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, nullptr);
	Expect(source.FindIndex("OpBitwiseAnd %uint %t1_0 %uint_31") != Kyty::Core::STRING8_INVALID_INDEX,
	       "shift-or masks the shift count to five bits");
	Expect(source.FindIndex("OpShiftLeftLogical %uint %t0_0 %ts_0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "shift-or shifts the first source by the masked count");
	Expect(source.FindIndex("OpBitwiseOr %uint %tm_0 %t2_0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "shift-or combines the shifted value with the third source");
}

void VerifyGen5AndOrVop3()
{
	// Captured Gen5 VOP3 at normalized PC 0x1abc:
	// v_and_or_b32 v95, 0x07000000, v175, v151.
	// AMD RDNA2 ISA, Table 83 (VOP3A opcode 881):
	// https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture
	const uint32_t shader[] = {
	    0xd771005fu, 0x065f5effu, 0x07000000u,
	    0xd771005fu, 0x065f5effu, 0x07000000u,
	    0xbf810000u,
	};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	Expect(code.GetInstructions().Size() == 3, "Gen5 VOP3 and-or parses with endpgm");
	const auto& instruction = code.GetInstructions().At(0);
	const auto& repeated    = code.GetInstructions().At(1);
	Expect(instruction.type == ShaderInstructionType::VAndOrB32, "Gen5 VOP3 opcode 0x371 decodes as v_and_or_b32");
	Expect(repeated.type == ShaderInstructionType::VAndOrB32 &&
	           repeated.src[0].type == ShaderOperandType::LiteralConstant && repeated.src[0].constant.u == 0x07000000u,
	       "repeated and-or consumes its own trailing literal before endpgm");
	Expect(instruction.format == ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2, "and-or keeps the three-source vector format");
	Expect(instruction.src_num == 3, "and-or consumes all three VOP3 sources");
	Expect(instruction.dst.type == ShaderOperandType::Vgpr && instruction.dst.register_id == 95, "and-or destination decoded");
	Expect(instruction.src[0].type == ShaderOperandType::LiteralConstant && instruction.src[0].constant.u == 0x07000000u,
	       "and-or literal mask decoded");
	Expect(instruction.src[1].type == ShaderOperandType::Vgpr && instruction.src[1].register_id == 175,
	       "and-or second source decoded");
	Expect(instruction.src[2].type == ShaderOperandType::Vgpr && instruction.src[2].register_id == 151,
	       "and-or third source decoded");
	Expect(instruction.dst.multiplier == 1.0f && !instruction.dst.clamp && !instruction.src[0].negate && !instruction.src[0].absolute &&
	           !instruction.src[1].negate && !instruction.src[1].absolute && !instruction.src[2].negate && !instruction.src[2].absolute,
	       "captured and-or encoding has neutral supported modifiers");

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, nullptr);
	Expect(source.FindIndex("OpBitwiseAnd %uint %t0_0 %t1_0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "and-or combines the first two sources with bitwise AND");
	Expect(source.FindIndex("OpBitwiseOr %uint %tm_0 %t2_0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "and-or combines the intermediate result with the third source");
}

void VerifyGen5UnsignedMinEncodings()
{
	// Captured Gen5 VOP2 at normalized PC 0x3ba4:
	// v_min_u32 v76, v2, v59.
	const uint32_t shader[] = {0xbf800000u, 0x26987702u, 0xbf810000u};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	Expect(code.GetInstructions().Size() == 3, "Gen5 VOP2 unsigned min parses with endpgm");
	const auto& instruction = code.GetInstructions().At(1);
	Expect(instruction.type == ShaderInstructionType::VMinU32, "Gen5 VOP2 opcode 0x13 decodes as v_min_u32");
	Expect(instruction.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1, "unsigned min uses the binary vector format");
	Expect(instruction.dst.type == ShaderOperandType::Vgpr && instruction.dst.register_id == 76,
	       "unsigned min decodes its destination");
	Expect(instruction.src[0].type == ShaderOperandType::Vgpr && instruction.src[0].register_id == 2,
	       "unsigned min decodes its first operand");
	Expect(instruction.src[1].type == ShaderOperandType::Vgpr && instruction.src[1].register_id == 59,
	       "unsigned min decodes its second operand");
	Expect(instruction.src[0].swizzle == 6 && instruction.src[1].swizzle == 6 && !instruction.src[0].absolute &&
	           !instruction.src[1].absolute && !instruction.src[0].negate && !instruction.src[1].negate &&
	           !instruction.dst.clamp && instruction.dst.multiplier == 1.0f,
	       "captured unsigned min uses neutral DWORD modifiers");

	ShaderComputeInputInfo input {};
	input.threads_num[0] = 1;
	input.threads_num[1] = 1;
	input.threads_num[2] = 1;
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	Expect(source.FindIndex("OpExtInst %uint %GLSL_std_450 UMin %t0_1 %t1_1") != Kyty::Core::STRING8_INVALID_INDEX,
	       "unsigned min compares operands without signed reinterpretation");
	Expect(source.FindIndex("OpSelect %float %exec_lo_b_1 %tf_1 %tdst_1") != Kyty::Core::STRING8_INVALID_INDEX,
	       "unsigned min preserves its destination when EXEC is inactive");
	ExpectValidSpirv(source, "unsigned min emits valid SPIR-V");

	const uint32_t vop3_shader[] = {0xbf800000u, 0xd513004cu, 0x00027702u, 0xbf810000u};
	ShaderCode     vop3_code;
	vop3_code.SetType(ShaderType::Compute);
	ShaderParse(vop3_shader, &vop3_code);

	Expect(vop3_code.GetInstructions().Size() == 3, "Gen5 VOP3 unsigned min parses with endpgm");
	const auto& vop3 = vop3_code.GetInstructions().At(1);
	Expect(vop3.type == ShaderInstructionType::VMinU32 && vop3.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1,
	       "Gen5 VOP3 opcode 0x113 shares unsigned min semantics");
	Expect(vop3.dst.type == ShaderOperandType::Vgpr && vop3.dst.register_id == 76 &&
	           vop3.src[0].type == ShaderOperandType::Vgpr && vop3.src[0].register_id == 2 &&
	           vop3.src[1].type == ShaderOperandType::Vgpr && vop3.src[1].register_id == 59,
	       "Gen5 VOP3 unsigned min preserves destination and operands");
	Expect(!vop3.src[0].absolute && !vop3.src[1].absolute && !vop3.src[0].negate && !vop3.src[1].negate &&
	           !vop3.dst.clamp && vop3.dst.multiplier == 1.0f,
	       "Gen5 VOP3 unsigned min uses neutral modifiers");

	const auto vop3_source = SpirvGenerateSource(vop3_code, nullptr, nullptr, &input);
	Expect(vop3_source.FindIndex("OpExtInst %uint %GLSL_std_450 UMin %t0_1 %t1_1") != Kyty::Core::STRING8_INVALID_INDEX,
	       "Gen5 VOP3 unsigned min uses the shared unsigned backend");
	Expect(vop3_source.FindIndex("OpSelect %float %exec_lo_b_1 %tf_1 %tdst_1") != Kyty::Core::STRING8_INVALID_INDEX,
	       "Gen5 VOP3 unsigned min preserves inactive destinations");
	ExpectValidSpirv(vop3_source, "Gen5 VOP3 unsigned min emits valid SPIR-V");
}

void VerifyGen5InverseTwoPiInlineConstant()
{
	// Captured Gen5 VOP2 at normalized PC 0x4118:
	// v_mul_f32 v123, 1/(2*pi), v124.
	// AMD defines operand code 248 as exact single-precision bits 0x3e22f983.
	const uint32_t shader[] = {0xbf800000u, 0x10f6f8f8u, 0xbf810000u};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	Expect(code.GetInstructions().Size() == 3, "inverse-two-pi VOP2 parses without consuming a literal DWORD");
	const auto& multiply = code.GetInstructions().At(1);
	Expect(multiply.type == ShaderInstructionType::VMulF32 && multiply.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1,
	       "captured inverse-two-pi operand belongs to v_mul_f32");
	Expect(multiply.dst.type == ShaderOperandType::Vgpr && multiply.dst.register_id == 123,
	       "inverse-two-pi multiply decodes its destination");
	Expect(multiply.src[0].type == ShaderOperandType::FloatInlineConstant && multiply.src[0].constant.u == 0x3e22f983u &&
	           multiply.src[0].size == 0,
	       "operand 248 preserves the architected single-precision bit pattern");
	Expect(multiply.src[1].type == ShaderOperandType::Vgpr && multiply.src[1].register_id == 124,
	       "inverse-two-pi multiply decodes its vector source");

	ShaderComputeInputInfo input {};
	input.threads_num[0] = 1;
	input.threads_num[1] = 1;
	input.threads_num[2] = 1;
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	Expect(source.FindIndex("%float_0_159155 = OpConstant %float 0.159154937") != Kyty::Core::STRING8_INVALID_INDEX,
	       "inverse-two-pi is serialized with round-trip precision");
	Expect(source.FindIndex("%t0_1 = OpCopyObject %float %float_0_159155") != Kyty::Core::STRING8_INVALID_INDEX,
	       "floating inline constants are copied without an invalid same-type bitcast");
	Expect(source.FindIndex("OpFMul %float %t0_1 %t1_1") != Kyty::Core::STRING8_INVALID_INDEX,
	       "captured v_mul_f32 consumes the inverse-two-pi constant");
	const auto binary = AssembleValidSpirv(source, "inverse-two-pi inline constant emits valid SPIR-V");
	Expect(std::find(binary.begin(), binary.end(), 0x3e22f983u) != binary.end(),
	       "assembled inverse-two-pi constant preserves its architected bits");
}

void VerifyGen5UnsignedMad64Vop3b()
{
	// Captured Gen5 VOP3B at normalized PC 0x1ec4:
	// v_mad_u64_u32 v[5:6], vcc, 0x92492492, v92, 0x92492492.
	// AMD RDNA2 ISA, VOP3B opcode 374; one literal may feed multiple operands:
	// https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture
	const uint32_t shader[] = {
	    0xd5766a05u, 0x03feb8ffu, 0x92492492u,
	    0xd5766a05u, 0x03feb8ffu, 0x92492492u,
	    0xbf810000u,
	};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	Expect(code.GetInstructions().Size() == 3, "Gen5 VOP3B unsigned MAD64 consumes one shared literal per instruction");
	const auto& instruction = code.GetInstructions().At(0);
	const auto& repeated    = code.GetInstructions().At(1);
	Expect(instruction.type == ShaderInstructionType::VMadU64U32, "Gen5 VOP3B opcode 0x176 decodes as v_mad_u64_u32");
	Expect(instruction.format == ShaderInstructionFormat::Vdst2Sdst2Vsrc0Vsrc1Vsrc2Pair,
	       "unsigned MAD64 exposes vector result, scalar carry, and 64-bit addend");
	Expect(instruction.dst.type == ShaderOperandType::Vgpr && instruction.dst.register_id == 5 && instruction.dst.size == 2,
	       "unsigned MAD64 decodes its 64-bit vector destination");
	Expect(instruction.dst2.type == ShaderOperandType::VccLo && instruction.dst2.size == 2,
	       "unsigned MAD64 decodes its scalar carry destination");
	Expect(instruction.src_num == 3, "unsigned MAD64 consumes three sources");
	Expect(instruction.src[0].type == ShaderOperandType::LiteralConstant && instruction.src[0].constant.u == 0x92492492u,
	       "unsigned MAD64 decodes the shared literal multiplier");
	Expect(instruction.src[1].type == ShaderOperandType::Vgpr && instruction.src[1].register_id == 92,
	       "unsigned MAD64 decodes the vector multiplier");
	Expect(instruction.src[2].type == ShaderOperandType::LiteralConstant && instruction.src[2].constant.u == 0x92492492u &&
	           instruction.src[2].size == 2,
	       "unsigned MAD64 reuses the literal as a zero-extended 64-bit addend");
	Expect(repeated.type == ShaderInstructionType::VMadU64U32 && repeated.src[0].constant.u == 0x92492492u &&
	           repeated.src[2].constant.u == 0x92492492u,
	       "repeated unsigned MAD64 starts immediately after the single literal");
	Expect(!instruction.dst.clamp && instruction.dst.multiplier == 1.0f && !instruction.src[0].negate &&
	           !instruction.src[0].absolute && !instruction.src[1].negate && !instruction.src[1].absolute &&
	           !instruction.src[2].negate && !instruction.src[2].absolute,
	       "captured unsigned MAD64 has no VOP3A modifiers");

	ShaderComputeInputInfo compute_input {};
	compute_input.threads_num[0] = 1;
	compute_input.threads_num[1] = 1;
	compute_input.threads_num[2] = 1;
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &compute_input);
	Expect(source.FindIndex("OpUMulExtended %ResTypeU") != Kyty::Core::STRING8_INVALID_INDEX,
	       "unsigned MAD64 preserves the full 64-bit multiplication product");
	const auto low_add = source.FindIndex("%tsum_lo_pair_0 = OpIAddCarry %ResTypeU %tmul_lo_0 %tadd_lo_0");
	const auto high_add = source.FindIndex("%tsum_hi_base_pair_0 = OpIAddCarry %ResTypeU %tmul_hi_0 %tadd_hi_0");
	const auto propagated_add = source.FindIndex("%tsum_hi_pair_0 = OpIAddCarry %ResTypeU %tsum_hi_base_0 %tcarry_lo_0");
	const auto final_carry = source.FindIndex("%tcarry_out_0 = OpBitwiseOr %uint %tcarry_hi_base_0 %tcarry_hi_extra_0");
	Expect(low_add != Kyty::Core::STRING8_INVALID_INDEX && high_add != Kyty::Core::STRING8_INVALID_INDEX &&
	           propagated_add != Kyty::Core::STRING8_INVALID_INDEX && final_carry != Kyty::Core::STRING8_INVALID_INDEX &&
	           low_add < high_add && high_add < propagated_add && propagated_add < final_carry,
	       "unsigned MAD64 propagates low and high carries in order");
	Expect(source.FindIndex("OpStore %v5") != Kyty::Core::STRING8_INVALID_INDEX &&
	           source.FindIndex("OpStore %v6") != Kyty::Core::STRING8_INVALID_INDEX,
	       "unsigned MAD64 writes both vector result dwords");
	Expect(source.FindIndex("OpStore %vcc_lo") != Kyty::Core::STRING8_INVALID_INDEX,
	       "unsigned MAD64 writes its carry-out destination");
	Expect(source.FindIndex("OpSelect %float %tactive_mad_0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "unsigned MAD64 preserves inactive vector lanes");
	ExpectValidSpirv(source, "unsigned MAD64 emits valid SPIR-V");

	const auto no_carry = ReferenceUnsignedMad64(3u, 7u, 11u);
	Expect(no_carry.value == 32u && no_carry.carry == 0u, "unsigned MAD64 computes a basic multiply-add");

	const auto low_carry = ReferenceUnsignedMad64(1u, 1u, 0x00000000ffffffffull);
	Expect(low_carry.value == 0x0000000100000000ull && low_carry.carry == 0u,
	       "unsigned MAD64 propagates low-word carry into the high word");

	const auto high_overflow = ReferenceUnsignedMad64(std::numeric_limits<uint32_t>::max(),
	                                                  std::numeric_limits<uint32_t>::max(), 0x00000001ffffffffull);
	Expect(high_overflow.value == 0u && high_overflow.carry == 1u,
	       "unsigned MAD64 reports high-word overflow after carry propagation");

	const auto full_overflow = ReferenceUnsignedMad64(std::numeric_limits<uint32_t>::max(),
	                                                  std::numeric_limits<uint32_t>::max(),
	                                                  std::numeric_limits<uint64_t>::max());
	Expect(full_overflow.value == 0xfffffffe00000000ull && full_overflow.carry == 1u,
	       "unsigned MAD64 handles the maximum product and addend");
}

ShaderCode ParseGen5ReciprocalIFlag(bool vop3)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);

	if (vop3)
	{
		const uint32_t shader[] = {
		    (0x35u << 26u) | (0x1abu << 16u) | 81u,
		    338u,
		    0xbf800000u,
		    0xbf810000u,
		};
		ShaderParse(shader, &code);
	} else
	{
		const uint32_t shader[] = {
		    0x7ea25752u, // Captured Gen5 VOP1 v_rcp_iflag_f32 at PC 0x5e8.
		    0xbf800000u,
		    0xbf810000u,
		};
		ShaderParse(shader, &code);
	}

	return code;
}

void VerifyGen5ReciprocalIFlag()
{
	for (const bool vop3: {false, true})
	{
		auto code = ParseGen5ReciprocalIFlag(vop3);
		Expect(code.GetInstructions().Size() == 3, "reciprocal-iflag encoding parsed with endpgm");

		const auto& instruction = code.GetInstructions().At(0);
		Expect(instruction.type == ShaderInstructionType::VRcpF32, "reciprocal-iflag shares the existing reciprocal value contract");
		Expect(instruction.format == ShaderInstructionFormat::SVdstSVsrc0, "reciprocal-iflag uses the one-source VGPR format");
		Expect(instruction.dst.type == ShaderOperandType::Vgpr && instruction.dst.register_id == 81,
		       "captured reciprocal-iflag destination decoded");
		Expect(instruction.src[0].type == ShaderOperandType::Vgpr && instruction.src[0].register_id == 82,
		       "captured reciprocal-iflag source decoded");
		Expect(instruction.dst.multiplier == 1.0f && !instruction.dst.clamp && !instruction.src[0].negate && !instruction.src[0].absolute,
		       "captured reciprocal-iflag has neutral modifiers");
	}

	const auto source = SpirvGenerateSource(ParseGen5ReciprocalIFlag(false), nullptr, nullptr, nullptr);
	Expect(std::strstr(source.c_str(), "OpFDiv %float %float_1_000000 %t0_0") != nullptr,
	       "reciprocal-iflag emits one divided by source through SPIR-V");
	Expect(std::strstr(source.c_str(), "OpExtInst %float %GLSL_std_450 Inverse") == nullptr,
	       "reciprocal-iflag does not use a host-library reciprocal shortcut");
	// OpFDiv preserves the signed zero/infinity and NaN classes. Subnormal
	// handling remains controlled by the guest FP_DENORM mode, which is not yet
	// represented in ShaderCode; the recompiler must not silently rewrite it.
}

ShaderCode ParseGen5ReciprocalIFlagLiteral(uint32_t input_bits)
{
	const uint32_t shader[] = {
	    (0x3fu << 25u) | (0x2bu << 9u) | (81u << 17u) | 255u,
	    input_bits,
	    0xbf800000u,
	    0xbf810000u,
	};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);
	return code;
}

void VerifyGen5ReciprocalIFlagExceptionalInputs()
{
	const uint32_t inputs[] = {
	    0x00000000u, // +0: signed +infinity result and integer divide-by-zero flag only.
	    0x80000000u, // -0: signed -infinity result and integer divide-by-zero flag only.
	    0x7f800000u, // +infinity: +0 result, no floating exception.
	    0xff800000u, // -infinity: -0 result, no floating exception.
	    0x7fc00000u, // quiet NaN: NaN result, no floating exception.
	    0x00000001u, // +subnormal: guest FP_DENORM mode controls handling.
	    0x80000001u, // -subnormal: guest FP_DENORM mode controls handling.
	};

	for (const auto input_bits: inputs)
	{
		auto code = ParseGen5ReciprocalIFlagLiteral(input_bits);
		Expect(code.GetInstructions().Size() == 3, "reciprocal-iflag literal has endpgm");

		const auto& instruction = code.GetInstructions().At(0);
		Expect(instruction.type == ShaderInstructionType::VRcpF32, "reciprocal-iflag exceptional input keeps shared IR");
		Expect(instruction.src[0].type == ShaderOperandType::LiteralConstant && instruction.src[0].constant.u == input_bits,
		       "reciprocal-iflag preserves exceptional input bits");

		const auto source = SpirvGenerateSource(code, nullptr, nullptr, nullptr);
		Expect(std::strstr(source.c_str(), "OpFDiv %float %float_1_000000 %t0_0") != nullptr,
		       "reciprocal-iflag exceptional input remains an explicit SPIR-V divide");
	}
}

ShaderCode ParseGen5ImageSampleLzDmask1()
{
	// Captured Gen5 MIMG image_sample_lz at PC 0xa68.  The third DWORD is the
	// NSA address word; the following s_endpgm begins at PC 0xc.
	const uint32_t shader[] = {0xbf800000u, 0xf09c010au, 0x0040a66cu, 0x00000083u, 0xbf810000u};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);
	return code;
}

ShaderCode ParseGen5ImageSampleLzDmask3()
{
	// Captured Gen5 MIMG at normalized PC 0x3a18. NSA contributes the third
	// DWORD, and dmask 0x3 requests the R and G result components.
	const uint32_t shader[] = {0xbf800000u, 0xf09c030au, 0x00403740u, 0x00000006u, 0xbf810000u};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);
	return code;
}

void VerifyGen5ImageSampleLzDmask3()
{
	auto code = ParseGen5ImageSampleLzDmask3();
	Expect(code.GetInstructions().Size() == 3, "image_sample_lz dmask 3 consumes its NSA word");

	const auto& sample = code.GetInstructions().At(1);
	Expect(sample.type == ShaderInstructionType::ImageSampleLz, "image_sample_lz dmask 3 decodes the captured opcode");
	Expect(sample.format == ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3,
	       "image_sample_lz dmask 3 uses the two-component MIMG format");
	Expect(sample.dst.type == ShaderOperandType::Vgpr && sample.dst.register_id == 55 && sample.dst.size == 2,
	       "image_sample_lz dmask 3 writes two consecutive VGPRs");
	Expect(sample.mimg_address_num == 5 && sample.mimg_address[0].register_id == 64 && sample.mimg_address[1].register_id == 6,
	       "image_sample_lz dmask 3 preserves captured NSA addressing");

	ShaderComputeInputInfo input {};
	input.threads_num[0]                              = 1;
	input.threads_num[1]                              = 1;
	input.threads_num[2]                              = 1;
	input.bind.push_constant_size                     = 48;
	input.bind.textures2D.textures_num                = 1;
	input.bind.textures2D.textures2d_sampled_num      = 1;
	input.bind.textures2D.desc[0].start_register      = 0;
	input.bind.textures2D.desc[0].usage               = ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].texture.fields[0]   = 0x0555f590u;
	input.bind.textures2D.desc[0].texture.fields[1]   = 0xc4700000u;
	input.bind.textures2D.desc[0].texture.fields[2]   = 0x00ffc0ffu;
	input.bind.textures2D.desc[0].texture.fields[3]   = 0x90000facu;
	input.bind.textures2D.desc[0].texture.fields[4]   = 0x00000000u;
	input.bind.textures2D.desc[0].texture.fields[5]   = 0x00700000u;
	input.bind.samplers.samplers_num                  = 1;
	input.bind.samplers.start_register[0]             = 8;
	const auto source                                 = SpirvGenerateSource(code, nullptr, nullptr, &input);

	Expect(source.FindIndex("OpImageSampleExplicitLod %v4float %t38_1 %t42_1 Lod %float_0_000000") !=
	           Kyty::Core::STRING8_INVALID_INDEX,
	       "image_sample_lz dmask 3 samples at explicit LOD");
	Expect(source.FindIndex("OpStore %v55") != Kyty::Core::STRING8_INVALID_INDEX &&
	           source.FindIndex("OpStore %v56") != Kyty::Core::STRING8_INVALID_INDEX,
	       "image_sample_lz dmask 3 stores R and G");
	Expect(source.FindIndex("OpAccessChain %_ptr_Function_float %temp_v4float %uint_2") == Kyty::Core::STRING8_INVALID_INDEX,
	       "image_sample_lz dmask 3 does not materialize B");
	ExpectValidSpirv(source, "image_sample_lz dmask 3 emits valid SPIR-V");
}

void VerifyGen5ImageSampleLzDmask1()
{
	auto code = ParseGen5ImageSampleLzDmask1();
	Expect(code.GetInstructions().Size() == 3, "image_sample_lz dmask 1 parses with endpgm");

	const auto& sample = code.GetInstructions().At(1);
	Expect(sample.type == ShaderInstructionType::ImageSampleLz, "image_sample_lz opcode is decoded");
	Expect(sample.format == ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1,
	       "image_sample_lz dmask 1 uses the single-component MIMG format");
	Expect(sample.dst.type == ShaderOperandType::Vgpr && sample.dst.register_id == 166 && sample.dst.size == 1,
	       "image_sample_lz dmask 1 writes one VDATA component");
	Expect(sample.src[0].type == ShaderOperandType::Vgpr && sample.src[0].register_id == 108 && sample.src[0].size == 3,
	       "image_sample_lz preserves the normalized address operand shape");
	Expect(sample.src[1].type == ShaderOperandType::Sgpr && sample.src[1].register_id == 0 && sample.src[1].size == 8,
	       "image_sample_lz preserves the SRSRC descriptor range");
	Expect(sample.src[2].type == ShaderOperandType::Sgpr && sample.src[2].register_id == 8 && sample.src[2].size == 4,
	       "image_sample_lz preserves the SSAMP descriptor range");
	Expect(sample.mimg_address_num == 5, "image_sample_lz preserves all four encoded NSA address bytes");
	const int expected_addresses[] = {108, 131, 0, 0, 0};
	for (int address = 0; address < 5; ++address)
	{
		Expect(sample.mimg_address[address].type == ShaderOperandType::Vgpr, "NSA address remains a VGPR operand");
		Expect(sample.mimg_address[address].register_id == expected_addresses[address], "NSA address register is preserved");
	}

	ShaderComputeInputInfo input {};
	input.bind.push_constant_size                   = 48;
	input.bind.textures2D.textures_num              = 1;
	input.bind.textures2D.textures2d_sampled_num    = 1;
	input.bind.textures2D.desc[0].start_register    = 0;
	input.bind.textures2D.desc[0].usage             = ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].texture.fields[0] = 0x0555f590u;
	input.bind.textures2D.desc[0].texture.fields[1] = 0xc4700000u;
	input.bind.textures2D.desc[0].texture.fields[2] = 0x00ffc0ffu;
	input.bind.textures2D.desc[0].texture.fields[3] = 0x90000facu;
	input.bind.textures2D.desc[0].texture.fields[4] = 0x00000000u;
	input.bind.textures2D.desc[0].texture.fields[5] = 0x00700000u;
	input.bind.samplers.samplers_num                = 1;
	input.bind.samplers.start_register[0]           = 8;
	const auto source                               = SpirvGenerateSource(code, nullptr, nullptr, &input);

	Expect(source.FindIndex("OpImageSampleExplicitLod %v4float") != Kyty::Core::STRING8_INVALID_INDEX,
	       "image_sample_lz uses explicit image sampling");
	Expect(source.FindIndex("Lod %float_0_000000") != Kyty::Core::STRING8_INVALID_INDEX, "image_sample_lz forces LOD zero");
	Expect(source.FindIndex("OpImageSampleImplicitLod") == Kyty::Core::STRING8_INVALID_INDEX, "image_sample_lz does not use implicit LOD");
	Expect(source.FindIndex("OpLoad %float %v108") != Kyty::Core::STRING8_INVALID_INDEX,
	       "2D sample consumes the first encoded X coordinate");
	Expect(source.FindIndex("OpLoad %float %v131") != Kyty::Core::STRING8_INVALID_INDEX, "2D NSA sample consumes the encoded Y coordinate");
	Expect(source.FindIndex("OpLoad %float %v0") == Kyty::Core::STRING8_INVALID_INDEX,
	       "2D sample rejects extra NSA coordinates instead of consuming padding");
	Expect(source.FindIndex("OpCompositeExtract %float %image_sample_lz_scalar_value_1 0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "dmask 1 extracts component R");
	Expect(source.FindIndex("OpCompositeExtract %float %image_sample_lz_scalar_value_1 1") == Kyty::Core::STRING8_INVALID_INDEX,
	       "dmask 1 does not extract component G");
}

Kyty::Core::String8 GenerateGen5DepthReferenceSample(uint8_t dimension, uint8_t descriptor_type, int flat_sampled_num,
                                                     int array_sampled_num)
{
	const bool arrayed = dimension != 1u;
	ShaderInstruction sample {};
	sample.type           = ShaderInstructionType::ImageSampleDrefLz;
	sample.format         = arrayed ? ShaderInstructionFormat::Vdata1Vaddr4StSsDmask1
	                                : ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1;
	sample.dst            = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	sample.src[0]         = {.type = ShaderOperandType::Vgpr, .register_id = 4, .size = arrayed ? 4 : 3};
	sample.src[1]         = {.type = ShaderOperandType::Sgpr, .register_id = 8, .size = 8};
	sample.src[2]         = {.type = ShaderOperandType::Sgpr, .register_id = 20, .size = 4};
	sample.src_num        = 3;
	sample.mimg_dimension = dimension;
	sample.mimg_dmask     = 0x1;

	ShaderInstruction end {};
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	ShaderInstruction nop {};
	nop.type   = ShaderInstructionType::VNop;
	nop.format = ShaderInstructionFormat::Empty;
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	code.GetInstructions().Add(sample);
	code.GetInstructions().Add(nop);
	code.GetInstructions().Add(end);

	ShaderPixelInputInfo input {};
	input.bind.push_constant_size                      = 48;
	input.bind.textures2D.textures_num                 = 1;
	input.bind.textures2D.textures2d_sampled_num       = flat_sampled_num;
	input.bind.textures2D.textures2d_array_sampled_num = array_sampled_num;
	input.bind.textures2D.desc[0].start_register       = 8;
	input.bind.textures2D.desc[0].usage                = ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].texture.fields[1]    = 22u << 20u;
	input.bind.textures2D.desc[0].texture.fields[3]    = static_cast<uint32_t>(descriptor_type) << 28u;
	input.bind.samplers.samplers_num                   = 1;
	input.bind.samplers.start_register[0]              = 20;
	input.bind.samplers.operations[0]                  = State::ImageSampleOperation::DepthReference;
	input.bind.samplers.samplers[0].fields[0]          = 6u << 12u;
	ShaderCalcBindingIndices(&input.bind);
	return SpirvGenerateSource(code, nullptr, &input, nullptr);
}

void VerifyGen5DepthReferenceSample()
{
	const auto flat = GenerateGen5DepthReferenceSample(1u, 9u, 1, 0);
	Expect(flat.FindIndex("OpImageSampleExplicitLod %v4float") != Kyty::Core::STRING8_INVALID_INDEX,
	       "2D SAMPLE_C samples LOD 0 as a regular image");
	Expect(flat.FindIndex("OpFOrdGreaterThanEqual %bool") != Kyty::Core::STRING8_INVALID_INDEX,
	       "2D SAMPLE_C applies S# DEPTH_COMPARE_FUNC in ALU");
	Expect(flat.FindIndex("OpImageSampleDrefExplicitLod") == Kyty::Core::STRING8_INVALID_INDEX,
	       "2D SAMPLE_C does not use Vulkan Dref sampling");
	Expect(flat.FindIndex("OpCompositeConstruct %v2float") != Kyty::Core::STRING8_INVALID_INDEX,
	       "2D depth-reference sample uses two coordinates");
	ExpectValidSpirv(flat, "2D depth-reference sample emits valid SPIR-V");

	const auto arrayed = GenerateGen5DepthReferenceSample(5u, 13u, 0, 1);
	Expect(arrayed.FindIndex("OpImageSampleExplicitLod %v4float") != Kyty::Core::STRING8_INVALID_INDEX,
	       "array SAMPLE_C samples LOD 0 as a regular image");
	Expect(arrayed.FindIndex("OpFOrdGreaterThanEqual %bool") != Kyty::Core::STRING8_INVALID_INDEX,
	       "array SAMPLE_C applies S# DEPTH_COMPARE_FUNC in ALU");
	Expect(arrayed.FindIndex("OpCompositeConstruct %v3float") != Kyty::Core::STRING8_INVALID_INDEX,
	       "array depth-reference sample includes the layer coordinate");
	ExpectValidSpirv(arrayed, "array depth-reference sample emits valid SPIR-V");

	const auto cube = GenerateGen5DepthReferenceSample(3u, 11u, 0, 1);
	Expect(cube.FindIndex("OpCompositeConstruct %v3float") != Kyty::Core::STRING8_INVALID_INDEX,
	       "cube depth-reference sample includes the face coordinate");
	ExpectValidSpirv(cube, "cube depth-reference sample emits valid SPIR-V");
}

ShaderCode ParseGen5SAndn1SaveexecB64()
{
	// Captured Gen5 SOP1 s_andn1_saveexec_b64 at PC 0xaac.
	const uint32_t shader[] = {0xbe96376au, 0xbf880005u, 0xbf810000u};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);
	return code;
}

void VerifyGen5SAndn1SaveexecB64()
{
	auto code = ParseGen5SAndn1SaveexecB64();
	Expect(code.GetInstructions().Size() == 3, "s_andn1_saveexec_b64 parses with endpgm");

	const auto& saveexec = code.GetInstructions().At(0);
	Expect(saveexec.type == ShaderInstructionType::SAndn1SaveexecB64, "SOP1 opcode 0x37 decodes as s_andn1_saveexec_b64");
	Expect(saveexec.format == ShaderInstructionFormat::Sdst2Ssrc02, "saveexec uses paired SGPR format");
	Expect(saveexec.dst.type == ShaderOperandType::Sgpr && saveexec.dst.register_id == 22 && saveexec.dst.size == 2,
	       "saveexec preserves the captured destination pair");
	Expect(saveexec.src[0].type == ShaderOperandType::VccLo && saveexec.src[0].register_id == 0 && saveexec.src[0].size == 2,
	       "saveexec preserves the captured source pair");

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, nullptr);
	Expect(source.FindIndex("OpNot %uint %t0_0") != Kyty::Core::STRING8_INVALID_INDEX, "andn1 negates the scalar source");
	Expect(source.FindIndex("OpBitwiseAnd %uint %t193_0 %t190_0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "andn1 intersects the negated source with EXEC");
	Expect(source.FindIndex("OpStore %s22 %t190_0") != Kyty::Core::STRING8_INVALID_INDEX, "saveexec stores the original EXEC low dword");
	Expect(source.FindIndex("OpStore %s23 %t191_0") != Kyty::Core::STRING8_INVALID_INDEX, "saveexec stores the original EXEC high dword");
	Expect(source.FindIndex("OpStore %exec_lo") != Kyty::Core::STRING8_INVALID_INDEX, "andn1 updates EXEC low dword");
	Expect(source.FindIndex("OpStore %exec_hi") != Kyty::Core::STRING8_INVALID_INDEX, "andn1 updates EXEC high dword");
	Expect(source.FindIndex("OpStore %execz") != Kyty::Core::STRING8_INVALID_INDEX, "andn1 updates EXECZ status");
	Expect(source.FindIndex("OpStore %scc") != Kyty::Core::STRING8_INVALID_INDEX, "andn1 updates SCC from the new EXEC value");
}

void InitializeGraphicsConfig()
{
	char                        program[]  = "kyty_graphics_diagnostics_integration";
	char*                       argv[]     = {program, nullptr};
	Kyty::Core::SubsystemsList* subsystems = Kyty::Core::SubsystemsListSingleton::Instance();
	subsystems->SetArgs(1, argv);
	using Kyty::Config::ConfigSubsystem;
	using Kyty::Core::CoreSubsystem;
	using Kyty::Core::ThreadsSubsystem;
	using Kyty::Log::LogSubsystem;
	using Kyty::Math::MathSubsystem;
	subsystems->Add(CoreSubsystem::Instance(), {});
	subsystems->Add(ConfigSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(MathSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(ThreadsSubsystem::Instance(), {CoreSubsystem::Instance()});
	subsystems->Add(LogSubsystem::Instance(), {CoreSubsystem::Instance(), ConfigSubsystem::Instance(), ThreadsSubsystem::Instance()});
	Expect(subsystems->InitAll(false), "graphics config subsystem must initialize");
	Kyty::Config::SetNextGen(true);
}

class VulkanSamplerContext
{
public:
	~VulkanSamplerContext()
	{
		if (bound)
		{
			Expect(GraphicsRenderUnbindContextForTesting(&context), "sampler test context must unbind");
		}
		if (context.device != VK_NULL_HANDLE)
		{
			vkDestroyDevice(context.device, nullptr);
		}
		if (context.instance != VK_NULL_HANDLE)
		{
			vkDestroyInstance(context.instance, nullptr);
		}
	}

	[[nodiscard]] bool Initialize()
	{
		VkApplicationInfo application {};
		application.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		application.apiVersion = VK_API_VERSION_1_0;
		VkInstanceCreateInfo instance_info {};
		instance_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instance_info.pApplicationInfo = &application;
		if (vkCreateInstance(&instance_info, nullptr, &context.instance) != VK_SUCCESS)
		{
			return false;
		}

		uint32_t physical_count = 0;
		if (vkEnumeratePhysicalDevices(context.instance, &physical_count, nullptr) != VK_SUCCESS || physical_count == 0)
		{
			return false;
		}
		std::vector<VkPhysicalDevice> physical_devices(physical_count);
		if (vkEnumeratePhysicalDevices(context.instance, &physical_count, physical_devices.data()) != VK_SUCCESS)
		{
			return false;
		}

		for (const auto physical: physical_devices)
		{
			uint32_t extension_count = 0;
			if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr) != VK_SUCCESS)
			{
				continue;
			}
			std::vector<VkExtensionProperties> extensions(extension_count);
			if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, extensions.data()) != VK_SUCCESS)
			{
				continue;
			}
			const auto has_extension = [&extensions](const char* name)
			{
				return std::any_of(extensions.cbegin(), extensions.cend(),
				                   [name](const auto& extension) { return std::strcmp(extension.extensionName, name) == 0; });
			};
			const char* load_store_op_none_extension = nullptr;
			if (has_extension(VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME))
			{
				load_store_op_none_extension = VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME;
			} else if (has_extension(VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME))
			{
				load_store_op_none_extension = VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME;
			}

			uint32_t family_count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
			std::vector<VkQueueFamilyProperties> families(family_count);
			vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
			for (uint32_t family = 0; family < family_count; ++family)
			{
				if (families[family].queueCount == 0 || (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
				{
					continue;
				}
				constexpr float priority = 1.0f;
				VkDeviceQueueCreateInfo queue_info {};
				queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
				queue_info.queueFamilyIndex = family;
				queue_info.queueCount       = 1;
				queue_info.pQueuePriorities = &priority;
				VkDeviceCreateInfo device_info {};
				device_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
				device_info.queueCreateInfoCount = 1;
				device_info.pQueueCreateInfos    = &queue_info;
				device_info.enabledExtensionCount   = load_store_op_none_extension != nullptr ? 1u : 0u;
				device_info.ppEnabledExtensionNames =
				    load_store_op_none_extension != nullptr ? &load_store_op_none_extension : nullptr;
				if (vkCreateDevice(physical, &device_info, nullptr, &context.device) != VK_SUCCESS)
				{
					continue;
				}

				VkQueue queue = VK_NULL_HANDLE;
				vkGetDeviceQueue(context.device, family, 0, &queue);
				context.physical_device                              = physical;
				context.load_store_op_none_supported                = load_store_op_none_extension != nullptr;
				context.queues[GraphicContext::QUEUE_GFX].vk_queue  = queue;
				context.queues[GraphicContext::QUEUE_GFX].family    = family;
				context.queues[GraphicContext::QUEUE_GFX].index     = 0;
				context.queues[GraphicContext::QUEUE_GFX].mutex     = &context.queue_mutexes[0];
				context.queues[GraphicContext::QUEUE_UTIL].vk_queue = queue;
				context.queues[GraphicContext::QUEUE_UTIL].family   = family;
				context.queues[GraphicContext::QUEUE_UTIL].index    = 0;
				context.queues[GraphicContext::QUEUE_UTIL].mutex    = &context.queue_mutexes[0];
				context.queue_mutex_count                           = 1;
				bound = GraphicsRenderBindContextForTesting(&context);
				return bound;
			}
		}
		return false;
	}

	GraphicContext context {};

private:
	bool bound = false;
};

void VerifyVertexClipProbeVulkanLifecycle()
{
	VulkanSamplerContext vulkan;
	Expect(vulkan.Initialize(), "vertex clip probe Vulkan context must initialize");
	GpuMemoryInit();

	auto* probe_renderer = g_render_ctx->GetVertexClipProbeRenderer();
	Expect(probe_renderer != nullptr && probe_renderer->Init(&vulkan.context),
	       "vertex clip probe renderer must create its dedicated host resources");
	Expect(probe_renderer->GetDescriptorSetLayout() != VK_NULL_HANDLE,
	       "vertex clip probe renderer exposes its dedicated descriptor layout");
	Expect(probe_renderer->Done(&vulkan.context), "vertex clip probe renderer releases its host resources while the device is live");
	Expect(probe_renderer->GetDescriptorSetLayout() == VK_NULL_HANDLE,
	       "vertex clip probe renderer clears its descriptor layout after teardown");
}

VkPipelineLayout CreateVertexClipProbePipelineLayout(VkDevice device, VkDescriptorSetLayout descriptor_set_layout)
{
	if (device == VK_NULL_HANDLE || descriptor_set_layout == VK_NULL_HANDLE)
	{
		return VK_NULL_HANDLE;
	}
	VkPipelineLayoutCreateInfo layout_info {};
	layout_info.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts    = &descriptor_set_layout;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	return vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout) == VK_SUCCESS ? pipeline_layout : VK_NULL_HANDLE;
}

bool CompleteFenceWithoutBlockingSleep(CommandBuffer* buffer)
{
	if (buffer == nullptr)
	{
		return false;
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	for (;;)
	{
		if (buffer->TryCompleteFenceAndResetWithoutLabelCallbacks())
		{
			return true;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return false;
		}
		std::this_thread::yield();
	}
}

void ExpectVertexClipProbeSyntheticEvent(const char* fixed_tests_message = nullptr)
{
	EventRecord events[5] {};
	const uint32_t expected = fixed_tests_message != nullptr ? 5u : 4u;
	const uint32_t first_vs = fixed_tests_message != nullptr ? 1u : 0u;
	Expect(EventRing::Instance().CopySince(0, events, 5) == expected &&
	           (fixed_tests_message == nullptr ||
	            (events[0].kind == EventKind::Info && std::strcmp(events[0].code, "depth_stencil_probe") == 0 &&
	             std::strstr(events[0].message, fixed_tests_message) != nullptr)) &&
	           events[first_vs].kind == EventKind::Info &&
	           std::strcmp(events[first_vs].code, "vs_clip_population") == 0 &&
	           std::strstr(events[first_vs].message,
	                       "cs=0123456789abcdef k=i n=37 s=0 wnp=0 oxy=0 oz01=0 in01=0 ozn=0 inn=0") !=
	               nullptr &&
	           events[first_vs + 1u].kind == EventKind::Info &&
	           std::strcmp(events[first_vs + 1u].code, "vs_clip_probe") == 0 &&
	           std::strstr(events[first_vs + 1u].message, "cs=0123456789abcdef k=i n=37 s=0 inv=0 nf=0 fin=0") != nullptr &&
	           events[first_vs + 2u].kind == EventKind::Info &&
	           std::strcmp(events[first_vs + 2u].code, "vs_param0_probe") == 0 &&
	           std::strstr(events[first_vs + 2u].message, "cs=0123456789abcdef k=i n=37 s=0 p0n=0 p0nf=0 p0fin=0") != nullptr &&
	           events[first_vs + 3u].kind == EventKind::Info &&
	           std::strcmp(events[first_vs + 3u].code, "vs_resolver_probe") == 0 &&
	           std::strstr(events[first_vs + 3u].message, "cs=0123456789abcdef k=i n=37 s=0 c=0") != nullptr,
	       "synthetic no-draw completion emits all selected zero-invocation events newest-first from one fence");
}

struct EmptyProbeRenderPass
{
	VkRenderPass  render_pass = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;

	bool Create(VkDevice device)
	{
		VkSubpassDescription subpass {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		VkRenderPassCreateInfo render_pass_info {};
		render_pass_info.sType        = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		render_pass_info.subpassCount = 1u;
		render_pass_info.pSubpasses   = &subpass;
		if (vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass) != VK_SUCCESS)
		{
			return false;
		}
		VkFramebufferCreateInfo framebuffer_info {};
		framebuffer_info.sType      = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer_info.renderPass = render_pass;
		framebuffer_info.width      = 1u;
		framebuffer_info.height     = 1u;
		framebuffer_info.layers     = 1u;
		return vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer) == VK_SUCCESS;
	}

	void Done(VkDevice device)
	{
		if (framebuffer != VK_NULL_HANDLE)
		{
			vkDestroyFramebuffer(device, framebuffer, nullptr);
			framebuffer = VK_NULL_HANDLE;
		}
		if (render_pass != VK_NULL_HANDLE)
		{
			vkDestroyRenderPass(device, render_pass, nullptr);
			render_pass = VK_NULL_HANDLE;
		}
	}
};

bool FindHostNanRasterMemoryType(VkPhysicalDevice physical_device, uint32_t type_bits, uint32_t* type_index)
{
	if (physical_device == VK_NULL_HANDLE || type_index == nullptr)
	{
		return false;
	}
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((type_bits & (1u << index)) != 0u &&
		    (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u)
		{
			*type_index = index;
			return true;
		}
	}
	return false;
}

class HostNanRasterTarget
{
public:
	static constexpr uint32_t kExtent = 8u;

	explicit HostNanRasterTarget(GraphicContext* context): m_context(context) {}
	~HostNanRasterTarget() { Done(); }

	[[nodiscard]] bool Create()
	{
		if (m_context == nullptr || m_context->device == VK_NULL_HANDLE || m_context->physical_device == VK_NULL_HANDLE)
		{
			return false;
		}

		VkFormatProperties format_properties {};
		vkGetPhysicalDeviceFormatProperties(m_context->physical_device, VK_FORMAT_R8G8B8A8_UNORM, &format_properties);
		if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0u)
		{
			return false;
		}

		VkImageCreateInfo image_info {};
		image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.imageType     = VK_IMAGE_TYPE_2D;
		image_info.format        = VK_FORMAT_R8G8B8A8_UNORM;
		image_info.extent        = {kExtent, kExtent, 1u};
		image_info.mipLevels     = 1u;
		image_info.arrayLayers   = 1u;
		image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(m_context->device, &image_info, nullptr, &image) != VK_SUCCESS)
		{
			return false;
		}

		VkMemoryRequirements requirements {};
		vkGetImageMemoryRequirements(m_context->device, image, &requirements);
		uint32_t memory_type = 0u;
		if (!FindHostNanRasterMemoryType(m_context->physical_device, requirements.memoryTypeBits, &memory_type))
		{
			return false;
		}
		VkMemoryAllocateInfo memory_info {};
		memory_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		memory_info.allocationSize  = requirements.size;
		memory_info.memoryTypeIndex = memory_type;
		if (vkAllocateMemory(m_context->device, &memory_info, nullptr, &memory) != VK_SUCCESS ||
		    vkBindImageMemory(m_context->device, image, memory, 0u) != VK_SUCCESS)
		{
			return false;
		}

		VkImageViewCreateInfo view_info {};
		view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.image                           = image;
		view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format                          = VK_FORMAT_R8G8B8A8_UNORM;
		view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		view_info.subresourceRange.baseMipLevel   = 0u;
		view_info.subresourceRange.levelCount     = 1u;
		view_info.subresourceRange.baseArrayLayer = 0u;
		view_info.subresourceRange.layerCount     = 1u;
		if (vkCreateImageView(m_context->device, &view_info, nullptr, &view) != VK_SUCCESS)
		{
			return false;
		}

		VkAttachmentDescription attachment {};
		attachment.format         = VK_FORMAT_R8G8B8A8_UNORM;
		attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
		attachment.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkAttachmentReference color_attachment {};
		color_attachment.attachment = 0u;
		color_attachment.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkSubpassDescription subpass {};
		subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1u;
		subpass.pColorAttachments    = &color_attachment;
		VkRenderPassCreateInfo render_pass_info {};
		render_pass_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		render_pass_info.attachmentCount = 1u;
		render_pass_info.pAttachments    = &attachment;
		render_pass_info.subpassCount    = 1u;
		render_pass_info.pSubpasses      = &subpass;
		if (vkCreateRenderPass(m_context->device, &render_pass_info, nullptr, &render_pass) != VK_SUCCESS)
		{
			return false;
		}

		VkFramebufferCreateInfo framebuffer_info {};
		framebuffer_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer_info.renderPass      = render_pass;
		framebuffer_info.attachmentCount = 1u;
		framebuffer_info.pAttachments    = &view;
		framebuffer_info.width           = kExtent;
		framebuffer_info.height          = kExtent;
		framebuffer_info.layers          = 1u;
		return vkCreateFramebuffer(m_context->device, &framebuffer_info, nullptr, &framebuffer) == VK_SUCCESS;
	}

	void Done()
	{
		if (m_context == nullptr || m_context->device == VK_NULL_HANDLE)
		{
			return;
		}
		if (framebuffer != VK_NULL_HANDLE)
		{
			vkDestroyFramebuffer(m_context->device, framebuffer, nullptr);
			framebuffer = VK_NULL_HANDLE;
		}
		if (render_pass != VK_NULL_HANDLE)
		{
			vkDestroyRenderPass(m_context->device, render_pass, nullptr);
			render_pass = VK_NULL_HANDLE;
		}
		if (view != VK_NULL_HANDLE)
		{
			vkDestroyImageView(m_context->device, view, nullptr);
			view = VK_NULL_HANDLE;
		}
		if (image != VK_NULL_HANDLE)
		{
			vkDestroyImage(m_context->device, image, nullptr);
			image = VK_NULL_HANDLE;
		}
		if (memory != VK_NULL_HANDLE)
		{
			vkFreeMemory(m_context->device, memory, nullptr);
			memory = VK_NULL_HANDLE;
		}
	}

	VkRenderPass  render_pass = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;

private:
	GraphicContext* m_context = nullptr;
	VkImage         image     = VK_NULL_HANDLE;
	VkDeviceMemory  memory    = VK_NULL_HANDLE;
	VkImageView     view      = VK_NULL_HANDLE;
};

Kyty::Core::String8 HostNanRasterVertexShaderSource(bool nan_position_z)
{
	// 0x7fc00000 is IEEE-754 binary32 quiet NaN. Only gl_Position.z of
	// vertex zero changes between the finite control and the NaN draw.
	Kyty::Core::String8 source = R"spv(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %position %vertex_index
OpDecorate %position BuiltIn Position
OpDecorate %vertex_index BuiltIn VertexIndex
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%v4float = OpTypeVector %float 4
%ptr_Output_v4float = OpTypePointer Output %v4float
%position = OpVariable %ptr_Output_v4float Output
%int = OpTypeInt 32 1
%ptr_Input_int = OpTypePointer Input %int
%vertex_index = OpVariable %ptr_Input_int Input
%uint = OpTypeInt 32 0
%bool = OpTypeBool
%float_n0_75 = OpConstant %float -0.75
%float_0_75 = OpConstant %float 0.75
%float_0 = OpConstant %float 0
%float_1 = OpConstant %float 1
%int_0 = OpConstant %int 0
%int_1 = OpConstant %int 1
%uint_quiet_nan = OpConstant %uint 2143289344
%main = OpFunction %void None %function
%entry = OpLabel
%vertex_index_value = OpLoad %int %vertex_index
%quiet_nan = OpBitcast %float %uint_quiet_nan
%first_position = OpCompositeConstruct %v4float %float_n0_75 %float_n0_75 )spv";
	source += nan_position_z ? "%quiet_nan" : "%float_0";
	source += R"spv( %float_1
%second_position = OpCompositeConstruct %v4float %float_0_75 %float_n0_75 %float_0 %float_1
%third_position = OpCompositeConstruct %v4float %float_0 %float_0_75 %float_0 %float_1
%is_first = OpIEqual %bool %vertex_index_value %int_0
%is_second = OpIEqual %bool %vertex_index_value %int_1
%remaining_position = OpSelect %v4float %is_second %second_position %third_position
%final_position = OpSelect %v4float %is_first %first_position %remaining_position
OpStore %position %final_position
OpReturn
OpFunctionEnd
)spv";
	return source;
}

Kyty::Core::String8 HostNanRasterFragmentShaderSource()
{
	return R"spv(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %color
OpExecutionMode %main OriginUpperLeft
OpDecorate %color Location 0
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%v4float = OpTypeVector %float 4
%ptr_Output_v4float = OpTypePointer Output %v4float
%color = OpVariable %ptr_Output_v4float Output
%float_1 = OpConstant %float 1
%white = OpConstantComposite %v4float %float_1 %float_1 %float_1 %float_1
%main = OpFunction %void None %function
%entry = OpLabel
OpStore %color %white
OpReturn
OpFunctionEnd
)spv";
}

VkShaderModule CreateHostNanRasterShaderModule(VkDevice device, const Kyty::Core::String8& source, const char* message)
{
	if (device == VK_NULL_HANDLE)
	{
		return VK_NULL_HANDLE;
	}
	const auto binary = AssembleValidSpirv(source, message);
	if (binary.empty())
	{
		return VK_NULL_HANDLE;
	}
	VkShaderModuleCreateInfo shader_info {};
	shader_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shader_info.codeSize = binary.size() * sizeof(binary[0]);
	shader_info.pCode    = binary.data();
	VkShaderModule shader = VK_NULL_HANDLE;
	return vkCreateShaderModule(device, &shader_info, nullptr, &shader) == VK_SUCCESS ? shader : VK_NULL_HANDLE;
}

VkPipeline CreateHostNanRasterPipeline(VkDevice device, VkRenderPass render_pass, VkPipelineLayout pipeline_layout,
	                                   VkShaderModule vertex_shader, VkShaderModule fragment_shader)
{
	if (device == VK_NULL_HANDLE || render_pass == VK_NULL_HANDLE || pipeline_layout == VK_NULL_HANDLE ||
	    vertex_shader == VK_NULL_HANDLE || fragment_shader == VK_NULL_HANDLE)
	{
		return VK_NULL_HANDLE;
	}
	VkPipelineShaderStageCreateInfo stages[2] {};
	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex_shader;
	stages[0].pName  = "main";
	stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment_shader;
	stages[1].pName  = "main";

	VkPipelineVertexInputStateCreateInfo vertex_input {};
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	VkPipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkViewport viewport {};
	viewport.width    = static_cast<float>(HostNanRasterTarget::kExtent);
	viewport.height   = static_cast<float>(HostNanRasterTarget::kExtent);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	VkRect2D scissor {};
	scissor.extent = {HostNanRasterTarget::kExtent, HostNanRasterTarget::kExtent};
	VkPipelineViewportStateCreateInfo viewport_state {};
	viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.viewportCount = 1u;
	viewport_state.pViewports    = &viewport;
	viewport_state.scissorCount  = 1u;
	viewport_state.pScissors     = &scissor;
	VkPipelineRasterizationStateCreateInfo rasterization {};
	rasterization.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.depthClampEnable         = VK_FALSE;
	rasterization.rasterizerDiscardEnable  = VK_FALSE;
	rasterization.polygonMode             = VK_POLYGON_MODE_FILL;
	rasterization.cullMode                = VK_CULL_MODE_NONE;
	rasterization.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth               = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample {};
	multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depth_stencil {};
	depth_stencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth_stencil.depthTestEnable  = VK_FALSE;
	depth_stencil.depthWriteEnable = VK_FALSE;
	depth_stencil.depthBoundsTestEnable = VK_FALSE;
	depth_stencil.stencilTestEnable     = VK_FALSE;
	VkPipelineColorBlendAttachmentState color_blend_attachment {};
	color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo color_blend {};
	color_blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	color_blend.attachmentCount = 1u;
	color_blend.pAttachments    = &color_blend_attachment;
	VkGraphicsPipelineCreateInfo pipeline_info {};
	pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.stageCount          = 2u;
	pipeline_info.pStages             = stages;
	pipeline_info.pVertexInputState   = &vertex_input;
	pipeline_info.pInputAssemblyState = &input_assembly;
	pipeline_info.pViewportState      = &viewport_state;
	pipeline_info.pRasterizationState = &rasterization;
	pipeline_info.pMultisampleState   = &multisample;
	pipeline_info.pDepthStencilState  = &depth_stencil;
	pipeline_info.pColorBlendState    = &color_blend;
	pipeline_info.layout               = pipeline_layout;
	pipeline_info.renderPass           = render_pass;
	pipeline_info.subpass              = 0u;
	VkPipeline pipeline = VK_NULL_HANDLE;
	return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1u, &pipeline_info, nullptr, &pipeline) == VK_SUCCESS ? pipeline :
	                                                                                                                   VK_NULL_HANDLE;
}

void VerifyHostNanRasterBaseline()
{
	VulkanSamplerContext vulkan;
	Expect(vulkan.Initialize(), "host NaN raster baseline Vulkan context must initialize");
	GpuMemoryInit();

	HostNanRasterTarget target(&vulkan.context);
	Expect(target.Create(), "host NaN raster baseline creates a color-only render target");
	const auto finite_vertex_shader = CreateHostNanRasterShaderModule(
	    vulkan.context.device, HostNanRasterVertexShaderSource(false), "finite host raster vertex shader assembles and validates");
	const auto nan_vertex_shader = CreateHostNanRasterShaderModule(
	    vulkan.context.device, HostNanRasterVertexShaderSource(true), "NaN host raster vertex shader assembles and validates");
	const auto fragment_shader = CreateHostNanRasterShaderModule(
	    vulkan.context.device, HostNanRasterFragmentShaderSource(), "host raster fragment shader assembles and validates");
	Expect(finite_vertex_shader != VK_NULL_HANDLE && nan_vertex_shader != VK_NULL_HANDLE && fragment_shader != VK_NULL_HANDLE,
	       "host NaN raster baseline creates all shader modules");
	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	Expect(vkCreatePipelineLayout(vulkan.context.device, &pipeline_layout_info, nullptr, &pipeline_layout) == VK_SUCCESS,
	       "host NaN raster baseline creates an empty pipeline layout");
	const auto finite_pipeline = CreateHostNanRasterPipeline(vulkan.context.device, target.render_pass, pipeline_layout,
	                                                         finite_vertex_shader, fragment_shader);
	const auto nan_pipeline = CreateHostNanRasterPipeline(vulkan.context.device, target.render_pass, pipeline_layout,
	                                                      nan_vertex_shader, fragment_shader);
	Expect(finite_pipeline != VK_NULL_HANDLE && nan_pipeline != VK_NULL_HANDLE,
	       "host NaN raster baseline creates finite and quiet-NaN pipelines");
	VkQueryPoolCreateInfo query_pool_info {};
	query_pool_info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	query_pool_info.queryType  = VK_QUERY_TYPE_OCCLUSION;
	query_pool_info.queryCount = 2u;
	VkQueryPool query_pool = VK_NULL_HANDLE;
	Expect(vkCreateQueryPool(vulkan.context.device, &query_pool_info, nullptr, &query_pool) == VK_SUCCESS,
	       "host NaN raster baseline creates bounded occlusion queries");

	struct QueryResult
	{
		uint64_t samples      = 0u;
		uint64_t availability = 0u;
	};
	QueryResult results[2] {};
	{
		CommandBuffer command_buffer(GraphicContext::QUEUE_GFX);
		command_buffer.Begin();
		auto* vk_buffer = command_buffer.GetPool()->buffers[command_buffer.GetIndex()];
		Expect(vk_buffer != VK_NULL_HANDLE, "host NaN raster baseline resolves its exact Vulkan command buffer");
		vkCmdResetQueryPool(vk_buffer, query_pool, 0u, 2u);
		VkRenderPassBeginInfo begin {};
		begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		begin.renderPass        = target.render_pass;
		begin.framebuffer       = target.framebuffer;
		begin.renderArea.extent = {HostNanRasterTarget::kExtent, HostNanRasterTarget::kExtent};
		vkCmdBeginRenderPass(vk_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, finite_pipeline);
		vkCmdBeginQuery(vk_buffer, query_pool, 0u, 0u);
		vkCmdDraw(vk_buffer, 3u, 1u, 0u, 0u);
		vkCmdEndQuery(vk_buffer, query_pool, 0u);
		vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, nan_pipeline);
		vkCmdBeginQuery(vk_buffer, query_pool, 1u, 0u);
		vkCmdDraw(vk_buffer, 3u, 1u, 0u, 0u);
		vkCmdEndQuery(vk_buffer, query_pool, 1u);
		vkCmdEndRenderPass(vk_buffer);
		command_buffer.End();
		command_buffer.Execute();
		Expect(CompleteFenceWithoutBlockingSleep(&command_buffer),
		       "host NaN raster baseline reaches its exact command-buffer fence");
	}
	const auto query_result = vkGetQueryPoolResults(vulkan.context.device, query_pool, 0u, 2u, sizeof(results), results,
	                                                sizeof(results[0]),
	                                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	Expect(query_result == VK_SUCCESS && results[0].availability != 0u && results[1].availability != 0u,
	       "host NaN raster baseline reads both completed occlusion queries after the exact fence");
	const char* const nan_classification = results[1].samples == 0u ? "no-raster-samples" : "rasterized";
	std::fprintf(stderr, "host_nan_raster_baseline finite_samples=%llu nan_samples=%llu nan_class=%s\n",
	             static_cast<unsigned long long>(results[0].samples), static_cast<unsigned long long>(results[1].samples),
	             nan_classification);
	Expect(results[0].samples != 0u, "host NaN raster finite control must produce covered samples");

	vkDestroyQueryPool(vulkan.context.device, query_pool, nullptr);
	vkDestroyPipeline(vulkan.context.device, nan_pipeline, nullptr);
	vkDestroyPipeline(vulkan.context.device, finite_pipeline, nullptr);
	vkDestroyPipelineLayout(vulkan.context.device, pipeline_layout, nullptr);
	vkDestroyShaderModule(vulkan.context.device, fragment_shader, nullptr);
	vkDestroyShaderModule(vulkan.context.device, nan_vertex_shader, nullptr);
	vkDestroyShaderModule(vulkan.context.device, finite_vertex_shader, nullptr);
}

void VerifyVertexClipProbeMatchOrdinal()
{
	VulkanSamplerContext vulkan;
	Expect(vulkan.Initialize(), "match-ordinal probe Vulkan context must initialize");
	GpuMemoryInit();

	auto* probe_renderer = g_render_ctx->GetVertexClipProbeRenderer();
	Expect(probe_renderer != nullptr && probe_renderer->GetDescriptorSetLayout() == VK_NULL_HANDLE,
	       "fresh match-ordinal probe starts without host diagnostic resources");
	CommandBuffer skipped(GraphicContext::QUEUE_GFX);
	Expect(!probe_renderer->Reserve(&vulkan.context, &skipped, 0x0123456789abcdefull, true, 37u, 0u,
	                                0x1111111111111111ull, false, true, true, true, false, false,
	                                ShaderPixelProbeKind::FinalMrtResult, 229u, 1u, 0u) &&
	           probe_renderer->GetDescriptorSetLayout() == VK_NULL_HANDLE,
	       "skipped first match neither consumes the one-shot nor initializes Vulkan resources");

	CommandBuffer selected(GraphicContext::QUEUE_GFX);
	Expect(probe_renderer->Reserve(&vulkan.context, &selected, 0x0123456789abcdefull, true, 37u, 0u,
	                               0x1111111111111111ull, false, true, true, true, false, false,
	                               ShaderPixelProbeKind::FinalMrtResult, 229u, 1u, 0u),
	       "second exact match reserves the requested one-shot occurrence");
	const auto pipeline_layout = CreateVertexClipProbePipelineLayout(vulkan.context.device,
	                                                                 probe_renderer->GetDescriptorSetLayout());
	Expect(pipeline_layout != VK_NULL_HANDLE, "selected match creates the diagnostic descriptor layout");
	EmptyProbeRenderPass pass;
	Expect(pass.Create(vulkan.context.device), "match-ordinal probe creates an empty render pass");

	EventRing::Instance().ResetForTests();
	selected.Begin();
	probe_renderer->Arm(&selected, pipeline_layout);
	auto* vk_buffer = selected.GetPool()->buffers[selected.GetIndex()];
	Expect(vk_buffer != VK_NULL_HANDLE, "match-ordinal probe resolves its Vulkan command buffer");
	VkRenderPassBeginInfo begin {};
	begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass        = pass.render_pass;
	begin.framebuffer       = pass.framebuffer;
	begin.renderArea.extent = {1u, 1u};
	vkCmdBeginRenderPass(vk_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
	probe_renderer->BeginDepthPassQuery(&selected);
	probe_renderer->EndDepthPassQuery(&selected);
	vkCmdEndRenderPass(vk_buffer);
	probe_renderer->Finish(&selected);
	selected.End();
	selected.Execute();
	Expect(CompleteFenceWithoutBlockingSleep(&selected),
	       "selected match completes through its exact command-buffer fence");

	EventRecord events[3] {};
	Expect(EventRing::Instance().CopySince(0, events, 3) == 3u &&
	           std::strcmp(events[0].code, "depth_stencil_probe") == 0 &&
	           std::strstr(events[0].message, "any_passed=0 m=1") != nullptr &&
	           std::strcmp(events[1].code, "ps_mrt_coverage") == 0 &&
	           std::strstr(events[1].message, "mrt=0 ord=229 cfin=0 m=1") != nullptr &&
	           std::strcmp(events[2].code, "ps_mrt_probe") == 0 &&
	           std::strstr(events[2].message, "mrt=0 ord=229 on=0 onf=0 ofin=0 m=1") != nullptr,
	       "selected final-MRT and depth events prove the requested match ordinal");

	pass.Done(vulkan.context.device);
	vkDestroyPipelineLayout(vulkan.context.device, pipeline_layout, nullptr);
	Expect(probe_renderer->Done(&vulkan.context), "match-ordinal probe releases selected host resources");
}

void VerifyVertexClipProbeFenceLifecycle(bool nonblocking)
{
	VulkanSamplerContext vulkan;
	Expect(vulkan.Initialize(), "vertex clip probe fence test Vulkan context must initialize");
	GpuMemoryInit();

	auto* probe_renderer = g_render_ctx->GetVertexClipProbeRenderer();
	Expect(probe_renderer != nullptr && probe_renderer->Init(&vulkan.context),
	       "vertex clip probe fence test initializes dedicated host resources");
	const auto pipeline_layout = CreateVertexClipProbePipelineLayout(vulkan.context.device, probe_renderer->GetDescriptorSetLayout());
	Expect(pipeline_layout != VK_NULL_HANDLE, "vertex clip probe fence test creates a set-zero pipeline layout");

	EventRing::Instance().ResetForTests();
	{
		CommandBuffer selected(GraphicContext::QUEUE_GFX);
		Expect(probe_renderer->Reserve(&vulkan.context, &selected, 0x0123456789abcdefull, true, 37u, 0u),
		       "vertex clip probe fence test reserves its selected command buffer");
		selected.Begin();
		probe_renderer->Arm(&selected, pipeline_layout);
		probe_renderer->Finish(&selected);
		selected.End();
		selected.Execute();

		if (nonblocking)
		{
			CommandBuffer non_owner(GraphicContext::QUEUE_GFX);
			non_owner.Begin();
			non_owner.End();
			non_owner.Execute();
			Expect(CompleteFenceWithoutBlockingSleep(&non_owner),
			       "non-owning command buffer fence completes without consuming the selected probe");
			Expect(EventRing::Instance().Size() == 0u,
			       "non-owning command buffer completion does not emit the selected probe result");
			Expect(CompleteFenceWithoutBlockingSleep(&selected),
			       "selected command buffer completion records the probe result through the nonblocking path");
		} else
		{
			// WaitForFence() drains the production label completion queue. The
			// blocking-only CTest runs in its own process, so initialize that
			// process-lifetime dependency before exercising the real wrapper.
			LabelInit();
			selected.WaitForFence();
		}
		// This test deliberately records no draw. invocations=0 is lifecycle-only
		// evidence and must not be interpreted as instrumented runtime output.
		ExpectVertexClipProbeSyntheticEvent();
	}

	vkDestroyPipelineLayout(vulkan.context.device, pipeline_layout, nullptr);
	Expect(probe_renderer->Done(&vulkan.context),
	       "vertex clip probe fence test releases its descriptor and buffer before context teardown");
}

void VerifySparsePixelSampleProbeLifecycle()
{
	VulkanSamplerContext vulkan;
	Expect(vulkan.Initialize(), "sparse pixel sample probe Vulkan context must initialize");
	GpuMemoryInit();

	auto* probe_renderer = g_render_ctx->GetVertexClipProbeRenderer();
	Expect(probe_renderer != nullptr && probe_renderer->GetDescriptorSetLayout() == VK_NULL_HANDLE,
	       "fresh sparse pixel sample probe starts without host diagnostic resources");
	CommandBuffer unsupported(GraphicContext::QUEUE_GFX);
	Expect(!probe_renderer->Reserve(&vulkan.context, &unsupported, 0x0123456789abcdefull, true, 6u, 0u,
	                                0x1111111111111111ull, false, true, false, false, false, false,
	                                ShaderPixelProbeKind::SampleResult, 15u, 0u, 0u, true) &&
	           probe_renderer->GetDescriptorSetLayout() == VK_NULL_HANDLE,
	       "sparse pixel sample probe preserves the ordinary draw when fragment subgroup operations are unavailable");

	vulkan.context.subgroup_stages     = VK_SHADER_STAGE_FRAGMENT_BIT;
	vulkan.context.subgroup_operations = VK_SUBGROUP_FEATURE_BASIC_BIT;
	CommandBuffer selected(GraphicContext::QUEUE_GFX);
	Expect(probe_renderer->Reserve(&vulkan.context, &selected, 0x0123456789abcdefull, true, 6u, 0u,
	                               0x1111111111111111ull, false, true, false, false, false, false,
	                               ShaderPixelProbeKind::SampleResult, 15u, 0u, 0u, true),
	       "sparse pixel sample probe reserves when fragment subgroup basic operations are available");
	const auto pipeline_layout = CreateVertexClipProbePipelineLayout(vulkan.context.device,
	                                                                 probe_renderer->GetDescriptorSetLayout());
	Expect(pipeline_layout != VK_NULL_HANDLE, "sparse pixel sample probe creates its diagnostic pipeline layout");

	EventRing::Instance().ResetForTests();
	selected.Begin();
	probe_renderer->Arm(&selected, pipeline_layout);
	probe_renderer->Finish(&selected);
	selected.End();
	selected.Execute();
	Expect(CompleteFenceWithoutBlockingSleep(&selected),
	       "sparse pixel sample probe completes through its exact command-buffer fence");

	EventRecord event {};
	Expect(EventRing::Instance().CopySince(0, &event, 1) == 1u && event.kind == EventKind::Info &&
	           std::strcmp(event.code, "ps_sample_probe") == 0 &&
	           std::strstr(event.message, "ord=15 sn=0 snf=0 sfin=0 sparse=1") != nullptr,
	       "sparse pixel sample completion records explicit event provenance");

	vkDestroyPipelineLayout(vulkan.context.device, pipeline_layout, nullptr);
	Expect(probe_renderer->Done(&vulkan.context), "sparse pixel sample probe releases selected host resources");
}

void VerifyVertexClipProbeDepthStencilQuery(bool depth_test_enabled, bool depth_bounds_test_enabled)
{
	VulkanSamplerContext vulkan;
	Expect(vulkan.Initialize(), "vertex clip query Vulkan context must initialize");
	GpuMemoryInit();

	auto* probe_renderer = g_render_ctx->GetVertexClipProbeRenderer();
	Expect(probe_renderer != nullptr && probe_renderer->Init(&vulkan.context),
	       "vertex clip query initializes dedicated host resources");
	const auto pipeline_layout = CreateVertexClipProbePipelineLayout(vulkan.context.device, probe_renderer->GetDescriptorSetLayout());
	Expect(pipeline_layout != VK_NULL_HANDLE, "vertex clip query creates a set-zero pipeline layout");
	EmptyProbeRenderPass pass;
	Expect(pass.Create(vulkan.context.device), "vertex clip query creates an empty render pass");

	EventRing::Instance().ResetForTests();
	{
		CommandBuffer selected(GraphicContext::QUEUE_GFX);
		Expect(probe_renderer->Reserve(&vulkan.context, &selected, 0x0123456789abcdefull, true, 37u, 0u, 0u, true, false,
		                               true, depth_test_enabled, false, depth_bounds_test_enabled),
		       "vertex clip query reserves its selected command buffer");
		selected.Begin();
		probe_renderer->Arm(&selected, pipeline_layout);
		auto* vk_buffer = selected.GetPool()->buffers[selected.GetIndex()];
		Expect(vk_buffer != VK_NULL_HANDLE, "vertex clip query resolves its Vulkan command buffer");
		VkRenderPassBeginInfo begin {};
		begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		begin.renderPass        = pass.render_pass;
		begin.framebuffer       = pass.framebuffer;
		begin.renderArea.extent = {1u, 1u};
		vkCmdBeginRenderPass(vk_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
		probe_renderer->BeginDepthPassQuery(&selected);
		probe_renderer->EndDepthPassQuery(&selected);
		vkCmdEndRenderPass(vk_buffer);
		probe_renderer->Finish(&selected);
		selected.End();
		selected.Execute();
		LabelInit();
		selected.WaitForFence();
		ExpectVertexClipProbeSyntheticEvent(
		    depth_test_enabled
		        ? "applicable=1 depth=1 stencil=0 bounds=0 ready=1 precise=0 any_passed=0"
		        : (depth_bounds_test_enabled
		               ? "applicable=1 depth=0 stencil=0 bounds=1 ready=1 precise=0 any_passed=0"
		               : "applicable=0 depth=0 stencil=0 bounds=0 ready=1 precise=0 any_passed=0"));
	}

	pass.Done(vulkan.context.device);
	vkDestroyPipelineLayout(vulkan.context.device, pipeline_layout, nullptr);
	Expect(probe_renderer->Done(&vulkan.context), "vertex clip query releases resources before context teardown");
}

bool FindDepthSampleMemoryType(VkPhysicalDevice physical_device, uint32_t type_bits, uint32_t* type_index)
{
	if (physical_device == VK_NULL_HANDLE || type_index == nullptr)
	{
		return false;
	}
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((type_bits & (1u << index)) != 0u &&
		    (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u)
		{
			*type_index = index;
			return true;
		}
	}
	return false;
}

class VulkanDepthSample
{
public:
	explicit VulkanDepthSample(GraphicContext* context): m_context(context) {}
	~VulkanDepthSample()
	{
		if (m_context == nullptr || m_context->device == VK_NULL_HANDLE)
		{
			return;
		}
		if (view != VK_NULL_HANDLE)
		{
			vkDestroyImageView(m_context->device, view, nullptr);
		}
		if (image != VK_NULL_HANDLE)
		{
			vkDestroyImage(m_context->device, image, nullptr);
		}
		if (memory != VK_NULL_HANDLE)
		{
			vkFreeMemory(m_context->device, memory, nullptr);
		}
	}

	[[nodiscard]] bool Create()
	{
		if (m_context == nullptr || m_context->device == VK_NULL_HANDLE || m_context->physical_device == VK_NULL_HANDLE)
		{
			return false;
		}
		VkFormatProperties format_properties {};
		vkGetPhysicalDeviceFormatProperties(m_context->physical_device, VK_FORMAT_D32_SFLOAT, &format_properties);
		constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		if ((format_properties.optimalTilingFeatures & required) != required)
		{
			return false;
		}

		VkImageCreateInfo image_info {};
		image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.imageType     = VK_IMAGE_TYPE_2D;
		image_info.format        = VK_FORMAT_D32_SFLOAT;
		image_info.extent        = {4u, 4u, 1u};
		image_info.mipLevels     = 1;
		image_info.arrayLayers   = 1;
		image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(m_context->device, &image_info, nullptr, &image) != VK_SUCCESS)
		{
			return false;
		}

		VkMemoryRequirements requirements {};
		vkGetImageMemoryRequirements(m_context->device, image, &requirements);
		uint32_t memory_type = 0;
		if (!FindDepthSampleMemoryType(m_context->physical_device, requirements.memoryTypeBits, &memory_type))
		{
			return false;
		}
		VkMemoryAllocateInfo memory_info {};
		memory_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		memory_info.allocationSize  = requirements.size;
		memory_info.memoryTypeIndex = memory_type;
		if (vkAllocateMemory(m_context->device, &memory_info, nullptr, &memory) != VK_SUCCESS)
		{
			return false;
		}
		if (vkBindImageMemory(m_context->device, image, memory, 0) != VK_SUCCESS)
		{
			return false;
		}

		VkImageViewCreateInfo view_info {};
		view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.image                           = image;
		view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format                          = VK_FORMAT_D32_SFLOAT;
		view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
		view_info.subresourceRange.baseMipLevel   = 0;
		view_info.subresourceRange.levelCount     = 1;
		view_info.subresourceRange.baseArrayLayer = 0;
		view_info.subresourceRange.layerCount     = 1;
		return vkCreateImageView(m_context->device, &view_info, nullptr, &view) == VK_SUCCESS;
	}

	VkImageView view = VK_NULL_HANDLE;

private:
	GraphicContext* m_context = nullptr;
	VkImage         image     = VK_NULL_HANDLE;
	VkDeviceMemory  memory    = VK_NULL_HANDLE;
};

class VulkanDepthReadOnlyFramebuffer
{
public:
	explicit VulkanDepthReadOnlyFramebuffer(VkDevice device): m_device(device) {}
	~VulkanDepthReadOnlyFramebuffer()
	{
		if (framebuffer != VK_NULL_HANDLE)
		{
			vkDestroyFramebuffer(m_device, framebuffer, nullptr);
		}
		if (render_pass != VK_NULL_HANDLE)
		{
			vkDestroyRenderPass(m_device, render_pass, nullptr);
		}
	}

	[[nodiscard]] bool Create(VkImageView depth_view)
	{
		if (m_device == VK_NULL_HANDLE || depth_view == VK_NULL_HANDLE)
		{
			return false;
		}
		VkAttachmentDescription attachment {};
		attachment.format         = VK_FORMAT_D32_SFLOAT;
		attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachment.storeOp        = VulkanAttachmentStoreOpNone();
		attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VulkanAttachmentStoreOpNone();
		attachment.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		attachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

		VkAttachmentReference depth_reference {};
		depth_reference.attachment = 0;
		depth_reference.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		VkSubpassDescription subpass {};
		subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.pDepthStencilAttachment = &depth_reference;
		VkRenderPassCreateInfo render_pass_info {};
		render_pass_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		render_pass_info.attachmentCount = 1;
		render_pass_info.pAttachments    = &attachment;
		render_pass_info.subpassCount    = 1;
		render_pass_info.pSubpasses      = &subpass;
		if (vkCreateRenderPass(m_device, &render_pass_info, nullptr, &render_pass) != VK_SUCCESS)
		{
			return false;
		}

		VkFramebufferCreateInfo framebuffer_info {};
		framebuffer_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer_info.renderPass      = render_pass;
		framebuffer_info.attachmentCount = 1;
		framebuffer_info.pAttachments    = &depth_view;
		framebuffer_info.width           = 4;
		framebuffer_info.height          = 4;
		framebuffer_info.layers          = 1;
		return vkCreateFramebuffer(m_device, &framebuffer_info, nullptr, &framebuffer) == VK_SUCCESS;
	}

private:
	VkDevice      m_device    = VK_NULL_HANDLE;
	VkRenderPass  render_pass = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
};

class VulkanDepthDescriptorBinding
{
public:
	explicit VulkanDepthDescriptorBinding(VkDevice device): m_device(device) {}
	~VulkanDepthDescriptorBinding()
	{
		if (pool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(m_device, pool, nullptr);
		}
		if (layout != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(m_device, layout, nullptr);
		}
	}

	[[nodiscard]] bool Create(VkImageView depth_view, VkSampler comparison_sampler)
	{
		if (m_device == VK_NULL_HANDLE || depth_view == VK_NULL_HANDLE || comparison_sampler == VK_NULL_HANDLE)
		{
			return false;
		}
		VkDescriptorSetLayoutBinding bindings[2] {};
		bindings[0].binding         = 0;
		bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		bindings[0].descriptorCount = 1;
		bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
		bindings[1].binding         = 1;
		bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
		bindings[1].descriptorCount = 1;
		bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
		VkDescriptorSetLayoutCreateInfo layout_info {};
		layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layout_info.bindingCount = 2;
		layout_info.pBindings    = bindings;
		if (vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr, &layout) != VK_SUCCESS)
		{
			return false;
		}

		VkDescriptorPoolSize pool_sizes[2] {};
		pool_sizes[0] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1};
		pool_sizes[1] = {VK_DESCRIPTOR_TYPE_SAMPLER, 1};
		VkDescriptorPoolCreateInfo pool_info {};
		pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pool_info.maxSets       = 1;
		pool_info.poolSizeCount = 2;
		pool_info.pPoolSizes    = pool_sizes;
		if (vkCreateDescriptorPool(m_device, &pool_info, nullptr, &pool) != VK_SUCCESS)
		{
			return false;
		}

		VkDescriptorSetAllocateInfo allocation {};
		allocation.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocation.descriptorPool     = pool;
		allocation.descriptorSetCount = 1;
		allocation.pSetLayouts        = &layout;
		if (vkAllocateDescriptorSets(m_device, &allocation, &set) != VK_SUCCESS)
		{
			return false;
		}

		VkDescriptorImageInfo image_info {};
		image_info.imageView   = depth_view;
		image_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		VkDescriptorImageInfo sampler_info {};
		sampler_info.sampler = comparison_sampler;
		VkWriteDescriptorSet writes[2] {};
		writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet          = set;
		writes[0].dstBinding      = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[0].pImageInfo      = &image_info;
		writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet          = set;
		writes[1].dstBinding      = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
		writes[1].pImageInfo      = &sampler_info;
		vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
		return true;
	}

private:
	VkDevice              m_device = VK_NULL_HANDLE;
	VkDescriptorSetLayout layout   = VK_NULL_HANDLE;
	VkDescriptorPool      pool     = VK_NULL_HANDLE;
	VkDescriptorSet       set      = VK_NULL_HANDLE;
};

void VerifyEventWriteExecutionContract()
{
	VulkanSamplerContext vulkan;
	Expect(vulkan.Initialize(), "EVENT_WRITE integration context must initialize");
	GpuMemoryInit();

	constexpr uint64_t untouched = 0x1122334455667788ull;
	constexpr uint64_t ready     = 1ull << 63u;
	alignas(16) std::array<std::array<uint64_t, 2>, 16> results {};
	for (auto& pair: results)
	{
		pair.fill(untouched);
	}
	const auto result_address = reinterpret_cast<uint64_t>(results.data());
	GpuMemorySetAllocatedRange(result_address, sizeof(results));

	GpuSubmissionCoordinator coordinator;
	// Runtime command processors are process-lifetime objects; their destructor
	// is intentionally unavailable, so this process-isolated integration uses
	// the same lifetime.
	auto* processor = new CommandProcessor(&coordinator, GraphicContext::QUEUE_GFX);
	GpuWriteHistoryConfigureForTesting(result_address, sizeof(results));
	const uint32_t write_data_value = 0xa5a55a5au;
	processor->WriteData(reinterpret_cast<uint32_t*>(result_address), &write_data_value, 1u, 0x04100500u, false, false);
	GpuWriteHistorySnapshot write_data_history {};
	Expect(GpuWriteHistoryQuery(result_address, sizeof(uint32_t), &write_data_history) && write_data_history.covers_query &&
	           write_data_history.entry_count == 1u &&
	           write_data_history.entries[0].kind == static_cast<uint32_t>(GpuWriteHistoryKind::WriteData),
	       "real CommandProcessor WriteData publishes bounded temporal provenance after its memcpy");
	GpuWriteHistoryConfigureForTesting(0u, 0u);
	results[0][0] = untouched;

	alignas(16) std::array<uint32_t, 4> const_ram_source {0x10203040u, 0x50607080u, 0x90a0b0c0u, 0xd0e0f000u};
	alignas(16) std::array<uint32_t, 4> const_ram_result {};
	GpuMemorySetAllocatedRange(reinterpret_cast<uint64_t>(const_ram_result.data()), sizeof(const_ram_result));
	processor->WriteConstRam(0u, const_ram_source.data(), static_cast<uint32_t>(const_ram_source.size()));
	GpuWriteHistoryConfigureForTesting(reinterpret_cast<uint64_t>(const_ram_result.data()), sizeof(const_ram_result));
	processor->DumpConstRam(const_ram_result.data(), 0u, static_cast<uint32_t>(const_ram_result.size()));
	GpuWriteHistorySnapshot const_ram_history {};
	Expect(const_ram_result == const_ram_source &&
	           GpuWriteHistoryQuery(reinterpret_cast<uint64_t>(const_ram_result.data()), sizeof(const_ram_result),
	                                &const_ram_history) &&
	           const_ram_history.covers_query && const_ram_history.entry_count == 1u &&
	           const_ram_history.entries[0].kind == static_cast<uint32_t>(GpuWriteHistoryKind::ConstRamDump),
	       "real DUMP_CONST_RAM publishes bounded temporal provenance after its memcpy");
	GpuWriteHistoryConfigureForTesting(0u, 0u);

	const auto execute = [&](uint64_t address)
	{
		std::array<uint32_t, 6> stream {
		    KYTY_PM4(4, Pm4::IT_EVENT_WRITE, 0u),
		    0x139u,
		    static_cast<uint32_t>(address),
		    static_cast<uint32_t>(address >> 32u),
		    KYTY_PM4(2, Pm4::IT_EVENT_WRITE, 0u),
		    0x07u,
		};
		const auto consumed = cp_op_event_write(processor, stream[0], stream.data() + 1, stream.size(), stream.size());
		Expect(consumed == 3u, "four-dword EVENT_WRITE consumes exactly its three body dwords");
		Expect(stream[consumed + 1u] == KYTY_PM4(2, Pm4::IT_EVENT_WRITE, 0u),
		       "EVENT_WRITE parser leaves the following packet aligned");
	};

	GpuWriteHistoryConfigureForTesting(result_address, sizeof(results));
	execute(result_address);
	GpuWriteHistorySnapshot event_write_history {};
	Expect(GpuWriteHistoryQuery(result_address, sizeof(results), &event_write_history) && event_write_history.covers_query &&
	           event_write_history.entry_count == 1u &&
	           event_write_history.entries[0].kind == static_cast<uint32_t>(GpuWriteHistoryKind::EventWrite),
	       "real addressed EVENT_WRITE publishes bounded temporal provenance after its result writes");
	GpuWriteHistoryConfigureForTesting(0u, 0u);
	for (const auto& pair: results)
	{
		Expect(pair[0] == ready && pair[1] == untouched, "first occlusion dump writes only the selected DB lane");
	}

	execute(result_address + sizeof(uint64_t));
	uint64_t visible_samples = 0;
	for (const auto& pair: results)
	{
		Expect(pair[0] == ready && pair[1] == (ready | 1u), "second occlusion dump publishes the incremented ready counter");
		visible_samples += pair[1] - pair[0];
	}
	Expect(visible_samples == results.size(), "synthetic occlusion result keeps every DB visible");

	alignas(16) std::array<uint64_t, 32> invalid {};
	invalid.fill(untouched);
	const auto invalid_address = reinterpret_cast<uint64_t>(invalid.data());
	GpuMemorySetAllocatedRange(invalid_address, sizeof(invalid));
	execute(invalid_address + 1u);
	for (const auto value: invalid)
	{
		Expect(value == untouched, "misaligned occlusion destination remains untouched");
	}
}

void VerifyComparisonSamplerCacheIdentity()
{
	VulkanSamplerContext vulkan;
	Expect(vulkan.Initialize(), "Vulkan sampler context must initialize");
	VerifyDepthStencilAttachmentAccess(vulkan.context.load_store_op_none_supported);
	ShaderSamplerResource descriptor {};
	const auto regular      = g_render_ctx->GetSamplerCache()->GetSamplerId(descriptor, State::ImageSampleOperation::Regular);
	const auto depth        = g_render_ctx->GetSamplerCache()->GetSamplerId(descriptor, State::ImageSampleOperation::DepthReference);
	const auto depth_reused = g_render_ctx->GetSamplerCache()->GetSamplerId(descriptor, State::ImageSampleOperation::DepthReference);
	Expect(regular != depth, "regular and depth-reference samplers must not alias");
	Expect(depth == depth_reused, "identical depth-reference samplers must reuse the cache entry");
	Expect(g_render_ctx->GetSamplerCache()->GetSampler(regular) != VK_NULL_HANDLE, "regular sampler must be created");
	const auto comparison_sampler = g_render_ctx->GetSamplerCache()->GetSampler(depth);
	Expect(comparison_sampler != VK_NULL_HANDLE, "comparison sampler must be created");

	VulkanDepthSample depth_sample(&vulkan.context);
	Expect(depth_sample.Create(), "sampled depth image and depth-aspect view must be created");
	VulkanDepthDescriptorBinding binding(vulkan.context.device);
	Expect(binding.Create(depth_sample.view, comparison_sampler), "depth view and comparison sampler descriptor update must succeed");
	if (vulkan.context.load_store_op_none_supported)
	{
		VulkanDepthReadOnlyFramebuffer framebuffer(vulkan.context.device);
		Expect(framebuffer.Create(depth_sample.view), "read-only sampled depth framebuffer must use store-op-none");
	}
}

} // namespace

int main(int argc, char** argv)
{
	InitializeGraphicsConfig();
	VerifyRenderTargetLifetimeAgentArmServerPublication();
	VerifyRenderTargetLifetimeAgentArmGate();
	VerifyRenderTargetLifetimeDepthFilter();
	if (argc == 2 && std::strcmp(argv[1], "--host-nan-raster-baseline-only") == 0)
	{
		VerifyHostNanRasterBaseline();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--render-target-index-alias-only") == 0)
	{
		VerifyRenderTargetIndexAliasContract();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--vertex-clip-probe-contract-only") == 0)
	{
		VerifyVertexClipProbeContract();
		VerifyVertexClipProbeFenceLifecycle(false);
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--vertex-clip-probe-sparse-only") == 0)
	{
		VerifySparsePixelSampleProbeLifecycle();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--vertex-clip-probe-blocking-only") == 0)
	{
		VerifyVertexClipProbeFenceLifecycle(false);
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--vertex-clip-probe-nonblocking-only") == 0)
	{
		VerifyVertexClipProbeFenceLifecycle(true);
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--vertex-clip-probe-depth-query-only") == 0)
	{
		VerifyVertexClipProbeDepthStencilQuery(true, false);
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--vertex-clip-probe-match-ordinal-only") == 0)
	{
		VerifyVertexClipProbeMatchOrdinal();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--vertex-clip-probe-color-only") == 0)
	{
		VerifyVertexClipProbeDepthStencilQuery(false, false);
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--vertex-clip-probe-depth-bounds-only") == 0)
	{
		VerifyVertexClipProbeDepthStencilQuery(false, true);
		return 0;
	}
	VerifyTextureBlockDumpSpecContract();
	if (argc == 2 && std::strcmp(argv[1], "--texture-block-dump-spec-only") == 0)
	{
		return 0;
	}
	VerifyEventWritePacketContract();
	if (argc == 2 && std::strcmp(argv[1], "--event-write-only") == 0)
	{
		VerifyEventWriteExecutionContract();
		return 0;
	}
	VerifyGuestReadVisitSerializesProtection();
	VerifyImageCopyNormalization();
	VerifyBoundedShaderDecode();
	VerifyScalarConditionalMoves();
	VerifyFusedShaderUsesEffectiveBackEntry();
	VerifyComparisonSamplerCacheIdentity();
	VerifyStencilFrontier();
	VerifyStorageFrontier();
	VerifyStorageRange();
	VerifyStorageUnknownReasonResolution();
	VerifyRenderColorArrayBackingGrouping();
	VerifyStorageConsumerAnalysis();
	VerifyUnusedMetadataExclusionPreservesActiveOrdering();
	VerifyResidualStencilPm4Boundary();
	VerifyActiveStencilPm4BoundaryRejectsMissingBases();
	VerifyRawGen5StorageDescriptorContract();
	VerifyUnsignedExecLessThanComparison();
	VerifyFloatExecNotLessEqualComparison();
	VerifyGen5FloatExecNotLessThanSdwa();
	VerifyUnsignedByteBufferLoad();
	VerifyGen5BufferLoadDwordIdxen();
	VerifyGen5BufferLoadDwordOffenIdxen();
	VerifyGen5UnsignedSub();
	VerifyGen5AddCarryIn();
	VerifyGen5XnorVop2();
	VerifyGen5BitCountVop3();
	VerifyGen5ShiftLeftOrVop3();
	VerifyGen5AndOrVop3();
	VerifyGen5UnsignedMinEncodings();
	VerifyGen5InverseTwoPiInlineConstant();
	VerifyGen5UnsignedMad64Vop3b();
	VerifyGen5ReciprocalIFlag();
	VerifyGen5ReciprocalIFlagExceptionalInputs();
	VerifyGen5ImageSampleLzDmask3();
	VerifyGen5ImageSampleLzDmask1();
	VerifyGen5DepthReferenceSample();
	VerifyGen5SAndn1SaveexecB64();
	return 0;
}
