#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderResourceFoldCapture.h"
#include "../../../emulator/src/Graphics/ShaderStorageAnalysis.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

UT_BEGIN(EmulatorShaderResourcePlans);

using namespace Libs::Graphics;

namespace {

constexpr uint64_t kFirstTableAddress  = 0x0000001200001000ull;
constexpr uint64_t kSecondTableAddress = 0x0000001300002000ull;
constexpr int      kTextureRegister    = 16;
constexpr int      kSamplerRegister    = 24;
constexpr uint32_t kTextureLoadPc      = 0u;
constexpr uint32_t kSamplerLoadPc      = 4u;
constexpr uint32_t kSamplePc           = 8u;
constexpr int      kSplitSamplerRegister = 16;
constexpr uint32_t kSplitLoadPc          = 0u;
constexpr std::array<uint32_t, 4> kSplitSamplePcs = {4u, 8u, 12u, 16u};

using DescriptorTable = std::array<uint32_t, 12>;
using SplitSamplerTable = std::array<uint32_t, 16>;

DescriptorTable MakeTable(const std::array<uint32_t, 8>& texture, const std::array<uint32_t, 4>& sampler)
{
	DescriptorTable words {};
	std::copy(texture.begin(), texture.end(), words.begin());
	std::copy(sampler.begin(), sampler.end(), words.begin() + 8);
	return words;
}

constexpr std::array<uint32_t, 8> kFirstTexture = {0x00002000u, 0x00000012u, 0x00000024u, 0x90000000u,
	                                               0x00000048u, 0x0000005au, 0x0000006cu, 0x0000007eu};
constexpr std::array<uint32_t, 4> kFirstSampler = {0x00000111u, 0x00000222u, 0x00000333u, 0x00000444u};
constexpr std::array<uint32_t, 8> kSecondTexture = {0x00003000u, 0x00000013u, 0x00000025u, 0x90000000u,
	                                                0x00000049u, 0x0000005bu, 0x0000006du, 0x0000007fu};
constexpr std::array<uint32_t, 4> kSecondSampler = {0x00000555u, 0x00000666u, 0x00000777u, 0x00000888u};
constexpr std::array<uint32_t, 8> kUpdatedTexture = {0x00004000u, 0x00000014u, 0x00000026u, 0x90000000u,
	                                                 0x0000004au, 0x0000005cu, 0x0000006eu, 0x00000080u};
constexpr std::array<uint32_t, 4> kUpdatedSampler = {0x00000999u, 0x00000aaau, 0x00000bbbu, 0x00000cccu};
constexpr SplitSamplerTable kSplitSamplerDescriptors = {
	0x00000111u, 0x00000222u, 0x00000333u, 0x00000444u,
	0x00000555u, 0x00000666u, 0x00000777u, 0x00000888u,
	0x00000999u, 0x00000aaau, 0x00000bbbu, 0x00000cccu,
	0x00000dddu, 0x00000eeeu, 0x00000fffu, 0x00001111u,
};

void EnsurePs5Config()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

ShaderInstruction MakeDescriptorLoad(int pointer_register, int destination_register, int dwords, uint32_t offset_bytes,
	                                 uint32_t pc)
{
	ShaderInstruction load {};
	load.pc                = pc;
	load.type              = dwords == 8 ? ShaderInstructionType::SLoadDwordx8 : ShaderInstructionType::SLoadDwordx4;
	load.format            = dwords == 8 ? ShaderInstructionFormat::Sdst8SbaseSoffset : ShaderInstructionFormat::Sdst4SbaseSoffset;
	load.dst               = {.type = ShaderOperandType::Sgpr, .register_id = destination_register, .size = dwords};
	load.src[0]            = {.type = ShaderOperandType::Sgpr, .register_id = pointer_register, .size = 2};
	load.src[1].type       = ShaderOperandType::IntegerInlineConstant;
	load.src[1].constant.u = offset_bytes;
	load.src_num           = 2;
	return load;
}

ShaderInstruction MakeDescriptorLoadX16(int pointer_register, int destination_register, uint32_t offset_bytes, uint32_t pc)
{
	ShaderInstruction load {};
	load.pc                = pc;
	load.type              = ShaderInstructionType::SLoadDwordx16;
	load.format            = ShaderInstructionFormat::Sdst16SbaseSoffset;
	load.dst               = {.type = ShaderOperandType::Sgpr, .register_id = destination_register, .size = 16};
	load.src[0]            = {.type = ShaderOperandType::Sgpr, .register_id = pointer_register, .size = 2};
	load.src[1].type       = ShaderOperandType::IntegerInlineConstant;
	load.src[1].constant.u = offset_bytes;
	load.src_num           = 2;
	return load;
}

ShaderCode MakeSampleCode(int pointer_register)
{
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	code.GetInstructions().Add(MakeDescriptorLoad(pointer_register, kTextureRegister, 8, 0u, kTextureLoadPc));
	code.GetInstructions().Add(MakeDescriptorLoad(pointer_register, kSamplerRegister, 4, 8u * sizeof(uint32_t), kSamplerLoadPc));

	ShaderInstruction sample {};
	sample.pc      = kSamplePc;
	sample.type    = ShaderInstructionType::ImageSampleDrefLz;
	sample.src[1]  = {.type = ShaderOperandType::Sgpr, .register_id = kTextureRegister, .size = 8};
	sample.src[2]  = {.type = ShaderOperandType::Sgpr, .register_id = kSamplerRegister, .size = 4};
	sample.src_num = 3;
	code.GetInstructions().Add(sample);

	ShaderInstruction end {};
	end.pc     = 12u;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	code.GetInstructions().Add(end);
	return code;
}

ShaderCode MakeFourSamplerSplitCode(int pointer_register)
{
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	code.GetInstructions().Add(MakeDescriptorLoadX16(pointer_register, kSplitSamplerRegister, 0u, kSplitLoadPc));
	for (size_t sampler = 0; sampler < kSplitSamplePcs.size(); ++sampler)
	{
		ShaderInstruction sample {};
		sample.pc      = kSplitSamplePcs[sampler];
		sample.type    = ShaderInstructionType::ImageSampleDrefLz;
		sample.src[1]  = {.type = ShaderOperandType::Sgpr, .register_id = 48, .size = 8};
		sample.src[2]  = {.type = ShaderOperandType::Sgpr, .register_id = kSplitSamplerRegister + static_cast<int>(sampler) * 4, .size = 4};
		sample.src_num = 3;
		code.GetInstructions().Add(sample);
	}

	ShaderInstruction end {};
	end.pc     = 20u;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	code.GetInstructions().Add(end);
	return code;
}

ShaderCode MakePartialSplitCode(int pointer_register)
{
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	code.GetInstructions().Add(MakeDescriptorLoadX16(pointer_register, kSplitSamplerRegister, 0u, kSplitLoadPc));

	ShaderInstruction sample {};
	sample.pc      = kSamplePc;
	sample.type    = ShaderInstructionType::ImageSampleDrefLz;
	sample.src[1]  = {.type = ShaderOperandType::Sgpr, .register_id = kSplitSamplerRegister, .size = 8};
	sample.src[2]  = {.type = ShaderOperandType::Sgpr, .register_id = kSplitSamplerRegister + 8, .size = 4};
	sample.src_num = 3;
	code.GetInstructions().Add(sample);

	ShaderInstruction end {};
	end.pc     = 12u;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	code.GetInstructions().Add(end);
	return code;
}

struct Fixture
{
	std::array<uint16_t, 6> direct_resources {};
	ShaderUserData         user_data {};
	HW::UserSgprInfo       user_sgpr {};
	ShaderCode             code;
	int                    user_sgpr_num = 12;

