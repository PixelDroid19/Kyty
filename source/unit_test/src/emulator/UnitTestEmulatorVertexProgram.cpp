#include "Kyty/UnitTest.h"

#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/GraphicsGeState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveVulkan.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Graphics/ShaderTranslationCache.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

UT_BEGIN(EmulatorVertexProgram);

using namespace Libs::Graphics;

namespace {

void Initialize()
{
	// ShaderInit is process-lifetime state. Each fixture owns one isolated child,
	// including under same-process suite repetition.
	std::fflush(stdout);
#if defined(_WIN32)
	if (::_dup2(::_fileno(stderr), ::_fileno(stdout)) < 0) { std::_Exit(125); }
#else
	if (::dup2(::fileno(stderr), ::fileno(stdout)) < 0) { std::_Exit(125); }
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t GetInteger(const Core::String&) const override { return 0; }
		bool GetBool(const Core::String&) const override { return true; }
		Core::String GetString(const Core::String&) const override { return {}; }
	} validation;
	Config::Load(validation);
	ShaderInit();
}

class MappedProgram
{
public:
	explicit MappedProgram(std::initializer_list<uint32_t> words)
	{
		address = Core::VirtualMemory::Alloc(0, Core::VirtualMemory::GetPageSize(), Core::VirtualMemory::Mode::ReadWrite);
		if (address == 0) { std::_Exit(125); }
		Replace(words);
	}

	~MappedProgram()
	{
		ShaderMapUserData(address, {});
		if (!Core::VirtualMemory::Free(address)) { std::_Exit(125); }
	}

	void Replace(std::initializer_list<uint32_t> words)
	{
		if (!Core::VirtualMemory::CopyToGuest(address, words.begin(), words.size() * sizeof(uint32_t))) { std::_Exit(125); }
		MapSize(static_cast<uint32_t>(words.size() * sizeof(uint32_t)));
	}

	void MapSize(uint32_t bytes)
	{
		ShaderMappedData mapping {};
		mapping.user_data = &m_user;
		mapping.code_size_bytes = bytes;
		ShaderMapUserData(address, mapping);
	}

	uint64_t address = 0;

private:
	ShaderUserData m_user {};
};

struct Draw
{
	Draw(uint64_t front, uint64_t back)
	{
		regs.es_regs.data_addr = front;
		regs.gs_back_addr = back;
		regs.gs_regs.chksum = 0x123456789abcdef0ull;
		stages.SetRaw(0x02002000u);
	}

	ShaderVertexInputInfo Acquire() const
	{
		ShaderVertexInputInfo info {};
		ShaderGetInputInfoVS(&regs, &sh, &info, &stages);
		return info;
	}

	HW::VertexShaderInfo regs {};
	HW::ShaderRegisters sh {};
	GraphicsGeRawRegister stages;
};

void CheckBackProof(uint32_t word, ShaderInstructionType type, ShaderNativeWaveProof expected)
{
	Initialize();
	MappedProgram front({0xbe802000u}); // terminal s_setpc_b64 s[0:1]
	MappedProgram back({word, 0xbf810000u});
	ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address));
	Draw draw(front.address, back.address);
	const auto info = draw.Acquire();
	const auto code = ShaderParseVS(&draw.regs, &draw.sh);
	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	EXPECT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::SBranch);
	EXPECT_EQ(code.GetInstructions().At(1).type, type);
	EXPECT_EQ(code.GetInstructions().At(1).pc, 16u);
	EXPECT_EQ(code.GetInstructions().At(2).pc, 20u);
	for (const auto& inst: code.GetInstructions()) { ASSERT_TRUE(ShaderInstructionLoweringPreconditions(inst)); }
	const auto complete = ShaderAnalyzeNativeWave(code, 64);
	ASSERT_EQ(complete.proof, expected);
	EXPECT_EQ(info.native_wave.proof, complete.proof);
	EXPECT_EQ(info.native_wave.guest_wave_size, 64u);
	EXPECT_EQ(info.required_subgroup_size, expected == ShaderNativeWaveProof::ExactSubgroup ? 64u : 0u);
	ShaderComputeWaveVulkanState host {};
	host.min_subgroup_size = host.max_subgroup_size = 32;
	const auto selection = ShaderSelectNativeSubgroup(host, VK_SHADER_STAGE_VERTEX_BIT, 32, 64,
	                                                info.native_wave.proof == ShaderNativeWaveProof::LaneLocal, false, true);
	EXPECT_EQ(selection.supported, expected == ShaderNativeWaveProof::LaneLocal);
	if (selection.supported)
	{
		EXPECT_EQ(selection.size, 32u); // A genuinely queried singleton, even for SPIR-V 1.6.
		EXPECT_FALSE(selection.require_size);
	}
}

} // namespace

