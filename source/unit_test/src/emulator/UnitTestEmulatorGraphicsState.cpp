#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Objects/DepthMeta.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/GpuMemoryTransientBuffer.h"
#include "Emulator/Graphics/Objects/GpuWritebackPageCache.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/RenderTexture.h"
#include "Emulator/Graphics/Objects/StorageBuffer.h"
#include "Emulator/Graphics/Objects/Texture.h"
#include "Emulator/Graphics/Objects/VertexBuffer.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/PipelineCacheStore.h"
#include "Emulator/Graphics/SpirvBinaryCacheStore.h"
#include "Emulator/Graphics/ShaderTranslationCache.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

UT_BEGIN(EmulatorGraphicsState);

using namespace Libs::Graphics;

namespace {

class ScopedSpirvCacheDirectory final
{
public:
	ScopedSpirvCacheDirectory()
	{
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		path             = std::filesystem::temp_directory_path() / ("kyty-spirv-cache-test-" + std::to_string(nonce));
	}
	~ScopedSpirvCacheDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path path;
};

struct PausedSpirvCacheWrite
{
	std::mutex              mutex;
	std::condition_variable condition;
	bool                    entered = false;
	bool                    release = false;
};

void PauseSpirvCacheWrite(void* opaque)
{
	auto* pause = static_cast<PausedSpirvCacheWrite*>(opaque);
	std::unique_lock<std::mutex> lock(pause->mutex);
	pause->entered = true;
	pause->condition.notify_all();
	pause->condition.wait(lock, [pause] { return pause->release; });
}

int                   g_test_gpu_object_deletes = 0;
int                   g_versioned_gpu_object_creates = 0;
int                   g_versioned_gpu_object_updates = 0;
int                   g_versioned_gpu_object_deletes = 0;
int                   g_covered_index_creates        = 0;
int                   g_covered_index_updates        = 0;
int                   g_covered_index_deletes        = 0;
bool                  g_covered_index_invalid_update = false;
std::atomic_uint64_t   g_versioned_submission_sequence {0};
std::atomic_uint64_t   g_covered_index_submission_sequence {0};
std::mutex             g_versioned_gpu_object_mutex;
std::condition_variable g_versioned_gpu_object_cond;
bool                    g_versioned_gpu_object_block_second   = false;
bool                    g_versioned_gpu_object_second_entered = false;
bool                    g_versioned_gpu_object_release_second = false;

struct TestGpuObject: public GpuObject
{
	explicit TestGpuObject(GpuMemoryObjectType object_type = GpuMemoryObjectType::StorageBuffer, bool is_read_only = false)
	{
		type       = object_type;
		params[0] = 0x53544f5241474555ull ^ static_cast<uint64_t>(object_type);
		read_only  = is_read_only;
	}

	bool Equal(const uint64_t* other) const override { return other != nullptr && other[0] == params[0]; }

	create_func_t GetCreateFunc() const override
	{
		return [](GraphicContext* /*ctx*/, const uint64_t* /*params*/, const uint64_t* /*vaddr*/, const uint64_t* /*size*/,
		          int /*vaddr_num*/, VulkanMemory* /*mem*/) -> void* { return reinterpret_cast<void*>(0x51515151ull); };
	}

	create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; }
	write_back_func_t          GetWriteBackFunc() const override { return nullptr; }

	delete_func_t GetDeleteFunc() const override
	{
		return [](GraphicContext* /*ctx*/, void* obj, VulkanMemory* /*mem*/)
		{
			if (obj == reinterpret_cast<void*>(0x51515151ull))
			{
				g_test_gpu_object_deletes++;
			}
		};
	}

	update_func_t GetUpdateFunc() const override { return nullptr; }
};

struct VersionedTestBacking
{
	int serial = 0;
};

struct VersionedTestGpuObject: public GpuObject
{
	explicit VersionedTestGpuObject(bool write_back = false)
	{
		type       = GpuMemoryObjectType::VertexBuffer;
		params[0] = 0x56455253494f4e31ull;
		check_hash = true;
		read_only  = false;
		m_write_back = write_back;
	}

	bool Equal(const uint64_t* other) const override { return other != nullptr && other[0] == params[0]; }

	create_func_t GetCreateFunc() const override
	{
		return [](GraphicContext* /*ctx*/, const uint64_t* /*params*/, const uint64_t* /*vaddr*/, const uint64_t* /*size*/,
		          int /*vaddr_num*/, VulkanMemory* /*mem*/) -> void*
		{
			auto* backing = new VersionedTestBacking;
			backing->serial = ++g_versioned_gpu_object_creates;
			if (backing->serial == 2 && g_versioned_gpu_object_block_second)
			{
				std::unique_lock lock(g_versioned_gpu_object_mutex);
				g_versioned_gpu_object_second_entered = true;
				g_versioned_gpu_object_cond.notify_all();
				g_versioned_gpu_object_cond.wait(lock, [] { return g_versioned_gpu_object_release_second; });
			}
			return backing;
		};
	}

	create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; }

	write_back_func_t GetWriteBackFunc() const override
	{
		if (!m_write_back)
		{
			return nullptr;
		}
		return [](GraphicContext* /*ctx*/, const uint64_t* /*params*/, void* /*obj*/, const uint64_t* /*vaddr*/,
		          const uint64_t* size, int /*vaddr_num*/)
		{
			return GpuWritebackResult {.changed_pages = 1u, .copied_bytes = *size, .content_changed = true};
		};
	}

	delete_func_t GetDeleteFunc() const override
	{
		return [](GraphicContext* /*ctx*/, void* obj, VulkanMemory* /*mem*/)
		{
			delete static_cast<VersionedTestBacking*>(obj);
			g_versioned_gpu_object_deletes++;
		};
	}

	update_func_t GetUpdateFunc() const override
	{
		return [](GraphicContext* /*ctx*/, const uint64_t* /*params*/, void* /*obj*/, const uint64_t* /*vaddr*/,
		          const uint64_t* /*size*/, int /*vaddr_num*/) { g_versioned_gpu_object_updates++; };
	}

private:
	bool m_write_back = false;
};

struct CoveredIndexTestBacking
{
	int       serial = 0;
	uint64_t  size   = 0;
	uint8_t*  bytes  = nullptr;
};

struct CoveredIndexTestGpuObject: public GpuObject
{
	CoveredIndexTestGpuObject()
	{
		type       = GpuMemoryObjectType::IndexBuffer;
		params[0] = 0x434f564552454449ull;
		check_hash = true;
		read_only  = false;
	}

	bool Equal(const uint64_t* other) const override { return other != nullptr && other[0] == params[0]; }

	create_func_t GetCreateFunc() const override
	{
		return [](GraphicContext* /*ctx*/, const uint64_t* /*params*/, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
		          VulkanMemory* /*mem*/) -> void*
		{
			if (vaddr == nullptr || size == nullptr || vaddr_num != 1 || vaddr[0] == 0 || size[0] == 0)
			{
				return nullptr;
			}
			auto* backing   = new CoveredIndexTestBacking;
			backing->serial = ++g_covered_index_creates;
			backing->size   = size[0];
			backing->bytes  = new uint8_t[backing->size];
			std::memcpy(backing->bytes, reinterpret_cast<const void*>(vaddr[0]), backing->size);
			return backing;
		};
	}

	create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; }
	write_back_func_t          GetWriteBackFunc() const override { return nullptr; }

	delete_func_t GetDeleteFunc() const override
	{
		return [](GraphicContext* /*ctx*/, void* obj, VulkanMemory* /*mem*/)
		{
			auto* backing = static_cast<CoveredIndexTestBacking*>(obj);
			if (backing != nullptr)
			{
				delete[] backing->bytes;
				delete backing;
				g_covered_index_deletes++;
			}
		};
	}

	update_func_t GetUpdateFunc() const override
	{
		return [](GraphicContext* /*ctx*/, const uint64_t* /*params*/, void* obj, const uint64_t* vaddr, const uint64_t* size,
		          int vaddr_num)
		{
			auto* backing = static_cast<CoveredIndexTestBacking*>(obj);
			if (backing == nullptr || vaddr == nullptr || size == nullptr || vaddr_num != 1 || vaddr[0] == 0 || size[0] == 0 ||
			    size[0] > backing->size)
			{
				g_covered_index_invalid_update = true;
				return;
			}
			std::memcpy(backing->bytes, reinterpret_cast<const void*>(vaddr[0]), size[0]);
			g_covered_index_updates++;
		};
	}
};

void EnsureGpuMemoryForTests()
{
	static bool initialized = false;
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	if (!initialized)
	{
		GpuMemoryInit();
		initialized = true;
	}
}

} // namespace

TEST(EmulatorGraphicsState, RegistersAnonymousGpuResourceBeforeOwnerRegistration)
{
	EnsureGpuMemoryForTests();

	uint32_t resource_handle = UINT32_MAX;
	GpuMemoryRegisterResource(&resource_handle, 0, reinterpret_cast<void*>(static_cast<uintptr_t>(0x100000)), 100,
	                          "bootstrap-resource", 1, 0);
	GpuMemoryUnregisterResource(resource_handle);
}

TEST(EmulatorGraphicsState, ResolvesGen5RectListAutoDrawExpansion)
{
	uint32_t vertex_count = 0;
	EXPECT_TRUE(GraphicsResolveRectListAutoDraw(7, 3, 0, &vertex_count));
	EXPECT_EQ(vertex_count, 4u);

	EXPECT_FALSE(GraphicsResolveRectListAutoDraw(7, 4, 0, &vertex_count));
	EXPECT_FALSE(GraphicsResolveRectListAutoDraw(7, 3, 1, &vertex_count));
	EXPECT_FALSE(GraphicsResolveRectListAutoDraw(17, 3, 0, &vertex_count));
	EXPECT_FALSE(GraphicsResolveRectListAutoDraw(7, 3, 0, nullptr));
}

TEST(EmulatorGraphicsState, TiledVideoOutBufferUpdateDoesNotCpuUpload)
{
	EXPECT_FALSE(VideoOutBufferShouldCpuUploadOnUpdate(true));
	EXPECT_TRUE(VideoOutBufferShouldCpuUploadOnUpdate(false));
}

TEST(EmulatorGraphicsState, VideoOutBufferMaterializationStateTracksTheHostImage)
{
	VideoOutVulkanImage image;
	EXPECT_TRUE(VideoOutBufferNeedsMaterialization(&image));
	image.image = reinterpret_cast<VkImage>(0x1);
	EXPECT_FALSE(VideoOutBufferNeedsMaterialization(&image));
	EXPECT_FALSE(VideoOutBufferNeedsMaterialization(nullptr));
}

TEST(EmulatorGraphicsState, GpuOwnedTiledRenderTextureSkipsCpuHash)
{
	const RenderTextureObject gpu_owned(RenderTextureFormat::R8G8B8A8Unorm, 1280, 720, true, false, 1280, false);
	const RenderTextureObject write_back(RenderTextureFormat::R8G8B8A8Unorm, 1280, 720, true, false, 1280, true);
	const RenderTextureObject linear(RenderTextureFormat::R8G8B8A8Unorm, 1280, 720, false, false, 1280, false);

	EXPECT_FALSE(gpu_owned.check_hash);
	EXPECT_TRUE(write_back.check_hash);
	EXPECT_TRUE(linear.check_hash);
}

TEST(EmulatorGraphicsState, TiledVideoOutBufferSkipsCpuHash)
{
	const VideoOutBufferObject tiled(VideoOutBufferFormat::R8G8B8A8Srgb, 1280, 720, true, false, 1280);
	const VideoOutBufferObject linear(VideoOutBufferFormat::R8G8B8A8Srgb, 1280, 720, false, false, 1280);

	EXPECT_FALSE(tiled.check_hash);
	EXPECT_TRUE(linear.check_hash);
}

TEST(EmulatorGraphicsState, DisabledShaderFilterIgnoresNullAddress)
{
	EXPECT_FALSE(ShaderIsDisabled(0));
}

TEST(EmulatorGraphicsState, UserConfigTracksGuestVertexOffset)
{
	HW::UserConfig config;
	EXPECT_EQ(config.GetIndexOffset(), 0u);

	config.SetIndexOffset(37u);
	EXPECT_EQ(config.GetIndexOffset(), 37u);

	config.Reset();
	EXPECT_EQ(config.GetIndexOffset(), 0u);
}

TEST(EmulatorGraphicsState, PipelineCacheAcceptsMatchingVulkanHeader)
{
	VkPhysicalDeviceProperties properties {};
	properties.vendorID = 0x1234;
	properties.deviceID = 0x5678;
	for (uint32_t i = 0; i < VK_UUID_SIZE; i++)
	{
		properties.pipelineCacheUUID[i] = static_cast<uint8_t>(i + 1);
	}

	PipelineCacheHeaderV1 header {};
	header.header_size    = sizeof(header);
	header.header_version = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
	header.vendor_id      = properties.vendorID;
	header.device_id      = properties.deviceID;
	memcpy(header.uuid, properties.pipelineCacheUUID, VK_UUID_SIZE);

	EXPECT_TRUE(PipelineCacheDataMatchesDevice(&header, sizeof(header), properties));
}

TEST(EmulatorGraphicsState, PipelineCacheRejectsMalformedOrForeignData)
{
	VkPhysicalDeviceProperties properties {};
	properties.vendorID = 0x1234;
	properties.deviceID = 0x5678;

	PipelineCacheHeaderV1 header {};
	header.header_size    = sizeof(header);
	header.header_version = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
	header.vendor_id      = properties.vendorID;
	header.device_id      = properties.deviceID;
	memcpy(header.uuid, properties.pipelineCacheUUID, VK_UUID_SIZE);

	EXPECT_FALSE(PipelineCacheDataMatchesDevice(&header, sizeof(header) - 1, properties));

	header.header_size = sizeof(header) + 1;
	EXPECT_FALSE(PipelineCacheDataMatchesDevice(&header, sizeof(header), properties));
	header.header_size = sizeof(header);

	header.header_version = VK_PIPELINE_CACHE_HEADER_VERSION_ONE + 1;
	EXPECT_FALSE(PipelineCacheDataMatchesDevice(&header, sizeof(header), properties));
	header.header_version = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;

	header.device_id++;
	EXPECT_FALSE(PipelineCacheDataMatchesDevice(&header, sizeof(header), properties));

	EXPECT_FALSE(PipelineCacheDataMatchesDevice(&header, PipelineCacheStoreMaxBytes() + 1, properties));
}

TEST(EmulatorGraphicsState, PipelineCacheCheckpointsAreDirtyAndRateLimited)
{
	EXPECT_FALSE(PipelineCacheStoreCheckpointDue(false, false, 0));
	EXPECT_TRUE(PipelineCacheStoreCheckpointDue(true, false, 0));
	EXPECT_FALSE(PipelineCacheStoreCheckpointDue(true, true, PipelineCacheStoreCheckpointSeconds() - 1));
	EXPECT_TRUE(PipelineCacheStoreCheckpointDue(true, true, PipelineCacheStoreCheckpointSeconds()));

	const size_t budget = PipelineCacheStoreSessionWriteBudgetBytes();
	EXPECT_TRUE(PipelineCacheStoreWriteBudgetAllows(0, budget));
	EXPECT_TRUE(PipelineCacheStoreWriteBudgetAllows(budget - 1, 1));
	EXPECT_FALSE(PipelineCacheStoreWriteBudgetAllows(1, budget));
	EXPECT_FALSE(PipelineCacheStoreWriteBudgetAllows(budget, 1));

	EXPECT_EQ(PipelineCacheStoreAccountWriteAttempt(0, budget / 2), budget / 2);
	EXPECT_EQ(PipelineCacheStoreAccountWriteAttempt(budget / 2, budget / 2), budget);
	EXPECT_EQ(PipelineCacheStoreAccountWriteAttempt(budget - 1, 2), budget);
	EXPECT_EQ(PipelineCacheStoreAccountWriteAttempt(budget, 1), budget);
}

TEST(EmulatorGraphicsState, SpirvBinaryCacheRequiresExactSourceAndCompileOptions)
{
	ScopedSpirvCacheDirectory directory;
	SpirvBinaryCacheStore     cache(directory.path);
	const String8             source_a("OpCapability Shader\nOpMemoryModel Logical GLSL450\n");
	const String8             source_b("OpCapability Shader\nOpMemoryModel Logical Vulkan\n");
	Vector<uint32_t>          expected {0x07230203u, 0x00010500u, 0u, 4u, 0u};
	Vector<uint32_t>          loaded;

	EXPECT_EQ(cache.Store(source_a, 0, false, expected), SpirvBinaryCacheStoreResult::Written);
	EXPECT_EQ(cache.Load(source_a, 0, false, &loaded), SpirvBinaryCacheLoadResult::Hit);
	EXPECT_EQ(loaded, expected);
	EXPECT_EQ(cache.Load(source_b, 0, false, &loaded), SpirvBinaryCacheLoadResult::Miss);
	EXPECT_EQ(cache.Load(source_a, 1, false, &loaded), SpirvBinaryCacheLoadResult::Miss);
	EXPECT_EQ(cache.Load(source_a, 0, true, &loaded), SpirvBinaryCacheLoadResult::Miss);
}

TEST(EmulatorGraphicsState, SpirvModuleCacheRequiresExactTranslationIdentity)
{
	ScopedSpirvCacheDirectory directory;
	SpirvBinaryCacheStore     cache(directory.path);
	ShaderId                  shader_id;
	shader_id.hash0 = 0x11223344u;
	shader_id.crc32 = 0x55667788u;
	shader_id.ids.Add(0x99aabbccu);
	const auto base =
	    ShaderModuleKey::Create(shader_id, ShaderModuleStage::Vertex, Config::ShaderOptimizationType::Performance, true);
	Vector<uint32_t> expected {0x07230203u, 0x00010500u, 0u, 4u, 0u};
	Vector<uint32_t> loaded;

	EXPECT_EQ(cache.StoreModule(base, false, expected), SpirvBinaryCacheStoreResult::Written);
	EXPECT_EQ(cache.LoadModule(base, false, &loaded), SpirvBinaryCacheLoadResult::Hit);
	EXPECT_EQ(loaded, expected);
	EXPECT_EQ(cache.LoadModule(ShaderModuleKey::Create(shader_id, ShaderModuleStage::Pixel,
	                                                  Config::ShaderOptimizationType::Performance, true),
	                           false, &loaded),
	          SpirvBinaryCacheLoadResult::Miss);
	EXPECT_EQ(cache.LoadModule(base, true, &loaded), SpirvBinaryCacheLoadResult::Miss);

	auto newer = base;
	newer.translator_version++;
	EXPECT_EQ(cache.LoadModule(newer, false, &loaded), SpirvBinaryCacheLoadResult::Miss);
}

TEST(EmulatorGraphicsState, SpirvBinaryCacheRejectsCorruptEntry)
{
	ScopedSpirvCacheDirectory directory;
	SpirvBinaryCacheStore     cache(directory.path);
	const String8             source("OpCapability Shader\n");
	Vector<uint32_t>          expected {0x07230203u, 0x00010500u, 0u, 4u, 0u};
	Vector<uint32_t>          loaded;

	ASSERT_EQ(cache.Store(source, 0, false, expected), SpirvBinaryCacheStoreResult::Written);
	for (const auto& entry: std::filesystem::directory_iterator(directory.path))
	{
		if (entry.is_regular_file() && entry.path().extension() == ".spvbin")
		{
			std::ofstream file(entry.path(), std::ios::binary | std::ios::trunc);
			file.write("broken", 6);
		}
	}

	EXPECT_EQ(cache.Load(source, 0, false, &loaded), SpirvBinaryCacheLoadResult::Corrupt);
	EXPECT_TRUE(loaded.IsEmpty());
}

TEST(EmulatorGraphicsState, SpirvBinaryCacheEnforcesDiskAndSessionBudgets)
{
	ScopedSpirvCacheDirectory directory;
	SpirvBinaryCacheLimits    limits;
	limits.max_total_bytes      = 512;
	limits.max_entry_bytes      = 256;
	limits.session_write_budget = 512;
	SpirvBinaryCacheStore cache(directory.path, limits);
	Vector<uint32_t>      binary {0x07230203u, 0x00010500u, 0u, 4u, 0u};

	for (uint32_t i = 0; i < 8; ++i)
	{
		const auto source = String8::FromPrintf("OpCapability Shader\n; entry %u\n", i);
		const auto result = cache.Store(source, 0, false, binary);
		EXPECT_TRUE(result == SpirvBinaryCacheStoreResult::Written || result == SpirvBinaryCacheStoreResult::BudgetExceeded);
		EXPECT_LE(cache.DiskUsageBytes(), limits.max_total_bytes);
	}
	EXPECT_LE(cache.SessionBytesAttempted(), limits.session_write_budget);
}

