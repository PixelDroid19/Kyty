#include "Emulator/Graphics/ComputePipelinePrecompile.h"

#include "Emulator/AtomicFile.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/SpirvBinaryCacheStore.h"
#include "Emulator/Log.h"
#include "Emulator/SystemContentPort.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

constexpr uint32_t kRecipeMagic   = 0x5250434bu; // "KCPR"
constexpr uint32_t kRecipeVersion = 1;
// magic, version, total words, vendor, device, stage flags, subgroup size,
// push-constant offset and size, set-layout flag, binding count.
constexpr size_t   kHeaderWords   = 11;
constexpr uint32_t kSpirvMagic    = 0x07230203u;
constexpr size_t   kMaxRecipes    = 2048;
constexpr uintmax_t kMaxDirectoryBytes = 256u * 1024u * 1024u;
constexpr char     kExtension[]   = ".kcpr";
constexpr uint32_t kWorkers       = 2;

bool DescriptorTypeKnown(uint32_t type)
{
	using Type = ShaderDescriptorLimits::DescriptorType;
	return type >= static_cast<uint32_t>(Type::SampledImage) && type <= static_cast<uint32_t>(Type::UniformBuffer);
}

std::string HashName(uint64_t hash)
{
	char name[32];
	snprintf(name, sizeof(name), "%016llx%s", static_cast<unsigned long long>(hash), kExtension);
	return name;
}

bool ReadRecipeFile(const std::filesystem::path& path, uintmax_t size, ComputePipelineRecipe* recipe)
{
	if (size % sizeof(uint32_t) != 0u || size > (kComputePipelineRecipeMaxSpirvWords + 64u * 1024u) * sizeof(uint32_t))
	{
		return false;
	}
	std::vector<uint32_t> words(static_cast<size_t>(size / sizeof(uint32_t)));
	std::ifstream         file(path, std::ios::binary);
	return file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size)) &&
	       ComputePipelineRecipeDecode(words.data(), words.size(), recipe);
}

enum class EntryState
{
	Running,
	Done
};

struct QueuedRecipe
{
	uint64_t              hash = 0;
	uintmax_t             size = 0;
	std::filesystem::path path;
};

struct Service
{
	std::mutex                               mutex;
	std::condition_variable                  done;
	std::unordered_map<uint64_t, EntryState> entries;
	bool                                     started         = false;
	std::filesystem::path                    directory;
	uint32_t                                 vendor_id       = 0;
	uint32_t                                 device_id       = 0;
	ComputePipelinePrecompileCreate          create          = nullptr;
	size_t                                   recipe_count    = 0;
	uintmax_t                                directory_bytes = 0;
};

// Never destroyed: a worker may still be inside the driver at process exit.
Service& State()
{
	static auto* service = new Service;
	return *service;
}

std::vector<QueuedRecipe> ScanDirectory(const std::filesystem::path& directory, size_t* count, uintmax_t* bytes)
{
	std::vector<QueuedRecipe> queue;
	std::error_code           error;
	for (const auto& entry: std::filesystem::directory_iterator(directory, error))
	{
		if (queue.size() >= kMaxRecipes)
		{
			break;
		}
		const auto& path = entry.path();
		if (!entry.is_regular_file(error) || path.extension() != kExtension)
		{
			continue;
		}
		const auto size = entry.file_size(error);
		char*      end  = nullptr;
		const auto stem = path.stem().string();
		const auto hash = std::strtoull(stem.c_str(), &end, 16);
		if (error || stem.size() != 16u || end != stem.c_str() + stem.size())
		{
			continue;
		}
		queue.push_back({hash, size, path});
		*bytes += size;
	}
	*count = queue.size();
	// Largest modules first: they are the ones whose compile can stall a dispatch.
	std::sort(queue.begin(), queue.end(), [](const auto& a, const auto& b) { return a.size > b.size; });
	return queue;
}

