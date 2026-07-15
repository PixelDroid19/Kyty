#include "Kyty/DevTools/Transport/WorkerSession.h"

#include "Kyty/DevTools/Protocol/Protocol.h"
#include "Kyty/DevTools/Transport/Bootstrap.h"

#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdint>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Kyty::DevTools {
namespace {

void ScrubBootstrapEnvironment() noexcept
{
#if defined(_WIN32)
	(void)::_putenv_s(kBootstrapEnvName, "");
#else
	(void)::unsetenv(kBootstrapEnvName);
#endif
}

[[nodiscard]] bool CopyBootstrapValue(const char* value, BootstrapText* out) noexcept
{
	if (out == nullptr)
	{
		return false;
	}
	*out = {};
	if (value == nullptr)
	{
		return true;
	}

	uint32_t length = 0;
	while (length + 1u < sizeof(out->bytes) && value[length] != '\0')
	{
		++length;
	}
	if (value[length] != '\0')
	{
		return false;
	}
	std::memcpy(out->bytes, value, length);
	out->bytes[length] = '\0';
	out->size         = length;
	return true;
}

} // namespace

struct WorkerSession::State
{
	std::unique_ptr<WorkerTelemetry> telemetry {};
	MutableMappingView mapping {};
	void*             mapped_address = nullptr;
	uint64_t          mapping_handle = 0;
	uint64_t          liveness_handle = 0;
};

void WorkerSession::CloseWorkerHandles(State* state) noexcept
{
	if (state == nullptr)
	{
		return;
	}
#if defined(_WIN32)
	if (state->mapping_handle != 0u)
	{
		::CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(state->mapping_handle)));
	}
	if (state->liveness_handle != 0u)
	{
		::CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(state->liveness_handle)));
	}
#else
	if (state->mapping_handle != 0u)
	{
		::close(static_cast<int>(state->mapping_handle));
	}
	if (state->liveness_handle != 0u)
	{
		::close(static_cast<int>(state->liveness_handle));
	}
#endif
	state->mapping_handle  = 0;
	state->liveness_handle = 0;
}

void WorkerSession::UnmapWorkerMapping(State* state) noexcept
{
	if (state == nullptr || state->mapped_address == nullptr)
	{
		return;
	}
#if defined(_WIN32)
	::UnmapViewOfFile(state->mapped_address);
#else
	::munmap(state->mapped_address, static_cast<size_t>(kProtocolMappingSize));
#endif
	state->mapped_address = nullptr;
	state->mapping        = {};
}

WorkerSession::WorkerSession() noexcept : state_(new (std::nothrow) State) {}

WorkerSession::~WorkerSession()
{
	(void)Stop();
}

WorkerSessionResult WorkerSession::StartFromBootstrap(const char* value, const WorkerTelemetryOptions& options) noexcept
{
	BootstrapText bootstrap_copy {};
	const bool    bootstrap_copy_valid = CopyBootstrapValue(value, &bootstrap_copy);
	ScrubBootstrapEnvironment();
	const char* parse_value = (value == nullptr || !bootstrap_copy_valid) ? nullptr : bootstrap_copy.bytes;

	if (!state_)
	{
		return WorkerSessionResult::MappingFailed;
	}
	if (Active())
	{
		return WorkerSessionResult::AlreadyActive;
	}

	BootstrapMetadata metadata {};
	const auto parsed = !bootstrap_copy_valid ? BootstrapParseResult::Malformed : ParseBootstrapMetadata(parse_value, &metadata);
	if (parsed == BootstrapParseResult::Missing)
	{
		return WorkerSessionResult::MissingBootstrap;
	}
	if (parsed != BootstrapParseResult::Valid)
	{
		return WorkerSessionResult::MalformedBootstrap;
	}

	state_->mapping_handle  = metadata.mapping_handle;
	state_->liveness_handle = metadata.liveness_handle;

#if defined(_WIN32)
	if (metadata.platform != BootstrapPlatform::Windows || metadata.mapping_handle == 0u || metadata.liveness_handle == 0u)
	{
		CloseWorkerHandles(state_.get());
		return WorkerSessionResult::MappingFailed;
	}
	state_->mapped_address = ::MapViewOfFile(
	    reinterpret_cast<HANDLE>(static_cast<uintptr_t>(metadata.mapping_handle)), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
	    static_cast<SIZE_T>(kProtocolMappingSize));
	if (state_->mapped_address == nullptr)
	{
		CloseWorkerHandles(state_.get());
		return WorkerSessionResult::MappingFailed;
	}
#else
	if (metadata.platform != BootstrapPlatform::Posix || metadata.mapping_handle != 3u || metadata.liveness_handle != 4u)
	{
		CloseWorkerHandles(state_.get());
		return WorkerSessionResult::MappingFailed;
	}
	struct stat st {};
	if (::fstat(static_cast<int>(metadata.mapping_handle), &st) != 0 || st.st_size < 0 ||
	    static_cast<uint64_t>(st.st_size) < kProtocolMappingSize)
	{
		CloseWorkerHandles(state_.get());
		return WorkerSessionResult::MappingFailed;
	}
	state_->mapped_address = ::mmap(nullptr, static_cast<size_t>(kProtocolMappingSize), PROT_READ | PROT_WRITE, MAP_SHARED,
	                                static_cast<int>(metadata.mapping_handle), 0);
	if (state_->mapped_address == MAP_FAILED)
	{
		state_->mapped_address = nullptr;
		CloseWorkerHandles(state_.get());
		return WorkerSessionResult::MappingFailed;
	}
#endif

	state_->mapping = {static_cast<uint8_t*>(state_->mapped_address), kProtocolMappingSize};
	RecordingMode requested_mode = RecordingMode::MetricsOnly;
	if (ReadWorkerBootstrap({state_->mapping.data, state_->mapping.size}, metadata.nonce.bytes, &requested_mode) != ProtocolResult::Ok)
	{
		UnmapWorkerMapping(state_.get());
		CloseWorkerHandles(state_.get());
		return WorkerSessionResult::ProtocolRejected;
	}

	WorkerTelemetryOptions effective = options;
	effective.requested_mode       = requested_mode;
	state_->telemetry.reset(new (std::nothrow) WorkerTelemetry);
	if (!state_->telemetry || !state_->telemetry->Start(state_->mapping, effective))
	{
		UnmapWorkerMapping(state_.get());
		CloseWorkerHandles(state_.get());
		return WorkerSessionResult::ProtocolRejected;
	}
	return WorkerSessionResult::Attached;
}

bool WorkerSession::Record(EventRecord record) noexcept
{
	return state_ != nullptr && state_->telemetry != nullptr && state_->telemetry->Record(record);
}

bool WorkerSession::Publish() noexcept
{
	return state_ != nullptr && state_->telemetry != nullptr && state_->telemetry->Publish();
}

bool WorkerSession::Stop() noexcept
{
	if (!state_)
	{
		return false;
	}
	const bool telemetry_ok = state_->telemetry == nullptr || !state_->telemetry->Active() || state_->telemetry->Stop();
	UnmapWorkerMapping(state_.get());
	CloseWorkerHandles(state_.get());
	return telemetry_ok;
}

bool WorkerSession::Active() const noexcept
{
	return state_ != nullptr && state_->telemetry != nullptr && state_->telemetry->Active();
}

} // namespace Kyty::DevTools
