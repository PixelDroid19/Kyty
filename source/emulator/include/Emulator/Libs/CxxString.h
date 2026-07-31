#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_CXX_STRING_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_CXX_STRING_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

#include <cstddef>
#include <cstdint>

namespace Kyty::Libs::LibC {

struct alignas(8) CxxStringLayout
{
	std::uint64_t allocator_state;
	union
	{
		char inline_data[16];
		struct
		{
			char*         data;
			std::uint64_t reserved;
		} allocated;
	} storage;
	std::uint64_t size;
	std::uint64_t capacity;
};

static_assert(offsetof(CxxStringLayout, storage) == 0x08);
static_assert(offsetof(CxxStringLayout, size) == 0x18);
static_assert(offsetof(CxxStringLayout, capacity) == 0x20);
static_assert(sizeof(CxxStringLayout) == 0x28);

[[nodiscard]] inline const char* CxxStringData(const CxxStringLayout& value)
{
	return value.capacity < sizeof(value.storage.inline_data) ? value.storage.inline_data : value.storage.allocated.data;
}

} // namespace Kyty::Libs::LibC

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_CXX_STRING_H_ */
