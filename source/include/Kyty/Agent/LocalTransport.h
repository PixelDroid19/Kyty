#ifndef KYTY_INCLUDE_KYTY_AGENT_LOCAL_TRANSPORT_H_
#define KYTY_INCLUDE_KYTY_AGENT_LOCAL_TRANSPORT_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Kyty::Agent::LocalTransport {

enum class Result: uint8_t
{
	Ok,
	InvalidArgument,
	InvalidEndpoint,
	NotFound,
	Busy,
	Interrupted,
	MessageTooLarge,
	IoError
};

struct Connection
{
	uintptr_t native = 0;
#if defined(_WIN32)
	uintptr_t stop_event = 0;
#endif
};

struct Listener
{
	std::atomic<uintptr_t> native {0};
	std::atomic<bool>      interrupted {false};
	std::string            endpoint;
#if defined(_WIN32)
	uintptr_t stop_event = 0;
#endif
};

[[nodiscard]] bool        IsValidEndpoint(const char* endpoint) noexcept;
[[nodiscard]] const char* EndpointKind() noexcept;
[[nodiscard]] const char* ResultName(Result result) noexcept;

[[nodiscard]] Result Listen(Listener* listener, const char* endpoint) noexcept;
[[nodiscard]] Result Accept(Listener* listener, Connection* connection) noexcept;
void                 Interrupt(Listener* listener) noexcept;
// Close only after Accept has returned.
void                 Close(Listener* listener) noexcept;

[[nodiscard]] Result Connect(const char* endpoint, Connection* connection) noexcept;
[[nodiscard]] Result WriteAll(Connection* connection, const void* data, size_t size) noexcept;
[[nodiscard]] Result ReadLine(Connection* connection, std::string* line, size_t max_size) noexcept;
// Signals the connection stop event. Pending Windows I/O is canceled and drained by its owner.
void                 Interrupt(Connection* connection) noexcept;
// Close only after the connection's current I/O operation has returned.
void                 Close(Connection* connection) noexcept;

} // namespace Kyty::Agent::LocalTransport

#endif // KYTY_INCLUDE_KYTY_AGENT_LOCAL_TRANSPORT_H_
