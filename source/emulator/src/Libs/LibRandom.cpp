#include "Kyty/Core/Common.h"
#include "Kyty/Math/Rand.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cinttypes>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Random", 1, "Random", 1, 1);

namespace Random {

constexpr int RANDOM_ERROR_INVALID = static_cast<int>(0x817c0016);
constexpr int MAX_RANDOM_BYTES     = 64;

static int KYTY_SYSV_ABI RandomGetRandomNumber(void* destination, size_t size)
{
	PRINT_NAME();
	printf("\t destination = 0x%016" PRIx64 " size = %" PRIu64 "\n", reinterpret_cast<uint64_t>(destination),
	       static_cast<uint64_t>(size));

	if ((destination == nullptr && size != 0) || size > MAX_RANDOM_BYTES)
	{
		return RANDOM_ERROR_INVALID;
	}
	if (size == 0)
	{
		return OK;
	}

	auto* bytes = static_cast<uint8_t*>(destination);
	for (size_t i = 0; i < size; ++i)
	{
		bytes[i] = static_cast<uint8_t>(Kyty::Math::Rand::Uint() & 0xffu);
	}
	return OK;
}

} // namespace Random

LIB_DEFINE(InitRandom_1)
{
	LIB_FUNC("PI7jIZj4pcE", Random::RandomGetRandomNumber);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
