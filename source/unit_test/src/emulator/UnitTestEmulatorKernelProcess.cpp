#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/RetailKernel.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Kyty/Core/File.h"
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

// Incomplete package dumps omit SIE system fonts and some Futura weights; score
// prefers same-family and close weight (Heavy → Heavy/Bold, Medium → Bold).
TEST(EmulatorKernelProcess, PackageFontFallbackScoresWeightAndFamily)
{
	using LibKernel::FileSystem::ScorePackageFontFallback;

	EXPECT_GT(ScorePackageFontFallback(U"SIE-ShinGoPr6N-Heavy.otf", U"Cobe-Heavy.otf"),
	          ScorePackageFontFallback(U"SIE-ShinGoPr6N-Heavy.otf", U"FuturaStd-Bold.otf"));
	EXPECT_GT(ScorePackageFontFallback(U"FuturaStd-Medium.otf", U"FuturaStd-Bold.otf"),
	          ScorePackageFontFallback(U"FuturaStd-Medium.otf", U"SeolSans-Heavy.otf"));
	EXPECT_GT(ScorePackageFontFallback(U"FuturaStd-Medium.otf", U"FuturaStd-Bold.otf"),
	          ScorePackageFontFallback(U"FuturaStd-Medium.otf", U"Kallisto-Medium.otf"));
	EXPECT_LT(ScorePackageFontFallback(U"SIE-ShinGoPr6N-Heavy.otf", U"readme.txt"), 0);
	EXPECT_EQ(ScorePackageFontFallback(U"FuturaStd-Bold.otf", U"FuturaStd-Bold.otf"), 100000);
}

// Incomplete Astro dumps store object defs as .odxb while the guest opens .odx.
TEST(EmulatorKernelProcess, HostExtensionAliasMapsOdxToOdxb)
{
	EnsureKernelProcessSubsystems();

	const String dir = U"/tmp/kyty_odx_alias_test/";
	Core::File::DeleteDirectories(dir);
	ASSERT_TRUE(Core::File::CreateDirectories(dir));

	const String odxb    = dir + U"ui_effect_temp_1.odxb";
	const String odx     = dir + U"ui_effect_temp_1.odx";
	{
		Core::File f;
		ASSERT_TRUE(f.Create(odxb));
		f.Write(U"odxb");
		f.Close();
	}
	ASSERT_TRUE(Core::File::IsFileExisting(odxb));
	ASSERT_FALSE(Core::File::IsFileExisting(odx));

	EXPECT_EQ(LibKernel::FileSystem::PreferHostExtensionAlias(odx), odxb);
	EXPECT_EQ(LibKernel::FileSystem::PreferHostExtensionAlias(odxb), odxb);
	EXPECT_EQ(LibKernel::FileSystem::PreferHostExtensionAlias(dir + U"missing.odx"), dir + U"missing.odx");

	Core::File::DeleteDirectories(dir);
}

// Astro path builders open /app0/prein/... while package files live under /app0/data/prein/...
TEST(EmulatorKernelProcess, HostApp0DataSegmentMapsPreinUnderData)
{
	EnsureKernelProcessSubsystems();

	const String root = U"/tmp/kyty_app0_data_seg_test/";
	Core::File::DeleteDirectories(root);
	ASSERT_TRUE(Core::File::CreateDirectories(root + U"data/prein/effects/odx/"));

	const String real_odxb = root + U"data/prein/effects/odx/ui_effect_temp_1.odxb";
	{
		Core::File f;
		ASSERT_TRUE(f.Create(real_odxb));
		f.Write(U"odxb");
		f.Close();
	}
	ASSERT_TRUE(Core::File::IsFileExisting(real_odxb));

	const String guest_wrong = U"/app0/prein/effects/odx/ui_effect_temp_1.odxb";
	const String host_wrong  = root + U"prein/effects/odx/ui_effect_temp_1.odxb";
	ASSERT_FALSE(Core::File::IsFileExisting(host_wrong));

	EXPECT_EQ(LibKernel::FileSystem::PreferHostApp0DataSegment(guest_wrong, host_wrong), real_odxb);

	// Combined with extension alias: guest .odx under wrong root → data/ + .odxb.
	const String guest_odx = U"/app0/prein/effects/odx/ui_effect_temp_1.odx";
	const String host_odx  = root + U"prein/effects/odx/ui_effect_temp_1.odx";
	EXPECT_EQ(LibKernel::FileSystem::PreferHostApp0DataSegment(guest_odx, host_odx), real_odxb);

	// Correct /app0/data/... guest is not rewritten.
	EXPECT_EQ(LibKernel::FileSystem::PreferHostApp0DataSegment(U"/app0/data/prein/effects/odx/ui_effect_temp_1.odxb", real_odxb),
	          real_odxb);

	Core::File::DeleteDirectories(root);
}

