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
	inst.exp_control = static_cast<uint8_t>(vm | (done << 1u) | (compr << 2u));

	// Color MRT targets 0x00-0x07 (mrt_color0..7). Compressed half2 uses two
	// VGPRs (en=0xf, compr=1); full float uses four. Captured Gen5 also exports
	// MRT2+ with done=0 and vm=0, so neither done nor vm is required for
	// color MRT forms other than the kill path.
	if (target <= 0x07u)
	{
		if (done != 0 && compr != 0 && en == 0x0u)
		{
			// Null export (no channels). Any MRT target may terminate a discard
			// block when preceded by exec=0; outside that pattern MRT1-7 are
			// no-ops that close the export sequence.
			static const ShaderInstructionFormat::Format k_null[] = {
			    ShaderInstructionFormat::Mrt0OffOffComprVmDone,
			    ShaderInstructionFormat::Mrt1OffOffComprVmDone,
			    ShaderInstructionFormat::Mrt2OffOffComprVmDone,
			    ShaderInstructionFormat::Mrt3OffOffComprVmDone,
			    ShaderInstructionFormat::Mrt4OffOffComprVmDone,
			    ShaderInstructionFormat::Mrt5OffOffComprVmDone,
			    ShaderInstructionFormat::Mrt6OffOffComprVmDone,
			    ShaderInstructionFormat::Mrt7OffOffComprVmDone,
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
			    ShaderInstructionFormat::Mrt4Vsrc0Vsrc1ComprVm,
			    ShaderInstructionFormat::Mrt5Vsrc0Vsrc1ComprVm,
			    ShaderInstructionFormat::Mrt6Vsrc0Vsrc1ComprVm,
			    ShaderInstructionFormat::Mrt7Vsrc0Vsrc1ComprVm,
			};
			inst.format  = k_compr[target];
			// The IR format has two physical packed-half source slots. Keep both
			// present even when the enable mask consumes only the first pair.
			inst.src_num = 2;
		} else if (compr == 0 && en != 0u)
		{
			// Full-precision MRT exports enable their four physical sources
			// independently. Retain EN for component-wise output accumulation.
			static const ShaderInstructionFormat::Format k_full[] = {
			    ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone,
			    ShaderInstructionFormat::Mrt1Vsrc0Vsrc1Vsrc2Vsrc3Vm,
			    ShaderInstructionFormat::Mrt2Vsrc0Vsrc1Vsrc2Vsrc3Vm,
			    ShaderInstructionFormat::Mrt3Vsrc0Vsrc1Vsrc2Vsrc3Vm,
			    ShaderInstructionFormat::Mrt4Vsrc0Vsrc1Vsrc2Vsrc3Vm,
			    ShaderInstructionFormat::Mrt5Vsrc0Vsrc1Vsrc2Vsrc3Vm,
			    ShaderInstructionFormat::Mrt6Vsrc0Vsrc1Vsrc2Vsrc3Vm,
			    ShaderInstructionFormat::Mrt7Vsrc0Vsrc1Vsrc2Vsrc3Vm,
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
	} else if (target == 0x08u && dst->GetType() == ShaderType::Pixel)
	{
		// RDNA2 Table 56: target 8 is pixel Z. Only the captured full-precision
		// one-channel form is accepted; all other target-8 encodings remain strict.
		if (done != 0 && compr == 0 && vm != 0 && en == 0x1u)
		{
			inst.format  = ShaderInstructionFormat::PixelZVsrc0VmDone;
			inst.src_num = 1;
		}
	} else if (target == 0x09u && next_gen && dst->GetType() == ShaderType::Pixel)
	{
		// RDNA2 Table 106: NULL carries the valid EXEC mask, without VGPR data.
		if (done != 0 && compr == 0 && vm != 0 && en == 0u)
		{
			inst.format  = ShaderInstructionFormat::NullVmDone;
			inst.src_num = 0;
		}
	} else if (target == 0x0cu)
	{
		if (done != 0 && en == 0xfu)
		{
			inst.format = ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done;
		}
	} else if (target == 0x0du && dst->GetType() == ShaderType::Vertex && Config::IsNextGen())
	{
		// Z is the only source in this form. DONE marks the last position
		// export, so both intermediate and final miscellaneous exports are valid.
		if (compr == 0 && vm == 0 && en == 0x4u)
		{
			inst.format  = ShaderInstructionFormat::Pos1OffOffVsrc0Off;
			inst.src[0]  = inst.src[2];
			inst.src_num = 1;
		}
	} else if (target == 0x14u)
	{
		if (done != 0 && en == 0x1u)
		{
			inst.format  = ShaderInstructionFormat::PrimVsrc0OffOffOffDone;
			inst.src_num = 1;
		}
	}

	// GCN/GFX: parameter exports use targets 0x20+N (N = param index, up to 31).
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
			case 0x28: inst.format = ShaderInstructionFormat::Param8Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x29: inst.format = ShaderInstructionFormat::Param9Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x2a: inst.format = ShaderInstructionFormat::Param10Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x2b: inst.format = ShaderInstructionFormat::Param11Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x2c: inst.format = ShaderInstructionFormat::Param12Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x2d: inst.format = ShaderInstructionFormat::Param13Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x2e: inst.format = ShaderInstructionFormat::Param14Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x2f: inst.format = ShaderInstructionFormat::Param15Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x30: inst.format = ShaderInstructionFormat::Param16Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x31: inst.format = ShaderInstructionFormat::Param17Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x32: inst.format = ShaderInstructionFormat::Param18Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x33: inst.format = ShaderInstructionFormat::Param19Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x34: inst.format = ShaderInstructionFormat::Param20Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x35: inst.format = ShaderInstructionFormat::Param21Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x36: inst.format = ShaderInstructionFormat::Param22Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x37: inst.format = ShaderInstructionFormat::Param23Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x38: inst.format = ShaderInstructionFormat::Param24Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x39: inst.format = ShaderInstructionFormat::Param25Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x3a: inst.format = ShaderInstructionFormat::Param26Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x3b: inst.format = ShaderInstructionFormat::Param27Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x3c: inst.format = ShaderInstructionFormat::Param28Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x3d: inst.format = ShaderInstructionFormat::Param29Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x3e: inst.format = ShaderInstructionFormat::Param30Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x3f: inst.format = ShaderInstructionFormat::Param31Vsrc0Vsrc1Vsrc2Vsrc3; break;
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
			case 0x28: inst.format = ShaderInstructionFormat::Param8Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x29: inst.format = ShaderInstructionFormat::Param9Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x2a: inst.format = ShaderInstructionFormat::Param10Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x2b: inst.format = ShaderInstructionFormat::Param11Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x2c: inst.format = ShaderInstructionFormat::Param12Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x2d: inst.format = ShaderInstructionFormat::Param13Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x2e: inst.format = ShaderInstructionFormat::Param14Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x2f: inst.format = ShaderInstructionFormat::Param15Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x30: inst.format = ShaderInstructionFormat::Param16Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x31: inst.format = ShaderInstructionFormat::Param17Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x32: inst.format = ShaderInstructionFormat::Param18Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x33: inst.format = ShaderInstructionFormat::Param19Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x34: inst.format = ShaderInstructionFormat::Param20Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x35: inst.format = ShaderInstructionFormat::Param21Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x36: inst.format = ShaderInstructionFormat::Param22Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x37: inst.format = ShaderInstructionFormat::Param23Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x38: inst.format = ShaderInstructionFormat::Param24Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x39: inst.format = ShaderInstructionFormat::Param25Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x3a: inst.format = ShaderInstructionFormat::Param26Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x3b: inst.format = ShaderInstructionFormat::Param27Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x3c: inst.format = ShaderInstructionFormat::Param28Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x3d: inst.format = ShaderInstructionFormat::Param29Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x3e: inst.format = ShaderInstructionFormat::Param30Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x3f: inst.format = ShaderInstructionFormat::Param31Vsrc0Vsrc1Vsrc2Vsrc3; break;
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

	// Canonicalize only the unused IR tail after format selection and source
	// remapping (POS1 Z becomes src[0]). EN does not change the physical source
	// slots retained by full or compressed exports.
	for (int source = inst.src_num; source < 4; ++source)
	{
		inst.src[source] = ShaderOperand {};
	}

	dst->GetInstructions().Add(inst);

	return 2;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
