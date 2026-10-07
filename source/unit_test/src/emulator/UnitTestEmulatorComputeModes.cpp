#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsGeState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"

#include "../../../emulator/src/Graphics/GraphicsComputeRegisters.h"
#include "../../../emulator/src/Graphics/ShaderDebugInternal.h"

#include <cstddef>

UT_BEGIN(EmulatorComputeModes);

using namespace Libs::Graphics;

namespace {

class ScopedGeneration
{
public:
	explicit ScopedGeneration(bool next_gen)
	{
		if (!Config::IsInitialized())
		{
			Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		}
		m_previous = Config::GetGuestPlatform();
		Config::SetNextGen(next_gen);
	}
	~ScopedGeneration()
	{
		Config::ResetGuestPlatform();
		if (m_previous != GuestPlatform::Unknown)
		{
			EXPECT_TRUE(Config::SetGuestPlatform(m_previous));
		}
	}

	KYTY_CLASS_NO_COPY(ScopedGeneration);

private:
	GuestPlatform m_previous = GuestPlatform::Unknown;
};

// Bounded synthetic legacy metadata with no resources. The header pointer is
// encoded by the first two words; the third is ENDPGM and the fourth a zero
// usage mask. This exercises the real input-info builder without global shader
// map initialization, mapped guest programs or shader/GPU execution.
struct ComputeMetadata
{
	uint32_t         words[4] = {0xbeeb03ffu, 1u, 0xbf810000u, 0u};
	ShaderBinaryInfo header {};

	ComputeMetadata()
	{
		header.length                     = 12u;
		header.chunk_usage_base_offset_dw = 1u;
		header.hash0                      = 0x12345678u;
		header.crc32                      = 0x9abcdef0u;
	}

	HW::ComputeShaderInfo Registers() const
	{
		HW::ComputeShaderInfo regs;
		regs.cs_regs.data_addr    = reinterpret_cast<uint64_t>(words);
		regs.cs_regs.chksum       = 0x123456789abcdef0ull;
		regs.cs_regs.num_thread_x = 1u;
		regs.cs_regs.num_thread_y = 1u;
		regs.cs_regs.num_thread_z = 1u;
		return regs;
	}
};

static_assert(offsetof(ComputeMetadata, header) == 4u * sizeof(uint32_t));

ShaderComputeInputInfo InputFromRegisters(const HW::ComputeShaderInfo& regs)
{
	// Mode propagation precedes generation-specific resource analysis. Use the
	// local resource-free metadata above for that common production path.
	const ScopedGeneration legacy(false);
	ShaderComputeInputInfo info;
	ShaderGetInputInfoCS(&regs, nullptr, 0u, &info);
	return info;
}

ShaderVertexInputInfo VertexInputFromRegisters(const HW::VertexShaderInfo& regs)
{
	// As with CS, exercise the common mode-copy path using local legacy metadata.
	// The decoded provenance remains tied to the platform at the register write.
	const ScopedGeneration legacy(false);
	HW::ShaderRegisters    sh;
	ShaderVertexInputInfo  info;
	ShaderGetInputInfoVS(&regs, &sh, &info, nullptr);
	return info;
}

ShaderPixelInputInfo PixelInputFromRegisters(const HW::PixelShaderInfo& regs)
{
	const ScopedGeneration legacy(false);
	HW::ShaderRegisters    sh;
	ShaderVertexInputInfo  vs_info;
	ShaderPixelInputInfo   info;
	ShaderGetInputInfoPS(&regs, &sh, &vs_info, &info, false);
	return info;
}

void ExpectOneIdentityWordChanged(const ShaderId& before, const ShaderId& after, uint32_t old_word, uint32_t new_word)
{
	EXPECT_EQ(before.hash0, after.hash0);
	EXPECT_EQ(before.crc32, after.crc32);
	ASSERT_EQ(before.ids.Size(), after.ids.Size());
	uint32_t differences = 0;
	for (uint32_t i = 0; i < before.ids.Size(); ++i)
	{
		if (before.ids.At(i) != after.ids.At(i))
		{
			++differences;
			EXPECT_EQ(before.ids.At(i), old_word);
			EXPECT_EQ(after.ids.At(i), new_word);
		}
	}
	EXPECT_EQ(differences, 1u);
	EXPECT_NE(before, after);
}

} // namespace

TEST(EmulatorComputeModes, DefaultsAreUnknownAndAnExplicitZeroWriteIsKnown)
{
	const ScopedGeneration gen5(true);
	HW::CsStageRegisters regs;
	ShaderComputeInputInfo input;
	EXPECT_FALSE(regs.fp_mode_known);
	EXPECT_FALSE(input.fp_mode_known);
	EXPECT_FALSE(regs.fp16_overflow_known);
	EXPECT_FALSE(input.fp16_overflow_known);
	decode_compute_pgm_rsrc2(regs, 0u);
	EXPECT_FALSE(regs.fp_mode_known);
	EXPECT_FALSE(regs.fp16_overflow_known);
	decode_compute_pgm_rsrc1(regs, 0u);
	EXPECT_TRUE(regs.fp_mode_known);
	EXPECT_EQ(regs.float_mode, 0u);
	EXPECT_FALSE(regs.dx10_clamp);
	EXPECT_FALSE(regs.ieee_mode);
	EXPECT_FALSE(regs.fp16_overflow);
	EXPECT_TRUE(regs.fp16_overflow_known);
}

TEST(EmulatorComputeModes, Rsrc1FieldsAreIndependentAndReplacementClearsOldModes)
{
	const ScopedGeneration gen5(true);
	struct Case
	{
		uint32_t word;
		uint8_t  mode;
		bool     clamp;
		bool     ieee;
		bool     overflow;
	};
	const Case cases[] = {
	    {0x000ff000u, 255u, false, false, false},
	    {0x00200000u,   0u, true,  false, false},
	    {0x00800000u,   0u, false, true,  false},
	    {0x04000000u,   0u, false, false, true},
	    {0x04a96000u, 150u, true,  true,  true},
	    {0xfb500fffu,   0u, false, false, false}, // All bits outside these four fields
	    {0x00000000u,   0u, false, false, false},
	};
	HW::CsStageRegisters regs;
	for (const auto& test: cases)
	{
		SCOPED_TRACE(test.word);
		decode_compute_pgm_rsrc1(regs, 0xffffffffu);
		decode_compute_pgm_rsrc1(regs, test.word);
		EXPECT_TRUE(regs.fp_mode_known);
		EXPECT_EQ(regs.float_mode, test.mode);
		EXPECT_EQ(regs.dx10_clamp, test.clamp);
		EXPECT_EQ(regs.ieee_mode, test.ieee);
		EXPECT_EQ(regs.fp16_overflow, test.overflow);
		EXPECT_TRUE(regs.fp16_overflow_known);
	}
}

TEST(EmulatorComputeModes, PackedRegisterCopyAndIndividualWritesKeepTheSameRecordedModes)
{
	const ScopedGeneration gen5(true);
	HW::CsStageRegisters packed;
	decode_compute_pgm_rsrc1(packed, 0x05a960d5u);
	HW::Shader packed_shader;
	packed_shader.SetCsShader(packed, 0u);
	HW::Shader individual_shader;
	decode_compute_pgm_rsrc1(individual_shader.CsRegs(), 0x05a960d5u);
	for (const auto* shader: {&packed_shader, &individual_shader})
	{
		const auto& regs = shader->GetCs().cs_regs;
		EXPECT_TRUE(regs.fp_mode_known);
		EXPECT_EQ(regs.float_mode, 0x96u);
		EXPECT_TRUE(regs.dx10_clamp);
		EXPECT_TRUE(regs.ieee_mode);
		EXPECT_TRUE(regs.fp16_overflow);
		EXPECT_TRUE(regs.fp16_overflow_known);
		EXPECT_EQ(regs.vgprs, 21u);
		EXPECT_EQ(regs.sgprs, 3u);
		EXPECT_EQ(regs.bulky, 1u);
	}
	const HW::Shader copied = packed_shader;
	packed_shader.Reset();
	EXPECT_FALSE(packed_shader.GetCs().cs_regs.fp_mode_known);
	EXPECT_FALSE(packed_shader.GetCs().cs_regs.fp16_overflow_known);
	EXPECT_TRUE(copied.GetCs().cs_regs.fp_mode_known);
	EXPECT_TRUE(copied.GetCs().cs_regs.fp16_overflow);
	EXPECT_TRUE(copied.GetCs().cs_regs.fp16_overflow_known);
	EXPECT_EQ(copied.GetCs().cs_regs.float_mode, 0x96u);
}

