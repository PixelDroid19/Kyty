#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderStorageAnalysis.h"

#include <array>
#include <cstdlib>

UT_BEGIN(EmulatorShaderResourceBounds);

using namespace Libs::Graphics;

TEST(EmulatorShaderResourceBounds, KeepsSampledAndStorageImagesInTheCombinedTable)
{
	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    ShaderTextureResources textures;
		    HW::UserSgprInfo sgprs {};
		    sgprs.value[3] = 9u << 28u;
		    bool direct[HW::UserSgprInfo::SGPRS_MAX] {};
		    for (int slot = 0; slot < 16; ++slot)
		    {
			    ShaderGetTextureBuffer(&textures, direct, 0, slot, ShaderTextureUsage::ReadOnly, sgprs, nullptr);
		    }
		    ShaderGetTextureBuffer(&textures, direct, 0, 0, ShaderTextureUsage::ReadWrite, sgprs, nullptr);
		    if (textures.textures_num != 17 || textures.textures2d_sampled_num != 16 || textures.textures2d_storage_num != 1 ||
		        textures.desc[16].usage != ShaderTextureUsage::ReadWrite)
		    {
			    std::_Exit(2);
		    }
		    for (int slot = 1; slot < 16; ++slot)
		    {
			    ShaderGetTextureBuffer(&textures, direct, 0, slot, ShaderTextureUsage::ReadWrite, sgprs, nullptr);
		    }
		    if (textures.textures_num != 32 || textures.textures2d_storage_num != 16 ||
		        textures.desc[31].usage != ShaderTextureUsage::ReadWrite)
		    {
			    std::_Exit(3);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderResourceBounds, RejectsTextureAppendAtCapacityBeforeWriting)
{
	struct GuardedResources
	{
		ShaderTextureResources textures;
		std::array<uint32_t, 128> padding {};
	};
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    GuardedResources resources;
		    resources.textures.textures_num = ShaderTextureResources::RES_MAX;
		    HW::UserSgprInfo sgprs {};
		    bool direct[HW::UserSgprInfo::SGPRS_MAX] {};
		    ShaderGetTextureBuffer(&resources.textures, direct, 0, 0, ShaderTextureUsage::ReadOnly, sgprs, nullptr);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(rejected_exit), "");
}

UT_END();