TEST(EmulatorVertexProgram, BackLaneReadParticipatesInNativeWaveAdmission)
{
	ASSERT_EXIT(
	    {
		    CheckBackProof(0x7e000501u, ShaderInstructionType::VReadfirstlaneB32, ShaderNativeWaveProof::ExactSubgroup);
		    std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, BackNumericExecReadParticipatesInNativeWaveAdmission)
{
	ASSERT_EXIT(
	    {
		    CheckBackProof(0x7e02027eu, ShaderInstructionType::VMovB32, ShaderNativeWaveProof::ExactSubgroup);
		    std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, NeutralBackRetainsLaneLocalAdmission)
{
	ASSERT_EXIT(
	    {
		    CheckBackProof(0x7e020280u, ShaderInstructionType::VMovB32, ShaderNativeWaveProof::LaneLocal);
		    std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, DifferentBacksWithEqualProofHaveDistinctModuleIdentities)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u});
		MappedProgram back_a({0x7e020280u, 0xbf810000u});
		MappedProgram back_b({0x7e020281u, 0xbf810000u});
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back_a.address));
		Draw draw(front.address, back_a.address);
		const auto a = draw.Acquire();
		const auto id_a = ShaderGetIdVS(&draw.regs, &a);
		ShaderTranslationCache cache(4);
		unsigned calls = 0;
		auto compile = [&calls]()
		{
			Vector<uint32_t> words;
			words.Add(++calls);
			return words;
		};
		const auto key_a = ShaderModuleKey::Create(id_a, ShaderModuleStage::Vertex, Config::ShaderOptimizationType::None, true);
		EXPECT_FALSE(cache.GetOrCompile(key_a, compile).hit);
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back_b.address));
		draw.regs.gs_back_addr = back_b.address;
		const auto b = draw.Acquire();
		EXPECT_EQ(a.native_wave.proof, ShaderNativeWaveProof::LaneLocal);
		EXPECT_EQ(a.native_wave.proof, b.native_wave.proof);
		const auto id_b = ShaderGetIdVS(&draw.regs, &b);
		EXPECT_NE(id_a, id_b);
		const auto key_b = ShaderModuleKey::Create(id_b, ShaderModuleStage::Vertex, Config::ShaderOptimizationType::None, true);
		EXPECT_FALSE(cache.GetOrCompile(key_b, compile).hit);
		EXPECT_EQ(calls, 2u);
		EXPECT_TRUE(cache.GetOrCompile(key_b, compile).hit);
		EXPECT_EQ(calls, 2u);
	};
	ASSERT_EXIT({ probe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, ReplacingBackBytesAtSameAddressChangesProofAndIdentity)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u});
		MappedProgram back({0x7e020280u, 0xbf810000u});
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address));
		Draw draw(front.address, back.address);
		const auto before = draw.Acquire();
		const auto id = ShaderGetIdVS(&draw.regs, &before);
		back.Replace({0x7e02027eu, 0xbf810000u});
		EXPECT_EQ(ShaderLookupContinuation(front.address), 0u);
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address));
		const auto after = draw.Acquire();
		EXPECT_EQ(before.native_wave.proof, ShaderNativeWaveProof::LaneLocal);
		EXPECT_EQ(after.native_wave.proof, ShaderNativeWaveProof::ExactSubgroup);
		EXPECT_EQ(after.required_subgroup_size, 64u);
		EXPECT_NE(id, ShaderGetIdVS(&draw.regs, &after));
	};
	ASSERT_EXIT({ probe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, EqualProgramContentCanShareModuleIdentityAcrossBackRelocation)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u});
		MappedProgram back_a({0x7e020280u, 0xbf810000u});
		MappedProgram back_b({0x7e020280u, 0xbf810000u});
		ASSERT_NE(back_a.address, back_b.address);
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back_a.address));
		Draw draw(front.address, back_a.address);
		const auto a = draw.Acquire();
		const auto id = ShaderGetIdVS(&draw.regs, &a);
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back_b.address));
		draw.regs.gs_back_addr = back_b.address;
		const auto b = draw.Acquire();
		EXPECT_EQ(id, ShaderGetIdVS(&draw.regs, &b));
	};
	ASSERT_EXIT({ probe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, RemappingFrontWithSameChecksumChangesCompleteIdentity)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0x7e040280u, 0xbe802000u}); // v2=0, terminal setpc
		MappedProgram back({0x7e020280u, 0xbf810000u});
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address));
		Draw draw(front.address, back.address);
		const auto before = draw.Acquire();
		const auto id = ShaderGetIdVS(&draw.regs, &before);
		front.Replace({0x7e040281u, 0xbe802000u}); // same checksum, v2=1
		EXPECT_EQ(ShaderLookupContinuation(front.address), 0u);
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address));
		const auto after = draw.Acquire();
		EXPECT_EQ(before.native_wave.proof, after.native_wave.proof);
		EXPECT_NE(id, ShaderGetIdVS(&draw.regs, &after));
	};
	ASSERT_EXIT({ probe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, NonterminalSetpcDoesNotAppendRegisteredBack)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u, 0xbf810000u});
		MappedProgram back({0x7e000501u, 0xbf810000u});
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address));
		Draw draw(front.address, back.address);
		const auto info = draw.Acquire();
		const auto code = ShaderParseVS(&draw.regs, &draw.sh);
		ASSERT_EQ(code.GetInstructions().Size(), 2u);
		EXPECT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::SSetpcB64);
		EXPECT_EQ(code.GetInstructions().At(1).type, ShaderInstructionType::SEndpgm);
		EXPECT_EQ(code.GetContinuationPc(), UINT32_MAX);
		EXPECT_EQ(info.native_wave.proof, ShaderNativeWaveProof::LaneLocal);
	};
	ASSERT_EXIT({ probe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, ProofIdentityAndEmissionRetainAcquiredBackAcrossRebinding)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u});
		MappedProgram back_a({0x7e020280u, 0xbf810000u});
		MappedProgram back_b({0x7e02027eu, 0xbf810000u});
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back_a.address));
		Draw draw_a(front.address, back_a.address);
		const auto a = draw_a.Acquire();
		const auto id_a = ShaderGetIdVS(&draw_a.regs, &a);
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back_b.address));
		Draw draw_b(front.address, back_b.address);
		const auto b = draw_b.Acquire();
		const auto a_code = ShaderParseVS(&draw_a.regs, &draw_a.sh, &a);
		const auto b_code = ShaderParseVS(&draw_b.regs, &draw_b.sh, &b);
		ASSERT_EQ(a_code.GetInstructions().Size(), 3u);
		ASSERT_EQ(b_code.GetInstructions().Size(), 3u);
		EXPECT_EQ(a_code.GetInstructions().At(1).src[0].constant.u, 0u);
		EXPECT_EQ(b_code.GetInstructions().At(1).src[0].type, ShaderOperandType::ExecLo);
		EXPECT_EQ(a.native_wave.proof, ShaderAnalyzeNativeWave(a_code, 64).proof);
		EXPECT_EQ(b.native_wave.proof, ShaderAnalyzeNativeWave(b_code, 64).proof);
		EXPECT_EQ(id_a, ShaderGetIdVS(&draw_a.regs, &a));
		EXPECT_NE(id_a, ShaderGetIdVS(&draw_b.regs, &b));
		EXPECT_NE(a.program.get(), b.program.get());
	};
	ASSERT_EXIT({ probe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, ReturnedCodeCannotMutateSharedOwner)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u});
		MappedProgram back({0x7e020280u, 0xbf810000u});
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address));
		Draw draw(front.address, back.address);
		const auto info = draw.Acquire();
		const auto id = ShaderGetIdVS(&draw.regs, &info);
		auto copy = ShaderParseVS(&draw.regs, &draw.sh, &info);
		copy.GetInstructions()[1].src[0].constant.u = 7u;
		copy.GetLabels().Clear();
		copy.SetContinuationPc(0u);
		const auto retained = ShaderParseVS(&draw.regs, &draw.sh, &info);
		EXPECT_EQ(retained.GetInstructions().At(1).src[0].constant.u, 0u);
		EXPECT_EQ(retained.GetContinuationPc(), 16u);
		EXPECT_FALSE(retained.GetLabels().IsEmpty());
		EXPECT_EQ(id, ShaderGetIdVS(&draw.regs, &info));
	};
	ASSERT_EXIT({ probe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, SameContentRemapReacquiresOwnerButPreservesStableIdentity)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u});
		MappedProgram back({0x7e020280u, 0xbf810000u});
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address));
		Draw draw(front.address, back.address);
		const auto a = draw.Acquire();
		const auto id = ShaderGetIdVS(&draw.regs, &a);
		back.Replace({0x7e020280u, 0xbf810000u});
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address));
		const auto b = draw.Acquire();
		EXPECT_NE(a.program.get(), b.program.get());
		EXPECT_EQ(id, ShaderGetIdVS(&draw.regs, &b));
		const auto repeat = draw.Acquire();
		EXPECT_EQ(b.program.get(), repeat.program.get());
	};
	ASSERT_EXIT({ probe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, ReleasedGuestStorageCannotChangeAcquiredEmission)
{
	const auto probe = []
	{
		Initialize();
		ShaderVertexInputInfo info;
		HW::VertexShaderInfo regs;
		HW::ShaderRegisters sh {};
		ShaderId id;
		{
			MappedProgram front({0xbe802000u});
			MappedProgram back({0x7e020280u, 0xbf810000u});
			ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address));
			Draw draw(front.address, back.address);
			info = draw.Acquire();
			regs = draw.regs;
			id = ShaderGetIdVS(&regs, &info);
		}
		const auto code = ShaderParseVS(&regs, &sh, &info);
		ASSERT_EQ(code.GetInstructions().Size(), 3u);
		EXPECT_EQ(id, ShaderGetIdVS(&regs, &info));
		ASSERT_TRUE(Config::ShaderValidationEnabled());
		ASSERT_EQ(Config::GetShaderOptimizationType(), Config::ShaderOptimizationType::None);
		const auto source = SpirvGenerateSource(code, &info, nullptr, nullptr);
		Vector<uint32_t> binary;
		String8 error;
		ASSERT_TRUE(ShaderToolchain::Run(source, &binary, &error)) << error.c_str();
		EXPECT_FALSE(binary.IsEmpty());
	};
	ASSERT_EXIT({ probe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, InteriorBackOwnerShrinkInvalidatesLinkWithoutDamagingAcquiredCode)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u});
		MappedProgram back({0xbf810000u});
		const uint32_t words[] = {0x7e020280u, 0xbf810000u};
		ASSERT_TRUE(Core::VirtualMemory::CopyToGuest(back.address + 256u, words, sizeof(words)));
		back.MapSize(256u + sizeof(words));
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address + 256u));
		Draw draw(front.address, back.address + 256u);
		const auto info = draw.Acquire();
		back.MapSize(128u);
		EXPECT_EQ(ShaderLookupContinuation(front.address), 0u);
		EXPECT_FALSE(ShaderRegisterContinuation(front.address, back.address + 256u));
		const auto code = ShaderParseVS(&draw.regs, &draw.sh, &info);
		ASSERT_EQ(code.GetInstructions().Size(), 3u);
		EXPECT_EQ(code.GetInstructions().At(1).src[0].constant.u, 0u);
	};
	ASSERT_EXIT({ probe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, CachedFrontCannotHideRemovedRequiredContinuation)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u});
		MappedProgram back({0x7e020280u, 0xbf810000u});
		if (!ShaderRegisterContinuation(front.address, back.address)) { std::_Exit(125); }
		Draw draw(front.address, back.address);
		const auto old = draw.Acquire();
		back.Replace({0x7e020281u, 0xbf810000u});
		(void)draw.Acquire();
	};
	EXPECT_DEATH(probe(), "vertex front has no complete reachable terminator");
}

