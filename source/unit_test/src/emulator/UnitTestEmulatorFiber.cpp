#include "Emulator/Kernel/Fiber.h"
#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Kyty/UnitTest.h"

#include <cstdint>
#include <cstring>

UT_BEGIN(EmulatorFiber);

using namespace Libs;
using namespace Libs::Fiber;

namespace {

void EnsureLog()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

static KYTY_SYSV_ABI void DummyFiberEntry(uint64_t /*arg_init*/, uint64_t /*arg_run*/) {}

} // namespace

TEST(EmulatorFiber, LayoutAndValidateArgs)
{
	static_assert(sizeof(FiberCpuContext) == 80);
	static_assert(sizeof(FiberInfo) == 128);
	static_assert(sizeof(FiberObject) <= 256);

	EXPECT_EQ(FiberValidateInitializeArgs(nullptr, "n", DummyFiberEntry, nullptr, 0, nullptr), FIBER_ERROR_NULL);

	alignas(16) uint8_t stack[1024] {};
	FiberObject         fiber {};
	EXPECT_EQ(FiberValidateInitializeArgs(&fiber, "n", DummyFiberEntry, stack, 100, nullptr), FIBER_ERROR_RANGE);
	EXPECT_EQ(FiberValidateInitializeArgs(&fiber, "n", DummyFiberEntry, stack, 0, nullptr), FIBER_ERROR_INVALID);
	EXPECT_EQ(FiberValidateInitializeArgs(&fiber, "n", DummyFiberEntry, nullptr, 0, nullptr), OK);
}

TEST(EmulatorFiber, InitializeFillsGuestObject)
{
	EnsureLog();

	alignas(16) uint8_t stack[1024] {};
	FiberObject         fiber {};
	const int32_t       rc =
	    FiberInitialize(&fiber, "boot", DummyFiberEntry, 0x11, stack, sizeof(stack), nullptr, 0x03500000u);
	EXPECT_EQ(rc, OK);
	EXPECT_TRUE(FiberObjectIsValid(&fiber));
	EXPECT_EQ(fiber.state, FIBER_STATE_IDLE);
	EXPECT_TRUE(fiber.entry == DummyFiberEntry);
	EXPECT_EQ(fiber.arg_on_initialize, 0x11u);
	EXPECT_EQ(fiber.addr_context, static_cast<void*>(stack));
	EXPECT_EQ(fiber.size_context, sizeof(stack));
	EXPECT_EQ(fiber.flags, FIBER_FLAG_SET_FPU_REGS);
	EXPECT_STREQ(fiber.name, "boot");
	EXPECT_EQ(*reinterpret_cast<uint64_t*>(stack), FIBER_STACK_MAGIC);
	EXPECT_FALSE(fiber.context_valid);
}

TEST(EmulatorFiber, ResolvesFiberExports)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libFiber_1", &symbols));

	auto resolve = [&](const char16_t* nid) {
		Loader::SymbolResolve query {};
		query.name                 = nid;
		query.library              = U"Fiber";
		query.library_version      = 1;
		query.module               = U"Fiber";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		return symbols.Find(query) != nullptr;
	};

	EXPECT_TRUE(resolve(u"hVYD7Ou2pCQ"));
	EXPECT_TRUE(resolve(u"a0LLrZWac0M"));
	EXPECT_TRUE(resolve(u"PFT2S-tJ7Uk"));
	EXPECT_TRUE(resolve(u"B0ZX2hx9DMw"));
	EXPECT_TRUE(resolve(u"asjUJJ+aa8s"));
}

UT_END();
