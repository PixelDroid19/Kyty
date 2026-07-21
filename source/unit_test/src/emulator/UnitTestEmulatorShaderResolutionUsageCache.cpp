#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/ShaderResolutionUsageCache.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

UT_BEGIN(EmulatorShaderResolutionUsageCache);

using namespace Libs::Graphics;

TEST(EmulatorShaderResolutionUsageCache, AnalyzesAnExactShaderIdentityOnlyOnce)
{
	ShaderResolutionUsageCache cache(2);
	int                        calls = 0;
	const ShaderResolutionUsageKey key {0x1000, 0x1234, 1};
	auto analyzer = [&calls]()
	{
		calls++;
		ResolutionShaderCoordinateUsage usage;
		usage.integer_image_coordinates = true;
		return ShaderResolutionAnalysis {usage, {}};
	};

	EXPECT_FALSE(cache.GetOrAnalyze(key, analyzer).hit);
	const auto second = cache.GetOrAnalyze(key, analyzer);
	EXPECT_TRUE(second.hit);
	EXPECT_TRUE(second.usage.integer_image_coordinates);
	EXPECT_EQ(calls, 1);
}

TEST(EmulatorShaderResolutionUsageCache, ChecksumAndTranslatorVersionArePartOfIdentity)
{
	ShaderResolutionUsageCache cache(3);
	int                        calls = 0;
	auto analyzer = [&calls]()
	{
		calls++;
		return ShaderResolutionAnalysis {};
	};
	EXPECT_FALSE(cache.GetOrAnalyze({0x1000, 1, 1}, analyzer).hit);
	EXPECT_FALSE(cache.GetOrAnalyze({0x1000, 2, 1}, analyzer).hit);
	EXPECT_FALSE(cache.GetOrAnalyze({0x1000, 2, 2}, analyzer).hit);
	EXPECT_EQ(calls, 3);
}

TEST(EmulatorShaderResolutionUsageCache, RetainsParsedIrForReadWriteClassification)
{
	ShaderResolutionUsageCache cache(1);
	const uint32_t             shader[] = {0xe01c2000u, 0x80000004u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	const auto result = cache.GetOrAnalyze(
	    {0x1000, 1, 1},
	    [&shader]()
	    {
		    auto code = std::make_shared<ShaderCode>();
		    code->SetType(ShaderType::Compute);
		    ShaderParse(shader, code.get());
		    return ShaderResolutionAnalysis {{}, code};
	    });
	ASSERT_NE(result.code, nullptr);
	EXPECT_EQ(ShaderGetDirectStorageUsage(*result.code, 0), ShaderStorageUsage::ReadWrite);
}

UT_END();
