#include "PthreadInternal.h"

#include "Emulator/Kernel/Errors.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel {

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

void PthreadKeys::Destruct(int thread_id)
{
	Core::LockGuard lock(m_mutex);

	struct CallInfo
	{
		pthread_key_destructor_func_t destructor;
		void*                         data;
	};

	for (int iter = 0; iter < DESTRUCTOR_ITERATIONS; iter++)
	{
		Vector<CallInfo> delete_list;

		for (auto& key: m_keys)
		{
			if (key.used && key.destructor != nullptr)
			{
				for (auto& v: key.specific_values)
				{
					if (v.thread_id == thread_id && v.data != nullptr)
					{
						delete_list.Add(CallInfo({key.destructor, v.data}));
					}
				}
			}
		}

		if (delete_list.IsEmpty())
		{
			return;
		}

		for (auto& d: delete_list)
		{
			d.destructor(d.data);
		}
	}
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