	explicit Fixture(int pointer_register): code(MakeSampleCode(pointer_register))
	{
		direct_resources.fill(0xffffu);
		user_data.direct_resource_offset = direct_resources.data();
		user_data.direct_resource_count  = static_cast<uint16_t>(direct_resources.size());
		user_data.srt_size_dw            = 2;
	}

	void SetPointer(int pointer_register, uint64_t address)
	{
		user_sgpr.value[pointer_register]     = static_cast<uint32_t>(address);
		user_sgpr.value[pointer_register + 1] = static_cast<uint32_t>(address >> 32u);
	}
};

struct ReadObservation
{
	uint64_t              address = 0;
	uint32_t              dwords  = 0;
	bool                  success = false;
	std::vector<uint32_t> words;
};

class MutableDescriptorReader final: public ShaderGuestDescriptorReader
{
public:
	template <size_t N>
	void SetTable(uint64_t address, const std::array<uint32_t, N>& table)
	{
		m_address = address;
		m_words.assign(table.begin(), table.end());
	}

	[[nodiscard]] bool Read(uint64_t guest_address, uint32_t dwords, uint32_t* words) override
	{
		ReadObservation observation {};
		observation.address = guest_address;
		observation.dwords  = dwords;
		observation.success = guest_address == m_address && words != nullptr && dwords <= m_words.size();
		if (observation.success)
		{
			std::copy_n(m_words.begin(), dwords, words);
			observation.words.assign(m_words.begin(), m_words.begin() + dwords);
		}
		m_reads.push_back(std::move(observation));
		return m_reads.back().success;
	}

