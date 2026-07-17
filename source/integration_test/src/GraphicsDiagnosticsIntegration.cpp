#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Agent/AgentLifecycle.h"
#include "Emulator/Agent/EventRing.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

void VerifyStorageFrontier()
{
	EventRing::Instance().ResetForTests();
	Lifecycle::StorageFrontierContext context {};
	context.access          = Lifecycle::StorageAccessClass::Mixed;
	context.source          = Lifecycle::StorageBindingSource::Metadata;
	context.unknown_reason  = Lifecycle::StorageUnknownReason::RegisterBaseMismatch;
	context.code_available  = true;
	context.exact_match     = false;
	context.unbased_match   = true;
	context.decoded_unknown = false;
	context.indirect_use    = true;
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
	Expect(std::strstr(event.message, "access=mixed") != nullptr, "access class serialized");
	Expect(std::strstr(event.message, "source=metadata reason=register_base_mismatch") != nullptr,
	       "binding source and unknown reason serialized");
	Expect(std::strstr(event.message, "code=1 exact=0 unbased=1") != nullptr, "instruction availability and register matches serialized");
	Expect(std::strstr(event.message, "decoded=0 indirect=1") != nullptr, "decode completeness and indirect use serialized");
	Expect(std::strstr(event.message, "idx=15 sgpr=31 slot=65535") != nullptr, "resource identity serialized");
	Expect(std::strstr(event.message, "usage=3 stride=16383 fmt=127") != nullptr, "descriptor geometry serialized");
	Expect(std::strstr(event.message, "dst=0xfff tid=1 swz=1") != nullptr, "descriptor controls serialized");
	Expect(std::strlen(event.message) < kAgentEventMessageMax, "storage message bounded");
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
	Expect(evidence.reason == ShaderStorageUnknownReason::NoMatchingInstruction, "no matching instruction reason");
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
	Expect(!ShaderGen5StorageDescriptorSupported(resource, ShaderStorageAccess::Raw), "raw stride must preserve dword alignment");
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
	Expect(load.src[2].type == ShaderOperandType::LiteralConstant && load.src[2].constant.u == 3u,
	       "byte buffer load byte offset preserved");

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
	Expect(source.FindIndex("%uint_0x000000ff") != Kyty::Core::STRING8_INVALID_INDEX, "byte load zero-extension mask is 0xff");
	Expect(source.FindIndex("OpBitcast %float") != Kyty::Core::STRING8_INVALID_INDEX, "unsigned byte result is stored as VGPR bits");
	Expect(source.FindIndex("%buffer_load_float1") == Kyty::Core::STRING8_INVALID_INDEX,
	       "unsigned byte load does not coerce to dword float loading");
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
	Expect(load.src[2].type == ShaderOperandType::LiteralConstant && load.src[2].constant.u == 0x38u,
	       "idxen buffer load preserves the instruction byte offset");

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
	Expect(load.src[2].type == ShaderOperandType::LiteralConstant && load.src[2].constant.u == 0x90u,
	       "offen buffer load preserves the instruction byte offset");

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
	Expect(source.FindIndex("OpAccessChain %_ptr_Function_float %temp_v4float %uint_0") != Kyty::Core::STRING8_INVALID_INDEX,
	       "dmask 1 extracts component R");
	Expect(source.FindIndex("OpAccessChain %_ptr_Function_float %temp_v4float %uint_1") == Kyty::Core::STRING8_INVALID_INDEX,
	       "dmask 1 does not materialize component G");
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

} // namespace

int main()
{
	InitializeGraphicsConfig();
	VerifyStencilFrontier();
	VerifyStorageFrontier();
	VerifyStorageUnknownReasonResolution();
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
	VerifyGen5ReciprocalIFlag();
	VerifyGen5ReciprocalIFlagExceptionalInputs();
	VerifyGen5ImageSampleLzDmask1();
	VerifyGen5SAndn1SaveexecB64();
	return 0;
}
