#include "Emulator/Graphics/ShaderTranslationCache.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/SpirvBinaryCacheStore.h"

#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

ShaderModuleKey ShaderModuleKey::Create(const ShaderId& shader_id, ShaderModuleStage stage, Config::ShaderOptimizationType optimization,
                                        bool next_gen, bool debug_printf_enabled)
{
	ShaderModuleKey key;
	key.shader_id          = shader_id;
	key.stage              = stage;
	key.optimization       = optimization;
	key.next_gen           = next_gen;
	key.debug_printf_enabled = debug_printf_enabled;
	key.translator_version = kShaderTranslatorVersion;
	return key;
}

bool ShaderModuleKey::operator==(const ShaderModuleKey& other) const
{
	return shader_id == other.shader_id && stage == other.stage && optimization == other.optimization && next_gen == other.next_gen &&
	       debug_printf_enabled == other.debug_printf_enabled && translator_version == other.translator_version;
}

namespace {

size_t HashCombine(size_t seed, size_t value)
{
	return seed ^ (value + static_cast<size_t>(0x9e3779b9u) + (seed << 6u) + (seed >> 2u));
}

struct ShaderModuleKeyHash
{
	size_t operator()(const ShaderModuleKey& key) const
	{
		size_t hash = key.shader_id.hash0;
		hash        = HashCombine(hash, key.shader_id.crc32);
		for (uint32_t id: key.shader_id.ids)
		{
			hash = HashCombine(hash, id);
		}
		hash = HashCombine(hash, static_cast<size_t>(key.stage));
		hash = HashCombine(hash, static_cast<size_t>(key.optimization));
		hash = HashCombine(hash, key.next_gen ? 1u : 0u);
		hash = HashCombine(hash, key.debug_printf_enabled ? 1u : 0u);
		return HashCombine(hash, key.translator_version);
	}
};

struct CacheEntry
{
	Vector<uint32_t>       binary;
	bool                   compiling = true;
	uint64_t               last_use  = 0;
	std::condition_variable ready;
};

} // namespace

struct ShaderTranslationCache::State
{
	State(size_t limit, SpirvBinaryCacheStore* store, bool validation)
	    : max_entries(limit == 0 ? 1 : limit), persistent_store(store), validation_enabled(validation)
	{
	}

	size_t                      max_entries = 1;
	SpirvBinaryCacheStore*      persistent_store = nullptr;
	bool                        validation_enabled = false;
	uint64_t                    use_seq     = 0;
	mutable std::mutex          mutex;
	std::condition_variable     capacity_changed;
	std::unordered_map<ShaderModuleKey, std::shared_ptr<CacheEntry>, ShaderModuleKeyHash> entries;
};

ShaderTranslationCache::ShaderTranslationCache(size_t max_entries, SpirvBinaryCacheStore* persistent_store, bool validation_enabled)
    : m_state(new State(max_entries, persistent_store, validation_enabled))
{
}

ShaderTranslationCache::~ShaderTranslationCache()
{
	delete m_state;
	m_state = nullptr;
}

ShaderTranslationCacheResult ShaderTranslationCache::GetOrCompile(const ShaderModuleKey& key, const Compiler& compiler)
{
	EXIT_IF(m_state == nullptr);
	EXIT_IF(!compiler);

	std::shared_ptr<CacheEntry> entry;
	bool                        evicted = false;
	{
		std::unique_lock<std::mutex> lock(m_state->mutex);
		for (;;)
		{
			if (const auto found = m_state->entries.find(key); found != m_state->entries.end())
			{
				entry = found->second;
				while (entry->compiling)
				{
					entry->ready.wait(lock);
				}
				entry->last_use = ++m_state->use_seq;
				return {entry->binary, true};
			}

			if (m_state->entries.size() >= m_state->max_entries)
			{
				auto     evict      = m_state->entries.end();
				uint64_t oldest_use = std::numeric_limits<uint64_t>::max();
				for (auto candidate = m_state->entries.begin(); candidate != m_state->entries.end(); ++candidate)
				{
					if (!candidate->second->compiling && candidate->second->last_use < oldest_use)
					{
						evict      = candidate;
						oldest_use = candidate->second->last_use;
					}
				}
				if (evict != m_state->entries.end())
				{
					m_state->entries.erase(evict);
					evicted = true;
				} else
				{
					m_state->capacity_changed.wait(lock);
					continue;
				}
			}

			entry           = std::make_shared<CacheEntry>();
			entry->last_use = ++m_state->use_seq;
			m_state->entries.emplace(key, entry);
			break;
		}
	}

	Vector<uint32_t> binary;
	const bool       persistent_eligible = m_state->persistent_store != nullptr && !key.debug_printf_enabled;
	if (persistent_eligible &&
	    m_state->persistent_store->LoadModule(key, m_state->validation_enabled, &binary) == SpirvBinaryCacheLoadResult::Hit)
	{
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			entry->binary    = binary;
			entry->compiling = false;
			entry->last_use  = ++m_state->use_seq;
		}
		entry->ready.notify_all();
		m_state->capacity_changed.notify_all();
		return {binary, true, evicted};
	}

	binary = compiler();
	if (!binary.IsEmpty() && persistent_eligible)
	{
		(void)m_state->persistent_store->QueueStoreModule(key, m_state->validation_enabled, binary);
	}
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		entry->binary    = binary;
		entry->compiling = false;
		entry->last_use  = ++m_state->use_seq;
	}
	entry->ready.notify_all();
	m_state->capacity_changed.notify_all();
	return {binary, false, evicted};
}

size_t ShaderTranslationCache::Size() const
{
	EXIT_IF(m_state == nullptr);
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return m_state->entries.size();
}

} // namespace Kyty::Libs::Graphics

#endif
