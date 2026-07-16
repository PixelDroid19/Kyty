#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/RetailKernel.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/Semaphore.h"
#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Kyty/UnitTest.h"

UT_BEGIN(EmulatorKernelProcess);

using namespace Libs;
using namespace Libs::LibKernel::EventQueue;

namespace {

void EnsureKernelProcessSubsystems()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
}

} // namespace

TEST(EmulatorKernelProcess, GuestProcessIdIsStable)
{
	const int first = Posix::getpid();
	EXPECT_GT(first, 0);
	EXPECT_EQ(Posix::getpid(), first);
}

// Retail non-devkit sceKernelGetGPI (NID 4oXYe9Xmk0Q) returns 0 without GPI state.
TEST(EmulatorKernelProcess, RetailGetGpiReturnsZero)
{
	EXPECT_EQ(LibKernel::KernelRetailGetGpiResult(), 0);
}

TEST(EmulatorKernelProcess, GettimeofdayAdvancesWithinOneSecond)
{
	EnsureKernelProcessSubsystems();
	LibKernel::KernelTimeval before {};
	LibKernel::KernelTimeval after {};

	ASSERT_EQ(LibKernel::KernelGettimeofday(&before), 0);
	ASSERT_EQ(LibKernel::KernelUsleep(5'000), 0);
	ASSERT_EQ(LibKernel::KernelGettimeofday(&after), 0);

	const int64_t elapsed_us = (after.tv_sec - before.tv_sec) * 1'000'000 + (after.tv_usec - before.tv_usec);
	EXPECT_GE(elapsed_us, 1'000);
	EXPECT_LT(elapsed_us, 100'000);
}

// sceKernelAddAmprEvent (bBfz7kMF2Ho): register Ampr completion interest and wake via Trigger.
TEST(EmulatorKernelProcess, AddAmprEventRegistersAndTriggers)
{
	EnsureKernelProcessSubsystems();

	KernelEqueue eq = nullptr;
	ASSERT_EQ(KernelCreateEqueue(&eq, "ampr-test"), OK);
	ASSERT_NE(eq, nullptr);

	int udata_probe = 0;
	ASSERT_EQ(KernelAddAmprEvent(eq, 0, 0, /*ident=*/2, &udata_probe), OK);

	KernelEvent ev {};
	int         out  = 0;
	LibKernel::KernelUseconds zero = 0;
	EXPECT_EQ(KernelWaitEqueue(eq, &ev, 1, &out, &zero), LibKernel::KERNEL_ERROR_ETIMEDOUT);

	ASSERT_EQ(KernelTriggerEvent(eq, 2, KERNEL_EVFILT_AMPR, reinterpret_cast<void*>(static_cast<uintptr_t>(0x42))), OK);
	out = 0;
	ASSERT_EQ(KernelWaitEqueue(eq, &ev, 1, &out, &zero), OK);
	EXPECT_EQ(out, 1);
	EXPECT_EQ(ev.ident, static_cast<uintptr_t>(2));
	EXPECT_EQ(ev.filter, KERNEL_EVFILT_AMPR);
	EXPECT_EQ(ev.udata, &udata_probe);
	EXPECT_EQ(ev.data, static_cast<intptr_t>(0x42));

	EXPECT_EQ(KernelDeleteAmprEvent(eq, 2), OK);
	EXPECT_EQ(KernelDeleteAmprEvent(eq, 2), LibKernel::KERNEL_ERROR_ENOENT);
	EXPECT_EQ(KernelDeleteEqueue(eq), OK);
}

TEST(EmulatorKernelProcess, AprSubmitCommandBufferRejectsNullAndAckNonNull)
{
	EnsureKernelProcessSubsystems();

	uint64_t fake_cmd = 0x1111;
	EXPECT_EQ(LibKernel::FileSystem::KernelAprSubmitCommandBuffer(nullptr, 1, nullptr, 2, nullptr), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(LibKernel::FileSystem::KernelAprSubmitCommandBuffer(&fake_cmd, 1, &fake_cmd, 2, &fake_cmd), OK);
}

TEST(EmulatorKernelProcess, AprSubmitGetIdAndWaitRoundTrip)
{
	EnsureKernelProcessSubsystems();

	uint64_t fake_cmd = 0x2222;
	uint32_t sub_id   = 0;
	EXPECT_EQ(LibKernel::FileSystem::KernelAprSubmitCommandBufferAndGetId(nullptr, 1, &sub_id), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(LibKernel::FileSystem::KernelAprSubmitCommandBufferAndGetId(&fake_cmd, 1, nullptr), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(LibKernel::FileSystem::KernelAprSubmitCommandBufferAndGetId(&fake_cmd, 1, &sub_id), OK);
	EXPECT_NE(sub_id, 0u);
	EXPECT_EQ(LibKernel::FileSystem::KernelAprWaitCommandBuffer(sub_id), OK);
	// Second wait on completed id is soft-OK (eager builders).
	EXPECT_EQ(LibKernel::FileSystem::KernelAprWaitCommandBuffer(sub_id), OK);
	EXPECT_EQ(LibKernel::FileSystem::KernelAprWaitCommandBuffer(0), LibKernel::KERNEL_ERROR_EINVAL);
}

// Gen5 NID IafI2PxcPnQ — null mutex is EINVAL at the HLE boundary.
TEST(EmulatorKernelProcess, PthreadMutexTimedlockRejectsNull)
{
	EXPECT_EQ(LibKernel::PthreadMutexTimedlock(nullptr, 1000), LibKernel::KERNEL_ERROR_EINVAL);
}

// ForEach with results[] continues after per-path errors and returns success count.
TEST(EmulatorKernelProcess, AprResolveForEachReportsPerPathResults)
{
	EnsureKernelProcessSubsystems();

	const char* paths[2] = {nullptr, nullptr};
	uint32_t    ids[2]   = {0, 0};
	int32_t     results[2] = {0, 0};
	// Null path list entry → EFAULT per entry; with results[] returns 0 successes.
	const int rc = LibKernel::FileSystem::KernelAprResolveFilepathsToIdsForEach(paths, 2, ids, results);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(results[0], LibKernel::KERNEL_ERROR_EFAULT);
	EXPECT_EQ(results[1], LibKernel::KERNEL_ERROR_EFAULT);
	EXPECT_EQ(ids[0], 0xffffffffu);
	EXPECT_EQ(ids[1], 0xffffffffu);
}

// Gen5 memory helpers: null size rejects; range name is success no-op.
TEST(EmulatorKernelProcess, ConfiguredFlexibleAndRangeNameBoundaries)
{
	EXPECT_EQ(LibKernel::Memory::KernelConfiguredFlexibleMemorySize(nullptr), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(LibKernel::Memory::KernelSetVirtualRangeName(nullptr, 0, "test"), OK);
}

// Gen5 VirtualQuery: reject bad info_size/flags; unmapped addr returns EACCES.
TEST(EmulatorKernelProcess, VirtualQueryRejectsBadArgs)
{
	static_assert(sizeof(LibKernel::Memory::VirtualQueryInfo) == 72);
	LibKernel::Memory::VirtualQueryInfo info {};
	EXPECT_EQ(LibKernel::Memory::KernelVirtualQuery(nullptr, 0, nullptr, sizeof(info)), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(LibKernel::Memory::KernelVirtualQuery(nullptr, 0, &info, 8), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(LibKernel::Memory::KernelVirtualQuery(nullptr, 2, &info, sizeof(info)), LibKernel::KERNEL_ERROR_EINVAL);
}

// Gen5 TLS setspecific/getspecific NIDs used after KeyCreate.
TEST(EmulatorKernelProcess, ResolvesGen5PthreadSpecificNids)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libkernel_1", &symbols));

	auto resolve = [&](const char16_t* nid) {
		Loader::SymbolResolve query {};
		query.name                 = nid;
		query.library              = U"libkernel";
		query.library_version      = 1;
		query.module               = U"libkernel";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		return symbols.Find(query) != nullptr;
	};

	EXPECT_TRUE(resolve(u"+BzXYkqYeLE"));
	EXPECT_TRUE(resolve(u"eoht7mQOCmo"));
	EXPECT_TRUE(resolve(u"rVjRvHJ0X6c"));
	EXPECT_TRUE(resolve(u"XD3mDeybCnk"));
	EXPECT_TRUE(resolve(u"mkgXxsoxWHg"));
}

TEST(EmulatorKernelProcess, ClearVirtualRangeNameSucceeds)
{
	EXPECT_EQ(LibKernel::Memory::KernelClearVirtualRangeName(nullptr, 0), OK);
}

// Gen5 Posix_v1 semaphore: init/post/wait/destroy round-trip on guest layout.
TEST(EmulatorKernelProcess, PosixSemInitPostWaitDestroy)
{
	EnsureKernelProcessSubsystems();
	struct
	{
		uint16_t magic;
		uint16_t nameid;
		uint32_t has_waiters;
		uint32_t count;
		uint32_t flags;
	} guest {};
	static_assert(sizeof(guest) == 16);

	EXPECT_EQ(Posix::sem_init(&guest, 0, 0), OK);
	EXPECT_EQ(Posix::sem_post(&guest), OK);
	EXPECT_EQ(Posix::sem_wait(&guest), OK);
	EXPECT_EQ(Posix::sem_destroy(&guest), OK);
	EXPECT_EQ(guest.magic, 0);
}

UT_END();