TEST(EmulatorVertexProgram, MismatchedBoundBackRefusesBeforeCertification)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u});
		MappedProgram back_a({0x7e020280u, 0xbf810000u});
		MappedProgram back_b({0x7e020281u, 0xbf810000u});
		if (!ShaderRegisterContinuation(front.address, back_a.address)) { std::_Exit(125); }
		Draw draw(front.address, back_b.address);
		(void)draw.Acquire();
	};
	EXPECT_DEATH(probe(), "vertex continuation binding mismatch");
}

TEST(EmulatorVertexProgram, ProductionCacheLookupRequiresAnOwner)
{
	const auto probe = []
	{
		Initialize();
		HW::VertexShaderInfo regs {};
		ShaderVertexInputInfo info {};
		ShaderRequireVertexProgram(&regs, &info);
	};
	EXPECT_DEATH(probe(), "Gen5 vertex program owner is required before cache lookup");
}

TEST(EmulatorVertexProgram, OwnerCannotBeUsedWithDifferentDrawBinding)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u});
		MappedProgram back({0x7e020280u, 0xbf810000u});
		if (!ShaderRegisterContinuation(front.address, back.address)) { std::_Exit(125); }
		Draw draw(front.address, back.address);
		const auto info = draw.Acquire();
		draw.regs.gs_regs.chksum ^= 1u;
		ShaderRequireVertexProgram(&draw.regs, &info);
	};
	EXPECT_DEATH(probe(), "vertex program owner does not match the draw binding");
}

