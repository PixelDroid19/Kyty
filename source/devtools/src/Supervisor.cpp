#include "Kyty/DevTools/Supervisor/Supervisor.h"

#include "Kyty/DevTools/Diagnostics/StallClassifier.h"
#include "Kyty/DevTools/Protocol/Protocol.h"
#include "Kyty/DevTools/Supervisor/BundleWriter.h"
#include "Kyty/DevTools/Supervisor/ProcessLauncher.h"
#include "Kyty/DevTools/Supervisor/SharedMapping.h"
#include "Kyty/DevTools/Transport/Bootstrap.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <time.h>
#include <unistd.h>

namespace Kyty::DevTools {
namespace {

[[nodiscard]] uint64_t MonoNs() noexcept
{
	struct timespec ts {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void SleepNs(uint64_t ns) noexcept
{
	struct timespec ts {};
	ts.tv_sec  = static_cast<time_t>(ns / 1000000000ull);
	ts.tv_nsec = static_cast<long>(ns % 1000000000ull);
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
	{
	}
}

void CopyBundlePath(SupervisorResult* r, const BundlePath& p) noexcept
{
	if (r == nullptr || p.size == 0u)
	{
		return;
	}
	const uint32_t n = p.size < sizeof(r->bundle_path) - 1u ? p.size : static_cast<uint32_t>(sizeof(r->bundle_path) - 1u);
	std::memcpy(r->bundle_path, p.bytes, n);
	r->bundle_path[n]  = '\0';
	r->bundle_path_size = n;
}

[[nodiscard]] SupervisorOutcome OutcomeFromProcess(const ProcessStatus& st) noexcept
{
	if (st.error != ProcessStatusError::None)
	{
		return SupervisorOutcome::StatusDecodeError;
	}
	if (st.liveness != ProcessLiveness::Terminated)
	{
		return SupervisorOutcome::ProtocolError;
	}
	switch (st.termination)
	{
		case ProcessTermination::ExitCode: return SupervisorOutcome::ChildExited;
		case ProcessTermination::Signal:
		case ProcessTermination::UnhandledException: return SupervisorOutcome::ChildCrashed;
		case ProcessTermination::OpaquePlatformStatus: return SupervisorOutcome::ChildTerminated;
		default: return SupervisorOutcome::ChildTerminated;
	}
}

void ComposeLoss(const ProgressPublication& pub, const ProtocolHealthSnapshot& health,
                 const ProtocolReadLossState& read_loss, LossSnapshot* out) noexcept
{
	*out = {};
	out->writers              = pub.writer_loss;
	out->progress             = pub.progress_loss;
	out->unregistered_writers = health.unregistered_writers;
	out->skipped_publications = health.skipped_publications;
	out->disconnects          = health.disconnects;
	out->rejected_samples     = health.rejected_samples;
	// Merge parent-side rejected samples into loss snapshot.
	out->rejected_samples.total += read_loss.rejected_samples.total;
	if (read_loss.rejected_samples.last_loss_monotonic_ns > out->rejected_samples.last_loss_monotonic_ns)
	{
		out->rejected_samples.last_loss_monotonic_ns = read_loss.rejected_samples.last_loss_monotonic_ns;
	}
	out->max_loss_monotonic_ns = health.max_loss_monotonic_ns;
	if (pub.writer_loss.max_loss_monotonic_ns > out->max_loss_monotonic_ns)
	{
		out->max_loss_monotonic_ns = pub.writer_loss.max_loss_monotonic_ns;
	}
	if (pub.progress_loss.max_loss_monotonic_ns > out->max_loss_monotonic_ns)
	{
		out->max_loss_monotonic_ns = pub.progress_loss.max_loss_monotonic_ns;
	}
}

[[nodiscard]] bool WriteStallBundle(const char* out_dir, const ProgressPublication& pub, const TimelineSnapshot& tl,
                                    const WaitGraphSnapshot& wg, const ClassifierState& clf, const StallResult& result,
                                    const ProcessStatus& process, const ProtocolHealthSnapshot& health,
                                    BundleTrigger trigger, uint64_t generation, SupervisorResult* sr) noexcept
{
	BundleInput in {};
	in.publication       = &pub;
	in.timeline          = &tl;
	in.wait_graph        = &wg;
	in.classifier        = &clf;
	in.result            = &result;
	in.process           = process;
	in.health            = health;
	in.bundle_generation = generation;
	in.trigger           = trigger;
	in.logging_mode      = LoggingMode::Silent;
	BundlePath path {};
	const BundleWriteResult wr = WriteBundle(out_dir, in, &path);
	if (wr != BundleWriteResult::Ok)
	{
		return false;
	}
	++sr->bundles_written;
	CopyBundlePath(sr, path);
	std::fprintf(stdout, "kyty_devtools: bundle %s\n", path.bytes);
	return true;
}

} // namespace

SupervisorResult RunSupervisor(const SupervisorOptions& options) noexcept
{
	SupervisorResult result {};
	if (options.absolute_output_dir == nullptr || options.absolute_output_dir[0] != '/' || options.worker == nullptr ||
	    options.worker_argv == nullptr || options.worker_argc == 0u)
	{
		result.outcome = SupervisorOutcome::LaunchError;
		return result;
	}

	SharedMapping mapping;
	if (SharedMapping::CreateOwnerOnly(kProtocolMappingSize, &mapping) != ProcessOperationError::None)
	{
		result.outcome = SupervisorOutcome::LaunchError;
		return result;
	}

	uint8_t nonce[16] = {};
	if (SecureRandomFill(nonce, 16) != ProcessOperationError::None)
	{
		result.outcome = SupervisorOutcome::LaunchError;
		return result;
	}

	uint64_t self_pid = 0;
	uint64_t self_start = 0;
	if (!QuerySelfProcessIdentity(&self_pid, &self_start))
	{
		result.outcome = SupervisorOutcome::LaunchError;
		return result;
	}

	ParentProtocolInit init {};
	init.supervisor_pid         = self_pid;
	init.supervisor_start_token = self_start;
	std::memcpy(init.nonce, nonce, 16);
	init.requested_mode = options.mode;

	auto view = mapping.MutableView();
	if (InitializeProtocolOwner(view, init) != ProtocolResult::Ok)
	{
		result.outcome = SupervisorOutcome::ProtocolError;
		return result;
	}

	int pipes[2] = {-1, -1};
	if (::pipe(pipes) != 0)
	{
		result.outcome = SupervisorOutcome::LaunchError;
		return result;
	}
	// Child inherits read end (pipes[0]); parent keeps write end (pipes[1]) for liveness.

	BootstrapMetadata boot_meta {};
	boot_meta.platform        = BootstrapPlatform::Posix;
	boot_meta.mapping_handle  = 3;
	boot_meta.liveness_handle = 4;
	std::memcpy(boot_meta.nonce.bytes, nonce, 16);
	BootstrapText boot {};
	if (!EncodeBootstrapMetadata(boot_meta, &boot))
	{
		::close(pipes[0]);
		::close(pipes[1]);
		result.outcome = SupervisorOutcome::LaunchError;
		return result;
	}

	LaunchOptions launch {};
	launch.executable  = options.worker;
	launch.argv        = options.worker_argv;
	launch.argc        = options.worker_argc;
	launch.bootstrap   = boot;
	launch.mapping_fd  = static_cast<int>(mapping.InheritableHandle());
	launch.liveness_fd = pipes[0];

	LaunchResult lr = ProcessLauncher::Launch(launch);
	::close(pipes[0]); // parent does not need child read end
	if (lr.error != ProcessOperationError::None)
	{
		::close(pipes[1]);
		result.outcome        = SupervisorOutcome::LaunchError;
		result.platform_error = lr.platform_error;
		return result;
	}
	ProcessIdentity launched_identity {};
	const bool launch_identity_valid = QueryProcessIdentity(lr.process, &launched_identity) == ProcessIdentityError::None;

	const uint64_t t_launch = MonoNs();
	bool           handshook = false;
	WorkerHandshake hs {};
	while (!handshook)
	{
		if (AcceptWorkerHandshake(mapping.MutableView(), init, &hs) == ProtocolResult::Ok)
		{
			ProcessIdentity child_identity {};
			if (QueryProcessIdentity(lr.process, &child_identity) != ProcessIdentityError::None && launch_identity_valid)
			{
				child_identity = launched_identity;
			}
			if ((child_identity.pid == 0u || child_identity.start_token == 0u) ||
				child_identity.pid != hs.worker_pid || child_identity.start_token != hs.worker_start_token)
			{
				mapping.Close();
				::close(pipes[1]);
				pipes[1] = -1;
				ProcessObservation terminal {};
				(void)ProcessLauncher::Wait(&lr.process, &terminal);
				result.process = terminal.status;
				result.outcome = SupervisorOutcome::WorkerHandshakeFailed;
				return result;
			}
			handshook = true;
			break;
		}
		ProcessObservation obs {};
		if (ProcessLauncher::Poll(&lr.process, &obs) != ProcessOperationError::None)
		{
			::close(pipes[1]);
			result.outcome = SupervisorOutcome::LaunchError;
			return result;
		}
		if (obs.status.liveness == ProcessLiveness::Terminated)
		{
			::close(pipes[1]);
			result.process = obs.status;
			result.outcome = SupervisorOutcome::WorkerHandshakeFailed;
			return result;
		}
		if (MonoNs() - t_launch >= options.handshake_timeout_ns)
		{
			// Close diagnostics; keep process ownership until one wait.
			mapping.Close();
			// Drop liveness write end so child can observe disconnect.
			::close(pipes[1]);
			pipes[1] = -1;
			ProcessObservation terminal {};
			(void)ProcessLauncher::Wait(&lr.process, &terminal);
			result.process = terminal.status;
			result.outcome = SupervisorOutcome::WorkerHandshakeFailed;
			return result;
		}
		SleepNs(10'000'000ull); // 10 ms
	}

	ClassifierState clf {};
	StallSettings   settings {};
	settings.suspected_after_ns = options.suspicion_ns;
	settings.confirmed_after_ns = options.confirmation_ns;
	settings.heartbeat_stale_ns = options.suspicion_ns;

	ProgressPublication last_pub {};
	TimelineSnapshot    last_tl {};
	bool                have_pub          = false;
	bool                stall_bundle_done = false;
	uint64_t            bundle_gen        = 1;
	uint64_t            samples           = 0;
	ProtocolReadLossState read_loss {};

	for (;;)
	{
		ProcessObservation obs {};
		if (ProcessLauncher::Poll(&lr.process, &obs) != ProcessOperationError::None)
		{
			result.outcome = SupervisorOutcome::LaunchError;
			break;
		}

		if (obs.status.error != ProcessStatusError::None)
		{
			// Decode error: one bundle from last coherent evidence; no terminal Observe.
			result.process = obs.status;
			result.outcome = SupervisorOutcome::StatusDecodeError;
			if (have_pub && options.absolute_output_dir != nullptr)
			{
				ProtocolHealthSnapshot health {};
				(void)ReadProtocolHealth(mapping.View(), &health);
				StallResult empty_result {};
				WaitGraphSnapshot wg {};
				(void)WriteStallBundle(options.absolute_output_dir, last_pub, last_tl, wg, clf, empty_result,
				                       obs.status, health, BundleTrigger::StatusDecodeError, bundle_gen++, &result);
			}
			(void)ProcessLauncher::Wait(&lr.process, &obs);
			result.process = obs.status;
			break;
		}

		if (obs.status.liveness == ProcessLiveness::Terminated)
		{
			result.process = obs.status;
			// One terminal Observe + optional bundle.
			ObservationInput in {};
			if (have_pub)
			{
				in.progress = last_pub.progress;
			}
			in.process        = obs.status;
			in.sample_time_ns = MonoNs();
			in.heartbeat_ns   = in.sample_time_ns;
			StallResult sr    = Observe(in, settings, &clf);
			ProtocolHealthSnapshot health {};
			(void)ReadProtocolHealth(mapping.View(), &health);
			WaitGraphSnapshot wg {};
			BundleTrigger trig = BundleTrigger::ProcessTerminal;
			if (have_pub)
			{
				(void)WriteStallBundle(options.absolute_output_dir, last_pub, last_tl, wg, clf, sr, obs.status, health,
				                       trig, bundle_gen++, &result);
			}
			result.outcome = OutcomeFromProcess(obs.status);
			break;
		}

		// Live sample.
		ProgressPublication pub {};
		TimelineSnapshot    tl {};
		const ProtocolResult pr = ReadProgressPublication(mapping.View(), &read_loss, &pub);
		if (pr == ProtocolResult::Ok)
		{
			last_pub  = pub;
			have_pub  = true;
			(void)ReadTimeline(mapping.View(), &read_loss, &tl);
			last_tl = tl;
		}

		ProtocolHealthSnapshot health {};
		(void)ReadProtocolHealth(mapping.View(), &health);

		ObservationInput in {};
		if (have_pub)
		{
			in.progress = last_pub.progress;
			ComposeLoss(last_pub, health, read_loss, &in.loss);
		}
		in.process        = obs.status;
		in.sample_time_ns = MonoNs();
		// Heartbeat: use latest progress last_change max as publisher liveness proxy.
		uint64_t hb = in.sample_time_ns;
		if (have_pub && last_pub.progress.count > 0u)
		{
			hb = 0;
			for (uint32_t i = 0; i < last_pub.progress.count; ++i)
			{
				if (last_pub.progress.entries[i].record.last_change_ns > hb)
				{
					hb = last_pub.progress.entries[i].record.last_change_ns;
				}
			}
			if (hb == 0u)
			{
				hb = in.sample_time_ns;
			}
		}
		// For blocked-lane, last_change is frozen; parent sample time advances so
		// heartbeat based on last_change becomes stale. Prefer sample time when
		// publications keep succeeding (publisher is alive).
		if (pr == ProtocolResult::Ok)
		{
			hb = in.sample_time_ns;
		}
		in.heartbeat_ns = hb;

		StallResult sr = Observe(in, settings, &clf);

		if (!stall_bundle_done && sr.category != StallCategory::None && sr.category != StallCategory::HealthyIdle &&
		    sr.confirmed_ns != 0u && have_pub)
		{
			WaitGraphSnapshot wg {};
			if (WriteStallBundle(options.absolute_output_dir, last_pub, last_tl, wg, clf, sr, obs.status, health,
			                     BundleTrigger::ConfirmedStall, bundle_gen++, &result))
			{
				if (options.capture_stall_once)
				{
					stall_bundle_done = true;
				}
			} else
			{
				result.outcome = SupervisorOutcome::BundleError;
				// Still continue owning child.
			}
		}

		++samples;
		if (options.max_samples != 0u && samples >= options.max_samples)
		{
			// Test/self-test bound: wait for child after signaling disconnect.
			::close(pipes[1]);
			pipes[1] = -1;
			ProcessObservation terminal {};
			(void)ProcessLauncher::Wait(&lr.process, &terminal);
			result.process = terminal.status;
			if (result.outcome != SupervisorOutcome::BundleError)
			{
				result.outcome = OutcomeFromProcess(terminal.status);
			}
			break;
		}

		SleepNs(options.sample_period_ns);
	}

	if (pipes[1] >= 0)
	{
		::close(pipes[1]);
	}
	mapping.Close();
	return result;
}

SupervisorResult RunMeasurement(const SupervisorOptions& options, uint64_t duration_ns) noexcept
{
	// Duration-bounded run: same launch path, stop after duration via interrupt + wait.
	SupervisorOptions opt = options;
	if (duration_ns == 0u)
	{
		SupervisorResult r {};
		r.outcome = SupervisorOutcome::LaunchError;
		return r;
	}
	// Approximate via max_samples from period.
	if (opt.sample_period_ns == 0u)
	{
		opt.sample_period_ns = 250'000'000ull;
	}
	opt.max_samples = duration_ns / opt.sample_period_ns;
	if (opt.max_samples == 0u)
	{
		opt.max_samples = 1;
	}
	return RunSupervisor(opt);
}

int RunSelfTest(const char* synthetic_mode, const char* absolute_output_dir, uint64_t suspected_ms,
                uint64_t confirmed_ms, uint64_t cleanup_ms) noexcept
{
	if (synthetic_mode == nullptr || absolute_output_dir == nullptr || absolute_output_dir[0] != '/')
	{
		return 2;
	}

	// Re-exec self as synthetic worker through the host executable-path adapter.
	char self_path[512] = {};
	if (!QueryCurrentExecutablePath(self_path, sizeof(self_path)))
	{
		return 2;
	}

	const char* argv[] = {self_path, "synthetic", synthetic_mode, nullptr};
	SupervisorOptions opt {};
	opt.absolute_output_dir = absolute_output_dir;
	opt.worker              = self_path;
	opt.worker_argv         = argv;
	opt.worker_argc         = 3;
	opt.mode                = RecordingMode::Full;
	opt.sample_period_ns    = 20'000'000ull; // 20 ms
	opt.suspicion_ns        = suspected_ms * 1'000'000ull;
	opt.confirmation_ns     = confirmed_ms * 1'000'000ull;
	opt.handshake_timeout_ns = 2'000'000'000ull;
	// Bound runtime for blocked-lane capture.
	const uint64_t window_ms = confirmed_ms + cleanup_ms;
	opt.max_samples          = (window_ms * 1'000'000ull) / opt.sample_period_ns + 5u;
	opt.capture_stall_once   = 1;

	SupervisorResult r = RunSupervisor(opt);

	if (std::strcmp(synthetic_mode, "blocked-lane") == 0)
	{
		// Expect at least one stall bundle; child may still exit 0 after bound.
		if (r.bundles_written == 0u)
		{
			std::fprintf(stderr, "self-test blocked-lane: no bundle written\n");
			return 1;
		}
		return 0;
	}
	if (std::strcmp(synthetic_mode, "normal-exit") == 0)
	{
		return (r.outcome == SupervisorOutcome::ChildExited && r.process.code == 0u) ? 0 : 1;
	}
	if (std::strcmp(synthetic_mode, "crash") == 0)
	{
		return (r.outcome == SupervisorOutcome::ChildCrashed) ? 0 : 1;
	}
	if (std::strcmp(synthetic_mode, "privacy-canary") == 0)
	{
		return (r.outcome == SupervisorOutcome::ChildExited) ? 0 : 1;
	}
	// Other modes: success if no launch/protocol hard failure.
	return (r.outcome == SupervisorOutcome::LaunchError || r.outcome == SupervisorOutcome::ProtocolError) ? 1 : 0;
}

} // namespace Kyty::DevTools
