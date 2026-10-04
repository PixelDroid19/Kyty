//
// Original algorithm is from:
// 	https://sqlite.org/src/file?name=src/mem3.c
// 	SQLite source code is in the public-domain and is free to everyone to use for any purpose.

#include "Kyty/Core/MSpace.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/LinkList.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"

#include <cstdint>
#include <cstring>

namespace Kyty::Core {

static constexpr size_t   MSPACE_HEADER_SIZE         = 1440;
static constexpr size_t   MSPACE_NAME_SIZE           = 32;
static constexpr uint32_t MSPACE_ALIGNED_PREFIX_SIZE = sizeof(uint64_t) * 2u;

static uint64_t align(uint64_t v, uint64_t a)
{
	return (v + (a - 1)) & ~(a - 1);
}

static constexpr uint32_t MSPACE_ARRAY_SMALL = 10;
static constexpr uint32_t MSPACE_ARRAY_HASH  = 61;

struct MSpaceBlock
{
	union
	{
		struct
		{
			uint32_t prev_size;
			uint32_t size_4x;
		} hdr;
		struct
		{
			uint32_t next;
			uint32_t prev;
		} list;
	} u;
};

struct MSpaceContext
{
	uint32_t              capacity                            = 0;
	MSpaceBlock*          base                                = nullptr;
	bool                  in_callback                         = false;
	Core::Mutex*          mutex                               = nullptr;
	mspace_dbg_callback_t dbg_callback                        = nullptr;
	char                  name[MSPACE_NAME_SIZE]              = {0};
	uint32_t              index_of_key_chunk                  = 0;
	uint32_t              size_of_key_chunk                   = 0;
	uint32_t              array_small[MSPACE_ARRAY_SMALL - 1] = {};
	uint32_t              array_hash[MSPACE_ARRAY_HASH]       = {};
};

struct MSpaceRegistration
{
	MSpaceContext* context           = nullptr;
	uintptr_t      begin             = 0;
	uintptr_t      end               = 0;
	MSpaceContext* parent_context    = nullptr;
	uintptr_t      parent_backing    = 0;
	uintptr_t      parent_allocation = 0;
};

struct MSpaceAllocationPin
{
	MSpaceContext* context    = nullptr;
	uintptr_t      allocation = 0;
};

static Core::Mutex                     g_mspace_registry_mutex;
static Core::List<MSpaceRegistration>  g_mspace_registry;
static Core::List<MSpaceAllocationPin> g_mspace_realloc_pins;

// The registry mutex protects both the arena registrations and the transient realloc pins.
static bool MSpaceAllocationIsPinned(MSpaceContext* context, uintptr_t allocation)
{
	const auto pin = g_mspace_realloc_pins.Find(context, allocation, [](const auto& registered, auto* pin_context, uintptr_t pin_allocation)
	                                            { return registered.context == pin_context && registered.allocation == pin_allocation; });
	return g_mspace_realloc_pins.IndexValid(pin);
}

static bool MSpaceContextIsPinned(MSpaceContext* context)
{
	const auto pin =
	    g_mspace_realloc_pins.Find(context, [](const auto& registered, auto* pin_context) { return registered.context == pin_context; });
	return g_mspace_realloc_pins.IndexValid(pin);
}

class MSpaceReallocPin
{
public:
	MSpaceReallocPin() = default;

	void Add(MSpaceContext* context, uintptr_t allocation) { m_pin = g_mspace_realloc_pins.Add({context, allocation}); }

	~MSpaceReallocPin()
	{
		if (g_mspace_realloc_pins.IndexValid(m_pin))
		{
			Core::LockGuard registry_lock(g_mspace_registry_mutex);
			g_mspace_realloc_pins.Remove(m_pin);
		}
	}

