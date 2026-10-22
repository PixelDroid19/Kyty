#include "ShaderStorageAnalysis.h"

#include <array>
#include <unordered_map>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr int kSgprCount = 104;
constexpr int16_t kUnknown = -1;
constexpr int16_t kExtendedLo = ShaderTextureResources::RES_MAX * 8;
constexpr int16_t kExtendedHi = kExtendedLo + 1;
constexpr int16_t kFirstSamplerWord = kExtendedHi + 1;
using Origins = std::array<int16_t, kSgprCount>;

bool ValidRange(int first, int count)
{
	return first >= 0 && count > 0 && first < kSgprCount && count <= kSgprCount - first;
}

bool Plain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && !operand.dpp;
}

int16_t TextureWord(int descriptor, int field)
{
	return static_cast<int16_t>(descriptor * 8 + field);
}

int16_t SamplerWord(int sampler, int field)
{
	return static_cast<int16_t>(kFirstSamplerWord + sampler * 4 + field);
}

bool ClearInitialRange(int first, int count, int shift, Origins* origins)
{
	if (first < 0 || first > kSgprCount - shift - count) { return false; }
	for (int word = 0; word < count; ++word) { (*origins)[first + shift + word] = kUnknown; }
	return true;
}

bool InitialOrigins(const ShaderBindResources& bind, int shift, Origins* origins)
{
	origins->fill(kUnknown);
	for (int descriptor = 0; descriptor < bind.textures2D.textures_num; ++descriptor)
	{
		const auto& texture = bind.textures2D.desc[descriptor];
		if (texture.extended || texture.dynamic_sload) { continue; }
		if (texture.start_register < 0 || texture.start_register > kSgprCount - shift - 8) { return false; }
		const int first = texture.start_register + shift;
		for (int field = 0; field < 8; ++field) { (*origins)[first + field] = TextureWord(descriptor, field); }
	}
	for (int sampler = 0; sampler < bind.samplers.samplers_num; ++sampler)
	{
		if (bind.samplers.extended[sampler] || bind.samplers.dynamic_sload[sampler]) { continue; }
		const int first = bind.samplers.start_register[sampler];
		if (first < 0 || first > kSgprCount - shift - 4) { return false; }
		for (int field = 0; field < 4; ++field) { (*origins)[first + shift + field] = SamplerWord(sampler, field); }
	}
	for (int pointer = 0; pointer < bind.gds_pointers.pointers_num; ++pointer)
	{
		if (bind.gds_pointers.extended[pointer]) { continue; }
		if (!ClearInitialRange(bind.gds_pointers.start_register[pointer], 1, shift, origins)) { return false; }
	}
	// Direct scalar data is initialized after descriptors by the SPIR-V prolog.
	for (int scalar = 0; scalar < bind.direct_sgprs.sgprs_num; ++scalar)
	{
		const int scalar_shift = bind.direct_sgprs.absolute_register[scalar] ? 0 : shift;
		const int first = bind.direct_sgprs.start_register[scalar];
		if (first < 0 || first >= kSgprCount - scalar_shift) { return false; }
		const int reg = first + scalar_shift;
		if (!bind.direct_sgprs.absolute_register[scalar] && (*origins)[reg] != kUnknown) { continue; }
		(*origins)[reg] = kUnknown;
	}
	if (bind.extended.used)
	{
		const int first = bind.extended.start_register;
		if (!ValidRange(first, 2)) { return false; }
		(*origins)[first] = kExtendedLo;
		(*origins)[first + 1] = kExtendedHi;
	}
	return true;
}

bool Invalidate(const ShaderOperand& destination, Origins* origins)
{
	if (destination.type != ShaderOperandType::Sgpr) { return true; }
	if (!ValidRange(destination.register_id, destination.size)) { return false; }
	for (int word = 0; word < destination.size; ++word) { (*origins)[destination.register_id + word] = kUnknown; }
	return true;
}

int ScalarLoadWords(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::SLoadDword: return 1;
		case ShaderInstructionType::SLoadDwordx2: return 2;
		case ShaderInstructionType::SLoadDwordx4: return 4;
		case ShaderInstructionType::SLoadDwordx8: return 8;
		case ShaderInstructionType::SLoadDwordx16: return 16;
		default: return 0;
	}
}

