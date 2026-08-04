#include "ShaderParseOperands.h"

#include "Kyty/Core/DbgAssert.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

ShaderOperand operand_parse(uint32_t code)
{
	ShaderOperand ret;

	ret.size = 1;

	if (code >= 0 && code <= 103)
	{
		ret.type        = ShaderOperandType::Sgpr;
		ret.register_id = static_cast<int>(code);
	} else if (code >= 128 && code <= 192)
	{
		ret.type       = ShaderOperandType::IntegerInlineConstant;
		ret.constant.i = static_cast<int>(code) - 128;
		ret.size       = 0;
	} else if (code >= 193 && code <= 208)
	{
		ret.type       = ShaderOperandType::IntegerInlineConstant;
		ret.constant.i = 192 - static_cast<int>(code);
		ret.size       = 0;
	} else if (code >= 240 && code <= 248)
	{
		static constexpr uint32_t bits[] = {
		    0x3f000000u, 0xbf000000u, 0x3f800000u, 0xbf800000u, 0x40000000u,
		    0xc0000000u, 0x40800000u, 0xc0800000u, 0x3e22f983u,
		};
		ret.type       = ShaderOperandType::FloatInlineConstant;
		ret.constant.u = bits[static_cast<int>(code) - 240];
		ret.size       = 0;
	} else if (code >= 256)
	{
		ret.type        = ShaderOperandType::Vgpr;
		ret.register_id = static_cast<int>(code) - 256;
	} else
	{
		switch (code)
		{
			case 106: ret.type = ShaderOperandType::VccLo; break;
			case 107: ret.type = ShaderOperandType::VccHi; break;
			case 124: ret.type = ShaderOperandType::M0; break;
			case 125: ret.type = ShaderOperandType::Null; break;
			case 126: ret.type = ShaderOperandType::ExecLo; break;
			case 127: ret.type = ShaderOperandType::ExecHi; break;
			case 251: ret.type = ShaderOperandType::VccZ; break;
			case 252: ret.type = ShaderOperandType::ExecZ; break;
			case 255:
				ret.type = ShaderOperandType::LiteralConstant;
				ret.size = 0;
				break;
			default: EXIT("unknown operand: %u\n", code);
		}
	}

	return ret;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
