#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

bool shader_parse_range(const uint32_t* src, const uint32_t* end, ShaderCode* dst, bool next_gen, bool allow_setpc_terminator,
	                    uint32_t* parsed_words)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(parsed_words == nullptr);

	auto type = dst->GetType();

	dst->GetInstructions().Clear();
	dst->GetLabels().Clear();
	dst->GetIndirectLabels().Clear();

	const auto* ptr = src;
	for (;;)
	{
		if (end != nullptr && ptr >= end)
		{
			return false;
		}
		auto instruction = ptr[0];
		auto pc          = 4 * static_cast<uint32_t>(ptr - src);
		constexpr uint32_t kMaxDecodedInstructionWords = 8u;
		uint32_t           bounded_words[kMaxDecodedInstructionWords] = {};
		const uint32_t*    decode_src = src;
		const uint32_t*    decode_ptr = ptr;
		if (end != nullptr)
		{
			const auto available = static_cast<uint32_t>(end - ptr);
			const auto copy_num  = available < kMaxDecodedInstructionWords ? available : kMaxDecodedInstructionWords;
			for (uint32_t i = 0; i < copy_num; ++i)
			{
				bounded_words[i] = ptr[i];
			}
			// Family decoders use src only for their local buffer-range assertion;
			// PC is supplied explicitly, so a bounded scratch copy prevents optional
			// literal/SDWA reads from crossing the mapped guest range.
			decode_src = bounded_words;
			decode_ptr = bounded_words;
		}

		uint32_t words = 0;
		if ((instruction & 0x80000000u) == 0x00000000)
		{
			words = shader_parse_vop2(pc, decode_src, decode_ptr, dst, next_gen);
		} else if ((instruction & 0xF8000000u) == 0xC0000000)
		{
			if (next_gen) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: next_gen guard ignored (continuing)\n"); }
			words = shader_parse_smrd(pc, decode_src, decode_ptr, dst, next_gen);
		} else if ((instruction & 0xC0000000u) == 0x80000000)
		{
			words = shader_parse_sop2(pc, decode_src, decode_ptr, dst, next_gen);
		} else
		{
			switch (instruction >> 26u)
			{
				case 0x32: words = shader_parse_vintrp(pc, decode_src, decode_ptr, dst, next_gen); break;
				case 0x34:
					if (next_gen) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: next_gen guard ignored (continuing)\n"); }
					words = shader_parse_vop3(pc, decode_src, decode_ptr, dst, next_gen);
					break;
				case 0x35:
					if (!next_gen) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !next_gen guard ignored (continuing)\n"); }
					words = shader_parse_vop3(pc, decode_src, decode_ptr, dst, next_gen);
					break;
				case 0x36: words = shader_parse_ds(pc, decode_src, decode_ptr, dst, next_gen); break;
				case 0x38: words = shader_parse_mubuf(pc, decode_src, decode_ptr, dst, next_gen); break;
				case 0x3a: words = shader_parse_mtbuf(pc, decode_src, decode_ptr, dst, next_gen); break;
				case 0x3c: words = shader_parse_mimg(pc, decode_src, decode_ptr, dst, next_gen); break;
				case 0x3d:
					if (!next_gen) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !next_gen guard ignored (continuing)\n"); }
					words = shader_parse_smem(pc, decode_src, decode_ptr, dst, next_gen);
					break;
				case 0x3e: words = shader_parse_exp(pc, decode_src, decode_ptr, dst, next_gen); break;
				default:
				{
					KYTY_LOG_DEBUG("%s", dst->DbgDump().c_str());
					EXIT("unknown code 0x%08" PRIx32 " at addr 0x%08" PRIx32 "\n", ptr[0], pc);
				}
			}
		}
		EXIT_IF(words == 0u);
		EXIT_IF(words > kMaxDecodedInstructionWords);
		if (end != nullptr && words > static_cast<uint32_t>(end - ptr))
		{
			return false;
		}
		ptr += words;
		const bool setpc_terminator =
		    allow_setpc_terminator && end != nullptr && ptr == end && !dst->GetInstructions().IsEmpty() &&
		    dst->GetInstructions().At(dst->GetInstructions().Size() - 1u).pc == pc &&
		    dst->GetInstructions().At(dst->GetInstructions().Size() - 1u).type == ShaderInstructionType::SSetpcB64;

		if ((instruction == 0xBF810000 && (type == ShaderType::Vertex || type == ShaderType::Pixel || type == ShaderType::Compute) &&
		     !dst->GetLabels().Contains(4 * static_cast<uint32_t>(ptr - src), [](auto label, auto pc) { return label.GetDst() == pc; })) ||
		    (instruction == 0xBE802000 && type == ShaderType::Fetch) || setpc_terminator)
		{
			break;
		}
	}

	*parsed_words = static_cast<uint32_t>(ptr - src);
	return true;
}

} // namespace

void ShaderParse(const uint32_t* src, ShaderCode* dst)
{
	uint32_t parsed_words = 0;
	EXIT_IF(!shader_parse_range(src, nullptr, dst, Config::IsNextGen(), false, &parsed_words));
	static_cast<void>(parsed_words);
}

bool ShaderTryParseBounded(const uint32_t* src, uint32_t code_size_bytes, ShaderCode* dst)
{
	if (src == nullptr || dst == nullptr || code_size_bytes == 0u || (code_size_bytes & 3u) != 0u)
	{
		return false;
	}
	uint32_t parsed_words = 0;
	return shader_parse_range(src, src + code_size_bytes / sizeof(uint32_t), dst, Config::IsNextGen(), false, &parsed_words);
}

void ShaderParse(const uint32_t* src, uint32_t code_size_bytes, ShaderCode* dst)
{
	if (!ShaderTryParseBounded(src, code_size_bytes, dst))
	{
		EXIT("shader code range ended without a complete reachable terminator: size=%u hash0=0x%08" PRIx32 " crc32=0x%08" PRIx32
		     "\n",
		     code_size_bytes, dst != nullptr ? dst->GetHash0() : 0u, dst != nullptr ? dst->GetCrc32() : 0u);
	}
}

void ShaderParseFusedFront(const uint32_t* src, uint32_t code_size_bytes, ShaderCode* dst)
{
	if (src == nullptr || dst == nullptr || code_size_bytes == 0u || (code_size_bytes & 3u) != 0u ||
	    ShaderLookupContinuation(reinterpret_cast<uint64_t>(src)) == 0u)
	{
		EXIT("invalid or unregistered fused shader code range\n");
	}
	uint32_t parsed_words = 0;
	if (!shader_parse_range(src, src + code_size_bytes / sizeof(uint32_t), dst, Config::IsNextGen(), true, &parsed_words))
	{
		EXIT("fused shader code range ended without a complete reachable terminator: size=%u hash0=0x%08" PRIx32
		     " crc32=0x%08" PRIx32 "\n",
		     code_size_bytes, dst->GetHash0(), dst->GetCrc32());
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
