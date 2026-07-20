#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/SpirvBinaryCacheStore.h"
#include "Emulator/Graphics/ShaderTranslationCache.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

UT_BEGIN(EmulatorShaderTranslationCache);

using namespace Libs::Graphics;

namespace {

class ScopedShaderModuleCacheDirectory final
{
public:
	ScopedShaderModuleCacheDirectory()
	{
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		path             = std::filesystem::temp_directory_path() / ("kyty-shader-module-cache-test-" + std::to_string(nonce));
	}
	~ScopedShaderModuleCacheDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path path;
};

static ShaderId TestShaderId(uint32_t hash, uint32_t crc, uint32_t interface_id)
{
	ShaderId id;
	id.hash0 = hash;
	id.crc32 = crc;
	id.ids.Add(interface_id);
	return id;
}

} // namespace

TEST(EmulatorShaderTranslationCache, KeyTracksOnlyExactTranslationInputs)
{
	const ShaderId id = TestShaderId(1, 2, 3);
	const ShaderModuleKey base =
	    ShaderModuleKey::Create(id, ShaderModuleStage::Vertex, Config::ShaderOptimizationType::Performance, true);

	ShaderId copied = id;
	EXPECT_EQ(base, ShaderModuleKey::Create(copied, ShaderModuleStage::Vertex, Config::ShaderOptimizationType::Performance, true));
	EXPECT_NE(base, ShaderModuleKey::Create(id, ShaderModuleStage::Pixel, Config::ShaderOptimizationType::Performance, true));
	EXPECT_NE(base, ShaderModuleKey::Create(TestShaderId(1, 2, 4), ShaderModuleStage::Vertex,
	                                       Config::ShaderOptimizationType::Performance, true));
	EXPECT_NE(base, ShaderModuleKey::Create(id, ShaderModuleStage::Vertex, Config::ShaderOptimizationType::Size, true));
	EXPECT_NE(base, ShaderModuleKey::Create(id, ShaderModuleStage::Vertex, Config::ShaderOptimizationType::Performance, false));

	auto newer = base;
	newer.translator_version++;
	EXPECT_NE(base, newer);

	auto debug = base;
	debug.debug_printf_enabled = !base.debug_printf_enabled;
	EXPECT_NE(base, debug);
}

TEST(EmulatorShaderTranslationCache, ExactMissCompilesOnceAndHitDoesNotInvokeCompiler)
{
	ShaderTranslationCache cache(16);
	const auto key =
	    ShaderModuleKey::Create(TestShaderId(10, 20, 30), ShaderModuleStage::Compute, Config::ShaderOptimizationType::None, true);
	std::atomic<uint32_t> compiles {0};
	auto compiler = [&]
	{
		compiles.fetch_add(1);
		Vector<uint32_t> binary;
		binary.Add(0x07230203u);
		return binary;
	};

	const auto miss = cache.GetOrCompile(key, compiler);
	const auto hit  = cache.GetOrCompile(key, compiler);

	EXPECT_FALSE(miss.hit);
	EXPECT_TRUE(hit.hit);
	EXPECT_EQ(compiles.load(), 1u);
	ASSERT_EQ(miss.binary.Size(), 1u);
	EXPECT_EQ(hit.binary, miss.binary);
}

TEST(EmulatorShaderTranslationCache, PersistentModuleHitSkipsCompilerAcrossCacheInstances)
{
	ScopedShaderModuleCacheDirectory directory;
	SpirvBinaryCacheStore            store(directory.path);
	const auto key =
	    ShaderModuleKey::Create(TestShaderId(11, 22, 33), ShaderModuleStage::Pixel, Config::ShaderOptimizationType::Performance, true);
	std::atomic<uint32_t> compiles {0};
	auto compiler = [&]
	{
		compiles.fetch_add(1);
		return Vector<uint32_t> {0x07230203u, 0x00010500u, 0u, 4u, 0u};
	};

	{
		ShaderTranslationCache cold_cache(16, &store, false);
		EXPECT_FALSE(cold_cache.GetOrCompile(key, compiler).hit);
	}
	{
		ShaderTranslationCache warm_cache(16, &store, false);
		const auto             warm = warm_cache.GetOrCompile(key, compiler);
		EXPECT_TRUE(warm.hit);
		EXPECT_EQ(warm.binary, (Vector<uint32_t> {0x07230203u, 0x00010500u, 0u, 4u, 0u}));
	}
	EXPECT_EQ(compiles.load(), 1u);
}

TEST(EmulatorShaderTranslationCache, DebugPrintfModulesRemainSessionLocal)
{
	ScopedShaderModuleCacheDirectory directory;
	SpirvBinaryCacheStore            store(directory.path);
	const auto key = ShaderModuleKey::Create(TestShaderId(77, 88, 99), ShaderModuleStage::Pixel,
	                                         Config::ShaderOptimizationType::Performance, true, true);
	uint32_t   compiles = 0;
	auto compiler = [&]
	{
		++compiles;
		return Vector<uint32_t> {0x07230203u, 0x00010500u, compiles, 4u, 0u};
	};

	{
		ShaderTranslationCache first(16, &store, false);
		EXPECT_FALSE(first.GetOrCompile(key, compiler).hit);
		EXPECT_TRUE(first.GetOrCompile(key, compiler).hit);
	}
	{
		ShaderTranslationCache second(16, &store, false);
		EXPECT_FALSE(second.GetOrCompile(key, compiler).hit);
	}
	EXPECT_EQ(compiles, 2u);
	EXPECT_FALSE(std::filesystem::exists(directory.path));
}

