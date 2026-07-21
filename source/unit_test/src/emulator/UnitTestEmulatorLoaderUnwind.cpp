#include "Emulator/Loader/RuntimeLinker.h"

#include "Kyty/UnitTest.h"

#include <array>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

UT_BEGIN(EmulatorLoaderUnwind);

using Kyty::Loader::EhFrameInfo;
using Kyty::Loader::LoaderDecodeEhFrameHeader;
using Kyty::Loader::LoaderBuildModuleStartOrder;
using Kyty::Loader::ModuleStartDescriptor;

TEST(EmulatorLoaderUnwind, DecodesCapturedPcRelativeSignedEhFramePointer)
{
	const std::array<uint8_t, 8> header = {0x01, 0x1b, 0x03, 0x3b, 0xec, 0x6e, 0x00, 0x00};
	EhFrameInfo                 info {};

	ASSERT_TRUE(LoaderDecodeEhFrameHeader(header.data(), header.size(), 0x1000, 0x10000, &info));
	EXPECT_EQ(info.header_addr, 0x1000u);
	EXPECT_EQ(info.frame_addr, 0x7ef0u);
	EXPECT_EQ(info.frame_size, 0x8110u);
}

TEST(EmulatorLoaderUnwind, RejectsUnsupportedOrOutOfRangeEhFramePointers)
{
	std::array<uint8_t, 8> header = {0x01, 0x1b, 0x03, 0x3b, 0xec, 0x6e, 0x00, 0x00};
	EhFrameInfo            info {};

	EXPECT_FALSE(LoaderDecodeEhFrameHeader(nullptr, header.size(), 0x1000, 0x10000, &info));
	EXPECT_FALSE(LoaderDecodeEhFrameHeader(header.data(), 7, 0x1000, 0x10000, &info));
	header[0] = 2;
	EXPECT_FALSE(LoaderDecodeEhFrameHeader(header.data(), header.size(), 0x1000, 0x10000, &info));
	header[0] = 1;
	header[1] = 0;
	EXPECT_FALSE(LoaderDecodeEhFrameHeader(header.data(), header.size(), 0x1000, 0x10000, &info));
	header[1] = 0x1b;
	EXPECT_FALSE(LoaderDecodeEhFrameHeader(header.data(), header.size(), 0x1000, 0x7ef0, &info));
}

TEST(EmulatorLoaderUnwind, StartsLoadedDependenciesBeforeTheirConsumer)
{
	Vector<ModuleStartDescriptor> modules;
	ModuleStartDescriptor        assemblies {};
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
	EXPECT_EQ(order[0], 2u);
	EXPECT_EQ(order[1], 1u);
	EXPECT_EQ(order[2], 0u);
}

UT_END();

#endif // KYTY_EMU_ENABLED
