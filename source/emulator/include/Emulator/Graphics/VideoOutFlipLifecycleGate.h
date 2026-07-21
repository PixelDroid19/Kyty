#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTFLIPLIFECYCLEGATE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTFLIPLIFECYCLEGATE_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class VideoOutFlipLifecycleGate final
{
public:
	static constexpr uint32_t MAX_OWNERS = 2;

	VideoOutFlipLifecycleGate()  = default;
	~VideoOutFlipLifecycleGate() = default;

	KYTY_CLASS_NO_COPY(VideoOutFlipLifecycleGate);

	void Accept(const void* owner);
	void Complete(const void* owner);
	void WaitUntilIdle(const void* owner);

private:
	struct OwnerState
	{
		const void* owner        = nullptr;
		uint32_t    active_flips = 0;
	};

	[[nodiscard]] OwnerState* FindOwner(const void* owner);

	Core::Mutex   m_mutex;
	Core::CondVar m_idle_cond_var;
	OwnerState    m_owners[MAX_OWNERS];
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTFLIPLIFECYCLEGATE_H_ */
