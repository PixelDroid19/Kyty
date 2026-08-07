#include "Emulator/Common.h"
#include "Emulator/Kernel/AmprPort.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"
#include "Emulator/VideoFrameMemory.h"

#include "Kyty/Core/File.h"
#include "Kyty/Core/Threads.h"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <ctime>
#include <limits>
#include <unordered_map>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

// libSceAmpr: Gen5 APR/command-buffer helpers for async file/memory ops.
// Layout and measure sizes reimplemented from public export naming and
// observed guest usage (no third-party source pasted).
LIB_VERSION("Ampr", 1, "Ampr", 1, 1);

namespace Ampr {

constexpr uint64_t kHeaderSize                  = 0x18;
constexpr uint64_t kOffType                     = 0x00;
constexpr uint64_t kOffWriteOffset              = 0x04;
constexpr uint64_t kOffCommandCount             = 0x08;
constexpr uint64_t kOffSize                     = 0x0c;
constexpr uint64_t kOffData                     = 0x10;
constexpr uint64_t kOffAprMapState              = 0x18;
constexpr uint64_t kOffAprGatherScatterState    = 0x20;
constexpr uint32_t kMaxCommandBufferSize         = 64u * 1024u * 1024u;
constexpr uint64_t kReadFileRecordSize           = 0x14;
constexpr uint64_t kReadFileRecordSizeExtended   = 0x18;
constexpr uint64_t kKernelEventQueueRecordSize   = 0x20;
constexpr uint64_t kWriteAddressRecordSize       = 0x20;
constexpr uint64_t kWriteCounterRecordSize       = 0x20;
constexpr uint64_t kReadGatherRecordSize         = 0x08;
constexpr uint64_t kReadGatherRecordSizeExtended = 0x0c;
constexpr uint64_t kReadScatterRecordSize        = 0x0c;
constexpr uint64_t kReadGatherScatterRecordSize  = 0x10;
constexpr uint64_t kReadGatherScatterExtended    = 0x14;
constexpr uint64_t kResetGatherScatterRecordSize = 0x04;
constexpr uint64_t kAprMapBeginRecordSize        = 0x0c;
constexpr uint64_t kAprMapDirectBeginRecordSize  = 0x10;
constexpr uint64_t kAprMapEndRecordSize          = 0x04;
constexpr uint64_t kMaxAprReadSize               = UINT64_C(0x100000000);
constexpr uint64_t kMaxAprFileOffset             = UINT64_C(0x10000000000);
constexpr uint64_t kMaxAprAddress                = UINT64_C(0x0000f00000000000);
constexpr uint32_t kReadChunkSize                 = 4u * 1024u * 1024u;
constexpr uint32_t kWriteAddressRecordType        = 3;

enum class PendingActionKind: uint8_t
{
	ReadFile,
	WriteAddress,
	KernelEvent,
};

struct PendingAction
{
	PendingActionKind                           kind          = PendingActionKind::ReadFile;
	uint64_t                                    record_offset = 0;
	uint32_t                                    file_id       = 0;
	uint64_t                                    destination   = 0;
	uint64_t                                    size          = 0;
	uint64_t                                    file_offset   = 0;
	uint64_t                                    address       = 0;
	uint64_t                                    value         = 0;
	Kernel::EventQueue::KernelEqueueIdentity    equeue_identity {};
	uintptr_t                                   ident            = 0;
	uintptr_t                                   completion_token = 0;
};

struct CommandBufferState
{
	uint32_t                   type          = 0;
	uint32_t                   write_offset  = 0;
	int32_t                    command_count = 0;
	uint32_t                   size          = 0;
	uint64_t                   data          = 0;
	std::vector<PendingAction> actions;
	bool                       submitting                 = false;
	bool                       gather_scatter_valid       = false;
	uint32_t                   gather_scatter_file_id     = 0;
	uint64_t                   gather_scatter_destination = 0;
	uint64_t                   gather_scatter_file_offset = 0;
};

static Core::Mutex                                      g_ampr_mutex;
static std::unordered_map<uint64_t, CommandBufferState> g_buffers;
static std::unordered_map<uint64_t, uint64_t>           g_buffer_aliases;

template <typename T>
static T ReadValue(uint64_t address)
{
	T value {};
	std::memcpy(&value, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), sizeof(value));
	return value;
}

template <typename T>
static void WriteValue(uint64_t address, T value)
{
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), &value, sizeof(value));
}

static CommandBufferState ReadHeader(uint64_t command_buffer)
{
	CommandBufferState state {};
	state.type          = ReadValue<uint32_t>(command_buffer + kOffType);
	state.write_offset  = ReadValue<uint32_t>(command_buffer + kOffWriteOffset);
	state.command_count = ReadValue<int32_t>(command_buffer + kOffCommandCount);
	state.size          = ReadValue<uint32_t>(command_buffer + kOffSize);
	state.data          = ReadValue<uint64_t>(command_buffer + kOffData);
	return state;
}

