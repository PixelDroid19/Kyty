#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Agent/AgentLifecycle.h"
#include "Emulator/Agent/EventRing.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"

#include "../../emulator/src/Graphics/GraphicsRenderInternal.h"
#include "Emulator/Log.h"

#include "spirv-tools/libspirv.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
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
	Expect(evidence.access == ShaderStorageAccess::UnusedMetadata, "unused direct resource classification");
	Expect(evidence.reason == ShaderStorageUnknownReason::None, "unused direct resource is not unknown");
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
	ShaderCalcBindingIndices(&input.bind);
	return SpirvGenerateSource(code, nullptr, &input, nullptr);
}

void VerifyGen5DepthReferenceSample()
{
	const auto flat = GenerateGen5DepthReferenceSample(1u, 9u, 1, 0);
	Expect(flat.FindIndex("OpImageSampleDrefExplicitLod %float") != Kyty::Core::STRING8_INVALID_INDEX,
	       "2D depth-reference sample uses the comparison instruction");
	Expect(flat.FindIndex("OpCompositeConstruct %v2float") != Kyty::Core::STRING8_INVALID_INDEX,
	       "2D depth-reference sample uses two coordinates");
	ExpectValidSpirv(flat, "2D depth-reference sample emits valid SPIR-V");

	const auto arrayed = GenerateGen5DepthReferenceSample(5u, 13u, 0, 1);
	Expect(arrayed.FindIndex("OpImageSampleDrefExplicitLod %float") != Kyty::Core::STRING8_INVALID_INDEX,
	       "array depth-reference sample uses the comparison instruction");
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
	subsystems->Add(CoreSubsystem::Instance(), {});
	subsystems->Add(ConfigSubsystem::Instance(), {CoreSubsystem::Instance()});
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
				context.load_store_op_none_supported                 = load_store_op_none_extension != nullptr;
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

int main()
{
	InitializeGraphicsConfig();
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
