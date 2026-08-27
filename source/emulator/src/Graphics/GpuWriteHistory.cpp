#include "Emulator/Graphics/GpuWriteHistory.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

namespace Kyty::Libs::Graphics {
namespace {

constexpr uint32_t kExactRingCapacity = 128;
constexpr uint32_t kAutoRingCapacity  = 65536;

struct State
{
	std::mutex                                      mutex;
	std::array<GpuWriteHistoryEvent, kExactRingCapacity> exact_ring {};
	std::unique_ptr<GpuWriteHistoryEvent[]>              auto_ring;
	uint64_t                                        watched_addr = 0;
	uint64_t                                        watched_size = 0;
	uint64_t                                        next_sequence = 1;
	uint64_t                                        retained = 0;
	uint64_t                                        dropped = 0;
	uint64_t                                        total_by_kind[GpuWriteHistorySnapshot::KINDS_MAX] = {};
	uint32_t                                        head = 0;
	uint32_t                                        capacity = kExactRingCapacity;
	bool                                            enabled = false;
	bool                                            capture_all = false;
	bool                                            initialized = false;

	GpuWriteHistoryEvent* Ring() { return capture_all ? auto_ring.get() : exact_ring.data(); }
};

State g_state;
std::atomic<int> g_fast_state {-1};

bool ValidRange(uint64_t addr, uint64_t size)
{
	return addr != 0 && size != 0 && addr <= UINT64_MAX - size;
}

bool Overlaps(uint64_t left_addr, uint64_t left_size, uint64_t right_addr, uint64_t right_size)
{
	return ValidRange(left_addr, left_size) && ValidRange(right_addr, right_size) &&
	       left_addr < right_addr + right_size && right_addr < left_addr + left_size;
}

bool ParseRange(const char* value, uint64_t* addr, uint64_t* size)
{
	if (value == nullptr || addr == nullptr || size == nullptr)
	{
		return false;
	}
	const char* colon = std::strchr(value, ':');
	if (colon == nullptr || colon == value || colon[1] == '\0')
	{
		return false;
	}
	errno       = 0;
	char* end   = nullptr;
	const auto a = std::strtoull(value, &end, 0);
	if (errno != 0 || end != colon)
	{
		return false;
	}
	errno        = 0;
	const auto n = std::strtoull(colon + 1, &end, 0);
	if (errno != 0 || end == colon + 1 || *end != '\0' || !ValidRange(a, n))
	{
		return false;
	}
	*addr = a;
	*size = n;
	return true;
}

void InitializeLocked()
{
	if (g_state.initialized)
	{
		return;
	}
	g_state.initialized = true;
	const char* value = std::getenv("KYTY_TRACE_GPU_WRITER_RANGE");
	if (value != nullptr && std::strcmp(value, "auto") == 0)
	{
		g_state.auto_ring.reset(new (std::nothrow) GpuWriteHistoryEvent[kAutoRingCapacity]);
		if (g_state.auto_ring != nullptr)
		{
			g_state.enabled     = true;
			g_state.capture_all = true;
			g_state.capacity    = kAutoRingCapacity;
		}
	} else
	{
		uint64_t addr = 0;
		uint64_t size = 0;
		if (ParseRange(value, &addr, &size))
		{
			g_state.enabled      = true;
			g_state.watched_addr = addr;
			g_state.watched_size = size;
		}
	}
	g_fast_state.store(g_state.enabled ? 1 : 0, std::memory_order_release);
}

} // namespace

void GpuWriteHistoryRecord(GpuWriteHistoryKind kind, uint64_t guest_addr, uint64_t size, uint64_t submit_id,
	                       uint32_t object_type, uint64_t content_sequence)
{
	const auto kind_index = static_cast<uint32_t>(kind);
	if (kind_index >= GpuWriteHistorySnapshot::KINDS_MAX || !ValidRange(guest_addr, size))
	{
		return;
	}
	if (g_fast_state.load(std::memory_order_acquire) == 0)
	{
		return;
	}
	std::lock_guard lock(g_state.mutex);
	InitializeLocked();
	if (!g_state.enabled)
	{
		return;
	}
	g_state.total_by_kind[kind_index]++;
	if (!g_state.capture_all && !Overlaps(g_state.watched_addr, g_state.watched_size, guest_addr, size))
	{
		return;
	}
	GpuWriteHistoryEvent event {};
	event.sequence         = g_state.next_sequence++;
	event.guest_addr       = guest_addr;
	event.size             = size;
	event.submit_id        = submit_id;
	event.content_sequence = content_sequence;
	event.kind             = kind_index;
	event.object_type      = object_type;
	auto* ring = g_state.Ring();
	if (ring == nullptr)
	{
		return;
	}
	ring[g_state.head] = event;
	g_state.head = (g_state.head + 1u) % g_state.capacity;
	if (g_state.retained < g_state.capacity)
	{
		g_state.retained++;
	} else
	{
		g_state.dropped++;
	}
}

bool GpuWriteHistoryQuery(uint64_t guest_addr, uint64_t size, GpuWriteHistorySnapshot* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = {};
	std::lock_guard lock(g_state.mutex);
	InitializeLocked();
	out->enabled      = g_state.enabled;
	out->capture_all  = g_state.capture_all;
	out->watched_addr = g_state.watched_addr;
	out->watched_size = g_state.watched_size;
	out->retained     = g_state.retained;
	out->dropped      = g_state.dropped;
	for (uint32_t i = 0; i < GpuWriteHistorySnapshot::KINDS_MAX; ++i)
	{
		out->total_by_kind[i] = g_state.total_by_kind[i];
	}
	if (!g_state.enabled || !ValidRange(guest_addr, size))
	{
		return true;
	}
	out->covers_query = g_state.capture_all ||
	                    (g_state.watched_addr <= guest_addr &&
	                     g_state.watched_addr + g_state.watched_size >= guest_addr + size);
	const uint32_t retained = static_cast<uint32_t>(g_state.retained);
	const uint32_t oldest   = (g_state.head + g_state.capacity - retained) % g_state.capacity;
	const auto*    ring     = g_state.Ring();
	if (ring == nullptr)
	{
		return true;
	}
	for (uint32_t i = 0; i < retained; ++i)
	{
		const auto& event = ring[(oldest + i) % g_state.capacity];
		if (!Overlaps(event.guest_addr, event.size, guest_addr, size))
		{
			continue;
		}
		out->matching_count++;
		if (out->entry_count == GpuWriteHistorySnapshot::ENTRIES_MAX)
		{
			for (uint32_t e = 1; e < out->entry_count; ++e)
			{
				out->entries[e - 1u] = out->entries[e];
			}
			out->entry_count--;
		}
		out->entries[out->entry_count++] = event;
	}
	out->entries_truncated = out->matching_count > out->entry_count;
	return true;
}

void GpuWriteHistoryConfigureForTesting(uint64_t guest_addr, uint64_t size, bool capture_all)
{
	std::lock_guard lock(g_state.mutex);
	if (capture_all && g_state.auto_ring == nullptr)
	{
		g_state.auto_ring.reset(new (std::nothrow) GpuWriteHistoryEvent[kAutoRingCapacity]);
	}
	g_state.watched_addr     = guest_addr;
	g_state.watched_size     = size;
	g_state.next_sequence    = 1;
	g_state.retained         = 0;
	g_state.dropped          = 0;
	g_state.head             = 0;
	g_state.capture_all      = capture_all && g_state.auto_ring != nullptr;
	g_state.capacity         = g_state.capture_all ? kAutoRingCapacity : kExactRingCapacity;
	g_state.enabled          = g_state.capture_all || ValidRange(guest_addr, size);
	g_state.initialized      = true;
	g_fast_state.store(g_state.enabled ? 1 : 0, std::memory_order_release);
	for (auto& total: g_state.total_by_kind)
	{
		total = 0;
	}
}

} // namespace Kyty::Libs::Graphics
