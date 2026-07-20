#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTHOSTACCESSGATE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTHOSTACCESSGATE_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Coordinates VideoOut operations that can retain or consume guest-backed
// buffers with exclusive host lifetime changes such as KernelMunmap.
class VideoOutHostAccessGate final
{
public:
	class AccessPin final
	{
	public:
		AccessPin() = default;
		AccessPin(AccessPin&& other) noexcept;
		AccessPin& operator=(AccessPin&& other) noexcept;
		~AccessPin();

		AccessPin(const AccessPin&)            = delete;
		AccessPin& operator=(const AccessPin&) = delete;

		void Reset();

	private:
		friend class VideoOutHostAccessGate;
		explicit AccessPin(VideoOutHostAccessGate* gate): m_gate(gate) {}

		VideoOutHostAccessGate* m_gate = nullptr;
	};

	class QuiescePin final
	{
	public:
		QuiescePin() = default;
		QuiescePin(QuiescePin&& other) noexcept;
		QuiescePin& operator=(QuiescePin&& other) noexcept;
		~QuiescePin();

		QuiescePin(const QuiescePin&)            = delete;
		QuiescePin& operator=(const QuiescePin&) = delete;

		void Reset();

	private:
		friend class VideoOutHostAccessGate;
		explicit QuiescePin(VideoOutHostAccessGate* gate): m_gate(gate) {}

		VideoOutHostAccessGate* m_gate = nullptr;
	};

	VideoOutHostAccessGate()  = default;
	~VideoOutHostAccessGate() = default;

	KYTY_CLASS_NO_COPY(VideoOutHostAccessGate);

	[[nodiscard]] AccessPin  Acquire();
	[[nodiscard]] QuiescePin Quiesce();

private:
	void ReleaseAccess();
	void EndQuiesce();

	Core::Mutex   m_mutex;
	Core::CondVar m_state_changed;
	uint32_t      m_active_accesses = 0;
	bool          m_quiescing       = false;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTHOSTACCESSGATE_H_ */
