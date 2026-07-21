#include "Emulator/Graphics/SpirvBinaryCacheStore.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/ShaderTranslationCache.h"

#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

constexpr uint8_t  k_magic[8]       = {'K', 'Y', 'T', 'S', 'P', 'V', '1', '\0'};
constexpr uint32_t k_header_size    = 80;
constexpr uint32_t k_format_version = 1;
constexpr uint32_t k_target_vulkan_12 = 1;
constexpr uint32_t k_spirv_magic      = 0x07230203u;
constexpr char     k_key_domain[]      = "KytySpirvAssemblyCacheKey";
constexpr char     k_module_key_domain[] = "KytySpirvModuleCacheKey";
constexpr char     k_source_extension[]  = ".spvbin";
constexpr char     k_module_extension[]  = ".spvmod";

void AppendU32(std::vector<uint8_t>* bytes, uint32_t value)
{
	for (uint32_t shift = 0; shift < 32; shift += 8)
	{
		bytes->push_back(static_cast<uint8_t>(value >> shift));
	}
}

void AppendU64(std::vector<uint8_t>* bytes, uint64_t value)
{
	for (uint32_t shift = 0; shift < 64; shift += 8)
	{
		bytes->push_back(static_cast<uint8_t>(value >> shift));
	}
}

uint32_t ReadU32(const uint8_t* bytes)
{
	uint32_t value = 0;
	for (uint32_t shift = 0; shift < 32; shift += 8)
	{
		value |= static_cast<uint32_t>(*bytes++) << shift;
	}
	return value;
}

uint64_t ReadU64(const uint8_t* bytes)
{
	uint64_t value = 0;
	for (uint32_t shift = 0; shift < 64; shift += 8)
	{
		value |= static_cast<uint64_t>(*bytes++) << shift;
	}
	return value;
}

XXH128_hash_t SourceKey(const String8& source, uint32_t optimization, bool validation_enabled)
{
	std::vector<uint8_t> canonical;
	canonical.reserve(sizeof(k_key_domain) + 24u + source.Size());
	canonical.insert(canonical.end(), std::begin(k_key_domain), std::end(k_key_domain));
	AppendU32(&canonical, kSpirvBinaryCacheSchemaVersion);
	AppendU32(&canonical, k_target_vulkan_12);
	AppendU32(&canonical, optimization);
	AppendU32(&canonical, validation_enabled ? 1u : 0u);
	AppendU64(&canonical, source.Size());
	canonical.insert(canonical.end(), source.GetDataConst(), source.GetDataConst() + source.Size());
	return XXH3_128bits(canonical.data(), canonical.size());
}

std::vector<uint8_t> ModuleIdentity(const ShaderModuleKey& key, bool validation_enabled)
{
	std::vector<uint8_t> canonical;
	canonical.reserve(sizeof(k_module_key_domain) + 40u + static_cast<size_t>(key.shader_id.ids.Size()) * sizeof(uint32_t));
	canonical.insert(canonical.end(), std::begin(k_module_key_domain), std::end(k_module_key_domain));
	AppendU32(&canonical, kSpirvBinaryCacheSchemaVersion);
	AppendU32(&canonical, k_target_vulkan_12);
	AppendU32(&canonical, key.shader_id.hash0);
	AppendU32(&canonical, key.shader_id.crc32);
	AppendU32(&canonical, static_cast<uint32_t>(key.shader_id.ids.Size()));
	for (uint32_t id: key.shader_id.ids)
	{
		AppendU32(&canonical, id);
	}
	AppendU32(&canonical, static_cast<uint32_t>(key.stage));
	AppendU32(&canonical, static_cast<uint32_t>(key.optimization));
	AppendU32(&canonical, key.next_gen ? 1u : 0u);
	AppendU32(&canonical, key.debug_printf_enabled ? 1u : 0u);
	AppendU32(&canonical, key.translator_version);
	AppendU32(&canonical, validation_enabled ? 1u : 0u);
	return canonical;
}

std::string KeyName(XXH128_hash_t key, const char* extension)
{
	std::ostringstream name;
	name << std::hex << std::setfill('0') << std::setw(16) << key.high64 << std::setw(16) << key.low64 << extension;
	return name.str();
}

