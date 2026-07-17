#include "Emulator/Libs/ApplicationHeap.h"

#include "Emulator/Loader/Elf.h"
#include "Emulator/Loader/RuntimeLinker.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::ApplicationHeap {

namespace {

using Kyty::Loader::Elf64_Half;
using Kyty::Loader::Elf64_Phdr;
using Kyty::Loader::PF_R;
using Kyty::Loader::PF_X;
using Kyty::Loader::PT_LOAD;

static ApiV2* g_registered_api = nullptr;
static bool   g_initialized    = false;

static uint64_t get_aligned_size(const Elf64_Phdr* p)
{
	return (p->p_align != 0 ? (p->p_memsz + (p->p_align - 1)) & ~(p->p_align - 1) : p->p_memsz);
}

static void get_text_bounds(Loader::Program* program, uint64_t* text_begin, uint64_t* text_end)
{
	*text_begin = 0;
	*text_end   = 0;

	if (program == nullptr || program->elf == nullptr)
	{
		return;
	}

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();
	if (ehdr == nullptr || phdr == nullptr)
	{
		return;
	}

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
	{
		if (phdr[i].p_memsz == 0 || phdr[i].p_type != PT_LOAD)
		{
			continue;
		}

		if ((phdr[i].p_flags & PF_X) == 0)
		{
			continue;
		}

		const uint64_t begin = phdr[i].p_vaddr + program->base_vaddr;
		const uint64_t end   = begin + get_aligned_size(phdr + i);

		if (*text_begin == 0 || begin < *text_begin)
		{
			*text_begin = begin;
		}
		if (end > *text_end)
		{
			*text_end = end;
		}
	}
}

static bool try_invoke_create(ApiV2* table, uint64_t text_begin, uint64_t text_end)
{
	if (table == nullptr || g_initialized)
	{
		return g_initialized;
	}

	// Full four-slot validation: header-only false positives must not call create.
	if (!IsValidApiV2Table(table, text_begin, text_end))
	{
		return false;
	}

	table->create();
	g_initialized = true;
	return true;
}

// Fallback when the guest never calls KernelRtldSetApplicationHeapAPI before
// early malloc (observed on a Gen5 startup path: GetGPI then null-mspace assert). Scan
// readable PT_LOAD for a fully validated v2 table; create is guest code.
static bool scan_segment_for_table(uint64_t segment_addr, uint64_t segment_size, uint64_t text_begin, uint64_t text_end)
{
	if (segment_size < sizeof(ApiV2))
	{
		return false;
	}

	auto* base = reinterpret_cast<uint8_t*>(segment_addr);
	for (uint64_t off = 0; off + sizeof(ApiV2) <= segment_size; off += alignof(uint64_t))
	{
		auto* candidate = reinterpret_cast<ApiV2*>(base + off);
		if (try_invoke_create(candidate, text_begin, text_end))
		{
			g_registered_api = candidate;
			return true;
		}
	}

	return false;
}

} // namespace

bool IsApiV2Header(uint64_t size, uint64_t version)
{
	return size == kApiV2Size && version == kApiV2Version;
}

bool IsGuestCodePointer(uint64_t addr, uint64_t text_begin, uint64_t text_end)
{
	return addr >= text_begin && addr < text_end;
}

bool IsValidApiV2Table(const ApiV2* table, uint64_t text_begin, uint64_t text_end)
{
	if (table == nullptr || text_begin == 0 || text_end <= text_begin)
	{
		return false;
	}

	if (!IsApiV2Header(table->size, table->version))
	{
		return false;
	}

	const uint64_t create    = reinterpret_cast<uint64_t>(table->create);
	const uint64_t destroy   = reinterpret_cast<uint64_t>(table->destroy);
	const uint64_t malloc_fn = reinterpret_cast<uint64_t>(table->malloc);
	const uint64_t free_fn   = reinterpret_cast<uint64_t>(table->free);

	return create != 0 && destroy != 0 && malloc_fn != 0 && free_fn != 0 && IsGuestCodePointer(create, text_begin, text_end) &&
	       IsGuestCodePointer(destroy, text_begin, text_end) && IsGuestCodePointer(malloc_fn, text_begin, text_end) &&
	       IsGuestCodePointer(free_fn, text_begin, text_end);
}

void RegisterApi(void* api)
{
	if (api == nullptr)
	{
		return;
	}

	auto* words = reinterpret_cast<uint64_t*>(api);
	if (IsApiV2Header(words[0], words[1]))
	{
		g_registered_api = reinterpret_cast<ApiV2*>(api);
		return;
	}

	// Legacy HeapAPI tables (malloc-first) are not the evidenced v2 layout.
	g_registered_api = nullptr;
}

void EnsureInitialized(Loader::Program* program)
{
	if (g_initialized)
	{
		return;
	}

	uint64_t text_begin = 0;
	uint64_t text_end   = 0;
	get_text_bounds(program, &text_begin, &text_end);

	if (g_registered_api != nullptr && try_invoke_create(g_registered_api, text_begin, text_end))
	{
		return;
	}

	if (program == nullptr || program->elf == nullptr)
	{
		return;
	}

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();
	if (ehdr == nullptr || phdr == nullptr)
	{
		return;
	}

	// Prefer registered API; if absent, scan main-image readable LOAD segments
	// with IsValidApiV2Table only (create/destroy/malloc/free all in text).
	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
	{
		if (phdr[i].p_memsz == 0 || phdr[i].p_type != PT_LOAD)
		{
			continue;
		}

		if ((phdr[i].p_flags & PF_R) == 0)
		{
			continue;
		}

		const uint64_t segment_addr = phdr[i].p_vaddr + program->base_vaddr;
		const uint64_t segment_size = get_aligned_size(phdr + i);

		if (scan_segment_for_table(segment_addr, segment_size, text_begin, text_end))
		{
			return;
		}
	}
}

bool IsInitialized()
{
	return g_initialized;
}

} // namespace Kyty::Libs::LibKernel::ApplicationHeap

#endif // KYTY_EMU_ENABLED