TEST(EmulatorComputeModes, EveryFloatModeReachesInputInfoAndChangesExactlyOneIdentityWord)
{
	const ComputeMetadata metadata;
	auto                  regs = metadata.Registers();
	for (bool next_gen: {false, true})
	{
		const ScopedGeneration generation(next_gen);
		decode_compute_pgm_rsrc1(regs.cs_regs, 0u);
		const auto baseline_input = InputFromRegisters(regs);
		ASSERT_TRUE(baseline_input.fp_mode_known);
		const auto baseline = ShaderGetIdCS(&regs, &baseline_input);
		for (uint32_t mode = 1u; mode <= 255u; ++mode)
		{
			SCOPED_TRACE(mode);
			decode_compute_pgm_rsrc1(regs.cs_regs, mode << 12u);
			const auto input = InputFromRegisters(regs);
			EXPECT_TRUE(input.fp_mode_known);
			EXPECT_EQ(input.float_mode, mode);
			EXPECT_FALSE(input.dx10_clamp);
			EXPECT_FALSE(input.ieee_mode);
			ExpectOneIdentityWordChanged(baseline, ShaderGetIdCS(&regs, &input), 0u, mode);
		}
		decode_compute_pgm_rsrc1(regs.cs_regs, 0u);
		const auto restored = InputFromRegisters(regs);
		EXPECT_EQ(ShaderGetIdCS(&regs, &restored), baseline);
	}
}

TEST(EmulatorComputeModes, ClampAndIeeeReachInputInfoAndHaveSeparateIdentityWords)
{
	const ComputeMetadata metadata;
	auto                  regs = metadata.Registers();
	for (bool next_gen: {false, true})
	{
		const ScopedGeneration generation(next_gen);
		decode_compute_pgm_rsrc1(regs.cs_regs, 0x000c0000u);
		const auto baseline_input = InputFromRegisters(regs);
		const auto baseline = ShaderGetIdCS(&regs, &baseline_input);
		decode_compute_pgm_rsrc1(regs.cs_regs, 0x002c0000u);
		const auto clamp = InputFromRegisters(regs);
		EXPECT_EQ(clamp.float_mode, 0xc0u);
		EXPECT_TRUE(clamp.fp_mode_known);
		EXPECT_TRUE(clamp.dx10_clamp);
		EXPECT_FALSE(clamp.ieee_mode);
		const auto clamp_id = ShaderGetIdCS(&regs, &clamp);
		ExpectOneIdentityWordChanged(baseline, clamp_id, 0u, 1u);
		decode_compute_pgm_rsrc1(regs.cs_regs, 0x008c0000u);
		const auto ieee = InputFromRegisters(regs);
		EXPECT_EQ(ieee.float_mode, 0xc0u);
		EXPECT_TRUE(ieee.fp_mode_known);
		EXPECT_FALSE(ieee.dx10_clamp);
		EXPECT_TRUE(ieee.ieee_mode);
		const auto ieee_id = ShaderGetIdCS(&regs, &ieee);
		ExpectOneIdentityWordChanged(baseline, ieee_id, 0u, 1u);
		EXPECT_NE(clamp_id, ieee_id);
	}
}

