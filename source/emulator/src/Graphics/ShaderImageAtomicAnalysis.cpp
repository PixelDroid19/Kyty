#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

bool PlainRegister(const ShaderOperand& operand, ShaderOperandType type, int count, int limit)
{
	return operand.type == type && operand.size == count && operand.register_id >= 0 && operand.register_id <= limit - count &&
	       operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u &&
	       !operand.dpp && operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u &&
	       !operand.dpp_fetch_inactive && !operand.dpp_bound_ctrl;
}

} // namespace

bool ShaderImageAtomicAddSupported(const ShaderInstruction& instruction)
{
	return instruction.type == ShaderInstructionType::ImageAtomicAdd &&
	       instruction.format == ShaderInstructionFormat::Vdata1Vaddr2StVsrc2Dmask1 && instruction.src_num == 3 &&
	       instruction.mimg_dimension == 1u && instruction.mimg_dmask == 1u && instruction.mimg_address_num == 0 &&
	       !instruction.mimg_explicit_lod && PlainRegister(instruction.dst, ShaderOperandType::Vgpr, 1, 256) &&
	       PlainRegister(instruction.src[0], ShaderOperandType::Vgpr, 2, 256) &&
	       PlainRegister(instruction.src[1], ShaderOperandType::Sgpr, 8, 104) && instruction.src[1].register_id % 4 == 0 &&
	       PlainRegister(instruction.src[2], ShaderOperandType::Vgpr, 1, 256) &&
	       instruction.src[2].register_id == instruction.dst.register_id &&
	       (instruction.dst2.type == ShaderOperandType::Unknown || instruction.dst2.type == ShaderOperandType::Null);
}

bool ShaderImageAtomicResourceSupported(const ShaderCode& code, uint32_t index, const ShaderBindResources& bind)
{
	const int descriptor = ShaderFindImageStorageTextureDescriptor(code, index, bind, 0);
	if (descriptor < 0) { return false; }
	const auto& texture = bind.textures2D.desc[descriptor].texture;
	return texture.Type() == 9u && texture.Format() == 20u;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
