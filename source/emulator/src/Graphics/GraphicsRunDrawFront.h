#ifndef EMULATOR_SRC_GRAPHICS_GRAPHICSRUNDRAWFRONT_H_
#define EMULATOR_SRC_GRAPHICS_GRAPHICSRUNDRAWFRONT_H_

#include "Emulator/Common.h"
#include "Emulator/Graphics/Shader.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

class CommandProcessor;

// What a packet does, as far as a replay that follows only register state is concerned.
enum class DrawFrontPacket : uint8_t
{
	Padding,     // a header the processor steps over without a body
	Registers,   // changes only the registers and the index state of its processor
	IndexedDraw, // a direct indexed draw
	Other,
};

// `packet` is followed by `available_dw - 1` more dwords of its run.
[[nodiscard]] DrawFrontPacket DrawFrontClassifyPacket(const uint32_t* packet, uint32_t available_dw);

// Prepares the shader inputs of indexed draws ahead of a command processor. A second thread replays,
// on a shadow processor, the register packets that follow the processor's position and analyses each
// indexed draw it meets, up to the first packet that is neither. The shadow starts from the registers
// of the processor and runs the same handlers over the same packets, so it holds the processor's
// registers at each draw. The processor takes the inputs of a draw only when no host write of guest
// memory was announced since their analysis began.
class DrawFront
{
public:
	explicit DrawFront(CommandProcessor* shadow);
	~DrawFront() = delete;

	KYTY_CLASS_NO_COPY(DrawFront);

	// Processor thread. The shadow processor may be written while the replay is not live.
	[[nodiscard]] bool              Live() const { return m_live; }
	[[nodiscard]] CommandProcessor* Shadow() const { return m_shadow; }
	// Replays from `draw_packet`, the draw the processor is recording, with the registers the shadow holds.
	void Start(const uint32_t* draw_packet, const uint32_t* run_end);
	// Returns once the replay reads no packet, register or guest memory.
	void Stop();
	// The inputs prepared for the draw at `draw_packet`, or null. They stay valid until Release.
	[[nodiscard]] PreparedDrawInputs* Take(const uint32_t* draw_packet, uint32_t instance_count);
	void                              Release();

	// Replay thread, from the indexed draw of the shadow processor.
	void Prepare(const HW::Context& ctx, const HW::UserConfig& ucfg, HW::Shader* sh_ctx, uint32_t index_count, uint32_t instance_count);

private:
	struct Record
	{
		const uint32_t*    packet            = nullptr;
		// A latch the processor resolves between packets can change it.
		uint32_t           instance_count    = 0;
		uint64_t           guest_write_epoch = 0;
		PreparedDrawInputs inputs;
	};
	// The replay runs about four draws for each one the processor records.
	static constexpr uint64_t kRecords = 64;

	static void ThreadRun(void* data);
	// Replays from `packet` while `generation` is current. Returns the draw packet to replay again
	// once a record is free, or null when the replay ends.
	const uint32_t* Replay(uint64_t generation, const uint32_t* packet);
	template <typename Condition>
	void Sleep(const Condition& condition);
	void Wake();

	CommandProcessor* const   m_shadow;
	std::unique_ptr<Record[]> m_records;

	// Processor thread.
	bool     m_live  = false;
	uint64_t m_taken = 0;
	// The host-write epoch this thread last saw, and the first record prepared wholly after it saw it:
	// an earlier record of the same epoch may have read bytes a write was still replacing.
	uint64_t m_epoch_seen = 0;
	uint64_t m_fresh_from = 0;

	// Written by Start while the replay is idle.
	const uint32_t* m_resume = nullptr;
	const uint32_t* m_end    = nullptr;

	// Start and Stop advance the generation; the replay runs a job only while it is the generation.
	std::atomic<uint64_t>        m_generation {0};
	std::atomic<uint64_t>        m_job {0};
	std::atomic<bool>            m_reading {false};
	std::atomic<uint64_t>        m_prepared {0};
	std::atomic<uint64_t>        m_released {0};
	std::atomic<const uint32_t*> m_processor_packet {nullptr};

	std::mutex              m_mutex;
	std::condition_variable m_wake;
	std::atomic<bool>       m_sleeping {false};

	// Replay thread.
	const uint32_t* m_replay_packet = nullptr;
	bool            m_replay_parked = false;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_GRAPHICSRUNDRAWFRONT_H_ */