static void WriteHeader(uint64_t command_buffer, const CommandBufferState& state)
{
	WriteValue(command_buffer + kOffType, state.type);
	WriteValue(command_buffer + kOffWriteOffset, state.write_offset);
	WriteValue(command_buffer + kOffCommandCount, state.command_count);
	WriteValue(command_buffer + kOffSize, state.size);
	WriteValue(command_buffer + kOffData, state.data);
}

static bool HeaderMatches(const CommandBufferState& state, const CommandBufferState& header)
{
	return state.type == header.type && state.write_offset == header.write_offset && state.command_count == header.command_count &&
	       state.size == header.size && state.data == header.data;
}

static void RemoveAliasesLocked(uint64_t command_buffer)
{
	for (auto it = g_buffer_aliases.begin(); it != g_buffer_aliases.end();)
	{
		if (it->first == command_buffer || it->second == command_buffer)
		{
			it = g_buffer_aliases.erase(it);
		} else
		{
			++it;
		}
	}
}

static void RemoveStateLocked(uint64_t command_buffer)
{
	g_buffers.erase(command_buffer);
	RemoveAliasesLocked(command_buffer);
}

static uint64_t ResolveCommandBufferLocked(uint64_t command_buffer)
{
	if (g_buffers.find(command_buffer) != g_buffers.end())
	{
		return command_buffer;
	}
	const auto alias = g_buffer_aliases.find(command_buffer);
	return alias != g_buffer_aliases.end() ? alias->second : command_buffer;
}

static CommandBufferState* GetStateLocked(uint64_t command_buffer, uint64_t* resolved_command_buffer = nullptr)
{
	if (command_buffer == 0)
	{
		return nullptr;
	}
	const uint64_t resolved = ResolveCommandBufferLocked(command_buffer);
	if (resolved_command_buffer != nullptr)
	{
		*resolved_command_buffer = resolved;
	}
	const auto header = ReadHeader(resolved);
	auto       it     = g_buffers.find(resolved);
	if (it == g_buffers.end() || !HeaderMatches(it->second, header))
	{
		RemoveStateLocked(resolved);
		it = g_buffers.emplace(resolved, header).first;
		if (header.data != 0 && header.data != resolved)
		{
			g_buffer_aliases[header.data] = resolved;
		}
	}
	return &it->second;
}

static bool TryGetState(uint64_t command_buffer, CommandBufferState* out)
{
	if (out == nullptr)
	{
		return false;
	}
	Core::LockGuard lock(g_ampr_mutex);
	auto*           state = GetStateLocked(command_buffer);
	if (state == nullptr)
	{
		return false;
	}
	*out = *state;
	return true;
}

static bool IsValidReadRange(uint64_t destination, uint64_t size)
{
	return destination != 0 && size != 0 && size <= kMaxAprReadSize && destination <= kMaxAprAddress &&
	       size <= kMaxAprAddress - destination;
}

static bool IsValidFileOffset(uint64_t file_offset)
{
	return file_offset < kMaxAprFileOffset;
}

static uint64_t ReadRecordSize(uint64_t file_offset)
{
	return file_offset > UINT32_MAX ? kReadFileRecordSizeExtended : kReadFileRecordSize;
}

static bool EnsureStreamSpace(const CommandBufferState& state, uint64_t record_size)
{
	return state.data != 0 && record_size != 0 && record_size <= state.size && state.write_offset <= state.size - record_size;
}

static int AppendRecord(uint64_t command_buffer, const uint8_t* record, uint64_t record_size, const PendingAction* pending = nullptr)
{
	if (command_buffer == 0 || record == nullptr || record_size == 0 || record_size > UINT32_MAX)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	Core::LockGuard lock(g_ampr_mutex);
	uint64_t        resolved = 0;
	auto*           state    = GetStateLocked(command_buffer, &resolved);
	if (state == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	if (state->submitting)
	{
		return LibKernel::KERNEL_ERROR_EBUSY;
	}
	if (!EnsureStreamSpace(*state, record_size) || state->command_count == std::numeric_limits<int32_t>::max())
	{
		return LibKernel::KERNEL_ERROR_EBUSY;
	}

	const uint64_t record_offset = state->write_offset;
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(state->data + record_offset)), record,
	            static_cast<size_t>(record_size));
	if (pending != nullptr)
	{
		auto action          = *pending;
		action.record_offset = record_offset;
		state->actions.push_back(action);
	}
	state->write_offset += static_cast<uint32_t>(record_size);
	state->command_count += 1;
	WriteHeader(resolved, *state);
	return OK;
}

static int AppendEmptyRecord(void* command_buffer, uint64_t record_size)
{
	if (record_size == 0 || record_size > kMaxCommandBufferSize)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::vector<uint8_t> record(static_cast<size_t>(record_size));
	return AppendRecord(reinterpret_cast<uint64_t>(command_buffer), record.data(), record_size);
}

