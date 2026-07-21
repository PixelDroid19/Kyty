#include "Emulator/Loader/RuntimeLinker.h"

#include "Kyty/UnitTest.h"

#ifdef KYTY_EMU_ENABLED

UT_BEGIN(EmulatorLoaderModuleStart);

using Kyty::Loader::LoaderBuildModuleStartOrder;
using Kyty::Loader::ModuleStartDescriptor;

TEST(EmulatorLoaderModuleStart, StartsLoadedDependenciesBeforeTheirConsumer)
{
	Vector<ModuleStartDescriptor> modules;
	ModuleStartDescriptor         assemblies {};
	assemblies.file_name = U"assemblies.prx";
	assemblies.needed.Add(U"support.prx");
	assemblies.needed.Add(U"libc.prx");
	modules.Add(assemblies);

	ModuleStartDescriptor support {};
	support.file_name = U"support.prx";
	modules.Add(support);

	ModuleStartDescriptor libc {};
	libc.file_name = U"libc.prx";
	modules.Add(libc);

	const auto order = LoaderBuildModuleStartOrder(modules);
	ASSERT_EQ(order.Size(), 3u);
	EXPECT_EQ(order[0], 2u); // libc seeded first
	EXPECT_EQ(order[1], 1u); // support before assemblies
	EXPECT_EQ(order[2], 0u);
}

TEST(EmulatorLoaderModuleStart, SeedsLibcBeforeConsumerWithoutNeededEdge)
{
	Vector<ModuleStartDescriptor> modules;
	ModuleStartDescriptor         assemblies {};
	assemblies.file_name = U"Il2CppUserAssemblies.prx";
	modules.Add(assemblies);

	ModuleStartDescriptor libc {};
	libc.file_name = U"libc.prx";
	modules.Add(libc);

	const auto order = LoaderBuildModuleStartOrder(modules);
	ASSERT_EQ(order.Size(), 2u);
	EXPECT_EQ(order[0], 1u);
	EXPECT_EQ(order[1], 0u);
}

UT_END();

#endif // KYTY_EMU_ENABLED
