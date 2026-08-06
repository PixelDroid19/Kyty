#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"
#include "Emulator/Loader/SystemContent.h"

#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("PlayGo", 1, "PlayGo", 1, 0);

namespace PlayGo {

static uint32_t g_chunks_num = 0;

struct PlayGoInitParams
{
	const void* buf_addr;
	uint32_t    buf_size;
	uint32_t    reserved;
};

// Optional-chunk queries (scePlayGo*OptionalChunk) select a bitfield family.
// type 0 = language mask, 1 = scenario mask. Values match the guest ABI.
static constexpr int32_t  PLAYGO_OPTIONAL_TYPE_LANGUAGE = 0;
static constexpr int32_t  PLAYGO_OPTIONAL_TYPE_SCENARIO = 1;
static constexpr uint64_t PLAYGO_LANGUAGE_MASK_ALL      = 0xffffffffffffffffull;
static constexpr uint64_t PLAYGO_SCENARIO_MASK_ALL      = 0x1full;

union PlayGoOptionalChunk
{
	uint64_t bitmask;
	uint64_t languages;
	uint64_t scenarios;
};

static int PlayGoValidateHandle(int handle)
{
	return (handle == 1 ? OK : PLAYGO_ERROR_BAD_HANDLE);
}

static int PlayGoValidateOptionalType(int32_t type)
{
	return (type == PLAYGO_OPTIONAL_TYPE_LANGUAGE || type == PLAYGO_OPTIONAL_TYPE_SCENARIO
	            ? OK
	            : PLAYGO_ERROR_INVALID_ARGUMENT);
}

static void PlayGoSetOptionalChunk(int32_t type, PlayGoOptionalChunk* option)
{
	EXIT_IF(option == nullptr);

	option->bitmask = (type == PLAYGO_OPTIONAL_TYPE_LANGUAGE ? PLAYGO_LANGUAGE_MASK_ALL
	                                                         : PLAYGO_SCENARIO_MASK_ALL);
}

int KYTY_SYSV_ABI PlayGoInitialize(const PlayGoInitParams* init)
{
	PRINT_NAME();

	if (init == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t buf_addr = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(init->buf_addr));
	KYTY_LOG_DEBUG("\t buf_size = %" PRIu32 "\n", init->buf_size);
	KYTY_LOG_DEBUG("\t reserved = %" PRId32 "\n", init->reserved);

	return OK;
}

int KYTY_SYSV_ABI PlayGoTerminate()
{
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI PlayGoOpen(int* out_handle, const void* param)
{
	PRINT_NAME();

	if (out_handle == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (param != nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	*out_handle = 1;

	if (!Loader::SystemContentGetChunksNum(&g_chunks_num))
	{
		KYTY_LOG_DEBUG("Warning: assume that chunks count is 1\n");
		g_chunks_num = 1;
	}

	return OK;
}

int KYTY_SYSV_ABI PlayGoClose(int handle)
{
	PRINT_NAME();

	if (handle != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	return OK;
}

int KYTY_SYSV_ABI PlayGoGetLocus(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries, int8_t* out_loci)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t handle = %d\n", handle);

	if (handle != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (chunk_ids == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (out_loci == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	// EXIT_NOT_IMPLEMENTED(number_of_entries != 1);
	if (g_chunks_num == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	for (uint32_t i = 0; i < number_of_entries; i++)
	{
		KYTY_LOG_DEBUG("\t chunk_ids[%u] = %" PRIu16 "\n", i, chunk_ids[i]);

		if (chunk_ids[i] <= g_chunks_num)
		{
			out_loci[i] = 3;
		} else
		{
			return PLAYGO_ERROR_BAD_CHUNK_ID;
		}
	}

	return OK;
}

// scePlayGoSetInstallSpeed — NID 4AAcTU9R3XM. speed: 0..2.
static int KYTY_SYSV_ABI PlayGoSetInstallSpeed(int handle, int32_t speed)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = %d\n", handle);
	KYTY_LOG_DEBUG("\t speed  = %" PRId32 "\n", speed);
	if (handle != 1)
	{
		return PLAYGO_ERROR_BAD_HANDLE;
	}
	if (speed < 0 || speed > 2)
	{
		return PLAYGO_ERROR_BAD_SPEED;
	}
	return OK;
}

// scePlayGoGetChunkId — NID 73fF1MFU8hA
// (handle, out_chunk_id_list, number_of_entries, out_entries)
static int KYTY_SYSV_ABI PlayGoGetChunkId(int handle, uint16_t* out_chunk_id_list,
                                          uint32_t number_of_entries, uint32_t* out_entries)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle            = %d\n", handle);
	KYTY_LOG_DEBUG("\t out_chunk_id_list = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(out_chunk_id_list));
	KYTY_LOG_DEBUG("\t number_of_entries = %" PRIu32 "\n", number_of_entries);
	KYTY_LOG_DEBUG("\t out_entries       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(out_entries));
	if (handle != 1)
	{
		return PLAYGO_ERROR_BAD_HANDLE;
	}
	if (out_entries == nullptr)
	{
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (number_of_entries != 0 && out_chunk_id_list == nullptr)
	{
		return PLAYGO_ERROR_BAD_POINTER;
	}
	const uint32_t total   = (g_chunks_num != 0 ? g_chunks_num : 1u);
	const uint32_t entries = (number_of_entries < total ? number_of_entries : total);
	for (uint32_t i = 0; i < entries; i++)
	{
		out_chunk_id_list[i] = static_cast<uint16_t>(i);
	}
	*out_entries = entries;
	return OK;
}

static int KYTY_SYSV_ABI PlayGoGetProgress(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries, void* out_progress)
{
	PRINT_NAME();
	if (handle != 1)
	{
		return PLAYGO_ERROR_BAD_HANDLE;
	}
	if (chunk_ids == nullptr || out_progress == nullptr)
	{
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (number_of_entries == 0)
	{
		return PLAYGO_ERROR_BAD_SIZE;
	}
	std::memset(out_progress, 0, 16);
	return OK;
}

static int KYTY_SYSV_ABI PlayGoPrefetch(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries, uint32_t /*offset*/, uint32_t /*size*/)
{
	PRINT_NAME();
	if (handle != 1)
	{
		return PLAYGO_ERROR_BAD_HANDLE;
	}
	if (chunk_ids == nullptr || number_of_entries == 0)
	{
		return PLAYGO_ERROR_BAD_POINTER;
	}
	return OK;
}

static int KYTY_SYSV_ABI PlayGoGetLanguageMask(int handle, uint64_t* mask)
{
	PRINT_NAME();
	if (handle != 1)
	{
		return PLAYGO_ERROR_BAD_HANDLE;
	}
	if (mask == nullptr)
	{
		return PLAYGO_ERROR_BAD_POINTER;
	}
	*mask = 0;
	return OK;
}

static int KYTY_SYSV_ABI PlayGoSetLanguageMask(int handle, uint64_t mask)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = %d\n", handle);
	KYTY_LOG_DEBUG("\t mask   = %" PRIu64 "\n", mask);
	return (handle == 1 ? OK : PLAYGO_ERROR_BAD_HANDLE);
}

static int KYTY_SYSV_ABI PlayGoGetToDoList(int handle, void* out_list, uint32_t number_of_entries, uint32_t* out_entries)
{
	PRINT_NAME();
	if (handle != 1)
	{
		return PLAYGO_ERROR_BAD_HANDLE;
	}
	if (out_list == nullptr || out_entries == nullptr)
	{
		return PLAYGO_ERROR_BAD_POINTER;
	}
	*out_entries = 0;
	return OK;
}

static int KYTY_SYSV_ABI PlayGoSetToDoList(int handle, const void* list, uint32_t number_of_entries)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle            = %d\n", handle);
	KYTY_LOG_DEBUG("\t number_of_entries = %" PRIu32 "\n", number_of_entries);
	(void)list;
	return (handle == 1 ? OK : PLAYGO_ERROR_BAD_HANDLE);
}

static int KYTY_SYSV_ABI PlayGoGetInstallSpeed(int handle, int32_t* speed)
{
	PRINT_NAME();
	if (handle != 1)
	{
		return PLAYGO_ERROR_BAD_HANDLE;
	}
	if (speed == nullptr)
	{
		return PLAYGO_ERROR_BAD_POINTER;
	}
	*speed = 2;
	return OK;
}

static int KYTY_SYSV_ABI PlayGoGetEta(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries, uint32_t* out_eta)
{
	PRINT_NAME();
	if (handle != 1)
	{
		return PLAYGO_ERROR_BAD_HANDLE;
	}
	if (chunk_ids == nullptr || out_eta == nullptr || number_of_entries == 0)
	{
		return PLAYGO_ERROR_BAD_POINTER;
	}
	*out_eta = 0;
	return OK;
}

// scePlayGoGetOptionalChunk — NID g4AZyxpSAlA
static int KYTY_SYSV_ABI PlayGoGetOptionalChunk(int handle, int32_t type, PlayGoOptionalChunk* option)
{
	PRINT_NAME();

	if (auto ret = PlayGoValidateHandle(handle); ret != OK)
	{
		return ret;
	}
	if (option == nullptr)
	{
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (auto ret = PlayGoValidateOptionalType(type); ret != OK)
	{
		return ret;
	}

	PlayGoSetOptionalChunk(type, option);

	return OK;
}

// scePlayGoPrefetchOptionalChunk — NID HVAa744ecdw
static int KYTY_SYSV_ABI PlayGoPrefetchOptionalChunk(int handle, int32_t type, const PlayGoOptionalChunk* option)
{
	PRINT_NAME();

	if (auto ret = PlayGoValidateHandle(handle); ret != OK)
	{
		return ret;
	}
	if (option == nullptr)
	{
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (auto ret = PlayGoValidateOptionalType(type); ret != OK)
	{
		return ret;
	}

	return OK;
}

// scePlayGoGetInstallChunkId — NID 8-e7E989rCU
// (handle, out_chunk_id_list, number_of_entries, out_entries)
static int KYTY_SYSV_ABI PlayGoGetInstallChunkId(int handle, uint16_t* out_chunk_id_list,
                                                 uint32_t number_of_entries, uint32_t* out_entries)
{
	PRINT_NAME();

	return PlayGoGetChunkId(handle, out_chunk_id_list, number_of_entries, out_entries);
}

// scePlayGoGetSupportedOptionalChunk — NID IfiN+-oeVWI
static int KYTY_SYSV_ABI PlayGoGetSupportedOptionalChunk(int handle, int32_t type, PlayGoOptionalChunk* option)
{
	PRINT_NAME();

	return PlayGoGetOptionalChunk(handle, type, option);
}

} // namespace PlayGo

LIB_DEFINE(InitPlayGo_1)
{
	LIB_FUNC("ts6GlZOKRrE", PlayGo::PlayGoInitialize);
	LIB_FUNC("MPe0EeBGM-E", PlayGo::PlayGoTerminate);
	LIB_FUNC("M1Gma1ocrGE", PlayGo::PlayGoOpen);
	LIB_FUNC("Uco1I0dlDi8", PlayGo::PlayGoClose);
	LIB_FUNC("uWIYLFkkwqk", PlayGo::PlayGoGetLocus);
	LIB_FUNC("4AAcTU9R3XM", PlayGo::PlayGoSetInstallSpeed);
	LIB_FUNC("73fF1MFU8hA", PlayGo::PlayGoGetChunkId);
	LIB_FUNC("-RJWNMK3fC8", PlayGo::PlayGoGetProgress);
	LIB_FUNC("-Q1-u1a7p0g", PlayGo::PlayGoPrefetch);
	LIB_FUNC("3OMbYZBaa50", PlayGo::PlayGoGetLanguageMask);
	LIB_FUNC("LosLlHOpNqQ", PlayGo::PlayGoSetLanguageMask);
	LIB_FUNC("Nn7zKwnA5q0", PlayGo::PlayGoGetToDoList);
	LIB_FUNC("gUPGiOQ1tmQ", PlayGo::PlayGoSetToDoList);
	LIB_FUNC("rvBSfTimejE", PlayGo::PlayGoGetInstallSpeed);
	LIB_FUNC("v6EZ-YWRdMs", PlayGo::PlayGoGetEta);
	LIB_FUNC("g4AZyxpSAlA", PlayGo::PlayGoGetOptionalChunk);
	LIB_FUNC("HVAa744ecdw", PlayGo::PlayGoPrefetchOptionalChunk);
	LIB_FUNC("8-e7E989rCU", PlayGo::PlayGoGetInstallChunkId);
	LIB_FUNC("IfiN+-oeVWI", PlayGo::PlayGoGetSupportedOptionalChunk);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