TEST(EmulatorComputeModes, InputInfoReplacesStaleModesAndIdentitySeparatesUnknownFromZero)
{
	const ScopedGeneration legacy(false);
	const ComputeMetadata  metadata;
	auto                   regs = metadata.Registers();
	ShaderComputeInputInfo  input;
	input.float_mode    = 0xffu;
	input.dx10_clamp    = true;
	input.ieee_mode     = true;
	input.fp_mode_known = true;
	input.fp16_overflow       = true;
	input.fp16_overflow_known = true;
	ShaderGetInputInfoCS(&regs, nullptr, 0u, &input);
	EXPECT_EQ(input.float_mode, 0u);
	EXPECT_FALSE(input.dx10_clamp);
	EXPECT_FALSE(input.ieee_mode);
	EXPECT_FALSE(input.fp_mode_known);
	EXPECT_FALSE(input.fp16_overflow);
	EXPECT_FALSE(input.fp16_overflow_known);
	const auto unknown = ShaderGetIdCS(&regs, &input);
	decode_compute_pgm_rsrc1(regs.cs_regs, 0u);
	ShaderGetInputInfoCS(&regs, nullptr, 0u, &input);
	EXPECT_TRUE(input.fp_mode_known);
	EXPECT_EQ(input.float_mode, 0u);
	EXPECT_FALSE(input.fp16_overflow_known); // Legacy RSRC1 does not establish this GFX9+ field.
	ExpectOneIdentityWordChanged(unknown, ShaderGetIdCS(&regs, &input), 0u, 1u);
}

TEST(EmulatorComputeModes, Fp16OverflowValueAndProvenanceChangeSeparateComputeIdentityWords)
{
	const ScopedGeneration gen5(true);
	const ComputeMetadata  metadata;
	auto                   regs = metadata.Registers();
	decode_compute_pgm_rsrc1(regs.cs_regs, 0x00a96000u);
	const auto clear = InputFromRegisters(regs);
	EXPECT_FALSE(clear.fp16_overflow);
	ASSERT_TRUE(clear.fp16_overflow_known);
	decode_compute_pgm_rsrc1(regs.cs_regs, 0x04a96000u);
	decode_compute_pgm_rsrc2(regs.cs_regs, 0u); // RSRC2 cannot erase the recorded mode.
	const auto set = InputFromRegisters(regs);
	EXPECT_TRUE(set.fp16_overflow);
	EXPECT_TRUE(set.fp16_overflow_known);
	EXPECT_EQ(set.float_mode, 0x96u);
	EXPECT_TRUE(set.dx10_clamp);
	EXPECT_TRUE(set.ieee_mode);
	decode_compute_pgm_rsrc1(regs.cs_regs, 0x00a96000u);
	const auto replaced = InputFromRegisters(regs);
	EXPECT_FALSE(replaced.fp16_overflow);
	EXPECT_TRUE(replaced.fp16_overflow_known);
	{
		const ScopedGeneration legacy(false);
		decode_compute_pgm_rsrc1(regs.cs_regs, 0x00a96000u);
	}
	const auto unknown = InputFromRegisters(regs);
	EXPECT_TRUE(unknown.fp_mode_known);
	EXPECT_FALSE(unknown.fp16_overflow);
	EXPECT_FALSE(unknown.fp16_overflow_known);
	for (bool next_gen: {false, true})
	{
		const ScopedGeneration generation(next_gen);
		const auto clear_id = ShaderGetIdCS(&regs, &clear);
		ExpectOneIdentityWordChanged(clear_id, ShaderGetIdCS(&regs, &set), 0u, 1u);
		ExpectOneIdentityWordChanged(ShaderGetIdCS(&regs, &unknown), clear_id, 0u, 1u);
		EXPECT_EQ(ShaderGetIdCS(&regs, &replaced), clear_id);
	}
}