	[[nodiscard]] const std::vector<ReadObservation>& Reads() const { return m_reads; }

private:
	uint64_t                     m_address = 0;
	std::vector<uint32_t>       m_words;
	std::vector<ReadObservation> m_reads;
};

struct ParseOutput
{
	ShaderParsedUsage usage {};
	ShaderBindResources bind {};
};

std::unique_ptr<ParseOutput> Parse(Fixture& fixture, MutableDescriptorReader& reader, int user_data_register_base,
	                              const ShaderCodeResourceMemo* memo)
{
	auto output = std::make_unique<ParseOutput>();
	const ScopedShaderGuestDescriptorReader scope(&reader);
	ShaderParseUsage2(&fixture.user_data, &output->usage, &output->bind, fixture.user_sgpr, fixture.user_sgpr_num,
	                  &fixture.code, user_data_register_base, false, nullptr, nullptr, nullptr, memo);
	return output;
}

std::string CanonicalOutput(const ParseOutput& output)
{
	return ShaderResourceFoldCanonicalOutput(output.usage, output.bind);
}

std::vector<ReadObservation> ReadsSince(const MutableDescriptorReader& reader, size_t first_read)
{
	const auto& reads = reader.Reads();
	if (first_read > reads.size())
	{
		return {};
	}
	return {reads.begin() + static_cast<std::ptrdiff_t>(first_read), reads.end()};
}

void ExpectReadWindowsEqual(const std::vector<ReadObservation>& first, const std::vector<ReadObservation>& second)
{
	ASSERT_EQ(first.size(), second.size());
	for (size_t index = 0; index < first.size(); ++index)
	{
		EXPECT_EQ(first[index].address, second[index].address);
		EXPECT_EQ(first[index].dwords, second[index].dwords);
		EXPECT_EQ(first[index].success, second[index].success);
		EXPECT_EQ(first[index].words, second[index].words);
	}
}

void ExpectStableReadsSince(const MutableDescriptorReader& reader, size_t first_read, uint64_t expected_address,
	                       std::initializer_list<uint32_t> expected_dwords)
{
	const auto& reads = reader.Reads();
	ASSERT_LE(first_read, reads.size());
	const size_t count = reads.size() - first_read;
	ASSERT_EQ(count, expected_dwords.size());
	ASSERT_EQ(count % 2u, 0u);
	auto expected = expected_dwords.begin();
	for (size_t index = first_read; index < reads.size(); index += 2u)
	{
		const auto& first        = reads[index];
		const auto& verification = reads[index + 1u];
		EXPECT_TRUE(first.success);
		EXPECT_TRUE(verification.success);
		EXPECT_EQ(first.address, expected_address);
		EXPECT_EQ(verification.address, expected_address);
		EXPECT_EQ(first.dwords, *expected++);
		EXPECT_EQ(verification.dwords, *expected++);
		EXPECT_EQ(first.words, verification.words);
	}
}

void ExpectNoSampledDescriptors(const ParseOutput& output)
{
	EXPECT_EQ(output.usage.textures2D_readonly, 0);
	EXPECT_EQ(output.usage.samplers, 0);
	EXPECT_EQ(output.bind.textures2D.textures_num, 0);
	EXPECT_EQ(output.bind.samplers.samplers_num, 0);
	EXPECT_EQ(output.bind.dynamic_sloads.records.Size(), 0u);
}

void ExpectSampledDescriptors(const ParseOutput& output, const std::array<uint32_t, 8>& expected_texture,
	                         const std::array<uint32_t, 4>& expected_sampler)
{
	ASSERT_EQ(output.usage.textures2D_readonly, 1);
	ASSERT_EQ(output.usage.samplers, 1);
	ASSERT_EQ(output.bind.textures2D.textures_num, 1);
	ASSERT_EQ(output.bind.samplers.samplers_num, 1);
	ASSERT_EQ(output.bind.dynamic_sloads.records.Size(), 2u);
	for (int field = 0; field < static_cast<int>(expected_texture.size()); ++field)
	{
		EXPECT_EQ(output.bind.textures2D.desc[0].texture.fields[field], expected_texture[static_cast<size_t>(field)]) << field;
	}
	for (int field = 0; field < static_cast<int>(expected_sampler.size()); ++field)
	{
		EXPECT_EQ(output.bind.samplers.samplers[0].fields[field], expected_sampler[static_cast<size_t>(field)]) << field;
	}

	const auto& texture_mapping = output.bind.dynamic_sloads.records.At(0);
	EXPECT_EQ(texture_mapping.kind, ShaderDynamicSLoadResourceKind::Texture);
	EXPECT_EQ(texture_mapping.resource_index, 0);
	EXPECT_EQ(texture_mapping.destination_register, kTextureRegister);
	EXPECT_EQ(texture_mapping.instruction_pc, kTextureLoadPc);
	EXPECT_EQ(texture_mapping.offset_dw, 0);
	EXPECT_EQ(texture_mapping.dword_count, 8);
	EXPECT_EQ(texture_mapping.last_consumer_pc, kSamplePc);

	const auto& sampler_mapping = output.bind.dynamic_sloads.records.At(1);
	EXPECT_EQ(sampler_mapping.kind, ShaderDynamicSLoadResourceKind::Sampler);
	EXPECT_EQ(sampler_mapping.resource_index, 0);
	EXPECT_EQ(sampler_mapping.destination_register, kSamplerRegister);
	EXPECT_EQ(sampler_mapping.instruction_pc, kSamplerLoadPc);
	EXPECT_EQ(sampler_mapping.offset_dw, 8);
	EXPECT_EQ(sampler_mapping.dword_count, 4);
	EXPECT_EQ(sampler_mapping.last_consumer_pc, kSamplePc);
}

void ExpectFourSplitSamplerDescriptors(const ParseOutput& output)
{
	ASSERT_EQ(output.usage.samplers, 4);
	ASSERT_EQ(output.bind.samplers.samplers_num, 4);
	ASSERT_EQ(output.bind.dynamic_sloads.records.Size(), 4u);
	for (size_t sampler = 0; sampler < kSplitSamplePcs.size(); ++sampler)
	{
		for (size_t field = 0; field < 4u; ++field)
		{
			EXPECT_EQ(output.bind.samplers.samplers[sampler].fields[field], kSplitSamplerDescriptors[sampler * 4u + field])
			    << "sampler=" << sampler << " field=" << field;
		}

		const auto& mapping = output.bind.dynamic_sloads.records.At(static_cast<uint32_t>(sampler));
		EXPECT_EQ(mapping.kind, ShaderDynamicSLoadResourceKind::Sampler);
		EXPECT_EQ(mapping.resource_index, static_cast<int>(sampler));
		EXPECT_EQ(mapping.destination_register, kSplitSamplerRegister + static_cast<int>(sampler) * 4);
		EXPECT_EQ(mapping.instruction_pc, kSplitLoadPc);
		EXPECT_EQ(mapping.offset_dw, static_cast<int>(sampler) * 4);
		EXPECT_EQ(mapping.dword_count, 4);
		EXPECT_EQ(mapping.last_consumer_pc, kSplitSamplePcs[sampler]);
	}
}

} // namespace

TEST(EmulatorShaderResourcePlans, MemoizedPlanUsesCurrentSrtPointerAndDescriptorSnapshot)
{
	EnsurePs5Config();
	Fixture fixture(0);
	fixture.SetPointer(0, kFirstTableAddress);
	MutableDescriptorReader reader;
	reader.SetTable(kFirstTableAddress, MakeTable(kFirstTexture, kFirstSampler));
	ShaderCodeResourceMemo memo;

	size_t first_read = reader.Reads().size();
	const auto uncached_first = Parse(fixture, reader, 0, nullptr);
	ExpectStableReadsSince(reader, first_read, kFirstTableAddress, {8u, 8u, 12u, 12u});
	ExpectSampledDescriptors(*uncached_first, kFirstTexture, kFirstSampler);
	const auto uncached_first_reads = ReadsSince(reader, first_read);

	first_read = reader.Reads().size();
	const auto cached_first = Parse(fixture, reader, 0, &memo);
	ExpectStableReadsSince(reader, first_read, kFirstTableAddress, {8u, 8u, 12u, 12u});
	ExpectSampledDescriptors(*cached_first, kFirstTexture, kFirstSampler);
	EXPECT_EQ(CanonicalOutput(*cached_first), CanonicalOutput(*uncached_first));
	ExpectReadWindowsEqual(uncached_first_reads, ReadsSince(reader, first_read));

	// Caching the guest pointer or descriptor words with the code would reuse the
	// first dispatch's values here; caching the whole parse would also skip reads.
	// Reusing the immutable code and memo must still read the current pointer
	// pair and current descriptor words for each draw.
	fixture.SetPointer(0, kSecondTableAddress);
	reader.SetTable(kSecondTableAddress, MakeTable(kSecondTexture, kSecondSampler));
	first_read = reader.Reads().size();
	const auto cached_second = Parse(fixture, reader, 0, &memo);
	ExpectStableReadsSince(reader, first_read, kSecondTableAddress, {8u, 8u, 12u, 12u});
	ExpectSampledDescriptors(*cached_second, kSecondTexture, kSecondSampler);
	const auto cached_second_reads = ReadsSince(reader, first_read);
	first_read = reader.Reads().size();
	const auto uncached_second = Parse(fixture, reader, 0, nullptr);
	ExpectStableReadsSince(reader, first_read, kSecondTableAddress, {8u, 8u, 12u, 12u});
	EXPECT_EQ(CanonicalOutput(*cached_second), CanonicalOutput(*uncached_second));
	ExpectReadWindowsEqual(cached_second_reads, ReadsSince(reader, first_read));

	// With the pointer unchanged, new table contents must replace the previous
	// binding instead of reusing the first snapshot associated with that address.
	reader.SetTable(kSecondTableAddress, MakeTable(kUpdatedTexture, kUpdatedSampler));
	first_read = reader.Reads().size();
	const auto cached_updated = Parse(fixture, reader, 0, &memo);
	ExpectStableReadsSince(reader, first_read, kSecondTableAddress, {8u, 8u, 12u, 12u});
	ExpectSampledDescriptors(*cached_updated, kUpdatedTexture, kUpdatedSampler);
	EXPECT_NE(CanonicalOutput(*cached_second), CanonicalOutput(*cached_updated));
	const auto cached_updated_reads = ReadsSince(reader, first_read);
	first_read = reader.Reads().size();
	const auto uncached_updated = Parse(fixture, reader, 0, nullptr);
	ExpectStableReadsSince(reader, first_read, kSecondTableAddress, {8u, 8u, 12u, 12u});
	EXPECT_EQ(CanonicalOutput(*cached_updated), CanonicalOutput(*uncached_updated));
	ExpectReadWindowsEqual(cached_updated_reads, ReadsSince(reader, first_read));
}

