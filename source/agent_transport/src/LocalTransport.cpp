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
		case ERROR_PIPE_NOT_CONNECTED:
		case ERROR_BROKEN_PIPE: return Result::NotFound;
		case ERROR_PIPE_BUSY:
		case ERROR_ACCESS_DENIED: return Result::Busy;
		case ERROR_OPERATION_ABORTED: return Result::Interrupted;
		default: return Result::IoError;
	}
}

class ScopedHandle final
{
public:
	explicit ScopedHandle(HANDLE handle = nullptr) noexcept: m_handle(handle) {}
	~ScopedHandle()
	{
		Reset();
	}

	ScopedHandle(const ScopedHandle&)            = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;

	[[nodiscard]] HANDLE Get() const noexcept
	{
		return m_handle;
	}

	[[nodiscard]] bool IsValid() const noexcept
	{
		return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
	}

	[[nodiscard]] HANDLE Release() noexcept
	{
		const HANDLE handle = m_handle;
		m_handle            = nullptr;
		return handle;
	}

	void Reset(HANDLE handle = nullptr) noexcept
	{
		if (IsValid())
		{
			(void)::CloseHandle(m_handle);
		}
		m_handle = handle;
	}

private:
	HANDLE m_handle = nullptr;
};

Result CheckStopEvent(HANDLE stop_event) noexcept
{
	if (stop_event == nullptr || stop_event == INVALID_HANDLE_VALUE)
	{
		return Result::IoError;
	}
	const DWORD wait_result = ::WaitForSingleObject(stop_event, 0);
	if (wait_result == WAIT_OBJECT_0)
	{
		return Result::Interrupted;
	}
	return wait_result == WAIT_TIMEOUT ? Result::Ok : Result::IoError;
}

void CancelAndDrain(HANDLE pipe, OVERLAPPED* overlapped) noexcept
{
	// Cancel only this operation, then keep its OVERLAPPED and buffer alive until
	// Windows reports completion. Cancellation can lose a race with normal completion.
	(void)::CancelIoEx(pipe, overlapped);
	DWORD ignored = 0;
	(void)::GetOverlappedResult(pipe, overlapped, &ignored, TRUE);
}

Result WaitForWindowsIo(HANDLE pipe, HANDLE stop_event, OVERLAPPED* overlapped, DWORD* transferred) noexcept
{
	const HANDLE wait_handles[] = {stop_event, overlapped->hEvent};
	const DWORD  wait_result   = ::WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
	if (wait_result == WAIT_OBJECT_0)
	{
		CancelAndDrain(pipe, overlapped);
		return Result::Interrupted;
	}
	if (wait_result == WAIT_OBJECT_0 + 1)
	{
		if (::GetOverlappedResult(pipe, overlapped, transferred, FALSE) == FALSE)
		{
			const DWORD error = ::GetLastError();
			const Result stop = CheckStopEvent(stop_event);
			return stop == Result::Ok ? MapWindowsError(error) : stop;
		}
		return CheckStopEvent(stop_event);
	}

	CancelAndDrain(pipe, overlapped);
	return Result::IoError;
}

Result ReadWindowsByte(HANDLE pipe, HANDLE stop_event, HANDLE operation_event, char* byte, DWORD* transferred) noexcept
{
	Result result = CheckStopEvent(stop_event);
	if (result != Result::Ok)
	{
		return result;
	}
	if (::ResetEvent(operation_event) == FALSE)
	{
		return Result::IoError;
	}

	OVERLAPPED overlapped {};
	overlapped.hEvent = operation_event;
	if (::ReadFile(pipe, byte, 1, transferred, &overlapped) != FALSE)
	{
		if (::GetOverlappedResult(pipe, &overlapped, transferred, FALSE) == FALSE)
		{
			const DWORD error = ::GetLastError();
			result            = CheckStopEvent(stop_event);
			return result == Result::Ok ? MapWindowsError(error) : result;
		}
		return CheckStopEvent(stop_event);
	}

	const DWORD error = ::GetLastError();
	if (error != ERROR_IO_PENDING)
	{
		result = CheckStopEvent(stop_event);
		return result == Result::Ok ? MapWindowsError(error) : result;
	}
	return WaitForWindowsIo(pipe, stop_event, &overlapped, transferred);
}

