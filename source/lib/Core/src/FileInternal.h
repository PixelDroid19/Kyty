#ifndef INCLUDE_KYTY_CORE_FILEINTERNAL_H_
#define INCLUDE_KYTY_CORE_FILEINTERNAL_H_

#include <cstdint>
#include <limits>

namespace Kyty::Core::FileInternal {

[[nodiscard]] constexpr bool CanReadWholeStringBuffer(uint64_t file_size, uint32_t bytes_per_character) noexcept
{
	if (bytes_per_character == 0u)
	{
		return false;
	}

	const uint64_t character_count = file_size / bytes_per_character;
	const uint64_t read_size       = character_count * bytes_per_character;
	return character_count < std::numeric_limits<uint32_t>::max() && read_size <= std::numeric_limits<uint32_t>::max();
}

} // namespace Kyty::Core::FileInternal

#endif // INCLUDE_KYTY_CORE_FILEINTERNAL_H_