void Precompile(const QueuedRecipe& queued)
{
	auto& service = State();
	{
		std::lock_guard lock(service.mutex);
		if (!service.entries.try_emplace(queued.hash, EntryState::Running).second)
		{
			return;
		}
	}
	ComputePipelineRecipe recipe;
	if (ReadRecipeFile(queued.path, queued.size, &recipe) && recipe.vendor_id == service.vendor_id &&
	    recipe.device_id == service.device_id && ComputePipelineRecipeHash(recipe) == queued.hash)
	{
		DebugStatsBeginComputePrecompile();
		const auto start = std::chrono::steady_clock::now();
		const bool ok    = service.create(recipe);
		const auto ns    = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
		DebugStatsEndComputePrecompile(ok, static_cast<uint64_t>(ns));
	}
	{
		std::lock_guard lock(service.mutex);
		service.entries[queued.hash] = EntryState::Done;
	}
	service.done.notify_all();
}

void Record(const ComputePipelineRecipe& recipe, uint64_t hash)
{
	auto& service = State();
	if (service.directory.empty())
	{
		return;
	}
	const auto      path = service.directory / HashName(hash);
	std::error_code error;
	if (std::filesystem::exists(path, error))
	{
		return;
	}
	const auto words = ComputePipelineRecipeEncode(recipe);
	const auto size  = static_cast<uintmax_t>(words.size()) * sizeof(uint32_t);
	{
		std::lock_guard lock(service.mutex);
		if (service.recipe_count >= kMaxRecipes || size > kMaxDirectoryBytes - std::min(service.directory_bytes, kMaxDirectoryBytes))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 1, "WARNING: compute pipeline recipe store is full: recipes=%zu bytes=%ju\n",
			               service.recipe_count, service.directory_bytes);
			return;
		}
		service.recipe_count++;
		service.directory_bytes += size;
	}
	std::filesystem::create_directories(service.directory, error);
	if (!AtomicFileWrite(path, words.data(), static_cast<size_t>(size)))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 4, "WARNING: compute pipeline recipe was not written\n");
	}
}

} // namespace

std::vector<uint32_t> ComputePipelineRecipeEncode(const ComputePipelineRecipe& recipe)
{
	const size_t bindings = recipe.has_set_layout ? recipe.plan.binding_count : 0u;
	const size_t total    = kHeaderWords + bindings * 3u + 1u + recipe.spirv.size();

	std::vector<uint32_t> words;
	words.reserve(total);
	words.insert(words.end(), {kRecipeMagic, kRecipeVersion, static_cast<uint32_t>(total), recipe.vendor_id, recipe.device_id,
	                           recipe.stage_flags, recipe.required_subgroup_size, recipe.push_constant_offset, recipe.push_constant_size,
	                           recipe.has_set_layout ? 1u : 0u, static_cast<uint32_t>(bindings)});
	for (size_t i = 0; i < bindings; ++i)
	{
		const auto& binding = recipe.plan.bindings[i];
		words.insert(words.end(), {binding.binding, static_cast<uint32_t>(binding.type), binding.count});
	}
	words.push_back(static_cast<uint32_t>(recipe.spirv.size()));
	words.insert(words.end(), recipe.spirv.begin(), recipe.spirv.end());
	return words;
}

