#include "Kyty/Agent/LocalTransport.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Kyty::Agent::LocalTransport {
namespace {

#if defined(_WIN32)

constexpr const char* kPipePrefix = R"(\\.\pipe\)";

HANDLE ToHandle(uintptr_t native) noexcept
{
	return reinterpret_cast<HANDLE>(native);
}

uintptr_t FromHandle(HANDLE handle) noexcept
{
	return reinterpret_cast<uintptr_t>(handle);
}

Result MapWindowsError(DWORD error) noexcept
{
	switch (error)
	{
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
		case ERROR_PIPE_NOT_CONNECTED: return Result::NotFound;
		case ERROR_PIPE_BUSY:
		case ERROR_ACCESS_DENIED: return Result::Busy;
		case ERROR_OPERATION_ABORTED: return Result::Interrupted;
		default: return Result::IoError;
	}
}

#else

int ToFd(uintptr_t native) noexcept
{
	return static_cast<int>(native) - 1;
}

uintptr_t FromFd(int fd) noexcept
{
	return static_cast<uintptr_t>(fd + 1);
}

Result MapPosixError(int error) noexcept
{
	switch (error)
	{
		case ENOENT:
		case ECONNREFUSED: return Result::NotFound;
		case EADDRINUSE:
		case EACCES:
		case EAGAIN: return Result::Busy;
		case EINTR:
		case EBADF:
		case EINVAL: return Result::Interrupted;
		default: return Result::IoError;
	}
}

#endif

} // namespace

bool IsValidEndpoint(const char* endpoint) noexcept
{
	if (endpoint == nullptr || endpoint[0] == '\0')
	{
		return false;
	}
#if defined(_WIN32)
	const size_t prefix_size = std::strlen(kPipePrefix);
	return std::strncmp(endpoint, kPipePrefix, prefix_size) == 0 && endpoint[prefix_size] != '\0' &&
	       std::strlen(endpoint) < 256;
#else
	return endpoint[0] == '/' && std::strlen(endpoint) < sizeof(sockaddr_un::sun_path);
#endif
}

const char* EndpointKind() noexcept
{
#if defined(_WIN32)
	return "windows_named_pipe";
#else
	return "unix_socket";
#endif
}

const char* ResultName(Result result) noexcept
{
	switch (result)
	{
		case Result::Ok: return "ok";
		case Result::InvalidArgument: return "invalid_argument";
		case Result::InvalidEndpoint: return "invalid_endpoint";
		case Result::NotFound: return "not_found";
		case Result::Busy: return "busy";
		case Result::Interrupted: return "interrupted";
		case Result::MessageTooLarge: return "message_too_large";
		case Result::IoError: return "io_error";
	}
	return "unknown";
}

Result Listen(Listener* listener, const char* endpoint) noexcept
{
	if (listener == nullptr)
	{
		return Result::InvalidArgument;
	}
	if (!IsValidEndpoint(endpoint))
	{
		return Result::InvalidEndpoint;
	}
	if (listener->native.load() != 0 || !listener->endpoint.empty())
	{
		return Result::Busy;
	}

	listener->endpoint    = endpoint;
	listener->interrupted = false;

#if defined(_WIN32)
	return Result::Ok;
#else
	const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
	{
		listener->endpoint.clear();
		return MapPosixError(errno);
	}

	struct stat existing {};
	if (::lstat(endpoint, &existing) == 0)
	{
		if (!S_ISSOCK(existing.st_mode) || ::unlink(endpoint) != 0)
		{
			::close(fd);
			listener->endpoint.clear();
			return Result::Busy;
		}
	} else if (errno != ENOENT)
	{
		::close(fd);
		listener->endpoint.clear();
		return MapPosixError(errno);
	}

	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", endpoint);
	if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
	{
		const int error = errno;
		::close(fd);
		listener->endpoint.clear();
		return MapPosixError(error);
	}
	if (::chmod(endpoint, S_IRUSR | S_IWUSR) != 0 || ::listen(fd, 1) != 0)
	{
		const int error = errno;
		::close(fd);
		::unlink(endpoint);
		listener->endpoint.clear();
		return MapPosixError(error);
	}
	listener->native = FromFd(fd);
	return Result::Ok;
#endif
}

Result Accept(Listener* listener, Connection* connection) noexcept
{
	if (listener == nullptr || connection == nullptr || connection->native != 0 || listener->endpoint.empty())
	{
		return Result::InvalidArgument;
	}
	if (listener->interrupted.load())
	{
		return Result::Interrupted;
	}

#if defined(_WIN32)
	const HANDLE pipe = ::CreateNamedPipeA(listener->endpoint.c_str(), PIPE_ACCESS_DUPLEX,
	                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
	                                       64u * 1024u, 4u * 1024u, 0, nullptr);
	if (pipe == INVALID_HANDLE_VALUE)
	{
		return MapWindowsError(::GetLastError());
	}
	listener->native = FromHandle(pipe);
	if (listener->interrupted.load())
	{
		listener->native = 0;
		::CloseHandle(pipe);
		return Result::Interrupted;
	}

	const BOOL connected = ::ConnectNamedPipe(pipe, nullptr);
	const DWORD error     = connected != FALSE ? ERROR_SUCCESS : ::GetLastError();
	listener->native      = 0;
	if (connected == FALSE && error != ERROR_PIPE_CONNECTED)
	{
		::CloseHandle(pipe);
		return MapWindowsError(error);
	}
	connection->native = FromHandle(pipe);
	return Result::Ok;
#else
	const int listen_fd = ToFd(listener->native.load());
	if (listen_fd < 0)
	{
		return Result::Interrupted;
	}
	const int fd = ::accept(listen_fd, nullptr, nullptr);
	if (fd < 0)
	{
		return listener->interrupted.load() ? Result::Interrupted : MapPosixError(errno);
	}
	connection->native = FromFd(fd);
	return Result::Ok;
#endif
}

