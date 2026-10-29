#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvInternal.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <array>
#include <cstdint>
#include <cstdlib>

UT_BEGIN(EmulatorShaderGlobalLoadD16Hi);

using namespace Libs::Graphics;

namespace {

// GLOBAL D16_HI encodings (RDNA2 Shader ISA, section 12.18): word 0 holds the opcode
// in [24:18], LDS in bit 13, SEG in [15:14] and the 12-bit OFFSET; word 1 holds VDST
// in [31:24], SADDR in [22:16] and ADDR in [7:0]. SADDR NULL (SGPR 125, 0x7d) and
// 0x7f select a full VGPR address; any other SADDR is an SGPR base.
constexpr uint32_t kSegmentGlobal = 2u;
constexpr uint32_t kSaddrNull     = 0x7du;
constexpr uint32_t kSaddrOff      = 0x7fu;
constexpr uint32_t kSEndpgm       = 0xbf810000u;

// Rejected encodings exit through EXIT/KYTY_NI with the halt status (321), which
// the POSIX wait status truncates to 65. Matching the status, not the message, keeps
// the check independent of wording.
#if defined(_WIN32)
constexpr int kRejectedExit = 321;
#else
constexpr int kRejectedExit = 65;
#endif

using Program = std::array<uint32_t, 3>;

uint32_t FlatWord0(uint32_t opcode, uint32_t segment, uint32_t offset, uint32_t lds = 0u)
{
	return 0xdc000000u | (opcode << 18u) | (lds << 13u) | (segment << 14u) | (offset & 0xfffu);
}

uint32_t FlatWord1(uint32_t vdst, uint32_t vaddr, uint32_t saddr = kSaddrNull)
{
	return (vdst << 24u) | (saddr << 16u) | vaddr;
}

void InitializeConfig()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

// Turns the SPIR-V validator on for the modules this suite emits.
class ValidationOn final: public Config::ConfigSource
{
public:
	ValidationOn() { Config::Load(*this); }
	bool         Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
	int64_t      GetInteger(const Core::String&) const override { return 0; }
	bool         GetBool(const Core::String&) const override { return true; }
	Core::String GetString(const Core::String&) const override { return {}; }
};

// Encodings the parser rejects end the process, so this runs only inside
// EXPECT_EXIT with kRejectedExit (a program that parses returns normally and fails
// the exit check).
void ParseOrDie(const Program& program)
{
	InitializeConfig();
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	(void)ShaderTryParseBounded(program.data(), sizeof(program), &code);
}

// Generates the module for one byte D16_HI load with device addressing and assembles
// it with the validator on. Generation exits the process on an internal emitter
// failure, so callers run this in a child process.
bool AssembleHighHalfLoad(uint32_t opcode)
{
	InitializeConfig();
	const ValidationOn validation;
	const Program      program = {FlatWord0(opcode, kSegmentGlobal, 0x7f0u), FlatWord1(5, 2), kSEndpgm};
	ShaderCode         code;
	code.SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(program.data(), sizeof(program), &code))
	{
		return false;
	}
	ShaderComputeInputInfo input {};
	input.threads_num[0]                   = 64;
	input.threads_num[1]                   = 1;
	input.threads_num[2]                   = 1;
	input.wave_layout.guest_wave_size      = 64;
	input.wave_layout.native_subgroup_size = 64;
	input.bind.device_address_used         = true;
	ShaderCalcBindingIndices(&input.bind);
	Spirv spirv;
	spirv.SetCode(code);
	spirv.SetCsInputInfo(&input);
	spirv.GenerateSource();
	Vector<uint32_t> binary;
	Core::String8    error;
	return ShaderToolchain::Run(spirv.GetSource(), &binary, &error);
}

} // namespace

