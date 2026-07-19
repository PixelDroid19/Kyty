#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/Objects/ExactStagingPool.h"

#include <cstdint>
#include <unordered_set>

UT_BEGIN(EmulatorExactStagingPool);

using namespace Libs::Graphics;

namespace {

struct FakeResource
{
	uint64_t size = 0;
};

struct FakeBackend
{
	uint32_t creates  = 0;
	uint32_t maps     = 0;
	uint32_t unmaps   = 0;
	uint32_t destroys = 0;

	std::unordered_set<FakeResource*> live;

	~FakeBackend()
	{
		for (auto* resource: live)
		{
			delete resource;
		}
	}

	static ExactStagingResource Create(void* user, uint64_t size)
	{
		auto* backend  = static_cast<FakeBackend*>(user);
		auto* resource = new FakeResource {size};
		backend->creates++;
		backend->maps++;
		backend->live.insert(resource);
		return {resource, resource};
	}

	static void Destroy(void* user, const ExactStagingResource& resource)
	{
		auto* backend = static_cast<FakeBackend*>(user);
		auto* object  = static_cast<FakeResource*>(resource.object);
		EXPECT_EQ(resource.object, resource.mapped);
		EXPECT_EQ(backend->live.erase(object), 1u);
		backend->unmaps++;
		backend->destroys++;
		delete object;
	}

	[[nodiscard]] ExactStagingPoolBackend Interface() { return {this, &FakeBackend::Create, &FakeBackend::Destroy}; }
};

} // namespace

TEST(EmulatorExactStagingPool, ExactSizeReusesOnePersistentMapping)
{
	FakeBackend      backend;
	ExactStagingPool pool(2, backend.Interface());

	const auto first = pool.Acquire(4096);
	ASSERT_TRUE(first.IsValid());
	EXPECT_TRUE(first.IsPooled());
	EXPECT_TRUE(pool.Release(first));

	const auto second = pool.Acquire(4096);
	ASSERT_TRUE(second.IsValid());
	EXPECT_EQ(second.resource.object, first.resource.object);
	EXPECT_EQ(backend.creates, 1u);
	EXPECT_EQ(backend.maps, 1u);
	EXPECT_TRUE(pool.Release(second));
	EXPECT_TRUE(pool.DeleteAll());
	EXPECT_EQ(backend.unmaps, 1u);
	EXPECT_EQ(backend.destroys, 1u);
}

TEST(EmulatorExactStagingPool, DifferentSizeNeverReusesBacking)
{
	FakeBackend      backend;
	ExactStagingPool pool(1, backend.Interface());

	const auto first = pool.Acquire(4096);
	ASSERT_TRUE(pool.Release(first));
	const auto second = pool.Acquire(8192);
	ASSERT_TRUE(second.IsValid());

	EXPECT_EQ(backend.creates, 2u);
	EXPECT_EQ(backend.destroys, 1u);
	EXPECT_EQ(static_cast<FakeResource*>(second.resource.object)->size, 8192u);
	EXPECT_TRUE(pool.Release(second));
	EXPECT_TRUE(pool.DeleteAll());
}

TEST(EmulatorExactStagingPool, OccupiedSlotIsNeverReused)
{
	FakeBackend      backend;
	ExactStagingPool pool(1, backend.Interface());

	const auto occupied  = pool.Acquire(4096);
	const auto transient = pool.Acquire(4096);
	ASSERT_TRUE(occupied.IsPooled());
	ASSERT_TRUE(transient.IsValid());
	EXPECT_FALSE(transient.IsPooled());
	EXPECT_NE(transient.resource.object, occupied.resource.object);

	EXPECT_TRUE(pool.Release(transient));
	EXPECT_EQ(backend.destroys, 1u);
	EXPECT_TRUE(pool.Release(occupied));
	EXPECT_TRUE(pool.DeleteAll());
}

TEST(EmulatorExactStagingPool, CapacityIsBoundedAndIdleSlotIsReplaced)
{
	FakeBackend      backend;
	ExactStagingPool pool(2, backend.Interface());

	const auto first = pool.Acquire(1024);
	EXPECT_TRUE(pool.Release(first));
	const auto second = pool.Acquire(2048);
	EXPECT_TRUE(pool.Release(second));
	EXPECT_EQ(pool.Size(), 2u);

	const auto replacement = pool.Acquire(4096);
	EXPECT_TRUE(replacement.IsPooled());
	EXPECT_EQ(pool.Size(), 2u);
	EXPECT_EQ(backend.creates, 3u);
	EXPECT_EQ(backend.destroys, 1u);
	EXPECT_TRUE(pool.Release(replacement));
	EXPECT_TRUE(pool.DeleteAll());
}

TEST(EmulatorExactStagingPool, DeleteAllRejectsOccupiedSlotAndTearsDownAfterRelease)
{
	FakeBackend      backend;
	ExactStagingPool pool(2, backend.Interface());

	const auto lease = pool.Acquire(4096);
	EXPECT_FALSE(pool.DeleteAll());
	EXPECT_EQ(backend.destroys, 0u);
	EXPECT_TRUE(pool.Release(lease));
	EXPECT_TRUE(pool.DeleteAll());
	EXPECT_EQ(pool.Size(), 0u);
	EXPECT_EQ(backend.unmaps, 1u);
	EXPECT_EQ(backend.destroys, 1u);
}

TEST(EmulatorExactStagingPool, StaleAndDoubleReleaseAreRejected)
{
	FakeBackend      backend;
	ExactStagingPool pool(1, backend.Interface());

	const auto first = pool.Acquire(4096);
	EXPECT_TRUE(pool.Release(first));
	EXPECT_FALSE(pool.Release(first));

	const auto second = pool.Acquire(4096);
	EXPECT_NE(second.generation, first.generation);
	EXPECT_FALSE(pool.Release(first));
	EXPECT_TRUE(pool.Release(second));
	EXPECT_TRUE(pool.DeleteAll());
}

TEST(EmulatorExactStagingPool, ZeroSizeIsRejectedWithoutAllocation)
{
	FakeBackend      backend;
	ExactStagingPool pool(2, backend.Interface());

	EXPECT_FALSE(pool.Acquire(0).IsValid());
	EXPECT_EQ(backend.creates, 0u);
	EXPECT_TRUE(pool.DeleteAll());
}

UT_END();