TEST(EmulatorShaderTranslationCache, CorruptPersistentModuleFallsBackAndIsReplaced)
{
	ScopedShaderModuleCacheDirectory directory;
	SpirvBinaryCacheStore            store(directory.path);
	const auto key =
	    ShaderModuleKey::Create(TestShaderId(44, 55, 66), ShaderModuleStage::Compute, Config::ShaderOptimizationType::None, true);
	uint32_t compiles = 0;
	auto compiler = [&]
	{
		++compiles;
		return Vector<uint32_t> {0x07230203u, 0x00010500u, compiles, 4u, 0u};
	};

	{
		ShaderTranslationCache seed(16, &store, false);
		EXPECT_FALSE(seed.GetOrCompile(key, compiler).hit);
	}
	for (const auto& entry: std::filesystem::directory_iterator(directory.path))
	{
		if (entry.is_regular_file() && entry.path().extension() == ".spvmod")
		{
			std::ofstream file(entry.path(), std::ios::binary | std::ios::trunc);
			file.write("broken", 6);
		}
	}
	{
		ShaderTranslationCache repair(16, &store, false);
		EXPECT_FALSE(repair.GetOrCompile(key, compiler).hit);
	}
	{
		ShaderTranslationCache warm(16, &store, false);
		EXPECT_TRUE(warm.GetOrCompile(key, compiler).hit);
	}
	EXPECT_EQ(compiles, 2u);
}

TEST(EmulatorShaderTranslationCache, ConcurrentSameKeyDoesNotDuplicateCompilation)
{
	ShaderTranslationCache cache(16);
	const auto key =
	    ShaderModuleKey::Create(TestShaderId(100, 200, 300), ShaderModuleStage::Pixel, Config::ShaderOptimizationType::Size, true);
	std::atomic<uint32_t> compiles {0};
	std::atomic<bool>     release {false};
	auto compiler = [&]
	{
		compiles.fetch_add(1);
		while (!release.load())
		{
			std::this_thread::yield();
		}
		Vector<uint32_t> binary;
		binary.Add(0x07230203u);
		return binary;
	};

	ShaderTranslationCacheResult first;
	ShaderTranslationCacheResult second;
	std::thread                  a([&] { first = cache.GetOrCompile(key, compiler); });
	while (compiles.load() == 0)
	{
		std::this_thread::yield();
	}
	std::thread b([&] { second = cache.GetOrCompile(key, compiler); });
	release.store(true);
	a.join();
	b.join();

	EXPECT_EQ(compiles.load(), 1u);
	EXPECT_NE(first.hit, second.hit);
	EXPECT_EQ(first.binary, second.binary);
}

TEST(EmulatorShaderTranslationCache, TranslatorVersionInvalidatesEntry)
{
	ShaderTranslationCache cache(16);
	auto key =
	    ShaderModuleKey::Create(TestShaderId(7, 8, 9), ShaderModuleStage::Vertex, Config::ShaderOptimizationType::Performance, true);
	uint32_t compiles = 0;
	auto compiler = [&]
	{
		++compiles;
		Vector<uint32_t> binary;
		binary.Add(compiles);
		return binary;
	};

	EXPECT_FALSE(cache.GetOrCompile(key, compiler).hit);
	key.translator_version++;
	EXPECT_FALSE(cache.GetOrCompile(key, compiler).hit);
	EXPECT_EQ(compiles, 2u);
}

TEST(EmulatorShaderTranslationCache, FullCacheWaitsForCompilingEntryBeforeEviction)
{
	ShaderTranslationCache cache(1);
	const auto first_key =
	    ShaderModuleKey::Create(TestShaderId(1, 1, 1), ShaderModuleStage::Vertex, Config::ShaderOptimizationType::None, true);
	const auto second_key =
	    ShaderModuleKey::Create(TestShaderId(2, 2, 2), ShaderModuleStage::Pixel, Config::ShaderOptimizationType::None, true);
	std::atomic<bool> first_started {false};
	std::atomic<bool> release_first {false};
	std::atomic<bool> second_entered {false};
	std::atomic<bool> second_started {false};
	std::atomic<bool> second_evicted {false};

	std::thread first(
	    [&]
	    {
		    const auto result = cache.GetOrCompile(first_key,
		                                           [&]
		                                           {
			                                           first_started.store(true);
			                                           while (!release_first.load())
			                                           {
				                                           std::this_thread::yield();
			                                           }
			                                           Vector<uint32_t> binary;
			                                           binary.Add(1);
			                                           return binary;
		                                           });
		    (void)result;
	    });
	while (!first_started.load())
	{
		std::this_thread::yield();
	}
	std::thread second(
	    [&]
	    {
		    second_entered.store(true);
		    const auto result = cache.GetOrCompile(second_key,
		                                           [&]
		                                           {
			                                           second_started.store(true);
			                                           Vector<uint32_t> binary;
			                                           binary.Add(2);
			                                           return binary;
		                                           });
		    second_evicted.store(result.evicted);
	    });

	while (!second_entered.load())
	{
		std::this_thread::yield();
	}
	for (int i = 0; i < 1000; ++i)
	{
		std::this_thread::yield();
	}
	EXPECT_FALSE(second_started.load());
	EXPECT_EQ(cache.Size(), 1u);
	release_first.store(true);
	first.join();
	second.join();
	EXPECT_TRUE(second_started.load());
	EXPECT_TRUE(second_evicted.load());
	EXPECT_EQ(cache.Size(), 1u);
}

UT_END();
