#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/SpirvBinaryCacheStore.h"
#include "Emulator/Graphics/ShaderTranslationCache.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

UT_BEGIN(EmulatorShaderTranslationCache);

using namespace Libs::Graphics;

namespace {

void TestSetEnvironment(const char* name, const char* value)
{
#if defined(_WIN32)
	_putenv_s(name, value);
#else
	::setenv(name, value, 1);
#endif
}

void TestUnsetEnvironment(const char* name)
{
#if defined(_WIN32)
	_putenv_s(name, "");
#else
	::unsetenv(name);
#endif
}

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

	const char* previous_tap       = std::getenv("KYTY_FS_TAP");
	const bool  had_previous_tap   = previous_tap != nullptr;
	const std::string previous_value = had_previous_tap ? previous_tap : "";
	const char* previous_signed       = std::getenv("KYTY_FS_TAP_SIGNED");
	const bool  had_previous_signed   = previous_signed != nullptr;
	const std::string previous_signed_value = had_previous_signed ? previous_signed : "";
	const char* previous_draw       = std::getenv("KYTY_FS_TAP_DRAW");
	const bool  had_previous_draw   = previous_draw != nullptr;
	const std::string previous_draw_value = had_previous_draw ? previous_draw : "";
	const char*       previous_lod       = std::getenv("KYTY_FS_TAP_LOD");
	const bool        had_previous_lod   = previous_lod != nullptr;
	const std::string previous_lod_value = had_previous_lod ? previous_lod : "";
	TestUnsetEnvironment("KYTY_FS_TAP");
	TestUnsetEnvironment("KYTY_FS_TAP_SIGNED");
	TestUnsetEnvironment("KYTY_FS_TAP_DRAW");
	TestUnsetEnvironment("KYTY_FS_TAP_LOD");
	const auto no_tap = ShaderResolveFragmentTapConfig(0x0000000100000002ull, true, 15366u);
	const auto untapped =
	    ShaderModuleKey::Create(id, ShaderModuleStage::Pixel, Config::ShaderOptimizationType::Performance, true);
	TestSetEnvironment("KYTY_FS_TAP", "0000000100000002:0");
	const auto unscoped_draw = ShaderResolveFragmentTapConfig(0x0000000100000002ull, true, 15366u);
	TestSetEnvironment("KYTY_FS_TAP_DRAW", "indexed:15366");
	const auto selected_draw = ShaderResolveFragmentTapConfig(0x0000000100000002ull, true, 15366u);
	const auto wrong_count   = ShaderResolveFragmentTapConfig(0x0000000100000002ull, true, 15365u);
	const auto wrong_kind    = ShaderResolveFragmentTapConfig(0x0000000100000002ull, false, 15366u);
	const auto wrong_shader  = ShaderResolveFragmentTapConfig(0x0000000300000004ull, true, 15366u);
	TestSetEnvironment("KYTY_FS_TAP_DRAW", "auto:15366");
	const auto selected_auto = ShaderResolveFragmentTapConfig(0x0000000100000002ull, false, 15366u);
	TestSetEnvironment("KYTY_FS_TAP_DRAW", "indexed:15366");
	const auto tapped =
	    ShaderModuleKey::Create(id, ShaderModuleStage::Pixel, Config::ShaderOptimizationType::Performance, true, false,
	                            selected_draw.diagnostic_identity);
	TestSetEnvironment("KYTY_FS_TAP_SIGNED", "1");
	const auto signed_config = ShaderResolveFragmentTapConfig(0x0000000100000002ull, true, 15366u);
	const auto signed_tapped =
	    ShaderModuleKey::Create(id, ShaderModuleStage::Pixel, Config::ShaderOptimizationType::Performance, true, false,
	                            signed_config.diagnostic_identity);
	TestUnsetEnvironment("KYTY_FS_TAP_SIGNED");
	TestSetEnvironment("KYTY_FS_TAP_LOD", "1");
	const auto lod_config = ShaderResolveFragmentTapConfig(0x0000000100000002ull, true, 15366u);
	const auto lod_tapped =
	    ShaderModuleKey::Create(id, ShaderModuleStage::Pixel, Config::ShaderOptimizationType::Performance, true, false,
	                            lod_config.diagnostic_identity);
	if (had_previous_tap)
	{
		TestSetEnvironment("KYTY_FS_TAP", previous_value.c_str());
	} else
	{
		TestUnsetEnvironment("KYTY_FS_TAP");
	}
	if (had_previous_signed)
	{
		TestSetEnvironment("KYTY_FS_TAP_SIGNED", previous_signed_value.c_str());
	} else
	{
		TestUnsetEnvironment("KYTY_FS_TAP_SIGNED");
	}
	if (had_previous_draw)
	{
		TestSetEnvironment("KYTY_FS_TAP_DRAW", previous_draw_value.c_str());
	} else
	{
		TestUnsetEnvironment("KYTY_FS_TAP_DRAW");
	}
	if (had_previous_lod)
	{
		TestSetEnvironment("KYTY_FS_TAP_LOD", previous_lod_value.c_str());
	} else
	{
		TestUnsetEnvironment("KYTY_FS_TAP_LOD");
	}
	EXPECT_FALSE(no_tap.enabled);
	EXPECT_TRUE(unscoped_draw.enabled);
	EXPECT_FALSE(unscoped_draw.draw_scoped);
	EXPECT_TRUE(selected_draw.enabled);
	EXPECT_TRUE(selected_draw.draw_scoped);
	EXPECT_FALSE(wrong_count.enabled);
	EXPECT_TRUE(wrong_count.draw_scoped);
	EXPECT_FALSE(wrong_kind.enabled);
	EXPECT_TRUE(wrong_kind.draw_scoped);
	EXPECT_FALSE(wrong_shader.enabled);
	EXPECT_FALSE(wrong_shader.draw_scoped);
	EXPECT_TRUE(selected_auto.enabled);
	EXPECT_TRUE(selected_auto.draw_scoped);
	EXPECT_TRUE(signed_config.signed_visualization);
	EXPECT_TRUE(lod_config.query_lod_visualization);
	EXPECT_NE(untapped, tapped);
	EXPECT_NE(tapped, signed_tapped);
	EXPECT_NE(tapped, lod_tapped);
	EXPECT_EQ(tapped.diagnostic_identity, 0x8000000000000000ull);
	EXPECT_EQ(signed_tapped.diagnostic_identity, 0xa000000000000000ull);
	EXPECT_EQ(lod_tapped.diagnostic_identity, 0x9800000000000000ull);
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
	const char*       previous_tap       = std::getenv("KYTY_FS_TAP");
	const bool        had_previous_tap   = previous_tap != nullptr;
	const std::string previous_tap_value = had_previous_tap ? previous_tap : "";
	TestUnsetEnvironment("KYTY_FS_TAP");
	const auto id = TestShaderId(11, 22, 33);
	const auto normal_key =
	    ShaderModuleKey::Create(id, ShaderModuleStage::Pixel, Config::ShaderOptimizationType::Performance, true);
	std::atomic<uint32_t> compiles {0};
	auto compiler = [&]
	{
		const auto serial = compiles.fetch_add(1) + 1u;
		return Vector<uint32_t> {0x07230203u, 0x00010500u, serial, 4u, 0u};
	};