bool IsCacheEntry(const std::filesystem::path& path)
{
	const auto extension = path.extension();
	return extension == k_source_extension || extension == k_module_extension;
}

size_t CacheUsage(const std::filesystem::path& root)
{
	std::error_code error;
	size_t          total = 0;
	for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
	{
		if (!it->is_regular_file(error) || error || !IsCacheEntry(it->path()))
		{
			error.clear();
			continue;
		}
		const auto size = it->file_size(error);
		if (!error && size <= std::numeric_limits<size_t>::max() - total)
		{
			total += static_cast<size_t>(size);
		}
		error.clear();
	}
	return total;
}

bool ReplaceFile(const std::filesystem::path& source, const std::filesystem::path& destination)
{
#ifdef _WIN32
	return MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
	return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
}

uint64_t ProcessId()
{
#ifdef _WIN32
	return GetCurrentProcessId();
#else
	return static_cast<uint64_t>(getpid());
#endif
}

} // namespace

struct SpirvBinaryCacheStore::State
{
	struct PendingEntry
	{
		std::vector<uint8_t> identity;
		Vector<uint32_t>     binary;
		std::string          key_name;
		std::string          extension;
		uint32_t             optimization      = 0;
		bool                 validation_enabled = false;
		uint64_t             key_low           = 0;
		uint64_t             key_high          = 0;
		size_t               entry_size        = 0;
	};
	struct CompletedIdentity
	{
		std::string          key_name;
		std::vector<uint8_t> identity;
	};

	std::filesystem::path root;
	SpirvBinaryCacheLimits limits;
	size_t                 session_bytes_attempted = 0;
	mutable std::mutex     write_mutex;
	std::atomic<uint64_t>  temporary_sequence {0};

	mutable std::mutex queue_mutex;
	std::condition_variable queue_ready;
	std::deque<std::shared_ptr<PendingEntry>> queue;
	std::unordered_map<std::string, std::shared_ptr<PendingEntry>> active;
	std::deque<CompletedIdentity> completed;
	std::thread worker;
	bool stopping = false;
	size_t pending_bytes = 0;
	SpirvBinaryCacheAsyncStats async_stats;
	std::mutex hook_mutex;
	WriteHookForTesting write_hook = nullptr;
	void* write_hook_opaque = nullptr;
};

SpirvBinaryCacheStore::SpirvBinaryCacheStore(std::filesystem::path root, SpirvBinaryCacheLimits limits): m_state(new State)
{
	m_state->root   = std::move(root);
	m_state->limits = limits;
	m_state->worker = std::thread([this] { WorkerMain(); });
}

SpirvBinaryCacheStore::~SpirvBinaryCacheStore()
{
	if (m_state != nullptr)
	{
		{
			std::lock_guard<std::mutex> lock(m_state->queue_mutex);
			m_state->stopping = true;
		}
		m_state->queue_ready.notify_one();
		if (m_state->worker.joinable())
		{
			m_state->worker.join();
		}
	}
	delete m_state;
	m_state = nullptr;
}

SpirvBinaryCacheLoadResult SpirvBinaryCacheStore::Load(const String8& source, uint32_t optimization, bool validation_enabled,
                                                       Vector<uint32_t>* binary)
{
	const auto key = SourceKey(source, optimization, validation_enabled);
	return LoadEntry(reinterpret_cast<const uint8_t*>(source.GetDataConst()), source.Size(), optimization, validation_enabled, key.low64,
	                 key.high64, k_source_extension, binary);
}

SpirvBinaryCacheLoadResult SpirvBinaryCacheStore::LoadModule(const ShaderModuleKey& key, bool validation_enabled,
                                                             Vector<uint32_t>* binary)
{
	const auto identity = ModuleIdentity(key, validation_enabled);
	const auto hash     = XXH3_128bits(identity.data(), identity.size());
	return LoadEntry(identity.data(), identity.size(), static_cast<uint32_t>(key.optimization), validation_enabled, hash.low64,
	                 hash.high64, k_module_extension, binary);
}

SpirvBinaryCacheLoadResult SpirvBinaryCacheStore::LoadEntry(const uint8_t* identity, size_t identity_size, uint32_t optimization,
                                                            bool validation_enabled, uint64_t key_low, uint64_t key_high,
                                                            const char* extension, Vector<uint32_t>* binary)
{
	EXIT_IF(m_state == nullptr);
	EXIT_IF(identity == nullptr && identity_size != 0);
	EXIT_IF(extension == nullptr);
	EXIT_IF(binary == nullptr);
	binary->Clear();

	const XXH128_hash_t key {key_low, key_high};
	const auto          key_name = KeyName(key, extension);
	const auto          cache_miss = [this, &key_name]
	{
		std::lock_guard<std::mutex> queue_lock(m_state->queue_mutex);
		m_state->completed.erase(
		    std::remove_if(m_state->completed.begin(), m_state->completed.end(),
		                   [&key_name](const auto& entry) { return entry.key_name == key_name; }),
		    m_state->completed.end());
		return SpirvBinaryCacheLoadResult::Miss;
	};
	const auto cache_corrupt = [this, &key_name]
	{
		std::lock_guard<std::mutex> queue_lock(m_state->queue_mutex);
		m_state->completed.erase(
		    std::remove_if(m_state->completed.begin(), m_state->completed.end(),
		                   [&key_name](const auto& entry) { return entry.key_name == key_name; }),
		    m_state->completed.end());
		return SpirvBinaryCacheLoadResult::Corrupt;
	};
	{
		std::lock_guard<std::mutex> queue_lock(m_state->queue_mutex);
		if (const auto found = m_state->active.find(key_name); found != m_state->active.end())
		{
			const auto& pending = *found->second;
			if (pending.identity.size() == identity_size &&
			    (identity_size == 0 || std::memcmp(pending.identity.data(), identity, identity_size) == 0))
			{
				*binary = pending.binary;
				return SpirvBinaryCacheLoadResult::Hit;
			}
		}
	}
	const auto path = m_state->root / key_name;

	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file)
	{
		return cache_miss();
	}
	const auto end = file.tellg();
	if (end < 0)
	{
		return cache_miss();
	}
	const auto file_size = static_cast<uint64_t>(end);
	if (file_size < k_header_size || file_size > m_state->limits.max_entry_bytes)
	{
		return cache_corrupt();
	}

	std::vector<uint8_t> data(static_cast<size_t>(file_size));
	file.seekg(0, std::ios::beg);
	if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
	{
		// A concurrent atomic replacement can make a previously observed path
		// temporarily unavailable. Treat persistence as a miss; malformed data
		// that was read completely is still rejected by the validation below.
		return cache_miss();
	}

	const uint8_t* header = data.data();
	if (memcmp(header, k_magic, sizeof(k_magic)) != 0 || ReadU32(header + 8) != k_header_size ||
	    ReadU32(header + 12) != k_format_version || ReadU32(header + 16) != kSpirvBinaryCacheSchemaVersion ||
	    ReadU32(header + 20) != k_target_vulkan_12 || ReadU32(header + 24) != optimization ||
	    ReadU32(header + 28) != (validation_enabled ? 1u : 0u))
	{
		return cache_corrupt();
	}

	const uint64_t stored_identity_size = ReadU64(header + 32);
	const uint64_t word_count           = ReadU64(header + 40);
	const uint64_t payload_size = ReadU64(header + 72);
	if (stored_identity_size != identity_size || word_count == 0 ||
	    word_count > (m_state->limits.max_entry_bytes / sizeof(uint32_t)) ||
	    stored_identity_size > m_state->limits.max_entry_bytes ||
	    word_count > (UINT64_MAX - stored_identity_size) / sizeof(uint32_t) ||
	    payload_size != stored_identity_size + word_count * sizeof(uint32_t) || payload_size > UINT64_MAX - k_header_size ||
	    file_size != k_header_size + payload_size || ReadU64(header + 48) != key.low64 || ReadU64(header + 56) != key.high64)
	{
		return cache_corrupt();
	}

	const uint8_t* stored_identity = data.data() + k_header_size;
	const uint8_t* stored_binary   = stored_identity + stored_identity_size;
	if ((identity_size != 0 && memcmp(stored_identity, identity, identity_size) != 0) ||
	    ReadU64(header + 64) != XXH3_64bits(stored_binary, static_cast<size_t>(word_count * sizeof(uint32_t))) ||
	    ReadU32(stored_binary) != k_spirv_magic)
	{
		return cache_corrupt();
	}

	for (uint64_t i = 0; i < word_count; ++i)
	{
		binary->Add(ReadU32(stored_binary + i * sizeof(uint32_t)));
	}
	return SpirvBinaryCacheLoadResult::Hit;
}