Result WriteWindowsChunk(HANDLE pipe, HANDLE stop_event, HANDLE operation_event, const void* data, DWORD size,
	                     DWORD* transferred) noexcept
{
	Result result = CheckStopEvent(stop_event);
	if (result != Result::Ok)
	{
		return result;
	}
	if (::ResetEvent(operation_event) == FALSE)
	{
		return Result::IoError;
	}

	OVERLAPPED overlapped {};
	overlapped.hEvent = operation_event;
	if (::WriteFile(pipe, data, size, transferred, &overlapped) != FALSE)
	{
		if (::GetOverlappedResult(pipe, &overlapped, transferred, FALSE) == FALSE)
		{
			const DWORD error = ::GetLastError();
			result            = CheckStopEvent(stop_event);
			return result == Result::Ok ? MapWindowsError(error) : result;
		}
		return CheckStopEvent(stop_event);
	}

	const DWORD error = ::GetLastError();
	if (error != ERROR_IO_PENDING)
	{
		result = CheckStopEvent(stop_event);
		return result == Result::Ok ? MapWindowsError(error) : result;
	}
	return WaitForWindowsIo(pipe, stop_event, &overlapped, transferred);
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

Result ConfigureNoSigPipe(int fd) noexcept
{
#if defined(SO_NOSIGPIPE)
	const int enabled = 1;
	if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
	{
		return MapPosixError(errno);
	}
#else
	(void)fd;
#endif
	return Result::Ok;
}

int SendFlags() noexcept
{
#if defined(MSG_NOSIGNAL)
	return MSG_NOSIGNAL;
#else
	return 0;
#endif
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
	if (listener->native.load() != 0 || !listener->endpoint.empty()
#if defined(_WIN32)
	    || listener->stop_event != 0
#endif
	)
	{
		return Result::Busy;
	}

	listener->endpoint    = endpoint;
	listener->interrupted = false;

#if defined(_WIN32)
	ScopedHandle stop_event(::CreateEventA(nullptr, TRUE, FALSE, nullptr));
	if (!stop_event.IsValid())
	{
		const DWORD error = ::GetLastError();
		listener->endpoint.clear();
		return MapWindowsError(error);
	}
	listener->stop_event = FromHandle(stop_event.Release());
	return Result::Ok;
#else
	const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
	{
		listener->endpoint.clear();
		return MapPosixError(errno);
	}
	const Result no_sigpipe = ConfigureNoSigPipe(fd);
	if (no_sigpipe != Result::Ok)
	{
		::close(fd);
		listener->endpoint.clear();
		return no_sigpipe;
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
	if (listener == nullptr || connection == nullptr || connection->native != 0 || listener->endpoint.empty()
#if defined(_WIN32)
	    || connection->stop_event != 0
#endif
	)
	{
		return Result::InvalidArgument;
	}
	if (listener->interrupted.load())
	{
		return Result::Interrupted;
	}

#if defined(_WIN32)
	if (listener->stop_event == 0)
	{
		return Result::InvalidArgument;
	}
	const HANDLE stop_event = ToHandle(listener->stop_event);
	Result       stopped    = CheckStopEvent(stop_event);
	if (stopped != Result::Ok)
	{
		return stopped;
	}
	ScopedHandle pipe(::CreateNamedPipeA(listener->endpoint.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
	                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
	                                       64u * 1024u, 4u * 1024u, 0, nullptr));
	if (!pipe.IsValid())
	{
		return MapWindowsError(::GetLastError());
	}
	listener->native = FromHandle(pipe.Get());
	ScopedHandle operation_event(::CreateEventA(nullptr, TRUE, FALSE, nullptr));
	if (!operation_event.IsValid())
	{
		listener->native = 0;
		return MapWindowsError(::GetLastError());
	}

	stopped = CheckStopEvent(stop_event);
	if (stopped != Result::Ok)
	{
		listener->native = 0;
		return stopped;
	}

	OVERLAPPED overlapped {};
	overlapped.hEvent = operation_event.Get();
	const BOOL connected = ::ConnectNamedPipe(pipe.Get(), &overlapped);
	if (connected == FALSE)
	{
		const DWORD error = ::GetLastError();
		if (error == ERROR_IO_PENDING)
		{
			DWORD ignored = 0;
			stopped = WaitForWindowsIo(pipe.Get(), stop_event, &overlapped, &ignored);
			if (stopped != Result::Ok)
			{
				listener->native = 0;
				return stopped;
			}
		} else if (error != ERROR_PIPE_CONNECTED)
		{
			listener->native = 0;
			stopped = CheckStopEvent(stop_event);
			return stopped == Result::Ok ? MapWindowsError(error) : stopped;
		}
	}

	stopped = CheckStopEvent(stop_event);
	if (stopped != Result::Ok)
	{
		listener->native = 0;
		return stopped;
	}
	ScopedHandle connection_stop_event(::CreateEventA(nullptr, TRUE, FALSE, nullptr));
	if (!connection_stop_event.IsValid())
	{
		const DWORD error = ::GetLastError();
		listener->native = 0;
		return MapWindowsError(error);
	}
	stopped = CheckStopEvent(stop_event);
	listener->native = 0;
	if (stopped != Result::Ok)
	{
		return stopped;
	}
	connection->native     = FromHandle(pipe.Release());
	connection->stop_event = FromHandle(connection_stop_event.Release());
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
	const Result no_sigpipe = ConfigureNoSigPipe(fd);
	if (no_sigpipe != Result::Ok)
	{
		::close(fd);
		return no_sigpipe;
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
	if (listener->stop_event != 0)
	{
		(void)::SetEvent(ToHandle(listener->stop_event));
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
	if (listener->stop_event != 0)
	{
		(void)::CloseHandle(ToHandle(listener->stop_event));
		listener->stop_event = 0;
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
	if (connection == nullptr || connection->native != 0
#if defined(_WIN32)
	    || connection->stop_event != 0
#endif
	)
	{
		return Result::InvalidArgument;
	}
	if (!IsValidEndpoint(endpoint))
	{
		return Result::InvalidEndpoint;
	}

#if defined(_WIN32)
	ScopedHandle pipe;
	DWORD last_error = ERROR_FILE_NOT_FOUND;
	// The server recreates its single local pipe instance after each client.
	// Bound the hand-off race so sequential commands (for example doctor) do
	// not observe a transient ERROR_FILE_NOT_FOUND between two requests.
	for (uint32_t attempt = 0; attempt < 50; ++attempt)
	{
		HANDLE opened = ::CreateFileA(endpoint, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
		                              FILE_FLAG_OVERLAPPED, nullptr);
		if (opened != INVALID_HANDLE_VALUE)
		{
			pipe.Reset(opened);
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
	if (!pipe.IsValid())
	{
		return MapWindowsError(last_error);
	}
	ScopedHandle stop_event(::CreateEventA(nullptr, TRUE, FALSE, nullptr));
	if (!stop_event.IsValid())
	{
		return MapWindowsError(::GetLastError());
	}
	connection->native     = FromHandle(pipe.Release());
	connection->stop_event = FromHandle(stop_event.Release());
	return Result::Ok;
#else
	const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
	{
		return MapPosixError(errno);
	}
	const Result no_sigpipe = ConfigureNoSigPipe(fd);
	if (no_sigpipe != Result::Ok)
	{
		::close(fd);
		return no_sigpipe;
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
	if (size == 0)
	{
		return Result::Ok;
	}

#if defined(_WIN32)
	if (connection->stop_event == 0)
	{
		return Result::InvalidArgument;
	}
	ScopedHandle operation_event(::CreateEventA(nullptr, TRUE, FALSE, nullptr));
	if (!operation_event.IsValid())
	{
		return MapWindowsError(::GetLastError());
	}
#endif

	size_t offset = 0;
	while (offset < size)
	{
#if defined(_WIN32)
		const size_t remaining = size - offset;
		const DWORD  chunk     = static_cast<DWORD>(remaining > 0x7fffffffu ? 0x7fffffffu : remaining);
		DWORD        written   = 0;
		const Result write_result = WriteWindowsChunk(ToHandle(connection->native), ToHandle(connection->stop_event),
		                                              operation_event.Get(), static_cast<const uint8_t*>(data) + offset,
		                                              chunk, &written);
		if (write_result != Result::Ok)
		{
			return write_result;
		}
		if (written == 0)
		{
			return Result::IoError;
		}
		offset += written;
#else
		// A diagnostic client may time out and disconnect while the server is
		// preparing a response. Never let that local disconnect raise SIGPIPE in
		// the emulator process; report the transport error to the caller instead.
		const ssize_t written = ::send(ToFd(connection->native), static_cast<const uint8_t*>(data) + offset, size - offset,
		                               SendFlags());
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

void Interrupt(Connection* connection) noexcept
{
	if (connection == nullptr)
	{
		return;
	}
#if defined(_WIN32)
	if (connection->stop_event != 0)
	{
		(void)::SetEvent(ToHandle(connection->stop_event));
	}
#else
	if (connection->native == 0)
	{
		return;
	}
	(void)::shutdown(ToFd(connection->native), SHUT_RDWR);
#endif
}

Result ReadLine(Connection* connection, std::string* line, size_t max_size) noexcept
{
	if (connection == nullptr || connection->native == 0 || line == nullptr || max_size == 0)
	{
		return Result::InvalidArgument;
	}
	line->clear();
	size_t consumed = 0;

#if defined(_WIN32)
	if (connection->stop_event == 0)
	{
		return Result::InvalidArgument;
	}
	ScopedHandle operation_event(::CreateEventA(nullptr, TRUE, FALSE, nullptr));
	if (!operation_event.IsValid())
	{
		return MapWindowsError(::GetLastError());
	}
#endif

	while (consumed < max_size)
	{
		char ch = 0;
#if defined(_WIN32)
		DWORD read = 0;
		const Result read_result = ReadWindowsByte(ToHandle(connection->native), ToHandle(connection->stop_event),
		                                           operation_event.Get(), &ch, &read);
		if (read_result != Result::Ok)
		{
			if (read_result == Result::NotFound)
			{
				return line->empty() ? Result::NotFound : Result::Ok;
			}
			return read_result;
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
		++consumed;
		if (ch == '\n')
		{
#if defined(_WIN32)
			const Result stopped = CheckStopEvent(ToHandle(connection->stop_event));
			if (stopped != Result::Ok)
			{
				return stopped;
			}
#endif
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
	if (connection == nullptr)
	{
		return;
	}
#if defined(_WIN32)
	if (connection->native != 0)
	{
		(void)::CloseHandle(ToHandle(connection->native));
		connection->native = 0;
	}
	if (connection->stop_event != 0)
	{
		(void)::CloseHandle(ToHandle(connection->stop_event));
		connection->stop_event = 0;
	}
#else
	if (connection->native == 0)
	{
		return;
	}
	::close(ToFd(connection->native));
	connection->native = 0;
#endif
}

} // namespace Kyty::Agent::LocalTransport