// --- measure APIs ------------------------------------------------------------

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeReadFile(uint64_t, uint64_t destination, uint64_t size, uint64_t file_offset)
{
	PRINT_NAME();
	return IsValidReadRange(destination, size) && IsValidFileOffset(file_offset)
	           ? ReadRecordSize(file_offset)
	           : static_cast<uint32_t>(LibKernel::KERNEL_ERROR_EINVAL);
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

// --- command buffer lifecycle ------------------------------------------------

static KYTY_SYSV_ABI int CommandBufferConstructor(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	if (cmd == 0)
	{
		return OK;
	}
	std::memset(cmd_obj, 0, kHeaderSize);
	Core::LockGuard lock(g_ampr_mutex);
	RemoveStateLocked(cmd);
	return OK;
}

static KYTY_SYSV_ABI int AprCommandBufferConstructor(void* cmd_obj, void* reserved_state0, void* reserved_state1)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	if (cmd == 0)
	{
		return OK;
	}
	const uint64_t state0 = reserved_state0 != nullptr ? reinterpret_cast<uint64_t>(reserved_state0) : cmd + kOffAprMapState;
	const uint64_t state1 = reserved_state1 != nullptr ? reinterpret_cast<uint64_t>(reserved_state1) : cmd + kOffAprGatherScatterState;
	WriteValue(state0, uint64_t {0});
	if (state1 != state0)
	{
		WriteValue(state1, uint64_t {0});
	}
	return OK;
}

static KYTY_SYSV_ABI int CommandBufferDestructor(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 "\n", cmd);
	if (cmd == 0)
	{
		return OK;
	}
	Core::LockGuard lock(g_ampr_mutex);
	RemoveStateLocked(cmd);
	return OK;
}

static KYTY_SYSV_ABI int AprCommandBufferDestructor(void* cmd_obj)
{
	PRINT_NAME();
	(void)cmd_obj;
	return OK;
}

static KYTY_SYSV_ABI int CommandBufferSetBuffer(void* cmd_obj, void* buffer, uint32_t size)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	const uint64_t buf = reinterpret_cast<uint64_t>(buffer);
	KYTY_LOG_DEBUG("\t cmd    = 0x%016" PRIx64 "\n", cmd);
	KYTY_LOG_DEBUG("\t buffer = 0x%016" PRIx64 "\n", buf);
	KYTY_LOG_DEBUG("\t size   = 0x%08" PRIx32 "\n", size);
	if (cmd == 0 || buffer == nullptr || size == 0 || size > kMaxCommandBufferSize || (buf & 3u) != 0 || (size & 3u) != 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (ReadValue<uint64_t>(cmd + kOffData) != 0)
	{
		return LibKernel::KERNEL_ERROR_EBUSY;
	}
	CommandBufferState state = ReadHeader(cmd);
	state.write_offset       = 0;
	state.command_count      = 0;
	state.size               = size;
	state.data               = buf;
	state.actions.clear();
	state.submitting                 = false;
	state.gather_scatter_valid       = false;
	state.gather_scatter_file_id     = 0;
	state.gather_scatter_destination = 0;
	state.gather_scatter_file_offset = 0;
	WriteHeader(cmd, state);
	Core::LockGuard lock(g_ampr_mutex);
	RemoveStateLocked(cmd);
	g_buffers[cmd]       = state;
	g_buffer_aliases[buf] = cmd;
	return OK;
}

static KYTY_SYSV_ABI int CommandBufferReset(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 "\n", cmd);
	if (cmd == 0)
	{
		return LibKernel::KERNEL_ERROR_EPERM;
	}
	Core::LockGuard lock(g_ampr_mutex);
	uint64_t        resolved = 0;
	auto*           state    = GetStateLocked(cmd, &resolved);
	if (state == nullptr || state->data == 0 || state->size == 0)
	{
		return LibKernel::KERNEL_ERROR_EPERM;
	}
	state->write_offset  = 0;
	state->command_count = 0;
	state->actions.clear();
	state->submitting                 = false;
	state->gather_scatter_valid       = false;
	state->gather_scatter_file_id     = 0;
	state->gather_scatter_destination = 0;
	state->gather_scatter_file_offset = 0;
	WriteHeader(resolved, *state);
	return OK;
}

static KYTY_SYSV_ABI void* CommandBufferClearBuffer(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 "\n", cmd);
	if (cmd == 0)
	{
		return nullptr;
	}
	CommandBufferState st {};
	if (!TryGetState(cmd, &st))
	{
		return nullptr;
	}
	const uint64_t old = st.data;
	st.write_offset    = 0;
	st.command_count   = 0;
	st.size            = 0;
	st.data            = 0;
	WriteHeader(cmd, st);
	Core::LockGuard lock(g_ampr_mutex);
	RemoveStateLocked(cmd);
	return reinterpret_cast<void*>(static_cast<uintptr_t>(old));
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
	KYTY_LOG_DEBUG("\t cmd  = 0x%016" PRIx64 " size = 0x%08" PRIx32 "\n", cmd, st.size);
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
	KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " offset = 0x%08" PRIx32 "\n", cmd, st.write_offset);
	return st.write_offset;
}

