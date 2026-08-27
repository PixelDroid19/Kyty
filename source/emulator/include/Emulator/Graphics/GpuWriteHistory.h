#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUWRITEHISTORY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUWRITEHISTORY_H_

#include <cstdint>

namespace Kyty::Libs::Graphics {

enum class GpuWriteHistoryKind: uint32_t
{
	DmaData,
	DmaDataCustom,
	WriteData,
	ConstRamDump,
	EventWrite,
	StorageWriteBack,
	RenderTargetWriteBack,
	OtherWriteBack,
	Count,
};

struct GpuWriteHistoryEvent
{
	uint64_t sequence         = 0;
	uint64_t guest_addr       = 0;
	uint64_t size             = 0;
	uint64_t submit_id        = 0;
	uint64_t content_sequence = 0;
	uint32_t kind             = 0;
	uint32_t object_type      = 0;
};

struct GpuWriteHistorySnapshot
{
	static constexpr uint32_t ENTRIES_MAX = 16;
	static constexpr uint32_t KINDS_MAX   = static_cast<uint32_t>(GpuWriteHistoryKind::Count);

	bool                 enabled = false;
	bool                 capture_all = false;
	uint64_t             watched_addr = 0;
	uint64_t             watched_size = 0;
	uint64_t             retained = 0;
	uint64_t             dropped = 0;
	uint64_t             total_by_kind[KINDS_MAX] = {};
	GpuWriteHistoryEvent entries[ENTRIES_MAX] = {};
	uint32_t             entry_count = 0;
	uint32_t             matching_count = 0;
	bool                 entries_truncated = false;
	bool                 covers_query = false;
};

void GpuWriteHistoryRecord(GpuWriteHistoryKind kind, uint64_t guest_addr, uint64_t size, uint64_t submit_id,
	                       uint32_t object_type, uint64_t content_sequence);
bool GpuWriteHistoryQuery(uint64_t guest_addr, uint64_t size, GpuWriteHistorySnapshot* out);

// Integration-only configuration. A zero range disables and clears the recorder.
void GpuWriteHistoryConfigureForTesting(uint64_t guest_addr, uint64_t size, bool capture_all = false);

} // namespace Kyty::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUWRITEHISTORY_H_
