#include "Emulator/Graphics/Shader.h"
#include "ShaderDebugInternal.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String8.h"

#include "Emulator/Log.h"

#include <algorithm>
#include <cinttypes>

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics {

static String8 operand_to_str(ShaderOperand op)
{
	String8 ret = "???";
	switch (op.type)
	{
		case ShaderOperandType::LiteralConstant:
			EXIT_IF(op.size != 0);
			EXIT_IF(op.negate || op.absolute);
			return String8::FromPrintf("%f (%u)", op.constant.f, op.constant.u);
			break;
		case ShaderOperandType::IntegerInlineConstant:
			EXIT_IF(op.size != 0);
			EXIT_IF(op.negate || op.absolute);
			return String8::FromPrintf("%d", op.constant.i);
			break;
		case ShaderOperandType::FloatInlineConstant:
			EXIT_IF(op.size != 0);
			EXIT_IF(op.negate || op.absolute);
			return String8::FromPrintf("%f", op.constant.f);
			break;
		default: break;
	}

	EXIT_IF(op.size != 1);

	switch (op.type)
	{
		case ShaderOperandType::VccHi: ret = "vcc_hi"; break;
		case ShaderOperandType::VccZ: ret = "vccz"; break;
		case ShaderOperandType::VccLo: ret = "vcc_lo"; break;
		case ShaderOperandType::ExecHi: ret = "exec_hi"; break;
		case ShaderOperandType::ExecLo: ret = "exec_lo"; break;
		case ShaderOperandType::ExecZ: ret = "execz"; break;
		case ShaderOperandType::Scc: ret = "scc"; break;
		case ShaderOperandType::M0: ret = "m0"; break;
		case ShaderOperandType::Vgpr: ret = String8::FromPrintf("v%d", op.register_id); break;
		case ShaderOperandType::Sgpr: ret = String8::FromPrintf("s%d", op.register_id); break;
		case ShaderOperandType::Null: ret = "null"; break;
		default: break;
	}

	if (op.absolute)
	{
		ret = "abs(" + ret + ")";
	}

	if (op.negate)
	{
		return "-" + ret;
	}

	return ret;
}

static String8 operand_array_to_str(ShaderOperand op, int n)
{
	String8 ret = "???";

	EXIT_IF(op.size != n);

	switch (op.type)
	{
		case ShaderOperandType::VccLo:
			if (n == 2)
			{
				ret = "vcc";
			}
			break;
		case ShaderOperandType::ExecLo:
			if (n == 2)
			{
				ret = "exec";
			}
			break;
		case ShaderOperandType::Sgpr: ret = String8::FromPrintf("s[%d:%d]", op.register_id, op.register_id + n - 1); break;
		case ShaderOperandType::Vgpr: ret = String8::FromPrintf("v[%d:%d]", op.register_id, op.register_id + n - 1); break;
		case ShaderOperandType::LiteralConstant:
			if (n == 2)
			{
				ret = String8::FromPrintf("%f (%u)", op.constant.f, op.constant.u);
			}
			break;
		case ShaderOperandType::IntegerInlineConstant:
			if (n == 2)
			{
				ret = String8::FromPrintf("%d", op.constant.i);
			}
			break;
		default: break;
	}

	EXIT_IF(ret == "???");

	if (op.absolute)
	{
		ret = "abs(" + ret + ")";
	}

	if (op.negate)
	{
		return "-" + ret;
	}

	return ret;
}

static String8 dbg_fmt_to_str(const ShaderInstruction& inst)
{
	switch (inst.format)
	{
		case ShaderInstructionFormat::Unknown: return "Unknown"; break;
		case ShaderInstructionFormat::Empty: return "Empty"; break;
		case ShaderInstructionFormat::Imm: return "Imm"; break;
		case ShaderInstructionFormat::Mrt0OffOffComprVmDone: return "Mrt0OffOffComprVmDone"; break;
		case ShaderInstructionFormat::Mrt1OffOffComprVmDone: return "Mrt1OffOffComprVmDone"; break;
		case ShaderInstructionFormat::Mrt2OffOffComprVmDone: return "Mrt2OffOffComprVmDone"; break;
		case ShaderInstructionFormat::Mrt3OffOffComprVmDone: return "Mrt3OffOffComprVmDone"; break;
		case ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone: return "Mrt0Vsrc0Vsrc1ComprVmDone"; break;
		case ShaderInstructionFormat::Mrt1Vsrc0Vsrc1ComprVm: return "Mrt1Vsrc0Vsrc1ComprVm"; break;
		case ShaderInstructionFormat::Mrt2Vsrc0Vsrc1ComprVm: return "Mrt2Vsrc0Vsrc1ComprVm"; break;
		case ShaderInstructionFormat::Mrt3Vsrc0Vsrc1ComprVm: return "Mrt3Vsrc0Vsrc1ComprVm"; break;
		case ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone: return "Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone"; break;
		case ShaderInstructionFormat::Mrt1Vsrc0Vsrc1Vsrc2Vsrc3Vm: return "Mrt1Vsrc0Vsrc1Vsrc2Vsrc3Vm"; break;
		case ShaderInstructionFormat::Mrt2Vsrc0Vsrc1Vsrc2Vsrc3Vm: return "Mrt2Vsrc0Vsrc1Vsrc2Vsrc3Vm"; break;
		case ShaderInstructionFormat::Mrt3Vsrc0Vsrc1Vsrc2Vsrc3Vm: return "Mrt3Vsrc0Vsrc1Vsrc2Vsrc3Vm"; break;
		case ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3: return "Param0Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3: return "Param1Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3: return "Param2Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3: return "Param3Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3: return "Param4Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param5Vsrc0Vsrc1Vsrc2Vsrc3: return "Param5Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param6Vsrc0Vsrc1Vsrc2Vsrc3: return "Param6Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param7Vsrc0Vsrc1Vsrc2Vsrc3: return "Param7Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done: return "Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done"; break;
		case ShaderInstructionFormat::PrimVsrc0OffOffOffDone: return "PrimVsrc0OffOffOffDone"; break;
		case ShaderInstructionFormat::Saddr: return "Saddr"; break;
		case ShaderInstructionFormat::SdstSbaseSoffset: return "SdstSbaseSoffset"; break;
		case ShaderInstructionFormat::Sdst4SbaseSoffset: return "Sdst4SbaseSoffset"; break;
		case ShaderInstructionFormat::Sdst8SbaseSoffset: return "Sdst8SbaseSoffset"; break;
		case ShaderInstructionFormat::SdstSvSoffset: return "SdstSvSoffset"; break;
		case ShaderInstructionFormat::Sdst2SvSoffset: return "Sdst2SvSoffset"; break;
		case ShaderInstructionFormat::Sdst4SvSoffset: return "Sdst4SvSoffset"; break;
		case ShaderInstructionFormat::Sdst8SvSoffset: return "Sdst8SvSoffset"; break;
		case ShaderInstructionFormat::Sdst16SvSoffset: return "Sdst16SvSoffset"; break;
		case ShaderInstructionFormat::SVdstSVsrc0: return "SVdstSVsrc0"; break;
		case ShaderInstructionFormat::Sdst2Ssrc02: return "Sdst2Ssrc02"; break;
		case ShaderInstructionFormat::Sdst2Ssrc02Ssrc1: return "Sdst2Ssrc02Ssrc1"; break;
		case ShaderInstructionFormat::Sdst2Ssrc02Ssrc12: return "Sdst2Ssrc02Ssrc12"; break;
		case ShaderInstructionFormat::SmaskVsrc0Vsrc1: return "SmaskVsrc0Vsrc1"; break;
		case ShaderInstructionFormat::Ssrc0Ssrc1: return "Ssrc0Ssrc1"; break;
		case ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen: return "Vdata1VaddrSvSoffsIdxen"; break;
		case ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxenFloat1: return "Vdata1VaddrSvSoffsIdxenFloat1"; break;
		case ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen: return "Vdata2VaddrSvSoffsIdxen"; break;
		case ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxenFloat2: return "Vdata2VaddrSvSoffsIdxenFloat2"; break;
		case ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen: return "Vdata3VaddrSvSoffsIdxen"; break;
		case ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen: return "Vdata4VaddrSvSoffsIdxen"; break;
		case ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxenFloat4: return "Vdata4VaddrSvSoffsIdxenFloat4"; break;
		case ShaderInstructionFormat::Vdata4Vaddr2SvSoffsOffenIdxenFloat4: return "Vdata4Vaddr2SvSoffsOffenIdxenFloat4"; break;
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1: return "Vdata1Vaddr3StSsDmask1"; break;
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2: return "Vdata1Vaddr3StSsDmask2"; break;
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4: return "Vdata1Vaddr3StSsDmask4"; break;
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask8: return "Vdata1Vaddr3StSsDmask8"; break;
		case ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3: return "Vdata2Vaddr3StSsDmask3"; break;
		case ShaderInstructionFormat::Vdata2Vaddr3StSsDmask5: return "Vdata2Vaddr3StSsDmask5"; break;
		case ShaderInstructionFormat::Vdata2Vaddr3StSsDmask9: return "Vdata2Vaddr3StSsDmask9"; break;
		case ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7: return "Vdata3Vaddr3StSsDmask7"; break;
		case ShaderInstructionFormat::Vdata3Vaddr3StSsDmaskB: return "Vdata3Vaddr3StSsDmaskB"; break;
		case ShaderInstructionFormat::Vdata3Vaddr4StSsDmask7: return "Vdata3Vaddr4StSsDmask7"; break;
		case ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF: return "Vdata4Vaddr3StSsDmaskF"; break;
		case ShaderInstructionFormat::Vdata4Vaddr3StSsMimgDmask: return "Vdata4Vaddr3StSsMimgDmask"; break;
		case ShaderInstructionFormat::Vdata4Vaddr3StDmaskF: return "Vdata4Vaddr3StDmaskF"; break;
		case ShaderInstructionFormat::Vdata4Vaddr4StDmaskF: return "Vdata4Vaddr4StDmaskF"; break;
		case ShaderInstructionFormat::VdataVaddrStDmask: return "VdataVaddrStDmask"; break;
		case ShaderInstructionFormat::VdataVaddr3StDmask: return "VdataVaddr3StDmask"; break;
		case ShaderInstructionFormat::SVdstSVsrc0SVsrc1: return "SVdstSVsrc0SVsrc1"; break;
		case ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2: return "VdstVsrc0Vsrc1Smask2"; break;
		case ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2: return "VdstVsrc0Vsrc1Vsrc2"; break;
		case ShaderInstructionFormat::VdstVsrcAttrChan: return "VdstVsrcAttrChan"; break;
		case ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1: return "VdstSdst2Vsrc0Vsrc1"; break;
		case ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2: return "VdstSdst2Vsrc0Vsrc1Ssrc2A2"; break;
		case ShaderInstructionFormat::VdstGds: return "VdstGds"; break;
		case ShaderInstructionFormat::Vdst2VaddrOffset01: return "Vdst2VaddrOffset01"; break;
		case ShaderInstructionFormat::Label: return "Label"; break;
		default: return "????"; break;
	}
}

static String8 dbg_fmt_print(const ShaderInstruction& inst)
{
	uint64_t f = inst.format;
	EXIT_IF(f == ShaderInstructionFormat::Unknown);
	String8 str;
	if (f == ShaderInstructionFormat::Empty)
	{
		return str;
	}
	int src_num = 0;
	for (;;)
	{
		String8 s;
		auto    fu = f & 0xffu;
		if (fu == 0)
		{
			break;
		}
		switch (fu)
		{
			case ShaderInstructionFormat::D:
				if (inst.dst.size != 1)
				{
					EXIT("shader debug format requires scalar destination: type=%u format=%" PRIu64 " pc=0x%08" PRIx32
					     " size=%d\n",
					     static_cast<unsigned>(inst.type), static_cast<uint64_t>(inst.format), inst.pc, inst.dst.size);
				}
				s = operand_to_str(inst.dst);
				break;
			case ShaderInstructionFormat::DA: s = operand_array_to_str(inst.dst, inst.dst.size); break;
			case ShaderInstructionFormat::D2: s = operand_to_str(inst.dst2); break;
			case ShaderInstructionFormat::S0:
			case ShaderInstructionFormat::S1:
			case ShaderInstructionFormat::S2:
			case ShaderInstructionFormat::S3:
			{
				const auto source_index = static_cast<uint32_t>(fu - ShaderInstructionFormat::S0);
				const auto& source = inst.src[source_index];
				const bool immediate = source.type == ShaderOperandType::LiteralConstant ||
				                       source.type == ShaderOperandType::IntegerInlineConstant ||
				                       source.type == ShaderOperandType::FloatInlineConstant;
				if (inst.buffer_offen && source_index == 0)
				{
					// MUBUF/MTBUF widens VADDR to {voffset, index} when OFFEN is set.
					// The addressing state is carried by the instruction rather than
					// duplicated across every static buffer format.
					EXIT_IF(source.type != ShaderOperandType::Vgpr || source.size != 2);
					s = operand_array_to_str(source, source.size);
					break;
				}
				if (!immediate && source.size != 1)
				{
					EXIT("shader debug format requires scalar source: type=%u format=%" PRIu64 " pc=0x%08" PRIx32
					     " source=%u size=%d\n",
					     static_cast<unsigned>(inst.type), static_cast<uint64_t>(inst.format), inst.pc, source_index,
					     source.size);
				}
				s = operand_to_str(source);
				break;
			}
			case ShaderInstructionFormat::DA2: s = operand_array_to_str(inst.dst, 2); break;
			case ShaderInstructionFormat::DA3: s = operand_array_to_str(inst.dst, 3); break;
			case ShaderInstructionFormat::DA4: s = operand_array_to_str(inst.dst, 4); break;
			case ShaderInstructionFormat::DA8: s = operand_array_to_str(inst.dst, 8); break;
			case ShaderInstructionFormat::DA16: s = operand_array_to_str(inst.dst, 16); break;
			case ShaderInstructionFormat::D2A2: s = operand_array_to_str(inst.dst2, 2); break;
			case ShaderInstructionFormat::D2A3: s = operand_array_to_str(inst.dst2, 3); break;
			case ShaderInstructionFormat::D2A4: s = operand_array_to_str(inst.dst2, 4); break;
			case ShaderInstructionFormat::S0A2: s = operand_array_to_str(inst.src[0], 2); break;
			case ShaderInstructionFormat::S0A3: s = operand_array_to_str(inst.src[0], 3); break;
			case ShaderInstructionFormat::S0A4: s = operand_array_to_str(inst.src[0], 4); break;
			case ShaderInstructionFormat::S1A2: s = operand_array_to_str(inst.src[1], 2); break;
			case ShaderInstructionFormat::S1A3: s = operand_array_to_str(inst.src[1], 3); break;
			case ShaderInstructionFormat::S1A4: s = operand_array_to_str(inst.src[1], 4); break;
			case ShaderInstructionFormat::S1A8: s = operand_array_to_str(inst.src[1], 8); break;
			case ShaderInstructionFormat::S2A2: s = operand_array_to_str(inst.src[2], 2); break;
			case ShaderInstructionFormat::S2A3: s = operand_array_to_str(inst.src[2], 3); break;
			case ShaderInstructionFormat::S2A4: s = operand_array_to_str(inst.src[2], 4); break;
			case ShaderInstructionFormat::Attr: s = String8::FromPrintf("attr%u.%u", inst.src[1].constant.u, inst.src[2].constant.u); break;
			case ShaderInstructionFormat::Idxen: s = "idxen"; break;
			case ShaderInstructionFormat::Offen: s = "offen"; break;
			case ShaderInstructionFormat::Float1: s = "format:float1"; break;
			case ShaderInstructionFormat::Float2: s = "format:float2"; break;
			case ShaderInstructionFormat::Float4: s = "format:float4"; break;
			case ShaderInstructionFormat::Pos0: s = "pos0"; break;
			case ShaderInstructionFormat::Done: s = "done"; break;
			case ShaderInstructionFormat::Param0: s = "param0"; break;
			case ShaderInstructionFormat::Param1: s = "param1"; break;
			case ShaderInstructionFormat::Param2: s = "param2"; break;
			case ShaderInstructionFormat::Param3: s = "param3"; break;
			case ShaderInstructionFormat::Param4: s = "param4"; break;
			case ShaderInstructionFormat::Param5: s = "param5"; break;
			case ShaderInstructionFormat::Param6: s = "param6"; break;
			case ShaderInstructionFormat::Param7: s = "param7"; break;
			case ShaderInstructionFormat::Mrt0: s = "mrt_color0"; break;
			case ShaderInstructionFormat::Mrt1: s = "mrt_color1"; break;
			case ShaderInstructionFormat::Mrt2: s = "mrt_color2"; break;
			case ShaderInstructionFormat::Mrt3: s = "mrt_color3"; break;
			case ShaderInstructionFormat::Prim: s = "prim"; break;
			case ShaderInstructionFormat::Off: s = "off"; break;
			case ShaderInstructionFormat::Compr: s = "compr"; break;
			case ShaderInstructionFormat::Vm: s = "vm"; break;
			case ShaderInstructionFormat::L: s = String8::FromPrintf("label_%04" PRIx32, inst.pc + 4 + inst.src[0].constant.i); break;
			case ShaderInstructionFormat::Dmask1: s = "dmask:0x1"; break;
			case ShaderInstructionFormat::Dmask2: s = "dmask:0x2"; break;
			case ShaderInstructionFormat::Dmask4: s = "dmask:0x4"; break;
			case ShaderInstructionFormat::Dmask8: s = "dmask:0x8"; break;
			case ShaderInstructionFormat::Dmask3: s = "dmask:0x3"; break;
			case ShaderInstructionFormat::Dmask5: s = "dmask:0x5"; break;
			case ShaderInstructionFormat::Dmask7: s = "dmask:0x7"; break;
			case ShaderInstructionFormat::Dmask9: s = "dmask:0x9"; break;
			case ShaderInstructionFormat::DmaskA: s = "dmask:0xa"; break;
			case ShaderInstructionFormat::DmaskB: s = "dmask:0xb"; break;
			case ShaderInstructionFormat::DmaskC: s = "dmask:0xc"; break;
			case ShaderInstructionFormat::DmaskD: s = "dmask:0xd"; break;
			case ShaderInstructionFormat::DmaskF: s = "dmask:0xf"; break;
			case ShaderInstructionFormat::Gds: s = "gds"; break;
			case ShaderInstructionFormat::MimgDmask: s = String8::FromPrintf("dmask:0x%x", inst.mimg_dmask); break;
			default: KYTY_LOG_DEBUG("WARNING: unknown shader code %u (continuing)\n", static_cast<uint32_t>(fu)); break;
		}
		switch (fu)
		{
			case ShaderInstructionFormat::L:
			case ShaderInstructionFormat::S0:
			case ShaderInstructionFormat::S0A2:
			case ShaderInstructionFormat::S0A3:
			case ShaderInstructionFormat::S0A4: src_num = std::max(src_num, 1); break;
			case ShaderInstructionFormat::S1:
			case ShaderInstructionFormat::S1A2:
			case ShaderInstructionFormat::S1A3:
			case ShaderInstructionFormat::S1A4:
			case ShaderInstructionFormat::S1A8: src_num = std::max(src_num, 2); break;
			case ShaderInstructionFormat::S2:
			case ShaderInstructionFormat::S2A2:
			case ShaderInstructionFormat::S2A3:
			case ShaderInstructionFormat::S2A4:
			case ShaderInstructionFormat::Attr: src_num = std::max(src_num, 3); break;
			case ShaderInstructionFormat::S3: src_num = std::max(src_num, 4); break;
			default: break;
		}
		str = s + (str.IsEmpty() ? "" : ", " + str);
		f >>= 8u;
	}
	EXIT_IF(src_num != inst.src_num);
	if (inst.dst.multiplier == 2.0f)
	{
		str += " mul:2";
	}
	if (inst.dst.multiplier == 4.0f)
	{
		str += " mul:4";
	}
	if (inst.dst.multiplier == 0.5f)
	{
		str += " div:2";
	}
	if (inst.dst.clamp)
	{
		str += " clamp";
	}
	return str;
}

String8 ShaderCode::DbgInstructionToStr(const ShaderInstruction& inst)
{
	String8 ret;

	String8 name   = Core::EnumName8(inst.type);
	String8 format = dbg_fmt_to_str(inst);

	ret += String8::FromPrintf("%-20s [%-30s] ", name.c_str(), format.c_str());
	ret += dbg_fmt_print(inst);
	if (inst.mimg_address_num > 0)
	{
		ret += String8::FromPrintf(" dim:%u nsa:", static_cast<unsigned>(inst.mimg_dimension));
		for (int address = 0; address < inst.mimg_address_num; ++address)
		{
			ret += (address == 0 ? "" : ",");
			ret += operand_to_str(inst.mimg_address[address]);
		}
	}

	return ret;
}

String8 ShaderCode::DbgDump() const
{
	String8 ret;
	for (const auto& inst: m_instructions)
	{
		if (m_labels.Contains(inst.pc, [](auto label, auto pc) { return (!label.IsDisabled() && label.GetDst() == pc); }))
		{
			ret += String8::FromPrintf("\nlabel_%04" PRIx32 ":\n", inst.pc);
		}
		if (m_indirect_labels.Contains(inst.pc, [](auto label, auto pc) { return (!label.IsDisabled() && label.GetDst() == pc); }))
		{
			ret += "\n";
		}
		ret += String8::FromPrintf("  %s\n", DbgInstructionToStr(inst).c_str());
	}
	return ret;
}
void vs_print(const char* func, const HW::VertexShaderInfo& vs, const HW::ShaderRegisters& sh)
{
	KYTY_LOG_DEBUG("%s\n", func);

	KYTY_LOG_DEBUG("\t vs.data_addr                 = 0x%016" PRIx64 "\n", vs.vs_regs.data_addr);
	KYTY_LOG_DEBUG("\t es.data_addr                 = 0x%016" PRIx64 "\n", vs.es_regs.data_addr);
	KYTY_LOG_DEBUG("\t gs.data_addr                 = 0x%016" PRIx64 "\n", vs.gs_regs.data_addr);

	if (vs.vs_regs.data_addr != 0)
	{
		KYTY_LOG_DEBUG("\t vs.vgprs                     = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc1.vgprs);
		KYTY_LOG_DEBUG("\t vs.sgprs                     = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc1.sgprs);
		KYTY_LOG_DEBUG("\t vs.priority                  = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc1.priority);
		KYTY_LOG_DEBUG("\t vs.float_mode                = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc1.float_mode);
		KYTY_LOG_DEBUG("\t vs.dx10_clamp                = %s\n", vs.vs_regs.rsrc1.dx10_clamp ? "true" : "false");
		KYTY_LOG_DEBUG("\t vs.ieee_mode                 = %s\n", vs.vs_regs.rsrc1.ieee_mode ? "true" : "false");
		KYTY_LOG_DEBUG("\t vs.vgpr_component_count      = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc1.vgpr_component_count);
		KYTY_LOG_DEBUG("\t vs.cu_group_enable           = %s\n", vs.vs_regs.rsrc1.cu_group_enable ? "true" : "false");
		KYTY_LOG_DEBUG("\t vs.require_forward_progress  = %s\n", vs.vs_regs.rsrc1.require_forward_progress ? "true" : "false");
		KYTY_LOG_DEBUG("\t vs.fp16_overflow             = %s\n", vs.vs_regs.rsrc1.fp16_overflow ? "true" : "false");
		KYTY_LOG_DEBUG("\t vs.scratch_en                = %s\n", vs.vs_regs.rsrc2.scratch_en ? "true" : "false");
		KYTY_LOG_DEBUG("\t vs.user_sgpr                 = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc2.user_sgpr);
		KYTY_LOG_DEBUG("\t vs.offchip_lds               = %s\n", vs.vs_regs.rsrc2.offchip_lds ? "true" : "false");
		KYTY_LOG_DEBUG("\t vs.streamout_enabled         = %s\n", vs.vs_regs.rsrc2.streamout_enabled ? "true" : "false");
		KYTY_LOG_DEBUG("\t vs.shared_vgprs              = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc2.shared_vgprs);
	}

	if (vs.gs_regs.data_addr != 0 || vs.es_regs.data_addr != 0)
	{
		KYTY_LOG_DEBUG("\t chksum                       = 0x%016" PRIx64 "\n", vs.gs_regs.chksum);
		KYTY_LOG_DEBUG("\t gs.vgprs                     = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc1.vgprs);
		KYTY_LOG_DEBUG("\t gs.sgprs                     = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc1.sgprs);
		KYTY_LOG_DEBUG("\t gs.priority                  = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc1.priority);
		KYTY_LOG_DEBUG("\t gs.float_mode                = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc1.float_mode);
		KYTY_LOG_DEBUG("\t gs.dx10_clamp                = %s\n", vs.gs_regs.rsrc1.dx10_clamp ? "true" : "false");
		KYTY_LOG_DEBUG("\t gs.ieee_mode                 = %s\n", vs.gs_regs.rsrc1.ieee_mode ? "true" : "false");
		KYTY_LOG_DEBUG("\t gs.debug_mode                = %s\n", vs.gs_regs.rsrc1.debug_mode ? "true" : "false");
		KYTY_LOG_DEBUG("\t gs.lds_configuration         = %s\n", vs.gs_regs.rsrc1.lds_configuration ? "true" : "false");
		KYTY_LOG_DEBUG("\t gs.cu_group_enable           = %s\n", vs.gs_regs.rsrc1.cu_group_enable ? "true" : "false");
		KYTY_LOG_DEBUG("\t gs.require_forward_progress  = %s\n", vs.gs_regs.rsrc1.require_forward_progress ? "true" : "false");
		KYTY_LOG_DEBUG("\t gs.fp16_overflow             = %s\n", vs.gs_regs.rsrc1.fp16_overflow ? "true" : "false");
		KYTY_LOG_DEBUG("\t gs.gs_vgpr_component_count   = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc1.gs_vgpr_component_count);
		KYTY_LOG_DEBUG("\t gs.scratch_en                = %s\n", vs.gs_regs.rsrc2.scratch_en ? "true" : "false");
		KYTY_LOG_DEBUG("\t gs.user_sgpr                 = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc2.user_sgpr);
		KYTY_LOG_DEBUG("\t gs.offchip_lds               = %s\n", vs.gs_regs.rsrc2.offchip_lds ? "true" : "false");
		KYTY_LOG_DEBUG("\t gs.shared_vgprs              = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc2.shared_vgprs);
		KYTY_LOG_DEBUG("\t gs.es_vgpr_component_count   = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc2.es_vgpr_component_count);
		KYTY_LOG_DEBUG("\t gs.lds_size                  = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc2.lds_size);
	}

	KYTY_LOG_DEBUG("\t m_spiVsOutConfig          = 0x%08" PRIx32 "\n", sh.m_spiVsOutConfig);
	KYTY_LOG_DEBUG("\t m_spiShaderPosFormat      = 0x%08" PRIx32 "\n", sh.m_spiShaderPosFormat);
	KYTY_LOG_DEBUG("\t m_paClVsOutCntl           = 0x%08" PRIx32 "\n", sh.m_paClVsOutCntl);
	KYTY_LOG_DEBUG("\t m_spiShaderIdxFormat      = 0x%08" PRIx32 "\n", sh.m_spiShaderIdxFormat);
	KYTY_LOG_DEBUG("\t m_geNggSubgrpCntl         = 0x%08" PRIx32 "\n", sh.m_geNggSubgrpCntl);
	KYTY_LOG_DEBUG("\t m_vgtGsInstanceCnt        = 0x%08" PRIx32 "\n", sh.m_vgtGsInstanceCnt);
	KYTY_LOG_DEBUG("\t GetEsVertsPerSubgrp()     = 0x%08" PRIx32 "\n", sh.GetEsVertsPerSubgrp());
	KYTY_LOG_DEBUG("\t GetGsPrimsPerSubgrp()     = 0x%08" PRIx32 "\n", sh.GetGsPrimsPerSubgrp());
	KYTY_LOG_DEBUG("\t GetGsInstPrimsInSubgrp()  = 0x%08" PRIx32 "\n", sh.GetGsInstPrimsInSubgrp());
	KYTY_LOG_DEBUG("\t m_geMaxOutputPerSubgroup  = 0x%08" PRIx32 "\n", sh.m_geMaxOutputPerSubgroup);
	KYTY_LOG_DEBUG("\t m_vgtEsgsRingItemsize     = 0x%08" PRIx32 "\n", sh.m_vgtEsgsRingItemsize);
	KYTY_LOG_DEBUG("\t m_vgtGsMaxVertOut         = 0x%08" PRIx32 "\n", sh.m_vgtGsMaxVertOut);
	KYTY_LOG_DEBUG("\t m_vgtGsOutPrimType        = 0x%08" PRIx32 "\n", sh.m_vgtGsOutPrimType);
}

void ps_print(const char* func, const HW::PsStageRegisters& ps, const HW::ShaderRegisters& sh)
{
	KYTY_LOG_DEBUG("%s\n", func);

	KYTY_LOG_DEBUG("\t data_addr                   = 0x%016" PRIx64 "\n", ps.data_addr);
	KYTY_LOG_DEBUG("\t chksum                      = 0x%016" PRIx64 "\n", ps.chksum);
	KYTY_LOG_DEBUG("\t conservative_z_export_value = 0x%08" PRIx32 "\n", sh.db_shader_control.conservative_z_export_value);
	KYTY_LOG_DEBUG("\t shader_z_behavior           = 0x%08" PRIx32 "\n", sh.db_shader_control.shader_z_behavior);
	KYTY_LOG_DEBUG("\t shader_kill_enable          = %s\n", sh.db_shader_control.shader_kill_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t shader_z_export_enable      = %s\n", sh.db_shader_control.shader_z_export_enable ? "true" : "false");
	KYTY_LOG_DEBUG("\t shader_execute_on_noop      = %s\n", sh.db_shader_control.shader_execute_on_noop ? "true" : "false");
	KYTY_LOG_DEBUG("\t vgprs                       = 0x%02" PRIx8 "\n", ps.rsrc1.vgprs);
	KYTY_LOG_DEBUG("\t sgprs                       = 0x%02" PRIx8 "\n", ps.rsrc1.sgprs);
	KYTY_LOG_DEBUG("\t priority                    = 0x%02" PRIx8 "\n", ps.rsrc1.priority);
	KYTY_LOG_DEBUG("\t float_mode                  = 0x%02" PRIx8 "\n", ps.rsrc1.float_mode);
	KYTY_LOG_DEBUG("\t dx10_clamp                  = %s\n", ps.rsrc1.dx10_clamp ? "true" : "false");
	KYTY_LOG_DEBUG("\t debug_mode                  = %s\n", ps.rsrc1.debug_mode ? "true" : "false");
	KYTY_LOG_DEBUG("\t ieee_mode                   = %s\n", ps.rsrc1.ieee_mode ? "true" : "false");
	KYTY_LOG_DEBUG("\t cu_group_disable            = %s\n", ps.rsrc1.cu_group_disable ? "true" : "false");
	KYTY_LOG_DEBUG("\t require_forward_progress    = %s\n", ps.rsrc1.require_forward_progress ? "true" : "false");
	KYTY_LOG_DEBUG("\t fp16_overflow               = %s\n", ps.rsrc1.fp16_overflow ? "true" : "false");
	KYTY_LOG_DEBUG("\t scratch_en                  = %s\n", ps.rsrc2.scratch_en ? "true" : "false");
	KYTY_LOG_DEBUG("\t user_sgpr                   = 0x%02" PRIx8 "\n", ps.rsrc2.user_sgpr);
	KYTY_LOG_DEBUG("\t wave_cnt_en                 = %s\n", ps.rsrc2.wave_cnt_en ? "true" : "false");
	KYTY_LOG_DEBUG("\t extra_lds_size              = 0x%02" PRIx8 "\n", ps.rsrc2.extra_lds_size);
	KYTY_LOG_DEBUG("\t raster_ordered_shading      = %s\n", ps.rsrc2.raster_ordered_shading ? "true" : "false");
	KYTY_LOG_DEBUG("\t shared_vgprs                = 0x%02" PRIx8 "\n", ps.rsrc2.shared_vgprs);

	KYTY_LOG_DEBUG("\t shader_z_format             = 0x%08" PRIx32 "\n", sh.shader_z_format);
	KYTY_LOG_DEBUG("\t target_output_mode[0]       = 0x%02" PRIx8 "\n", sh.target_output_mode[0]);
	KYTY_LOG_DEBUG("\t ps_input_ena                = 0x%08" PRIx32 "\n", sh.ps_input_ena);
	KYTY_LOG_DEBUG("\t ps_input_addr               = 0x%08" PRIx32 "\n", sh.ps_input_addr);
	KYTY_LOG_DEBUG("\t ps_in_control               = 0x%08" PRIx32 "\n", sh.ps_in_control);
	KYTY_LOG_DEBUG("\t baryc_cntl                  = 0x%08" PRIx32 "\n", sh.baryc_cntl);
	KYTY_LOG_DEBUG("\t m_cbShaderMask              = 0x%08" PRIx32 "\n", sh.m_cbShaderMask);

	KYTY_LOG_DEBUG("\t m_paScShaderControl         = 0x%08" PRIx32 "\n", sh.m_paScShaderControl);
}

void cs_print(const char* func, const HW::CsStageRegisters& cs, const HW::ShaderRegisters& /*sh*/)
{
	KYTY_LOG_DEBUG("%s\n", func);

	//	KYTY_LOG_DEBUG("\t GetGpuAddress()        = 0x%016" PRIx64 "\n", cs.GetGpuAddress());
	//	KYTY_LOG_DEBUG("\t m_computePgmLo         = 0x%08" PRIx32 "\n", cs.m_computePgmLo);
	//	KYTY_LOG_DEBUG("\t m_computePgmHi         = 0x%08" PRIx32 "\n", cs.m_computePgmHi);
	//	KYTY_LOG_DEBUG("\t m_computePgmRsrc1      = 0x%08" PRIx32 "\n", cs.m_computePgmRsrc1);
	//	KYTY_LOG_DEBUG("\t m_computePgmRsrc2      = 0x%08" PRIx32 "\n", cs.m_computePgmRsrc2);
	//	KYTY_LOG_DEBUG("\t m_computeNumThreadX    = 0x%08" PRIx32 "\n", cs.m_computeNumThreadX);
	//	KYTY_LOG_DEBUG("\t m_computeNumThreadY    = 0x%08" PRIx32 "\n", cs.m_computeNumThreadY);
	//	KYTY_LOG_DEBUG("\t m_computeNumThreadZ    = 0x%08" PRIx32 "\n", cs.m_computeNumThreadZ);
	KYTY_LOG_DEBUG("\t data_addr      = 0x%016" PRIx64 "\n", cs.data_addr);
	KYTY_LOG_DEBUG("\t num_thread_x   = 0x%08" PRIx32 "\n", cs.num_thread_x);
	KYTY_LOG_DEBUG("\t num_thread_y   = 0x%08" PRIx32 "\n", cs.num_thread_y);
	KYTY_LOG_DEBUG("\t num_thread_z   = 0x%08" PRIx32 "\n", cs.num_thread_z);
	KYTY_LOG_DEBUG("\t vgprs          = 0x%02" PRIx8 "\n", cs.vgprs);
	KYTY_LOG_DEBUG("\t sgprs          = 0x%02" PRIx8 "\n", cs.sgprs);
	KYTY_LOG_DEBUG("\t bulky          = 0x%02" PRIx8 "\n", cs.bulky);
	KYTY_LOG_DEBUG("\t scratch_en     = 0x%02" PRIx8 "\n", cs.scratch_en);
	KYTY_LOG_DEBUG("\t user_sgpr      = 0x%02" PRIx8 "\n", cs.user_sgpr);
	KYTY_LOG_DEBUG("\t tgid_x_en      = 0x%02" PRIx8 "\n", cs.tgid_x_en);
	KYTY_LOG_DEBUG("\t tgid_y_en      = 0x%02" PRIx8 "\n", cs.tgid_y_en);
	KYTY_LOG_DEBUG("\t tgid_z_en      = 0x%02" PRIx8 "\n", cs.tgid_z_en);
	KYTY_LOG_DEBUG("\t tg_size_en     = 0x%02" PRIx8 "\n", cs.tg_size_en);
	KYTY_LOG_DEBUG("\t tidig_comp_cnt = 0x%02" PRIx8 "\n", cs.tidig_comp_cnt);
	KYTY_LOG_DEBUG("\t lds_size       = 0x%03" PRIx16 "\n", cs.lds_size);
}

void bi_print(const char* func, const ShaderBinaryInfo& bi)
{
	KYTY_LOG_DEBUG("%s\n", func);

	KYTY_LOG_DEBUG("\t signature                  = %.7s\n", bi.signature);
	KYTY_LOG_DEBUG("\t version                    = 0x%02" PRIx8 "\n", bi.version);
	KYTY_LOG_DEBUG("\t pssl_or_cg                 = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.pssl_or_cg));
	KYTY_LOG_DEBUG("\t cached                     = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.cached));
	KYTY_LOG_DEBUG("\t type                       = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.type));
	KYTY_LOG_DEBUG("\t source_type                = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.source_type));
	KYTY_LOG_DEBUG("\t length                     = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.length));
	KYTY_LOG_DEBUG("\t chunk_usage_base_offset_dw = 0x%02" PRIx8 "\n", bi.chunk_usage_base_offset_dw);
	KYTY_LOG_DEBUG("\t num_input_usage_slots      = 0x%02" PRIx8 "\n", bi.num_input_usage_slots);
	KYTY_LOG_DEBUG("\t is_srt                     = 0x%02" PRIx8 "\n", bi.is_srt);
	KYTY_LOG_DEBUG("\t is_srt_used_info_valid     = 0x%02" PRIx8 "\n", bi.is_srt_used_info_valid);
	KYTY_LOG_DEBUG("\t is_extended_usage_info     = 0x%02" PRIx8 "\n", bi.is_extended_usage_info);
	KYTY_LOG_DEBUG("\t reserved2                  = 0x%02" PRIx8 "\n", bi.reserved2);
	KYTY_LOG_DEBUG("\t reserved3                  = 0x%02" PRIx8 "\n", bi.reserved3);
	KYTY_LOG_DEBUG("\t hash0                      = 0x%08" PRIx32 "\n", bi.hash0);
	KYTY_LOG_DEBUG("\t hash1                      = 0x%08" PRIx32 "\n", bi.hash1);
	KYTY_LOG_DEBUG("\t crc32                      = 0x%08" PRIx32 "\n", bi.crc32);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void vs_check(const HW::VertexShaderInfo& vs, const HW::ShaderRegisters& sh)
{
	if (vs.vs_regs.data_addr != 0)
	{
		if (vs.vs_regs.rsrc1.priority != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.vs_regs.rsrc1.priority != 0 condition ignored (continuing)\n"); }
		if (vs.vs_regs.rsrc1.float_mode != 192) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.vs_regs.rsrc1.float_mode != 192 condition ignored (continuing)\n"); }
		if (vs.vs_regs.rsrc1.dx10_clamp != true) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.vs_regs.rsrc1.dx10_clamp != true condition ignored (continuing)\n"); }
		if (vs.vs_regs.rsrc1.ieee_mode != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.vs_regs.rsrc1.ieee_mode != false condition ignored (continuing)\n"); }
		if (vs.vs_regs.rsrc1.cu_group_enable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.vs_regs.rsrc1.cu_group_enable != false condition ignored (continuing)\n"); }
		if (vs.vs_regs.rsrc1.require_forward_progress != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.vs_regs.rsrc1.require_forward_progress != false condition ignored (continuing)\n"); }
		if (vs.vs_regs.rsrc1.fp16_overflow != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.vs_regs.rsrc1.fp16_overflow != false condition ignored (continuing)\n"); }
		if (vs.vs_regs.rsrc2.scratch_en != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.vs_regs.rsrc2.scratch_en != false condition ignored (continuing)\n"); }
		if (vs.vs_regs.rsrc2.offchip_lds != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.vs_regs.rsrc2.offchip_lds != false condition ignored (continuing)\n"); }
		if (vs.vs_regs.rsrc2.streamout_enabled != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.vs_regs.rsrc2.streamout_enabled != false condition ignored (continuing)\n"); }
		if (vs.vs_regs.rsrc2.shared_vgprs != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.vs_regs.rsrc2.shared_vgprs != 0 condition ignored (continuing)\n"); }
	}

	if (vs.es_regs.data_addr != 0 || vs.gs_regs.data_addr != 0)
	{
		if (vs.gs_regs.rsrc1.priority != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc1.priority != 0 condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc1.float_mode != 192) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc1.float_mode != 192 condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc1.dx10_clamp != true) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc1.dx10_clamp != true condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc1.debug_mode != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc1.debug_mode != false condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc1.ieee_mode != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc1.ieee_mode != false condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc1.cu_group_enable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc1.cu_group_enable != false condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc1.require_forward_progress != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc1.require_forward_progress != false condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc1.lds_configuration != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc1.lds_configuration != false condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc1.gs_vgpr_component_count != 3) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc1.gs_vgpr_component_count != 3 condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc1.fp16_overflow != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc1.fp16_overflow != false condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc2.scratch_en != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc2.scratch_en != false condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc2.offchip_lds != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc2.offchip_lds != false condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc2.es_vgpr_component_count != 3) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc2.es_vgpr_component_count != 3 condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc2.lds_size != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc2.lds_size != 0 condition ignored (continuing)\n"); }
		if (vs.gs_regs.rsrc2.shared_vgprs != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vs.gs_regs.rsrc2.shared_vgprs != 0 condition ignored (continuing)\n"); }
	}

	if (sh.m_spiShaderPosFormat != 0x00000004) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.m_spiShaderPosFormat != 0x00000004 condition ignored (continuing)\n"); }
	if (sh.m_paClVsOutCntl != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.m_paClVsOutCntl != 0x00000000 condition ignored (continuing)\n"); }

	if (sh.m_spiShaderIdxFormat != 0x00000000 && sh.m_spiShaderIdxFormat != 0x00000001) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.m_spiShaderIdxFormat != 0x00000000 && sh.m_spiShaderIdxFormat != 0x00000001 condition ignored (continuing)\n"); }
	if (sh.m_geNggSubgrpCntl != 0x00000000 && sh.m_geNggSubgrpCntl != 0x00000001) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.m_geNggSubgrpCntl != 0x00000000 && sh.m_geNggSubgrpCntl != 0x00000001 condition ignored (continuing)\n"); }
	if (sh.m_vgtGsInstanceCnt != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.m_vgtGsInstanceCnt != 0x00000000 condition ignored (continuing)\n"); }
	// Subgroup counts: accept 0..wave64 (0x40), not only the exact endpoints.
	if (sh.GetEsVertsPerSubgrp() > 0x00000040) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.GetEsVertsPerSubgrp() > 0x00000040 condition ignored (continuing)\n"); }
	if (sh.GetGsPrimsPerSubgrp() > 0x00000040) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.GetGsPrimsPerSubgrp() > 0x00000040 condition ignored (continuing)\n"); }
	if (sh.GetGsInstPrimsInSubgrp() > 0x00000040) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.GetGsInstPrimsInSubgrp() > 0x00000040 condition ignored (continuing)\n"); }
	if (sh.m_geMaxOutputPerSubgroup > 0x00000040) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.m_geMaxOutputPerSubgroup > 0x00000040 condition ignored (continuing)\n"); }
	if (sh.m_vgtEsgsRingItemsize != 0x00000000 && sh.m_vgtEsgsRingItemsize != 0x00000004) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.m_vgtEsgsRingItemsize != 0x00000000 && sh.m_vgtEsgsRingItemsize != 0x00000004 condition ignored (continuing)\n"); }
	// Gen5 NGG may program GS max-vert-out / out-prim-type for the hardware
// passthrough path while the host still runs a single vertex stage. Accept the
// documented zero/default and the small set of GS output primitive encodings.
const auto is_known_gs_out_prim_type = [](uint32_t value) {
	switch (value)
	{
		case 0x0u: // points
		case 0x1u: // lines
		case 0x2u: // triangles
		case 0x3u: // rectangle / 2d
		case 0x4u: // rect list
			return true;
		default: return false;
	}
};
if (sh.m_vgtGsMaxVertOut != 0x00000000 && sh.m_vgtGsMaxVertOut > 0x00000003u) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.m_vgtGsMaxVertOut != 0x00000000 && sh.m_vgtGsMaxVertOut > 0x00000003u condition ignored (continuing)\n"); }
if (!is_known_gs_out_prim_type(sh.m_vgtGsOutPrimType)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !is_known_gs_out_prim_type(sh.m_vgtGsOutPrimType) condition ignored (continuing)\n"); }
}

void ps_check(const HW::PsStageRegisters& ps, const HW::ShaderRegisters& sh)
{
	// target_output_mode[i]: 0 = unused RT, 4 = half/compr export, 9 = float32.
	// Captured multi-RT PS sets mode on several slots (MRT0..MRT3).
	for (int i = 0; i < 8; i++)
	{
		const uint8_t mode = sh.target_output_mode[i];
		if (mode != 0 && mode != 4 && mode != 9) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: mode != 0 && mode != 4 && mode != 9 condition ignored (continuing)\n"); }
	}
	if (sh.db_shader_control.conservative_z_export_value != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.db_shader_control.conservative_z_export_value != 0x00000000 condition ignored (continuing)\n"); }
	if (sh.db_shader_control.shader_z_behavior != 0x00000001 && sh.db_shader_control.shader_z_behavior != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.db_shader_control.shader_z_behavior != 0x00000001 && sh.db_shader_control.shader_z_behavior != 0x00000000 condition ignored (continuing)\n"); }
	// EXIT_NOT_IMPLEMENTED(ps.shader_kill_enable != false);
	if (sh.db_shader_control.shader_z_export_enable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.db_shader_control.shader_z_export_enable != false condition ignored (continuing)\n"); }
	// EXIT_NOT_IMPLEMENTED(ps.shader_execute_on_noop != false);
	// EXIT_NOT_IMPLEMENTED(ps.m_spiShaderPgmRsrc1Ps != 0x002c0000);
	// EXIT_NOT_IMPLEMENTED(ps.m_spiShaderPgmRsrc2Ps != 0x00000000);
	// EXIT_NOT_IMPLEMENTED(ps.vgprs != 0x00 && ps.vgprs != 0x01);
	// EXIT_NOT_IMPLEMENTED(ps.sgprs != 0x00 && ps.sgprs != 0x01);
	if (ps.rsrc1.priority != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc1.priority != 0 condition ignored (continuing)\n"); }
	if (ps.rsrc1.float_mode != 192) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc1.float_mode != 192 condition ignored (continuing)\n"); }
	if (ps.rsrc1.dx10_clamp != true) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc1.dx10_clamp != true condition ignored (continuing)\n"); }
	if (ps.rsrc1.debug_mode != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc1.debug_mode != false condition ignored (continuing)\n"); }
	if (ps.rsrc1.ieee_mode != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc1.ieee_mode != false condition ignored (continuing)\n"); }
	if (ps.rsrc1.cu_group_disable != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc1.cu_group_disable != false condition ignored (continuing)\n"); }
	if (ps.rsrc1.require_forward_progress != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc1.require_forward_progress != false condition ignored (continuing)\n"); }
	if (ps.rsrc1.fp16_overflow != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc1.fp16_overflow != false condition ignored (continuing)\n"); }
	if (ps.rsrc2.scratch_en != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc2.scratch_en != false condition ignored (continuing)\n"); }
	// EXIT_NOT_IMPLEMENTED(ps.user_sgpr != 0 && ps.user_sgpr != 4 && ps.user_sgpr != 12);
	if (ps.rsrc2.wave_cnt_en != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc2.wave_cnt_en != false condition ignored (continuing)\n"); }
	if (ps.rsrc2.extra_lds_size != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc2.extra_lds_size != 0 condition ignored (continuing)\n"); }
	if (ps.rsrc2.raster_ordered_shading != false) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc2.raster_ordered_shading != false condition ignored (continuing)\n"); }
	if (ps.rsrc2.shared_vgprs != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ps.rsrc2.shared_vgprs != 0 condition ignored (continuing)\n"); }

	if (sh.shader_z_format != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.shader_z_format != 0x00000000 condition ignored (continuing)\n"); }
	if (!ShaderPixelInputMaskSupported(sh.ps_input_ena, sh.ps_input_addr)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ShaderPixelInputMaskSupported(sh.ps_input_ena, sh.ps_input_addr) condition ignored (continuing)\n"); }
	// EXIT_NOT_IMPLEMENTED(ps.m_spiPsInControl != 0x00000000);
	if (sh.baryc_cntl != 0x00000000 && sh.baryc_cntl != 0x01000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.baryc_cntl != 0x00000000 && sh.baryc_cntl != 0x01000000 condition ignored (continuing)\n"); }
	// CB_SHADER_MASK is a four-bit per-target channel gate. Partial masks are
	// valid hardware state and are intersected with CB_TARGET_MASK when the
	// Vulkan color-write state is assembled.

	if (sh.db_shader_control.other_bits != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.db_shader_control.other_bits != 0x00000000 condition ignored (continuing)\n"); }
	if (sh.m_paScShaderControl != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sh.m_paScShaderControl != 0x00000000 condition ignored (continuing)\n"); }
}
void cs_check(const HW::CsStageRegisters& cs, const HW::ShaderRegisters& /*sh*/)
{
	// EXIT_NOT_IMPLEMENTED(cs.num_thread_x != 0x00000040);
	// EXIT_NOT_IMPLEMENTED(cs.num_thread_y != 0x00000001);
	// EXIT_NOT_IMPLEMENTED(cs.num_thread_z != 0x00000001);
	// EXIT_NOT_IMPLEMENTED(cs.vgprs != 0x00 && cs.vgprs != 0x01);
	// EXIT_NOT_IMPLEMENTED(cs.sgprs != 0x01 && cs.sgprs != 0x02);
	if (cs.bulky != 0x00) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cs.bulky != 0x00 condition ignored (continuing)\n"); }
	if (cs.scratch_en != 0x00) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cs.scratch_en != 0x00 condition ignored (continuing)\n"); }
	// EXIT_NOT_IMPLEMENTED(cs.user_sgpr != 0x0c);
	// TGID_X_EN controls whether the shader receives WorkGroupID.x. Zero is a
	// valid ABI state for compute shaders that do not consume a group id.
	if (cs.tgid_x_en > 0x01) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cs.tgid_x_en > 0x01 condition ignored (continuing)\n"); }
	// EXIT_NOT_IMPLEMENTED(cs.tgid_y_en != 0x00);
	// EXIT_NOT_IMPLEMENTED(cs.tgid_z_en != 0x00);
	if (cs.tg_size_en != 0x00) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cs.tg_size_en != 0x00 condition ignored (continuing)\n"); }
	if (cs.tidig_comp_cnt > 2) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cs.tidig_comp_cnt > 2 condition ignored (continuing)\n"); }
	//	EXIT_NOT_IMPLEMENTED(cs.m_computePgmRsrc1 != 0x002c0040);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computePgmRsrc2 != 0x00000098);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computeNumThreadX != 0x00000040);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computeNumThreadY != 0x00000001);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computeNumThreadZ != 0x00000001);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