TEST(EmulatorShaderResourcePlans, RechecksSrtSpanAndUserDataRegisterBase)
{
	EnsurePs5Config();
	Fixture fixture(8);
	fixture.SetPointer(0, kFirstTableAddress);
	MutableDescriptorReader reader;
	reader.SetTable(kFirstTableAddress, MakeTable(kFirstTexture, kFirstSampler));
	ShaderCodeResourceMemo memo;

	// Caching this eligibility result with the code, without rechecking the
	// dispatch's SRT span and register base, would either keep descriptors
	// filtered after they become eligible or bind them after they stop fitting.
	fixture.user_data.srt_size_dw = 0;
	const size_t no_srt_read = reader.Reads().size();
	const auto   no_srt       = Parse(fixture, reader, 8, &memo);
	ExpectNoSampledDescriptors(*no_srt);
	EXPECT_EQ(reader.Reads().size(), no_srt_read);

	fixture.user_data.srt_size_dw = 2;
	size_t first_read = reader.Reads().size();
	const auto eligible = Parse(fixture, reader, 8, &memo);
	ExpectStableReadsSince(reader, first_read, kFirstTableAddress, {8u, 8u, 12u, 12u});
	ExpectSampledDescriptors(*eligible, kFirstTexture, kFirstSampler);

	first_read = reader.Reads().size();
	const auto wrong_base = Parse(fixture, reader, 0, &memo);
	ExpectNoSampledDescriptors(*wrong_base);
	EXPECT_EQ(reader.Reads().size(), first_read);

	fixture.user_data.srt_size_dw = 1;
	const auto short_srt = Parse(fixture, reader, 8, &memo);
	ExpectNoSampledDescriptors(*short_srt);
	EXPECT_EQ(reader.Reads().size(), first_read);

	fixture.user_data.srt_size_dw = 2;
	first_read = reader.Reads().size();
	const auto eligible_again = Parse(fixture, reader, 8, &memo);
	ExpectStableReadsSince(reader, first_read, kFirstTableAddress, {8u, 8u, 12u, 12u});
	ExpectSampledDescriptors(*eligible_again, kFirstTexture, kFirstSampler);
}

