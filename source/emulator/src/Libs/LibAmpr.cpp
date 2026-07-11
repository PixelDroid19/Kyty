#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include "Kyty/Core/Threads.h"

#include <cstring>
#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

// libSceAmpr: Gen5 APR/command-buffer helpers for async file/memory ops.
// Layout and measure sizes reimplemented from public export naming and
// observed guest usage (no third-party source pasted).
LIB_VERSION("Ampr", 1, "Ampr", 1, 1);

namespace Ampr {

// Guest-visible command-buffer object header (40 bytes).
constexpr size_t   kHeaderSize     = 0x28;
constexpr uint64_t kOffSelf        = 0x00;
constexpr uint64_t kOffData        = 0x08;
constexpr uint64_t kOffSize        = 0x10;
constexpr uint64_t kOffAux0        = 0x18;
constexpr uint64_t kOffAux1        = 0x20;

// Fixed record sizes returned by measure APIs (guest sizes the command stream).
constexpr uint64_t kReadFileRecordSize         = 0x30;
constexpr uint64_t kKernelEventQueueRecordSize = 0x30;
constexpr uint64_t kWriteAddressRecordSize     = 0x20;

struct CommandBufferState
{
	uint64_t data         = 0;
	uint64_t size         = 0;
	uint64_t write_offset = 0;
};

static Core::Mutex                                      g_ampr_mutex;
static std::unordered_map<uint64_t, CommandBufferState> g_buffers;

static void WriteU64(void* p, uint64_t v)
{
	std::memcpy(p, &v, sizeof(v));
}

static uint64_t ReadU64(const void* p)
{
	uint64_t v = 0;
	std::memcpy(&v, p, sizeof(v));
	return v;
}

static bool WriteHeader(uint64_t cmd, uint64_t data, uint64_t size, uint64_t aux0, uint64_t aux1)
{
	if (cmd == 0)
	{
		return false;
	}
	uint8_t header[kHeaderSize] {};
	WriteU64(header + kOffSelf, cmd);
	WriteU64(header + kOffData, data);
	WriteU64(header + kOffSize, size);
	WriteU64(header + kOffAux0, aux0);
	WriteU64(header + kOffAux1, aux1);
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(cmd)), header, kHeaderSize);
	return true;
}

static bool WriteVisiblePointers(uint64_t cmd, uint64_t data, uint64_t size)
{
	if (cmd == 0)
	{
		return false;
	}
	uint8_t ptrs[sizeof(uint64_t) * 3] {};
	WriteU64(ptrs + 0, cmd);
	WriteU64(ptrs + 8, data);
	WriteU64(ptrs + 16, size);
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(cmd)), ptrs, sizeof(ptrs));
	return true;
}

static void UpdateState(uint64_t cmd, uint64_t data, uint64_t size, uint64_t write_offset)
{
	Core::LockGuard lock(g_ampr_mutex);
	CommandBufferState st {};
	st.data         = data;
	st.size         = size;
	st.write_offset = write_offset;
	g_buffers[cmd]  = st;
}

static bool TryGetState(uint64_t cmd, CommandBufferState* out)
{
	Core::LockGuard lock(g_ampr_mutex);
	auto            it = g_buffers.find(cmd);
	if (it == g_buffers.end())
	{
		// Recover from guest memory if host map is cold.
		if (cmd == 0)
		{
			return false;
		}
		const auto* base = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(cmd));
		CommandBufferState st {};
		st.data         = ReadU64(base + kOffData);
		st.size         = ReadU64(base + kOffSize);
		st.write_offset = 0;
		g_buffers[cmd]  = st;
		*out            = st;
		return true;
	}
	*out = it->second;
	return true;
}