TEST(EmulatorGraphicsState, SpirvBinaryCacheReplacementDoesNotEvictUnrelatedEntry)
{
	ScopedSpirvCacheDirectory directory;
	SpirvBinaryCacheLimits    limits;
	limits.max_total_bytes      = 300;
	limits.max_entry_bytes      = 256;
	limits.session_write_budget = 1024;
	SpirvBinaryCacheStore cache(directory.path, limits);
	const String8         source_a("OpCapability Shader\n; entry a\n");
	const String8         source_b("OpCapability Shader\n; entry b\n");
	Vector<uint32_t>      binary_a {0x07230203u, 0x00010500u, 1u, 4u, 0u};
	Vector<uint32_t>      binary_b {0x07230203u, 0x00010500u, 2u, 4u, 0u};
	Vector<uint32_t>      replacement {0x07230203u, 0x00010500u, 3u, 4u, 0u};
	Vector<uint32_t>      loaded;

	ASSERT_EQ(cache.Store(source_a, 0, false, binary_a), SpirvBinaryCacheStoreResult::Written);
	ASSERT_EQ(cache.Store(source_b, 0, false, binary_b), SpirvBinaryCacheStoreResult::Written);
	ASSERT_EQ(cache.Store(source_a, 0, false, replacement), SpirvBinaryCacheStoreResult::Written);

	EXPECT_EQ(cache.Load(source_a, 0, false, &loaded), SpirvBinaryCacheLoadResult::Hit);
	EXPECT_EQ(loaded, replacement);
	EXPECT_EQ(cache.Load(source_b, 0, false, &loaded), SpirvBinaryCacheLoadResult::Hit);
	EXPECT_EQ(loaded, binary_b);
	EXPECT_LE(cache.DiskUsageBytes(), limits.max_total_bytes);
}

TEST(EmulatorGraphicsState, SpirvBinaryCacheWriteBehindCoalescesAndDrainsOnDestruction)
{
	ScopedSpirvCacheDirectory directory;
	const String8             source("OpCapability Shader\n; queued entry\n");
	Vector<uint32_t>          expected {0x07230203u, 0x00010500u, 0u, 4u, 0u};

	{
		SpirvBinaryCacheStore cache(directory.path);
		EXPECT_EQ(cache.QueueStore(source, 0, false, expected), SpirvBinaryCacheQueueResult::Queued);
		EXPECT_EQ(cache.QueueStore(source, 0, false, expected), SpirvBinaryCacheQueueResult::Coalesced);
	}

	SpirvBinaryCacheStore cache(directory.path);
	Vector<uint32_t>      loaded;
	EXPECT_EQ(cache.Load(source, 0, false, &loaded), SpirvBinaryCacheLoadResult::Hit);
	EXPECT_EQ(loaded, expected);
}

TEST(EmulatorGraphicsState, SpirvBinaryCacheWriteBehindDropsSaturatedQueuesWithinBounds)
{
	ScopedSpirvCacheDirectory directory;
	const String8             source("OpCapability Shader\n; dropped entry\n");
	Vector<uint32_t>          binary {0x07230203u, 0x00010500u, 0u, 4u, 0u};

	{
		SpirvBinaryCacheLimits limits;
		limits.max_pending_entries = 0;
		SpirvBinaryCacheStore cache(directory.path, limits);
		EXPECT_EQ(cache.QueueStore(source, 0, false, binary), SpirvBinaryCacheQueueResult::QueueFull);
		const auto stats = cache.AsyncStats();
		EXPECT_EQ(stats.dropped, 1u);
		EXPECT_EQ(stats.pending_entries, 0u);
		EXPECT_EQ(stats.pending_bytes, 0u);
	}
	{
		SpirvBinaryCacheLimits limits;
		limits.max_pending_entries = 4;
		limits.max_pending_bytes   = 1;
		SpirvBinaryCacheStore cache(directory.path, limits);
		EXPECT_EQ(cache.QueueStore(source, 0, false, binary), SpirvBinaryCacheQueueResult::QueueFull);
		EXPECT_EQ(cache.AsyncStats().dropped, 1u);
	}
}

TEST(EmulatorGraphicsState, SpirvBinaryCacheLoadDoesNotWaitForAnUnrelatedWrite)
{
	ScopedSpirvCacheDirectory directory;
	SpirvBinaryCacheStore     cache(directory.path);
	const String8             readable("OpCapability Shader\n; readable entry\n");
	const String8             writing("OpCapability Shader\n; paused write\n");
	Vector<uint32_t>          binary {0x07230203u, 0x00010500u, 0u, 4u, 0u};
	ASSERT_EQ(cache.Store(readable, 0, false, binary), SpirvBinaryCacheStoreResult::Written);

	PausedSpirvCacheWrite pause;
	cache.SetWriteHookForTesting(PauseSpirvCacheWrite, &pause);
	ASSERT_EQ(cache.QueueStore(writing, 0, false, binary), SpirvBinaryCacheQueueResult::Queued);
	{
		std::unique_lock<std::mutex> lock(pause.mutex);
		pause.condition.wait(lock, [&pause] { return pause.entered; });
	}

	std::promise<void> reader_started;
	auto               reader_started_future = reader_started.get_future();
	auto reader = std::async(std::launch::async,
	                         [&]
	                         {
		                         reader_started.set_value();
		                         Vector<uint32_t> loaded;
		                         return cache.Load(readable, 0, false, &loaded);
	                         });
	reader_started_future.wait();
	const auto reader_status = reader.wait_for(std::chrono::seconds(2));
	{
		std::lock_guard<std::mutex> lock(pause.mutex);
		pause.release = true;
	}
	pause.condition.notify_all();

	EXPECT_EQ(reader_status, std::future_status::ready);
	EXPECT_EQ(reader.get(), SpirvBinaryCacheLoadResult::Hit);
	cache.Drain();
	cache.SetWriteHookForTesting(nullptr, nullptr);
}

TEST(EmulatorGraphicsState, SpirvBinaryCacheCompletedIdentityHistoryIsBounded)
{
	ScopedSpirvCacheDirectory directory;
	SpirvBinaryCacheLimits    limits;
	limits.max_completed_entries = 1;
	SpirvBinaryCacheStore cache(directory.path, limits);
	const String8         source_a("OpCapability Shader\n; completed a\n");
	const String8         source_b("OpCapability Shader\n; completed b\n");
	Vector<uint32_t>      binary {0x07230203u, 0x00010500u, 0u, 4u, 0u};

	ASSERT_EQ(cache.QueueStore(source_a, 0, false, binary), SpirvBinaryCacheQueueResult::Queued);
	cache.Drain();
	EXPECT_EQ(cache.QueueStore(source_a, 0, false, binary), SpirvBinaryCacheQueueResult::Coalesced);
	ASSERT_EQ(cache.QueueStore(source_b, 0, false, binary), SpirvBinaryCacheQueueResult::Queued);
	cache.Drain();
	EXPECT_EQ(cache.AsyncStats().completed_entries, 1u);
	EXPECT_EQ(cache.QueueStore(source_a, 0, false, binary), SpirvBinaryCacheQueueResult::Queued);
}

TEST(EmulatorGraphicsState, GpuMemoryFreeDeletesExactRange)
{
	EnsureGpuMemoryForTests();

	GraphicContext ctx {};
	const uint64_t heap_base = 0x0000005100000000ull;
	const uint64_t heap_size = 0x20000ull;
	const uint64_t obj_addr  = heap_base + 0x4000ull;
	const uint64_t obj_size  = 0x1000ull;

	GpuMemorySetAllocatedRange(heap_base, heap_size);
	g_test_gpu_object_deletes = 0;

	ASSERT_NE(GpuMemoryCreateObject(1, &ctx, nullptr, obj_addr, obj_size, TestGpuObject()), nullptr);
	EXPECT_EQ(GpuMemoryFindObjects(obj_addr, obj_size, GpuMemoryObjectType::StorageBuffer, true, false).Size(), 1u);

	GpuMemoryFree(&ctx, obj_addr, obj_size);

	EXPECT_EQ(g_test_gpu_object_deletes, 1);
	EXPECT_TRUE(GpuMemoryFindObjects(obj_addr, obj_size, GpuMemoryObjectType::StorageBuffer, true, false).IsEmpty());
}

TEST(EmulatorGraphicsState, GpuMemoryValidatesTheCompleteAllocatedGuestRangeReadOnly)
{
	EnsureGpuMemoryForTests();

	const uint64_t heap_base = 0x0000005101000000ull;
	const uint64_t heap_size = 0x20000ull;
	GpuMemorySetAllocatedRange(heap_base, heap_size);

	EXPECT_EQ(GpuMemoryValidateAllocatedRange(heap_base, heap_size), GpuMemoryRangeValidationStatus::Valid);
	EXPECT_EQ(GpuMemoryValidateAllocatedRange(heap_base + 0x1000ull, 8), GpuMemoryRangeValidationStatus::Valid);
	EXPECT_EQ(GpuMemoryValidateAllocatedRange(heap_base - 1, 8), GpuMemoryRangeValidationStatus::Unallocated);
	EXPECT_EQ(GpuMemoryValidateAllocatedRange(heap_base + heap_size - 4, 8), GpuMemoryRangeValidationStatus::Unallocated);
	EXPECT_EQ(GpuMemoryValidateAllocatedRange(heap_base, 0), GpuMemoryRangeValidationStatus::InvalidArgument);
	EXPECT_EQ(GpuMemoryValidateAllocatedRange(UINT64_MAX - 3, 8), GpuMemoryRangeValidationStatus::InvalidArgument);
}

TEST(EmulatorGraphicsState, GpuMemoryFindForSubmissionDefersDeletionUntilCompletion)
{
	EnsureGpuMemoryForTests();

	GraphicContext ctx {};
	const uint64_t heap_base = 0x0000005110000000ull;
	const uint64_t heap_size = 0x20000ull;
	const uint64_t obj_addr  = heap_base + 0x4000ull;
	const uint64_t obj_size  = 0x1000ull;
	const SubmissionId submission {GpuQueueId(97), 1};

	GpuMemorySetAllocatedRange(heap_base, heap_size);
	g_test_gpu_object_deletes = 0;

	ASSERT_NE(GpuMemoryCreateObject(1, &ctx, nullptr, obj_addr, obj_size, TestGpuObject()), nullptr);
	const auto found = GpuMemoryFindObjectsForSubmission(submission, obj_addr, obj_size, GpuMemoryObjectType::StorageBuffer, true, false);
	ASSERT_EQ(found.Size(), 1u);
	EXPECT_EQ(found[0].obj, reinterpret_cast<void*>(0x51515151ull));

	GpuMemoryFree(&ctx, obj_addr, obj_size);

	EXPECT_EQ(g_test_gpu_object_deletes, 0);
	EXPECT_TRUE(GpuMemoryFindObjects(obj_addr, obj_size, GpuMemoryObjectType::StorageBuffer, true, false).IsEmpty());

	GpuMemoryCompleteSubmission(submission);
	EXPECT_EQ(g_test_gpu_object_deletes, 1);
}

TEST(EmulatorGraphicsState, GpuMemoryVersionsChangedBackingWithPendingUse)
{
	EnsureGpuMemoryForTests();

	GraphicContext ctx {};
	auto*          guest = new uint8_t[0x1000] {};
	const uint64_t addr  = reinterpret_cast<uint64_t>(guest);
	const uint64_t size  = 0x1000;
	const SubmissionId submission {GpuQueueId(190), 1};

	GpuMemorySetAllocatedRange(addr, size);
	g_versioned_gpu_object_creates = 0;
	g_versioned_gpu_object_updates = 0;
	g_versioned_gpu_object_deletes = 0;

	void* const old_backing = GpuMemoryCreateObject(1, &ctx, nullptr, addr, size, VersionedTestGpuObject());
	ASSERT_NE(old_backing, nullptr);
	ASSERT_EQ(GpuMemoryFindObjectsForSubmission(submission, addr, size, GpuMemoryObjectType::VertexBuffer, true, false).Size(), 1u);

	guest[0] = 0x7f;
	void* const new_backing = GpuMemoryCreateObject(2, &ctx, nullptr, addr, size, VersionedTestGpuObject());

	EXPECT_NE(new_backing, old_backing);
	EXPECT_EQ(g_versioned_gpu_object_creates, 2);
	EXPECT_EQ(g_versioned_gpu_object_updates, 0);
	EXPECT_EQ(g_versioned_gpu_object_deletes, 0);

	// The replacement may be recorded by the same still-recording submission;
	// that must not retire the old backing before the submission completes.
	const auto current =
	    GpuMemoryFindObjectsForSubmission(submission, addr, size, GpuMemoryObjectType::VertexBuffer, true, false);
	ASSERT_EQ(current.Size(), 1u);
	EXPECT_EQ(current[0].obj, new_backing);
	EXPECT_EQ(g_versioned_gpu_object_deletes, 0);

	GpuMemoryCompleteSubmission(submission);
	EXPECT_EQ(g_versioned_gpu_object_deletes, 1);

	GpuMemoryFree(&ctx, addr, size);
	EXPECT_EQ(g_versioned_gpu_object_deletes, 2);
}

TEST(EmulatorGraphicsState, VideoOutResolverPublishesCowReplacementOnlyAfterRestoring720pExtent)
{
	EnsureGpuMemoryForTests();

	GraphicContext ctx {};
	constexpr uint32_t guest_width  = 1920;
	constexpr uint32_t guest_height = 1080;
	constexpr uint32_t host_width   = 1280;
	constexpr uint32_t host_height  = 720;
	const uint64_t     size         = static_cast<uint64_t>(guest_width) * guest_height * 4;
	auto*              guest        = new uint8_t[size] {};
	const uint64_t     addr         = reinterpret_cast<uint64_t>(guest);
	const SubmissionId old_use {GpuQueueId(194), 1};
	const SubmissionId new_use {GpuQueueId(195), 1};

	GpuMemorySetAllocatedRange(addr, size);
	const VideoOutBufferObject info(VideoOutBufferFormat::R8G8B8A8Srgb, guest_width, guest_height, false, false, guest_width);
	auto* const old_image = static_cast<VideoOutVulkanImage*>(GpuMemoryCreateObject(1, &ctx, nullptr, addr, size, info));
	ASSERT_NE(old_image, nullptr);

	VideoOutHostExtentState extent_state;
	ASSERT_EQ(VideoOutBufferSelectHostExtent(old_image, host_width, host_height, &extent_state),
	          VideoOutHostExtentStatus::Selected);
	ASSERT_EQ(GpuMemoryFindObjectsForSubmission(old_use, addr, size, GpuMemoryObjectType::VideoOutBuffer, true, false).Size(), 1u);

	guest[0] = 0x7f;
	auto* const current_image = static_cast<VideoOutVulkanImage*>(GpuMemoryCreateObject(2, &ctx, nullptr, addr, size, info));
	ASSERT_NE(current_image, nullptr);
	ASSERT_NE(current_image, old_image);

	VideoOutVulkanImage* published_cache = old_image;
	const auto current =
	    GpuMemoryFindObjectsForSubmission(new_use, addr, size, GpuMemoryObjectType::VideoOutBuffer, true, false);
	ASSERT_EQ(current.Size(), 1u);
	ASSERT_EQ(current[0].obj, current_image);
	EXPECT_EQ(VideoOutBufferRefreshPublishedImage(current_image, host_width, host_height, &published_cache),
	          VideoOutPublishedImageRefreshStatus::Published);
	EXPECT_EQ(published_cache, current_image);
	ASSERT_TRUE(VideoOutBufferGetHostExtentState(current_image, &extent_state));
	EXPECT_TRUE(extent_state.selected);
	EXPECT_EQ(extent_state.width, host_width);
	EXPECT_EQ(extent_state.height, host_height);

	GpuMemoryCompleteSubmission(old_use);
	GpuMemoryCompleteSubmission(new_use);
	GpuMemoryFree(&ctx, addr, size);
}

TEST(EmulatorGraphicsState, GpuMemoryDoesNotVersionUnchangedBacking)
{
	EnsureGpuMemoryForTests();

	GraphicContext ctx {};
	auto*          guest = new uint8_t[0x1000] {};
	const uint64_t addr  = reinterpret_cast<uint64_t>(guest);
	const uint64_t size  = 0x1000;
	const SubmissionId submission {GpuQueueId(191), 1};

	GpuMemorySetAllocatedRange(addr, size);
	g_versioned_gpu_object_creates = 0;
	g_versioned_gpu_object_updates = 0;
	g_versioned_gpu_object_deletes = 0;

	void* const initial = GpuMemoryCreateObject(1, &ctx, nullptr, addr, size, VersionedTestGpuObject());
	ASSERT_NE(initial, nullptr);
	ASSERT_EQ(GpuMemoryFindObjectsForSubmission(submission, addr, size, GpuMemoryObjectType::VertexBuffer, true, false).Size(), 1u);

	void* const reused = GpuMemoryCreateObject(2, &ctx, nullptr, addr, size, VersionedTestGpuObject());
	EXPECT_EQ(reused, initial);
	EXPECT_EQ(g_versioned_gpu_object_creates, 1);
	EXPECT_EQ(g_versioned_gpu_object_updates, 0);
	EXPECT_EQ(g_versioned_gpu_object_deletes, 0);

	GpuMemoryCompleteSubmission(submission);
	GpuMemoryFree(&ctx, addr, size);
	EXPECT_EQ(g_versioned_gpu_object_deletes, 1);
}

TEST(EmulatorGraphicsState, GpuMemoryConcurrentUsePinsOldBackingDuringVersionCreation)
{
	EnsureGpuMemoryForTests();

	GraphicContext ctx {};
	auto*          guest = new uint8_t[0x1000] {};
	const uint64_t addr  = reinterpret_cast<uint64_t>(guest);
	const uint64_t size  = 0x1000;
	const uint64_t sequence = g_versioned_submission_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	const SubmissionId first_use {GpuQueueId(192), sequence};
	const SubmissionId concurrent_use {GpuQueueId(193), sequence};

	GpuMemorySetAllocatedRange(addr, size);
	g_versioned_gpu_object_creates = 0;
	g_versioned_gpu_object_updates = 0;
	g_versioned_gpu_object_deletes = 0;
	{
		std::lock_guard lock(g_versioned_gpu_object_mutex);
		g_versioned_gpu_object_block_second   = true;
		g_versioned_gpu_object_second_entered = false;
		g_versioned_gpu_object_release_second = false;
	}

	void* const old_backing = GpuMemoryCreateObject(1, &ctx, nullptr, addr, size, VersionedTestGpuObject());
	ASSERT_NE(old_backing, nullptr);
	ASSERT_EQ(GpuMemoryFindObjectsForSubmission(first_use, addr, size, GpuMemoryObjectType::VertexBuffer, true, false).Size(), 1u);

	guest[0] = 0xa5;
	void* new_backing = nullptr;
	std::thread version_thread(
	    [&] { new_backing = GpuMemoryCreateObject(2, &ctx, nullptr, addr, size, VersionedTestGpuObject()); });

	bool factory_entered = false;
	{
		std::unique_lock lock(g_versioned_gpu_object_mutex);
		factory_entered = g_versioned_gpu_object_cond.wait_for(
		    lock, std::chrono::seconds(2), [] { return g_versioned_gpu_object_second_entered; });
	}
	if (factory_entered)
	{
		const auto during_create =
		    GpuMemoryFindObjectsForSubmission(concurrent_use, addr, size, GpuMemoryObjectType::VertexBuffer, true, false);
		EXPECT_EQ(during_create.Size(), 1u);
		if (!during_create.IsEmpty())
		{
			EXPECT_EQ(during_create[0].obj, old_backing);
		}
	}
	{
		std::lock_guard lock(g_versioned_gpu_object_mutex);
		g_versioned_gpu_object_release_second = true;
	}
	g_versioned_gpu_object_cond.notify_all();
	version_thread.join();
	g_versioned_gpu_object_block_second = false;

	ASSERT_TRUE(factory_entered);
	ASSERT_NE(new_backing, nullptr);
	EXPECT_NE(new_backing, old_backing);
	EXPECT_EQ(g_versioned_gpu_object_deletes, 0);

	GpuMemoryCompleteSubmission(first_use);
	EXPECT_EQ(g_versioned_gpu_object_deletes, 0);
	GpuMemoryCompleteSubmission(concurrent_use);
	EXPECT_EQ(g_versioned_gpu_object_deletes, 1);

	GpuMemoryFree(&ctx, addr, size);
	EXPECT_EQ(g_versioned_gpu_object_deletes, 2);
}