SpirvBinaryCacheStoreResult SpirvBinaryCacheStore::Store(const String8& source, uint32_t optimization, bool validation_enabled,
                                                         const Vector<uint32_t>& binary)
{
	const auto key = SourceKey(source, optimization, validation_enabled);
	return StoreEntry(reinterpret_cast<const uint8_t*>(source.GetDataConst()), source.Size(), optimization, validation_enabled, key.low64,
	                  key.high64, k_source_extension, binary);
}

SpirvBinaryCacheQueueResult SpirvBinaryCacheStore::QueueStore(const String8& source, uint32_t optimization, bool validation_enabled,
                                                              const Vector<uint32_t>& binary)
{
	const auto key = SourceKey(source, optimization, validation_enabled);
	return QueueEntry(reinterpret_cast<const uint8_t*>(source.GetDataConst()), source.Size(), optimization, validation_enabled, key.low64,
	                  key.high64, k_source_extension, binary);
}

SpirvBinaryCacheStoreResult SpirvBinaryCacheStore::StoreModule(const ShaderModuleKey& key, bool validation_enabled,
                                                               const Vector<uint32_t>& binary)
{
	const auto identity = ModuleIdentity(key, validation_enabled);
	const auto hash     = XXH3_128bits(identity.data(), identity.size());
	return StoreEntry(identity.data(), identity.size(), static_cast<uint32_t>(key.optimization), validation_enabled, hash.low64,
	                  hash.high64, k_module_extension, binary);
}

SpirvBinaryCacheQueueResult SpirvBinaryCacheStore::QueueStoreModule(const ShaderModuleKey& key, bool validation_enabled,
                                                                    const Vector<uint32_t>& binary)
{
	const auto identity = ModuleIdentity(key, validation_enabled);
	const auto hash     = XXH3_128bits(identity.data(), identity.size());
	return QueueEntry(identity.data(), identity.size(), static_cast<uint32_t>(key.optimization), validation_enabled, hash.low64,
	                  hash.high64, k_module_extension, binary);
}

