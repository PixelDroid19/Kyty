#ifndef INCLUDE_KYTY_CORE_MEMORYALLOC_H_
#define INCLUDE_KYTY_CORE_MEMORYALLOC_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"

#include <atomic>

namespace Kyty::Core {

namespace MemoryAllocDetail {

class ThreadDomainRegistry
{
public:
	static constexpr size_t capacity = 64;

	bool Add(uint64_t thread_token)
	{
		if (thread_token == 0)
		{
			return false;
		}
		if (Contains(thread_token))
		{
			return true;
		}
		for (auto& slot: m_slots)
		{
			uint64_t empty = 0;
			if (slot.compare_exchange_strong(empty, thread_token, std::memory_order_acq_rel))
			{
				return true;
			}
		}
		return false;
	}

	bool Remove(uint64_t thread_token)
	{
		for (auto& slot: m_slots)
		{
			uint64_t expected = thread_token;
			if (slot.compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
			{
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] bool Contains(uint64_t thread_token) const
	{
		for (const auto& slot: m_slots)
		{
			if (slot.load(std::memory_order_acquire) == thread_token)
			{
				return true;
			}
		}
		return false;
	}

private:
	std::atomic<uint64_t> m_slots[capacity] {};
};

// Tracks recursive allocator entry with the complete native thread token. The
// state is allocation-free so the memory tracker can identify its own metadata
// allocations without consulting C++ TLS or indexing a collision-prone table.
class RecursionState
{
public:
	[[nodiscard]] bool IsOwnedBy(uint64_t thread_token) const
	{
		return thread_token != 0 && m_owner.load(std::memory_order_acquire) == thread_token;
	}

	void Enter(uint64_t thread_token)
	{
		EXIT_IF(thread_token == 0);

		const auto depth = m_depth.load(std::memory_order_relaxed);
		EXIT_IF(depth != 0 && !IsOwnedBy(thread_token));
		if (depth == 0)
		{
			m_owner.store(thread_token, std::memory_order_release);
		}
		m_depth.store(depth + 1, std::memory_order_relaxed);
	}

	void Leave(uint64_t thread_token)
	{
		EXIT_IF(!IsOwnedBy(thread_token));

		const auto depth = m_depth.load(std::memory_order_relaxed);
		EXIT_IF(depth == 0);
		m_depth.store(depth - 1, std::memory_order_relaxed);
		if (depth == 1)
		{
			m_owner.store(0, std::memory_order_release);
		}
	}

	[[nodiscard]] uint32_t Depth() const { return m_depth.load(std::memory_order_relaxed); }

private:
	std::atomic<uint64_t> m_owner {0};
	std::atomic<uint32_t> m_depth {0};
};

} // namespace MemoryAllocDetail

// SUBSYSTEM_DEFINE(Memory);
void core_memory_init();

struct MemStats
{
	int      state;
	size_t   total_allocated;
	uint32_t blocks_num;
};

void* mem_alloc(size_t size);
void* mem_realloc(void* ptr, size_t size);
void  mem_free(void* ptr);
void  mem_print(int from_state);
void  mem_get_stat(MemStats* s);
void  mem_set_max_size(size_t size);
int   mem_new_state();
bool  mem_tracker_enabled();
void  mem_tracker_enable();
void  mem_tracker_disable();
bool  mem_check(const void* ptr);
void  mem_guest_thread_enter();
void  mem_guest_thread_leave();
bool  mem_guest_thread_is_active();

#define KYTY_MEM_CHECK(ptr) EXIT_IF(!mem_check(ptr))

} // namespace Kyty::Core

#endif /* INCLUDE_KYTY_CORE_MEMORYALLOC_H_ */
