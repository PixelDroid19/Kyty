#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_EXACTSTAGINGPOOL_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_EXACTSTAGINGPOOL_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct ExactStagingResource
{
	void* object = nullptr;
	void* mapped = nullptr;
};

struct ExactStagingPoolBackend
{
	void* user = nullptr;

	// Backend callbacks run under the pool lock and must not call back into the pool.
	ExactStagingResource (*create)(void* user, uint64_t size)         = nullptr;
	void (*destroy)(void* user, const ExactStagingResource& resource) = nullptr;
};

struct ExactStagingLease
{
	ExactStagingResource resource;
	uint64_t             size       = 0;
	uint64_t             generation = 0;
	uint32_t             slot       = UINT32_MAX;

	[[nodiscard]] bool IsValid() const { return resource.object != nullptr && resource.mapped != nullptr && size != 0; }
	[[nodiscard]] bool IsPooled() const { return slot != UINT32_MAX; }
};

class ExactStagingPool
{
public:
	ExactStagingPool(uint32_t max_slots, ExactStagingPoolBackend backend);
	~ExactStagingPool();
	KYTY_CLASS_NO_COPY(ExactStagingPool);

	// Only an idle resource with the exact requested size can be reused.
	// Saturation preserves correctness with a transient resource.
	[[nodiscard]] ExactStagingLease Acquire(uint64_t size);
	[[nodiscard]] bool              Release(const ExactStagingLease& lease);

	// Explicit teardown is required while the backend device/context is live.
	[[nodiscard]] bool              DeleteAll();

	[[nodiscard]] uint32_t Size() const;

private:
	struct Slot
	{
		ExactStagingResource resource;
		uint64_t             size       = 0;
		uint64_t             generation = 0;
		uint64_t             last_use   = 0;
		bool                 in_use     = false;
	};

	struct Transient
	{
		ExactStagingResource resource;
		uint64_t             generation = 0;
	};

	ExactStagingLease CreatePooled(uint32_t slot, uint64_t size);

	mutable Core::Mutex m_mutex;

	Vector<Slot>            m_slots;
	Vector<Transient>       m_transients;
	uint32_t                m_max_slots = 0;
	uint64_t                m_sequence  = 0;
	ExactStagingPoolBackend m_backend;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_EXACTSTAGINGPOOL_H_ */