TEST(EmulatorShaderGlobalLoadD16Hi, DecodesHighHalfByteLoadsIntoOneVgprWithSignedOffset)
{
	InitializeConfig();
	struct Case
	{
		uint32_t              opcode;
		ShaderInstructionType type;
	};
	const Case cases[] = {
	    {33u, ShaderInstructionType::GlobalLoadUbyteD16Hi},
	    {35u, ShaderInstructionType::GlobalLoadSbyteD16Hi},
	};
	for (const auto& test: cases)
	{
		// An offset of 0x800 is -2048 in the signed GLOBAL immediate.
		const Program program = {FlatWord0(test.opcode, kSegmentGlobal, 0x800u), FlatWord1(5, 2), kSEndpgm};
		ShaderCode    code;
		code.SetType(ShaderType::Compute);
		ASSERT_TRUE(ShaderTryParseBounded(program.data(), sizeof(program), &code));
		ASSERT_TRUE(code.GetInstructions().Size() == 2u);
		const auto& load = code.GetInstructions().At(0);
		EXPECT_TRUE(load.type == test.type);
		EXPECT_TRUE(load.dst.register_id == 5);
		EXPECT_TRUE(load.dst.size == 1);
		EXPECT_TRUE(load.flat_offset == -2048);
		EXPECT_TRUE(ShaderHasGlobalMemoryLoad(code));
	}
}

TEST(EmulatorShaderGlobalLoadD16Hi, DecodesSaddrAsSgprBaseOnlyWhenNotNullOrOff)
{
	InitializeConfig();
	struct Case
	{
		uint32_t saddr;
		uint32_t src_num;
	};
	// NULL and 0x7f keep the VGPR as the full address. SADDR 4 is the SGPR pair
	// s[4:5] base, and the VGPR (v2) is the 32-bit offset.
	const Case cases[] = {{kSaddrNull, 1u}, {kSaddrOff, 1u}, {4u, 2u}};
	for (const auto& test: cases)
	{
		const Program program = {FlatWord0(33u, kSegmentGlobal, 0u), FlatWord1(5, 2, test.saddr), kSEndpgm};
		ShaderCode    code;
		code.SetType(ShaderType::Compute);
		ASSERT_TRUE(ShaderTryParseBounded(program.data(), sizeof(program), &code));
		ASSERT_TRUE(code.GetInstructions().Size() == 2u);
		const auto& load = code.GetInstructions().At(0);
		EXPECT_TRUE(load.src_num == static_cast<int>(test.src_num));
		EXPECT_TRUE(load.src[0].register_id == 2);
		EXPECT_TRUE(load.src[0].size == (test.src_num == 1u ? 2 : 1));
		if (test.src_num == 2u)
		{
			EXPECT_TRUE(load.src[1].type == ShaderOperandType::Sgpr);
			EXPECT_TRUE(load.src[1].register_id == 4);
			EXPECT_TRUE(load.src[1].size == 2);
			EXPECT_TRUE(load.format == ShaderInstructionFormat::VdataVaddrSaddr2);
		} else
		{
			EXPECT_TRUE(load.format == ShaderInstructionFormat::VdataVaddr2Off);
		}
	}
}

TEST(EmulatorShaderGlobalLoadD16Hi, RejectsFlatScratchReservedLdsAndShortEncodings)
{
	// Only GLOBAL byte loads without LDS are modelled. FLAT and SCRATCH need aperture
	// and scratch translation, segment 3 is reserved, LDS=1 writes LDS, and the
	// GLOBAL_LOAD_SHORT_D16_HI form (opcode 37) is refused because its alignment at odd
	// addresses is not defined.
	const Program cases[] = {
	    {FlatWord0(33u, 0u, 0u), FlatWord1(5, 2), kSEndpgm},
	    {FlatWord0(33u, 1u, 0u), FlatWord1(5, 2), kSEndpgm},
	    {FlatWord0(33u, 3u, 0u), FlatWord1(5, 2), kSEndpgm},
	    {FlatWord0(33u, kSegmentGlobal, 0u, 1u), FlatWord1(5, 2), kSEndpgm},
	    {FlatWord0(37u, kSegmentGlobal, 0u), FlatWord1(5, 2), kSEndpgm},
	};
	for (const auto& program: cases)
	{
		EXPECT_EXIT(ParseOrDie(program), ::testing::ExitedWithCode(kRejectedExit), "");
	}
}

// Checks that the generated module assembles and passes the SPIR-V validator. It does
// not check the merged register values: those need a device run.
TEST(EmulatorShaderGlobalLoadD16Hi, EmitsModuleThatAssemblesForEachByteLoad)
{
	InitializeConfig();
	for (uint32_t opcode: {33u, 35u})
	{
		EXPECT_EXIT(std::_Exit(AssembleHighHalfLoad(opcode) ? 0 : 3), ::testing::ExitedWithCode(0), "");
	}
}

UT_END();