void Interrupt(Listener* listener) noexcept
{
	if (listener == nullptr)
	{
		return;
	}
	listener->interrupted = true;
#if defined(_WIN32)
	const HANDLE pipe = ToHandle(listener->native.load());
	if (pipe != nullptr)
	{
		(void)::CancelIoEx(pipe, nullptr);
		const HANDLE wake = ::CreateFileA(listener->endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (wake != INVALID_HANDLE_VALUE)
		{
			::CloseHandle(wake);
		}
	}
#else
	const uintptr_t native = listener->native.exchange(0);
	if (native != 0)
	{
		const int fd = ToFd(native);
		(void)::shutdown(fd, SHUT_RDWR);
		::close(fd);
	}
#endif
}

void Close(Listener* listener) noexcept
{
	if (listener == nullptr)
	{
		return;
	}
	Interrupt(listener);
#if defined(_WIN32)
	const uintptr_t native = listener->native.exchange(0);
	if (native != 0)
	{
		::CloseHandle(ToHandle(native));
	}
#else
	if (!listener->endpoint.empty())
	{
		::unlink(listener->endpoint.c_str());
	}
#endif
	listener->endpoint.clear();
}

Result Connect(const char* endpoint, Connection* connection) noexcept
{
	if (connection == nullptr || connection->native != 0)
	{
		return Result::InvalidArgument;
	}
	if (!IsValidEndpoint(endpoint))
	{
		return Result::InvalidEndpoint;
	}

#if defined(_WIN32)
	HANDLE pipe       = INVALID_HANDLE_VALUE;
	DWORD last_error = ERROR_FILE_NOT_FOUND;
	// The server recreates its single local pipe instance after each client.
	// Bound the hand-off race so sequential commands (for example doctor) do
	// not observe a transient ERROR_FILE_NOT_FOUND between two requests.
	for (uint32_t attempt = 0; attempt < 50; ++attempt)
	{
		pipe = ::CreateFileA(endpoint, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (pipe != INVALID_HANDLE_VALUE)
		{
			break;
		}
		last_error = ::GetLastError();
		if (last_error == ERROR_PIPE_BUSY)
		{
			(void)::WaitNamedPipeA(endpoint, 10);
		} else if (last_error == ERROR_FILE_NOT_FOUND || last_error == ERROR_PATH_NOT_FOUND)
		{
			::Sleep(5);
		} else
		{
			break;
		}
	}
	if (pipe == INVALID_HANDLE_VALUE)
	{
		return MapWindowsError(last_error);
	}
	connection->native = FromHandle(pipe);
	return Result::Ok;
#else
	const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
	{
		return MapPosixError(errno);
	}
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", endpoint);
	if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
	{
		const int error = errno;
		::close(fd);
		return MapPosixError(error);
	}
	connection->native = FromFd(fd);
	return Result::Ok;
#endif
}

Result WriteAll(Connection* connection, const void* data, size_t size) noexcept
{
	if (connection == nullptr || connection->native == 0 || (data == nullptr && size != 0))
	{
		return Result::InvalidArgument;
	}
	size_t offset = 0;
	while (offset < size)
	{
#if defined(_WIN32)
		const size_t remaining = size - offset;
		const DWORD  chunk     = static_cast<DWORD>(remaining > 0x7fffffffu ? 0x7fffffffu : remaining);
		DWORD        written   = 0;
		if (::WriteFile(ToHandle(connection->native), static_cast<const uint8_t*>(data) + offset, chunk, &written, nullptr) == FALSE ||
		    written == 0)
		{
			return MapWindowsError(::GetLastError());
		}
		offset += written;
#else
		const ssize_t written = ::write(ToFd(connection->native), static_cast<const uint8_t*>(data) + offset, size - offset);
		if (written < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return MapPosixError(errno);
		}
		if (written == 0)
		{
			return Result::IoError;
		}
		offset += static_cast<size_t>(written);
#endif
	}
	return Result::Ok;
}

Result ReadLine(Connection* connection, std::string* line, size_t max_size) noexcept
{
	if (connection == nullptr || connection->native == 0 || line == nullptr || max_size == 0)
	{
		return Result::InvalidArgument;
	}
	line->clear();
	while (line->size() < max_size)
	{
		char ch = 0;
#if defined(_WIN32)
		DWORD read = 0;
		if (::ReadFile(ToHandle(connection->native), &ch, 1, &read, nullptr) == FALSE)
		{
			const DWORD error = ::GetLastError();
			if (error == ERROR_BROKEN_PIPE)
			{
				return line->empty() ? Result::NotFound : Result::Ok;
			}
			return MapWindowsError(error);
		}
		if (read == 0)
		{
			return line->empty() ? Result::NotFound : Result::Ok;
		}
#else
		const ssize_t read = ::read(ToFd(connection->native), &ch, 1);
		if (read < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return MapPosixError(errno);
		}
		if (read == 0)
		{
			return line->empty() ? Result::NotFound : Result::Ok;
		}
#endif
		if (ch == '\n')
		{
			return Result::Ok;
		}
		if (ch != '\r')
		{
			line->push_back(ch);
		}
	}
	return Result::MessageTooLarge;
}

void Close(Connection* connection) noexcept
{
	if (connection == nullptr || connection->native == 0)
	{
		return;
	}
#if defined(_WIN32)
	::CloseHandle(ToHandle(connection->native));
#else
	::close(ToFd(connection->native));
#endif
	connection->native = 0;
}

} // namespace Kyty::Agent::LocalTransport