// sceAmprCommandBufferGetNumCommands (NID gzndltBEzWc). Same ABI as GetSize:
// count in RAX. Report drained/empty so poll-until-zero callers proceed.
static KYTY_SYSV_ABI uint64_t CommandBufferGetNumCommands(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 "\n", cmd);
	CommandBufferState st {};
	if (cmd == 0 || !TryGetState(cmd, &st))
	{
		return 0;
	}
	return st.command_count > 0 ? static_cast<uint64_t>(st.command_count) : 0;
}

static int AppendDeferredRead(uint64_t command_buffer, uint8_t opcode, uint32_t file_id, uint64_t destination, uint64_t size,
                              uint64_t file_offset, uint64_t record_size)
{
	std::array<uint8_t, kReadFileRecordSizeExtended> record {};
	record[0] = opcode;
	PendingAction action {};
	action.kind        = PendingActionKind::ReadFile;
	action.file_id     = file_id;
	action.destination = destination;
	action.size        = size;
	action.file_offset = file_offset;
	const int rc = AppendRecord(command_buffer, record.data(), record_size, &action);
	if (rc == OK)
	{
		Core::LockGuard lock(g_ampr_mutex);
		auto*           state = GetStateLocked(command_buffer);
		if (state != nullptr && destination <= std::numeric_limits<uint64_t>::max() - size &&
		    file_offset <= std::numeric_limits<uint64_t>::max() - size)
		{
			state->gather_scatter_valid       = true;
			state->gather_scatter_file_id     = file_id;
			state->gather_scatter_destination = destination + size;
			state->gather_scatter_file_offset = file_offset + size;
		}
	}
	return rc;
}

static KYTY_SYSV_ABI int AprCommandBufferReadFile(void* cmd_obj, uint64_t /*a1*/, uint64_t /*a2*/, uint32_t file_id, void* dest,
                                                  uint64_t size, uint64_t file_offset)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	KYTY_LOG_DEBUG("\t cmd         = 0x%016" PRIx64 "\n", cmd);
	KYTY_LOG_DEBUG("\t file_id     = 0x%08" PRIx32 "\n", file_id);
	KYTY_LOG_DEBUG("\t dest        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dest));
	KYTY_LOG_DEBUG("\t size        = 0x%016" PRIx64 "\n", size);
	KYTY_LOG_DEBUG("\t file_offset = 0x%016" PRIx64 "\n", file_offset);

	const uint64_t destination = reinterpret_cast<uint64_t>(dest);
	if (cmd == 0 || !IsValidReadRange(destination, size) || !IsValidFileOffset(file_offset))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	return AppendDeferredRead(cmd, 0x17, file_id, destination, size, file_offset, ReadRecordSize(file_offset));
}

// Completion writes execute after all records appended before them.
static KYTY_SYSV_ABI int CommandBufferWriteAddressOnCompletion(void* cmd_obj, uint64_t* address, uint64_t value)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	KYTY_LOG_DEBUG("\t cmd     = 0x%016" PRIx64 "\n", cmd);
	KYTY_LOG_DEBUG("\t address = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
	KYTY_LOG_DEBUG("\t value   = 0x%016" PRIx64 "\n", value);

	if (cmd == 0 || address == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	std::array<uint8_t, kWriteAddressRecordSize> record {};
	WriteValue(reinterpret_cast<uint64_t>(record.data()), kWriteAddressRecordType);
	WriteValue(reinterpret_cast<uint64_t>(record.data()) + 0x08, reinterpret_cast<uint64_t>(address));
	WriteValue(reinterpret_cast<uint64_t>(record.data()) + 0x10, value);
	PendingAction action {};
	action.kind    = PendingActionKind::WriteAddress;
	action.address = reinterpret_cast<uint64_t>(address);
	action.value   = value;
	return AppendRecord(cmd, record.data(), record.size(), &action);
}

// sceAmprCommandBufferWriteKernelEventQueueOnCompletion (o67gODLFpls)
static KYTY_SYSV_ABI int CommandBufferWriteKernelEventQueueOnCompletion(void* cmd_obj, void* equeue, uint64_t ident,
                                                                        uint64_t completion_token, uint64_t user_data)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	KYTY_LOG_DEBUG("\t cmd              = 0x%016" PRIx64 "\n", cmd);
	KYTY_LOG_DEBUG("\t equeue           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(equeue));
	KYTY_LOG_DEBUG("\t ident            = 0x%016" PRIx64 "\n", ident);
	KYTY_LOG_DEBUG("\t completion_token = 0x%016" PRIx64 "\n", completion_token);
	KYTY_LOG_DEBUG("\t user_data        = 0x%016" PRIx64 "\n", user_data);

	if (cmd == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	Kernel::EventQueue::KernelEqueueIdentity identity {};
	if (equeue != nullptr)
	{
		auto equeue_pin = Kernel::EventQueue::KernelAcquireEqueue(static_cast<Kernel::EventQueue::KernelEqueue>(equeue));
		if (!equeue_pin)
		{
			return LibKernel::KERNEL_ERROR_EBADF;
		}
		identity = equeue_pin.GetIdentity();
	}
	std::array<uint8_t, kKernelEventQueueRecordSize> record {};
	WriteValue(reinterpret_cast<uint64_t>(record.data()), uint32_t {2});
	WriteValue(reinterpret_cast<uint64_t>(record.data()) + 0x08, reinterpret_cast<uint64_t>(equeue));
	WriteValue(reinterpret_cast<uint64_t>(record.data()) + 0x10, static_cast<int32_t>(ident));
	WriteValue(reinterpret_cast<uint64_t>(record.data()) + 0x18, completion_token);
	PendingAction action {};
	action.kind             = PendingActionKind::KernelEvent;
	action.equeue_identity  = identity;
	action.ident            = static_cast<uintptr_t>(ident);
	action.completion_token = static_cast<uintptr_t>(completion_token);
	(void)user_data;
	return AppendRecord(cmd, record.data(), record.size(), &action);
}

static int ExecuteRead(const PendingAction& action)
{
	Core::String host_path;
	if (!Kernel::FileSystem::AprTryGetHostPath(action.file_id, &host_path))
	{
		return LibKernel::KERNEL_ERROR_ENOENT;
	}
	Core::File file;
	if (!file.Open(host_path, Core::File::Mode::Read))
	{
		return LibKernel::KERNEL_ERROR_ENOENT;
	}
	if (!file.Seek(action.file_offset))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	uint64_t total = 0;
	auto*    dest  = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(action.destination));
	while (total < action.size)
	{
		const auto request = static_cast<uint32_t>(
		    action.size - total > kReadChunkSize ? kReadChunkSize : action.size - total);
		uint32_t read = 0;
		file.Read(dest + total, request, &read);
		if (read == 0)
		{
			break;
		}
		total += read;
	}
	if (total != 0)
	{
		Emulator::VideoFrameMemory::NotifyHostWrite(action.destination, total);
	}
	return OK;
}

static int ExecuteAction(const PendingAction& action, uintptr_t submit_ident)
{
	switch (action.kind)
	{
		case PendingActionKind::ReadFile: return ExecuteRead(action);
		case PendingActionKind::WriteAddress:
			std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(action.address)), &action.value, sizeof(action.value));
			std::atomic_thread_fence(std::memory_order_release);
			Emulator::VideoFrameMemory::NotifyHostWrite(action.address, sizeof(action.value));
			return OK;
		case PendingActionKind::KernelEvent:
		{
			if (!action.equeue_identity)
			{
				return OK;
			}
			const uintptr_t ident = action.ident != 0 ? action.ident : submit_ident;
			if (ident == 0)
			{
				return LibKernel::KERNEL_ERROR_EINVAL;
			}
			auto equeue_pin = Kernel::EventQueue::KernelAcquireEqueue(action.equeue_identity);
			if (!equeue_pin)
			{
				return LibKernel::KERNEL_ERROR_EBADF;
			}
			return Kernel::EventQueue::KernelTriggerEvent(equeue_pin, ident, Kernel::EventQueue::KERNEL_EVFILT_AMPR,
			                                                    reinterpret_cast<void*>(action.completion_token));
		}
	}
	return LibKernel::KERNEL_ERROR_EINVAL;
}

