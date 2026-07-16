#include "Emulator/Common.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include "Kyty/Core/File.h"
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
constexpr uint64_t kWriteCounterRecordSize     = 0x20;
constexpr uint32_t kWriteAddressRecordType     = 3;

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

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeWriteKernelEventQueueOnCompletion()
{
	PRINT_NAME();
	return kKernelEventQueueRecordSize;
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeWriteCounterOnCompletion()
{
	PRINT_NAME();
	return kWriteCounterRecordSize;
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeReadFileGather()
{
	PRINT_NAME();
	// Same payload footprint as a single ReadFile record until gather layout is evidenced.
	return kReadFileRecordSize;
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

// sceAmprCommandBufferGetNumCommands (NID gzndltBEzWc). Same ABI as GetSize:
// count in RAX. Report drained/empty so poll-until-zero callers proceed.
// Ported from SharpEmu feature/astro-bot-baseline.
static KYTY_SYSV_ABI uint64_t CommandBufferGetNumCommands(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	printf("\t cmd = 0x%016" PRIx64 "\n", cmd);
	if (cmd == 0)
	{
		return 0;
	}
	return 0;
}

// sceAmprAprCommandBufferReadFile(cmd, a1, a2, file_id, dest, size, file_offset)
// Reads host file bytes into guest dest and appends a fixed-size ReadFile record
// to the command buffer stream (measure size 0x30).
static KYTY_SYSV_ABI int AprCommandBufferReadFile(void* cmd_obj, uint64_t /*a1*/, uint64_t /*a2*/, uint32_t file_id, void* dest,
                                                  uint64_t size, uint64_t file_offset)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	printf("\t cmd         = 0x%016" PRIx64 "\n", cmd);
	printf("\t file_id     = 0x%08" PRIx32 "\n", file_id);
	printf("\t dest        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dest));
	printf("\t size        = 0x%016" PRIx64 "\n", size);
	printf("\t file_offset = 0x%016" PRIx64 "\n", file_offset);

	if (cmd == 0 || (dest == nullptr && size != 0))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	Core::String host_path;
	if (!LibKernel::FileSystem::AprTryGetHostPath(file_id, &host_path))
	{
		printf("\t unknown APR file id\n");
		return LibKernel::KERNEL_ERROR_ENOENT;
	}

	if (size != 0)
	{
		Core::File f;
		if (!f.Open(host_path, Core::File::Mode::Read))
		{
			printf("\t open failed: %s\n", host_path.C_Str());
			return LibKernel::KERNEL_ERROR_ENOENT;
		}
		if (!f.Seek(file_offset))
		{
			return LibKernel::KERNEL_ERROR_EINVAL;
		}
		uint32_t read_n = 0;
		f.Read(dest, static_cast<uint32_t>(size > UINT32_MAX ? UINT32_MAX : size), &read_n);
		if (static_cast<uint64_t>(read_n) != size)
		{
			printf("\t short read: got %" PRIu32 " want %" PRIu64 "\n", read_n, size);
			return LibKernel::KERNEL_ERROR_EIO;
		}
	}

	CommandBufferState st {};
	if (!TryGetState(cmd, &st))
	{
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	if (st.write_offset + kReadFileRecordSize > st.size)
	{
		printf("\t command buffer overflow\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (st.data != 0)
	{
		uint8_t record[kReadFileRecordSize] {};
		WriteU64(record + 0x00, cmd);
		WriteU64(record + 0x08, static_cast<uint64_t>(file_id));
		WriteU64(record + 0x10, reinterpret_cast<uint64_t>(dest));
		WriteU64(record + 0x18, size);
		WriteU64(record + 0x20, file_offset);
		WriteU64(record + 0x28, size); // bytes_read
		std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(st.data + st.write_offset)), record, kReadFileRecordSize);
	}
	UpdateState(cmd, st.data, st.size, st.write_offset + kReadFileRecordSize);
	return OK;
}

// sceAmprCommandBufferWriteAddressOnCompletion(cmd, address, value)
// Eagerly stores value at address (same eager model as ReadFile) and appends a
// 0x20 completion record. NID/name from PS5 libSceAmpr stubs (sJXyWHjP-F8).
static KYTY_SYSV_ABI int CommandBufferWriteAddressOnCompletion(void* cmd_obj, uint64_t* address, uint64_t value)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	printf("\t cmd     = 0x%016" PRIx64 "\n", cmd);
	printf("\t address = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
	printf("\t value   = 0x%016" PRIx64 "\n", value);

	if (cmd == 0 || address == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	*address = value;

	CommandBufferState st {};
	if (!TryGetState(cmd, &st))
	{
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	if (st.write_offset + kWriteAddressRecordSize > st.size)
	{
		printf("\t command buffer overflow\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (st.data != 0)
	{
		uint8_t record[kWriteAddressRecordSize] {};
		WriteU64(record + 0x00, static_cast<uint64_t>(kWriteAddressRecordType));
		WriteU64(record + 0x08, reinterpret_cast<uint64_t>(address));
		WriteU64(record + 0x10, value);
		std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(st.data + st.write_offset)), record, kWriteAddressRecordSize);
	}
	UpdateState(cmd, st.data, st.size, st.write_offset + kWriteAddressRecordSize);
	return OK;
}

// sceAmprCommandBufferWriteKernelEventQueueOnCompletion (o67gODLFpls)
// Eager model (same as ReadFile / WriteAddressOnCompletion): append the record
// and wake the Ampr equeue registration immediately. Real hardware defers this
// until AprSubmit; sync HLE is enough while submit remains unimplemented.
static KYTY_SYSV_ABI int CommandBufferWriteKernelEventQueueOnCompletion(void* cmd_obj, void* equeue, uint64_t ident,
                                                                        uint64_t completion_token, uint64_t user_data)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	printf("\t cmd              = 0x%016" PRIx64 "\n", cmd);
	printf("\t equeue           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(equeue));
	printf("\t ident            = 0x%016" PRIx64 "\n", ident);
	printf("\t completion_token = 0x%016" PRIx64 "\n", completion_token);
	printf("\t user_data        = 0x%016" PRIx64 "\n", user_data);

	if (cmd == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	CommandBufferState st {};
	if (!TryGetState(cmd, &st))
	{
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	if (st.write_offset + kKernelEventQueueRecordSize > st.size)
	{
		printf("\t command buffer overflow\n");
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (st.data != 0)
	{
		uint8_t record[kKernelEventQueueRecordSize] {};
		WriteU64(record + 0x00, reinterpret_cast<uint64_t>(equeue));
		WriteU64(record + 0x08, ident);
		WriteU64(record + 0x10, completion_token);
		WriteU64(record + 0x18, user_data);
		std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(st.data + st.write_offset)), record, kKernelEventQueueRecordSize);
	}
	UpdateState(cmd, st.data, st.size, st.write_offset + kKernelEventQueueRecordSize);

	if (equeue != nullptr)
	{
		auto* eq = static_cast<LibKernel::EventQueue::KernelEqueue>(equeue);
		const int trigger_rc =
		    LibKernel::EventQueue::KernelTriggerEvent(eq, static_cast<uintptr_t>(ident), LibKernel::EventQueue::KERNEL_EVFILT_AMPR,
		                                              reinterpret_cast<void*>(static_cast<uintptr_t>(completion_token)));
		if (trigger_rc != OK && trigger_rc != LibKernel::KERNEL_ERROR_ENOENT)
		{
			return trigger_rc;
		}
	}
	return OK;
}

} // namespace Ampr

LIB_DEFINE(InitAmpr_1)
{
	// Measure APIs — NIDs from libSceAmpr stubs.
	LIB_FUNC("vWU-odnS+fU", Ampr::MeasureCommandSizeReadFile);
	LIB_FUNC("sSAUCCU1dv4", Ampr::MeasureCommandSizeWriteKernelEventQueue0400);
	LIB_FUNC("C+IEj+BsAFM", Ampr::MeasureCommandSizeWriteAddressOnCompletion);
	LIB_FUNC("Zi3dBUjgyXI", Ampr::MeasureCommandSizeWriteKernelEventQueueOnCompletion);
	LIB_FUNC("4muPEJ-x5N8", Ampr::MeasureCommandSizeWriteCounterOnCompletion);
	LIB_FUNC("qesF88X4DRg", Ampr::MeasureCommandSizeReadFileGather);

	// Command buffer lifecycle
	LIB_FUNC("8aI7R7WaOlc", Ampr::CommandBufferConstructor);
	LIB_FUNC("a8uLzYY--tM", Ampr::AprCommandBufferConstructor);
	LIB_FUNC("GuchCTefuZw", Ampr::CommandBufferDestructor);
	LIB_FUNC("Qs1xtplKo0U", Ampr::AprCommandBufferDestructor);
	LIB_FUNC("N-FSPA4S3nI", Ampr::CommandBufferSetBuffer);
	LIB_FUNC("baQO9ez2gL4", Ampr::CommandBufferReset);
	LIB_FUNC("ULvXMDz56po", Ampr::CommandBufferClearBuffer);
	LIB_FUNC("tZDDEo2tE5k", Ampr::CommandBufferGetSize);
	LIB_FUNC("GnxKOHEawhk", Ampr::CommandBufferGetCurrentOffset);
	LIB_FUNC("gzndltBEzWc", Ampr::CommandBufferGetNumCommands);

	// APR / completion builders
	LIB_FUNC("mQ16-QdKv7k", Ampr::AprCommandBufferReadFile);
	LIB_FUNC("sJXyWHjP-F8", Ampr::CommandBufferWriteAddressOnCompletion);
	LIB_FUNC("o67gODLFpls", Ampr::CommandBufferWriteKernelEventQueueOnCompletion);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