TEST(EmulatorGraphicsState, GpuMemoryMutationPolicyKeepsWriteBackConflictStrict)
{
	EXPECT_EQ(GpuMemoryChooseMutationAction(false, false, true, false), GpuMemoryMutationAction::None);
	EXPECT_EQ(GpuMemoryChooseMutationAction(true, true, true, false), GpuMemoryMutationAction::None);
	EXPECT_EQ(GpuMemoryChooseMutationAction(true, false, false, false), GpuMemoryMutationAction::UpdateInPlace);
	EXPECT_EQ(GpuMemoryChooseMutationAction(true, false, true, false), GpuMemoryMutationAction::VersionBacking);
	EXPECT_EQ(GpuMemoryChooseMutationAction(true, false, true, true), GpuMemoryMutationAction::RejectWriteBackConflict);
}

TEST(EmulatorGraphicsState, GpuMemoryCoveredIndexReusePreservesOneBackingAndVersionsSafely)
{
	EnsureGpuMemoryForTests();

	GraphicContext ctx {};
	constexpr uint64_t heap_size    = 0x1000;
	constexpr uint64_t initial_size = 0x100;
	constexpr uint64_t covered_size = 0x80;
	constexpr uint64_t grown_size   = 0x180;
	auto*              guest        = new uint8_t[heap_size];
	for (uint64_t i = 0; i < heap_size; i++)
	{
		guest[i] = static_cast<uint8_t>(i);
	}
	const uint64_t base = reinterpret_cast<uint64_t>(guest);
	const uint32_t stats_index = static_cast<uint32_t>(GpuMemoryObjectType::IndexBuffer) -
	                             static_cast<uint32_t>(GpuMemoryObjectType::VideoOutBuffer);
	const uint64_t submission_sequence = g_covered_index_submission_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	const SubmissionId pending_use {GpuQueueId(210), submission_sequence};

	GpuMemorySetAllocatedRange(base, heap_size);
	g_covered_index_creates        = 0;
	g_covered_index_updates        = 0;
	g_covered_index_deletes        = 0;
	g_covered_index_invalid_update = false;

	auto* const initial = static_cast<CoveredIndexTestBacking*>(
	    GpuMemoryCreateObject(1000, &ctx, nullptr, base, initial_size, CoveredIndexTestGpuObject()));
	ASSERT_NE(initial, nullptr);
	ASSERT_EQ(initial->size, initial_size);
	ASSERT_EQ(g_covered_index_creates, 1);

	const auto before_covered = DebugStatsGetPerformanceSnapshot(false);
	auto* const covered = static_cast<CoveredIndexTestBacking*>(
	    GpuMemoryCreateObject(1001, &ctx, nullptr, base, covered_size, CoveredIndexTestGpuObject()));
	const auto after_covered = DebugStatsGetPerformanceSnapshot(false);
	EXPECT_EQ(covered, initial);
	EXPECT_EQ(g_covered_index_creates, 1);
	EXPECT_EQ(g_covered_index_deletes, 0);
	EXPECT_EQ(after_covered.gpu_memory_types[stats_index].covered_reuse,
	          before_covered.gpu_memory_types[stats_index].covered_reuse + 1);
	EXPECT_EQ(after_covered.gpu_memory_types[stats_index].reclaim_new,
	          before_covered.gpu_memory_types[stats_index].reclaim_new);
	EXPECT_EQ(after_covered.gpu_memory_types[stats_index].logical_free,
	          before_covered.gpu_memory_types[stats_index].logical_free);

	GpuMemoryOverlapSnapshot overlap;
	ASSERT_TRUE(GpuMemoryQueryOverlaps(&base, &covered_size, 1, &overlap));
	EXPECT_EQ(overlap.total_count, 1u);
	EXPECT_EQ(GpuMemoryFindObjects(base, initial_size, GpuMemoryObjectType::IndexBuffer, true, false).Size(), 1u);
	EXPECT_TRUE(GpuMemoryFindObjects(base, covered_size, GpuMemoryObjectType::IndexBuffer, true, false).IsEmpty());

	guest[0] = 0xa5;
	auto* const updated = static_cast<CoveredIndexTestBacking*>(
	    GpuMemoryCreateObject(1002, &ctx, nullptr, base, covered_size, CoveredIndexTestGpuObject()));
	ASSERT_EQ(updated, initial);
	EXPECT_EQ(g_covered_index_updates, 1);
	EXPECT_EQ(updated->bytes[0], 0xa5);
	EXPECT_EQ(updated->bytes[initial_size - 1], static_cast<uint8_t>(initial_size - 1));
	EXPECT_FALSE(g_covered_index_invalid_update);

	ASSERT_EQ(GpuMemoryFindObjectsForSubmission(pending_use, base, initial_size, GpuMemoryObjectType::IndexBuffer, true, false).Size(),
	          1u);
	guest[1] = 0x5a;
	auto* const versioned = static_cast<CoveredIndexTestBacking*>(
	    GpuMemoryCreateObject(1003, &ctx, nullptr, base, covered_size, CoveredIndexTestGpuObject()));
	ASSERT_NE(versioned, nullptr);
	EXPECT_NE(versioned, updated);
	EXPECT_EQ(g_covered_index_creates, 2);
	EXPECT_EQ(g_covered_index_updates, 1);
	EXPECT_EQ(g_covered_index_deletes, 0);
	EXPECT_EQ(versioned->bytes[0], 0xa5);
	EXPECT_EQ(versioned->bytes[1], 0x5a);

	auto* const versioned_reuse = static_cast<CoveredIndexTestBacking*>(
	    GpuMemoryCreateObject(1004, &ctx, nullptr, base, covered_size, CoveredIndexTestGpuObject()));
	EXPECT_EQ(versioned_reuse, versioned);
	EXPECT_EQ(g_covered_index_creates, 2);
	EXPECT_EQ(g_covered_index_updates, 1);
	EXPECT_EQ(g_covered_index_deletes, 0);
	EXPECT_EQ(GpuMemoryFindObjects(base, initial_size, GpuMemoryObjectType::IndexBuffer, true, false).Size(), 1u);

	GpuMemoryCompleteSubmission(pending_use);
	EXPECT_EQ(g_covered_index_deletes, 1);

	const auto before_growth = DebugStatsGetPerformanceSnapshot(false);
	auto* const grown = static_cast<CoveredIndexTestBacking*>(
	    GpuMemoryCreateObject(1005, &ctx, nullptr, base, grown_size, CoveredIndexTestGpuObject()));
	const auto after_growth = DebugStatsGetPerformanceSnapshot(false);
	ASSERT_NE(grown, nullptr);
	EXPECT_NE(grown, versioned);
	EXPECT_EQ(grown->size, grown_size);
	EXPECT_EQ(g_covered_index_creates, 3);
	EXPECT_EQ(g_covered_index_deletes, 2);
	EXPECT_EQ(after_growth.gpu_memory_types[stats_index].reclaim_new,
	          before_growth.gpu_memory_types[stats_index].reclaim_new + 1);
	EXPECT_EQ(after_growth.gpu_memory_types[stats_index].logical_free,
	          before_growth.gpu_memory_types[stats_index].logical_free + 1);

	auto* const grown_covered = static_cast<CoveredIndexTestBacking*>(
	    GpuMemoryCreateObject(1006, &ctx, nullptr, base, covered_size, CoveredIndexTestGpuObject()));
	EXPECT_EQ(grown_covered, grown);
	EXPECT_EQ(g_covered_index_creates, 3);
	EXPECT_EQ(g_covered_index_deletes, 2);
	EXPECT_EQ(GpuMemoryFindObjects(base, grown_size, GpuMemoryObjectType::IndexBuffer, true, false).Size(), 1u);

	const uint64_t shifted_base = base + 4;
	const uint64_t shifted_size = 0x40;
	const auto     before_shift  = DebugStatsGetPerformanceSnapshot(false);
	auto* const shifted = static_cast<CoveredIndexTestBacking*>(
	    GpuMemoryCreateObject(1007, &ctx, nullptr, shifted_base, shifted_size, CoveredIndexTestGpuObject()));
	const auto after_shift = DebugStatsGetPerformanceSnapshot(false);
	ASSERT_NE(shifted, nullptr);
	EXPECT_NE(shifted, grown);
	EXPECT_EQ(g_covered_index_creates, 4);
	EXPECT_EQ(g_covered_index_deletes, 3);
	EXPECT_EQ(after_shift.gpu_memory_types[stats_index].covered_reuse,
	          before_shift.gpu_memory_types[stats_index].covered_reuse);
	EXPECT_EQ(after_shift.gpu_memory_types[stats_index].reclaim_new,
	          before_shift.gpu_memory_types[stats_index].reclaim_new + 1);
	EXPECT_EQ(after_shift.gpu_memory_types[stats_index].logical_free,
	          before_shift.gpu_memory_types[stats_index].logical_free + 1);
	EXPECT_EQ(GpuMemoryFindObjects(shifted_base, shifted_size, GpuMemoryObjectType::IndexBuffer, true, false).Size(), 1u);

	GpuMemoryFree(&ctx, shifted_base, shifted_size);
	EXPECT_EQ(g_covered_index_deletes, 4);
	EXPECT_FALSE(g_covered_index_invalid_update);
}

TEST(EmulatorGraphicsState, GpuMemoryOverlapQueryIsBoundedAndReadOnly)
{
	EnsureGpuMemoryForTests();

	GraphicContext ctx {};
	const uint64_t heap_base        = 0x0000005200000000ull;
	const uint64_t second_heap_base = heap_base + 0x40000ull;
	const uint64_t heap_size        = 0x20000ull;
	const uint64_t first            = heap_base + 0x4000ull;
	const uint64_t second           = second_heap_base + 0x6000ull;
	const uint64_t obj_size         = 0x1000ull;
	GpuMemorySetAllocatedRange(heap_base, heap_size);
	GpuMemorySetAllocatedRange(second_heap_base, heap_size);

	g_test_gpu_object_deletes = 0;
	ASSERT_NE(GpuMemoryCreateObject(1, &ctx, nullptr, first, obj_size,
	                                TestGpuObject(GpuMemoryObjectType::StorageBuffer, true)),
	          nullptr);
	ASSERT_NE(GpuMemoryCreateObject(1, &ctx, nullptr, second, obj_size, TestGpuObject(GpuMemoryObjectType::VertexBuffer)), nullptr);

	GpuMemoryOverlapSnapshot snapshot;
	const uint64_t           none_addr = heap_base + 0x9000ull;
	ASSERT_TRUE(GpuMemoryQueryOverlaps(&none_addr, &obj_size, 1, &snapshot));
	EXPECT_EQ(snapshot.total_count, 0u);
	EXPECT_EQ(snapshot.entry_count, 0u);
	EXPECT_FALSE(snapshot.truncated);

	const auto stats_before = DebugStatsGetPerformanceSnapshot(false);
	ASSERT_TRUE(GpuMemoryQueryOverlaps(&first, &obj_size, 1, &snapshot));
	ASSERT_EQ(snapshot.total_count, 1u);
	ASSERT_EQ(snapshot.entry_count, 1u);
	EXPECT_EQ(snapshot.exact_count, 1u);
	EXPECT_EQ(snapshot.entries[0].type, GpuMemoryObjectType::StorageBuffer);
	EXPECT_EQ(snapshot.entries[0].relation, GpuMemoryOverlapType::Equals);
	EXPECT_TRUE(snapshot.entries[0].exact);
	EXPECT_EQ(snapshot.entries[0].count, 1u);
	EXPECT_TRUE(snapshot.entries[0].all_read_only);

	const uint64_t partial_addr = first + 0x800ull;
	ASSERT_TRUE(GpuMemoryQueryOverlaps(&partial_addr, &obj_size, 1, &snapshot));
	ASSERT_EQ(snapshot.total_count, 1u);
	EXPECT_EQ(snapshot.entries[0].relation, GpuMemoryOverlapType::Crosses);
	EXPECT_FALSE(snapshot.entries[0].exact);

	const uint64_t covering_size = (second + obj_size) - first;
	ASSERT_TRUE(GpuMemoryQueryOverlaps(&first, &covering_size, 1, &snapshot));
	EXPECT_EQ(snapshot.total_count, 2u);
	EXPECT_EQ(snapshot.entry_count, 2u);
	EXPECT_EQ(snapshot.exact_count, 0u);
	EXPECT_FALSE(snapshot.truncated);

	const uint64_t outer_addr = heap_base - 0x1000ull;
	const uint64_t outer_size = (second_heap_base + heap_size + 0x1000ull) - outer_addr;
	ASSERT_TRUE(GpuMemoryQueryOverlaps(&outer_addr, &outer_size, 1, &snapshot));
	EXPECT_EQ(snapshot.total_count, 2u);

	const uint64_t range_addrs[] = {first, second};
	const uint64_t range_sizes[] = {obj_size, obj_size};
	ASSERT_TRUE(GpuMemoryQueryOverlaps(range_addrs, range_sizes, 2, &snapshot));
	EXPECT_EQ(snapshot.total_count, 2u);
	EXPECT_EQ(snapshot.exact_count, 0u);

	const uint64_t last_addr = UINT64_MAX;
	const uint64_t last_size = 1;
	ASSERT_TRUE(GpuMemoryQueryOverlaps(&last_addr, &last_size, 1, &snapshot));
	EXPECT_EQ(snapshot.total_count, 0u);

	const auto stats_after = DebugStatsGetPerformanceSnapshot(false);
	EXPECT_EQ(stats_after.hash_calls, stats_before.hash_calls);
	EXPECT_EQ(stats_after.hash_bytes, stats_before.hash_bytes);
	EXPECT_EQ(stats_after.hash_ns, stats_before.hash_ns);
	EXPECT_EQ(stats_after.gpu_memory_create_calls, stats_before.gpu_memory_create_calls);
	EXPECT_EQ(stats_after.gpu_memory_create_ns, stats_before.gpu_memory_create_ns);
	EXPECT_EQ(stats_after.live_objects, stats_before.live_objects);
	for (uint32_t type = 0; type < kDebugStatsGpuMemoryTypeCount; type++)
	{
		EXPECT_EQ(stats_after.gpu_memory_types[type].fast_reuse, stats_before.gpu_memory_types[type].fast_reuse);
		EXPECT_EQ(stats_after.gpu_memory_types[type].exact_reuse, stats_before.gpu_memory_types[type].exact_reuse);
		EXPECT_EQ(stats_after.gpu_memory_types[type].covered_reuse, stats_before.gpu_memory_types[type].covered_reuse);
		EXPECT_EQ(stats_after.gpu_memory_types[type].new_standalone, stats_before.gpu_memory_types[type].new_standalone);
		EXPECT_EQ(stats_after.gpu_memory_types[type].new_linked, stats_before.gpu_memory_types[type].new_linked);
		EXPECT_EQ(stats_after.gpu_memory_types[type].new_from_objects, stats_before.gpu_memory_types[type].new_from_objects);
		EXPECT_EQ(stats_after.gpu_memory_types[type].reclaim_new, stats_before.gpu_memory_types[type].reclaim_new);
		EXPECT_EQ(stats_after.gpu_memory_types[type].logical_free, stats_before.gpu_memory_types[type].logical_free);
		EXPECT_EQ(stats_after.gpu_memory_types[type].live, stats_before.gpu_memory_types[type].live);
	}
	EXPECT_EQ(g_test_gpu_object_deletes, 0);
	EXPECT_EQ(GpuMemoryFindObjects(first, obj_size, GpuMemoryObjectType::StorageBuffer, true, false).Size(), 1u);
	EXPECT_EQ(GpuMemoryFindObjects(second, obj_size, GpuMemoryObjectType::VertexBuffer, true, false).Size(), 1u);

	GpuMemoryFree(&ctx, first, obj_size);
	GpuMemoryFree(&ctx, second, obj_size);
	EXPECT_EQ(g_test_gpu_object_deletes, 2);
}

TEST(EmulatorGraphicsState, StorageBufferBackingIdentityIgnoresViewShape)
{
	const StorageBufferGpuObject dword_view(4, 16, true);
	const StorageBufferGpuObject vec4_view(16, 4, true);

	EXPECT_TRUE(dword_view.Equal(vec4_view.params));
	EXPECT_TRUE(vec4_view.Equal(dword_view.params));
	EXPECT_FALSE(dword_view.Equal(nullptr));
}

TEST(EmulatorGraphicsState, ClassifiesTransientBufferOverlapSnapshotsStrictly)
{
	GpuMemoryOverlapSnapshot empty;
	EXPECT_TRUE(GpuMemoryOverlapsAllowTransientReadOnlyBuffer(empty));

	GpuMemoryOverlapSnapshot read_only_storage;
	read_only_storage.total_count              = 2;
	read_only_storage.entry_count              = 1;
	read_only_storage.entries[0].type          = GpuMemoryObjectType::StorageBuffer;
	read_only_storage.entries[0].relation      = GpuMemoryOverlapType::Crosses;
	read_only_storage.entries[0].count         = 2;
	read_only_storage.entries[0].all_read_only = true;
	EXPECT_TRUE(GpuMemoryOverlapsAllowTransientReadOnlyBuffer(read_only_storage));

	auto read_only_vertex            = read_only_storage;
	read_only_vertex.entries[0].type = GpuMemoryObjectType::VertexBuffer;
	EXPECT_TRUE(GpuMemoryOverlapsAllowTransientReadOnlyBuffer(read_only_vertex));

	auto read_only_index            = read_only_storage;
	read_only_index.entries[0].type = GpuMemoryObjectType::IndexBuffer;
	EXPECT_TRUE(GpuMemoryOverlapsAllowTransientReadOnlyBuffer(read_only_index));

	auto writable_storage                     = read_only_storage;
	writable_storage.entries[0].all_read_only = false;
	EXPECT_FALSE(GpuMemoryOverlapsAllowTransientReadOnlyBuffer(writable_storage));

	auto surface            = read_only_storage;
	surface.entries[0].type = GpuMemoryObjectType::RenderTexture;
	EXPECT_FALSE(GpuMemoryOverlapsAllowTransientReadOnlyBuffer(surface));

	auto truncated      = read_only_storage;
	truncated.truncated = true;
	EXPECT_FALSE(GpuMemoryOverlapsAllowTransientReadOnlyBuffer(truncated));
}

TEST(EmulatorGraphicsState, UsesTransientBuffersOnlyForSmallSafeReadOnlyViews)
{
	EXPECT_TRUE(GpuMemoryCanUseTransientReadOnlyBuffer(true, 0x80u, true, true));
	EXPECT_TRUE(GpuMemoryCanUseTransientReadOnlyBuffer(true, 0x1000u, true, true));
	EXPECT_FALSE(GpuMemoryCanUseTransientReadOnlyBuffer(false, 0x80u, true, false));
	EXPECT_FALSE(GpuMemoryCanUseTransientReadOnlyBuffer(true, 0x1001u, true, false));
	EXPECT_FALSE(GpuMemoryCanUseTransientReadOnlyBuffer(true, 0x80u, false, false));
	EXPECT_FALSE(GpuMemoryCanUseTransientReadOnlyBuffer(true, 0x80u, true, false));
}

TEST(EmulatorGraphicsState, VertexAndIndexBuffersDeclareReadOnlyGpuUse)
{
	EXPECT_TRUE(VertexBufferGpuObject().read_only);
	EXPECT_TRUE(IndexBufferGpuObject().read_only);
}

TEST(EmulatorGraphicsState, TransientBufferPoolReservesCapacityPerUsage)
{
	EXPECT_TRUE(GpuMemoryTransientBufferPoolCanAllocate(0u, 512u, 0u, 0x80u));
	EXPECT_FALSE(GpuMemoryTransientBufferPoolCanAllocate(512u, 512u, 0u, 0x80u));
	EXPECT_FALSE(GpuMemoryTransientBufferPoolCanAllocate(0u, 1536u, 0u, 0x80u));
	EXPECT_FALSE(GpuMemoryTransientBufferPoolCanAllocate(0u, 0u, 16u * 1024u * 1024u, 0x80u));
	EXPECT_FALSE(GpuMemoryTransientBufferPoolCanAllocate(0u, 0u, 0u, 0u));
}

TEST(EmulatorGraphicsState, GpuMemoryAccumulatesWriteIntentUntilWriteBack)
{
	EXPECT_FALSE(GpuMemoryMergeReadOnlyUse(true, false, true));
	EXPECT_FALSE(GpuMemoryMergeReadOnlyUse(true, true, false));
	EXPECT_TRUE(GpuMemoryMergeReadOnlyUse(true, true, true));

	EXPECT_TRUE(GpuMemoryMergeReadOnlyUse(false, false, true));
	EXPECT_FALSE(GpuMemoryMergeReadOnlyUse(false, true, false));
}

