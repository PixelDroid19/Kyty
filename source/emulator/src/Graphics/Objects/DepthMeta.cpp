#include "Emulator/Graphics/Objects/DepthMeta.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

constexpr uint32_t kConsumedHistoryCapacity = 64u;
constexpr uint32_t kPendingCapacity         = 64u;

std::mutex                                  g_mutex;
std::unordered_map<uint64_t, DepthMetaClearEvent> g_pending;
std::array<DepthMetaClearEvent, kConsumedHistoryCapacity> g_consumed {};
uint64_t                                    g_next_sequence = 1u;
uint32_t                                    g_consumed_next  = 0u;
uint32_t                                    g_consumed_count = 0u;

bool ObserveStorageWriteWithSource(uint64_t address, const void* data, uint64_t size, DepthMetaClearSource source)
{
	DepthMetaPatternSnapshot pattern {};
	if (address == 0 || !DepthMetaInspectPattern(data, size, &pattern) ||
	    pattern.kind != DepthMetaPatternKind::RecognizedClear)
	{
		return false;
	}
	DepthMetaMarkClear(address, source, &pattern, size);
	return true;
}

void RetainPendingBounded(const DepthMetaClearEvent& event)
{
	if (g_pending.find(event.address) == g_pending.end() && g_pending.size() >= kPendingCapacity)
	{
		const auto oldest = std::min_element(g_pending.begin(), g_pending.end(), [](const auto& lhs, const auto& rhs)
		                                     { return lhs.second.sequence < rhs.second.sequence; });
		if (oldest != g_pending.end())
		{
			g_pending.erase(oldest);
		}
	}
	g_pending[event.address] = event;
}

bool ConsumePending(std::unordered_map<uint64_t, DepthMetaClearEvent>::iterator match, DepthMetaClearEvent* event)
{
	const auto consumed = match->second;
	g_pending.erase(match);
	g_consumed[g_consumed_next] = consumed;
	g_consumed_next = (g_consumed_next + 1u) % kConsumedHistoryCapacity;
	g_consumed_count = std::min(g_consumed_count + 1u, kConsumedHistoryCapacity);
	if (event != nullptr)
	{
		*event = consumed;
	}
	return true;
}

bool ComputeFillIdentityMatches(const DepthMetaClearEvent& pending, const DepthMetaStorageIdentity& identity)
{
	return pending.source == DepthMetaClearSource::ComputeMetadataFill && pending.size == identity.size &&
	       pending.logical_generation == identity.logical_generation &&
	       pending.backing_generation == identity.backing_generation &&
	       pending.producer_submit <= identity.producer_or_consumer_submit;
}

} // namespace

bool DepthMetaIsClearPattern(const void* data, uint64_t size)
{
	DepthMetaPatternSnapshot snapshot {};
	return DepthMetaInspectPattern(data, size, &snapshot) && snapshot.kind == DepthMetaPatternKind::RecognizedClear;
}

bool DepthMetaInspectPattern(const void* data, uint64_t size, DepthMetaPatternSnapshot* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = {};
	if (data == nullptr || size == 0 || (size % sizeof(uint32_t)) != 0)
	{
		return false;
	}

	const auto* words = static_cast<const uint32_t*>(data);
	const auto  count = size / sizeof(uint32_t);
	const auto  first = words[0];
	out->first_word = first;
	out->word_count = count;
	for (uint64_t i = 1; i < count; i++)
	{
		if (words[i] != first)
		{
			out->kind = DepthMetaPatternKind::Mixed;
			return true;
		}
	}
	out->kind = first == 0u          ? DepthMetaPatternKind::UniformZero
	            : first == 0xfffffff0u ? DepthMetaPatternKind::RecognizedClear
	                                   : DepthMetaPatternKind::UniformOther;
	return true;
}

bool DepthMetaMatchesStorageRange(uint64_t storage_address, uint64_t storage_size, uint64_t htile_address, uint64_t htile_size)
{
	return storage_address != 0 && htile_address != 0 && storage_size != 0 && storage_address == htile_address &&
	       storage_size == htile_size;
}

bool DepthMetaObserveStorageWrite(uint64_t address, const void* data, uint64_t size)
{
	return ObserveStorageWriteWithSource(address, data, size, DepthMetaClearSource::StorageUpload);
}

bool DepthMetaObserveStorageFlush(uint64_t storage_address, uint64_t storage_size, uint64_t htile_address, uint64_t htile_size,
                                  const void* storage_data, uint64_t flush_address, uint64_t flush_size)
{
	const bool valid_flush = flush_size != 0 && flush_address <= UINT64_MAX - flush_size;
	const bool valid_storage = storage_size != 0 && storage_address <= UINT64_MAX - storage_size;
	const bool overlaps = valid_flush && valid_storage && flush_address < storage_address + storage_size &&
	                      storage_address < flush_address + flush_size;
	if (!DepthMetaMatchesStorageRange(storage_address, storage_size, htile_address, htile_size) || !overlaps)
	{
		return false;
	}
	return ObserveStorageWriteWithSource(htile_address, storage_data, storage_size, DepthMetaClearSource::StorageFlush);
}

