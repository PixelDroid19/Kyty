#ifndef EMULATOR_INCLUDE_EMULATOR_GUESTPLATFORM_H_
#define EMULATOR_INCLUDE_EMULATOR_GUESTPLATFORM_H_

#include <cstdint>

namespace Kyty {

enum class GuestPlatform: uint8_t
{
	Unknown = 0,
	Ps4     = 1,
	Ps5     = 2,
};

[[nodiscard]] constexpr const char* GuestPlatformName(GuestPlatform platform)
{
	switch (platform)
	{
		case GuestPlatform::Ps4: return "ps4";
		case GuestPlatform::Ps5: return "ps5";
		case GuestPlatform::Unknown: return "unknown";
	}
	return "unknown";
}

[[nodiscard]] constexpr uint8_t GuestPlatformAbiVersion(GuestPlatform platform)
{
	switch (platform)
	{
		case GuestPlatform::Ps4: return 0;
		case GuestPlatform::Ps5: return 2;
		case GuestPlatform::Unknown: return 0xff;
	}
	return 0xff;
}

} // namespace Kyty

#endif /* EMULATOR_INCLUDE_EMULATOR_GUESTPLATFORM_H_ */
