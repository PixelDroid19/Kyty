#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Font", 1, "Font", 1, 1);

namespace Font {

// Guest SceFontMemory descriptor filled by sceFontMemoryInit. Layout matches
// the public Prospero font memory handle used by CreateLibrary / renderer paths.
struct FontMemory
{
	uint16_t type;
	uint16_t attr;
	uint32_t size;
	void*    address;
	void*    mspace_object;
	const void* mem_interface;
	void*    destroy_callback;
	void*    destroy_object;
	void*    user_object;
	void*    parent_object;
};

// sceFontMemoryInit — NID whrS4oksXc4
// (font_memory, address, size_byte, mem_interface, mspace_object,
//  destroy_callback, destroy_object). Observed size 0x800000 with mspace from
// sceLibcMspaceCreate. Fills the descriptor only; allocation is guest-owned.
static int KYTY_SYSV_ABI FontMemoryInit(FontMemory* font_memory, void* address, uint32_t size_byte,
                                        const void* mem_interface, void* mspace_object,
                                        void* destroy_callback, void* destroy_object)
{
	PRINT_NAME();
	printf("\t font_memory      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_memory));
	printf("\t address          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
	printf("\t size_byte        = 0x%" PRIx32 "\n", size_byte);
	printf("\t mem_interface    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(mem_interface));
	printf("\t mspace_object    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(mspace_object));
	printf("\t destroy_callback = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(destroy_callback));
	printf("\t destroy_object   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(destroy_object));

	if (font_memory == nullptr)
	{
		return -1;
	}

	std::memset(font_memory, 0, sizeof(FontMemory));
	font_memory->type             = 1;
	font_memory->attr             = 0;
	font_memory->size             = size_byte;
	font_memory->address          = address;
	font_memory->mspace_object    = mspace_object;
	font_memory->mem_interface    = mem_interface;
	font_memory->destroy_callback = destroy_callback;
	font_memory->destroy_object   = destroy_object;
	return OK;
}

// sceFontMemoryTerm — NID h6hIgxXEiEc
static int KYTY_SYSV_ABI FontMemoryTerm(FontMemory* font_memory)
{
	PRINT_NAME();
	if (font_memory != nullptr)
	{
		font_memory->type = 0;
	}
	return OK;
}

struct LibraryState
{
	const FontMemory* memory;
	void*             selection;
	uint64_t          edition;
};

// sceFontCreateLibraryWithEdition — NID n590hj5Oe-k
// (memory, selection, edition, library_out)
static int KYTY_SYSV_ABI FontCreateLibraryWithEdition(const FontMemory* memory, void* selection,
                                                      uint64_t edition, void** library)
{
	PRINT_NAME();
	printf("\t memory    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(memory));
	printf("\t selection = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(selection));
	printf("\t edition   = 0x%016" PRIx64 "\n", edition);
	printf("\t library   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	if (library == nullptr)
	{
		return -1;
	}
	auto* state     = new LibraryState {};
	state->memory   = memory;
	state->selection = selection;
	state->edition  = edition;
	*library        = state;
	return OK;
}

// sceFontCreateLibrary — NID nWrfPI4Okmg
static int KYTY_SYSV_ABI FontCreateLibrary(const FontMemory* memory, void* selection, void** library)
{
	return FontCreateLibraryWithEdition(memory, selection, 0, library);
}

// sceFontDestroyLibrary — NID FXP359ygujs
static int KYTY_SYSV_ABI FontDestroyLibrary(void** library)
{
	PRINT_NAME();
	if (library != nullptr && *library != nullptr)
	{
		delete static_cast<LibraryState*>(*library);
		*library = nullptr;
	}
	return OK;
}

// sceFontSupportSystemFonts / ExternalFonts — return OK so library setup continues.
static int KYTY_SYSV_ABI FontSupportSystemFonts(void* library)
{
	PRINT_NAME();
	printf("\t library = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	return OK;
}

static int KYTY_SYSV_ABI FontSupportExternalFonts(void* library)
{
	PRINT_NAME();
	printf("\t library = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	return OK;
}

// sceFontAttachDeviceCacheBuffer — NID CUKn5pX-NVY (library, buffer, size).
// Cache is optional for boot; accept guest buffer (null buffer observed with size).
static int KYTY_SYSV_ABI FontAttachDeviceCacheBuffer(void* library, void* buffer, uint32_t size)
{
	PRINT_NAME();
	printf("\t library = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	printf("\t buffer  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buffer));
	printf("\t size    = 0x%" PRIx32 "\n", size);
	return OK;
}

} // namespace Font

LIB_DEFINE(InitFont_1)
{
	LIB_FUNC("whrS4oksXc4", Font::FontMemoryInit);
	LIB_FUNC("h6hIgxXEiEc", Font::FontMemoryTerm);
	LIB_FUNC("nWrfPI4Okmg", Font::FontCreateLibrary);
	LIB_FUNC("n590hj5Oe-k", Font::FontCreateLibraryWithEdition);
	LIB_FUNC("FXP359ygujs", Font::FontDestroyLibrary);
	LIB_FUNC("SsRbbCiWoGw", Font::FontSupportSystemFonts);
	LIB_FUNC("mz2iTY0MK4A", Font::FontSupportExternalFonts);
	LIB_FUNC("CUKn5pX-NVY", Font::FontAttachDeviceCacheBuffer);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
