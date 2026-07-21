#include "Kyty/UnitTest.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/ApplicationHeap.h"

UT_BEGIN(EmulatorApplicationHeap);

using Kyty::Libs::LibKernel::ApplicationHeap::Api;
using Kyty::Libs::LibKernel::ApplicationHeap::IsInitialized;
using Kyty::Libs::LibKernel::ApplicationHeap::IsValidApi;
using Kyty::Libs::LibKernel::ApplicationHeap::RegisterApi;
using Kyty::Libs::LibKernel::ApplicationHeap::Reset;
using Kyty::Libs::LibKernel::ApplicationHeap::kFreeSlot;
using Kyty::Libs::LibKernel::ApplicationHeap::kMallocSlot;

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
	RegisterApi(api.slots);
	EXPECT_TRUE(IsInitialized());

	RegisterApi(nullptr);
	EXPECT_FALSE(IsInitialized());
}

UT_END();
