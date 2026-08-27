#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_DEPTH_META_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_DEPTH_META_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

enum class DepthMetaPatternKind: uint32_t
{
	Invalid,
	UniformZero,
	RecognizedClear,
	UniformOther,
	Mixed,
};

struct DepthMetaPatternSnapshot
{
	DepthMetaPatternKind kind       = DepthMetaPatternKind::Invalid;
	uint32_t             first_word = 0;
	uint64_t             word_count = 0;
};

enum class DepthMetaClearSource: uint32_t
{
	Unknown,
	SyntheticImageCreate,
	StorageUpload,
	StorageFlush,
	StorageWriteback,
	ComputeMetadataFill,
};

struct DepthMetaStorageIdentity
{
	uint64_t address                = 0;
	uint64_t size                   = 0;
	uint64_t logical_generation     = 0;
	uint64_t backing_generation     = 0;
	uint64_t producer_or_consumer_submit = 0;
};

struct DepthMetaClearEvent
{
	bool                     valid              = false;
	uint64_t                 address            = 0;
	uint64_t                 size               = 0;
	uint64_t                 sequence           = 0;
	uint64_t                 logical_generation = 0;
	uint64_t                 backing_generation = 0;
	uint64_t                 producer_submit    = 0;
	DepthMetaClearSource     source             = DepthMetaClearSource::Unknown;
	DepthMetaPatternSnapshot pattern {};
};

struct DepthMetaTraceSnapshot
{
	bool                pending = false;
	DepthMetaClearEvent pending_event {};
	bool                has_last_consumed = false;
	DepthMetaClearEvent last_consumed {};
};

// Observed HTILE clear pattern owner: exact-range pending clears only.
// Mark inserts a pending clear; ConsumeClear returns true once then removes it.
// No invented addresses; zero/partial ranges never match.

[[nodiscard]] bool DepthMetaIsClearPattern(const void* data, uint64_t size);
[[nodiscard]] bool DepthMetaInspectPattern(const void* data, uint64_t size, DepthMetaPatternSnapshot* out);
[[nodiscard]] bool DepthMetaMatchesStorageRange(uint64_t storage_address, uint64_t storage_size, uint64_t htile_address,
                                                uint64_t htile_size);
// HTILE clear is an event, not only a byte transition. Call these only from
// observed write seams: a later guest write of the same clear pattern must
// republish the event after the prior mark was consumed, while ordinary
// unchanged-buffer reuse must not.
[[nodiscard]] bool DepthMetaObserveStorageWrite(uint64_t address, const void* data, uint64_t size);
[[nodiscard]] bool DepthMetaObserveStorageFlush(uint64_t storage_address, uint64_t storage_size, uint64_t htile_address,
                                                uint64_t htile_size, const void* storage_data, uint64_t flush_address,
                                                uint64_t flush_size);
void               DepthMetaMarkClear(uint64_t address, DepthMetaClearSource source = DepthMetaClearSource::Unknown,
	                                  const DepthMetaPatternSnapshot* pattern = nullptr, uint64_t size = 0);
// A compute fill is retained as a candidate until an exact HTILE range backed
// by the same live storage incarnation consumes it. The fill word is recorded;
// the depth-format consumer remains responsible for accepting its semantics.
[[nodiscard]] bool DepthMetaPublishComputeFill(const DepthMetaStorageIdentity& identity, uint32_t fill_word);
[[nodiscard]] bool DepthMetaConsumeClear(uint64_t address, DepthMetaClearEvent* event = nullptr);
[[nodiscard]] bool DepthMetaConsumeClear(const DepthMetaStorageIdentity& identity, DepthMetaClearEvent* event = nullptr);
[[nodiscard]] bool DepthMetaDiscardComputeFill(const DepthMetaStorageIdentity& identity, uint64_t expected_sequence);
[[nodiscard]] bool DepthMetaQueryTraceState(uint64_t address, DepthMetaTraceSnapshot* out);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_DEPTH_META_H_
