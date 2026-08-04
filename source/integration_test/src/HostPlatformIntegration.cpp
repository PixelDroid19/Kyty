#include "Emulator/Host/Platform.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace Host = Kyty::Emulator::Host;

namespace {

bool Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "host platform integration failed: %s\n", message);
	}
	return condition;
}

bool UtcTimestampHasCaptureFileShape()
{
	const std::string timestamp = Host::UtcTimestamp();
	if (!Check(timestamp.size() == 16, "UTC timestamp does not have YYYYMMDDTHHMMSSZ shape"))
	{
		return false;
	}
	if (!Check(timestamp[8] == 'T', "UTC timestamp is missing its T separator") ||
	    !Check(timestamp[15] == 'Z', "UTC timestamp is missing its Z suffix"))
	{
		return false;
	}

	for (size_t i = 0; i < timestamp.size(); i++)
	{
		if (i != 8 && i != 15 && !Check(std::isdigit(static_cast<unsigned char>(timestamp[i])) != 0, "UTC timestamp contains a non-digit"))
		{
			return false;
		}
	}
	return true;
}

bool PeakRssBytesIsCallable()
{
	const uint64_t peak_rss_bytes = Host::PeakRssBytes();
	std::printf("host platform integration peak_rss_bytes=%llu\n", static_cast<unsigned long long>(peak_rss_bytes));
#if defined(_WIN32)
	if (!Check(peak_rss_bytes > 0, "Windows peak RSS query returned zero"))
	{
		return false;
	}
#endif
	return true;
}

} // namespace

int main()
{
	if (!UtcTimestampHasCaptureFileShape() || !PeakRssBytesIsCallable())
	{
		return 1;
	}
	std::puts("host platform integration passed");
	return 0;
}