TEST(EmulatorVertexProgram, DebugMutationGetsNewIdentityWithoutMutatingAcquiredCode)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u});
		MappedProgram back({0x7e020280u, 0xbf810000u});
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address));
		Draw draw(front.address, back.address);
		const auto before = draw.Acquire();
		const auto id = ShaderGetIdVS(&draw.regs, &before);
		ShaderDebugPrintf command;
		command.pc = 16u;
		command.format = U"owned-program-marker";
		ShaderInjectDebugPrintf(draw.regs.gs_regs.chksum, command);
		const auto after = draw.Acquire();
		EXPECT_NE(before.program.get(), after.program.get());
		EXPECT_EQ(id, ShaderGetIdVS(&draw.regs, &before));
		EXPECT_NE(id, ShaderGetIdVS(&draw.regs, &after));
		const auto old_code = ShaderParseVS(&draw.regs, &draw.sh, &before);
		const auto new_code = ShaderParseVS(&draw.regs, &draw.sh, &after);
		EXPECT_TRUE(old_code.GetDebugPrintfs().IsEmpty());
		ASSERT_EQ(new_code.GetDebugPrintfs().Size(), 1u);
		EXPECT_EQ(new_code.GetDebugPrintfs().At(0).format, command.format);
	};
	ASSERT_EXIT({ probe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, EmbeddedMetadataReuseReleasesPreviousOwner)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbe802000u});
		MappedProgram back({0x7e020280u, 0xbf810000u});
		ASSERT_TRUE(ShaderRegisterContinuation(front.address, back.address));
		Draw draw(front.address, back.address);
		auto info = draw.Acquire();
		ASSERT_NE(info.program, nullptr);
		draw.regs.vs_embedded = true;
		ShaderGetInputInfoVS(&draw.regs, &draw.sh, &info, &draw.stages);
		EXPECT_EQ(info.program, nullptr);
		ShaderRequireVertexProgram(&draw.regs, &info);
	};
	ASSERT_EXIT({ probe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorVertexProgram, UnreadableMappedProgramFailsBeforePublishingOwner)
{
	const auto probe = []
	{
		Initialize();
		MappedProgram front({0xbf810000u});
		Draw draw(front.address, 0u);
		if (!Core::VirtualMemory::Free(front.address)) { std::_Exit(125); }
		(void)draw.Acquire();
	};
	EXPECT_DEATH(probe(), "vertex program range became unreadable");
}

UT_END();