TEST(EmulatorShaderResourcePlans, RechecksEudPointerEligibilityForTheSameCode)
{
	EnsurePs5Config();
	Fixture fixture(8);
	fixture.SetPointer(8, kFirstTableAddress);
	fixture.direct_resources[k_gen5_eud_direct_type] = 8u;
	fixture.user_data.srt_size_dw                = 0;
	fixture.user_data.eud_size_dw                = 0;
	MutableDescriptorReader reader;
	reader.SetTable(kFirstTableAddress, MakeTable(kFirstTexture, kFirstSampler));
	ShaderCodeResourceMemo memo;

	// Caching the EUD pointer eligibility bit with the code would retain the
	// disabled state after eud_size_dw becomes nonzero, or keep it enabled after
	// the dispatch removes the EUD span.
	size_t first_read = reader.Reads().size();
	const auto no_eud = Parse(fixture, reader, 0, &memo);
	ExpectNoSampledDescriptors(*no_eud);
	EXPECT_FALSE(no_eud->bind.extended.used);
	EXPECT_EQ(reader.Reads().size(), first_read);

	fixture.user_data.eud_size_dw = 12;
	first_read = reader.Reads().size();
	const auto with_eud = Parse(fixture, reader, 0, &memo);
	ExpectStableReadsSince(reader, first_read, kFirstTableAddress, {12u, 12u});
	EXPECT_TRUE(with_eud->bind.extended.used);
	ExpectSampledDescriptors(*with_eud, kFirstTexture, kFirstSampler);

	fixture.user_data.eud_size_dw = 0;
	first_read = reader.Reads().size();
	const auto eud_removed = Parse(fixture, reader, 0, &memo);
	ExpectNoSampledDescriptors(*eud_removed);
	EXPECT_FALSE(eud_removed->bind.extended.used);
	EXPECT_EQ(reader.Reads().size(), first_read);

	fixture.user_data.eud_size_dw = 12;
	first_read = reader.Reads().size();
	const auto eud_restored = Parse(fixture, reader, 0, &memo);
	ExpectStableReadsSince(reader, first_read, kFirstTableAddress, {12u, 12u});
	EXPECT_TRUE(eud_restored->bind.extended.used);
	ExpectSampledDescriptors(*eud_restored, kFirstTexture, kFirstSampler);
}

TEST(EmulatorShaderResourcePlans, EudPointerAliasSuppressesWarmSrtPlanReads)
{
	EnsurePs5Config();
	Fixture fixture(8);
	fixture.SetPointer(8, kFirstTableAddress);
	fixture.user_data.direct_resource_offset[k_gen5_eud_direct_type] = 8u;
	fixture.user_data.srt_size_dw                              = 10;
	ShaderCodeResourceMemo memo;

	// First cache the SRT recipe while the aliased type-5 pointer is not yet an
	// EUD pointer. The later dispatch keeps the SRT span but enables EUD use.
	MutableDescriptorReader srt_reader;
	srt_reader.SetTable(kFirstTableAddress, MakeTable(kFirstTexture, kFirstSampler));
	const auto srt_output = Parse(fixture, srt_reader, 0, &memo);
	ExpectStableReadsSince(srt_reader, 0, kFirstTableAddress, {8u, 8u, 12u, 12u});
	ExpectSampledDescriptors(*srt_output, kFirstTexture, kFirstSampler);

	fixture.user_data.eud_size_dw = 12;
	MutableDescriptorReader direct_reader;
	direct_reader.SetTable(kFirstTableAddress, MakeTable(kFirstTexture, kFirstSampler));
	const auto direct_eud = Parse(fixture, direct_reader, 0, nullptr);
	ExpectStableReadsSince(direct_reader, 0, kFirstTableAddress, {12u, 12u});
	ExpectSampledDescriptors(*direct_eud, kFirstTexture, kFirstSampler);
	const auto direct_reads = ReadsSince(direct_reader, 0);

	MutableDescriptorReader warm_reader;
	warm_reader.SetTable(kFirstTableAddress, MakeTable(kFirstTexture, kFirstSampler));
	const auto warm_eud = Parse(fixture, warm_reader, 0, &memo);
	ExpectStableReadsSince(warm_reader, 0, kFirstTableAddress, {12u, 12u});
	ExpectSampledDescriptors(*warm_eud, kFirstTexture, kFirstSampler);
	EXPECT_EQ(CanonicalOutput(*warm_eud), CanonicalOutput(*direct_eud));
	ExpectReadWindowsEqual(direct_reads, ReadsSince(warm_reader, 0));
}