int16_t ExtendedWord(const ShaderBindResources& bind, uint32_t offset)
{
	if (bind.extended.eud_size_dw != 0 && offset >= bind.extended.eud_size_dw) { return kUnknown; }
	int16_t origin = kUnknown;
	for (int descriptor = 0; descriptor < bind.textures2D.textures_num; ++descriptor)
	{
		const auto& texture = bind.textures2D.desc[descriptor];
		if (!texture.extended || texture.dynamic_sload || texture.start_register < 16) { continue; }
		const auto first = static_cast<uint32_t>(texture.start_register - 16);
		if (offset >= first && offset - first < 8u) { origin = TextureWord(descriptor, static_cast<int>(offset - first)); }
	}
	for (int sampler = 0; sampler < bind.samplers.samplers_num; ++sampler)
	{
		if (!bind.samplers.extended[sampler] || bind.samplers.dynamic_sload[sampler] || bind.samplers.start_register[sampler] < 16)
		{
			continue;
		}
		const auto first = static_cast<uint32_t>(bind.samplers.start_register[sampler] - 16);
		if (offset >= first && offset - first < 4u) { return SamplerWord(sampler, static_cast<int>(offset - first)); }
	}
	for (int pointer = 0; pointer < bind.gds_pointers.pointers_num; ++pointer)
	{
		if (bind.gds_pointers.extended[pointer] && bind.gds_pointers.start_register[pointer] >= 16 &&
		    offset == static_cast<uint32_t>(bind.gds_pointers.start_register[pointer] - 16)) { return kUnknown; }
	}
	return origin;
}

bool TransferLoad(const ShaderInstruction& instruction, const ShaderBindResources& bind, const Origins& before, Origins* after)
{
	const int count = ScalarLoadWords(instruction.type);
	if (count == 0 || instruction.dst.type != ShaderOperandType::Sgpr) { return true; }
	if (instruction.dst.size != count || instruction.src_num != 2)
	{
		return false;
	}
	// The records of one load name disjoint destination blocks (a table load
	// partitioned into descriptors); words outside them stay unknown.
	bool mapped = false;
	for (const auto& mapping: bind.dynamic_sloads.records)
	{
		if (mapping.instruction_pc != instruction.pc) { continue; }
		mapped = true;
		const bool sampler = mapping.kind == ShaderDynamicSLoadResourceKind::Sampler;
		if (!sampler && mapping.kind != ShaderDynamicSLoadResourceKind::Texture) { continue; }
		const int first     = mapping.destination_register - instruction.dst.register_id;
		const int fields    = sampler ? 4 : 8;
		const int resources = sampler ? bind.samplers.samplers_num : bind.textures2D.textures_num;
		if (first < 0 || mapping.dword_count <= 0 || mapping.dword_count > count - first || mapping.resource_index < 0 ||
		    mapping.resource_index >= resources || mapping.resource_field_offset < 0 ||
		    mapping.resource_field_offset > fields - mapping.dword_count)
		{
			if (sampler) { continue; }
			return false;
		}
		for (int word = 0; word < mapping.dword_count; ++word)
		{
			const int field = mapping.resource_field_offset + word;
			(*after)[mapping.destination_register + word] =
			    sampler ? SamplerWord(mapping.resource_index, field) : TextureWord(mapping.resource_index, field);
		}
	}
	if (mapped) { return true; }
	const auto& base = instruction.src[0];
	const auto& offset = instruction.src[1];
	if (!bind.extended.used || base.type != ShaderOperandType::Sgpr || base.size != 2 || !ValidRange(base.register_id, 2) ||
	    base.register_id != bind.extended.start_register || before[base.register_id] != kExtendedLo ||
	    before[base.register_id + 1] != kExtendedHi || !Plain(base) || !Plain(offset) || instruction.smem_imm_offset != 0 ||
	    (offset.type != ShaderOperandType::IntegerInlineConstant && offset.type != ShaderOperandType::LiteralConstant))
	{
		return true;
	}
	const uint32_t first = offset.constant.u >> 2u;
	for (int word = 0; word < count; ++word)
	{
		(*after)[instruction.dst.register_id + word] = ExtendedWord(bind, first + static_cast<uint32_t>(word));
	}
	return true;
}

bool Transfer(const ShaderInstruction& instruction, const ShaderBindResources& bind, const Origins& before, Origins* after)
{
	*after = before;
	if (!Invalidate(instruction.dst, after) || !Invalidate(instruction.dst2, after)) { return false; }
	const int copy_words = instruction.type == ShaderInstructionType::SMovB32 ? 1 :
	                       instruction.type == ShaderInstructionType::SMovB64 ? 2 : 0;
	if (copy_words == 0) { return TransferLoad(instruction, bind, before, after); }
	if (instruction.dst.type != ShaderOperandType::Sgpr || instruction.dst.size != copy_words || instruction.src_num != 1 ||
	    instruction.src[0].type != ShaderOperandType::Sgpr || instruction.src[0].size != copy_words ||
	    !ValidRange(instruction.src[0].register_id, copy_words) || !Plain(instruction.dst) || !Plain(instruction.src[0]))
	{
		return true;
	}
	// Read the old state so overlapping scalar copies retain simultaneous reads.
	for (int word = 0; word < copy_words; ++word)
	{
		(*after)[instruction.dst.register_id + word] = before[instruction.src[0].register_id + word];
	}
	return true;
}