bool ComputePipelineRecipeDecode(const uint32_t* words, size_t count, ComputePipelineRecipe* recipe)
{
	if (words == nullptr || recipe == nullptr || count < kHeaderWords + 1u || words[0] != kRecipeMagic ||
	    words[1] != kRecipeVersion || words[2] != count || words[9] > 1u)
	{
		return false;
	}
	const bool   has_set_layout = words[9] != 0u;
	const size_t bindings       = words[10];
	if (bindings > ShaderDescriptorLayoutPlan::MAX_BINDINGS || (!has_set_layout && bindings != 0u) ||
	    count < kHeaderWords + bindings * 3u + 1u)
	{
		return false;
	}
	ComputePipelineRecipe out;
	out.vendor_id              = words[3];
	out.device_id              = words[4];
	out.stage_flags            = words[5];
	out.required_subgroup_size = words[6];
	out.push_constant_offset   = words[7];
	out.push_constant_size     = words[8];
	out.has_set_layout         = has_set_layout;
	out.plan.binding_count     = bindings;
	const uint32_t* cursor     = words + kHeaderWords;
	for (size_t i = 0; i < bindings; ++i, cursor += 3)
	{
		if (!DescriptorTypeKnown(cursor[1]) || cursor[2] == 0u)
		{
			return false;
		}
		out.plan.bindings[i] = {cursor[0], static_cast<ShaderDescriptorLimits::DescriptorType>(cursor[1]), cursor[2]};
	}
	const size_t spirv_words = *cursor++;
	if (spirv_words < 5u || spirv_words > kComputePipelineRecipeMaxSpirvWords ||
	    static_cast<size_t>(cursor - words) + spirv_words != count || cursor[0] != kSpirvMagic)
	{
		return false;
	}
	out.spirv.assign(cursor, cursor + spirv_words);
	*recipe = std::move(out);
	return true;
}

uint64_t ComputePipelineRecipeHash(const ComputePipelineRecipe& recipe)
{
	const auto words = ComputePipelineRecipeEncode(recipe);
	return XXH3_64bits(words.data(), words.size() * sizeof(uint32_t));
}

std::filesystem::path ComputePipelinePrecompileDirectory()
{
	Core::String title_id;
	Core::String app_version;
	if (!::Kyty::Emulator::SystemContentPort::GetMetadata(&title_id, &app_version) || title_id.IsEmpty())
	{
		return {};
	}
	std::string name = std::string(title_id.C_Str()) + "-" + app_version.C_Str();
	std::replace_if(name.begin(), name.end(), [](char c) { return !(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.'); },
	                '_');
	return SpirvBinaryCacheDefaultRoot() / "compute-pipelines" / name;
}

void ComputePipelinePrecompileStart(const std::filesystem::path& directory, uint32_t vendor_id, uint32_t device_id,
                                    ComputePipelinePrecompileCreate create)
{
	auto&                     service = State();
	std::vector<QueuedRecipe> queue;
	{
		std::lock_guard lock(service.mutex);
		if (service.started)
		{
			return;
		}
		service.started   = true;
		service.directory = directory;
		service.vendor_id = vendor_id;
		service.device_id = device_id;
		service.create    = create;
		if (directory.empty() || create == nullptr)
		{
			return;
		}
		queue = ScanDirectory(directory, &service.recipe_count, &service.directory_bytes);
	}
	if (queue.empty())
	{
		return;
	}
	KYTY_LOG_DEBUG("compute pipeline precompile: %zu recipes\n", queue.size());
	// A heavy compile holds about 2.4 GiB of driver memory, so the workers are few.
	struct SharedQueue
	{
		std::vector<QueuedRecipe> recipes;
		std::atomic<size_t>       next {0};
	};
	auto shared     = std::make_shared<SharedQueue>();
	shared->recipes = std::move(queue);
	for (uint32_t worker = 0; worker < kWorkers; ++worker)
	{
		std::thread(
		    [shared]
		    {
			    for (size_t index = shared->next++; index < shared->recipes.size(); index = shared->next++)
			    {
				    Precompile(shared->recipes[index]);
			    }
		    })
		    .detach();
	}
}

void ComputePipelinePrecompileClaim(const ComputePipelineRecipe& recipe)
{
	auto&      service = State();
	const auto hash    = ComputePipelineRecipeHash(recipe);
	{
		std::unique_lock lock(service.mutex);
		const auto [entry, inserted] = service.entries.try_emplace(hash, EntryState::Done);
		if (!inserted)
		{
			if (entry->second == EntryState::Running)
			{
				DebugStatsBeginComputePrecompileWait();
				const auto start = std::chrono::steady_clock::now();
				service.done.wait(lock, [&] { return service.entries[hash] == EntryState::Done; });
				const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
				DebugStatsEndComputePrecompileWait(static_cast<uint64_t>(ns));
			}
			return;
		}
	}
	Record(recipe, hash);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