	MSpaceReallocPin(const MSpaceReallocPin&)            = delete;
	MSpaceReallocPin& operator=(const MSpaceReallocPin&) = delete;

private:
	Core::List<MSpaceAllocationPin>::Index m_pin = {nullptr};
};

static bool MSpaceNameValid(const char* name)
{
	if (name == nullptr)
	{
		return false;
	}

	for (size_t i = 0; i < MSPACE_NAME_SIZE; i++)
	{
		if (name[i] == '\0')
		{
			return true;
		}
	}
	return false;
}

static void MSpaceInternalUnlinkFromList(MSpaceContext& ctx, uint32_t i, uint32_t* root)
{
	uint32_t next = ctx.base[i].u.list.next;
	uint32_t prev = ctx.base[i].u.list.prev;

	if (prev == 0)
	{
		*root = next;
	} else
	{
		ctx.base[prev].u.list.next = next;
	}
	if (next != 0)
	{
		ctx.base[next].u.list.prev = prev;
	}
	ctx.base[i].u.list.next = 0;
	ctx.base[i].u.list.prev = 0;
}

static void MSpaceInternalUnlink(MSpaceContext& ctx, uint32_t i)
{
	uint32_t size = ctx.base[i - 1].u.hdr.size_4x / 4;
	if (size <= MSPACE_ARRAY_SMALL)
	{
		MSpaceInternalUnlinkFromList(ctx, i, &ctx.array_small[size - 2]);
	} else
	{
		uint32_t hash = size % MSPACE_ARRAY_HASH;
		MSpaceInternalUnlinkFromList(ctx, i, &ctx.array_hash[hash]);
	}
}

static void MSpaceInternalLinkIntoList(MSpaceContext& ctx, uint32_t i, uint32_t* root)
{
	ctx.base[i].u.list.next = *root;
	ctx.base[i].u.list.prev = 0;
	if (*root != 0u)
	{
		ctx.base[*root].u.list.prev = i;
	}
	*root = i;
}

static void MSpaceInternalLink(MSpaceContext& ctx, uint32_t i)
{
	uint32_t size = ctx.base[i - 1].u.hdr.size_4x / 4;

	if (size <= MSPACE_ARRAY_SMALL)
	{
		MSpaceInternalLinkIntoList(ctx, i, &ctx.array_small[size - 2]);
	} else
	{
		uint32_t hash = size % MSPACE_ARRAY_HASH;
		MSpaceInternalLinkIntoList(ctx, i, &ctx.array_hash[hash]);
	}
}

static void MSpaceInternalEnter(MSpaceContext& ctx)
{
	if (ctx.mutex != nullptr)
	{
		ctx.mutex->Lock();
	}
}

static void MSpaceInternalLeave(MSpaceContext& ctx)
{
	if (ctx.mutex != nullptr)
	{
		ctx.mutex->Unlock();
	}
}

static void MSpaceInternalOutOfMemory(MSpaceContext& ctx, uint32_t free_bytes, uint32_t bytes)
{
	if (!ctx.in_callback)
	{
		ctx.in_callback = true;

		if (ctx.dbg_callback != nullptr)
		{
			MSpaceInternalLeave(ctx);

			ctx.dbg_callback(&ctx, free_bytes, bytes);

			MSpaceInternalEnter(ctx);
		}

		ctx.in_callback = false;
	}
}

static void* MSpaceInternalCheckout(MSpaceContext& ctx, uint32_t i, uint32_t num_blocks)
{
	uint32_t x                                   = ctx.base[i - 1].u.hdr.size_4x;
	ctx.base[i - 1].u.hdr.size_4x                = num_blocks * 4 | 1u | (x & 2u);
	ctx.base[i + num_blocks - 1].u.hdr.prev_size = num_blocks;
	ctx.base[i + num_blocks - 1].u.hdr.size_4x |= 2u;
	return &ctx.base[i];
}

static void* MSpaceInternalFromKeyBlk(MSpaceContext& ctx, uint32_t num_blocks)
{
	if (num_blocks >= ctx.size_of_key_chunk - 1)
	{
		void* p                = MSpaceInternalCheckout(ctx, ctx.index_of_key_chunk, ctx.size_of_key_chunk);
		ctx.index_of_key_chunk = 0;
		ctx.size_of_key_chunk  = 0;
		return p;
	}

	uint32_t newi = ctx.index_of_key_chunk + ctx.size_of_key_chunk - num_blocks;

	ctx.base[ctx.index_of_key_chunk + ctx.size_of_key_chunk - 1].u.hdr.prev_size = num_blocks;
	ctx.base[ctx.index_of_key_chunk + ctx.size_of_key_chunk - 1].u.hdr.size_4x |= 2u;
	ctx.base[newi - 1].u.hdr.size_4x = num_blocks * 4 + 1;
	ctx.size_of_key_chunk -= num_blocks;
	ctx.base[newi - 1].u.hdr.prev_size                 = ctx.size_of_key_chunk;
	uint32_t x                                         = ctx.base[ctx.index_of_key_chunk - 1].u.hdr.size_4x & 2u;
	ctx.base[ctx.index_of_key_chunk - 1].u.hdr.size_4x = ctx.size_of_key_chunk * 4 | x;
	return static_cast<void*>(&ctx.base[newi]);
}

static void MSpaceInternalMerge(MSpaceContext& ctx, uint32_t* root)
{
	uint32_t next = 0;

	for (uint32_t i = *root; i > 0; i = next)
	{
		next          = ctx.base[i].u.list.next;
		uint32_t size = ctx.base[i - 1].u.hdr.size_4x;
		if ((size & 2u) == 0)
		{
			MSpaceInternalUnlinkFromList(ctx, i, root);
			uint32_t prev = i - ctx.base[i - 1].u.hdr.prev_size;
			if (prev == next)
			{
				next = ctx.base[prev].u.list.next;
			}
			MSpaceInternalUnlink(ctx, prev);
			size                                      = i + size / 4 - prev;
			uint32_t x                                = ctx.base[prev - 1].u.hdr.size_4x & 2u;
			ctx.base[prev - 1].u.hdr.size_4x          = size * 4 | x;
			ctx.base[prev + size - 1].u.hdr.prev_size = size;
			MSpaceInternalLink(ctx, prev);
			i = prev;
		} else
		{
			size /= 4;
		}
		if (size > ctx.size_of_key_chunk)
		{
			ctx.index_of_key_chunk = i;
			ctx.size_of_key_chunk  = size;
		}
	}
}

static void* MSpaceInternalMallocUnsafe(MSpaceContext& ctx, uint32_t size, uint32_t report_size)
{
	uint32_t num_blocks = 0;

	if (size <= 12)
	{
		num_blocks = 2;
	} else
	{
		num_blocks = (size + 11) / 8;
	}

	if (num_blocks <= MSPACE_ARRAY_SMALL)
	{
		uint32_t i = ctx.array_small[num_blocks - 2];
		if (i > 0)
		{
			MSpaceInternalUnlinkFromList(ctx, i, &ctx.array_small[num_blocks - 2]);
			return MSpaceInternalCheckout(ctx, i, num_blocks);
		}
	} else
	{
		uint32_t hash = num_blocks % MSPACE_ARRAY_HASH;
		for (uint32_t i = ctx.array_hash[hash]; i > 0; i = ctx.base[i].u.list.next)
		{
			if (ctx.base[i - 1].u.hdr.size_4x / 4 == num_blocks)
			{
				MSpaceInternalUnlinkFromList(ctx, i, &ctx.array_hash[hash]);
				return MSpaceInternalCheckout(ctx, i, num_blocks);
			}
		}
	}

	if (ctx.size_of_key_chunk >= num_blocks)
	{
		return MSpaceInternalFromKeyBlk(ctx, num_blocks);
	}

	for (uint32_t to_free = num_blocks * 16; to_free < (ctx.capacity * 16); to_free *= 2)
	{
		MSpaceInternalOutOfMemory(ctx, to_free, report_size);
		if (ctx.index_of_key_chunk != 0u)
		{
			MSpaceInternalLink(ctx, ctx.index_of_key_chunk);
			ctx.index_of_key_chunk = 0;
			ctx.size_of_key_chunk  = 0;
		}
		for (auto& hash: ctx.array_hash)
		{
			MSpaceInternalMerge(ctx, &hash);
		}
		for (auto& small: ctx.array_small)
		{
			MSpaceInternalMerge(ctx, &small);
		}
		if (ctx.size_of_key_chunk != 0u)
		{
			MSpaceInternalUnlink(ctx, ctx.index_of_key_chunk);
			if (ctx.size_of_key_chunk >= num_blocks)
			{
				return MSpaceInternalFromKeyBlk(ctx, num_blocks);
			}
		}
	}

	return nullptr;
}

static void MSpaceInternalFreeUnsafe(MSpaceContext& ctx, void* old)
{
	auto* p = static_cast<MSpaceBlock*>(old);

	uint32_t index = p - ctx.base;
	uint32_t size  = ctx.base[index - 1].u.hdr.size_4x / 4;
	ctx.base[index - 1].u.hdr.size_4x &= ~1u;
	ctx.base[index + size - 1].u.hdr.prev_size = size;
	ctx.base[index + size - 1].u.hdr.size_4x &= ~2u;
	MSpaceInternalLink(ctx, index);

	if (ctx.index_of_key_chunk != 0u)
	{
		while ((ctx.base[ctx.index_of_key_chunk - 1].u.hdr.size_4x & 2u) == 0)
		{
			size = ctx.base[ctx.index_of_key_chunk - 1].u.hdr.prev_size;
			ctx.index_of_key_chunk -= size;
			ctx.size_of_key_chunk += size;
			MSpaceInternalUnlink(ctx, ctx.index_of_key_chunk);
			uint32_t x                                         = ctx.base[ctx.index_of_key_chunk - 1].u.hdr.size_4x & 2u;
			ctx.base[ctx.index_of_key_chunk - 1].u.hdr.size_4x = ctx.size_of_key_chunk * 4 | x;
			ctx.base[ctx.index_of_key_chunk + ctx.size_of_key_chunk - 1].u.hdr.prev_size = ctx.size_of_key_chunk;
		}
		uint32_t x = ctx.base[ctx.index_of_key_chunk - 1].u.hdr.size_4x & 2u;
		while ((ctx.base[ctx.index_of_key_chunk + ctx.size_of_key_chunk - 1].u.hdr.size_4x & 1u) == 0)
		{
			MSpaceInternalUnlink(ctx, ctx.index_of_key_chunk + ctx.size_of_key_chunk);
			ctx.size_of_key_chunk += ctx.base[ctx.index_of_key_chunk + ctx.size_of_key_chunk - 1].u.hdr.size_4x / 4;
			ctx.base[ctx.index_of_key_chunk - 1].u.hdr.size_4x                           = ctx.size_of_key_chunk * 4 | x;
			ctx.base[ctx.index_of_key_chunk + ctx.size_of_key_chunk - 1].u.hdr.prev_size = ctx.size_of_key_chunk;
		}
	}
}

static uint32_t MSpaceInternalSize(void* p)
{
	auto* block = static_cast<MSpaceBlock*>(p);
	return (block[-1].u.hdr.size_4x & ~3u) * 2 - 4;
}

struct MSpaceAllocation
{
	uintptr_t address     = 0;
	size_t    usable_size = 0;
};

static bool MSpaceInternalGetAllocationHeader(const MSpaceContext& ctx, uintptr_t user, MSpaceAllocation* allocation_info)
{
	if (allocation_info == nullptr || ctx.base == nullptr)
	{
		return false;
	}

	const auto   base         = reinterpret_cast<uintptr_t>(ctx.base);
	const size_t arena_blocks = static_cast<size_t>(ctx.capacity) + 2u;
	if (arena_blocks > UINTPTR_MAX / sizeof(MSpaceBlock))
	{
		return false;
	}
	const size_t arena_size = arena_blocks * sizeof(MSpaceBlock);
	if (arena_size > UINTPTR_MAX - base)
	{
		return false;
	}
	const auto end = base + arena_size;

	if (user < base || user >= end || user - base < sizeof(uint64_t))
	{
		return false;
	}

	uint64_t allocation_address = 0;
	std::memcpy(&allocation_address, reinterpret_cast<const void*>(user - sizeof(uint64_t)), sizeof(allocation_address));
	if (static_cast<uint64_t>(static_cast<uintptr_t>(allocation_address)) != allocation_address)
	{
		return false;
	}

	const auto allocation = static_cast<uintptr_t>(allocation_address);
	if (allocation < base || allocation >= end || allocation - base < sizeof(MSpaceBlock) ||
	    (allocation - base) % sizeof(MSpaceBlock) != 0u)
	{
		return false;
	}

	const size_t allocation_index = (allocation - base) / sizeof(MSpaceBlock);
	if (allocation_index == 0u || allocation_index > ctx.capacity)
	{
		return false;
	}

	const uint32_t size_4x     = ctx.base[allocation_index - 1].u.hdr.size_4x;
	const size_t   block_count = static_cast<size_t>(size_4x & ~3u) / 4u;
	if ((size_4x & 1u) == 0u || block_count == 0u || block_count > static_cast<size_t>(ctx.capacity) - allocation_index + 1u)
	{
		return false;
	}

	const size_t block_size = block_count * sizeof(MSpaceBlock);
	if (block_size < MSPACE_ALIGNED_PREFIX_SIZE)
	{
		return false;
	}
	const size_t usable_size = block_size - sizeof(uint32_t);
	if (usable_size > end - allocation || user < allocation)
	{
		return false;
	}

	const size_t user_offset = user - allocation;
	if (user_offset < MSPACE_ALIGNED_PREFIX_SIZE || user_offset > usable_size)
	{
		return false;
	}

	uint64_t canonical_user = 0;
	std::memcpy(&canonical_user, reinterpret_cast<const void*>(allocation), sizeof(canonical_user));
	if (canonical_user != static_cast<uint64_t>(user))
	{
		return false;
	}

	allocation_info->address     = allocation;
	allocation_info->usable_size = usable_size - user_offset;
	return true;
}

static bool MSpaceInternalGetAllocation(const MSpaceContext& ctx, uintptr_t user, MSpaceAllocation* allocation_info)
{
	if (!MSpaceInternalGetAllocationHeader(ctx, user, allocation_info))
	{
		return false;
	}

	// Registry admission is infrequent; walk the chunk chain here so a forged
	// header inside a live arena cannot become a nested heap backing.
	const auto   base             = reinterpret_cast<uintptr_t>(ctx.base);
	const size_t allocation_index = (allocation_info->address - base) / sizeof(MSpaceBlock);
	for (size_t index = 1; index <= ctx.capacity;)
	{
		const uint32_t size_4x     = ctx.base[index - 1].u.hdr.size_4x;
		const size_t   block_count = static_cast<size_t>(size_4x & ~3u) / 4u;
		if (block_count == 0u || block_count > static_cast<size_t>(ctx.capacity) - index + 1u)
		{
			return false;
		}
		if (index == allocation_index)
		{
			return true;
		}
		index += block_count;
	}

	return false;
}

static void* MSpaceInternalMalloc(MSpaceContext& ctx, uint32_t size, uint32_t report_size)
{
	MSpaceInternalEnter(ctx);
	auto* p = MSpaceInternalMallocUnsafe(ctx, size, report_size);
	MSpaceInternalLeave(ctx);
	return p;
}

static bool MSpaceInternalGetAlignedAllocationSize(uint32_t size, uint64_t boundary, uint32_t* allocation_size)
{
	if (allocation_size == nullptr || boundary == 0u || boundary > UINT32_MAX)
	{
		return false;
	}

	const auto boundary_size = static_cast<uint32_t>(boundary);
	if (boundary_size > UINT32_MAX - size)
	{
		return false;
	}

	const uint32_t padded_size = size + boundary_size;
	if (MSPACE_ALIGNED_PREFIX_SIZE > UINT32_MAX - padded_size)
	{
		return false;
	}

	*allocation_size = padded_size + MSPACE_ALIGNED_PREFIX_SIZE;
	return true;
}

static void MSpaceInternalSetAlignedPrefix(uint64_t allocation, uint64_t user)
{
	// raw[0] binds the allocation to its one canonical aligned return pointer;
	// user[-1] remains the raw chunk marker consumed by free and realloc.
	auto* raw = reinterpret_cast<uint64_t*>(allocation);
	raw[0]    = user;

	auto* buffer = reinterpret_cast<uint64_t*>(user);
	buffer[-1]   = allocation;
}

static void* MSpaceInternalMalloc_align(MSpaceContext& ctx, uint32_t size, uint64_t boundary)
{
	uint32_t allocation_size = 0;
	if (!MSpaceInternalGetAlignedAllocationSize(size, boundary, &allocation_size))
	{
		return nullptr;
	}

	auto addr = reinterpret_cast<uint64_t>(MSpaceInternalMalloc(ctx, allocation_size, size));
	if (addr != 0)
	{
		const uint64_t aligned_addr = align(addr + MSPACE_ALIGNED_PREFIX_SIZE, boundary);
		MSpaceInternalSetAlignedPrefix(addr, aligned_addr);
		return reinterpret_cast<void*>(aligned_addr);
	}
	return nullptr;
}

static void MSpaceInternalFree(MSpaceContext& ctx, void* prior)
{
	MSpaceInternalEnter(ctx);
	MSpaceInternalFreeUnsafe(ctx, prior);
	MSpaceInternalLeave(ctx);
}

static void MSpaceInternalFree_align(MSpaceContext& ctx, void* buf)
{
	auto*    buf64     = static_cast<uint64_t*>(buf);
	uint64_t real_addr = buf64[-1];
	MSpaceInternalFree(ctx, reinterpret_cast<void*>(real_addr));
}

static void* MSpaceInternalRealloc(MSpaceContext& ctx, void* prior, uint32_t size, uint32_t report_size)
{
	if (prior == nullptr)
	{
		return MSpaceInternalMalloc(ctx, size, report_size);
	}
	if (size <= 0)
	{
		MSpaceInternalFree(ctx, prior);
		return nullptr;
	}
	auto old = MSpaceInternalSize(prior);
	if (size <= old && size >= old - 128)
	{
		return prior;
	}
	MSpaceInternalEnter(ctx);
	auto* p = MSpaceInternalMallocUnsafe(ctx, size, report_size);
	if (p != nullptr)
	{
		if (old < size)
		{
			memcpy(p, prior, old);
		} else
		{
			memcpy(p, prior, size);
		}
		MSpaceInternalFreeUnsafe(ctx, prior);
	}
	MSpaceInternalLeave(ctx);
	return p;
}

static void* MSpaceInternalRealloc_align(MSpaceContext& ctx, void* buf, uint32_t size, uint64_t boundary)
{
	uint32_t allocation_size = 0;
	if (!MSpaceInternalGetAlignedAllocationSize(size, boundary, &allocation_size))
	{
		return nullptr;
	}

	auto*    buf64     = static_cast<uint64_t*>(buf);
	uint64_t real_addr = (buf64 != nullptr ? buf64[-1] : 0);
	auto     addr = reinterpret_cast<uint64_t>(MSpaceInternalRealloc(ctx, reinterpret_cast<uint64_t*>(real_addr), allocation_size, size));
	if (addr != 0)
	{
		const uint64_t aligned_addr = align(addr + MSPACE_ALIGNED_PREFIX_SIZE, boundary);
		if (addr != real_addr)
		{
			auto* dst = reinterpret_cast<void*>(aligned_addr);
			auto* src = reinterpret_cast<void*>(addr + reinterpret_cast<uint64_t>(buf) - real_addr);
			if (dst != src)
			{
				memmove(dst, src, size);
			}
		}
		MSpaceInternalSetAlignedPrefix(addr, aligned_addr);
		return reinterpret_cast<void*>(aligned_addr);
	}
	return nullptr;
}

static bool MSpaceInternalInit(MSpaceContext& ctx, const char* name, void* base, size_t capacity, bool thread_safe,
                               mspace_dbg_callback_t dbg_callback)
{
	if (sizeof(MSpaceBlock) != 8)
	{
		return false;
	}

	if ((capacity / sizeof(MSpaceBlock)) < 3)
	{
		return false;
	}

	ctx = MSpaceContext();

	int s = snprintf(ctx.name, sizeof(ctx.name), "%s", name);

	if (static_cast<size_t>(s) >= sizeof(ctx.name))
	{
		return false;
	}

	ctx.base     = static_cast<MSpaceBlock*>(base);
	ctx.capacity = static_cast<uint32_t>((capacity / sizeof(MSpaceBlock)) - 2);

	ctx.size_of_key_chunk                  = ctx.capacity;
	ctx.index_of_key_chunk                 = 1;
	ctx.base[0].u.hdr.size_4x              = (ctx.size_of_key_chunk << 2u) + 2;
	ctx.base[ctx.capacity].u.hdr.prev_size = ctx.capacity;
	ctx.base[ctx.capacity].u.hdr.size_4x   = 1;

	ctx.mutex        = (thread_safe ? new Core::Mutex : nullptr);
	ctx.dbg_callback = dbg_callback;

	return true;
}

static void MSpaceInternalShutdown(MSpaceContext& ctx)
{
	delete ctx.mutex;
}

static bool MSpaceDestroyRegistered(MSpaceContext* context)
{
	if (context == nullptr)
	{
		return false;
	}

	Core::LockGuard registry_lock(g_mspace_registry_mutex);
	const auto      registration =
	    g_mspace_registry.Find(context, [](const auto& registered, auto* value) { return registered.context == value; });
	if (!g_mspace_registry.IndexValid(registration))
	{
		return false;
	}
	if (MSpaceContextIsPinned(context))
	{
		return false;
	}

	FOR_LIST(index, g_mspace_registry)
	{
		if (g_mspace_registry[index].parent_context == context)
		{
			return false;
		}
	}

	g_mspace_registry.Remove(registration);
	MSpaceInternalShutdown(*context);
	return true;
}

static bool MSpaceRegister(MSpaceContext* context, uintptr_t begin, uintptr_t end, const char* name, void* heap_base, size_t heap_capacity,
                           bool thread_safe, mspace_dbg_callback_t dbg_callback)
{
	if (context == nullptr || begin >= end)
	{
		return false;
	}

	Core::LockGuard           registry_lock(g_mspace_registry_mutex);
	const MSpaceRegistration* parent_registration = nullptr;
	size_t                    parent_size         = 0;
	FOR_LIST(index, g_mspace_registry)
	{
		const auto& registered = g_mspace_registry[index];
		if (registered.context == context || (begin == registered.begin && end == registered.end))
		{
			return false;
		}

		if (!(begin < registered.end && registered.begin < end))
		{
			continue;
		}

		if (begin >= registered.begin && end <= registered.end)
		{
			const size_t registered_size = registered.end - registered.begin;
			if (parent_registration == nullptr || registered_size < parent_size)
			{
				parent_registration = &registered;
				parent_size         = registered_size;
			}
			continue;
		}

		return false;
	}

	MSpaceRegistration registration {};
	registration.context = context;
	registration.begin   = begin;
	registration.end     = end;

	if (parent_registration != nullptr)
	{
		auto* parent_context = parent_registration->context;
		if (parent_context == nullptr)
		{
			return false;
		}

		// Registry before arena: usable-size lookup, creation, and free all use this order.
		MSpaceInternalEnter(*parent_context);
		MSpaceAllocation parent_allocation {};
		const bool       valid_parent_allocation =
		    MSpaceInternalGetAllocation(*parent_context, begin, &parent_allocation) && end - begin <= parent_allocation.usable_size;
		if (!valid_parent_allocation || MSpaceAllocationIsPinned(parent_context, parent_allocation.address))
		{
			MSpaceInternalLeave(*parent_context);
			return false;
		}

		registration.parent_context    = parent_context;
		registration.parent_backing    = begin;
		registration.parent_allocation = parent_allocation.address;
		if (!MSpaceInternalInit(*context, name, heap_base, heap_capacity, thread_safe, dbg_callback))
		{
			MSpaceInternalLeave(*parent_context);
			return false;
		}

		g_mspace_registry.Add(registration);
		MSpaceInternalLeave(*parent_context);
		return true;
	}

	if (!MSpaceInternalInit(*context, name, heap_base, heap_capacity, thread_safe, dbg_callback))
	{
		return false;
	}

	g_mspace_registry.Add(registration);
	return true;
}

mspace_t MSpaceCreate(const char* name, void* base, size_t capacity, bool thread_safe, mspace_dbg_callback_t dbg_callback)
{
	auto addr = reinterpret_cast<uint64_t>(base);

	if ((addr & 0x7u) != 0)
	{
		return nullptr;
	}

	if ((capacity & 0x7u) != 0)
	{
		return nullptr;
	}

	if (sizeof(MSpaceContext) > MSPACE_HEADER_SIZE)
	{
		return nullptr;
	}

	if (base == nullptr || capacity < MSPACE_HEADER_SIZE || capacity > UINT64_MAX - addr)
	{
		return nullptr;
	}

	if (!MSpaceNameValid(name))
	{
		return nullptr;
	}

	uint64_t aligned_buf_addr = align(addr + MSPACE_HEADER_SIZE, 1);

	auto* ctx = reinterpret_cast<MSpaceContext*>(addr);
	auto* buf = reinterpret_cast<void*>(aligned_buf_addr);

	if (!MSpaceRegister(ctx, addr, addr + capacity, name, buf, (capacity - (aligned_buf_addr - addr)), thread_safe, dbg_callback))
	{
		return nullptr;
	}

	return ctx;
}

bool MSpaceDestroy(mspace_t msp)
{
	if (msp == nullptr)
	{
		return false;
	}

	auto* ctx = static_cast<MSpaceContext*>(msp);

	return MSpaceDestroyRegistered(ctx);
}

void* MSpaceMalloc(mspace_t msp, size_t size)
{
	if (msp == nullptr)
	{
		return nullptr;
	}

	if ((size >> 32u) != 0)
	{
		return nullptr;
	}

	auto* ctx = static_cast<MSpaceContext*>(msp);

	return MSpaceInternalMalloc_align(*ctx, static_cast<uint32_t>(size), 32);
}

bool MSpaceFree(mspace_t msp, void* ptr)
{
	if (msp == nullptr || ptr == nullptr)
	{
		return false;
	}

	auto*           ctx = static_cast<MSpaceContext*>(msp);
	Core::LockGuard registry_lock(g_mspace_registry_mutex);
	const auto      registration =
	    g_mspace_registry.Find(ctx, [](const auto& registered, const auto* value) { return registered.context == value; });
	if (!g_mspace_registry.IndexValid(registration))
	{
		return false;
	}

	MSpaceInternalEnter(*ctx);
	MSpaceAllocation allocation {};
	const bool       valid_allocation = MSpaceInternalGetAllocationHeader(*ctx, reinterpret_cast<uintptr_t>(ptr), &allocation);
	MSpaceInternalLeave(*ctx);
	if (!valid_allocation)
	{
		return false;
	}
	if (MSpaceAllocationIsPinned(ctx, allocation.address))
	{
		return false;
	}

	FOR_LIST(index, g_mspace_registry)
	{
		const auto& registered = g_mspace_registry[index];
		if (registered.parent_context == ctx && registered.parent_allocation == allocation.address)
		{
			return false;
		}
	}

	// MSpaceInternalFree_align acquires the arena after the registry lock.
	MSpaceInternalFree_align(*ctx, ptr);

	return true;
}

void* MSpaceRealloc(mspace_t msp, void* ptr, size_t size)
{
	if (msp == nullptr)
	{
		return nullptr;
	}

	if ((size >> 32u) != 0)
	{
		return nullptr;
	}

	auto* ctx = static_cast<MSpaceContext*>(msp);
	if (ptr == nullptr)
	{
		return MSpaceInternalRealloc_align(*ctx, ptr, static_cast<uint32_t>(size), 32);
	}

	MSpaceReallocPin pin;
	{
		Core::LockGuard registry_lock(g_mspace_registry_mutex);
		const auto      registration =
		    g_mspace_registry.Find(ctx, [](const auto& registered, const auto* value) { return registered.context == value; });
		if (!g_mspace_registry.IndexValid(registration))
		{
			return nullptr;
		}

		MSpaceInternalEnter(*ctx);
		MSpaceAllocation allocation {};
		const bool       valid_allocation = MSpaceInternalGetAllocationHeader(*ctx, reinterpret_cast<uintptr_t>(ptr), &allocation);
		MSpaceInternalLeave(*ctx);
		if (!valid_allocation || MSpaceAllocationIsPinned(ctx, allocation.address))
		{
			return nullptr;
		}

		FOR_LIST(index, g_mspace_registry)
		{
			const auto& registered = g_mspace_registry[index];
			if (registered.parent_context == ctx && registered.parent_allocation == allocation.address)
			{
				return nullptr;
			}
		}

		pin.Add(ctx, allocation.address);
	}

	// The pin blocks the moving chunk without holding the registry lock across the OOM callback.
	return MSpaceInternalRealloc_align(*ctx, ptr, static_cast<uint32_t>(size), 32);
}

void* MSpaceMemalign(mspace_t msp, size_t boundary, size_t size)
{
	if (msp == nullptr)
	{
		return nullptr;
	}

	if ((size >> 32u) != 0)
	{
		return nullptr;
	}

	auto* ctx = static_cast<MSpaceContext*>(msp);

	return MSpaceInternalMalloc_align(*ctx, static_cast<uint32_t>(size), boundary);
}

void* MSpaceCalloc(mspace_t msp, size_t nelem, size_t size)
{
	if (msp == nullptr)
	{
		return nullptr;
	}
	if (nelem != 0 && size > (static_cast<size_t>(-1) / nelem))
	{
		return nullptr;
	}
	const size_t total = nelem * size;
	if ((total >> 32u) != 0)
	{
		return nullptr;
	}
	void* p = MSpaceMalloc(msp, total);
	if (p != nullptr && total != 0)
	{
		std::memset(p, 0, total);
	}
	return p;
}

void* MSpaceAlignedAlloc(mspace_t msp, size_t alignment, size_t size)
{
	return MSpaceMemalign(msp, alignment, size);
}

size_t MSpaceMallocUsableSize(const void* ptr)
{
	if (ptr == nullptr)
	{
		return 0;
	}

	const auto                user = reinterpret_cast<uintptr_t>(ptr);
	Core::LockGuard           registry_lock(g_mspace_registry_mutex);
	const MSpaceRegistration* owner      = nullptr;
	size_t                    owner_size = 0;
	FOR_LIST(index, g_mspace_registry)
	{
		const auto& registered = g_mspace_registry[index];
		if (user < registered.begin || user >= registered.end)
		{
			continue;
		}

		const size_t registered_size = registered.end - registered.begin;
		if (owner == nullptr || registered_size < owner_size)
		{
			owner      = &registered;
			owner_size = registered_size;
		}
	}
	if (owner == nullptr || owner->context == nullptr)
	{
		return 0;
	}

	MSpaceInternalEnter(*owner->context);
	MSpaceAllocation allocation {};
	const bool       owner_has_allocation = MSpaceInternalGetAllocationHeader(*owner->context, user, &allocation);
	MSpaceInternalLeave(*owner->context);
	if (owner_has_allocation)
	{
		return allocation.usable_size;
	}

	// A nested arena begins at its parent's aligned allocation pointer. The
	// child owns ordinary pointers in its range, but this one pointer remains
	// queryable through the parent so callers can later release the backing.
	if (user != owner->parent_backing || owner->parent_context == nullptr)
	{
		return 0;
	}

	MSpaceInternalEnter(*owner->parent_context);
	const bool parent_has_allocation = MSpaceInternalGetAllocationHeader(*owner->parent_context, user, &allocation);
	MSpaceInternalLeave(*owner->parent_context);
	return parent_has_allocation ? allocation.usable_size : 0;
}

static bool MSpaceInternalStats(MSpaceContext& ctx, MSpaceSize* mmsize)
{
	if (mmsize == nullptr)
	{
		return false;
	}

	// free area after the fixed control header: (capacity + 2) blocks of 8 bytes.
	const size_t free_area_bytes =
	    (static_cast<size_t>(ctx.capacity) + 2u) * sizeof(MSpaceBlock);
	const size_t system_size = MSPACE_HEADER_SIZE + free_area_bytes;

	// Largest free key chunk approximates remaining free space when the heap is
	// mostly contiguous (fresh mspace). Fragmentation under-reports free space.
	const size_t free_bytes = static_cast<size_t>(ctx.size_of_key_chunk) * sizeof(MSpaceBlock);
	const size_t inuse_bytes = (free_bytes < free_area_bytes) ? (free_area_bytes - free_bytes) : 0;

	mmsize->max_system_size     = system_size;
	mmsize->current_system_size = system_size;
	mmsize->max_inuse_size      = system_size;
	mmsize->current_inuse_size  = inuse_bytes;
	return true;
}

bool MSpaceMallocStats(mspace_t msp, MSpaceSize* mmsize)
{
	if (msp == nullptr)
	{
		return false;
	}
	auto* ctx = static_cast<MSpaceContext*>(msp);
	MSpaceInternalEnter(*ctx);
	const bool ok = MSpaceInternalStats(*ctx, mmsize);
	MSpaceInternalLeave(*ctx);
	return ok;
}

bool MSpaceMallocStatsFast(mspace_t msp, MSpaceSize* mmsize)
{
	// Same fields as the full walk for this allocator; free-list walk is not required
	// for the managed-size pre-check titles issue before memalign.
	return MSpaceMallocStats(msp, mmsize);
}

bool MSpaceIsHeapEmpty(mspace_t msp)
{
	if (msp == nullptr)
	{
		return true;
	}
	auto* ctx = static_cast<MSpaceContext*>(msp);
	MSpaceInternalEnter(*ctx);
	const bool empty = (ctx->size_of_key_chunk == ctx->capacity && ctx->index_of_key_chunk == 1u);
	MSpaceInternalLeave(*ctx);
	return empty;
}

bool MSpaceIsManaged(mspace_t msp)
{
	if (msp == nullptr)
	{
		return false;
	}
	Core::LockGuard registry_lock(g_mspace_registry_mutex);
	const auto      registration =
	    g_mspace_registry.Find(msp, [](const auto& registered, mspace_t value) { return registered.context == value; });
	return g_mspace_registry.IndexValid(registration);
}

} // namespace Kyty::Core