SpirvBinaryCacheQueueResult SpirvBinaryCacheStore::QueueEntry(const uint8_t* identity, size_t identity_size, uint32_t optimization,
                                                              bool validation_enabled, uint64_t key_low, uint64_t key_high,
                                                              const char* extension, const Vector<uint32_t>& binary)
{
	EXIT_IF(m_state == nullptr);
	EXIT_IF(identity == nullptr && identity_size != 0);
	EXIT_IF(extension == nullptr);
	if (binary.IsEmpty() || binary[0] != k_spirv_magic)
	{
		return SpirvBinaryCacheQueueResult::Failed;
	}
	if (identity_size > std::numeric_limits<size_t>::max() - k_header_size ||
	    static_cast<size_t>(binary.Size()) >
	        (std::numeric_limits<size_t>::max() - k_header_size - identity_size) / sizeof(uint32_t))
	{
		return SpirvBinaryCacheQueueResult::TooLarge;
	}
	const size_t entry_size = k_header_size + identity_size + static_cast<size_t>(binary.Size()) * sizeof(uint32_t);
	if (entry_size > m_state->limits.max_entry_bytes || entry_size > m_state->limits.max_total_bytes)
	{
		return SpirvBinaryCacheQueueResult::TooLarge;
	}

	const std::string key_name = KeyName(XXH128_hash_t {key_low, key_high}, extension);
	std::lock_guard<std::mutex> lock(m_state->queue_mutex);
	if (m_state->stopping)
	{
		return SpirvBinaryCacheQueueResult::Failed;
	}
	if (const auto found = m_state->active.find(key_name); found != m_state->active.end())
	{
		const auto& pending = *found->second;
		if (pending.identity.size() != identity_size ||
		    (identity_size != 0 && std::memcmp(pending.identity.data(), identity, identity_size) != 0))
		{
			m_state->async_stats.dropped++;
			return SpirvBinaryCacheQueueResult::Failed;
		}
		m_state->async_stats.coalesced++;
		return SpirvBinaryCacheQueueResult::Coalesced;
	}
	const auto completed = std::find_if(m_state->completed.begin(), m_state->completed.end(),
	                                    [&key_name](const auto& entry) { return entry.key_name == key_name; });
	if (completed != m_state->completed.end())
	{
		if (completed->identity.size() != identity_size ||
		    (identity_size != 0 && std::memcmp(completed->identity.data(), identity, identity_size) != 0))
		{
			m_state->async_stats.dropped++;
			return SpirvBinaryCacheQueueResult::Failed;
		}
		m_state->async_stats.coalesced++;
		return SpirvBinaryCacheQueueResult::Coalesced;
	}
	if (m_state->active.size() >= m_state->limits.max_pending_entries ||
	    entry_size > m_state->limits.max_pending_bytes ||
	    m_state->pending_bytes > m_state->limits.max_pending_bytes - entry_size)
	{
		m_state->async_stats.dropped++;
		return SpirvBinaryCacheQueueResult::QueueFull;
	}

	auto pending = std::make_shared<State::PendingEntry>();
	if (identity_size != 0)
	{
		pending->identity.assign(identity, identity + identity_size);
	}
	pending->binary             = binary;
	pending->key_name           = key_name;
	pending->extension          = extension;
	pending->optimization       = optimization;
	pending->validation_enabled = validation_enabled;
	pending->key_low            = key_low;
	pending->key_high           = key_high;
	pending->entry_size         = entry_size;
	m_state->queue.push_back(pending);
	m_state->active.emplace(key_name, pending);
	m_state->pending_bytes += entry_size;
	m_state->async_stats.queued++;
	m_state->queue_ready.notify_one();
	return SpirvBinaryCacheQueueResult::Queued;
}

void SpirvBinaryCacheStore::WorkerMain()
{
	for (;;)
	{
		std::shared_ptr<State::PendingEntry> pending;
		{
			std::unique_lock<std::mutex> lock(m_state->queue_mutex);
			m_state->queue_ready.wait(lock, [this] { return m_state->stopping || !m_state->queue.empty(); });
			if (m_state->queue.empty())
			{
				if (m_state->stopping)
				{
					return;
				}
				continue;
			}
			pending = std::move(m_state->queue.front());
			m_state->queue.pop_front();
		}

		const auto result = StoreEntry(pending->identity.data(), pending->identity.size(), pending->optimization,
		                               pending->validation_enabled, pending->key_low, pending->key_high,
		                               pending->extension.c_str(), pending->binary);
		{
			std::lock_guard<std::mutex> lock(m_state->queue_mutex);
			m_state->active.erase(pending->key_name);
			EXIT_IF(pending->entry_size > m_state->pending_bytes);
			m_state->pending_bytes -= pending->entry_size;
			if (result == SpirvBinaryCacheStoreResult::Written)
			{
				if (m_state->limits.max_completed_entries != 0)
				{
					m_state->completed.erase(
					    std::remove_if(m_state->completed.begin(), m_state->completed.end(),
					                   [&pending](const auto& entry) { return entry.key_name == pending->key_name; }),
					    m_state->completed.end());
					while (m_state->completed.size() >= m_state->limits.max_completed_entries)
					{
						m_state->completed.pop_front();
					}
					m_state->completed.push_back({pending->key_name, pending->identity});
				}
				m_state->async_stats.written++;
			} else
			{
				m_state->async_stats.failed++;
				m_state->async_stats.dropped++;
			}
		}
		m_state->queue_ready.notify_all();
	}
}