struct FlowState
{
	Origins words {};
	bool reachable = false;
	bool queued = false;
};

void Merge(uint32_t index, const Origins& outgoing, std::vector<FlowState>* states, std::vector<uint32_t>* work)
{
	auto& state = (*states)[index];
	bool changed = !state.reachable;
	if (!state.reachable) { state.words = outgoing; }
	for (int word = 0; word < kSgprCount; ++word)
	{
		if (state.words[word] == kUnknown || state.words[word] == outgoing[word]) { continue; }
		state.words[word] = kUnknown;
		changed = true;
	}
	state.reachable = true;
	if (changed && !state.queued)
	{
		state.queued = true;
		work->push_back(index);
	}
}

bool Analyze(const ShaderCode& code, const ShaderBindResources& bind, int shift, std::vector<FlowState>* states)
{
	const auto& instructions = code.GetInstructions();
	std::unordered_map<uint32_t, uint32_t> by_pc;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		if (!by_pc.emplace(instructions.At(index).pc, index).second) { return false; }
	}
	states->resize(instructions.Size());
	Origins entry;
	if (!InitialOrigins(bind, shift, &entry)) { return false; }
	std::vector<uint32_t> work;
	Merge(0, entry, states, &work);
	while (!work.empty())
	{
		const uint32_t index = work.back();
		work.pop_back();
		(*states)[index].queued = false;
		const auto& instruction = instructions.At(index);
		Origins outgoing;
		if (!Transfer(instruction, bind, (*states)[index].words, &outgoing)) { return false; }
		if (ShaderInstructionHasStaticBranchTarget(instruction.type))
		{
			const auto target = by_pc.find(ShaderLabel(instruction).GetDst());
			if (target == by_pc.end()) { return false; }
			Merge(target->second, outgoing, states, &work);
			if (instruction.type == ShaderInstructionType::SBranch) { continue; }
		}
		if (instruction.type == ShaderInstructionType::SEndpgm) { continue; }
		if (instruction.type == ShaderInstructionType::SSetpcB64 || instruction.type == ShaderInstructionType::SSwappcB64 ||
		    index + 1u >= instructions.Size()) { return false; }
		Merge(index + 1u, outgoing, states, &work);
	}
	return true;
}

bool SameDescriptor(const ShaderTextureResource& left, const ShaderTextureResource& right)
{
	for (int word = 0; word < 8; ++word)
	{
		if (left.fields[word] != right.fields[word]) { return false; }
	}
	return true;
}

int DescriptorWithUsage(int descriptor, const ShaderBindResources& bind, ShaderTextureUsage usage)
{
	if (bind.textures2D.desc[descriptor].usage == usage) { return descriptor; }
	// A single guest T# can have separate sampled and storage Vulkan bindings.
	// Preserve its complete guest identity when selecting the binding.
	int match = -1;
	for (int candidate = 0; candidate < bind.textures2D.textures_num; ++candidate)
	{
		if (bind.textures2D.desc[candidate].usage != usage ||
		    !SameDescriptor(bind.textures2D.desc[descriptor].texture, bind.textures2D.desc[candidate].texture)) { continue; }
		if (match >= 0) { return -1; }
		match = candidate;
	}
	return match;
}

int DescriptorAt(const Origins& origins, int first, const ShaderBindResources& bind, ShaderTextureUsage usage)
{
	const int origin = origins[first];
	if (origin < 0 || origin >= bind.textures2D.textures_num * 8 || origin % 8 != 0) { return -1; }
	for (int word = 1; word < 8; ++word)
	{
		if (origins[first + word] != origin + word) { return -1; }
	}
	return DescriptorWithUsage(origin / 8, bind, usage);
}

int SamplerAt(const Origins& origins, const ShaderOperand& operand, const ShaderBindResources& bind)
{
	if (operand.type != ShaderOperandType::Sgpr || operand.size != 4 || !ValidRange(operand.register_id, 4)) { return -1; }
	const int origin = origins[operand.register_id] - kFirstSamplerWord;
	if (origin < 0 || origin >= bind.samplers.samplers_num * 4 || origin % 4 != 0) { return -1; }
	for (int word = 1; word < 4; ++word)
	{
		if (origins[operand.register_id + word] != origins[operand.register_id] + word) { return -1; }
	}
	return origin / 4;
}

