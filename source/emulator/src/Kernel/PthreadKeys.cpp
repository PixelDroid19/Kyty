#include "PthreadInternal.h"

#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Kernel/Errors.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel {

class ScopedPthreadKeyDestructorPass
{
public:
	ScopedPthreadKeyDestructorPass()
	{
		EXIT_IF(g_pthread_key_destructors_active);
		g_pthread_key_destructors_active = true;
	}

	~ScopedPthreadKeyDestructorPass() { g_pthread_key_destructors_active = false; }

	KYTY_CLASS_NO_COPY(ScopedPthreadKeyDestructorPass);
};

bool PthreadKeys::Create(int* key, pthread_key_destructor_func_t destructor)
{
	EXIT_IF(key == nullptr);

	Core::LockGuard lock(m_mutex);

	for (int index = 0; index < KEYS_MAX; index++)
	{
		if (!m_keys[index].used)
		{
			*key                     = index;
			m_keys[index].used       = true;
			m_keys[index].destructor = destructor;
			m_keys[index].specific_values.Clear();
			return true;
		}
	}

	return false;
}

bool PthreadKeys::Delete(int key)
{
	Core::LockGuard lock(m_mutex);

	if (key < 0 || key >= KEYS_MAX || !m_keys[key].used)
	{
		return false;
	}

	m_keys[key].used       = false;
	m_keys[key].destructor = nullptr;
	m_keys[key].specific_values.Clear();

	return true;
}

void PthreadKeys::Destruct(int thread_id, void* guest_stack_top)
{
	if (g_pthread_key_destructors_active)
	{
		// A destructor may legitimately call PthreadExit, which runs this
		// cleanup handler again. It must not recurse through callbacks, but the
		// retiring thread's maps still need reclamation before that exit finishes.
		Core::LockGuard lock(m_mutex);
		EraseSpecificValuesForThreadLocked(thread_id);
		return;
	}
	EXIT_IF(guest_stack_top == nullptr);

	ScopedPthreadKeyDestructorPass destructor_pass;

	struct CallInfo
	{
		pthread_key_destructor_func_t destructor;
		void*                         data;
	};

	for (int iter = 0; iter < DESTRUCTOR_ITERATIONS; iter++)
	{
		Vector<CallInfo> delete_list;

		{
			Core::LockGuard lock(m_mutex);

			for (auto& key: m_keys)
			{
				if (key.used && key.destructor != nullptr)
				{
					for (auto& v: key.specific_values)
					{
						if (v.thread_id == thread_id && v.data != nullptr)
						{
							delete_list.Add(CallInfo({key.destructor, v.data}));
							// POSIX permits another destructor pass only if this callback
							// stores a non-null value again.
							v.data = nullptr;
						}
					}
				}
			}
		}

		if (delete_list.IsEmpty())
		{
			break;
		}

		for (auto& d: delete_list)
		{
			// Destructors are guest callbacks. Invoke them on the retiring guest
			// stack so ABI stack-relative state and the exiting pthread identity
			// remain valid across all four POSIX destructor passes.
			(void)::Kyty::Emulator::GuestRuntimePort::InvokeOnStack(reinterpret_cast<uint64_t>(d.destructor),
			                                                        reinterpret_cast<uint64_t>(d.data), 0, 0, guest_stack_top);
		}
	}

	Core::LockGuard lock(m_mutex);
	EraseSpecificValuesForThreadLocked(thread_id);
}

void PthreadKeys::EraseSpecificValuesForThreadLocked(int thread_id)
{
	// A nullptr value does not schedule another callback, but its map entry is
	// still per-thread state. Remove every entry for this retired thread after
	// the final pass so repeated workers cannot accumulate stale key records.
	for (auto& key: m_keys)
	{
		for (uint32_t index = key.specific_values.Size(); index > 0; index--)
		{
			if (key.specific_values.At(index - 1).thread_id == thread_id)
			{
				key.specific_values.RemoveAt(index - 1);
			}
		}
	}
}

uint32_t PthreadKeys::CountSpecificValuesForThread(int thread_id)
{
	Core::LockGuard lock(m_mutex);
	uint32_t        count = 0;
	for (const auto& key: m_keys)
	{
		for (const auto& value: key.specific_values)
		{
			if (value.thread_id == thread_id)
			{
				count++;
			}
		}
	}
	return count;
}

bool PthreadKeys::Set(int key, int thread_id, void* data)
{
	Core::LockGuard lock(m_mutex);

	if (key < 0 || key >= KEYS_MAX || !m_keys[key].used)
	{
		return false;
	}

	for (auto& v: m_keys[key].specific_values)
	{
		if (v.thread_id == thread_id)
		{
			v.data = data;
			return true;
		}
	}

	m_keys[key].specific_values.Add(Map({thread_id, data}));

	return true;
}

bool PthreadKeys::Get(int key, int thread_id, void** data)
{
	EXIT_IF(data == nullptr);

	Core::LockGuard lock(m_mutex);

	if (key < 0 || key >= KEYS_MAX || !m_keys[key].used)
	{
		return false;
	}

	for (auto& v: m_keys[key].specific_values)
	{
		if (v.thread_id == thread_id)
		{
			*data = v.data;
			return true;
		}
	}

	*data = nullptr;

	return true;
}

} // namespace Kyty::Kernel

#endif // KYTY_EMU_ENABLED