TEST(EmulatorComputeModes, Fp16OverflowLocationsAndGenerationProvenanceAreIndependent)
{
	const ScopedGeneration restore_platform(true);
	struct Stage
	{
		GraphicsFp16OverflowStage stage;
		uint32_t                 mask;
	};
	const Stage stages[] = {
	    {GraphicsFp16OverflowStage::Compute, 0x04000000u},
	    {GraphicsFp16OverflowStage::Vertex, 0x80000000u},
	    {GraphicsFp16OverflowStage::Geometry, 0x80000000u},
	    {GraphicsFp16OverflowStage::Pixel, 0x20000000u},
	};
	for (auto platform: {GuestPlatform::Ps5, GuestPlatform::Ps4, GuestPlatform::Unknown})
	{
		Config::ResetGuestPlatform();
		if (platform != GuestPlatform::Unknown)
		{
			ASSERT_TRUE(Config::SetGuestPlatform(platform));
		}
		for (const auto& stage: stages)
		{
			for (uint32_t word: {0u, stage.mask, ~stage.mask})
			{
				SCOPED_TRACE(word);
				const auto mode = GraphicsDecodeFp16Overflow(word, stage.stage);
				EXPECT_EQ(mode.enabled, word == stage.mask);
				EXPECT_EQ(mode.known, platform == GuestPlatform::Ps5);
			}
		}
		HW::CsStageRegisters regs;
		decode_compute_pgm_rsrc1(regs, 0x04000000u);
		EXPECT_TRUE(regs.fp16_overflow);
		EXPECT_EQ(regs.fp16_overflow_known, platform == GuestPlatform::Ps5);
		decode_compute_pgm_rsrc1(regs, 0u);
		EXPECT_FALSE(regs.fp16_overflow);
		EXPECT_EQ(regs.fp16_overflow_known, platform == GuestPlatform::Ps5);
	}
}

TEST(EmulatorComputeModes, GsFp16OverflowDirectAndIndirectWritesReplaceRecordedState)
{
	const ScopedGeneration gen5(true);
	HW::Shader direct;
	HW::Shader indirect;
	EXPECT_FALSE(direct.GetVs().gs_regs.rsrc1.fp16_overflow_known);
	for (uint32_t word: {0u, 0x80000000u, 0u})
	{
		ASSERT_TRUE(GraphicsDecodeGeShaderRegisters(&direct, Pm4::SPI_SHADER_PGM_RSRC1_GS, &word, 1u));
		ASSERT_TRUE(GraphicsDecodeGeShaderRegister(indirect, Pm4::SPI_SHADER_PGM_RSRC1_GS, word));
		for (const auto* shader: {&direct, &indirect})
		{
			EXPECT_EQ(shader->GetVs().gs_regs.rsrc1.fp16_overflow, word != 0u);
			EXPECT_TRUE(shader->GetVs().gs_regs.rsrc1.fp16_overflow_known);
			EXPECT_EQ(shader->GetGsShaderResource1Raw().value, word);
			EXPECT_TRUE(shader->GetGsShaderResource1Raw().known);
		}
	}
	HW::GsShaderResource1 typed;
	typed.fp16_overflow = true;
	direct.SetGsShaderResource1(typed);
	EXPECT_TRUE(direct.GetVs().gs_regs.rsrc1.fp16_overflow);
	EXPECT_FALSE(direct.GetVs().gs_regs.rsrc1.fp16_overflow_known);
	EXPECT_FALSE(direct.GetGsShaderResource1Raw().known);
	direct.Reset();
	EXPECT_FALSE(direct.GetVs().gs_regs.rsrc1.fp16_overflow_known);
}