TEST(EmulatorGraphicsState, BlitInitializesUndefinedSource)
{
	EXPECT_TRUE(UtilBlitImageNeedsSourceInitialization(VK_IMAGE_LAYOUT_UNDEFINED));
	EXPECT_FALSE(UtilBlitImageNeedsSourceInitialization(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
	EXPECT_FALSE(UtilBlitImageNeedsSourceInitialization(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
}

TEST(EmulatorGraphicsState, MapsColorExportComponents)
{
	EXPECT_EQ(ShaderColorExportSourceComponent(0, 0), 0u);
	EXPECT_EQ(ShaderColorExportSourceComponent(0, 3), 3u);
	EXPECT_EQ(ShaderColorExportSourceComponent(1, 0), 2u);
	EXPECT_EQ(ShaderColorExportSourceComponent(1, 3), 3u);
	EXPECT_EQ(ShaderColorExportSourceComponent(2, 0), 3u);
	EXPECT_EQ(ShaderColorExportSourceComponent(2, 3), 0u);
	EXPECT_EQ(ShaderColorExportSourceComponent(3, 0), 3u);
	EXPECT_EQ(ShaderColorExportSourceComponent(3, 3), 2u);
}

TEST(EmulatorGraphicsState, DecodesGenericScissorHalves)
{
	HW::Context context;

	State::SetGenericScissorTl(context, 0x80020001u);
	State::SetGenericScissorBr(context, 0x00140010u);

	const auto& viewport = context.GetScreenViewport();
	EXPECT_EQ(viewport.generic_scissor_left, 1);
	EXPECT_EQ(viewport.generic_scissor_top, 2);
	EXPECT_EQ(viewport.generic_scissor_right, 16);
	EXPECT_EQ(viewport.generic_scissor_bottom, 20);
	EXPECT_FALSE(viewport.generic_scissor_window_offset_enable);
}

TEST(EmulatorGraphicsState, DecodesScreenScissorHalves)
{
	HW::Context context;

	State::SetScreenScissorTl(context, 0x000A0005u);
	State::SetScreenScissorBr(context, 0x00C800B4u);

	const auto& viewport = context.GetScreenViewport();
	EXPECT_EQ(viewport.screen_scissor_left, 5);
	EXPECT_EQ(viewport.screen_scissor_top, 10);
	EXPECT_EQ(viewport.screen_scissor_right, 180);
	EXPECT_EQ(viewport.screen_scissor_bottom, 200);
}

TEST(EmulatorGraphicsState, DecodesRenderControl)
{
	HW::Context context;

	const uint32_t value = (1u << Pm4::DB_RENDER_CONTROL_DEPTH_CLEAR_ENABLE_SHIFT) |
	                       (1u << Pm4::DB_RENDER_CONTROL_STENCIL_CLEAR_ENABLE_SHIFT) |
	                       (1u << Pm4::DB_RENDER_CONTROL_DEPTH_COMPRESS_DISABLE_SHIFT) |
	                       (3u << Pm4::DB_RENDER_CONTROL_COPY_SAMPLE_SHIFT);

	State::SetRenderControl(context, value);

	const auto& rc = context.GetRenderControl();
	EXPECT_TRUE(rc.depth_clear_enable);
	EXPECT_TRUE(rc.stencil_clear_enable);
	EXPECT_FALSE(rc.resummarize_enable);
	EXPECT_FALSE(rc.stencil_compress_disable);
	EXPECT_TRUE(rc.depth_compress_disable);
	EXPECT_FALSE(rc.copy_centroid);
	EXPECT_EQ(rc.copy_sample, 3u);
}

TEST(EmulatorGraphicsState, DecodesStencilControlAndRefMaskHalves)
{
	HW::Context context;

	const uint32_t control = (1u << Pm4::DB_STENCIL_CONTROL_STENCILFAIL_SHIFT) |
	                         (2u << Pm4::DB_STENCIL_CONTROL_STENCILZPASS_SHIFT) |
	                         (3u << Pm4::DB_STENCIL_CONTROL_STENCILZFAIL_SHIFT) |
	                         (4u << Pm4::DB_STENCIL_CONTROL_STENCILFAIL_BF_SHIFT) |
	                         (5u << Pm4::DB_STENCIL_CONTROL_STENCILZPASS_BF_SHIFT) |
	                         (6u << Pm4::DB_STENCIL_CONTROL_STENCILZFAIL_BF_SHIFT);
	State::SetStencilControl(context, control);

	const auto& sc = context.GetStencilControl();
	EXPECT_EQ(sc.stencil_fail, 1u);
	EXPECT_EQ(sc.stencil_zpass, 2u);
	EXPECT_EQ(sc.stencil_zfail, 3u);
	EXPECT_EQ(sc.stencil_fail_bf, 4u);
	EXPECT_EQ(sc.stencil_zpass_bf, 5u);
	EXPECT_EQ(sc.stencil_zfail_bf, 6u);

	State::SetStencilRefMask(context, 0x04030201u);
	State::SetStencilRefMaskBf(context, 0x08070605u);

	const auto& sm = context.GetStencilMask();
	EXPECT_EQ(sm.stencil_testval, 0x01u);
	EXPECT_EQ(sm.stencil_mask, 0x02u);
	EXPECT_EQ(sm.stencil_writemask, 0x03u);
	EXPECT_EQ(sm.stencil_opval, 0x04u);
	EXPECT_EQ(sm.stencil_testval_bf, 0x05u);
	EXPECT_EQ(sm.stencil_mask_bf, 0x06u);
	EXPECT_EQ(sm.stencil_writemask_bf, 0x07u);
	EXPECT_EQ(sm.stencil_opval_bf, 0x08u);
}

TEST(EmulatorGraphicsState, Gen5SampledRgba8FormatUsesUnormByDefault)
{
	EXPECT_EQ(Kyty::Libs::Graphics::TextureResolveSampledVkFormat(0, 0, 56), VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(Kyty::Libs::Graphics::TextureResolveSampledVkFormat(0, 0, 56, true), VK_FORMAT_R8G8B8A8_SRGB);
	EXPECT_EQ(Kyty::Libs::Graphics::TextureResolveSampledVkFormat(0, 0, 13), VK_FORMAT_R16_SFLOAT);
	EXPECT_EQ(Kyty::Libs::Graphics::ShaderGen5TextureBytesPerElement(13), 2u);
	EXPECT_EQ(Kyty::Libs::Graphics::TextureResolveSampledVkFormat(0, 0, 14), VK_FORMAT_R8G8_UNORM);
	EXPECT_EQ(Kyty::Libs::Graphics::TextureResolveSampledVkFormat(0, 0, 71), VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(Kyty::Libs::Graphics::TextureResolveSampledVkFormat(0, 0, 133), VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
}

TEST(EmulatorGraphicsState, Gen5SharpSampledTextureAcceptsTexture2DType)
{
	HW::UserSgprInfo user_sgpr {};
	for (int i = 0; i < 8; i++)
	{
		user_sgpr.type[i] = HW::UserSgprType::Vsharp;
	}
	user_sgpr.value[3] = 9u << 28u;

	ShaderSharp read_only_texture {};
	read_only_texture.offset_dw = 0;
	read_only_texture.size      = 0;

	ShaderUserData user_data {};
	user_data.sharp_resource_offset[0] = &read_only_texture;
	user_data.sharp_resource_count[0]  = 1;

	ShaderParsedUsage  usage {};
	ShaderBindResources bind {};

	ShaderParseUsage2(&user_data, &usage, &bind, user_sgpr, 8);

	ASSERT_EQ(bind.textures2D.textures_num, 1);
	EXPECT_EQ(bind.textures2D.textures2d_sampled_num, 1);
	EXPECT_EQ(bind.textures2D.desc[0].texture.Type(), 9);
	EXPECT_EQ(bind.textures2D.desc[0].usage, ShaderTextureUsage::ReadOnly);
	EXPECT_EQ(usage.textures2D_readonly, 1);
}

TEST(EmulatorGraphicsState, Gen5SharpNullBufferDescriptorIsNotStorageBuffer)
{
	HW::UserSgprInfo user_sgpr {};
	for (int i = 0; i < 4; i++)
	{
		user_sgpr.type[i] = HW::UserSgprType::Region;
	}

	ShaderSharp read_only_buffer {};
	read_only_buffer.offset_dw = 0;
	read_only_buffer.size      = 1;

	ShaderUserData user_data {};
	user_data.sharp_resource_offset[0] = &read_only_buffer;
	user_data.sharp_resource_count[0]  = 1;

	ShaderParsedUsage  usage {};
	ShaderBindResources bind {};

	ShaderParseUsage2(&user_data, &usage, &bind, user_sgpr, 4);

	EXPECT_EQ(bind.storage_buffers.buffers_num, 0);
	EXPECT_EQ(usage.storage_buffers_constant, 0);
	EXPECT_EQ(bind.direct_sgprs.sgprs_num, 4);
}

TEST(EmulatorGraphicsState, Gen5CodeUnavailableSkipsInvalidDirectStorageDescriptor)
{
	HW::UserSgprInfo user_sgpr {};
	for (int i = 0; i < 4; i++)
	{
		user_sgpr.type[i] = HW::UserSgprType::Region;
	}

	ShaderBufferResource invalid {};
	invalid.fields[0] = 0x3f068687u;
	invalid.fields[1] = (0x3f16u << 16u) | 0x9697u; // non-dword-aligned stride 16150
	invalid.fields[2] = 1058115986u;
	invalid.fields[3] = 0u; // no typed format/swizzle evidence
	for (int i = 0; i < 4; i++)
	{
		user_sgpr.value[i] = invalid.fields[i];
	}

	uint16_t direct_offsets[1] = {0};
	ShaderUserData user_data {};
	user_data.direct_resource_offset = direct_offsets;
	user_data.direct_resource_count  = 1;

	ShaderParsedUsage  usage {};
	ShaderBindResources bind {};

	ShaderParseUsage2(&user_data, &usage, &bind, user_sgpr, 4, nullptr);

	EXPECT_EQ(bind.storage_buffers.buffers_num, 0);
	EXPECT_EQ(usage.storage_buffers_readonly, 0);
	EXPECT_EQ(bind.direct_sgprs.sgprs_num, 4);
}

TEST(EmulatorGraphicsState, RawStorageDescriptorOnlyRequiresDwordStride)
{
	ShaderBufferResource raw {};
	raw.fields[1] = 12u << 16u;
	raw.fields[3] = 0xffffffffu;
	EXPECT_TRUE(ShaderRawStorageDescriptorSupported(raw));

	raw.fields[1] = 10u << 16u;
	EXPECT_FALSE(ShaderRawStorageDescriptorSupported(raw));

	raw.fields[1] = 0u;
	raw.fields[2] = 0x6000u;
	EXPECT_TRUE(ShaderRawStorageDescriptorSupported(raw));
}

TEST(EmulatorGraphicsState, Gen5DirectImageSampleBindsTextureAndSampler)
{
	const uint32_t word0 = (0x3cu << 26u) | (0x20u << 18u) | (0xfu << 8u);
	const uint32_t word1 = 2u << 21u; // T#0 = s[0:7], S#2 = s[8:11].
	const uint32_t shader[] = {word0, word1, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	HW::UserSgprInfo user_sgpr {};
	for (int i = 0; i < 12; i++)
	{
		user_sgpr.type[i] = HW::UserSgprType::Region;
	}
	user_sgpr.value[3] = 9u << 28u;

	uint16_t direct_offsets[2] = {0xffffu, 0u};
	ShaderUserData user_data {};
	user_data.direct_resource_offset = direct_offsets;
	user_data.direct_resource_count  = 2;
	user_data.srt_size_dw            = 4;

	ShaderParsedUsage  usage {};
	ShaderBindResources bind {};
	ShaderParseUsage2(&user_data, &usage, &bind, user_sgpr, 12, &code);

	EXPECT_EQ(bind.storage_buffers.buffers_num, 0);
	ASSERT_EQ(bind.textures2D.textures_num, 1);
	EXPECT_EQ(bind.textures2D.desc[0].start_register, 0);
	EXPECT_EQ(bind.textures2D.desc[0].usage, ShaderTextureUsage::ReadOnly);
	ASSERT_EQ(bind.samplers.samplers_num, 1);
	EXPECT_EQ(bind.samplers.start_register[0], 8);
}

TEST(EmulatorGraphicsState, Gen5DirectSgprsAllowFullUserWindow)
{
	HW::UserSgprInfo user_sgpr {};
	for (int i = 0; i < 28; i++)
	{
		user_sgpr.type[i] = HW::UserSgprType::Region;
	}

	ShaderUserData user_data {};

	ShaderParsedUsage  usage {};
	ShaderBindResources bind {};

	ShaderParseUsage2(&user_data, &usage, &bind, user_sgpr, 28);

	EXPECT_EQ(bind.direct_sgprs.sgprs_num, 28);
	EXPECT_EQ(usage.direct_sgprs, 28);
}

TEST(EmulatorGraphicsState, DecodesModeControl)
{
	HW::Context context;

	const uint32_t value = (1u << Pm4::PA_SU_SC_MODE_CNTL_CULL_FRONT_SHIFT) |
	                       (1u << Pm4::PA_SU_SC_MODE_CNTL_CULL_BACK_SHIFT) |
	                       (1u << Pm4::PA_SU_SC_MODE_CNTL_FACE_SHIFT) |
	                       (2u << Pm4::PA_SU_SC_MODE_CNTL_POLY_MODE_SHIFT) |
	                       (3u << Pm4::PA_SU_SC_MODE_CNTL_POLYMODE_FRONT_PTYPE_SHIFT) |
	                       (4u << Pm4::PA_SU_SC_MODE_CNTL_POLYMODE_BACK_PTYPE_SHIFT) |
	                       (1u << Pm4::PA_SU_SC_MODE_CNTL_PROVOKING_VTX_LAST_SHIFT);

	State::SetModeControl(context, value);

	const auto& mode = context.GetModeControl();
	EXPECT_TRUE(mode.cull_front);
	EXPECT_TRUE(mode.cull_back);
	EXPECT_TRUE(mode.face);
	EXPECT_EQ(mode.poly_mode, 2);
	EXPECT_EQ(mode.polymode_front_ptype, 3);
	EXPECT_EQ(mode.polymode_back_ptype, 4);
	EXPECT_TRUE(mode.provoking_vtx_last);
}

TEST(EmulatorGraphicsState, DecodesPolygonOffsetRegisters)
{
	HW::Context context;

	State::SetPolygonOffsetRegister(context, Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL, 0x1e9u);
	State::SetPolygonOffsetRegister(context, Pm4::PA_SU_POLY_OFFSET_CLAMP, 0x40200000u);
	State::SetPolygonOffsetRegister(context, Pm4::PA_SU_POLY_OFFSET_FRONT_SCALE, 0x42000000u);
	State::SetPolygonOffsetRegister(context, Pm4::PA_SU_POLY_OFFSET_FRONT_OFFSET, 0x40800000u);
	State::SetPolygonOffsetRegister(context, Pm4::PA_SU_POLY_OFFSET_BACK_SCALE, 0x42400000u);
	State::SetPolygonOffsetRegister(context, Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET, 0x40c00000u);

	const auto& offset = context.GetPolygonOffset();
	EXPECT_EQ(offset.db_format_control, 0x1e9u);
	EXPECT_FLOAT_EQ(offset.clamp, 2.5f);
	EXPECT_FLOAT_EQ(offset.front_scale, 32.0f);
	EXPECT_FLOAT_EQ(offset.front_offset, 4.0f);
	EXPECT_FLOAT_EQ(offset.back_scale, 48.0f);
	EXPECT_FLOAT_EQ(offset.back_offset, 6.0f);
}

TEST(EmulatorGraphicsState, ResolvesFrontPolygonOffsetForVulkan)
{
	HW::ModeControl mode;
	mode.poly_offset_front_enable = true;

	HW::PolygonOffset offset;
	offset.clamp        = 2.5f;
	offset.front_scale  = 32.0f;
	offset.front_offset = 4.0f;
	offset.back_scale   = 48.0f;
	offset.back_offset  = 6.0f;

	const auto bias = State::ResolveDepthBias(mode, offset);
	EXPECT_TRUE(bias.enabled);
	EXPECT_FLOAT_EQ(bias.constant_factor, 4.0f);
	EXPECT_FLOAT_EQ(bias.clamp, 2.5f);
	EXPECT_FLOAT_EQ(bias.slope_factor, 2.0f);
}

TEST(EmulatorGraphicsState, ResolvesBackPolygonOffsetWhenFrontIsDisabled)
{
	HW::ModeControl mode;
	mode.poly_offset_back_enable = true;

	HW::PolygonOffset offset;
	offset.clamp       = 1.5f;
	offset.back_scale  = 48.0f;
	offset.back_offset = 6.0f;

	const auto bias = State::ResolveDepthBias(mode, offset);
	EXPECT_TRUE(bias.enabled);
	EXPECT_FLOAT_EQ(bias.constant_factor, 6.0f);
	EXPECT_FLOAT_EQ(bias.clamp, 1.5f);
	EXPECT_FLOAT_EQ(bias.slope_factor, 3.0f);
}

TEST(EmulatorGraphicsState, DisablesDepthBiasWhenPolygonOffsetIsDisabled)
{
	const auto bias = State::ResolveDepthBias(HW::ModeControl {}, HW::PolygonOffset {});
	EXPECT_FALSE(bias.enabled);
	EXPECT_FLOAT_EQ(bias.constant_factor, 0.0f);
	EXPECT_FLOAT_EQ(bias.clamp, 0.0f);
	EXPECT_FLOAT_EQ(bias.slope_factor, 0.0f);
}

TEST(EmulatorGraphicsState, AcceptsSupportedPixelInputLayouts)
{
	EXPECT_TRUE(ShaderPixelInputMaskSupported(0x00000002u, 0x00000002u));
	EXPECT_TRUE(ShaderPixelInputMaskSupported(0x00000020u, 0x00000020u));
	EXPECT_TRUE(ShaderPixelInputMaskSupported(0x00000302u, 0x00000302u));
	EXPECT_TRUE(ShaderPixelInputMaskSupported(0x00000320u, 0x00000320u));

	EXPECT_FALSE(ShaderPixelInputMaskSupported(0x00000022u, 0x00000022u));
	EXPECT_FALSE(ShaderPixelInputMaskSupported(0x00000120u, 0x00000120u));
	EXPECT_FALSE(ShaderPixelInputMaskSupported(0x00000020u, 0x00000002u));
}

TEST(EmulatorGraphicsState, DetectsPixelPositionFromInputBits)
{
	EXPECT_FALSE(ShaderPixelPositionEnabled(0x00000020u, 0x00000020u));
	EXPECT_TRUE(ShaderPixelPositionEnabled(0x00000320u, 0x00000320u));
}

TEST(EmulatorGraphicsState, DecodesBlendControl)
{
	HW::Context context;

	const uint32_t value = (5u << Pm4::CB_BLEND0_CONTROL_COLOR_SRCBLEND_SHIFT) |
	                       (3u << Pm4::CB_BLEND0_CONTROL_COLOR_COMB_FCN_SHIFT) |
	                       (6u << Pm4::CB_BLEND0_CONTROL_COLOR_DESTBLEND_SHIFT) |
	                       (7u << Pm4::CB_BLEND0_CONTROL_ALPHA_SRCBLEND_SHIFT) |
	                       (2u << Pm4::CB_BLEND0_CONTROL_ALPHA_COMB_FCN_SHIFT) |
	                       (8u << Pm4::CB_BLEND0_CONTROL_ALPHA_DESTBLEND_SHIFT) |
	                       (1u << Pm4::CB_BLEND0_CONTROL_SEPARATE_ALPHA_BLEND_SHIFT) |
	                       (1u << Pm4::CB_BLEND0_CONTROL_ENABLE_SHIFT);

	// Slot 0 and slot 1 (CB_BLEND1_CONTROL = 0x1e1, captured on indirect CX path)
	// share the same decoder.
	for (uint32_t slot = 0; slot < 8; slot++)
	{
		State::SetBlendControl(context, slot, value);
		const auto& blend = context.GetBlendControl(slot);
		EXPECT_EQ(blend.color_srcblend, 5) << "slot " << slot;
		EXPECT_EQ(blend.color_comb_fcn, 3) << "slot " << slot;
		EXPECT_EQ(blend.color_destblend, 6) << "slot " << slot;
		EXPECT_EQ(blend.alpha_srcblend, 7) << "slot " << slot;
		EXPECT_EQ(blend.alpha_comb_fcn, 2) << "slot " << slot;
		EXPECT_EQ(blend.alpha_destblend, 8) << "slot " << slot;
		EXPECT_TRUE(blend.separate_alpha_blend) << "slot " << slot;
		EXPECT_TRUE(blend.enable) << "slot " << slot;
	}
	// Register spacing matches direct+indirect jump tables.
	EXPECT_EQ(Pm4::CB_BLEND0_CONTROL + 1u, 0x1e1u);
}

TEST(EmulatorGraphicsState, IntersectsEnabledScissorRectangles)
{
	HW::Context context;
	context.SetScreenScissor(5, 15, 180, 200);
	context.SetGenericScissor(20, 5, 170, 190, false);
	context.SetViewportScissor(0, 10, 25, 160, 180, false);

	HW::ScanModeControl mode;
	mode.vport_scissor_enable = true;

	const auto scissor = State::ResolveScissor(context.GetScreenViewport(), mode, 0);
	EXPECT_EQ(scissor.left, 20);
	EXPECT_EQ(scissor.top, 25);
	EXPECT_EQ(scissor.right, 160);
	EXPECT_EQ(scissor.bottom, 180);
}

TEST(EmulatorGraphicsState, IgnoresViewportScissorWhenDisabled)
{
	HW::Context context;
	context.SetScreenScissor(5, 15, 180, 200);
	context.SetGenericScissor(20, 5, 170, 190, false);
	context.SetViewportScissor(0, 40, 50, 100, 120, false);

	HW::ScanModeControl mode;
	mode.vport_scissor_enable = false;

	const auto scissor = State::ResolveScissor(context.GetScreenViewport(), mode, 0);
	EXPECT_EQ(scissor.left, 20);
	EXPECT_EQ(scissor.top, 15);
	EXPECT_EQ(scissor.right, 170);
	EXPECT_EQ(scissor.bottom, 190);
}

TEST(EmulatorGraphicsState, RepresentsEmptyScissorIntersectionWithoutUnsignedWrap)
{
	HW::Context context;
	context.SetScreenScissor(0, 0, 10, 10);
	context.SetGenericScissor(20, 30, 40, 50, false);

	HW::ScanModeControl mode;

	const auto scissor = State::ResolveScissor(context.GetScreenViewport(), mode, 0);
	EXPECT_EQ(scissor.left, 20);
	EXPECT_EQ(scissor.top, 30);
	EXPECT_EQ(scissor.right, 20);
	EXPECT_EQ(scissor.bottom, 30);
}

TEST(EmulatorGraphicsState, ResolvesViewportAndGenericScissorWithoutScreenState)
{
	HW::Context context;
	context.SetGenericScissor(0, 0, 384, 216, false);
	context.SetViewportScissor(0, 0, 0, 384, 216, false);

	HW::ScanModeControl mode;
	mode.vport_scissor_enable = true;

	const auto scissor = State::ResolveScissor(context.GetScreenViewport(), mode, 0);
	EXPECT_EQ(scissor.left, 0);
	EXPECT_EQ(scissor.top, 0);
	EXPECT_EQ(scissor.right, 384);
	EXPECT_EQ(scissor.bottom, 216);
}

TEST(EmulatorGraphicsState, RequiresAnActiveDepthStencilOperationForTargetBinding)
{
	HW::DepthRenderTarget target;
	target.z_info.tile_surface_enable = true;

	HW::RenderControl render_control;
	HW::DepthControl  depth_control;
	depth_control.z_write_enable = true;

	auto usage = State::ResolveDepthStencilUsage(target, render_control, depth_control);
	EXPECT_FALSE(usage.target_active);
	EXPECT_FALSE(usage.depth_write_enable);

	depth_control.z_enable = true;
	usage                  = State::ResolveDepthStencilUsage(target, render_control, depth_control);
	EXPECT_TRUE(usage.target_active);
	EXPECT_TRUE(usage.depth_write_enable);

	depth_control.z_write_enable = false;
	usage                        = State::ResolveDepthStencilUsage(target, render_control, depth_control);
	EXPECT_TRUE(usage.target_active);
	EXPECT_FALSE(usage.depth_write_enable);

	depth_control                = {};
	depth_control.stencil_enable = true;
	usage                        = State::ResolveDepthStencilUsage(target, render_control, depth_control);
	EXPECT_TRUE(usage.target_active);
	EXPECT_FALSE(usage.depth_write_enable);

	depth_control                         = {};
	render_control.depth_compress_disable = true;
	usage                                 = State::ResolveDepthStencilUsage(target, render_control, depth_control);
	EXPECT_TRUE(usage.target_active);
	EXPECT_FALSE(usage.depth_write_enable);

	target.z_info.tile_surface_enable = false;
	usage                             = State::ResolveDepthStencilUsage(target, render_control, depth_control);
	EXPECT_FALSE(usage.target_active);
	EXPECT_FALSE(usage.depth_write_enable);
}

TEST(EmulatorGraphicsState, ResolvesViewportDepthForClipSpaceAndHostLimits)
{
	// Captured first MRT world pass: zscale=1, zoffset=0, dx_clip_space=false.
	auto ogl = State::ResolveViewportDepth(1.0f, 0.0f, false, true);
	EXPECT_FLOAT_EQ(ogl.min_depth, -1.0f);
	EXPECT_FLOAT_EQ(ogl.max_depth, 1.0f);

	// Intel Arc and many hosts lack VK_EXT_depth_range_unrestricted.
	ogl = State::ResolveViewportDepth(1.0f, 0.0f, false, false);
	EXPECT_FLOAT_EQ(ogl.min_depth, 0.0f);
	EXPECT_FLOAT_EQ(ogl.max_depth, 1.0f);

	auto dx = State::ResolveViewportDepth(0.5f, 0.5f, true, true);
	EXPECT_FLOAT_EQ(dx.min_depth, 0.5f);
	EXPECT_FLOAT_EQ(dx.max_depth, 1.0f);

	dx = State::ResolveViewportDepth(1.0f, 0.0f, true, false);
	EXPECT_FLOAT_EQ(dx.min_depth, 0.0f);
	EXPECT_FLOAT_EQ(dx.max_depth, 1.0f);
}

TEST(EmulatorGraphicsState, ResolvesViewportXyFromScaleAndOffset)
{
	const auto xy = State::ResolveViewportXy(640.0f, 640.0f, 360.0f, 360.0f);
	EXPECT_FLOAT_EQ(xy.x, 0.0f);
	EXPECT_FLOAT_EQ(xy.y, 0.0f);
	EXPECT_FLOAT_EQ(xy.width, 1280.0f);
	EXPECT_FLOAT_EQ(xy.height, 720.0f);

	const auto xy2 = State::ResolveViewportXy(100.0f, 250.0f, 50.0f, 150.0f);
	EXPECT_FLOAT_EQ(xy2.x, 150.0f);
	EXPECT_FLOAT_EQ(xy2.y, 100.0f);
	EXPECT_FLOAT_EQ(xy2.width, 200.0f);
	EXPECT_FLOAT_EQ(xy2.height, 100.0f);
}

TEST(EmulatorGraphicsState, SeparatesHtileMetaClearFromRegisterDepthClear)
{
	// Captured world path: register DEPTH_CLEAR_ENABLE=0, HTILE meta clear=1.
	auto htile_only = State::ResolveDepthClearActions(false, true);
	EXPECT_TRUE(htile_only.vulkan_clear);
	EXPECT_FALSE(htile_only.suppress_depth_write);

	auto register_only = State::ResolveDepthClearActions(true, false);
	EXPECT_TRUE(register_only.vulkan_clear);
	EXPECT_TRUE(register_only.suppress_depth_write);

	auto both = State::ResolveDepthClearActions(true, true);
	EXPECT_TRUE(both.vulkan_clear);
	EXPECT_TRUE(both.suppress_depth_write);

	auto neither = State::ResolveDepthClearActions(false, false);
	EXPECT_FALSE(neither.vulkan_clear);
	EXPECT_FALSE(neither.suppress_depth_write);
}

TEST(EmulatorGraphicsState, DepthAttachmentLoadOpsClearWhenGuestDepthClear)
{
	using namespace Kyty::Libs::Graphics;
	// HTILE/register clear → loadOp CLEAR + UNDEFINED.
	auto ds = ResolveDepthAttachmentLoadOps(VK_FORMAT_D32_SFLOAT_S8_UINT, true, false);
	EXPECT_EQ(ds.depth_load, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(ds.stencil_load, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	EXPECT_EQ(ds.initial_layout, VK_IMAGE_LAYOUT_UNDEFINED);

	auto both = ResolveDepthAttachmentLoadOps(VK_FORMAT_D32_SFLOAT_S8_UINT, true, true);
	EXPECT_EQ(both.depth_load, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(both.stencil_load, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(both.initial_layout, VK_IMAGE_LAYOUT_UNDEFINED);

	auto depth_only = ResolveDepthAttachmentLoadOps(VK_FORMAT_D32_SFLOAT, true, false);
	EXPECT_EQ(depth_only.depth_load, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(depth_only.stencil_load, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	EXPECT_EQ(depth_only.initial_layout, VK_IMAGE_LAYOUT_UNDEFINED);

	auto load = ResolveDepthAttachmentLoadOps(VK_FORMAT_D32_SFLOAT_S8_UINT, false, false);
	EXPECT_EQ(load.depth_load, VK_ATTACHMENT_LOAD_OP_LOAD);
	EXPECT_EQ(load.stencil_load, VK_ATTACHMENT_LOAD_OP_LOAD);
	EXPECT_EQ(load.initial_layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

TEST(EmulatorGraphicsState, ResolvesSharedVideoOutExportsForGen5Module)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libVideoOut_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"CBiu4mCE1DA";
	query.library              = U"VideoOut";
	query.library_version      = 1;
	query.module               = U"VideoOut";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	EXPECT_NE(symbols.Find(query), nullptr);
}

// Gen5 ACB/DCB sizing helpers and Trinity query contracts used by Astro boot.
TEST(EmulatorGraphicsState, Gen5AgcSizeHelpersAndTrinityMode)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	using namespace Kyty::Libs::Graphics::Gen5;
	EXPECT_EQ(GraphicsCbNopGetSize(4), 16u);
	EXPECT_EQ(GraphicsCbNopGetSize(1), 4u);
	EXPECT_EQ(GraphicsCbDispatchGetSize(), 20u);
	EXPECT_EQ(GraphicsCbSetShRegisterRangeDirectGetSize(3), 20u);
	EXPECT_EQ(GraphicsGetIsTrinityMode(), 0u);
	EXPECT_EQ(GraphicsDebugRaiseException(0x1234u), OK);

	uint32_t write_cmd[4] = {KYTY_PM4(4, Pm4::IT_WRITE_DATA, 0u), 0u, 0u, 0u};
	EXPECT_EQ(GraphicsWriteDataPatchSetAddressOrOffset(write_cmd, 0x1122334455667788ull), OK);
	EXPECT_EQ(write_cmd[2], 0x55667788u);
	EXPECT_EQ(write_cmd[3], 0x11223344u);
	uint32_t bad_cmd[2] = {KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO), 0u};
	EXPECT_NE(GraphicsWriteDataPatchSetAddressOrOffset(bad_cmd, 0), OK);
}

// Missing Gen5 AGC / AgcDriver exports that blocked Astro after Ampr/VideoOut.
TEST(EmulatorGraphicsState, ResolvesGen5AgcAndDriverExports)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libGraphicsDriver_1", &symbols));

	auto resolve = [&](const char16_t* nid, const char16_t* library, const char16_t* module) {
		Loader::SymbolResolve query {};
		query.name                 = nid;
		query.library              = library;
		query.library_version      = 1;
		query.module               = module;
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		return symbols.Find(query) != nullptr;
	};

	EXPECT_TRUE(resolve(u"BfBDZGbti7A", u"Agc", u"Agc"));
	EXPECT_TRUE(resolve(u"htn36gPnBk4", u"Agc", u"Agc"));
	EXPECT_TRUE(resolve(u"t7PlZ9nt5Lc", u"Agc", u"Agc"));
	EXPECT_TRUE(resolve(u"1rZSWUv1IRc", u"Agc", u"Agc"));
	EXPECT_TRUE(resolve(u"+kSrjIVxKFE", u"Agc", u"Agc"));
	EXPECT_TRUE(resolve(u"AhGvpITrf4M", u"Graphics5Driver", u"Graphics5Driver"));
	EXPECT_TRUE(resolve(u"gSRnr79F8tQ", u"Graphics5Driver", u"Graphics5Driver"));
	EXPECT_TRUE(resolve(u"w2rJhmD+dsE", u"Graphics5Driver", u"Graphics5Driver"));
	EXPECT_TRUE(resolve(u"XlNp7jzGiPo", u"Graphics5Driver", u"Graphics5Driver"));
	EXPECT_TRUE(resolve(u"MM4IZSEYytQ", u"Graphics5Driver", u"Graphics5Driver"));
	EXPECT_EQ(Gen5Driver::GraphicsDriverSetTFRing(reinterpret_cast<const volatile void*>(0x10000), 0x3fff8), OK);
	EXPECT_EQ(Gen5Driver::GraphicsDriverSetHsOffchipParam(0, 0x1ff, 0), OK);
}

// WaitFlipDone body observed post-Play: handle=0, index=3 while Open() left
// only slot 1 opened. Resolve handle 0 to that sole open port.
TEST(EmulatorGraphicsState, ResolvesVideoOutHandleZeroToSoleOpenPort)
{
	using Kyty::Libs::VideoOut::VideoOutResolveHandle;

	const bool only_one[] = {false, true};
	EXPECT_EQ(VideoOutResolveHandle(0, only_one, 2), 1);
	EXPECT_EQ(VideoOutResolveHandle(1, only_one, 2), 1);

	const bool none[] = {false, false};
	EXPECT_EQ(VideoOutResolveHandle(0, none, 2), 0);

	const bool both[] = {true, true};
	EXPECT_EQ(VideoOutResolveHandle(0, both, 2), 0); // ambiguous

	const bool only_zero[] = {true, false};
	EXPECT_EQ(VideoOutResolveHandle(0, only_zero, 2), 0);
}

TEST(EmulatorGraphicsState, ReportsNoSystemServiceEventWithoutFabricatingOne)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libSystemService_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"656LMQSrg6U";
	query.library              = U"SystemService";
	query.library_version      = 1;
	query.module               = U"SystemService";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* record = symbols.Find(query);
	ASSERT_NE(record, nullptr);

	using ReceiveEventFunc = int (*)(void*);
	auto    receive_event  = reinterpret_cast<ReceiveEventFunc>(record->vaddr);
	uint8_t event[64]      = {};
	for (auto& value: event)
	{
		value = 0xa5;
	}

	EXPECT_EQ(receive_event(nullptr), Libs::SystemService::SYSTEM_SERVICE_ERROR_PARAMETER);
	EXPECT_EQ(receive_event(event), Libs::SystemService::SYSTEM_SERVICE_ERROR_NO_EVENT);
	for (const auto value: event)
	{
		EXPECT_EQ(value, 0xa5);
	}
}

TEST(EmulatorGraphicsState, ClassifiesConstantStorageResourcesAsReadOnly)
{
	EXPECT_TRUE(ShaderStorageUsageIsReadOnly(ShaderStorageUsage::Constant));
	EXPECT_TRUE(ShaderStorageUsageIsReadOnly(ShaderStorageUsage::ReadOnly));
	EXPECT_FALSE(ShaderStorageUsageIsReadOnly(ShaderStorageUsage::ReadWrite));
	EXPECT_FALSE(ShaderStorageUsageIsReadOnly(ShaderStorageUsage::Unknown));
}

// Guest malloc/new → host heap; small V# bases land here (Dreaming Sarah).
TEST(EmulatorGraphicsState, ClassifiesHostGuestMallocRangesForLazyGpuHeaps)
{
	EXPECT_FALSE(GpuMemoryIsHostGuestMallocRange(0, 0x40));
	EXPECT_FALSE(GpuMemoryIsHostGuestMallocRange(0x1190000, 0x40));           // direct GPU map
	EXPECT_FALSE(GpuMemoryIsHostGuestMallocRange(0x900000000ull, 0x40));      // main image
	EXPECT_TRUE(GpuMemoryIsHostGuestMallocRange(0x7f005c0b3d20ull, 0x40));    // captured VB
	EXPECT_TRUE(GpuMemoryIsHostGuestMallocRange(0x7fffcc068720ull, 0x18));    // host index-ish
	EXPECT_FALSE(GpuMemoryIsHostGuestMallocRange(0x7f005c0b3d20ull, 0));
	// Overflow / past host band
	EXPECT_FALSE(GpuMemoryIsHostGuestMallocRange(0x7fffffffffffffull, 0x20));

	uint64_t start = 0;
	uint64_t size  = 0;
	GpuMemoryHostGuestMallocPageCover(0x7f005c0b3d20ull, 0x40, &start, &size);
	EXPECT_EQ(start, 0x7f005c0b3000ull);
	EXPECT_EQ(size, 0x1000ull);
	// Spans a page boundary
	GpuMemoryHostGuestMallocPageCover(0x7f005c0b3ff0ull, 0x20, &start, &size);
	EXPECT_EQ(start, 0x7f005c0b3000ull);
	EXPECT_EQ(size, 0x2000ull);
}

TEST(EmulatorGraphicsState, SharesOverlappingReadOnlyStorageViews)
{
	EXPECT_TRUE(GpuMemoryCanShareReadOnlyStorageViews(0x1000, 0x80, true, 0x1010, 0x70, true));
	EXPECT_TRUE(GpuMemoryCanShareReadOnlyStorageViews(0x1000, 0x80, true, 0x1010, 0x80, true));
	EXPECT_FALSE(GpuMemoryCanShareReadOnlyStorageViews(0x1000, 0x80, false, 0x1010, 0x70, true));
	EXPECT_FALSE(GpuMemoryCanShareReadOnlyStorageViews(0x1000, 0x80, true, 0x1010, 0x70, false));
	EXPECT_FALSE(GpuMemoryCanShareReadOnlyStorageViews(0x1000, 0x80, true, 0x1080, 0x20, true));
	EXPECT_FALSE(GpuMemoryCanShareReadOnlyStorageViews(0x1000, 0x80, true, 0x1000, 0x80, true));

	// Observed multi-parent RO StorageBuffer geometry: a 0x70 view Contained in
	// a 0x80 parent and Crossing a neighboring 0x70 parent. Both parents must
	// individually share so CreateObject can link them without inventing a
	// single-parent policy.
	constexpr uint64_t parent_contains_addr = 0x12267d9a0ull;
	constexpr uint64_t parent_contains_size = 0x80ull;
	constexpr uint64_t parent_crosses_addr  = 0x12267d9c0ull;
	constexpr uint64_t parent_crosses_size  = 0x70ull;
	constexpr uint64_t child_addr           = 0x12267d9b0ull;
	constexpr uint64_t child_size           = 0x70ull;
	EXPECT_TRUE(GpuMemoryCanShareReadOnlyStorageViews(parent_contains_addr, parent_contains_size, true, child_addr, child_size, true));
	EXPECT_TRUE(GpuMemoryCanShareReadOnlyStorageViews(parent_crosses_addr, parent_crosses_size, true, child_addr, child_size, true));
	EXPECT_FALSE(GpuMemoryCanShareReadOnlyStorageViews(parent_contains_addr, parent_contains_size, false, child_addr, child_size, true));
	EXPECT_FALSE(GpuMemoryCanShareReadOnlyStorageViews(parent_crosses_addr, parent_crosses_size, true, child_addr, child_size, false));
}

TEST(EmulatorGraphicsState, ReversesGpuMemoryOverlapRelations)
{
	EXPECT_EQ(GpuMemoryReverseOverlap(GpuMemoryOverlapType::Equals), GpuMemoryOverlapType::Equals);
	EXPECT_EQ(GpuMemoryReverseOverlap(GpuMemoryOverlapType::Crosses), GpuMemoryOverlapType::Crosses);
	EXPECT_EQ(GpuMemoryReverseOverlap(GpuMemoryOverlapType::Contains), GpuMemoryOverlapType::IsContainedWithin);
	EXPECT_EQ(GpuMemoryReverseOverlap(GpuMemoryOverlapType::IsContainedWithin), GpuMemoryOverlapType::Contains);
	EXPECT_EQ(GpuMemoryReverseOverlap(GpuMemoryOverlapType::None), GpuMemoryOverlapType::None);
}

// Non-exact FindRenderTexture: IsContainedWithin and Contains (same-base or
// offset-into-parent). PreferGpuMemoryAliasIndex picks among multi-matches;
// Size()>1 no longer EXIT. Crosses rejected (wrong-sized bind).
TEST(EmulatorGraphicsState, FindObjectsNonExactAcceptsContainedSampleInRenderTarget)
{
	EXPECT_TRUE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Equals, true));
	EXPECT_TRUE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Equals, false));
	EXPECT_FALSE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Contains, true));
	EXPECT_FALSE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::IsContainedWithin, true));
	EXPECT_TRUE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::IsContainedWithin, false));
	// Same-base Contains: sample and RT share start address (size mismatch only).
	EXPECT_TRUE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Contains, false, true));
	// Offset-into-parent Contains: bind the live parent RT instead of empty
	// tile-27 upload (opaque-black props). Cropped views are a follow-up.
	EXPECT_TRUE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Contains, false, false));
	EXPECT_TRUE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Contains, false));
	EXPECT_FALSE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Crosses, false));
	EXPECT_FALSE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::None, false));
}

