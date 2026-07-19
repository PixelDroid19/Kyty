#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTMATERIALIZATIONGATE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTMATERIALIZATIONGATE_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class VideoOutMaterializationGate final
{
public:
	class Pin final
	{
	public:
		Pin() = default;
		Pin(Pin&& other) noexcept;
		Pin& operator=(Pin&& other) noexcept;
		~Pin();

		Pin(const Pin&)            = delete;
		Pin& operator=(const Pin&) = delete;

		void Reset();

	private:
		friend class VideoOutMaterializationGate;
		explicit Pin(VideoOutMaterializationGate* gate): m_gate(gate) {}

		VideoOutMaterializationGate* m_gate = nullptr;
	};

	VideoOutMaterializationGate()  = default;
	~VideoOutMaterializationGate() = default;

	KYTY_CLASS_NO_COPY(VideoOutMaterializationGate);

	[[nodiscard]] Pin Acquire();

	// The owner must prevent new acquisitions before waiting. VideoOutContext does so while holding its context mutex after marking the
	// handle closed.
	void WaitUntilIdle();

private:
	void Release();

	Core::Mutex   m_mutex;
	Core::CondVar m_idle_cond_var;
	uint32_t      m_active_pins = 0;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTMATERIALIZATIONGATE_H_ */
