#pragma once

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#include <array>
#include <cstdint>
#include <mutex>

namespace Kyty::UnitTest::GraphicsRetirementHelpers {

using namespace Libs::Graphics;

struct RetirementTestFixtureState
{
	static constexpr uint32_t kMaxBackings = 132u;

	std::array<uint32_t, kMaxBackings> delete_counts {};
	uint32_t                           writeback_calls = 0;
};

struct RetirementTestBacking
{
	RetirementTestFixtureState* fixture = nullptr;
	uint32_t                    token   = 0;
};

struct RetirementTestGpuObject final: public GpuObject
{
	RetirementTestGpuObject(RetirementTestFixtureState* fixture, uint32_t token, GpuMemoryObjectType object_type,
	                        bool has_write_back = false)
	    : m_has_write_back(has_write_back)
	{
		type       = object_type;
		params[0]  = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fixture));
		params[1]  = token;
		read_only  = true;
		check_hash = false;
	}

	bool Equal(const uint64_t* other) const override { return other != nullptr && other[0] == params[0] && other[1] == params[1]; }

	create_func_t GetCreateFunc() const override
	{
		return [](GraphicContext* /*ctx*/, const uint64_t* params, const uint64_t* /*vaddr*/, const uint64_t* /*size*/, int /*vaddr_num*/,
		          VulkanMemory* /*mem*/) -> void*
		{
			if (params == nullptr)
			{
				return nullptr;
			}
			auto* fixture = reinterpret_cast<RetirementTestFixtureState*>(static_cast<uintptr_t>(params[0]));
			if (fixture == nullptr)
			{
				return nullptr;
			}
			auto* backing    = new RetirementTestBacking;
			backing->fixture = fixture;
			backing->token   = static_cast<uint32_t>(params[1]);
			if (backing->token >= RetirementTestFixtureState::kMaxBackings)
			{
				delete backing;
				return nullptr;
			}
			return backing;
		};
	}

	create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; }

	write_back_func_t GetWriteBackFunc() const override
	{
		if (!m_has_write_back)
		{
			return nullptr;
		}
		return [](GraphicContext* /*ctx*/, const uint64_t* params, void* /*obj*/, const uint64_t* /*vaddr*/, const uint64_t* /*size*/,
		          int /*vaddr_num*/) -> GpuWritebackResult
		{
			if (params != nullptr)
			{
				auto* fixture = reinterpret_cast<RetirementTestFixtureState*>(static_cast<uintptr_t>(params[0]));
				if (fixture != nullptr)
				{
					fixture->writeback_calls++;
				}
			}
			return {};
		};
	}

	delete_func_t GetDeleteFunc() const override
	{
		return [](GraphicContext* /*ctx*/, void* obj, VulkanMemory* /*mem*/)
		{
			auto* backing = static_cast<RetirementTestBacking*>(obj);
			if (backing != nullptr)
			{
				if (backing->fixture != nullptr && backing->token < backing->fixture->delete_counts.size())
				{
					backing->fixture->delete_counts[backing->token]++;
				}
				delete backing;
			}
		};
	}

	update_func_t GetUpdateFunc() const override { return nullptr; }

private:
	bool m_has_write_back = false;
};

class ScopedRetirementFixture final
{
public:
	// Register one dedicated range and serialize its owners. ObjectsOnly cleanup
	// removes all owned backings before reuse, without requiring a global unmap
	// gate or accumulating heap records on gtest_repeat.
	explicit ScopedRetirementFixture(GraphicContext* ctx, uint64_t heap_size): m_ctx(ctx), m_lock(RangeMutex())
	{
		EXIT_IF(heap_size == 0 || heap_size > kRangeSize);
		static std::once_flag registered;
		std::call_once(registered, [] { GpuMemorySetAllocatedRange(kRangeBase, kRangeSize); });
	}

	~ScopedRetirementFixture() { GpuMemoryFree(m_ctx, kRangeBase, kRangeSize); }

	ScopedRetirementFixture(const ScopedRetirementFixture&)            = delete;
	ScopedRetirementFixture& operator=(const ScopedRetirementFixture&) = delete;

	[[nodiscard]] uint64_t                    Base() const { return kRangeBase; }
	[[nodiscard]] RetirementTestFixtureState& State() { return m_state; }

private:
	static constexpr uint64_t kRangeBase = 0x0000005300000000ull;
	static constexpr uint64_t kRangeSize = 0x4000ull;
	static std::mutex&        RangeMutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	GraphicContext*              m_ctx = nullptr;
	std::unique_lock<std::mutex> m_lock;
	RetirementTestFixtureState   m_state {};
};

[[nodiscard]] inline bool GpuMemoryHasExactTestBacking(uint64_t address, uint64_t size, GpuMemoryObjectType type, void* backing)
{
	if (backing == nullptr)
	{
		return false;
	}
	const auto found = GpuMemoryFindObjects(address, size, type, true, false);
	return found.Size() == 1u && found[0].obj == backing;
}

} // namespace Kyty::UnitTest::GraphicsRetirementHelpers
