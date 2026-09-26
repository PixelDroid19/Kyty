#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_bvh)
{
	EXIT_IF(dst == nullptr || src == nullptr || buffer == nullptr || buffer < src);
	EXIT_NOT_IMPLEMENTED(!next_gen);
	const uint32_t nsa = (buffer[0] >> 1u) & 3u;
	// GFX10, 32-bit node pointer, full precision addresses: fixed R128,
	// UNORM and DMASK, no cache modifiers, sampler, A16 or D16.
	constexpr uint32_t header = (0x3cu << 26u) | (0x66u << 18u) | 0x9f01u;
	EXIT_NOT_IMPLEMENTED((buffer[0] & ~6u) != header);
	EXIT_NOT_IMPLEMENTED((buffer[1] & 0xffe00000u) != 0);
	EXIT_NOT_IMPLEMENTED(nsa != 0 && nsa != 3);

	const uint32_t vdata = (buffer[1] >> 8u) & 255u;
	const uint32_t vaddr = buffer[1] & 255u;
	const uint32_t srsrc = ((buffer[1] >> 16u) & 31u) * 4u;
	EXIT_NOT_IMPLEMENTED(vdata > 252u || srsrc > 100u);
	EXIT_NOT_IMPLEMENTED(nsa == 0 && vaddr > 245u);

	ShaderInstruction inst;
	inst.pc                  = pc;
	inst.type                = ShaderInstructionType::ImageBvhIntersectRay;
	inst.format              = ShaderInstructionFormat::Vdata4BvhAddressSrsrc4;
	inst.dst                 = operand_parse(vdata + 256u);
	inst.dst.size            = 4;
	inst.src[0]              = operand_parse(vaddr + 256u);
	inst.src[1]              = operand_parse(srsrc);
	inst.src[1].size         = 4;
	inst.src_num             = 2;
	inst.mimg_address_num    = 11;
	inst.mimg_dmask          = 15;
	inst.mimg_address[0]     = inst.src[0];
	for (uint32_t i = 1; i < 11; ++i)
	{
		const uint32_t vgpr = nsa == 0 ? vaddr + i : ((buffer[2 + (i - 1) / 4] >> (((i - 1) % 4) * 8u)) & 255u);
		inst.mimg_address[i] = operand_parse(vgpr + 256u);
	}
	// NSA padding is not an operand; all eleven true sources are explicit,
	// including the contiguous form, so variable collection sees every read.
	dst->GetInstructions().Add(inst);
	return 2 + nsa;
}

} // namespace Kyty::Libs::Graphics

#endif
