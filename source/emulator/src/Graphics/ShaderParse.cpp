#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer != nullptr);

	auto type = dst->GetType();

	dst->GetInstructions().Clear();
	dst->GetLabels().Clear();
	dst->GetIndirectLabels().Clear();

	const auto* ptr = src + pc / 4;
	for (;;)
	{
		auto instruction = ptr[0];
		auto pc          = 4 * static_cast<uint32_t>(ptr - src);

		if ((instruction & 0x80000000u) == 0x00000000)
		{
			ptr += shader_parse_vop2(pc, src, ptr, dst, next_gen);
		} else if ((instruction & 0xF8000000u) == 0xC0000000)
		{
			if (next_gen) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: next_gen guard ignored (continuing)\n"); }
			ptr += shader_parse_smrd(pc, src, ptr, dst, next_gen);
		} else if ((instruction & 0xC0000000u) == 0x80000000)
		{
			ptr += shader_parse_sop2(pc, src, ptr, dst, next_gen);
		} else
		{
			switch (instruction >> 26u)
			{
				case 0x32: ptr += shader_parse_vintrp(pc, src, ptr, dst, next_gen); break;
				case 0x34:
					if (next_gen) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: next_gen guard ignored (continuing)\n"); }
					ptr += shader_parse_vop3(pc, src, ptr, dst, next_gen);
					break;
				case 0x35:
					if (!next_gen) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !next_gen guard ignored (continuing)\n"); }
					ptr += shader_parse_vop3(pc, src, ptr, dst, next_gen);
					break;
				case 0x36: ptr += shader_parse_ds(pc, src, ptr, dst, next_gen); break;
				case 0x38: ptr += shader_parse_mubuf(pc, src, ptr, dst, next_gen); break;
				case 0x3a: ptr += shader_parse_mtbuf(pc, src, ptr, dst, next_gen); break;
				case 0x3c: ptr += shader_parse_mimg(pc, src, ptr, dst, next_gen); break;
				case 0x3d:
					if (!next_gen) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !next_gen guard ignored (continuing)\n"); }
					ptr += shader_parse_smem(pc, src, ptr, dst, next_gen);
					break;
				case 0x3e: ptr += shader_parse_exp(pc, src, ptr, dst, next_gen); break;
				default:
				{
					KYTY_LOG_DEBUG("%s", dst->DbgDump().c_str());
					EXIT("unknown code 0x%08" PRIx32 " at addr 0x%08" PRIx32 "\n", ptr[0], pc);
				}
			}
		}

		if ((instruction == 0xBF810000 && (type == ShaderType::Vertex || type == ShaderType::Pixel || type == ShaderType::Compute) &&
		     !dst->GetLabels().Contains(4 * static_cast<uint32_t>(ptr - src), [](auto label, auto pc) { return label.GetDst() == pc; })) ||
		    (instruction == 0xBE802000 && type == ShaderType::Fetch))
		{
			break;
		}
	}

	return ptr - src;
}

void ShaderParse(const uint32_t* src, ShaderCode* dst)
{
	shader_parse(0, src, nullptr, dst, Config::IsNextGen());
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