void DepthMetaMarkClear(uint64_t address, DepthMetaClearSource source, const DepthMetaPatternSnapshot* pattern, uint64_t size)
{
	if (address == 0)
	{
		return;
	}
	std::lock_guard lock(g_mutex);
	DepthMetaClearEvent event {};
	event.valid    = true;
	event.address  = address;
	event.size     = size;
	event.sequence = g_next_sequence++;
	event.source   = source;
	if (pattern != nullptr)
	{
		event.pattern = *pattern;
	}
	RetainPendingBounded(event);
}

bool DepthMetaPublishComputeFill(const DepthMetaStorageIdentity& identity, uint32_t fill_word)
{
	if (identity.address == 0 || identity.size == 0 || (identity.size % sizeof(uint32_t)) != 0 ||
	    identity.logical_generation == 0 || identity.backing_generation == 0)
	{
		return false;
	}
	std::lock_guard lock(g_mutex);
	DepthMetaClearEvent event {};
	event.valid              = true;
	event.address            = identity.address;
	event.size               = identity.size;
	event.sequence           = g_next_sequence++;
	event.logical_generation = identity.logical_generation;
	event.backing_generation = identity.backing_generation;
	event.producer_submit    = identity.producer_or_consumer_submit;
	event.source             = DepthMetaClearSource::ComputeMetadataFill;
	event.pattern.kind       = fill_word == 0u ? DepthMetaPatternKind::UniformZero : DepthMetaPatternKind::UniformOther;
	event.pattern.first_word = fill_word;
	event.pattern.word_count = identity.size / sizeof(uint32_t);
	RetainPendingBounded(event);
	return true;
}

bool DepthMetaConsumeClear(uint64_t address, DepthMetaClearEvent* event)
{
	std::lock_guard lock(g_mutex);
	const auto match = g_pending.find(address);
	if (match == g_pending.end())
	{
		if (event != nullptr)
		{
			*event = {};
		}
		return false;
	}
	return ConsumePending(match, event);
}

bool DepthMetaConsumeClear(const DepthMetaStorageIdentity& identity, DepthMetaClearEvent* event)
{
	if (identity.address == 0 || identity.size == 0)
	{
		if (event != nullptr)
		{
			*event = {};
		}
		return false;
	}
	std::lock_guard lock(g_mutex);
	const auto match = g_pending.find(identity.address);
	if (match == g_pending.end())
	{
		if (event != nullptr)
		{
			*event = {};
		}
		return false;
	}

	const auto& pending = match->second;
	const bool compute_identity = pending.source != DepthMetaClearSource::ComputeMetadataFill ||
	                              ComputeFillIdentityMatches(pending, identity);
	const bool legacy_range = pending.source == DepthMetaClearSource::ComputeMetadataFill || pending.size == 0 ||
	                          pending.size == identity.size;
	if (!compute_identity || !legacy_range)
	{
		// The same address now names a different range/incarnation. Do not leave a
		// stale clear available for a later ABA match.
		g_pending.erase(match);
		if (event != nullptr)
		{
			*event = {};
		}
		return false;
	}
	return ConsumePending(match, event);
}

bool DepthMetaDiscardComputeFill(const DepthMetaStorageIdentity& identity, uint64_t expected_sequence)
{
	if (identity.address == 0 || identity.size == 0 || expected_sequence == 0)
	{
		return false;
	}
	std::lock_guard lock(g_mutex);
	const auto match = g_pending.find(identity.address);
	if (match == g_pending.end() || match->second.sequence != expected_sequence ||
	    !ComputeFillIdentityMatches(match->second, identity))
	{
		return false;
	}
	g_pending.erase(match);
	return true;
}

bool DepthMetaQueryTraceState(uint64_t address, DepthMetaTraceSnapshot* out)
{
	if (out == nullptr || address == 0)
	{
		return false;
	}
	*out = {};
	std::lock_guard lock(g_mutex);
	if (const auto pending = g_pending.find(address); pending != g_pending.end())
	{
		out->pending       = true;
		out->pending_event = pending->second;
	}
	for (uint32_t age = 0; age < g_consumed_count; ++age)
	{
		const uint32_t index = (g_consumed_next + kConsumedHistoryCapacity - 1u - age) % kConsumedHistoryCapacity;
		if (g_consumed[index].valid && g_consumed[index].address == address)
		{
			out->has_last_consumed = true;
			out->last_consumed     = g_consumed[index];
			break;
		}
	}
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
