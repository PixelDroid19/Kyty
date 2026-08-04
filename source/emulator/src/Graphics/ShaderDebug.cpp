#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String8.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

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
			case ShaderInstructionFormat::DmaskF: s = "dmask:0xf"; break;
			case ShaderInstructionFormat::Gds: s = "gds"; break;
			case ShaderInstructionFormat::MimgDmask: s = String8::FromPrintf("dmask:0x%x", inst.mimg_dmask); break;
			default: printf("WARNING: unknown shader code %u (continuing)\n", static_cast<uint32_t>(fu)); break;
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
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