// --- measure APIs ------------------------------------------------------------

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeReadFile()
{
	PRINT_NAME();
	return kReadFileRecordSize;
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeWriteKernelEventQueue0400()
{
	PRINT_NAME();
	return kKernelEventQueueRecordSize;
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeWriteAddressOnCompletion()
{
	PRINT_NAME();
	return kWriteAddressRecordSize;
}

// --- command buffer lifecycle ------------------------------------------------

// sceAmprCommandBufferConstructor(cmd, buffer, size) → returns cmd
static KYTY_SYSV_ABI uint64_t CommandBufferConstructor(void* cmd_obj, void* buffer, uint64_t size)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	const uint64_t buf = reinterpret_cast<uint64_t>(buffer);
	printf("\t cmd    = 0x%016" PRIx64 "\n", cmd);
	printf("\t buffer = 0x%016" PRIx64 "\n", buf);
	printf("\t size   = 0x%016" PRIx64 "\n", size);
	if (cmd == 0)
	{
		return 0;
	}
	if (!WriteHeader(cmd, buf, size, 0, 0))
	{
		return 0;
	}
	UpdateState(cmd, buf, size, 0);
	return cmd;
}

// sceAmprAprCommandBufferConstructor(cmd, aux0, aux1) → returns cmd
static KYTY_SYSV_ABI uint64_t AprCommandBufferConstructor(void* cmd_obj, uint64_t aux0, uint64_t aux1)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	printf("\t cmd  = 0x%016" PRIx64 "\n", cmd);
	printf("\t aux0 = 0x%016" PRIx64 "\n", aux0);
	printf("\t aux1 = 0x%016" PRIx64 "\n", aux1);
	if (cmd == 0)
	{
		return 0;
	}
	// Preserve existing data/size; set aux fields.
	CommandBufferState st {};
	uint64_t           data = 0;
	uint64_t           size = 0;
	if (TryGetState(cmd, &st))
	{
		data = st.data;
		size = st.size;
	}
	else
	{
		const auto* base = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(cmd));
		data             = ReadU64(base + kOffData);
		size             = ReadU64(base + kOffSize);
	}
	if (!WriteHeader(cmd, data, size, aux0, aux1))
	{
		return 0;
	}
	UpdateState(cmd, data, size, 0);
	return cmd;
}

static KYTY_SYSV_ABI int CommandBufferDestructor(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	printf("\t cmd = 0x%016" PRIx64 "\n", cmd);
	if (cmd == 0)
	{
		return OK;
	}
	WriteVisiblePointers(cmd, 0, 0);
	Core::LockGuard lock(g_ampr_mutex);
	g_buffers.erase(cmd);
	return OK;
}

static KYTY_SYSV_ABI int AprCommandBufferDestructor(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	printf("\t cmd = 0x%016" PRIx64 "\n", cmd);
	if (cmd == 0)
	{
		return OK;
	}
	// Zero aux pointers only.
	uint8_t zeros[16] {};
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(cmd + kOffAux0)), zeros, sizeof(zeros));
	return OK;
}

