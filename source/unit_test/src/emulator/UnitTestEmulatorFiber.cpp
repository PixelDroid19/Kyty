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
using namespace Kyty::Kernel::Fiber;

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

struct FiberRoundTripState
{
	uint64_t initial_run_arg = 0;
	uint64_t resumed_run_arg = 0;
	int32_t  return_result   = FIBER_ERROR_INVALID;
	uint32_t entry_count     = 0;
};

static KYTY_SYSV_ABI void RoundTripFiberEntry(uint64_t arg_init, uint64_t arg_run)
{
	auto* state = reinterpret_cast<FiberRoundTripState*>(arg_init);
	state->entry_count++;
	state->initial_run_arg = arg_run;
	state->return_result   = FiberReturnToThread(0xa1u, &state->resumed_run_arg);
}

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

TEST(EmulatorFiber, RunsReturnsAndResumesOnGuestStack)
{
	EnsureLog();

	alignas(16) uint8_t stack[4096] {};
	FiberObject         fiber {};
	FiberRoundTripState state {};

	ASSERT_EQ(FiberInitialize(&fiber, "round-trip", RoundTripFiberEntry, reinterpret_cast<uint64_t>(&state), stack,
	                          sizeof(stack), nullptr, 0x03500000u),
	          OK);

	uint64_t return_arg = 0;
	EXPECT_EQ(FiberRun(&fiber, 0xb1u, &return_arg), OK);
	EXPECT_EQ(return_arg, 0xa1u);
	EXPECT_EQ(state.entry_count, 1u);
	EXPECT_EQ(state.initial_run_arg, 0xb1u);
	EXPECT_EQ(state.return_result, FIBER_ERROR_INVALID);

	return_arg = 0xffffu;
	EXPECT_EQ(FiberRun(&fiber, 0xb2u, &return_arg), FIBER_ERROR_STATE);
	EXPECT_EQ(return_arg, 0u);
	EXPECT_EQ(state.entry_count, 1u);
	EXPECT_EQ(state.resumed_run_arg, 0xb2u);
	EXPECT_EQ(state.return_result, OK);
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