SpirvBinaryCacheStoreResult SpirvBinaryCacheStore::StoreEntry(const uint8_t* identity, size_t identity_size, uint32_t optimization,
                                                              bool validation_enabled, uint64_t key_low, uint64_t key_high,
                                                              const char* extension, const Vector<uint32_t>& binary)
{
	EXIT_IF(m_state == nullptr);
	EXIT_IF(identity == nullptr && identity_size != 0);
	EXIT_IF(extension == nullptr);
	if (binary.IsEmpty() || binary[0] != k_spirv_magic)
	{
		return SpirvBinaryCacheStoreResult::Failed;
	}

	const size_t payload_size = identity_size + static_cast<size_t>(binary.Size()) * sizeof(uint32_t);
	const size_t entry_size   = k_header_size + payload_size;
	if (entry_size > m_state->limits.max_entry_bytes || entry_size > m_state->limits.max_total_bytes)
	{
		return SpirvBinaryCacheStoreResult::TooLarge;
	}

	const XXH128_hash_t key {key_low, key_high};
	const auto          path = m_state->root / KeyName(key, extension);
	// All synchronous stores and the write-behind worker serialize here. Loads
	// intentionally do not: they only observe complete files published by the
	// temporary-file + atomic-replace protocol. On Windows, a replacement that
	// loses to an open reader fails without exposing a partial destination.
	std::lock_guard<std::mutex> lock(m_state->write_mutex);
	WriteHookForTesting hook = nullptr;
	void*               hook_opaque = nullptr;
	{
		std::lock_guard<std::mutex> hook_lock(m_state->hook_mutex);
		hook        = m_state->write_hook;
		hook_opaque = m_state->write_hook_opaque;
	}
	if (hook != nullptr)
	{
		hook(hook_opaque);
	}
	if (m_state->session_bytes_attempted > m_state->limits.session_write_budget ||
	    entry_size > m_state->limits.session_write_budget - m_state->session_bytes_attempted)
	{
		return SpirvBinaryCacheStoreResult::BudgetExceeded;
	}

	std::error_code error;
	std::filesystem::create_directories(m_state->root, error);
	if (error)
	{
		return SpirvBinaryCacheStoreResult::Failed;
	}

	size_t usage = CacheUsage(m_state->root);
	const auto existing_size = std::filesystem::file_size(path, error);
	if (!error && existing_size <= usage)
	{
		usage -= static_cast<size_t>(existing_size);
	}
	error.clear();

	struct Candidate
	{
		std::filesystem::path path;
		std::filesystem::file_time_type time;
		size_t size;
	};
	std::vector<Candidate> candidates;
	for (std::filesystem::directory_iterator it(m_state->root, error), end; !error && it != end; it.increment(error))
	{
		if (!it->is_regular_file(error) || error || !IsCacheEntry(it->path()) || it->path() == path)
		{
			error.clear();
			continue;
		}
		const auto size = it->file_size(error);
		const auto time = it->last_write_time(error);
		if (!error)
		{
			candidates.push_back({it->path(), time, static_cast<size_t>(size)});
		}
		error.clear();
	}
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
	{
		return a.time != b.time ? a.time < b.time : a.path.string() < b.path.string();
	});
	for (const auto& candidate: candidates)
	{
		if (usage <= m_state->limits.max_total_bytes - entry_size)
		{
			break;
		}
		std::filesystem::remove(candidate.path, error);
		if (!error && candidate.size <= usage)
		{
			usage -= candidate.size;
		}
		error.clear();
	}
	if (usage > m_state->limits.max_total_bytes - entry_size)
	{
		return SpirvBinaryCacheStoreResult::BudgetExceeded;
	}

	std::vector<uint8_t> data;
	data.reserve(entry_size);
	data.insert(data.end(), std::begin(k_magic), std::end(k_magic));
	AppendU32(&data, k_header_size);
	AppendU32(&data, k_format_version);
	AppendU32(&data, kSpirvBinaryCacheSchemaVersion);
	AppendU32(&data, k_target_vulkan_12);
	AppendU32(&data, optimization);
	AppendU32(&data, validation_enabled ? 1u : 0u);
	AppendU64(&data, identity_size);
	AppendU64(&data, binary.Size());
	AppendU64(&data, key.low64);
	AppendU64(&data, key.high64);
	AppendU64(&data, XXH3_64bits(binary.GetDataConst(), static_cast<size_t>(binary.Size()) * sizeof(uint32_t)));
	AppendU64(&data, payload_size);
	if (identity_size != 0)
	{
		data.insert(data.end(), identity, identity + identity_size);
	}
	for (uint32_t word: binary)
	{
		AppendU32(&data, word);
	}
	EXIT_IF(data.size() != entry_size);

	m_state->session_bytes_attempted += entry_size;
	auto temporary = path;
	temporary += "." + std::to_string(ProcessId()) + "." + std::to_string(m_state->temporary_sequence.fetch_add(1)) + ".tmp";
	{
		std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		if (!file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())) || !file.flush())
		{
			std::filesystem::remove(temporary, error);
			return SpirvBinaryCacheStoreResult::Failed;
		}
	}
	if (!ReplaceFile(temporary, path))
	{
		std::filesystem::remove(temporary, error);
		return SpirvBinaryCacheStoreResult::Failed;
	}
	return SpirvBinaryCacheStoreResult::Written;
}

