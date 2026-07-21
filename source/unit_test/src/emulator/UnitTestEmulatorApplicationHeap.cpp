#include "Kyty/UnitTest.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/ApplicationHeap.h"

UT_BEGIN(EmulatorApplicationHeap);

using Kyty::Libs::LibKernel::ApplicationHeap::Api;
using Kyty::Libs::LibKernel::ApplicationHeap::IsInitialized;
using Kyty::Libs::LibKernel::ApplicationHeap::IsValidApi;
using Kyty::Libs::LibKernel::ApplicationHeap::HasMallocStatsFast;
using Kyty::Libs::LibKernel::ApplicationHeap::RegisterApi;
using Kyty::Libs::LibKernel::ApplicationHeap::Reset;
using Kyty::Libs::LibKernel::ApplicationHeap::kFreeSlot;
using Kyty::Libs::LibKernel::ApplicationHeap::kMallocSlot;
using Kyty::Libs::LibKernel::ApplicationHeap::kMallocStatsFastSlot;

TEST(EmulatorApplicationHeap, AcceptsDirectRuntimeFunctionTable)
{
	Api api {};
	api.slots[kMallocSlot] = reinterpret_cast<void*>(0x900f09370ull);
	api.slots[kFreeSlot]   = reinterpret_cast<void*>(0x900f08920ull);

	EXPECT_TRUE(IsValidApi(&api));
	EXPECT_FALSE(IsValidApi(nullptr));

	api.slots[kMallocSlot] = nullptr;
	EXPECT_FALSE(IsValidApi(&api));
}

TEST(EmulatorApplicationHeap, RegistrationRequiresMallocAndFree)
{
	Reset();
	EXPECT_FALSE(IsInitialized());

	Api api {};
	api.slots[kMallocSlot] = reinterpret_cast<void*>(0x900f09370ull);
	api.slots[kFreeSlot]   = reinterpret_cast<void*>(0x900f08920ull);
	api.slots[kMallocStatsFastSlot] = reinterpret_cast<void*>(0x900f08a10ull);
	RegisterApi(api.slots);
	EXPECT_TRUE(IsInitialized());
	EXPECT_TRUE(HasMallocStatsFast());

	RegisterApi(nullptr);
	EXPECT_FALSE(IsInitialized());
	EXPECT_FALSE(HasMallocStatsFast());
}

UT_END();
