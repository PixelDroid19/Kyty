#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveVectorBuffer.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

UT_BEGIN(EmulatorComputeWaveVectorBuffer);

using namespace Libs::Graphics;

static constexpr uint32_t kEnd = 0xbf810000u; // s_endpgm

static void InitializeConfig()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

TEST(EmulatorComputeWaveVectorBuffer, ClassifiesRawUmaxAsGuardedWrite)
{
	constexpr uint32_t words[] = {0xe0e00004u, 0x80000507u, kEnd};
	InitializeConfig();
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ASSERT_TRUE(ShaderTryParseBounded(words, sizeof(words), &code));
	const auto use = AnalyzeShaderStorageUse(code, 0);
	EXPECT_EQ(use.access, ShaderStorageAccess::Raw);
	EXPECT_TRUE(use.raw_vmem_oob_guarded);
	EXPECT_EQ(ShaderGetDirectStorageUsage(code, 0), ShaderStorageUsage::ReadWrite);
}

UT_END();
