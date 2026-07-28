#include "Kyty/UnitTest.h"

#include "Emulator/Loader/ModuleLoad.h"

UT_BEGIN(EmulatorModuleDiscovery);

TEST(EmulatorModuleDiscovery, RejectsPluginDirectoryFromPackageBootstrap)
{
	using namespace Loader::ModuleDiscovery;

	EXPECT_TRUE(IsSupportedPackageSubdir(""));
	EXPECT_TRUE(IsSupportedPackageSubdir("Media/Modules/"));
	EXPECT_FALSE(IsSupportedPackageSubdir("Media/Plugins/"));
}

UT_END();