TEST(EmulatorGraphicsState, PreferGpuMemoryAliasPicksTightestCover)
{
	const uint64_t sizes[] = {0x60000ull, 0x140000ull, 0xa0000ull};
	// Sample fits in 0xa0000 and 0x140000; prefer the smaller cover.
	EXPECT_EQ(PreferGpuMemoryAliasIndex(sizes, 3, 0xa0000ull), 2u);
	// Sample larger than every object: prefer the largest under-sample RT.
	EXPECT_EQ(PreferGpuMemoryAliasIndex(sizes, 3, 0x200000ull), 1u);
	// Comparable size proxy only: prefer the smallest object.
	EXPECT_EQ(PreferGpuMemoryAliasIndex(sizes, 3, 0ull), 0u);
	EXPECT_EQ(PreferGpuMemoryAliasIndex(sizes, 1, 0x100ull), 0u);
}

// Residual world false-color: ufmt-56 samples must not alias float lighting RTs.
// GraphicsRender rejects the RT alias entirely when every overlap is the wrong
// family for known ufmts 56/71 (falls through to guest/storage upload).
TEST(EmulatorGraphicsState, Gen5SampleFormatMatchesVulkanRejectsFloatForRgba8)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_TRUE(Gen5SampleFormatMatchesVulkan(56u, VK_FORMAT_R8G8B8A8_UNORM));
	EXPECT_TRUE(Gen5SampleFormatMatchesVulkan(56u, VK_FORMAT_R8G8B8A8_SRGB));
	EXPECT_TRUE(Gen5SampleFormatMatchesVulkan(56u, VK_FORMAT_B8G8R8A8_UNORM));
	EXPECT_FALSE(Gen5SampleFormatMatchesVulkan(56u, VK_FORMAT_R16G16B16A16_SFLOAT));
	EXPECT_TRUE(Gen5SampleFormatMatchesVulkan(71u, VK_FORMAT_R16G16B16A16_SFLOAT));
	EXPECT_FALSE(Gen5SampleFormatMatchesVulkan(71u, VK_FORMAT_R8G8B8A8_UNORM));
	// Unknown ufmt: do not invent a filter that drops every match.
	EXPECT_TRUE(Gen5SampleFormatMatchesVulkan(14u, VK_FORMAT_R8G8_UNORM));
	EXPECT_TRUE(Gen5SampleFormatMatchesVulkan(14u, VK_FORMAT_R16G16B16A16_SFLOAT));

	// Simulated multi-match policy used by PrepareTextures: when every RT is
	// float and the sample is ufmt 56, zero compatible aliases → reject path.
	const VkFormat only_float[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
	size_t         compatible   = 0;
	for (VkFormat f: only_float)
	{
		if (Gen5SampleFormatMatchesVulkan(56u, f))
		{
			compatible++;
		}
	}
	EXPECT_EQ(compatible, 0u);
	const VkFormat mixed[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM};
	compatible             = 0;
	for (VkFormat f: mixed)
	{
		if (Gen5SampleFormatMatchesVulkan(56u, f))
		{
			compatible++;
		}
	}
	EXPECT_EQ(compatible, 1u);
}

