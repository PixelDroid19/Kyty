#include "Emulator/Graphics/VideoOutFlipLifecycleGate.h"

#include "Kyty/Core/DbgAssert.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

VideoOutFlipLifecycleGate::OwnerState* VideoOutFlipLifecycleGate::FindOwner(const void* owner)
{
	for (auto& state: m_owners)
	{
		if (state.owner == owner)
		{
			return &state;
		}
	}
	return nullptr;
}

void VideoOutFlipLifecycleGate::Accept(const void* owner)
{
	EXIT_IF(owner == nullptr);
	Core::LockGuard lock(m_mutex);

	auto* state = FindOwner(owner);
	if (state == nullptr)
	{
		for (auto& candidate: m_owners)
		{
			if (candidate.owner == nullptr)
			{
				state        = &candidate;
				state->owner = owner;
				break;
			}
		}
	}

	EXIT_IF(state == nullptr);
	EXIT_IF(state->active_flips == UINT32_MAX);
	state->active_flips++;
}

void VideoOutFlipLifecycleGate::Complete(const void* owner)
{
	EXIT_IF(owner == nullptr);
	Core::LockGuard lock(m_mutex);

	auto* state = FindOwner(owner);
	EXIT_IF(state == nullptr || state->active_flips == 0);
	state->active_flips--;
	if (state->active_flips == 0)
	{
		state->owner = nullptr;
		m_idle_cond_var.SignalAll();
	}
}

void VideoOutFlipLifecycleGate::WaitUntilIdle(const void* owner)
{
	EXIT_IF(owner == nullptr);
	Core::LockGuard lock(m_mutex);
	while (FindOwner(owner) != nullptr)
	{
		m_idle_cond_var.Wait(&m_mutex);
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
