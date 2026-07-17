#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Loader/SystemContent.h"

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

int KYTY_SYSV_ABI PlayGoInitialize(const PlayGoInitParams* init)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(init == nullptr);

	printf("\t buf_addr = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(init->buf_addr));
	printf("\t buf_size = %" PRIu32 "\n", init->buf_size);
	printf("\t reserved = %" PRId32 "\n", init->reserved);

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

	EXIT_NOT_IMPLEMENTED(out_handle == nullptr);
	EXIT_NOT_IMPLEMENTED(param != nullptr);

	*out_handle = 1;

	if (!Loader::SystemContentGetChunksNum(&g_chunks_num))
	{
		printf("Warning: assume that chunks count is 1\n");
		g_chunks_num = 1;
	}

	return OK;
}

int KYTY_SYSV_ABI PlayGoClose(int handle)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(handle != 1);

	return OK;
}

int KYTY_SYSV_ABI PlayGoGetLocus(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries, int8_t* out_loci)
{
	PRINT_NAME();

	printf("\t handle = %d\n", handle);

	EXIT_NOT_IMPLEMENTED(handle != 1);
	EXIT_NOT_IMPLEMENTED(chunk_ids == nullptr);
	EXIT_NOT_IMPLEMENTED(out_loci == nullptr);
	// EXIT_NOT_IMPLEMENTED(number_of_entries != 1);
	EXIT_NOT_IMPLEMENTED(g_chunks_num == 0);

	for (uint32_t i = 0; i < number_of_entries; i++)
	{
		printf("\t chunk_ids[%u] = %" PRIu16 "\n", i, chunk_ids[i]);

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
	printf("\t handle = %d\n", handle);
	printf("\t speed  = %" PRId32 "\n", speed);
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
static int KYTY_SYSV_ABI PlayGoGetChunkId(int handle, uint16_t* out_chunk_id_list,
                                          uint32_t number_of_entries, uint32_t* out_entries)
{
	PRINT_NAME();
	printf("\t handle            = %d\n", handle);
	printf("\t out_chunk_id_list = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(out_chunk_id_list));
	printf("\t number_of_entries = %" PRIu32 "\n", number_of_entries);
	printf("\t out_entries       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(out_entries));
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
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
