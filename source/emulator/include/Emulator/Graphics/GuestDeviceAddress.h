#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUESTDEVICEADDRESS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUESTDEVICEADDRESS_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct GraphicContext;

// Guest GPU-visible mappings imported as host-pointer device memory, so a
// shader can dereference a computed guest address. Guest and host virtual
// addresses are identical; a device-resident table translates guest ranges to
// device addresses. The table starts with kGuestDeviceAddressNullBytes of
// zeros, the target of any unmapped guest address, followed by entries of
// eight dwords:
//   {guest_base_lo, guest_base_hi, size_lo, size_hi, device_lo, device_hi, span_lo, span_hi}
// `size` is the guest byte count. `span` is the imported byte count: `size`,
// plus one page when the backing continues, so a load may finish in that page.
constexpr uint32_t kGuestDeviceAddressEntryDwords = 8;
constexpr uint32_t kGuestDeviceAddressNullBytes   = 256;

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

// Bookkeeping only; safe to call from the kernel mapping path.
void GuestDeviceAddressRegisterRange(uint64_t vaddr, uint64_t size);

// Must run with GPU submissions quiesced: drops the imports overlapping the
// range (its protection or contents may have changed); they are imported
// again on the next use. The range stays registered.
void GuestDeviceAddressInvalidateRangeQuiesced(GraphicContext* ctx, uint64_t vaddr, uint64_t size);

// Must run with GPU submissions quiesced: destroys imports of the range and
// every table retired since the last quiesced release.
void GuestDeviceAddressReleaseRangeQuiesced(GraphicContext* ctx, uint64_t vaddr, uint64_t size);

// Writes back GPU results held in Kyty storage-buffer objects over every
// registered range, so a shader dereferencing guest pointers reads the
// current guest memory. Call before recording such a dispatch.
void GuestDeviceAddressWriteBack(GraphicContext* ctx);

// Imports all registered ranges not imported yet and returns the translation
// table. Fails when the device cannot import guest memory.
[[nodiscard]] bool GuestDeviceAddressPrepare(GraphicContext* ctx, uint64_t* table_address, uint32_t* entry_count);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUESTDEVICEADDRESS_H_
