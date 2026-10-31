#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUESTDEVICEADDRESS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUESTDEVICEADDRESS_H_

#include "Emulator/Common.h"
#include "Emulator/Graphics/GpuSubmissionTracker.h"

#include <cstdint>
#include <vector>

struct VkCommandBuffer_T;

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct GraphicContext;
struct VulkanBuffer;
struct GuestInPlaceWindow;
struct GpuMemoryStoragePublication;

// Guest GPU-visible mappings imported as host-pointer device memory, so a
// shader can dereference a computed guest address. Guest and host virtual
// addresses are identical; a device-resident table translates guest ranges to
// device addresses. The table starts with kGuestDeviceAddressNullBytes of
// zeros, the target of any unmapped guest address, followed by entries of
// eight dwords:
//   {guest_base_lo, guest_base_hi, size_lo, size_hi, device_lo, device_hi, span_lo, span_hi}
// `size` is the guest byte count and `span` the imported byte count. Chunks
// cover whole pages; translated multiword loads resolve each page separately.
// A miss reads at most 128 bytes of the zero prefix, so the prefix also holds,
// at kGuestDeviceAddressDirectoryHeaderOffset, the directory header
//   {base_lo, base_hi, shift, last_region}
// and the directory words follow the entries (GuestDeviceAddressBuildDirectory).
constexpr uint32_t kGuestDeviceAddressEntryDwords           = 8;
constexpr uint32_t kGuestDeviceAddressNullBytes             = 256;
constexpr uint32_t kGuestDeviceAddressPageBytes             = 4096;
constexpr uint32_t kGuestDeviceAddressDirectoryHeaderOffset = 128;

// Bounds the device binary search to the entries of one region of guest space:
// regions are 2^shift bytes from `base`, and an address past the last region
// belongs to it. words[r] counts the entries whose guest base is at most the
// start of region r; the final word is the entry count. Entries are disjoint
// and sorted, so for an address in region r the count of entries whose base is
// at most the address, which the search returns, lies in [words[r], words[r + 1]].
struct GuestDeviceAddressDirectory
{
	uint64_t              base  = 0;
	uint32_t              shift = 0;
	std::vector<uint32_t> words;
};

[[nodiscard]] GuestDeviceAddressDirectory GuestDeviceAddressBuildDirectory(const uint64_t* sorted_bases, uint32_t count);

struct GuestDeviceAddressTable
{
	uint64_t address     = 0;
	uint32_t entry_count = 0;
};

enum class GuestDeviceAddressPrepareResult: uint8_t
{
	Ready,
	PendingReader,
	Failed,
};

// A load of `bytes` at `offset` into a chunk matches only when it starts in
// the guest `size` and ends inside the imported `span`. An access that would
// walk off the allocation misses, instead of faulting the device.
[[nodiscard]] constexpr bool GuestDeviceAddressAccessFits(uint64_t size, uint64_t span, uint64_t offset, uint64_t bytes)
{
	if (bytes == 0 || offset >= size || offset >= span || bytes > span - offset)
	{
		return false;
	}
	return true;
}

// Bookkeeping only; safe to call from the kernel mapping path. A range that
// views the kernel's physical backing is rescanned for newly resident pages
// only when that backing's population changed; any other range on every
// preparation. Untouched pages read as zero, like the table's null target.
void GuestDeviceAddressRegisterRange(uint64_t vaddr, uint64_t size, bool physical_backing);

// Must run with GPU submissions quiesced: drops the imports overlapping the
// range (its protection or contents may have changed); they are imported
// again on the next use. The range stays registered.
void GuestDeviceAddressInvalidateRangeQuiesced(GraphicContext* ctx, uint64_t vaddr, uint64_t size);

// Must run with GPU submissions quiesced: destroys imports of the range and
// every table still retained in the retirement list.
void GuestDeviceAddressReleaseRangeQuiesced(GraphicContext* ctx, uint64_t vaddr, uint64_t size);