bool ValidBind(const ShaderBindResources& bind, int user_data_register_base)
{
	return user_data_register_base >= 0 && user_data_register_base < kSgprCount && bind.direct_sgprs.sgprs_num >= 0 &&
	       bind.direct_sgprs.sgprs_num <= ShaderDirectSgprsResources::SGPRS_MAX && bind.samplers.samplers_num >= 0 &&
	       bind.samplers.samplers_num <= ShaderSamplerResources::RES_MAX && bind.gds_pointers.pointers_num >= 0 &&
	       bind.gds_pointers.pointers_num <= ShaderGdsResources::POINTERS_MAX && bind.textures2D.textures_num > 0 &&
	       bind.textures2D.textures_num <= ShaderTextureResources::RES_MAX;
}

bool IsUnmodified(const ShaderCode& code, int first)
{
	for (const auto& instruction: code.GetInstructions())
	{
		if (ShaderOperandOverlapsSgprRange(instruction.dst, first, 8) ||
		    ShaderOperandOverlapsSgprRange(instruction.dst2, first, 8)) { return false; }
	}
	return true;
}

} // namespace

int ShaderFindImageStorageTextureDescriptor(const ShaderCode& code, uint32_t index, const ShaderBindResources& bind,
                                            int user_data_register_base)
{
	if (index >= code.GetInstructions().Size() || !ValidBind(bind, user_data_register_base)) { return -1; }
	const auto& instruction = code.GetInstructions().At(index);
	if (instruction.src_num < 2 || instruction.src[1].type != ShaderOperandType::Sgpr || instruction.src[1].size != 8 ||
	    !ValidRange(instruction.src[1].register_id, 8)) { return -1; }
	const int first = instruction.src[1].register_id;
	Origins entry;
	if (!InitialOrigins(bind, user_data_register_base, &entry)) { return -1; }
	const int direct = DescriptorAt(entry, first, bind, ShaderTextureUsage::ReadWrite);
	// An unchanged descriptor has the same origin along every control-flow edge,
	// including indirect edges whose targets this analysis cannot resolve.
	if (direct >= 0 && IsUnmodified(code, first)) { return direct; }
	std::vector<FlowState> states;
	if (!Analyze(code, bind, user_data_register_base, &states) || !states[index].reachable) { return -1; }
	return DescriptorAt(states[index].words, first, bind, ShaderTextureUsage::ReadWrite);
}

void ShaderCollectCopiedImageDescriptors(const ShaderCode& code, ShaderBindResources* bind, int user_data_register_base)
{
	EXIT_IF(bind == nullptr);
	bind->copied_image_descriptors.Clear();
	if (!ValidBind(*bind, user_data_register_base)) { return; }
	const auto& instructions = code.GetInstructions();
	std::vector<FlowState> states;
	bool analyzed = false;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& instruction = instructions.At(index);
		if (!ShaderInstructionReadsImageResource(instruction.type) || instruction.src_num < 2 ||
		    instruction.src[1].type != ShaderOperandType::Sgpr || instruction.src[1].size != 8 ||
		    !ValidRange(instruction.src[1].register_id, 8)) { continue; }
		const bool samples = ShaderInstructionUsesImageSampler(instruction.type);
		const bool texture_found = ShaderFindImageSampledTextureDescriptor(instruction, *bind, user_data_register_base) >= 0;
		const bool sampler_found = !samples || ShaderFindImageSamplerDescriptor(instruction, *bind, user_data_register_base) >= 0;
		if (texture_found && sampler_found) { continue; }
		// One flow pass serves every unresolved read; shaders without copies never run it.
		if (!analyzed && !Analyze(code, *bind, user_data_register_base, &states)) { states.clear(); }
		analyzed = true;
		if (index >= states.size() || !states[index].reachable) { continue; }
		const auto& words = states[index].words;
		const ShaderCopiedImageDescriptor copied {
		    instruction.pc,
		    texture_found ? -1 : DescriptorAt(words, instruction.src[1].register_id, *bind, ShaderTextureUsage::ReadOnly),
		    sampler_found ? -1 : SamplerAt(words, instruction.src[2], *bind)};
		if (copied.texture_index >= 0 || copied.sampler_index >= 0) { bind->copied_image_descriptors.Add(copied); }
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