// Sample guest upload must never flush StorageBuffers first (linear SSBO ≠ texture).
TEST(EmulatorGraphicsState, Gen5SampleGuestUploadNeverWriteBackStorage)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_FALSE(Gen5SampleMayWriteBackStorageBeforeGuestUpload());
}

// Tiled guest upload under a live color surface is forbidden (GPU-owned guest).
// Tile 27 + RGBA8 (ufmt 56) is kRenderTarget layout: never detile from guest
// (period-16 bands). BC1 (133) package data may detile when uncovered.
TEST(EmulatorGraphicsState, Gen5SampleMayGuestUploadTiledRejectsCoveredTile27)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, true));
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(9u, true));
	// Overload defaults ufmt=56: tile 27 RGBA8 must not detile even when uncovered.
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, false));
	EXPECT_TRUE(Gen5SampleMayGuestUploadTiled(9u, false));
	EXPECT_TRUE(Gen5SampleMayGuestUploadTiled(0u, true));
	EXPECT_TRUE(Gen5SampleMayGuestUploadTiled(0u, false));
}

TEST(EmulatorGraphicsState, Gen5SampleMayGuestUploadTiledTile27ByFormat)
{
	using namespace Kyty::Libs::Graphics;
	// RT-class samples: never guest-detile kRenderTarget layout.
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, 56u, false));
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, 71u, false));
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, 56u, true));
	// BC1 package textures may detile when no live surface covers the range.
	EXPECT_TRUE(Gen5SampleMayGuestUploadTiled(27u, 133u, false));
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, 133u, true));
	// Tile 9 package RGBA8 when uncovered only.
	EXPECT_TRUE(Gen5SampleMayGuestUploadTiled(9u, 56u, false));
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(9u, 56u, true));
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(9u, 133u, false));
}

// Hash refresh stays enabled so late package loads still upload; SSBO clobber
// is handled by skipping guest re-detile when a live RT/ST parent is linked.
TEST(EmulatorGraphicsState, Gen5SampleTextureHashRefreshStaysEnabled)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_TRUE(Gen5SampleTextureUsesHashRefresh(56u));
	EXPECT_TRUE(Gen5SampleTextureUsesHashRefresh(71u));
	EXPECT_TRUE(Gen5SampleTextureUsesHashRefresh(0u));
}

// CreateFromObjects may copy a live surface parent only when ufmt families match.
// Float lighting under an RGBA8 sample must not be blitted into the Texture.
TEST(EmulatorGraphicsState, Gen5SampleMayCopyFromSurfaceParentMatchesFormatFamily)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_TRUE(Gen5SampleMayCopyFromSurfaceParent(56u, VK_FORMAT_R8G8B8A8_UNORM));
	EXPECT_TRUE(Gen5SampleMayCopyFromSurfaceParent(56u, VK_FORMAT_R8G8B8A8_SRGB));
	EXPECT_FALSE(Gen5SampleMayCopyFromSurfaceParent(56u, VK_FORMAT_R16G16B16A16_SFLOAT));
	EXPECT_TRUE(Gen5SampleMayCopyFromSurfaceParent(71u, VK_FORMAT_R16G16B16A16_SFLOAT));
	EXPECT_FALSE(Gen5SampleMayCopyFromSurfaceParent(71u, VK_FORMAT_R8G8B8A8_UNORM));
	// Unknown sample ufmt: do not block all parents.
	EXPECT_TRUE(Gen5SampleMayCopyFromSurfaceParent(14u, VK_FORMAT_R8G8_UNORM));
	EXPECT_TRUE(Gen5SampleMayCopyFromSurfaceParent(14u, VK_FORMAT_R16G16B16A16_SFLOAT));
}

// Bind only the exact sample extent. A larger parent without a crop view leaves
// banded or false-color tiles, so it must not be used as a fallback.
TEST(EmulatorGraphicsState, Gen5PickSampleSurfaceAliasesPrefersExactExtent)
{
	using namespace Kyty::Libs::Graphics;
	const VkFormat formats[]   = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM,
	                              VK_FORMAT_R8G8B8A8_UNORM};
	const uint32_t extents_w[] = {1920u, 1920u, 128u};
	const uint32_t extents_h[] = {1080u, 1080u, 128u};
	int            indices[16] = {};
	size_t         count       = 0;
	bool           reject      = false;

	// Sample 128x128 ufmt 56: skip float parent; only exact 128x128.
	ASSERT_TRUE(Gen5PickSampleSurfaceAliases(56u, 128u, 128u, 3, formats, extents_w, extents_h, indices,
	                                         &count, &reject));
	EXPECT_FALSE(reject);
	ASSERT_EQ(count, 1u);
	EXPECT_EQ(indices[0], 2);

	// Only float candidates for ufmt 56 → reject.
	ASSERT_TRUE(Gen5PickSampleSurfaceAliases(56u, 1920u, 1080u, 1, formats, extents_w, extents_h, indices,
	                                         &count, &reject));
	EXPECT_TRUE(reject);
	EXPECT_EQ(count, 0u);

	// Format match without exact extent → reject (no full-screen parent bind).
	const VkFormat only_large[] = {VK_FORMAT_R8G8B8A8_UNORM};
	const uint32_t large_w[]    = {1920u};
	const uint32_t large_h[]    = {1080u};
	ASSERT_TRUE(Gen5PickSampleSurfaceAliases(56u, 128u, 128u, 1, only_large, large_w, large_h, indices,
	                                         &count, &reject));
	EXPECT_TRUE(reject);
	EXPECT_EQ(count, 0u);
}

// Sample pixel area vs RT extent areas (GraphicsRender sample-bind path).
// A large sample covering a tiny child RT and a full-size parent must bind the
// parent (tightest cover >= sample), not the child (sample_size==0 bug).
TEST(EmulatorGraphicsState, PreferGpuMemoryAliasUsesSampleAreaAgainstRtExtents)
{
	const uint64_t rt_areas[] = {64ull * 64ull, 1920ull * 1080ull, 128ull * 128ull};
	const uint64_t sample_area = 1920ull * 1080ull;
	EXPECT_EQ(PreferGpuMemoryAliasIndex(rt_areas, 3, sample_area), 1u);
	// Smaller sample: prefer the 128x128 cover over 1920x1080.
	EXPECT_EQ(PreferGpuMemoryAliasIndex(rt_areas, 3, 128ull * 128ull), 2u);
}

// Guest allocation bytes (GraphicsRender when every RT recorded guest_size).
// Same-extent children can still differ in tiled guest size; Prefer must use
// those bytes against size.size, not invent an area proxy.
TEST(EmulatorGraphicsState, PreferGpuMemoryAliasUsesGuestByteSizes)
{
	// Child IsContainedWithin: 0x10000; parent cover: 0x800000; sibling: 0x20000.
	const uint64_t guest_sizes[] = {0x10000ull, 0x800000ull, 0x20000ull};
	const uint64_t sample_bytes  = 0x7f0000ull;
	EXPECT_EQ(PreferGpuMemoryAliasIndex(guest_sizes, 3, sample_bytes), 1u);
	// Sample fits only under-sample objects: pick the largest child.
	EXPECT_EQ(PreferGpuMemoryAliasIndex(guest_sizes, 3, 0x900000ull), 1u);
	EXPECT_EQ(PreferGpuMemoryAliasIndex(guest_sizes, 3, 0x18000ull), 2u);
}

// Capture/disk bounds: unset env defaults to 1280 so 4K VideoOut dumps are not
// multi-dozen-MB BMPs; explicit 0 keeps full resolution; prune math is pure.
TEST(EmulatorGraphicsState, NativeCaptureDefaultsAndPruneBoundDisk)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_EQ(NativeCaptureResolveMaxEdge(nullptr), 1280u);
	EXPECT_EQ(NativeCaptureResolveMaxEdge(""), 1280u);
	EXPECT_EQ(NativeCaptureResolveMaxEdge("0"), 0u);
	EXPECT_EQ(NativeCaptureResolveMaxEdge("1920"), 1920u);
	EXPECT_EQ(NativeCaptureResolveMaxEdge("nope"), 1280u);

	EXPECT_EQ(NativeCapturePruneCount(3, 8), 0u);
	EXPECT_EQ(NativeCapturePruneCount(8, 8), 0u);
	EXPECT_EQ(NativeCapturePruneCount(12, 8), 4u);
	EXPECT_EQ(NativeCapturePruneCount(5, 0), 0u);
}

// Pipeline cache must recycle slots instead of EXIT when variants exceed the cap.
TEST(EmulatorGraphicsState, PipelineCacheNextEvictIndexRotates)
{
	using namespace Kyty::Libs::Graphics;
	uint32_t cursor = 0;
	EXPECT_EQ(PipelineCacheNextEvictIndex(4, &cursor), 0u);
	EXPECT_EQ(PipelineCacheNextEvictIndex(4, &cursor), 1u);
	EXPECT_EQ(PipelineCacheNextEvictIndex(4, &cursor), 2u);
	EXPECT_EQ(PipelineCacheNextEvictIndex(4, &cursor), 3u);
	EXPECT_EQ(PipelineCacheNextEvictIndex(4, &cursor), 0u);
	EXPECT_EQ(PipelineCacheNextEvictIndex(0, &cursor), 0u);
	EXPECT_EQ(PipelineCacheNextEvictIndex(4, nullptr), 0u);
}

TEST(EmulatorGraphicsState, AllowsOnlyObservedTextureStorageAliases)
{
	EXPECT_TRUE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Equals,
	                                               GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                               GpuMemoryObjectType::StorageBuffer));
	// Captured post-menu path: StorageBuffer registration that Crosses an
	// existing Texture (partial guest range) must link, not EXIT.
	EXPECT_TRUE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Crosses,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_FALSE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::IsContainedWithin,
	                                                GpuMemoryObjectType::StorageBuffer));
	EXPECT_FALSE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                                GpuMemoryObjectType::RenderTexture));
	EXPECT_FALSE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Equals,
	                                                GpuMemoryObjectType::StorageBuffer));
}

// Multi-parent StorageBuffer: each parent must independently pass texture-alias
// or vertex-share. Mixed Texture Contains + Vertex Contains is the observed
// create_all_the_same failure class under post-menu load.
TEST(EmulatorGraphicsState, AllowsMixedTextureVertexStorageParents)
{
	EXPECT_TRUE(GpuMemoryAllowsVertexStorageShare(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Contains,
	                                              GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexStorageShare(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Crosses,
	                                              GpuMemoryObjectType::StorageBuffer));
	EXPECT_FALSE(GpuMemoryAllowsVertexStorageShare(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                               GpuMemoryObjectType::StorageBuffer));
	// Both sides of a mixed multi-parent set must be acceptable.
	EXPECT_TRUE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexStorageShare(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Contains,
	                                              GpuMemoryObjectType::StorageBuffer));
}

// Captured dual-strict post-RT layout fix: Texture Contains IndexBuffer 0xe4.
TEST(EmulatorGraphicsState, AllowsIndexContainedInTextureSurface)
{
	EXPECT_TRUE(GpuMemoryAllowsIndexContainedInSurface(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                                   GpuMemoryObjectType::IndexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsIndexContainedInSurface(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Crosses,
	                                                   GpuMemoryObjectType::IndexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsIndexContainedInSurface(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::IsContainedWithin,
	                                                   GpuMemoryObjectType::IndexBuffer));
	EXPECT_FALSE(GpuMemoryAllowsIndexContainedInSurface(GpuMemoryObjectType::IndexBuffer, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::IndexBuffer));
	EXPECT_FALSE(GpuMemoryAllowsIndexContainedInSurface(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::VertexBuffer));
}

TEST(EmulatorGraphicsState, ReusesOnlySameBaseCoveringIndexBacking)
{
	constexpr uint64_t base = 0x100000;

	EXPECT_TRUE(GpuMemoryCanReuseIndexBacking(base, 0x100, base, 0xe4));
	EXPECT_TRUE(GpuMemoryCanReuseIndexBacking(base, 0x100, base, 0x100));
	EXPECT_FALSE(GpuMemoryCanReuseIndexBacking(base, 0xe4, base, 0x100));
	EXPECT_FALSE(GpuMemoryCanReuseIndexBacking(base, 0x100, base + 4, 0xfc));
	EXPECT_FALSE(GpuMemoryCanReuseIndexBacking(base, 0, base, 0));
}

// Captured: VertexBuffer Contained by co-located StorageBuffer + RenderTexture.
// Multi-parent load also uses surface IsContainedWithin/Crosses + peer VB reclaim.
TEST(EmulatorGraphicsState, AllowsVertexContainedInStorageAndRenderTarget)
{
	EXPECT_TRUE(GpuMemoryAllowsVertexContainedInSurface(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Contains,
	                                                   GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexContainedInSurface(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Contains,
	                                                   GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexContainedInSurface(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                                   GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexContainedInSurface(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Crosses,
	                                                   GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexContainedInSurface(GpuMemoryObjectType::DepthStencilBuffer, GpuMemoryOverlapType::Crosses,
	                                                   GpuMemoryObjectType::VertexBuffer));
	EXPECT_FALSE(GpuMemoryAllowsVertexContainedInSurface(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexReclaimVertex(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Crosses,
	                                               GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexReclaimVertex(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                               GpuMemoryObjectType::VertexBuffer));
	EXPECT_FALSE(GpuMemoryAllowsVertexReclaimVertex(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Contains,
	                                                GpuMemoryObjectType::VertexBuffer));
	// Captured: Texture Contains + IndexBuffer Crosses → link IB with new VB.
	EXPECT_TRUE(GpuMemoryAllowsVertexLinkIndexBuffer(GpuMemoryObjectType::IndexBuffer, GpuMemoryOverlapType::Crosses,
	                                                 GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexLinkIndexBuffer(GpuMemoryObjectType::IndexBuffer, GpuMemoryOverlapType::Contains,
	                                                 GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexLinkIndexBuffer(GpuMemoryObjectType::IndexBuffer, GpuMemoryOverlapType::Equals,
	                                                 GpuMemoryObjectType::VertexBuffer));
	EXPECT_FALSE(GpuMemoryAllowsVertexLinkIndexBuffer(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Crosses,
	                                                  GpuMemoryObjectType::VertexBuffer));
	// Captured: IndexBuffer IsContainedWithin + VertexBuffer Contains → reclaim
	// peer IB, link VB with the new IndexBuffer.
	EXPECT_TRUE(GpuMemoryAllowsIndexReclaimIndex(GpuMemoryObjectType::IndexBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                             GpuMemoryObjectType::IndexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsIndexLinkVertexBuffer(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Contains,
	                                                 GpuMemoryObjectType::IndexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsIndexLinkVertexBuffer(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Equals,
	                                                 GpuMemoryObjectType::IndexBuffer));
	EXPECT_FALSE(GpuMemoryAllowsIndexLinkVertexBuffer(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                                  GpuMemoryObjectType::IndexBuffer));
}

// Captured post-Param5: RenderTexture with SB Equals + SB/RT Contains parents.
TEST(EmulatorGraphicsState, AllowsRenderTargetMultiSurfaceParents)
{
	EXPECT_TRUE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Equals,
	                                                    GpuMemoryObjectType::RenderTexture));
	EXPECT_TRUE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::RenderTexture));
	EXPECT_TRUE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::RenderTexture));
	EXPECT_TRUE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Crosses,
	                                                    GpuMemoryObjectType::RenderTexture));
	EXPECT_TRUE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::RenderTexture));
	EXPECT_TRUE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::DepthStencilBuffer, GpuMemoryOverlapType::Crosses,
	                                                    GpuMemoryObjectType::RenderTexture));
	EXPECT_FALSE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::DepthStencilBuffer, GpuMemoryOverlapType::Equals,
	                                                     GpuMemoryObjectType::RenderTexture));
	EXPECT_FALSE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::DepthStencilBuffer, GpuMemoryOverlapType::Contains,
	                                                     GpuMemoryObjectType::RenderTexture));
	EXPECT_FALSE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Equals,
	                                                     GpuMemoryObjectType::Texture));
}

TEST(EmulatorGraphicsState, ReclaimsSingleSurfaceForDepthStencilReuse)
{
	EXPECT_TRUE(GpuMemoryAllowsDepthStencilReclaimSurface(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Crosses,
	                                                      GpuMemoryObjectType::DepthStencilBuffer));
	EXPECT_TRUE(GpuMemoryAllowsDepthStencilReclaimSurface(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Equals,
	                                                      GpuMemoryObjectType::DepthStencilBuffer));
	EXPECT_FALSE(GpuMemoryAllowsDepthStencilReclaimSurface(GpuMemoryObjectType::DepthStencilBuffer, GpuMemoryOverlapType::Crosses,
	                                                       GpuMemoryObjectType::DepthStencilBuffer));
	EXPECT_FALSE(GpuMemoryAllowsDepthStencilReclaimSurface(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Crosses,
	                                                       GpuMemoryObjectType::Texture));
}