// Writes back GPU results held in Kyty storage-buffer objects over every
// registered range, so a shader dereferencing guest pointers reads the
// current guest memory. Call before recording such a dispatch on `consumer`.
// Results already published to the imported view on that queue are skipped.
void GuestDeviceAddressWriteBack(GraphicContext* ctx, GpuQueueId consumer);

// True when a registered range still waits for GPU results held outside storage
// buffers (GpuMemoryPendingWriteBackOutsideStorage). Storage results reach
// device-address readers through GuestDeviceAddressPendingWriteBack; these need
// the processor write-back before the reader is recorded.
[[nodiscard]] bool GuestDeviceAddressPendingWriteBackOutsideStorage();

// Creates `out` directly over the guest pages of [vaddr, vaddr + size), so device
// writes reach guest memory without a copy and CPU stores reach the device. False:
// the range is not one bindable run of direct memory, and `out` stays untouched.
[[nodiscard]] bool GuestDeviceAddressBindInPlace(GraphicContext* ctx, uint64_t vaddr, uint64_t size, uint32_t usage,
                                                 VulkanBuffer* out, GuestInPlaceWindow* window);
void               GuestDeviceAddressReleaseInPlace(GraphicContext* ctx, const GuestInPlaceWindow& window);

// The caller holds the render recording lock through the later write-back.
// Wait for a returned dependency only after releasing recording locks, then retry.
// With `publishable`, results the caller can publish in the consumer's queue order
// (GuestDeviceAddressRecordPublication) are listed there instead of reported;
// `device_awaited` names the other queues' submissions the consumer's recording
// waits for on the device (GpuMemoryPendingStorageWriteBack).
[[nodiscard]] bool GuestDeviceAddressPendingWriteBack(GpuQueueId consumer, const std::vector<SubmissionId>& device_awaited,
                                                      std::vector<GpuMemoryStoragePublication>* publishable, SubmissionId* dependency);

// Records into `cmd`, after the GPU work already recorded there, a copy of
// `source` (whose first byte is guest address `vaddr`, `size` bytes) into the
// imported view of that guest range, between barriers ordering it after every
// earlier command and before every later one in the queue. Device-address
// reads recorded later in the queue then observe the result in GPU order, as
// the guest does on unified memory. Only ranges wholly inside chunks that alias
// the guest's physical memory qualify: a snapshot chunk could be refreshed
// from stale guest bytes before the CPU write-back.
[[nodiscard]] bool GuestDeviceAddressRecordPublication(GraphicContext* ctx, VkCommandBuffer_T* cmd, const VulkanBuffer& source,
                                                       uint64_t vaddr, uint64_t size);

// Records, with the same ordering and range rules as GuestDeviceAddressRecordPublication,
// `word` repeated over the imported view of `size` bytes at guest address `vaddr`.
// Fails without recording when a piece of the range is not 4-byte aligned.
[[nodiscard]] bool GuestDeviceAddressRecordFill(GraphicContext* ctx, VkCommandBuffer_T* cmd, uint32_t word, uint64_t vaddr, uint64_t size);

// Imports all registered ranges not imported yet and returns the translation
// table. This legacy synchronous helper is for quiesced unit-test setup; runtime
// GPU readers must use GuestDeviceAddressPrepareForRead to track their lifetime.
[[nodiscard]] bool GuestDeviceAddressPrepare(GraphicContext* ctx, uint64_t* table_address, uint32_t* entry_count);

// Prepares the translation table for a command buffer that will read through
// it. If a tracked snapshot refresh or table rebuild must wait for an earlier
// reader, returns that reader without publishing a table. The caller must wait
// outside recording locks and acknowledge the exact completed ID.
[[nodiscard]] GuestDeviceAddressPrepareResult GuestDeviceAddressPrepareForRead(GraphicContext* ctx, SubmissionId reader,
	                                                                          GuestDeviceAddressTable* table,
	                                                                          SubmissionId* dependency);
[[nodiscard]] bool GuestDeviceAddressAcknowledgeReader(SubmissionId completed);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUESTDEVICEADDRESS_H_
