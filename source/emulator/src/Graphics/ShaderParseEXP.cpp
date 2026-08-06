#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_exp)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("exp");

	uint32_t vm     = (buffer[0] >> 12u) & 0x1u;
	uint32_t done   = (buffer[0] >> 11u) & 0x1u;
	uint32_t compr  = (buffer[0] >> 10u) & 0x1u;
	uint32_t target = (buffer[0] >> 4u) & 0x3fu;
	uint32_t en     = (buffer[0] >> 0u) & 0xfu;

	uint32_t vsrc0 = (buffer[1] >> 0u) & 0xffu;
	uint32_t vsrc1 = (buffer[1] >> 8u) & 0xffu;
	uint32_t vsrc2 = (buffer[1] >> 16u) & 0xffu;
	uint32_t vsrc3 = (buffer[1] >> 24u) & 0xffu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(vsrc0 + 256);
	inst.src[1]  = operand_parse(vsrc1 + 256);
	inst.src[2]  = operand_parse(vsrc2 + 256);
	inst.src[3]  = operand_parse(vsrc3 + 256);
	inst.src_num = 4;

	inst.type = ShaderInstructionType::Exp;
	inst.exp_enable_mask = static_cast<uint8_t>(en);

	// Color MRT targets 0x00-0x03 (mrt_color0..3). Compressed half2 uses two
	// VGPRs (en=0xf, compr=1); full float uses four. Captured Gen5 also exports
	// MRT2/MRT3 with done=0 and vm=0, so neither done nor vm is required for
	// color MRT forms other than the kill path.
	if (target <= 0x03u)
	{
		if (done != 0 && compr != 0 && en == 0x0u)
		{
			// Null export (no channels). Any MRT target may terminate a discard
			// block when preceded by exec=0; outside that pattern MRT1-3 are
			// no-ops that close the export sequence.
			static const ShaderInstructionFormat::Format k_null[] = {
			    ShaderInstructionFormat::Mrt0OffOffComprVmDone,
			    ShaderInstructionFormat::Mrt1OffOffComprVmDone,
			    ShaderInstructionFormat::Mrt2OffOffComprVmDone,
			    ShaderInstructionFormat::Mrt3OffOffComprVmDone,
			};
			// Historical MRT0 kill path also required vm=1; keep that gate for RT0.
			if (target == 0x00u)
			{
				if (vm != 0)
				{
					inst.format  = k_null[0];
					inst.src_num = 0;
				}
			} else
			{
				inst.format  = k_null[target];
				inst.src_num = 0;
			}
		} else if (compr != 0 && en != 0u)
		{
			static const ShaderInstructionFormat::Format k_compr[] = {
			    ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone,
			    ShaderInstructionFormat::Mrt1Vsrc0Vsrc1ComprVm,
			    ShaderInstructionFormat::Mrt2Vsrc0Vsrc1ComprVm,
			    ShaderInstructionFormat::Mrt3Vsrc0Vsrc1ComprVm,
			};
			inst.format  = k_compr[target];
			// The IR format has two physical packed-half source slots. Keep both
			// present even when the enable mask consumes only the first pair.
			inst.src_num = 2;
		} else if (compr == 0 && en == 0xfu)
		{
			static const ShaderInstructionFormat::Format k_full[] = {
			    ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone,
			    ShaderInstructionFormat::Mrt1Vsrc0Vsrc1Vsrc2Vsrc3Vm,
			    ShaderInstructionFormat::Mrt2Vsrc0Vsrc1Vsrc2Vsrc3Vm,
			    ShaderInstructionFormat::Mrt3Vsrc0Vsrc1Vsrc2Vsrc3Vm,
			};
			// MRT0 full form historically required done=1; keep that for RT0 only.
			if (target == 0x00u)
			{
				if (done != 0)
				{
					inst.format = k_full[0];
				}
			} else
			{
				inst.format = k_full[target];
			}
		}
	} else if (target == 0x0cu)
	{
		if (done != 0 && en == 0xfu)
		{
			inst.format = ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done;
		}
	} else if (target == 0x14u)
	{
		if (done != 0 && en == 0x1u)
		{
			inst.format  = ShaderInstructionFormat::PrimVsrc0OffOffOffDone;
			inst.src_num = 1;
		}
	}

	// GCN/GFX: parameter exports use targets 0x20+N (N = param index). Targets
	// in [32,64) are treated the same way. Targets through 0x27 map directly
	// to Param0 through Param7.
	if (inst.format == ShaderInstructionFormat::Unknown && done == 0 && compr == 0 && vm == 0 && en == 0xf)
	{
		switch (target)
		{
			case 0x20: inst.format = ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x21: inst.format = ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x22: inst.format = ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x23: inst.format = ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x24: inst.format = ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x25: inst.format = ShaderInstructionFormat::Param5Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x26: inst.format = ShaderInstructionFormat::Param6Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x27: inst.format = ShaderInstructionFormat::Param7Vsrc0Vsrc1Vsrc2Vsrc3; break;
			default: break;
		}
	}

	// Fallback: parameter exports with a partial channel mask (en != 0xf) still
	// map to the full ParamN format for bring-up — unwritten channels read
	// whatever is in the vsrc regs, which is harmless for a param.
	if (inst.format == ShaderInstructionFormat::Unknown && done == 0 && compr == 0 && vm == 0)
	{
		switch (target)
		{
			case 0x20: inst.format = ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x21: inst.format = ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x22: inst.format = ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x23: inst.format = ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x24: inst.format = ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x25: inst.format = ShaderInstructionFormat::Param5Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x26: inst.format = ShaderInstructionFormat::Param6Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x27: inst.format = ShaderInstructionFormat::Param7Vsrc0Vsrc1Vsrc2Vsrc3; break;
			default: break;
		}
	}

	if (inst.format == ShaderInstructionFormat::Unknown)
	{
		KYTY_LOG_DEBUG("%s", dst->DbgDump().c_str());
		EXIT("%s\n"
		     "unknown exp target: 0x%02" PRIx32 " done=%u compr=%u vm=%u en=0x%x at addr 0x%08" PRIx32 " (hash0 = 0x%08" PRIx32
		     ", crc32 = 0x%08" PRIx32 ")\n",
		     dst->DbgDump().c_str(), target, done, compr, vm, en, pc, dst->GetHash0(), dst->GetCrc32());
	}

	dst->GetInstructions().Add(inst);

	return 2;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