// Captured: StorageBuffer with RT+SB Crosses and IsContainedWithin parents.
// Also dual-strict after VOP1 SDWA: SB 0x8000 Crosses DepthStencilBuffer.
TEST(EmulatorGraphicsState, AllowsStorageSurfaceShareWithContainedWithin)
{
	EXPECT_TRUE(GpuMemoryAllowsStorageSurfaceShare(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Crosses,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryAllowsStorageSurfaceShare(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryAllowsStorageSurfaceShare(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::IsContainedWithin,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryAllowsStorageSurfaceShare(GpuMemoryObjectType::DepthStencilBuffer, GpuMemoryOverlapType::Crosses,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_FALSE(GpuMemoryAllowsStorageSurfaceShare(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Crosses,
	                                                GpuMemoryObjectType::StorageBuffer));
}

// Captured: Texture over 5 VBs (reclaim) + StorageBuffer/RenderTexture Contains (link).
// Also VB Contains texture (link larger VB) mixed with SB/RT Contains.
TEST(EmulatorGraphicsState, AllowsTextureMixedReclaimAndSurfaceParents)
{
	EXPECT_TRUE(GpuMemoryAllowsTextureReclaimVertex(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Crosses,
	                                               GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureReclaimVertex(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                               GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureLinkVertex(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Contains,
	                                            GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                                    GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Crosses,
	                                                    GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType::VideoOutBuffer, GpuMemoryOverlapType::Equals,
	                                                    GpuMemoryObjectType::Texture));
	EXPECT_FALSE(GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType::VideoOutBuffer, GpuMemoryOverlapType::Contains,
	                                                     GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureLinkDepthMetadata(GpuMemoryObjectType::DepthStencilBuffer, GpuMemoryOverlapType::Crosses,
	                                                   GpuMemoryObjectType::Texture));
	EXPECT_FALSE(GpuMemoryAllowsTextureLinkDepthMetadata(GpuMemoryObjectType::DepthStencilBuffer,
	                                                    GpuMemoryOverlapType::Contains, GpuMemoryObjectType::Texture));
	EXPECT_FALSE(GpuMemoryAllowsTextureLinkDepthMetadata(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Crosses,
	                                                    GpuMemoryObjectType::Texture));
	EXPECT_FALSE(GpuMemoryAllowsTextureReclaimVertex(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Contains,
	                                                GpuMemoryObjectType::Texture));
	EXPECT_FALSE(GpuMemoryAllowsTextureLinkVertex(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Crosses,
	                                             GpuMemoryObjectType::Texture));
}

// Captured multi-parent SB write-back always links GPU-owned tiled RTs
// (PARAM_TILED=1, PARAM_WRITE_BACK=0). Those parents must skip hash invalidate.
TEST(EmulatorGraphicsState, SkipWriteBackInvalidateForGpuOwnedRenderTexture)
{
	uint64_t gpu_owned[8] = {};
	gpu_owned[3]          = 1; // PARAM_TILED
	gpu_owned[6]          = 0; // PARAM_WRITE_BACK
	EXPECT_TRUE(GpuMemoryIsGpuOwnedRenderTextureParams(gpu_owned));
	EXPECT_TRUE(GpuMemorySkipWriteBackParentInvalidate(GpuMemoryObjectType::RenderTexture, gpu_owned));

	uint64_t cpu_wb[8] = {};
	cpu_wb[3]          = 1;
	cpu_wb[6]          = 1;
	EXPECT_FALSE(GpuMemoryIsGpuOwnedRenderTextureParams(cpu_wb));
	EXPECT_FALSE(GpuMemorySkipWriteBackParentInvalidate(GpuMemoryObjectType::RenderTexture, cpu_wb));

	EXPECT_FALSE(GpuMemorySkipWriteBackParentInvalidate(GpuMemoryObjectType::Texture, gpu_owned));
	EXPECT_FALSE(GpuMemorySkipWriteBackParentInvalidate(GpuMemoryObjectType::VertexBuffer, gpu_owned));
}

TEST(EmulatorGraphicsState, TiledGpuOwnedRenderTextureUpdatePreservesTrackedLayout)
{
	GraphicContext                ctx {};
	RenderTextureVulkanImage      image;
	const RenderTextureObject     render_texture(RenderTextureFormat::R8G8B8A8Unorm, 642, 362, true, false, 642, false);
	const uint64_t                vaddr = 0x0000005100000000ull;
	const uint64_t                size  = 642u * 362u * 4u;

	image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	render_texture.GetUpdateFunc()(&ctx, render_texture.params, &image, &vaddr, &size, 1);

	EXPECT_EQ(image.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

TEST(EmulatorGraphicsState, UsesTrackedLayoutWhenUploadingOverExistingImage)
{
	VulkanImage image(VulkanImageType::Texture);
	image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	EXPECT_EQ(UtilGetImageUploadSourceLayout(&image), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Captured Gen5 WriteBack topology: RW StorageBuffer with 8 VertexBuffer
// Crosses/IsContainedWithin parents plus one Equals RenderTexture peer.
// Policy must accept the set, recompute via Equals (not self), and invalidate
// the partial-overlap VertexBuffers — without inventing None/Max relations.
TEST(EmulatorGraphicsState, WriteBackClassifiesMultiParentStorageTopology)
{
	EXPECT_EQ(GpuMemoryWriteBackParentActionFor(GpuMemoryOverlapType::Equals), GpuMemoryWriteBackParentAction::PropagateEquals);
	EXPECT_EQ(GpuMemoryWriteBackParentActionFor(GpuMemoryOverlapType::Crosses), GpuMemoryWriteBackParentAction::InvalidateOnly);
	EXPECT_EQ(GpuMemoryWriteBackParentActionFor(GpuMemoryOverlapType::IsContainedWithin), GpuMemoryWriteBackParentAction::InvalidateOnly);
	EXPECT_EQ(GpuMemoryWriteBackParentActionFor(GpuMemoryOverlapType::None), GpuMemoryWriteBackParentAction::Unsupported);

	// Empty: recompute self only.
	bool     recompute = false;
	uint32_t equals    = 99;
	uint32_t inv       = 99;
	ASSERT_TRUE(GpuMemoryWriteBackClassifyParents(nullptr, 0, &recompute, &equals, &inv));
	EXPECT_TRUE(recompute);
	EXPECT_EQ(equals, 0u);
	EXPECT_EQ(inv, 0u);

	// Observed multi-parent list (order matches CreateObject link dump).
	const GpuMemoryOverlapType observed[] = {
	    GpuMemoryOverlapType::Crosses,           // VB
	    GpuMemoryOverlapType::IsContainedWithin, // VB
	    GpuMemoryOverlapType::IsContainedWithin, // VB
	    GpuMemoryOverlapType::IsContainedWithin, // VB
	    GpuMemoryOverlapType::IsContainedWithin, // VB
	    GpuMemoryOverlapType::IsContainedWithin, // VB
	    GpuMemoryOverlapType::IsContainedWithin, // VB
	    GpuMemoryOverlapType::Crosses,           // VB
	    GpuMemoryOverlapType::Equals,            // RenderTexture peer
	};
	ASSERT_TRUE(GpuMemoryWriteBackClassifyParents(observed, static_cast<uint32_t>(sizeof(observed) / sizeof(observed[0])), &recompute,
	                                              &equals, &inv));
	EXPECT_FALSE(recompute);
	EXPECT_EQ(equals, 1u);
	EXPECT_EQ(inv, 8u);

	// WriteBackObjectLocked must classify arbitrary parent counts (stack
	// limit of 64 was observed too small after logo video dispose).
	GpuMemoryOverlapType wide[96];
	for (auto& rel: wide)
	{
		rel = GpuMemoryOverlapType::Crosses;
	}
	wide[95] = GpuMemoryOverlapType::Equals;
	ASSERT_TRUE(GpuMemoryWriteBackClassifyParents(wide, 96, &recompute, &equals, &inv));
	EXPECT_FALSE(recompute);
	EXPECT_EQ(equals, 1u);
	EXPECT_EQ(inv, 95u);

	// Only partial overlaps: still recompute self after GPU->CPU write-back.
	const GpuMemoryOverlapType only_vb[] = {GpuMemoryOverlapType::Crosses, GpuMemoryOverlapType::IsContainedWithin};
	ASSERT_TRUE(GpuMemoryWriteBackClassifyParents(only_vb, 2, &recompute, &equals, &inv));
	EXPECT_TRUE(recompute);
	EXPECT_EQ(equals, 0u);
	EXPECT_EQ(inv, 2u);

	// Unsupported relation must fail closed.
	const GpuMemoryOverlapType bad[] = {GpuMemoryOverlapType::Equals, GpuMemoryOverlapType::None};
	EXPECT_FALSE(GpuMemoryWriteBackClassifyParents(bad, 2, &recompute, &equals, &inv));
}


// CB_TARGET_MASK 0x0000ffff (RT0..RT3 full) must still yield RT0 nibble 0xF
// for the single-attachment pipeline write mask.
TEST(EmulatorGraphicsState, TargetMaskRt0NibbleFromMultiTarget)
{
	constexpr uint32_t multi = 0x0000ffffu;
	EXPECT_EQ(multi & 0xFu, 0xFu);
	EXPECT_EQ(0x00000000u & 0xFu, 0x0u);
	EXPECT_EQ(0x0000000Fu & 0xFu, 0xFu);
	// Partial RT0 channels
	EXPECT_EQ(0x00000005u & 0xFu, 0x5u);
}


// Gen5 depth size: table path for 1280x720 D32S8, formula path for 642x362
// (captured DEPTH_SIZE_FAIL).
TEST(EmulatorGraphicsState, TileGetDepthSizeNextGenNonTable)
{
	using namespace Kyty::Libs::Graphics;
	TileSizeAlign stencil {}, htile {}, depth {};
	ASSERT_TRUE(TileGetDepthSize(1280, 720, 0, 3, 1, true, true, true, &stencil, &htile, &depth));
	EXPECT_EQ(depth.size, 3932160u);
	EXPECT_EQ(stencil.size, 983040u);
	// Captured non-table surface.
	ASSERT_TRUE(TileGetDepthSize(642, 362, 0, 3, 1, true, true, true, &stencil, &htile, &depth));
	EXPECT_GT(depth.size, 0u);
	EXPECT_EQ(depth.size % 4u, 0u);
	EXPECT_EQ(depth.align, 65536u);
	EXPECT_GT(stencil.size, 0u);
	EXPECT_EQ(stencil.align, 65536u);
	EXPECT_GT(htile.size, 0u);
}


// Captured DepthStencilBuffer create (3 vaddrs) Crossing Texture + StorageBuffer.
TEST(EmulatorGraphicsState, DepthStencilReclaimParentsAreSurfaces)
{
	// Document the accepted parent types for multi_depth_stencil_reclaim.
	const GpuMemoryObjectType ok[] = {GpuMemoryObjectType::Texture, GpuMemoryObjectType::StorageBuffer,
	                                  GpuMemoryObjectType::RenderTexture, GpuMemoryObjectType::VertexBuffer};
	for (auto t : ok)
	{
		EXPECT_NE(t, GpuMemoryObjectType::DepthStencilBuffer);
	}
	EXPECT_EQ(GpuMemoryOverlapType::Crosses, GpuMemoryOverlapType::Crosses);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsClearOnUndefinedFirstUse)
{
	using namespace Kyty::Libs::Graphics;
	// Mesa/RADV R8G8B8A8: CLEAR_WORD0/1=0 packs to transparent black (A=0).
	// Inventing opaque A=1 made sprite-layer intermediates composite as black quads.
	auto ops = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, false, 0u, 0u, VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(ops.initial_layout, VK_IMAGE_LAYOUT_UNDEFINED);
	EXPECT_FLOAT_EQ(ops.clear_r, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_g, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_b, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_a, 0.0f);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsFloatZeroWordsClearToZeroAlpha)
{
	using namespace Kyty::Libs::Graphics;
	// Captured lighting RT: fmt=12/ctype=7, CLEAR_WORD0/1=0, blend SRC_ALPHA,ONE.
	auto ops = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, false, 0u, 0u, VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_FLOAT_EQ(ops.clear_r, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_g, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_b, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_a, 0.0f);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsLoadOnOptimalSubsequentPass)
{
	using namespace Kyty::Libs::Graphics;
	auto ops = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false, 0u, 0u, VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_LOAD);
	EXPECT_EQ(ops.initial_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsClearWhenRebindingAfterSample)
{
	using namespace Kyty::Libs::Graphics;
	// After composite samples the lighting RT, layout is SHADER_READ_ONLY. The next
	// frame's first additive light pass must CLEAR (zeros), not LOAD prior-frame HDR.
	auto ops =
	    ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false, 0u, 0u, VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(ops.initial_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	EXPECT_FLOAT_EQ(ops.clear_r, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_g, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_b, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_a, 0.0f);

	// Within-frame accumulation (still COLOR_ATTACHMENT) must keep LOAD.
	auto load_ops =
	    ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false, 0u, 0u, VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(load_ops.load_op, VK_ATTACHMENT_LOAD_OP_LOAD);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsRgba8RebindClearsTransparent)
{
	using namespace Kyty::Libs::Graphics;
	// Captured GBuffer/sprite RTs: fmt=10, CLEAR_WORD=0, fast=0, sampled then rebound.
	auto ops = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false, 0u, 0u, VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(ops.initial_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	EXPECT_FLOAT_EQ(ops.clear_r, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_g, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_b, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_a, 0.0f);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsClearUsesRgba8GuestClearWords)
{
	using namespace Kyty::Libs::Graphics;
	// Mesa/RADV: WORD0 = R|(G<<8)|(B<<16)|(A<<24), WORD1 = 0 for R8G8B8A8.
	const uint32_t word0 = 0xff804020u; // R=0x20 G=0x40 B=0x80 A=0xff
	const uint32_t word1 = 0u;
	auto           ops   = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, true, word0, word1, VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_NEAR(ops.clear_r, 32.0f / 255.0f, 1e-5);
	EXPECT_NEAR(ops.clear_g, 64.0f / 255.0f, 1e-5);
	EXPECT_NEAR(ops.clear_b, 128.0f / 255.0f, 1e-5);
	EXPECT_NEAR(ops.clear_a, 1.0f, 1e-5);
}

TEST(EmulatorGraphicsState, ColorAttachmentClearValuesDoNotChangeRenderPassCompatibility)
{
	using namespace Kyty::Libs::Graphics;
	// VkClearValue is supplied at BeginRenderPass. Two clears on the same image
	// therefore require the same render-pass compatibility while preserving their
	// distinct per-pass colors; keying framebuffer/pipeline caches by these values
	// recompiles Metal pipelines every frame.
	auto first = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, true, 0xff0000ffu, 0u, VK_FORMAT_R8G8B8A8_UNORM);
	auto next  = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, true, 0xffff0000u, 0u, VK_FORMAT_R8G8B8A8_UNORM);

	EXPECT_EQ(first.load_op, next.load_op);
	EXPECT_EQ(first.initial_layout, next.initial_layout);
	EXPECT_NE(first.clear_r, next.clear_r);
	EXPECT_NE(first.clear_b, next.clear_b);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsClearUsesRgba16FloatGuestClearWords)
{
	using namespace Kyty::Libs::Graphics;
	// Mesa/RADV: WORD0 = f16(R)|(f16(G)<<16), WORD1 = f16(B)|(f16(A)<<16).
	// f16(1.0)=0x3c00, f16(0.5)=0x3800, f16(0.0)=0x0000, f16(1.0)=0x3c00.
	const uint32_t word0 = 0x38003c00u;
	const uint32_t word1 = 0x3c000000u;
	auto           ops   = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, true, word0, word1, VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_FLOAT_EQ(ops.clear_r, 1.0f);
	EXPECT_FLOAT_EQ(ops.clear_g, 0.5f);
	EXPECT_FLOAT_EQ(ops.clear_b, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_a, 1.0f);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsRejectsInventedFloat32RgClear)
{
	using namespace Kyty::Libs::Graphics;
	// Legacy invented packing bitcast float32 R/G and forced B=0 A=1. Those
	// bit patterns as f16 pairs must NOT decode to (1.0, 0.5, 0, 1).
	const uint32_t word0 = 0x3f800000u; // float32 1.0
	const uint32_t word1 = 0x3f000000u; // float32 0.5
	auto           ops   = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, true, word0, word1, VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_FALSE(ops.clear_r == 1.0f && ops.clear_g == 0.5f && ops.clear_b == 0.0f && ops.clear_a == 1.0f);
}

TEST(EmulatorGraphicsState, WaitRegMemCompareMasksBothSides)
{
	// GraphicsDcbWaitRegMem size=0 zeroes the high half of the 64-bit mask.
	// Comparing (*addr & mask) == ref (unmasked) never matches when ref keeps
	// high bits; both sides must apply mask (WaitRegMem32/64 contract).
	const uint64_t val  = 0x1u;
	const uint64_t ref  = 0x0000000100000001ull;
	const uint64_t mask = 0x00000000ffffffffull;
	EXPECT_EQ((val & mask), (ref & mask));
	EXPECT_NE((val & mask), ref);
}

TEST(EmulatorGraphicsState, MemcpySkipAbsoluteRangesPreservesFenceHoles)
{
	using namespace Kyty::Libs::Graphics;
	uint8_t dst[32];
	uint8_t src[32];
	for (int i = 0; i < 32; i++)
	{
		dst[i] = static_cast<uint8_t>(0xA0 + i);
		src[i] = static_cast<uint8_t>(0x10 + i);
	}
	const uint64_t base       = reinterpret_cast<uint64_t>(dst);
	const uint64_t hole_begin = base + 8u;
	const uint64_t hole_end   = base + 16u;
	MemcpySkipAbsoluteRanges(dst, src, 32, &hole_begin, &hole_end, 1);
	for (int i = 0; i < 8; i++)
	{
		EXPECT_EQ(dst[i], static_cast<uint8_t>(0x10 + i));
	}
	for (int i = 8; i < 16; i++)
	{
		EXPECT_EQ(dst[i], static_cast<uint8_t>(0xA0 + i));
	}
	for (int i = 16; i < 32; i++)
	{
		EXPECT_EQ(dst[i], static_cast<uint8_t>(0x10 + i));
	}
}

TEST(EmulatorGraphicsState, GpuWritebackPageCacheCopiesOnlyChangedPagesAndPreservesFenceHoles)
{
	using namespace Kyty::Libs::Graphics;
	constexpr uint64_t PageSize = 16u;
	uint8_t            guest[PageSize * 3u] {};
	uint8_t            gpu[PageSize * 3u] {};
	for (uint64_t i = 0; i < sizeof(gpu); ++i)
	{
		guest[i] = static_cast<uint8_t>(i);
		gpu[i]   = static_cast<uint8_t>(i);
	}

	GpuWritebackPageCache cache(PageSize);
	cache.Reset(gpu, sizeof(gpu));

	gpu[PageSize + 2u] = 0xe1u;
	gpu[PageSize + 8u] = 0xe2u;
	const uint64_t hole_begin = reinterpret_cast<uint64_t>(guest) + PageSize + 8u;
	const uint64_t hole_end   = hole_begin + 1u;

	struct Notifications
	{
		uint64_t calls   = 0;
		uint64_t address = 0;
		uint64_t size    = 0;
	} notifications;
	const auto notify = [](void* opaque, uint64_t address, uint64_t size)
	{
		auto* state = static_cast<Notifications*>(opaque);
		state->calls++;
		state->address = address;
		state->size = size;
	};

	const auto first =
	    cache.CopyChangedPages(guest, gpu, sizeof(gpu), &hole_begin, &hole_end, 1, notify, &notifications);
	EXPECT_TRUE(first.content_changed);
	EXPECT_EQ(first.changed_pages, 1u);
	EXPECT_EQ(first.copied_bytes, PageSize);
	EXPECT_EQ(notifications.calls, 1u);
	EXPECT_EQ(notifications.address, reinterpret_cast<uint64_t>(guest) + PageSize);
	EXPECT_EQ(notifications.size, PageSize);
	EXPECT_EQ(guest[PageSize + 2u], 0xe1u);
	EXPECT_NE(guest[PageSize + 8u], 0xe2u);

	notifications = {};
	const auto second =
	    cache.CopyChangedPages(guest, gpu, sizeof(gpu), &hole_begin, &hole_end, 1, notify, &notifications);
	EXPECT_FALSE(second.content_changed);
	EXPECT_EQ(second.changed_pages, 0u);
	EXPECT_EQ(second.copied_bytes, 0u);
	EXPECT_EQ(notifications.calls, 0u);
}

// OnlyFlip → ActiveDeleted must be force-completed with Active after BufferFlush.
// Skipping ActiveDeleted left VideoOutSubmitFlip on vkGetEventStatus (empty Flip
// queue; guest ThreadFlag bit 0x1 And+ClearBits soft-lock around present 2400).
TEST(EmulatorGraphicsState, LabelForceCompleteFiresActiveDeletedOnlyFlip)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_EQ(LabelForceCompleteActionFor(true, false), LabelForceCompleteKind::FireKeep);
	EXPECT_EQ(LabelForceCompleteActionFor(false, true), LabelForceCompleteKind::FireDestroy);
	EXPECT_EQ(LabelForceCompleteActionFor(false, false), LabelForceCompleteKind::Skip);
	EXPECT_EQ(LabelForceCompleteActionFor(true, true), LabelForceCompleteKind::FireKeep);
}

TEST(EmulatorGraphicsState, LabelFenceRegistryPreservesUniqueHolesAfterTransientOwnersRetire)
{
	uint8_t dst[32] = {};
	uint8_t src[32] = {};
	for (int i = 0; i < 32; i++)
	{
		dst[i] = static_cast<uint8_t>(0xa0 + i);
		src[i] = static_cast<uint8_t>(0x10 + i);
	}

	LabelFenceRegistry registry;
	const uint64_t     hole = reinterpret_cast<uint64_t>(dst) + 8u;
	EXPECT_EQ(registry.Register(hole, 8), LabelFenceRegistrationStatus::Inserted);
	EXPECT_EQ(registry.Register(hole, 8), LabelFenceRegistrationStatus::AlreadyRegistered);
	EXPECT_EQ(registry.Register(hole, 0), LabelFenceRegistrationStatus::InvalidArgument);
	EXPECT_EQ(registry.Register(UINT64_MAX - 3, 8), LabelFenceRegistrationStatus::InvalidArgument);
	EXPECT_EQ(registry.Size(), 1u);

	Vector<uint64_t> hole_begin;
	Vector<uint64_t> hole_end;
	registry.Snapshot(&hole_begin, &hole_end);
	ASSERT_EQ(hole_begin.Size(), 1u);
	ASSERT_EQ(hole_end.Size(), 1u);

	// The registry is the durable owner. No live Label object is needed here.
	MemcpySkipAbsoluteRanges(dst, src, sizeof(dst), hole_begin.GetData(), hole_end.GetData(),
	                         static_cast<int>(hole_begin.Size()));
	for (int i = 0; i < 32; i++)
	{
		const auto expected = static_cast<uint8_t>((i >= 8 && i < 16) ? (0xa0 + i) : (0x10 + i));
		EXPECT_EQ(dst[i], expected);
	}
}

TEST(EmulatorGraphicsState, LabelFenceRegistryUnmapRemovesHolesBeforeAddressReuse)
{
	uint8_t            storage[64];
	uint8_t            source[64];
	for (int i = 0; i < 64; i++)
	{
		storage[i] = static_cast<uint8_t>(0xa0 + i);
		source[i]  = static_cast<uint8_t>(0x10 + i);
	}
	LabelFenceRegistry registry;
	const uint64_t     allocation = reinterpret_cast<uint64_t>(storage);

	ASSERT_EQ(registry.Register(allocation + 8u, 8u), LabelFenceRegistrationStatus::Inserted);
	ASSERT_EQ(registry.Register(allocation + 32u, 12u), LabelFenceRegistrationStatus::Inserted);
	ASSERT_EQ(registry.Size(), 2u);

	Vector<uint64_t> begin;
	Vector<uint64_t> end;
	registry.Snapshot(&begin, &end);
	MemcpySkipAbsoluteRanges(storage, source, sizeof(storage), begin.GetData(), end.GetData(), static_cast<int>(begin.Size()));
	for (int i = 0; i < 64; i++)
	{
		const bool protected_byte = (i >= 8 && i < 16) || (i >= 32 && i < 44);
		EXPECT_EQ(storage[i], static_cast<uint8_t>(protected_byte ? (0xa0 + i) : (0x10 + i)));
	}

	EXPECT_EQ(registry.ReleaseAllocation(allocation + 10u, 4u), LabelFenceReleaseStatus::PartialOverlap);
	EXPECT_EQ(registry.Size(), 2u);
	EXPECT_EQ(registry.ReleaseAllocation(allocation, sizeof(storage)), LabelFenceReleaseStatus::Released);
	EXPECT_EQ(registry.Size(), 0u);

	std::memset(storage, 0xa5, sizeof(storage));
	registry.Snapshot(&begin, &end);
	MemcpySkipAbsoluteRanges(storage, source, sizeof(storage), begin.GetData(), end.GetData(), static_cast<int>(begin.Size()));
	EXPECT_EQ(std::memcmp(storage, source, sizeof(storage)), 0);

	EXPECT_EQ(registry.Register(allocation + 8u, 4u), LabelFenceRegistrationStatus::Inserted);
	EXPECT_EQ(registry.Size(), 1u);
	EXPECT_EQ(registry.ReleaseAllocation(allocation + sizeof(storage), 16u), LabelFenceReleaseStatus::NotFound);
	EXPECT_EQ(registry.ReleaseAllocation(allocation, 0u), LabelFenceReleaseStatus::InvalidArgument);
	EXPECT_EQ(registry.ReleaseAllocation(UINT64_MAX - 3u, 8u), LabelFenceReleaseStatus::InvalidArgument);

	registry.Snapshot(&begin, &end);
	ASSERT_EQ(begin.Size(), 1u);
	ASSERT_EQ(end.Size(), 1u);
	EXPECT_EQ(begin.At(0), allocation + 8u);
	EXPECT_EQ(end.At(0), allocation + 12u);
}

TEST(EmulatorGraphicsState, LabelFenceGdsStoreProtectsEveryWrittenDword)
{
	uint8_t dst[32];
	uint8_t src[32];
	for (int i = 0; i < 32; i++)
	{
		dst[i] = static_cast<uint8_t>(0xa0 + i);
		src[i] = static_cast<uint8_t>(0x10 + i);
	}

	constexpr uint32_t dword_count = 3;
	const uint64_t     base        = reinterpret_cast<uint64_t>(dst);
	const uint64_t     store_bytes = LabelDwordStoreSizeBytes(dword_count);
	ASSERT_EQ(store_bytes, 12u);

	LabelFenceRegistry registry;
	ASSERT_EQ(registry.Register(base + 8u, store_bytes), LabelFenceRegistrationStatus::Inserted);

	Vector<uint64_t> begin;
	Vector<uint64_t> end;
	registry.Snapshot(&begin, &end);
	MemcpySkipAbsoluteRanges(dst, src, sizeof(dst), begin.GetData(), end.GetData(), static_cast<int>(begin.Size()));

	for (int i = 0; i < 32; i++)
	{
		const auto expected = static_cast<uint8_t>((i >= 8 && i < 20) ? (0xa0 + i) : (0x10 + i));
		EXPECT_EQ(dst[i], expected);
	}
}

// SubmitAndFlip without an embedded R_FLIP/0x777 must still flip after BufferFlush.
// Detect SubmitAndFlip via an explicit batch flag — not flip.handle != 0 (handle 0
// is legal, and plain Submit also zeroes the flip fields).
TEST(EmulatorGraphicsState, GraphicsBatchNeedsApiFlipWhenDcbOmitsFlip)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_TRUE(GraphicsBatchNeedsApiFlip(true, false));
	EXPECT_FALSE(GraphicsBatchNeedsApiFlip(true, true));
	EXPECT_FALSE(GraphicsBatchNeedsApiFlip(false, false));
	EXPECT_FALSE(GraphicsBatchNeedsApiFlip(false, true));
}

// Guest-visible completion callbacks must not depend on a later batch to
// publish their containing submission.
TEST(EmulatorGraphicsState, GraphicsBatchWaitsForSubmissionContainingCallback)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_TRUE(GraphicsBatchNeedsSubmissionCompletion(true));
	EXPECT_FALSE(GraphicsBatchNeedsSubmissionCompletion(false));
}

// Host presentation: default swapchain selection must stay LDR sRGB even when a
// driver lists HDR10 (or other host-HDR color spaces) first.
TEST(EmulatorGraphicsState, HostHdrColorSpacesAreDetected)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_TRUE(VulkanColorSpaceIsHostHdr(VK_COLOR_SPACE_HDR10_ST2084_EXT));
	EXPECT_TRUE(VulkanColorSpaceIsHostHdr(VK_COLOR_SPACE_HDR10_HLG_EXT));
	EXPECT_TRUE(VulkanColorSpaceIsHostHdr(VK_COLOR_SPACE_DOLBYVISION_EXT));
	EXPECT_TRUE(VulkanColorSpaceIsHostHdr(VK_COLOR_SPACE_BT2020_LINEAR_EXT));
	EXPECT_FALSE(VulkanColorSpaceIsHostHdr(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR));
}

TEST(EmulatorGraphicsState, DefaultSwapchainPrefersLdrSrgbOverHdr10First)
{
	using namespace Kyty::Libs::Graphics;
	const VkSurfaceFormatKHR formats[] = {
	    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
	    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	};
	const auto chosen = SelectDefaultSwapchainSurfaceFormat(formats, 3u);
	EXPECT_EQ(chosen.format, VK_FORMAT_B8G8R8A8_UNORM);
	EXPECT_EQ(chosen.colorSpace, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
	EXPECT_FALSE(VulkanColorSpaceIsHostHdr(chosen.colorSpace));
}

TEST(EmulatorGraphicsState, DefaultSwapchainPrefersUnormSrgbOverSrgbFormat)
{
	using namespace Kyty::Libs::Graphics;
	const VkSurfaceFormatKHR formats[] = {
	    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	};
	const auto chosen = SelectDefaultSwapchainSurfaceFormat(formats, 2u);
	EXPECT_EQ(chosen.format, VK_FORMAT_B8G8R8A8_UNORM);
	EXPECT_EQ(chosen.colorSpace, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
}

TEST(EmulatorGraphicsState, ResolvesContiguousMultiRenderTargetLayout)
{
	auto zero = State::ResolveColorTargetLayout(0u);
	EXPECT_EQ(zero.count, 0u);
	EXPECT_EQ(zero.error, State::ColorTargetLayoutError::None);

	auto one = State::ResolveColorTargetLayout(0xFu);
	EXPECT_EQ(one.count, 1u);
	EXPECT_EQ(one.nibbles[0], 0xFu);
	EXPECT_EQ(one.error, State::ColorTargetLayoutError::None);

	auto four = State::ResolveColorTargetLayout(0x0000FFFFu);
	EXPECT_EQ(four.count, 4u);
	for (uint32_t i = 0; i < 4; i++)
	{
		EXPECT_EQ(four.nibbles[i], 0xFu) << "slot " << i;
	}
	EXPECT_EQ(four.error, State::ColorTargetLayoutError::None);

	auto eight = State::ResolveColorTargetLayout(0xFFFFFFFFu);
	EXPECT_EQ(eight.count, 8u);
	for (uint32_t i = 0; i < 8; i++)
	{
		EXPECT_EQ(eight.nibbles[i], 0xFu) << "slot " << i;
	}
	EXPECT_EQ(eight.error, State::ColorTargetLayoutError::None);
}

TEST(EmulatorGraphicsState, RejectsGappedMultiRenderTargetLayout)
{
	// RT0 full, RT1 zero, RT2 full → hole after contiguous prefix.
	auto gap = State::ResolveColorTargetLayout(0x00000F0Fu);
	EXPECT_EQ(gap.error, State::ColorTargetLayoutError::Gapped);
}

TEST(EmulatorGraphicsState, IgnoresMaskBitsForUnconfiguredRenderTargets)
{
	// Only RT0 has a CB_COLORn_BASE; stale/nonzero higher mask nibbles do not
	// create attachments and must not turn this single-target pass into a gap.
	auto rt0 = State::ResolveColorTargetLayout(0xb8a601afu, 1u);
	EXPECT_EQ(rt0.count, 1u);
	EXPECT_EQ(rt0.nibbles[0], 0xfu);
	EXPECT_EQ(rt0.error, State::ColorTargetLayoutError::None);

	// A real RT2 with RT1 absent remains an invalid gapped layout.
	auto gap = State::ResolveColorTargetLayout(0x00000f0fu, 3u);
	EXPECT_EQ(gap.error, State::ColorTargetLayoutError::Gapped);
}

TEST(EmulatorGraphicsState, AcceptsPartialChannelRenderTargetLayout)
{
	// RT0 partial channels (R+B only).
	auto partial = State::ResolveColorTargetLayout(0x00000005u);
	EXPECT_EQ(partial.count, 1u);
	EXPECT_EQ(partial.nibbles[0], 0x5u);
	EXPECT_EQ(partial.error, State::ColorTargetLayoutError::None);
}

TEST(EmulatorGraphicsState, RecognizesObservedHtileClearPattern)
{
	uint32_t clear_words[8];
	for (auto& word: clear_words)
	{
		word = 0xfffffff0u;
	}
	EXPECT_TRUE(DepthMetaIsClearPattern(clear_words, sizeof(clear_words)));

	clear_words[3] = 0xffffffffu;
	EXPECT_FALSE(DepthMetaIsClearPattern(clear_words, sizeof(clear_words)));
	EXPECT_FALSE(DepthMetaIsClearPattern(nullptr, sizeof(clear_words)));
	EXPECT_FALSE(DepthMetaIsClearPattern(clear_words, sizeof(clear_words) - 1));
}

TEST(EmulatorGraphicsState, ConsumesTrackedHtileClearOnce)
{
	constexpr uint64_t address = 0x12345000u;
	DepthMetaMarkClear(address);
	EXPECT_TRUE(DepthMetaConsumeClear(address));
	EXPECT_FALSE(DepthMetaConsumeClear(address));
}

TEST(EmulatorGraphicsState, HtilePendingClearDoesNotSuppressDepthWrite)
{
	auto actions = State::ResolveDepthClearActions(false, true);
	EXPECT_TRUE(actions.vulkan_clear);
	EXPECT_FALSE(actions.suppress_depth_write);
}

TEST(EmulatorGraphicsState, MatchesOnlyExactHtileStorageRange)
{
	EXPECT_TRUE(DepthMetaMatchesStorageRange(0x120000, 0x8000, 0x120000, 0x8000));
	EXPECT_FALSE(DepthMetaMatchesStorageRange(0x120000, 0x4000, 0x120000, 0x8000));
	EXPECT_FALSE(DepthMetaMatchesStorageRange(0x124000, 0x4000, 0x120000, 0x8000));
	EXPECT_FALSE(DepthMetaMatchesStorageRange(0, 0x8000, 0, 0x8000));
}

TEST(EmulatorGraphicsState, Gen5SampleBackingRequiresExactLiveRenderTarget)
{
	using namespace Kyty::Libs::Graphics::State;
	// Third arg is "live RT alias found" (Equals, or non-exact Contains /
	// IsContainedWithin from FindRenderTexture).
	EXPECT_EQ(ResolveGen5SampleBacking(56, 27, true), Gen5SampleBacking::ExactRenderTarget);
	// 56+27 without RT: still GuestMemoryTexture object, but upload is skip_guest
	// (see Gen5SampleBackingAndUploadPolicyAreConsistent).
	EXPECT_EQ(ResolveGen5SampleBacking(56, 27, false), Gen5SampleBacking::GuestMemoryTexture);
	EXPECT_EQ(ResolveGen5SampleBacking(14, 27, false), Gen5SampleBacking::Unsupported);
	EXPECT_EQ(ResolveGen5SampleBacking(71, 27, false), Gen5SampleBacking::Unsupported);
	EXPECT_EQ(ResolveGen5SampleBacking(71, 27, true), Gen5SampleBacking::ExactRenderTarget);
	EXPECT_EQ(ResolveGen5SampleBacking(133, 27, false), Gen5SampleBacking::GuestMemoryTexture);
	EXPECT_EQ(ResolveGen5SampleBacking(133, 27, true), Gen5SampleBacking::ExactRenderTarget);
	EXPECT_EQ(ResolveGen5SampleBacking(56, 0, false), Gen5SampleBacking::GuestMemoryTexture);
	EXPECT_EQ(ResolveGen5SampleBacking(56, 9, false), Gen5SampleBacking::GuestMemoryTexture);
	EXPECT_EQ(ResolveGen5SampleBacking(133, 9, false), Gen5SampleBacking::Unsupported);
}

// ResolveGen5SampleBacking and Gen5SampleMayGuestUploadTiled share one contract:
// tile27+56 may create a Texture object without ever CPU-detiling guest pages.
TEST(EmulatorGraphicsState, Gen5SampleBackingAndUploadPolicyAreConsistent)
{
	using namespace Kyty::Libs::Graphics;
	using namespace Kyty::Libs::Graphics::State;

	// RGBA8 kRenderTarget sample without live alias: Texture object + skip_guest.
	EXPECT_EQ(ResolveGen5SampleBacking(56, 27, false), Gen5SampleBacking::GuestMemoryTexture);
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, 56u, false));
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, 56u, true));

	// RGBA16F kRenderTarget without live alias: no Texture object (must find RT).
	EXPECT_EQ(ResolveGen5SampleBacking(71, 27, false), Gen5SampleBacking::Unsupported);
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, 71u, false));

	// BC1 package may detile when uncovered; never under a live surface.
	EXPECT_EQ(ResolveGen5SampleBacking(133, 27, false), Gen5SampleBacking::GuestMemoryTexture);
	EXPECT_TRUE(Gen5SampleMayGuestUploadTiled(27u, 133u, false));
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, 133u, true));

	// With live RT alias, Resolve prefers ExactRenderTarget; upload never detiles.
	EXPECT_EQ(ResolveGen5SampleBacking(56, 27, true), Gen5SampleBacking::ExactRenderTarget);
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, 56u, true));
}