size_t SpirvBinaryCacheStore::DiskUsageBytes() const
{
	EXIT_IF(m_state == nullptr);
	std::lock_guard<std::mutex> lock(m_state->write_mutex);
	return CacheUsage(m_state->root);
}

size_t SpirvBinaryCacheStore::SessionBytesAttempted() const
{
	EXIT_IF(m_state == nullptr);
	std::lock_guard<std::mutex> lock(m_state->write_mutex);
	return m_state->session_bytes_attempted;
}

SpirvBinaryCacheAsyncStats SpirvBinaryCacheStore::AsyncStats() const
{
	EXIT_IF(m_state == nullptr);
	std::lock_guard<std::mutex> lock(m_state->queue_mutex);
	auto stats              = m_state->async_stats;
	stats.pending_entries   = m_state->active.size();
	stats.pending_bytes     = m_state->pending_bytes;
	stats.completed_entries = m_state->completed.size();
	return stats;
}

void SpirvBinaryCacheStore::Drain()
{
	EXIT_IF(m_state == nullptr);
	std::unique_lock<std::mutex> lock(m_state->queue_mutex);
	m_state->queue_ready.wait(lock, [this] { return m_state->active.empty(); });
}

void SpirvBinaryCacheStore::SetWriteHookForTesting(WriteHookForTesting hook, void* opaque)
{
	EXIT_IF(m_state == nullptr);
	std::lock_guard<std::mutex> lock(m_state->hook_mutex);
	m_state->write_hook        = hook;
	m_state->write_hook_opaque = opaque;
}

std::filesystem::path SpirvBinaryCacheDefaultRoot()
{
	if (const char* explicit_path = std::getenv("KYTY_SPIRV_CACHE"); explicit_path != nullptr && explicit_path[0] != '\0')
	{
		return std::filesystem::path(explicit_path);
	}
#ifdef _WIN32
	if (const char* root = std::getenv("LOCALAPPDATA"); root != nullptr && root[0] != '\0')
	{
		return std::filesystem::path(root) / "Kyty" / "Cache" / "spirv-v1";
	}
#elif defined(__APPLE__)
	if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
	{
		return std::filesystem::path(home) / "Library" / "Caches" / "Kyty" / "spirv-v1";
	}
#else
	if (const char* root = std::getenv("XDG_CACHE_HOME"); root != nullptr && root[0] != '\0')
	{
		return std::filesystem::path(root) / "kyty" / "spirv-v1";
	}
	if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
	{
		return std::filesystem::path(home) / ".cache" / "kyty" / "spirv-v1";
	}
#endif
	std::error_code error;
	const auto      temp = std::filesystem::temp_directory_path(error);
	return error ? std::filesystem::path() : temp / "kyty" / "spirv-v1";
}

SpirvBinaryCacheStore& SpirvBinaryCacheDefaultStore()
{
	static SpirvBinaryCacheStore cache(SpirvBinaryCacheDefaultRoot());
	return cache;
}

} // namespace Kyty::Libs::Graphics

#endif
