#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTFLIPQUEUEADMISSIONGATE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTFLIPQUEUEADMISSIONGATE_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class VideoOutFlipQueueAdmissionGate final
{
public:
	explicit VideoOutFlipQueueAdmissionGate(uint32_t capacity);
	~VideoOutFlipQueueAdmissionGate() = default;

	KYTY_CLASS_NO_COPY(VideoOutFlipQueueAdmissionGate);

	[[nodiscard]] bool TryAcquire();
	void               AcquireBlocking();
	void               Release();

private:
	Core::Mutex   m_mutex;
	Core::CondVar m_space_available;
	uint32_t      m_capacity = 0;
	uint32_t      m_active   = 0;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTFLIPQUEUEADMISSIONGATE_H_ */