static KYTY_SYSV_ABI int CommandBufferSetBuffer(void* cmd_obj, void* buffer, uint64_t size)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	const uint64_t buf = reinterpret_cast<uint64_t>(buffer);
	printf("\t cmd    = 0x%016" PRIx64 "\n", cmd);
	printf("\t buffer = 0x%016" PRIx64 "\n", buf);
	printf("\t size   = 0x%016" PRIx64 "\n", size);
	if (cmd == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (!WriteVisiblePointers(cmd, buf, size))
	{
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	UpdateState(cmd, buf, size, 0);
	return OK;
}

static KYTY_SYSV_ABI int CommandBufferReset(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	printf("\t cmd = 0x%016" PRIx64 "\n", cmd);
	if (cmd == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	CommandBufferState st {};
	if (!TryGetState(cmd, &st))
	{
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	if (!WriteVisiblePointers(cmd, st.data, st.size))
	{
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	UpdateState(cmd, st.data, st.size, 0);
	return OK;
}

static KYTY_SYSV_ABI uint64_t CommandBufferClearBuffer(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	printf("\t cmd = 0x%016" PRIx64 "\n", cmd);
	if (cmd == 0)
	{
		return 0;
	}
	CommandBufferState st {};
	if (!TryGetState(cmd, &st))
	{
		return 0;
	}
	const uint64_t old = st.data;
	WriteVisiblePointers(cmd, 0, 0);
	Core::LockGuard lock(g_ampr_mutex);
	g_buffers.erase(cmd);
	return old;
}

static KYTY_SYSV_ABI uint64_t CommandBufferGetSize(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	CommandBufferState st {};
	if (!TryGetState(cmd, &st))
	{
		return 0;
	}
	printf("\t cmd  = 0x%016" PRIx64 " size = 0x%016" PRIx64 "\n", cmd, st.size);
	return st.size;
}

static KYTY_SYSV_ABI uint64_t CommandBufferGetCurrentOffset(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	CommandBufferState st {};
	if (!TryGetState(cmd, &st))
	{
		return 0;
	}
	printf("\t cmd = 0x%016" PRIx64 " offset = 0x%016" PRIx64 "\n", cmd, st.write_offset);
	return st.write_offset;
}

// Residual Ampr NIDs observed after measure APIs on the second private title.
// Names not yet triangulated; log args and return success so boot can proceed
// to the next evidenced fail. Replace with named contracts when known.
static KYTY_SYSV_ABI int AmprLogAndOk(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	PRINT_NAME();
	printf("\t a0 = 0x%016" PRIx64 "\n", a0);
	printf("\t a1 = 0x%016" PRIx64 "\n", a1);
	printf("\t a2 = 0x%016" PRIx64 "\n", a2);
	printf("\t a3 = 0x%016" PRIx64 "\n", a3);
	return OK;
}

static KYTY_SYSV_ABI uint64_t AmprLogAndReturn0(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	PRINT_NAME();
	printf("\t a0 = 0x%016" PRIx64 "\n", a0);
	printf("\t a1 = 0x%016" PRIx64 "\n", a1);
	printf("\t a2 = 0x%016" PRIx64 "\n", a2);
	printf("\t a3 = 0x%016" PRIx64 "\n", a3);
	return 0;
}

} // namespace Ampr

LIB_DEFINE(InitAmpr_1)
{
	// Measure (fixed record sizes)
	LIB_FUNC("vWU-odnS+fU", Ampr::MeasureCommandSizeReadFile);
	LIB_FUNC("sSAUCCU1dv4", Ampr::MeasureCommandSizeWriteKernelEventQueue0400);
	LIB_FUNC("C+IEj+BsAFM", Ampr::MeasureCommandSizeWriteAddressOnCompletion);

	// Command buffer lifecycle (public export names / ABI from Gen5 usage)
	LIB_FUNC("8aI7R7WaOlc", Ampr::CommandBufferConstructor);
	LIB_FUNC("a8uLzYY--tM", Ampr::AprCommandBufferConstructor);
	LIB_FUNC("GuchCTefuZw", Ampr::CommandBufferDestructor);
	LIB_FUNC("Qs1xtplKo0U", Ampr::AprCommandBufferDestructor);
	LIB_FUNC("N-FSPA4S3nI", Ampr::CommandBufferSetBuffer);
	LIB_FUNC("baQO9ez2gL4", Ampr::CommandBufferReset);
	LIB_FUNC("ULvXMDz56po", Ampr::CommandBufferClearBuffer);
	LIB_FUNC("tZDDEo2tE5k", Ampr::CommandBufferGetSize);
	LIB_FUNC("GnxKOHEawhk", Ampr::CommandBufferGetCurrentOffset);

	// Residual NIDs from second-title import stream (after measure APIs)
	LIB_FUNC("Zi3dBUjgyXI", Ampr::AmprLogAndOk);
	LIB_FUNC("4muPEJ-x5N8", Ampr::AmprLogAndOk);
	LIB_FUNC("qesF88X4DRg", Ampr::AmprLogAndReturn0);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
