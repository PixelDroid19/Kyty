#ifndef EMULATOR_INCLUDE_EMULATOR_AVPLAYER_MEDIA_CLOCK_H_
#define EMULATOR_INCLUDE_EMULATOR_AVPLAYER_MEDIA_CLOCK_H_

#include <chrono>
#include <cstdint>

namespace Kyty::Emulator::AvPlayer {

class MediaClock final
{
public:
	using Clock     = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	static constexpr uint64_t GraceMilliseconds = 250;

	explicit MediaClock(TimePoint started_at = Clock::now()): last_video_request_at_(started_at) {}

	void Restart(TimePoint now = Clock::now()) { last_video_request_at_ = now; }
	void MarkVideoRequest(TimePoint now = Clock::now()) { last_video_request_at_ = now; }

	[[nodiscard]] bool HasPlayedOut(uint64_t duration_ms, uint64_t last_media_timestamp_ms,
	                                TimePoint now = Clock::now()) const
	{
		if (duration_ms == 0 || now < last_video_request_at_)
		{
			return false;
		}
		const uint64_t remaining_ms = duration_ms > last_media_timestamp_ms ? duration_ms - last_media_timestamp_ms : 0;
		const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_video_request_at_).count();
		if (idle < 0)
		{
			return false;
		}
		const uint64_t idle_ms = static_cast<uint64_t>(idle);
		return idle_ms >= remaining_ms && idle_ms - remaining_ms >= GraceMilliseconds;
	}

private:
	TimePoint last_video_request_at_;
};

} // namespace Kyty::Emulator::AvPlayer

#endif // EMULATOR_INCLUDE_EMULATOR_AVPLAYER_MEDIA_CLOCK_H_
