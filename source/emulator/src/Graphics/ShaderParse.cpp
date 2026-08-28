#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr uint32_t kSCodeEndWord  = 0xbf9f0000u;
constexpr uint32_t kSCodeEndCount = 5u;

bool shader_code_end_padding_valid(const uint32_t* ptr, const uint32_t* end, const uint32_t* src, const ShaderCode& code)
{
	if (ptr == nullptr || end == nullptr || src == nullptr || ptr >= end)
	{
		return false;
	}
	uint32_t marker_count = 0;
	for (const auto* marker = ptr; marker < end && *marker == kSCodeEndWord; ++marker)
	{
		++marker_count;
	}
	if (marker_count < kSCodeEndCount)
	{
		return false;
	}
	const uint32_t padding_pc = 4u * static_cast<uint32_t>(ptr - src);
	const uint32_t range_end_pc = 4u * static_cast<uint32_t>(end - src);
	for (const auto& label: code.GetLabels())
	{
		if (label.GetDst() >= padding_pc && label.GetDst() < range_end_pc)
		{
			return false;
		}
	}
	return true;
}

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
	bool        saw_endpgm = false;
	for (;;)
	{
		if (end != nullptr && ptr >= end)
		{
			return false;
		}
		auto instruction = ptr[0];
		auto pc          = 4 * static_cast<uint32_t>(ptr - src);
		if (instruction == kSCodeEndWord)
		{
			if (!saw_endpgm || dst->GetInstructions().IsEmpty() ||
			    !shader_code_end_padding_valid(ptr, end, src, *dst))
			{
				return false;
			}
			const auto last_type = dst->GetInstructions().At(dst->GetInstructions().Size() - 1u).type;
			if (last_type != ShaderInstructionType::SEndpgm && last_type != ShaderInstructionType::SBranch)
			{
				return false;
			}
			*parsed_words = static_cast<uint32_t>(ptr - src);
			return true;
		}
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
		if (!dst->GetInstructions().IsEmpty() &&
		    dst->GetInstructions().At(dst->GetInstructions().Size() - 1u).type == ShaderInstructionType::SEndpgm)
		{
			saw_endpgm = true;
		}
		const bool setpc_terminator =
		    allow_setpc_terminator && end != nullptr && ptr == end && !dst->GetInstructions().IsEmpty() &&
		    dst->GetInstructions().At(dst->GetInstructions().Size() - 1u).pc == pc &&
		    dst->GetInstructions().At(dst->GetInstructions().Size() - 1u).type == ShaderInstructionType::SSetpcB64;

		const uint32_t next_pc = 4u * static_cast<uint32_t>(ptr - src);
		const bool tail_has_static_entry = end != nullptr
		                                       ? dst->GetLabels().Contains(
		                                             next_pc, [](auto label, auto target) { return label.GetDst() >= target; })
		                                       : dst->GetLabels().Contains(
		                                             next_pc, [](auto label, auto target) { return label.GetDst() == target; });
		const bool endpgm_terminator =
		    instruction == 0xBF810000 && (type == ShaderType::Vertex || type == ShaderType::Pixel || type == ShaderType::Compute) &&
		    !tail_has_static_entry;

		if (endpgm_terminator ||
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
		const auto instruction_count = dst != nullptr ? dst->GetInstructions().Size() : 0u;
		const auto* last = instruction_count != 0u ? &dst->GetInstructions().At(instruction_count - 1u) : nullptr;
		uint32_t   code_end_words_after_last = 0u;
		uint32_t   labels_after_last         = 0u;
		uint32_t   first_label_after_last    = 0u;
		if (src != nullptr && last != nullptr && last->pc / sizeof(uint32_t) + 1u < code_size_bytes / sizeof(uint32_t))
		{
			const uint32_t tail_word = last->pc / sizeof(uint32_t) + 1u;
			for (uint32_t index = tail_word; index < code_size_bytes / sizeof(uint32_t) && src[index] == kSCodeEndWord; ++index)
			{
				++code_end_words_after_last;
			}
			for (const auto& label: dst->GetLabels())
			{
				if (label.GetDst() >= tail_word * sizeof(uint32_t))
				{
					if (labels_after_last == 0u)
					{
						first_label_after_last = label.GetDst();
					}
					++labels_after_last;
				}
			}
		}
		EXIT("shader code range ended without a complete reachable terminator: size=%u hash0=0x%08" PRIx32 " crc32=0x%08" PRIx32
		     " stage=%u decoded=%u last_pc=0x%08" PRIx32 " last_type=%u last_format=0x%016" PRIx64
		     " code_end_words_after_last=%u labels_after_last=%u first_label_after_last=0x%08" PRIx32 "\n",
		     code_size_bytes, dst != nullptr ? dst->GetHash0() : 0u, dst != nullptr ? dst->GetCrc32() : 0u,
		     dst != nullptr ? static_cast<unsigned>(dst->GetType()) : 0u, static_cast<unsigned>(instruction_count),
		     last != nullptr ? last->pc : 0u, last != nullptr ? static_cast<unsigned>(last->type) : 0u,
		     last != nullptr ? static_cast<uint64_t>(last->format) : 0u, code_end_words_after_last, labels_after_last,
		     first_label_after_last);
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
