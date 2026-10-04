#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTFLIPPENDING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTFLIPPENDING_H_

#include <cstdint>

namespace Kyty::Libs::Graphics {

// SCE_VIDEO_OUT_BUFFER_INDEX_BLANK: a flip that displays a black frame.
constexpr int VIDEO_OUT_BUFFER_INDEX_BLANK = -1;
constexpr int VIDEO_OUT_BUFFER_INDEX_COUNT = 16;

// A flip names the blank index or a registered display buffer.
[[nodiscard]] constexpr bool VideoOutIsFlippableIndex(int index, bool registered)
{
	return index == VIDEO_OUT_BUFFER_INDEX_BLANK || (index >= 0 && index < VIDEO_OUT_BUFFER_INDEX_COUNT && registered);
}

// Guest-visible flip accounting of one VideoOut port. A flip is pending
// (flipPendingNum) from the moment the command processor decodes it
// (gcQueueNum) until the presenter completes it, so a guest that waits for
// IsFlipPending == 0 before unregistering its buffers never races a flip
// still in its command stream.
class VideoOutFlipPending final
{
public:
	void QueueGpuFlip() { m_gpu_queued++; }

	// The decoded flip reached the flip queue.
	[[nodiscard]] bool TakeGpuFlip()
	{
		if (m_gpu_queued <= 0)
		{
			return false;
		}
		m_gpu_queued--;
		return true;
	}

	[[nodiscard]] int32_t GcQueueNum() const { return m_gpu_queued; }
	[[nodiscard]] int32_t FlipPendingNum(int32_t queued_requests) const { return queued_requests + m_gpu_queued; }

private:
	int32_t m_gpu_queued = 0;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTFLIPPENDING_H_ */
