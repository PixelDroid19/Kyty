#include "Kyty/DevTools/Supervisor/SharedMapping.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Kyty::DevTools {

struct SharedMapping::State
{
	int      fd      = -1;
	void*    map     = nullptr;
	uint64_t size    = 0;
	bool     valid   = false;
};

SharedMapping::SharedMapping() noexcept = default;

SharedMapping::SharedMapping(SharedMapping&& other) noexcept : state_(std::move(other.state_)) {}

SharedMapping& SharedMapping::operator=(SharedMapping&& other) noexcept
{
	if (this != &other)
	{
		Close();
		state_ = std::move(other.state_);
	}
	return *this;
}

SharedMapping::~SharedMapping()
{
	Close();
}

bool SharedMapping::IsValid() const noexcept
{
	return state_ && state_->valid;
}

MutableMappingView SharedMapping::MutableView() noexcept
{
	if (!IsValid())
	{
		return {};
	}
	return {static_cast<uint8_t*>(state_->map), state_->size};
}

ConstMappingView SharedMapping::View() const noexcept
{
	if (!IsValid())
	{
		return {};
	}
	return {static_cast<const uint8_t*>(state_->map), state_->size};
}

uint64_t SharedMapping::InheritableHandle() const noexcept
{
	if (!IsValid())
	{
		return 0;
	}
	return static_cast<uint64_t>(state_->fd);
}

void SharedMapping::Close() noexcept
{
	if (!state_)
	{
		return;
	}
	if (state_->map != nullptr && state_->map != MAP_FAILED)
	{
		::munmap(state_->map, static_cast<size_t>(state_->size));
		state_->map = nullptr;
	}
	if (state_->fd >= 0)
	{
		::close(state_->fd);
		state_->fd = -1;
	}
	state_->valid = false;
	state_.reset();
}

ProcessOperationError SharedMapping::CreateOwnerOnly(uint64_t size, SharedMapping* out) noexcept
{
	if (out == nullptr || size == 0u || size > (1ull << 30))
	{
		return ProcessOperationError::InvalidArgument;
	}
	out->Close();

	for (int attempt = 0; attempt < 32; ++attempt)
	{
		uint8_t rnd[8] = {};
		if (SecureRandomFill(rnd, sizeof(rnd)) != ProcessOperationError::None)
		{
			return ProcessOperationError::EntropyUnavailable;
		}
		char name[64] = {};
		// Anonymous-ish exclusive name; unlinked immediately after open.
		std::snprintf(name, sizeof(name), "/kytydt-%02x%02x%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5],
		              rnd[6], rnd[7]);

		const int fd = ::shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd < 0)
		{
			if (errno == EEXIST)
			{
				continue;
			}
			return ProcessOperationError::MappingFailed;
		}
		const int descriptor_flags = ::fcntl(fd, F_GETFD);
		if (descriptor_flags < 0 || ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0)
		{
			::close(fd);
			return ProcessOperationError::MappingFailed;
		}
		// Unlink immediately so the object is not discoverable by name.
		::shm_unlink(name);

		if (::ftruncate(fd, static_cast<off_t>(size)) != 0)
		{
			::close(fd);
			return ProcessOperationError::MappingFailed;
		}
		struct stat st {};
		// Darwin may round a POSIX shared-memory object to its host page size
		// (16 KiB on Apple Silicon). The logical mapping remains `size` bytes;
		// reject only a backing object that is smaller than requested.
		if (::fstat(fd, &st) != 0 || st.st_size < 0 || static_cast<uint64_t>(st.st_size) < size)
		{
			::close(fd);
			return ProcessOperationError::MappingFailed;
		}
		void* map = ::mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (map == MAP_FAILED)
		{
			::close(fd);
			return ProcessOperationError::MappingFailed;
		}
		std::memset(map, 0, static_cast<size_t>(size));

		auto state   = std::make_unique<State>();
		state->fd    = fd;
		state->map   = map;
		state->size  = size;
		state->valid = true;
		out->state_  = std::move(state);
		return ProcessOperationError::None;
	}
	return ProcessOperationError::MappingFailed;
}

} // namespace Kyty::DevTools