TEST(EmulatorShaderResourcePlans, X16SplitSamplerPartitionsMatchDirectColdAndWarmParses)
{
	EnsurePs5Config();
	Fixture fixture(0);
	fixture.code = MakeFourSamplerSplitCode(0);
	fixture.SetPointer(0, kFirstTableAddress);

	MutableDescriptorReader direct_reader;
	direct_reader.SetTable(kFirstTableAddress, kSplitSamplerDescriptors);
	const auto direct = Parse(fixture, direct_reader, 0, nullptr);
	ExpectStableReadsSince(direct_reader, 0, kFirstTableAddress, {16u, 16u});
	ExpectFourSplitSamplerDescriptors(*direct);
	const auto direct_reads = ReadsSince(direct_reader, 0);

	MutableDescriptorReader memo_reader;
	memo_reader.SetTable(kFirstTableAddress, kSplitSamplerDescriptors);
	ShaderCodeResourceMemo memo;
	const auto cold = Parse(fixture, memo_reader, 0, &memo);
	ExpectStableReadsSince(memo_reader, 0, kFirstTableAddress, {16u, 16u});
	ExpectFourSplitSamplerDescriptors(*cold);
	EXPECT_EQ(CanonicalOutput(*cold), CanonicalOutput(*direct));
	const auto cold_reads = ReadsSince(memo_reader, 0);
	ExpectReadWindowsEqual(direct_reads, cold_reads);

	const size_t warm_first_read = memo_reader.Reads().size();
	const auto warm = Parse(fixture, memo_reader, 0, &memo);
	ExpectStableReadsSince(memo_reader, warm_first_read, kFirstTableAddress, {16u, 16u});
	ExpectFourSplitSamplerDescriptors(*warm);
	EXPECT_EQ(CanonicalOutput(*warm), CanonicalOutput(*direct));
	ExpectReadWindowsEqual(direct_reads, ReadsSince(memo_reader, warm_first_read));
}

TEST(EmulatorShaderResourcePlans, X16SplitWithIncompletePartitionDoesNotReadOrBind)
{
	EnsurePs5Config();
	Fixture fixture(0);
	fixture.code = MakePartialSplitCode(0);
	fixture.SetPointer(0, kFirstTableAddress);

	MutableDescriptorReader direct_reader;
	direct_reader.SetTable(kFirstTableAddress, kSplitSamplerDescriptors);
	const auto direct = Parse(fixture, direct_reader, 0, nullptr);
	ExpectNoSampledDescriptors(*direct);
	EXPECT_TRUE(direct_reader.Reads().empty());

	MutableDescriptorReader memo_reader;
	memo_reader.SetTable(kFirstTableAddress, kSplitSamplerDescriptors);
	ShaderCodeResourceMemo memo;
	const auto cold = Parse(fixture, memo_reader, 0, &memo);
	ExpectNoSampledDescriptors(*cold);
	EXPECT_TRUE(memo_reader.Reads().empty());
	EXPECT_EQ(CanonicalOutput(*cold), CanonicalOutput(*direct));

	const auto warm = Parse(fixture, memo_reader, 0, &memo);
	ExpectNoSampledDescriptors(*warm);
	EXPECT_TRUE(memo_reader.Reads().empty());
	EXPECT_EQ(CanonicalOutput(*warm), CanonicalOutput(*direct));
}

UT_END();
