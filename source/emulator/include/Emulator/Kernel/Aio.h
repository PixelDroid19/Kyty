#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_AIO_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_AIO_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

// Kernel asynchronous file I/O. Requests run to completion when they are
// submitted, so every submission is already COMPLETED when the guest polls or
// waits for it; the submission id stays valid until it is deleted.
namespace Kyty::Kernel::Aio {

struct AioResult
{
	int64_t  return_value;
	uint32_t state;
	uint32_t reserved;
};

struct AioRwRequest
{
	int64_t    offset;
	uint64_t   nbyte;
	void*      buf;
	AioResult* result;
	int        fd;
	int        reserved;
};

static_assert(sizeof(AioResult) == 16);
static_assert(sizeof(AioRwRequest) == 40);

constexpr uint32_t AIO_STATE_COMPLETED = 3;

using AioSubmitId = int32_t;

int KYTY_SYSV_ABI AioInitializeParam(void* param);
int KYTY_SYSV_ABI AioInitializeImpl(void* param, int size);
int KYTY_SYSV_ABI AioSubmitReadCommands(AioRwRequest* requests, int count, int priority, AioSubmitId* id);
int KYTY_SYSV_ABI AioSubmitReadCommandsMultiple(AioRwRequest* requests, int count, int priority, AioSubmitId* ids);
int KYTY_SYSV_ABI AioSubmitWriteCommands(AioRwRequest* requests, int count, int priority, AioSubmitId* id);
int KYTY_SYSV_ABI AioSubmitWriteCommandsMultiple(AioRwRequest* requests, int count, int priority, AioSubmitId* ids);
int KYTY_SYSV_ABI AioPollRequest(AioSubmitId id, int* state);
int KYTY_SYSV_ABI AioPollRequests(const AioSubmitId* ids, int count, int* states);
int KYTY_SYSV_ABI AioWaitRequest(AioSubmitId id, int* state, uint32_t* usec);
int KYTY_SYSV_ABI AioWaitRequests(const AioSubmitId* ids, int count, int* states, uint32_t mode, uint32_t* usec);
int KYTY_SYSV_ABI AioDeleteRequest(AioSubmitId id, int* result);
int KYTY_SYSV_ABI AioDeleteRequests(const AioSubmitId* ids, int count, int* results);

} // namespace Kyty::Kernel::Aio

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_AIO_H_ */