TEST(EmulatorGraphicsState, ResolvesObservedSamplerClampModes)
{
	using namespace Kyty::Libs::Graphics::State;

	EXPECT_EQ(ResolveSamplerAddressMode(0), SamplerAddressMode::Repeat);
	EXPECT_EQ(ResolveSamplerAddressMode(1), SamplerAddressMode::MirroredRepeat);
	EXPECT_EQ(ResolveSamplerAddressMode(2), SamplerAddressMode::ClampToEdge);
	EXPECT_EQ(ResolveSamplerAddressMode(6), SamplerAddressMode::ClampToBorder);
	EXPECT_EQ(ResolveSamplerAddressMode(7), SamplerAddressMode::ClampToBorder);
}

// Residual visual (world false-color, HUD correct): intermediate format-71 and
// format-56 sampled paths must keep distinct Vulkan formats — float RT aliases
// as SFLOAT, RGBA8 samples default UNORM (sRGB only with evidenced force_degamma).
TEST(EmulatorGraphicsState, Gen5SampledFormatsPreserveFloatAndUnormContracts)
{
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 71, false), VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 71, true), VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 56, false), VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 56, true), VK_FORMAT_R8G8B8A8_SRGB);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 13, false), VK_FORMAT_R16_SFLOAT);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 14, false), VK_FORMAT_R8G8_UNORM);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 133, false), VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 133, true), VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
}

TEST(EmulatorGraphicsState, RegularImageSamplingDisablesSamplerComparison)
{
	using namespace Kyty::Libs::Graphics::State;
	const auto comparison = ResolveSamplerComparison(0, ImageSampleOperation::Regular);
	EXPECT_FALSE(comparison.enabled);
	EXPECT_EQ(comparison.function, 0);
}

// GraphicsInit must publish API version and feature-flag words into the guest
// AGC state buffer; leaving them unset lets titles take a null-deref after
// CreateShader / register-default use (see GraphicsInitWriteGuestState).
TEST(EmulatorGraphicsState, GraphicsInitWriteGuestStatePublishesVersionAndFeatureWords)
{
	EXPECT_FALSE(Gen5::GraphicsInitWriteGuestState(nullptr, 8u));

	uint32_t state[4] = {0xdeadbeefu, 0xcafebabeu, 0x11111111u, 0x22222222u};
	EXPECT_TRUE(Gen5::GraphicsInitWriteGuestState(state, 8u));
	EXPECT_EQ(state[0], 8u);
	EXPECT_EQ(state[1], 0u);
	// Only the documented two words are written.
	EXPECT_EQ(state[2], 0x11111111u);
	EXPECT_EQ(state[3], 0x22222222u);

	EXPECT_TRUE(Gen5::GraphicsInitWriteGuestState(state, 7u));
	EXPECT_EQ(state[0], 7u);
	EXPECT_EQ(state[1], 0u);
}

TEST(EmulatorGraphicsState, ResolvesEffectiveStencilFormatWithoutSeparatePlane)
{
	HW::DepthRenderTarget z {};
	z.stencil_info.format               = 1;
	z.stencil_info.tile_stencil_disable = true;
	z.stencil_read_base_addr            = 0;
	z.stencil_write_base_addr           = 0;
	EXPECT_EQ(State::ResolveEffectiveStencilFormat(z), 0u);

	z.stencil_write_base_addr = 0x1000;
	EXPECT_EQ(State::ResolveEffectiveStencilFormat(z), 1u);

	z.stencil_write_base_addr           = 0;
	z.stencil_info.tile_stencil_disable = false;
	EXPECT_EQ(State::ResolveEffectiveStencilFormat(z), 1u);

	z.stencil_info.format = 0;
	EXPECT_EQ(State::ResolveEffectiveStencilFormat(z), 0u);
}

UT_END();