int SubmitCommandBuffer(void* cmd_obj, uintptr_t submit_ident)
{
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	if (cmd == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	CommandBufferState pending {};
	uint64_t           resolved = 0;
	{
		Core::LockGuard lock(g_ampr_mutex);
		auto*           state = GetStateLocked(cmd, &resolved);
		if (state == nullptr)
		{
			return LibKernel::KERNEL_ERROR_EFAULT;
		}
		if (state->submitting)
		{
			return LibKernel::KERNEL_ERROR_EBUSY;
		}
		state->submitting = true;
		pending           = *state;
	}

	for (const auto& action: pending.actions)
	{
		const int rc = ExecuteAction(action, submit_ident);
		if (rc != OK)
		{
			Core::LockGuard lock(g_ampr_mutex);
			auto            it = g_buffers.find(resolved);
			if (it != g_buffers.end())
			{
				it->second.submitting = false;
			}
			return rc;
		}
	}

	Core::LockGuard lock(g_ampr_mutex);
	auto            it = g_buffers.find(resolved);
	if (it != g_buffers.end())
	{
		auto& state                      = it->second;
		state.write_offset               = 0;
		state.command_count              = 0;
		state.actions.clear();
		state.submitting                 = false;
		state.gather_scatter_valid       = false;
		state.gather_scatter_file_id     = 0;
		state.gather_scatter_destination = 0;
		state.gather_scatter_file_offset = 0;
		WriteHeader(resolved, state);
	}
	return OK;
}

// --- Gen5 measure / marker / nop helpers --------------------------------------

constexpr uint64_t kMeasureFixed32Size = 0x20;
constexpr uint64_t kPopMarkerSize      = 0x4;

static uint64_t AlignUp4(uint64_t n)
{
	return (n + 3u) & ~uint64_t {3};
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeFixed32(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
	PRINT_NAME();
	return kMeasureFixed32Size;
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeNop(uint32_t num_u32)
{
	PRINT_NAME();
	return num_u32 == 0 ? sizeof(uint32_t) : AlignUp4(static_cast<uint64_t>(num_u32) * sizeof(uint32_t));
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeNopWithData(uint32_t num_u32, const uint32_t*)
{
	PRINT_NAME();
	return AlignUp4((static_cast<uint64_t>(num_u32) + 1u) * sizeof(uint32_t));
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeMarker(const char* msg)
{
	PRINT_NAME();
	const uint64_t len = (msg != nullptr) ? std::strlen(msg) + 1u : 1u;
	return AlignUp4(0x04u + len);
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeMarkerWithColor(const char* msg, uint32_t)
{
	PRINT_NAME();
	const uint64_t len = (msg != nullptr) ? std::strlen(msg) + 1u : 1u;
	return AlignUp4(0x08u + len);
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizePopMarker()
{
	PRINT_NAME();
	return kPopMarkerSize;
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeReadFileScatter(uint64_t destination, uint64_t size)
{
	PRINT_NAME();
	return IsValidReadRange(destination, size) ? kReadScatterRecordSize : static_cast<uint32_t>(LibKernel::KERNEL_ERROR_EINVAL);
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeReadFileGatherScatter(uint64_t destination, uint64_t size, uint64_t file_offset)
{
	PRINT_NAME();
	if (!IsValidReadRange(destination, size) || !IsValidFileOffset(file_offset))
	{
		return static_cast<uint32_t>(LibKernel::KERNEL_ERROR_EINVAL);
	}
	return file_offset > UINT32_MAX ? kReadGatherScatterExtended : kReadGatherScatterRecordSize;
}

static KYTY_SYSV_ABI uint64_t MeasureCommandSizeReadFileGather(uint64_t size, uint64_t file_offset)
{
	PRINT_NAME();
	if (size == 0 || size > kMaxAprReadSize || !IsValidFileOffset(file_offset))
	{
		return static_cast<uint32_t>(LibKernel::KERNEL_ERROR_EINVAL);
	}
	return file_offset > 0x3ffffu ? kReadGatherRecordSizeExtended : kReadGatherRecordSize;
}

static KYTY_SYSV_ABI int64_t MeasureAprCommandSizeMapBegin(uint64_t, uint64_t, int32_t, int32_t)
{
	PRINT_NAME();
	return static_cast<int64_t>(kAprMapBeginRecordSize);
}

static KYTY_SYSV_ABI int64_t MeasureAprCommandSizeMapDirectBegin(uint64_t, uint64_t, uint64_t, int32_t, int32_t)
{
	PRINT_NAME();
	return static_cast<int64_t>(kAprMapDirectBeginRecordSize);
}

static KYTY_SYSV_ABI int CommandBufferMarkerNoOp(void* cmd_obj)
{
	PRINT_NAME();
	if (reinterpret_cast<uint64_t>(cmd_obj) == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	return OK;
}

static KYTY_SYSV_ABI int CommandBufferMarkerNoOp2(void* cmd_obj, uint64_t)
{
	return CommandBufferMarkerNoOp(cmd_obj);
}

static KYTY_SYSV_ABI int CommandBufferMarkerNoOp3(void* cmd_obj, uint64_t, uint64_t)
{
	return CommandBufferMarkerNoOp(cmd_obj);
}

static KYTY_SYSV_ABI uint64_t CommandBufferGetBufferBaseAddress(void* cmd_obj)
{
	PRINT_NAME();
	CommandBufferState st {};
	if (!TryGetState(reinterpret_cast<uint64_t>(cmd_obj), &st))
	{
		return 0;
	}
	return st.data;
}

static KYTY_SYSV_ABI uint32_t CommandBufferGetType(void* cmd_obj)
{
	PRINT_NAME();
	CommandBufferState state {};
	return TryGetState(reinterpret_cast<uint64_t>(cmd_obj), &state) ? state.type : 0;
}

static KYTY_SYSV_ABI int AprCommandBufferMapBegin(void* cmd_obj, uint64_t, uint64_t, int32_t, int32_t)
{
	return AppendEmptyRecord(cmd_obj, kAprMapBeginRecordSize);
}

static KYTY_SYSV_ABI int AprCommandBufferMapDirectBegin(void* cmd_obj, uint64_t, uint64_t, uint64_t, int32_t, int32_t)
{
	return AppendEmptyRecord(cmd_obj, kAprMapDirectBeginRecordSize);
}

static KYTY_SYSV_ABI int AprCommandBufferMapEnd(void* cmd_obj)
{
	return AppendEmptyRecord(cmd_obj, kAprMapEndRecordSize);
}

static KYTY_SYSV_ABI int AprCommandBufferReadFileGather(void* cmd_obj, uint64_t, uint64_t, uint64_t size, uint64_t file_offset)
{
	CommandBufferState state {};
	if (!TryGetState(reinterpret_cast<uint64_t>(cmd_obj), &state) || !state.gather_scatter_valid || size == 0 ||
	    size > kMaxAprReadSize || !IsValidFileOffset(file_offset) || !IsValidReadRange(state.gather_scatter_destination, size))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	const uint64_t record_size = file_offset > 0x3ffffu ? kReadGatherRecordSizeExtended : kReadGatherRecordSize;
	return AppendDeferredRead(reinterpret_cast<uint64_t>(cmd_obj), 0x18, state.gather_scatter_file_id,
	                          state.gather_scatter_destination, size, file_offset, record_size);
}

static KYTY_SYSV_ABI int AprCommandBufferReadFileScatter(void* cmd_obj, uint64_t, uint64_t, void* destination, uint64_t size)
{
	CommandBufferState state {};
	const uint64_t     dest = reinterpret_cast<uint64_t>(destination);
	if (!TryGetState(reinterpret_cast<uint64_t>(cmd_obj), &state) || !state.gather_scatter_valid ||
	    !IsValidReadRange(dest, size) || !IsValidFileOffset(state.gather_scatter_file_offset))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	return AppendDeferredRead(reinterpret_cast<uint64_t>(cmd_obj), 0x19, state.gather_scatter_file_id, dest, size,
	                          state.gather_scatter_file_offset, kReadScatterRecordSize);
}

static KYTY_SYSV_ABI int AprCommandBufferReadFileGatherScatter(void* cmd_obj, uint64_t, uint64_t, void* destination, uint64_t size,
                                                               uint64_t file_offset)
{
	CommandBufferState state {};
	const uint64_t     dest = reinterpret_cast<uint64_t>(destination);
	if (!TryGetState(reinterpret_cast<uint64_t>(cmd_obj), &state) || !state.gather_scatter_valid ||
	    !IsValidReadRange(dest, size) || !IsValidFileOffset(file_offset))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	const uint64_t record_size = file_offset > UINT32_MAX ? kReadGatherScatterExtended : kReadGatherScatterRecordSize;
	return AppendDeferredRead(reinterpret_cast<uint64_t>(cmd_obj), 0x1a, state.gather_scatter_file_id, dest, size, file_offset,
	                          record_size);
}

static KYTY_SYSV_ABI int AprCommandBufferResetGatherScatterState(void* cmd_obj, uint64_t, uint64_t)
{
	const int rc = AppendEmptyRecord(cmd_obj, kResetGatherScatterRecordSize);
	if (rc != OK)
	{
		return rc;
	}
	Core::LockGuard lock(g_ampr_mutex);
	auto*           state = GetStateLocked(reinterpret_cast<uint64_t>(cmd_obj));
	if (state != nullptr)
	{
		state->gather_scatter_valid       = false;
		state->gather_scatter_file_id     = 0;
		state->gather_scatter_destination = 0;
		state->gather_scatter_file_offset = 0;
	}
	return OK;
}

// Submission is synchronous, while command effects remain ordered at the submit boundary.
static KYTY_SYSV_ABI int AmmSubmitCommandBuffer(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 "\n", cmd);
	if (cmd == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	return SubmitCommandBuffer(cmd_obj, 0);
}

static KYTY_SYSV_ABI int AmmSubmitCommandBuffer2(void* cmd_obj, uint64_t /*arg1*/)
{
	return AmmSubmitCommandBuffer(cmd_obj);
}

static KYTY_SYSV_ABI int AmmWaitCommandBufferCompletion(void* cmd_obj)
{
	PRINT_NAME();
	const uint64_t cmd = reinterpret_cast<uint64_t>(cmd_obj);
	KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 "\n", cmd);
	if (cmd == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	return OK;
}

} // namespace Ampr

LIB_DEFINE(InitAmpr_1)
{
	::Kyty::Kernel::AmprPort::Install(&Ampr::SubmitCommandBuffer);

	// Measure APIs — NIDs from libSceAmpr stubs.
	LIB_FUNC("vWU-odnS+fU", Ampr::MeasureCommandSizeReadFile);
	LIB_FUNC("sSAUCCU1dv4", Ampr::MeasureCommandSizeWriteKernelEventQueue0400);
	LIB_FUNC("C+IEj+BsAFM", Ampr::MeasureCommandSizeWriteAddressOnCompletion);
	LIB_FUNC("Zi3dBUjgyXI", Ampr::MeasureCommandSizeWriteKernelEventQueueOnCompletion);
	LIB_FUNC("4muPEJ-x5N8", Ampr::MeasureCommandSizeWriteCounterOnCompletion);
	LIB_FUNC("qesF88X4DRg", Ampr::MeasureCommandSizeReadFileGather);
	LIB_FUNC("7nXGDGMXSqo", Ampr::MeasureCommandSizeReadFileScatter);
	LIB_FUNC("DXmgc5op8Yw", Ampr::MeasureCommandSizeReadFileGatherScatter);
	LIB_FUNC("0BMj1hgG+kE", Ampr::MeasureCommandSizeFixed32);
	LIB_FUNC("ClnsFLLLcss", Ampr::MeasureCommandSizeFixed32);
	LIB_FUNC("4fgtGfXDrFc", Ampr::MeasureCommandSizeFixed32);
	LIB_FUNC("gAtc79UTt5E", Ampr::MeasureCommandSizeFixed32);
	LIB_FUNC("JYd9g9L+TmE", Ampr::MeasureCommandSizeFixed32);
	LIB_FUNC("2Hw8gjMdwSY", Ampr::MeasureCommandSizeFixed32);
	LIB_FUNC("I-Qm+MEso5c", Ampr::MeasureCommandSizeFixed32);
	LIB_FUNC("NNIZ-FMyz3M", Ampr::MeasureCommandSizeNop);
	LIB_FUNC("Xp85BP3+BBI", Ampr::MeasureCommandSizeNopWithData);
	LIB_FUNC("VGkEj4d6-Kg", Ampr::MeasureCommandSizeMarker);
	LIB_FUNC("0RdLmAh7WVo", Ampr::MeasureCommandSizeMarker);
	LIB_FUNC("tmfr97+ED5I", Ampr::MeasureCommandSizeMarkerWithColor);
	LIB_FUNC("3OfeY4pzDV0", Ampr::MeasureCommandSizeMarkerWithColor);
	LIB_FUNC("iwTNhyaemnw", Ampr::MeasureCommandSizePopMarker);
	LIB_FUNC("pbnNnahE8vk", Ampr::MeasureCommandSizePopMarker);
	LIB_FUNC("rddQYXM0CjM", Ampr::MeasureCommandSizePopMarker);
	LIB_FUNC("kdFImtTD0hc", Ampr::MeasureAprCommandSizeMapBegin);
	LIB_FUNC("qvbdJc7bG+s", Ampr::MeasureAprCommandSizeMapDirectBegin);

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
	LIB_FUNC("RPCAhx-aabE", Ampr::CommandBufferGetBufferBaseAddress);
	LIB_FUNC("VEDMaQmJZng", Ampr::CommandBufferGetType);
	LIB_FUNC("tNn5WBkta60", Ampr::CommandBufferMarkerNoOp);
	LIB_FUNC("GmOguNIsuKk", Ampr::CommandBufferMarkerNoOp);
	LIB_FUNC("pFQ9UHpO52s", Ampr::CommandBufferMarkerNoOp2);
	LIB_FUNC("4UkZbYKVF7c", Ampr::CommandBufferMarkerNoOp2);
	LIB_FUNC("sWbST0oQKsc", Ampr::CommandBufferMarkerNoOp3);
	LIB_FUNC("4quckD2y7Pg", Ampr::CommandBufferMarkerNoOp2);
	LIB_FUNC("f12ObAMEi9A", Ampr::CommandBufferMarkerNoOp3);
	LIB_FUNC("dXPaz65HNmk", Ampr::CommandBufferMarkerNoOp2);
	LIB_FUNC("mv0O8Zg0woU", Ampr::CommandBufferMarkerNoOp);
	LIB_FUNC("DLfoNxTFNVk", Ampr::CommandBufferMarkerNoOp3);
	LIB_FUNC("cQb8Zr8Q0Y0", Ampr::CommandBufferMarkerNoOp2);
	LIB_FUNC("j0+3uJMxYJY", Ampr::CommandBufferMarkerNoOp3);
	LIB_FUNC("jK+yuYCI7MA", Ampr::CommandBufferMarkerNoOp3);
	LIB_FUNC("bt3LHR9xjK4", Ampr::CommandBufferMarkerNoOp2);
	LIB_FUNC("enZm-6GjWqw", Ampr::CommandBufferMarkerNoOp3);
	LIB_FUNC("t4ExS+SwLjs", Ampr::CommandBufferMarkerNoOp3);
	LIB_FUNC("H896Pt-yB4I", Ampr::CommandBufferMarkerNoOp3);
	LIB_FUNC("BVmR1H8l+XI", Ampr::AprCommandBufferReadFileGatherScatter);

	// APR / completion builders
	LIB_FUNC("mQ16-QdKv7k", Ampr::AprCommandBufferReadFile);
	LIB_FUNC("mZSbNJVJpV8", Ampr::AprCommandBufferReadFileGather);
	LIB_FUNC("Jg-AgkdJHkk", Ampr::AprCommandBufferReadFileScatter);
	LIB_FUNC("YPxkUDhgoNI", Ampr::AprCommandBufferResetGatherScatterState);
	LIB_FUNC("Eul7AGEpjLo", Ampr::AprCommandBufferMapBegin);
	LIB_FUNC("bFEs0Gs6D2A", Ampr::AprCommandBufferMapDirectBegin);
	LIB_FUNC("X169CE6G3Y4", Ampr::AprCommandBufferMapEnd);
	LIB_FUNC("sJXyWHjP-F8", Ampr::CommandBufferWriteAddressOnCompletion);
	LIB_FUNC("o67gODLFpls", Ampr::CommandBufferWriteKernelEventQueueOnCompletion);

	// AMM submit/wait (NID map: lwS-7y3jcBI / OJf3vCckPAM / HXymib4T8gc)
	LIB_FUNC("lwS-7y3jcBI", Ampr::AmmSubmitCommandBuffer);
	LIB_FUNC("OJf3vCckPAM", Ampr::AmmSubmitCommandBuffer2);
	LIB_FUNC("HXymib4T8gc", Ampr::AmmWaitCommandBufferCompletion);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