TEST(EmulatorComputeModes, GraphicsFp16OverflowMetadataSelectsTheStageAndKeysBothFields)
{
	const ScopedGeneration gen5(true);
	const ComputeMetadata  metadata;
	const auto address = reinterpret_cast<uint64_t>(metadata.words);
	for (bool gs_instead_of_vs: {false, true})
	{
		HW::VertexShaderInfo regs;
		regs.vs_regs.data_addr = gs_instead_of_vs ? 0u : address;
		regs.es_regs.data_addr = address;
		regs.gs_regs.chksum = 0x123456789abcdef0ull;
		const auto unknown = VertexInputFromRegisters(regs);
		EXPECT_FALSE(unknown.fp16_overflow_known);
		// Populate the same field/provenance pair used by the packed VS producers.
		const auto zero = GraphicsDecodeFp16Overflow(0u, GraphicsFp16OverflowStage::Vertex);
		regs.vs_regs.rsrc1.fp16_overflow       = zero.enabled;
		regs.vs_regs.rsrc1.fp16_overflow_known = zero.known;
		regs.gs_regs.rsrc1 = GraphicsDecodeGsShaderResource1(0u);
		const auto clear = VertexInputFromRegisters(regs);
		EXPECT_FALSE(clear.fp16_overflow);
		EXPECT_TRUE(clear.fp16_overflow_known);
		// The unselected stage is deliberately given the opposite value/provenance.
		if (gs_instead_of_vs)
		{
			regs.gs_regs.rsrc1 = GraphicsDecodeGsShaderResource1(0x80000000u);
			regs.vs_regs.rsrc1 = {};
		} else
		{
			const auto mode = GraphicsDecodeFp16Overflow(0x80000000u, GraphicsFp16OverflowStage::Vertex);
			regs.vs_regs.rsrc1.fp16_overflow       = mode.enabled;
			regs.vs_regs.rsrc1.fp16_overflow_known = mode.known;
			regs.gs_regs.rsrc1 = {};
		}
		const auto set = VertexInputFromRegisters(regs);
		EXPECT_TRUE(set.fp16_overflow);
		EXPECT_TRUE(set.fp16_overflow_known);
		const auto clear_id = ShaderGetIdVS(&regs, &clear);
		ExpectOneIdentityWordChanged(ShaderGetIdVS(&regs, &unknown), clear_id, 0u, 1u);
		ExpectOneIdentityWordChanged(clear_id, ShaderGetIdVS(&regs, &set), 0u, 1u);
		regs.vs_regs.rsrc1.fp16_overflow       = zero.enabled;
		regs.vs_regs.rsrc1.fp16_overflow_known = zero.known;
		regs.gs_regs.rsrc1 = GraphicsDecodeGsShaderResource1(0u);
		const auto replaced = VertexInputFromRegisters(regs);
		EXPECT_FALSE(replaced.fp16_overflow);
		EXPECT_TRUE(replaced.fp16_overflow_known);
		EXPECT_EQ(ShaderGetIdVS(&regs, &replaced), clear_id);
	}
	HW::PixelShaderInfo regs;
	regs.ps_regs.data_addr = address;
	regs.ps_regs.chksum = 0x123456789abcdef0ull;
	const auto unknown = PixelInputFromRegisters(regs);
	EXPECT_FALSE(unknown.fp16_overflow_known);
	const auto zero = GraphicsDecodeFp16Overflow(0u, GraphicsFp16OverflowStage::Pixel);
	regs.ps_regs.rsrc1.fp16_overflow       = zero.enabled;
	regs.ps_regs.rsrc1.fp16_overflow_known = zero.known;
	const auto clear = PixelInputFromRegisters(regs);
	EXPECT_FALSE(clear.fp16_overflow);
	EXPECT_TRUE(clear.fp16_overflow_known);
	const auto mode = GraphicsDecodeFp16Overflow(0x20000000u, GraphicsFp16OverflowStage::Pixel);
	regs.ps_regs.rsrc1.fp16_overflow       = mode.enabled;
	regs.ps_regs.rsrc1.fp16_overflow_known = mode.known;
	const auto set = PixelInputFromRegisters(regs);
	EXPECT_TRUE(set.fp16_overflow);
	EXPECT_TRUE(set.fp16_overflow_known);
	const auto clear_id = ShaderGetIdPS(&regs, &clear);
	ExpectOneIdentityWordChanged(ShaderGetIdPS(&regs, &unknown), clear_id, 0u, 1u);
	ExpectOneIdentityWordChanged(clear_id, ShaderGetIdPS(&regs, &set), 0u, 1u);
	regs.ps_regs.rsrc1.fp16_overflow       = zero.enabled;
	regs.ps_regs.rsrc1.fp16_overflow_known = zero.known;
	const auto replaced = PixelInputFromRegisters(regs);
	EXPECT_FALSE(replaced.fp16_overflow);
	EXPECT_TRUE(replaced.fp16_overflow_known);
	EXPECT_EQ(ShaderGetIdPS(&regs, &replaced), clear_id);
	regs.ps_regs.rsrc1 = {};
	EXPECT_FALSE(PixelInputFromRegisters(regs).fp16_overflow_known);
}

UT_END();
