#include "Kyty/Core/SDLSubsystem.h"

#include "Kyty/Core/Common.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#endif

// IWYU pragma: no_include <intrin.h>
// IWYU pragma: no_include "SDL_error.h"
// IWYU pragma: no_include "SDL_platform.h"
// IWYU pragma: no_include "SDL_stdinc.h"
// IWYU pragma: no_include "begin_code.h"

#include "SDL.h"

namespace Kyty::Core {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
struct alignas(std::max_align_t) SdlHostAllocation
{
	size_t size = 0;
};

static void* sdl_host_virtual_alloc(size_t size)
{
	if (size == 0)
	{
		size = 1;
	}
	if (size > SIZE_MAX - sizeof(SdlHostAllocation))
	{
		return nullptr;
	}

	const SIZE_T total_size = static_cast<SIZE_T>(sizeof(SdlHostAllocation) + size);
	for (uint32_t attempt = 0; attempt < 8; ++attempt)
	{
		auto* raw = VirtualAlloc(nullptr, total_size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
		if (raw == nullptr)
		{
			return nullptr;
		}
		if (reinterpret_cast<uintptr_t>(raw) >= UINT64_C(0x100000000))
		{
			auto* allocation = static_cast<SdlHostAllocation*>(raw);
			allocation->size = size;
			return allocation + 1;
		}
		VirtualFree(raw, 0, MEM_RELEASE);
	}
	return nullptr;
}

static SdlHostAllocation* sdl_host_allocation(void* memory)
{
	return static_cast<SdlHostAllocation*>(memory) - 1;
}
#endif

static void* sdl_host_malloc(size_t size)
{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return sdl_host_virtual_alloc(size);
#else
	if (size == 0)
	{
		size = 1;
	}
	return std::malloc(size);
#endif
}

static void* sdl_host_calloc(size_t nmemb, size_t size)
{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (size != 0 && nmemb > SIZE_MAX / size)
	{
		return nullptr;
	}
	void* memory = sdl_host_virtual_alloc(nmemb * size);
	if (memory != nullptr)
	{
		std::memset(memory, 0, nmemb * size);
	}
	return memory;
#else
	return std::calloc(nmemb, size);
#endif
}

static void* sdl_host_realloc(void* memory, size_t size)
{
	if (size == 0)
	{
		size = 1;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (memory == nullptr)
	{
		return sdl_host_virtual_alloc(size);
	}
	void* replacement = sdl_host_virtual_alloc(size);
	if (replacement != nullptr)
	{
		auto* allocation = sdl_host_allocation(memory);
		std::memcpy(replacement, memory, allocation->size < size ? allocation->size : size);
		VirtualFree(allocation, 0, MEM_RELEASE);
	}
	return replacement;
#else
	if (size == 0)
	{
		size = 1;
	}
	return std::realloc(memory, size);
#endif
}

static void sdl_host_free(void* memory)
{
	if (memory == nullptr)
	{
		return;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	VirtualFree(sdl_host_allocation(memory), 0, MEM_RELEASE);
#else
	std::free(memory);
#endif
}

void SDLSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	if (SDL_SetMemoryFunctions(sdl_host_malloc, sdl_host_calloc, sdl_host_realloc, sdl_host_free) != 0)
	{
		this->Fail("%s\n", SDL_GetError());
		return;
	}
	SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");

#if SDL_DYNAMIC_API != 0
#error "SDL_DYNAMIC_API"
#endif

	if (SDL_Init(0) < 0)
	{
		this->Fail("%s\n", SDL_GetError());
		return;
	}
}

void SDLSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}

void SDLSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent)
{
	SDL_Quit();
}

} // namespace Kyty::Core
