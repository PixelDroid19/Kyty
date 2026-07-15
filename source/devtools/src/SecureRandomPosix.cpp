#include "Kyty/DevTools/Supervisor/SharedMapping.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace Kyty::DevTools {

ProcessOperationError SecureRandomFill(uint8_t* out, uint32_t size) noexcept
{
	if (out == nullptr || size == 0u)
	{
		return ProcessOperationError::InvalidArgument;
	}
	const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		return ProcessOperationError::EntropyUnavailable;
	}
	uint32_t filled = 0;
	while (filled < size)
	{
		const ssize_t n = ::read(fd, out + filled, size - filled);
		if (n < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			::close(fd);
			return ProcessOperationError::EntropyUnavailable;
		}
		if (n == 0)
		{
			::close(fd);
			return ProcessOperationError::EntropyUnavailable;
		}
		filled += static_cast<uint32_t>(n);
	}
	::close(fd);
	return ProcessOperationError::None;
}

} // namespace Kyty::DevTools
