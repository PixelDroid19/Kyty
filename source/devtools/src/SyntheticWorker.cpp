#include "Kyty/DevTools/Supervisor/Supervisor.h"

#include "Kyty/DevTools/Protocol/Protocol.h"
#include "Kyty/DevTools/Supervisor/DurableFile.h"
#include "Kyty/DevTools/Supervisor/ProcessLauncher.h"
#include "Kyty/DevTools/Telemetry/Progress.h"
#include "Kyty/DevTools/Transport/Bootstrap.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace Kyty::DevTools {
namespace {

[[nodiscard]] bool MapFromFd(int fd, uint64_t size, void** out) noexcept
{
	if (fd < 0 || out == nullptr || size == 0u)
	{
		return false;
	}
	void* map = ::mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
	{
		return false;
	}
	*out = map;
	return true;
}

[[nodiscard]] bool PublishMinimalProgress(MutableMappingView mapping, Domain domain, uint64_t instance,
                                          ProgressState state, uint64_t submitted, uint64_t completed,
                                          uint64_t now_ns) noexcept
{
	ProgressPublication pub {};
	pub.progress.count = 1;
	pub.progress.entries[0].key.domain   = domain;
	pub.progress.entries[0].key.instance = instance;
	uint64_t packed = 0;
	if (!pub.progress.entries[0].key.TryPack(&packed))
	{
		return false;
	}
	pub.progress.entries[0].record.instance_key   = packed;
	pub.progress.entries[0].record.state          = static_cast<uint16_t>(state);
	pub.progress.entries[0].record.operation      = static_cast<uint32_t>(OperationCode::HleCall);
	pub.progress.entries[0].record.submitted      = submitted;
	pub.progress.entries[0].record.completed      = completed;
	pub.progress.entries[0].record.last_change_ns = now_ns;
	pub.gpu_fault.state = GpuFaultState::NotObserved;
	return PublishProgress(mapping, pub) == ProtocolResult::Ok;
}

[[nodiscard]] uint64_t NowNs() noexcept
{
	struct timespec ts {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void SleepMs(unsigned ms) noexcept
{
	struct timespec ts {};
	ts.tv_sec  = static_cast<time_t>(ms / 1000u);
	ts.tv_nsec = static_cast<long>((ms % 1000u) * 1000000u);
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
	{
	}
}

[[nodiscard]] bool ParentAlive(int liveness_fd) noexcept
{
	if (liveness_fd < 0)
	{
		return false;
	}
	// Nonblocking zero-byte read: EOF => parent closed write end.
	uint8_t b = 0;
	const ssize_t n = ::read(liveness_fd, &b, 1);
	if (n == 0)
	{
		return false;
	}
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	{
		return true;
	}
	// Unexpected data: still treat as alive.
	return n > 0 || errno == EINTR;
}

} // namespace

int RunSyntheticWorker(const char* mode, int /*argc*/, char** /*argv*/) noexcept
{
	if (mode == nullptr || mode[0] == '\0')
	{
		return 2;
	}

	const char* boot_env = ::getenv(kBootstrapEnvName);
	BootstrapMetadata meta {};
	const BootstrapParseResult pr = ParseBootstrapMetadata(boot_env, &meta);
	if (pr == BootstrapParseResult::Missing)
	{
		// Standalone: success without diagnostics.
		return 0;
	}
	if (pr != BootstrapParseResult::Valid || meta.platform != BootstrapPlatform::Posix)
	{
		std::fprintf(stderr, "synthetic: malformed bootstrap\n");
		return 2;
	}

	// Fixed descriptors from launch contract.
	const int map_fd  = 3;
	const int live_fd = 4;
	// Make liveness nonblocking for parent-disconnect observation.
	{
		const int flags = ::fcntl(live_fd, F_GETFL);
		if (flags >= 0)
		{
			::fcntl(live_fd, F_SETFL, flags | O_NONBLOCK);
		}
	}

	void* map_ptr = nullptr;
	if (!MapFromFd(map_fd, kProtocolMappingSize, &map_ptr))
	{
		std::fprintf(stderr, "synthetic: mmap failed\n");
		return 2;
	}
	MutableMappingView mapping {static_cast<uint8_t*>(map_ptr), kProtocolMappingSize};

	// Validate parent nonce in mapping header is done by Accept on parent side;
	// worker publishes handshake with matching nonce from bootstrap.
	WorkerHandshake hs {};
	uint64_t self_pid = 0;
	uint64_t self_start = 0;
	if (!QuerySelfProcessIdentity(&self_pid, &self_start))
	{
		::munmap(map_ptr, static_cast<size_t>(kProtocolMappingSize));
		return 2;
	}
	hs.worker_pid         = self_pid;
	hs.worker_start_token = self_start;
	std::memcpy(hs.nonce, meta.nonce.bytes, 16);
	hs.accepted_mode      = RecordingMode::Full;
	hs.logging_mode       = LoggingMode::Silent;
	hs.shader_cache_state = ShaderCacheState::NoPersistentCache;
	if (PublishWorkerHandshake(mapping, hs) != ProtocolResult::Ok)
	{
		::munmap(map_ptr, static_cast<size_t>(kProtocolMappingSize));
		return 2;
	}

	if (std::strcmp(mode, "normal-exit") == 0)
	{
		const uint64_t t0 = NowNs();
		(void)PublishMinimalProgress(mapping, Domain::Hle, 1, ProgressState::Idle, 1, 1, t0);
		SleepMs(50);
		::munmap(map_ptr, static_cast<size_t>(kProtocolMappingSize));
		return 0;
	}

	if (std::strcmp(mode, "crash") == 0)
	{
		const uint64_t t0 = NowNs();
		(void)PublishMinimalProgress(mapping, Domain::Hle, 1, ProgressState::Active, 1, 0, t0);
		SleepMs(20);
		::raise(SIGABRT);
		return 134; // unreachable
	}

	if (std::strcmp(mode, "blocked-lane") == 0)
	{
		// Heartbeat via control cell; HLE lane stuck in Waiting.
		const uint64_t t_block = NowNs();
		uint64_t       i       = 0;
		for (;;)
		{
			const uint64_t now = NowNs();
			// Lane never completes (submitted > completed, Waiting).
			(void)PublishMinimalProgress(mapping, Domain::Hle, 1, ProgressState::Waiting, 1, 0, t_block);
			// Publisher health / heartbeat via skipped_publications cell kept at 0 but
			// still publish so parent sees samples.
			GlobalLossCounter hb {};
			hb.total                  = i;
			hb.last_loss_monotonic_ns = now;
			// Use AggregateRing as a non-loss heartbeat vehicle with total=0 only:
			// instead bump measurement frame via progress pub (already done).
			(void)hb;
			if (!ParentAlive(live_fd))
			{
				// parent-disconnect path also covered here.
				(void)PublishWorkerControl(mapping, ControlCell::Disconnects, GlobalLossCounter {1, now});
				// Continue synthetic workload without further transport writes of progress.
				for (int k = 0; k < 20; ++k)
				{
					SleepMs(10);
				}
				break;
			}
			SleepMs(20);
			++i;
			// Self-test will terminate us after capture; run for a while.
			if (i > 500u)
			{
				break;
			}
		}
		::munmap(map_ptr, static_cast<size_t>(kProtocolMappingSize));
		return 0;
	}

	if (std::strcmp(mode, "publication-stop") == 0)
	{
		const uint64_t t0 = NowNs();
		(void)PublishMinimalProgress(mapping, Domain::Hle, 1, ProgressState::Active, 1, 0, t0);
		// Stop publishing; remain alive.
		for (int k = 0; k < 200; ++k)
		{
			if (!ParentAlive(live_fd))
			{
				break;
			}
			SleepMs(20);
		}
		::munmap(map_ptr, static_cast<size_t>(kProtocolMappingSize));
		return 0;
	}

	if (std::strcmp(mode, "parent-disconnect") == 0)
	{
		const uint64_t t0 = NowNs();
		(void)PublishMinimalProgress(mapping, Domain::Presentation, 1, ProgressState::Active, 1, 0, t0);
		while (ParentAlive(live_fd))
		{
			SleepMs(10);
		}
		const uint64_t now = NowNs();
		(void)PublishWorkerControl(mapping, ControlCell::Disconnects, GlobalLossCounter {1, now});
		// Continue without further progress publications.
		SleepMs(100);
		::munmap(map_ptr, static_cast<size_t>(kProtocolMappingSize));
		return 0;
	}

	if (std::strcmp(mode, "privacy-canary") == 0)
	{
		// Canary values exist only in local stack; never written to mapping payloads
		// as free-form text (numeric-only protocol).
		char canary_argv[] = "CANARY_ARGV_SHOULD_NOT_APPEAR";
		char canary_path[] = "/secret/PPSA/CANARY_PATH";
		(void)canary_argv;
		(void)canary_path;
		const uint64_t t0 = NowNs();
		(void)PublishMinimalProgress(mapping, Domain::Hle, 1, ProgressState::Idle, 1, 1, t0);
		SleepMs(30);
		::munmap(map_ptr, static_cast<size_t>(kProtocolMappingSize));
		return 0;
	}

	std::fprintf(stderr, "synthetic: unknown mode\n");
	::munmap(map_ptr, static_cast<size_t>(kProtocolMappingSize));
	return 2;
}

} // namespace Kyty::DevTools
