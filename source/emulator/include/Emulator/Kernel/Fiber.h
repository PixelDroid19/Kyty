#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_FIBER_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_FIBER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Fiber {

// Guest Fiber errors (SCE_FIBER_ERROR_*).
constexpr int32_t FIBER_ERROR_NULL       = static_cast<int32_t>(0x80590001u);
constexpr int32_t FIBER_ERROR_ALIGNMENT  = static_cast<int32_t>(0x80590002u);
constexpr int32_t FIBER_ERROR_RANGE      = static_cast<int32_t>(0x80590003u);
constexpr int32_t FIBER_ERROR_INVALID    = static_cast<int32_t>(0x80590004u);
constexpr int32_t FIBER_ERROR_PERMISSION = static_cast<int32_t>(0x80590005u);
constexpr int32_t FIBER_ERROR_STATE      = static_cast<int32_t>(0x80590006u);

constexpr uint32_t FIBER_MAGIC_START      = 0xdef1649cu;
constexpr uint32_t FIBER_MAGIC_END        = 0xb37592a0u;
constexpr uint32_t FIBER_OPT_MAGIC        = 0xbb40e64du;
constexpr uint64_t FIBER_STACK_MAGIC      = 0x7149f2ca7149f2caull;
constexpr uint64_t FIBER_CONTEXT_MIN_SIZE = 512;
constexpr size_t   FIBER_MAX_NAME_LENGTH  = 31;

constexpr uint32_t FIBER_STATE_RUNNING    = 1;
constexpr uint32_t FIBER_STATE_IDLE       = 2;
constexpr uint32_t FIBER_STATE_TERMINATED = 3;
constexpr uint32_t FIBER_FLAG_SET_FPU_REGS = 0x100;

using FiberEntry = KYTY_SYSV_ABI void (*)(uint64_t arg_on_initialize, uint64_t arg_on_run);

struct FiberOptParam
{
	uint32_t magic;
};

struct FiberCpuContext
{
	uint64_t rbx;
	uint64_t rbp;
	uint64_t rdi;
	uint64_t rsi;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t rsp;
	uint64_t rip;
};

static_assert(sizeof(FiberCpuContext) == 80, "FiberCpuContext size");

// Guest-visible fiber control block filled by Initialize.
struct FiberObject
{
	uint32_t        magic_start;
	uint32_t        state;
	FiberEntry      entry;
	uint64_t        arg_on_initialize;
	void*           addr_context;
	uint64_t        size_context;
	char            name[FIBER_MAX_NAME_LENGTH + 1];
	void*           context;
	uint32_t        flags;
	uint32_t        padding;
	void*           context_start;
	void*           context_end;
	FiberCpuContext saved_context;
	uint64_t        arg_on_run;
	uint64_t        arg_on_return;
	bool            context_valid;
	uint32_t        magic_end;
};

static_assert(sizeof(FiberObject) <= 256, "FiberObject exceeds guest allocation budget");

struct FiberInfo
{
	uint64_t   size;
	FiberEntry entry;
	uint64_t   arg_on_initialize;
	void*      addr_context;
	uint64_t   size_context;
	char       name[FIBER_MAX_NAME_LENGTH + 1];
	uint64_t   size_context_margin;
	uint8_t    padding[48];
};

static_assert(sizeof(FiberInfo) == 128, "FiberInfo size");

// NID hVYD7Ou2pCQ — _sceFiberInitializeImpl
int32_t KYTY_SYSV_ABI FiberInitialize(FiberObject* fiber, const char* name, FiberEntry entry, uint64_t arg_on_initialize,
                                      void* addr_context, uint64_t size_context, const FiberOptParam* opt_param, uint32_t build_version);

// NID 7+OJIpko9RY — _sceFiberInitializeWithInternalOptionImpl
int32_t KYTY_SYSV_ABI FiberInitializeInternal(FiberObject* fiber, const char* name, FiberEntry entry, uint64_t arg_on_initialize,
                                              void* addr_context, uint64_t size_context, const FiberOptParam* opt_param, uint32_t flags,
                                              uint32_t build_version);

int32_t KYTY_SYSV_ABI FiberOptParamInitialize(FiberOptParam* opt_param);
int32_t KYTY_SYSV_ABI FiberFinalize(FiberObject* fiber);
int32_t KYTY_SYSV_ABI FiberRun(FiberObject* fiber, uint64_t arg_on_run, uint64_t* arg_on_return);
int32_t KYTY_SYSV_ABI FiberSwitch(FiberObject* fiber, uint64_t arg_on_run, uint64_t* arg_on_return);
int32_t KYTY_SYSV_ABI FiberGetSelf(FiberObject** fiber);
int32_t KYTY_SYSV_ABI FiberReturnToThread(uint64_t arg_on_return, uint64_t* arg_on_run);
int32_t KYTY_SYSV_ABI FiberGetInfo(FiberObject* fiber, FiberInfo* fiber_info);
int32_t KYTY_SYSV_ABI FiberStartContextSizeCheck(uint32_t flags);
int32_t KYTY_SYSV_ABI FiberStopContextSizeCheck();
int32_t KYTY_SYSV_ABI FiberRename(FiberObject* fiber, const char* name);
int32_t KYTY_SYSV_ABI FiberGetThreadFramePointerAddress(uint64_t* addr_frame_pointer);

// Pure helpers for deterministic tests (no guest context switch).
bool FiberObjectIsValid(const FiberObject* fiber);
int32_t FiberValidateInitializeArgs(const FiberObject* fiber, const char* name, FiberEntry entry, void* addr_context,
                                    uint64_t size_context, const FiberOptParam* opt_param);

} // namespace Kyty::Libs::Fiber

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_FIBER_H_ */