// PreferPackageFontHostPath substitutes a sibling OTF when the requested file is missing.
TEST(EmulatorKernelProcess, PackageFontHostPathPicksSibling)
{
	EnsureKernelProcessSubsystems();

	const String dir = U"/tmp/kyty_font_fallback_test/";
	Core::File::DeleteDirectories(dir);
	ASSERT_TRUE(Core::File::CreateDirectories(dir));

	const String present = dir + U"FuturaStd-Bold.otf";
	const String missing = dir + U"SIE-ShinGoPr6N-Heavy.otf";
	{
		Core::File f;
		ASSERT_TRUE(f.Create(present));
		f.Write(U"otf");
		f.Close();
	}
	ASSERT_TRUE(Core::File::IsFileExisting(present));
	ASSERT_FALSE(Core::File::IsFileExisting(missing));

	const String chosen = LibKernel::FileSystem::PreferPackageFontHostPath(missing);
	EXPECT_EQ(chosen, present);
	// Exact hit is unchanged.
	EXPECT_EQ(LibKernel::FileSystem::PreferPackageFontHostPath(present), present);
	// Non-font missing path is unchanged.
	EXPECT_EQ(LibKernel::FileSystem::PreferPackageFontHostPath(dir + U"missing.bin"), dir + U"missing.bin");

	Core::File::DeleteDirectories(dir);
}

// Bare /app0/.jxm|.skel|.anim map to companions of the last OD host path.
TEST(EmulatorKernelProcess, HostOdCompanionMapsBareExtensionsFromLastOdxb)
{
	EnsureKernelProcessSubsystems();

	const String root = U"/tmp/kyty_od_companion_test/";
	Core::File::DeleteDirectories(root);
	ASSERT_TRUE(Core::File::CreateDirectories(root + U"data/prein/effects/odx/"));
	ASSERT_TRUE(Core::File::CreateDirectories(root + U"data/prein/effects/gfx/"));
	ASSERT_TRUE(Core::File::CreateDirectories(root + U"data/prein/effects/anim/"));

	const String odxb = root + U"data/prein/effects/odx/ui_effect_temp_1.odxb";
	const String jxm  = root + U"data/prein/effects/gfx/ui_effect_temp_1.jxm";
	const String skel = root + U"data/prein/effects/anim/ui_effect_temp_1.skel";
	const String anim = root + U"data/prein/effects/anim/ui_effect_temp_1_anim_play.anim";
	for (const String& path : {odxb, jxm, skel, anim})
	{
		Core::File f;
		ASSERT_TRUE(f.Create(path));
		f.Write(U"x");
		f.Close();
		ASSERT_TRUE(Core::File::IsFileExisting(path));
	}

	const String bare_jxm  = root + U".jxm";
	const String bare_skel = root + U".skel";
	const String bare_anim = root + U".anim";
	EXPECT_EQ(LibKernel::FileSystem::PreferHostOdCompanionAsset(U"/app0/.jxm", bare_jxm, odxb), jxm);
	EXPECT_EQ(LibKernel::FileSystem::PreferHostOdCompanionAsset(U"/app0/.skel", bare_skel, odxb), skel);
	EXPECT_EQ(LibKernel::FileSystem::PreferHostOdCompanionAsset(U"/app0/.anim", bare_anim, odxb), anim);
	EXPECT_EQ(LibKernel::FileSystem::PreferHostOdCompanionAsset(U"/app0/.jxm", bare_jxm, U""), bare_jxm);

	Core::File::DeleteDirectories(root);
}

// Gen5 Posix_v1 pthread_attr_init/setstacksize wrap LibKernel attr helpers.
TEST(EmulatorKernelProcess, PosixPthreadAttrInitAndSetstacksize)
{
	EnsureKernelProcessSubsystems();

	LibKernel::PthreadAttr attr = nullptr;
	EXPECT_EQ(Posix::pthread_attr_init(&attr), OK);
	ASSERT_NE(attr, nullptr);
	EXPECT_EQ(Posix::pthread_attr_setstacksize(&attr, 0x400000), OK);
	size_t stack_size = 0;
	EXPECT_EQ(Posix::pthread_attr_getstacksize(&attr, &stack_size), OK);
	EXPECT_EQ(stack_size, static_cast<size_t>(0x400000));
	EXPECT_EQ(Posix::pthread_attr_destroy(&attr), OK);
}

// Gen5 Posix_v1 pthread_detach rejects null thread like LibKernel::PthreadDetach.
TEST(EmulatorKernelProcess, PosixPthreadDetachRejectsNull)
{
	EXPECT_NE(Posix::pthread_detach(nullptr), OK);
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
