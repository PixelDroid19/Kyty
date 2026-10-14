#ifndef INCLUDE_KYTY_CORE_REFCOUNTER_H_
#define INCLUDE_KYTY_CORE_REFCOUNTER_H_

#include "Kyty/Core/Common.h"

#include <atomic>

namespace Kyty::Core {

// Shared ownership for copy-on-write containers. Only the count is shared
// between threads; mutating one container from two threads still needs the
// caller's own synchronization.
class RefCounter
{
public:
	RefCounter() = default;

	virtual ~RefCounter() = default;

	void Release() const
	{
		const uint32_t previous = m_refs.fetch_sub(1, std::memory_order_acq_rel);
		EXIT_IF(previous == 0);
		if (previous == 1)
		{
			delete this;
		}
	}

	template <class PtrType>
	void CopyPtr(PtrType** dst, PtrType* src) const
	{
		m_refs.fetch_add(1, std::memory_order_relaxed);
		*dst = src;
	}

	// The copy is taken before this reference is dropped, so another owner
	// releasing at the same time cannot free the source first.
	template <class PtrType>
	void CopyOnWrite(PtrType** data) const
	{
		if (m_refs.load(std::memory_order_acquire) > 1)
		{
			auto* copy = new PtrType(**data);
			Release();
			*data = copy;
		}
	}

	KYTY_CLASS_NO_COPY(RefCounter);

private:
	mutable std::atomic<uint32_t> m_refs {1};
};

} // namespace Kyty::Core

#endif /* INCLUDE_KYTY_CORE_REFCOUNTER_H_ */
