#include "Emulator/Kernel/Aio.h"

#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Errno.h"

#include <cstring>
#include <mutex>
#include <unordered_set>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::Aio {

using namespace Libs::LibKernel;

namespace {

constexpr size_t kParamSize    = 0x3c; // three 20-byte scheduling parameter blocks
constexpr int    kMaxRequests  = 0x10000;
constexpr int    kMaxWaitBatch = 128;

std::mutex                      g_mutex;
std::unordered_set<AioSubmitId> g_submitted;
AioSubmitId                     g_next_id = 0;

AioSubmitId Register()
{
	std::lock_guard lock(g_mutex);
	const AioSubmitId id = ++g_next_id;
	g_submitted.insert(id);
	return id;
}

bool IsSubmitted(AioSubmitId id)
{
	std::lock_guard lock(g_mutex);
	return g_submitted.count(id) != 0;
}

bool Forget(AioSubmitId id)
{
	std::lock_guard lock(g_mutex);
	return g_submitted.erase(id) != 0;
}

// A failed transfer reports the kernel error as its return value.
void Execute(AioRwRequest* request, bool write)
{
	const int64_t transferred = write ? FileSystem::KernelPwrite(request->fd, request->buf, request->nbyte, request->offset)
	                                  : FileSystem::KernelPread(request->fd, request->buf, request->nbyte, request->offset);
	if (request->result != nullptr)
	{
		request->result->return_value = transferred;
		request->result->state        = AIO_STATE_COMPLETED;
	}
}

int Submit(AioRwRequest* requests, int count, AioSubmitId* ids, bool write, bool one_id_per_request)
{
	if (requests == nullptr || ids == nullptr || count <= 0 || count > kMaxRequests)
	{
		return KERNEL_ERROR_EINVAL;
	}
	for (int i = 0; i < count; i++)
	{
		Execute(&requests[i], write);
		if (one_id_per_request)
		{
			ids[i] = Register();
		}
	}
	if (!one_id_per_request)
	{
		ids[0] = Register();
	}
	return OK;
}

int States(const AioSubmitId* ids, int count, int* states)
{
	if (ids == nullptr || states == nullptr || count <= 0 || count > kMaxWaitBatch)
	{
		return KERNEL_ERROR_EINVAL;
	}
	for (int i = 0; i < count; i++)
	{
		if (!IsSubmitted(ids[i]))
		{
			return KERNEL_ERROR_EINVAL;
		}
		states[i] = static_cast<int>(AIO_STATE_COMPLETED);
	}
	return OK;
}

} // namespace

int KYTY_SYSV_ABI AioInitializeParam(void* param)
{
	if (param == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	std::memset(param, 0, kParamSize);
	return OK;
}

int KYTY_SYSV_ABI AioInitializeImpl(void* param, int size)
{
	return param == nullptr || size < static_cast<int>(kParamSize) ? KERNEL_ERROR_EINVAL : OK;
}

int KYTY_SYSV_ABI AioSubmitReadCommands(AioRwRequest* requests, int count, int /*priority*/, AioSubmitId* id)
{
	return Submit(requests, count, id, false, false);
}

int KYTY_SYSV_ABI AioSubmitReadCommandsMultiple(AioRwRequest* requests, int count, int /*priority*/, AioSubmitId* ids)
{
	return Submit(requests, count, ids, false, true);
}

int KYTY_SYSV_ABI AioSubmitWriteCommands(AioRwRequest* requests, int count, int /*priority*/, AioSubmitId* id)
{
	return Submit(requests, count, id, true, false);
}

int KYTY_SYSV_ABI AioSubmitWriteCommandsMultiple(AioRwRequest* requests, int count, int /*priority*/, AioSubmitId* ids)
{
	return Submit(requests, count, ids, true, true);
}

int KYTY_SYSV_ABI AioPollRequest(AioSubmitId id, int* state)
{
	return States(&id, 1, state);
}

int KYTY_SYSV_ABI AioPollRequests(const AioSubmitId* ids, int count, int* states)
{
	return States(ids, count, states);
}

int KYTY_SYSV_ABI AioWaitRequest(AioSubmitId id, int* state, uint32_t* /*usec*/)
{
	return States(&id, 1, state);
}

// mode 1 waits for all requests, 2 for any; every submission is already done.
int KYTY_SYSV_ABI AioWaitRequests(const AioSubmitId* ids, int count, int* states, uint32_t mode, uint32_t* /*usec*/)
{
	return mode != 1 && mode != 2 ? KERNEL_ERROR_EINVAL : States(ids, count, states);
}

int KYTY_SYSV_ABI AioDeleteRequest(AioSubmitId id, int* result)
{
	if (!Forget(id))
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (result != nullptr)
	{
		*result = OK;
	}
	return OK;
}

int KYTY_SYSV_ABI AioDeleteRequests(const AioSubmitId* ids, int count, int* results)
{
	if (ids == nullptr || count <= 0 || count > kMaxWaitBatch)
	{
		return KERNEL_ERROR_EINVAL;
	}
	for (int i = 0; i < count; i++)
	{
		const int result = AioDeleteRequest(ids[i], results != nullptr ? &results[i] : nullptr);
		if (result != OK)
		{
			return result;
		}
	}
	return OK;
}

} // namespace Kyty::Kernel::Aio

#endif // KYTY_EMU_ENABLED