	{
		SpirvBinaryCacheStore store(directory.path);
		ShaderTranslationCache cold_cache(16, &store, false);
		EXPECT_FALSE(cold_cache.GetOrCompile(normal_key, compiler).hit);
		store.Drain();
	}
	TestSetEnvironment("KYTY_FS_TAP", "0000000b00000016:0");
	const auto tapped_config = ShaderResolveFragmentTapConfig(0x0000000b00000016ull, true, 1u);
	const auto tapped_key =
	    ShaderModuleKey::Create(id, ShaderModuleStage::Pixel, Config::ShaderOptimizationType::Performance, true, false,
	                            tapped_config.diagnostic_identity);
	EXPECT_NE(normal_key, tapped_key);
	{
		SpirvBinaryCacheStore store(directory.path);
		ShaderTranslationCache tapped_cache(16, &store, false);
		const auto             tapped = tapped_cache.GetOrCompile(tapped_key, compiler);
		EXPECT_FALSE(tapped.hit);
		EXPECT_EQ(tapped.binary, (Vector<uint32_t> {0x07230203u, 0x00010500u, 2u, 4u, 0u}));
		store.Drain();
	}
	TestUnsetEnvironment("KYTY_FS_TAP");
	{
		SpirvBinaryCacheStore store(directory.path);
		ShaderTranslationCache warm_cache(16, &store, false);
		const auto             warm = warm_cache.GetOrCompile(normal_key, compiler);
		EXPECT_TRUE(warm.hit);
		EXPECT_EQ(warm.binary, (Vector<uint32_t> {0x07230203u, 0x00010500u, 1u, 4u, 0u}));
	}
	if (had_previous_tap)
	{
		TestSetEnvironment("KYTY_FS_TAP", previous_tap_value.c_str());
	} else
	{
		TestUnsetEnvironment("KYTY_FS_TAP");
	}
	EXPECT_EQ(compiles.load(), 2u);
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

TEST(EmulatorShaderTranslationCache, SaturatedPersistenceDoesNotInvalidateTheMemoryEntry)
{
	ScopedShaderModuleCacheDirectory directory;
	SpirvBinaryCacheLimits           limits;
	limits.max_pending_entries = 0;
	limits.max_pending_bytes   = 0;
	SpirvBinaryCacheStore store(directory.path, limits);
	ShaderTranslationCache cache(16, &store, false);
	const auto key =
	    ShaderModuleKey::Create(TestShaderId(71, 72, 73), ShaderModuleStage::Compute, Config::ShaderOptimizationType::None, true);
	std::atomic<uint32_t> compiles {0};
	auto compiler = [&]
	{
		compiles.fetch_add(1);
		return Vector<uint32_t> {0x07230203u, 0x00010500u, 0u, 4u, 0u};
	};

	const auto miss = cache.GetOrCompile(key, compiler);
	const auto hit  = cache.GetOrCompile(key, compiler);

	EXPECT_FALSE(miss.hit);
	EXPECT_TRUE(hit.hit);
	EXPECT_EQ(hit.binary, miss.binary);
	EXPECT_EQ(compiles.load(), 1u);
	EXPECT_EQ(store.AsyncStats().dropped, 1u);
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
	store.Drain();
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
	store.Drain();
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

TEST(EmulatorShaderTranslationCache, Gen5StreamSgprSemanticsInvalidateVersion29)
{
	const auto current = ShaderModuleKey::Create(TestShaderId(17, 18, 19), ShaderModuleStage::Vertex,
	                                             Config::ShaderOptimizationType::Performance, true);
	auto legacy = current;
	legacy.translator_version = 29;

	EXPECT_GT(current.translator_version, legacy.translator_version);
	EXPECT_NE(current, legacy);
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
