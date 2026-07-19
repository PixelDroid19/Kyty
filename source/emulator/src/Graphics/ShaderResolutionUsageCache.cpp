#include "Emulator/Graphics/ShaderResolutionUsageCache.h"

#include "Kyty/Core/DbgAssert.h"

#include <limits>
#include <mutex>
#include <unordered_map>

namespace Kyty::Libs::Graphics {

bool ShaderResolutionUsageKey::operator==(const ShaderResolutionUsageKey& other) const
{
	return address == other.address && checksum == other.checksum && translator_version == other.translator_version;
}

namespace {
struct KeyHash
{
	size_t operator()(const ShaderResolutionUsageKey& key) const
	{
		size_t hash = std::hash<uint64_t> {}(key.address);
		hash ^= std::hash<uint64_t> {}(key.checksum) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
		return hash ^ (static_cast<size_t>(key.translator_version) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u));
	}
};
struct Entry
{
	ShaderResolutionAnalysis analysis;
	uint64_t                 last_use = 0;
};
} // namespace

struct ShaderResolutionUsageCache::State
{
	explicit State(size_t limit): max_entries(limit == 0 ? 1 : limit) {}
	size_t max_entries;
	uint64_t use_seq = 0;
	std::mutex mutex;
	std::unordered_map<ShaderResolutionUsageKey, Entry, KeyHash> entries;
};

ShaderResolutionUsageCache::ShaderResolutionUsageCache(size_t max_entries): m_state(new State(max_entries)) {}
ShaderResolutionUsageCache::~ShaderResolutionUsageCache()
{
	delete m_state;
}

ShaderResolutionUsageResult ShaderResolutionUsageCache::GetOrAnalyze(const ShaderResolutionUsageKey& key, const Analyzer& analyzer)
{
	EXIT_IF(m_state == nullptr || !analyzer);
	std::lock_guard<std::mutex> lock(m_state->mutex);
	if (auto found = m_state->entries.find(key); found != m_state->entries.end())
	{
		found->second.last_use = ++m_state->use_seq;
		return {found->second.analysis.usage, found->second.analysis.code, true, false};
	}

	bool evicted = false;
	if (m_state->entries.size() >= m_state->max_entries)
	{
		auto oldest = m_state->entries.end();
		for (auto it = m_state->entries.begin(); it != m_state->entries.end(); ++it)
		{
			if (oldest == m_state->entries.end() || it->second.last_use < oldest->second.last_use)
			{
				oldest = it;
			}
		}
		m_state->entries.erase(oldest);
		evicted = true;
	}
	const auto analysis = analyzer();
	m_state->entries.emplace(key, Entry {analysis, ++m_state->use_seq});
	return {analysis.usage, analysis.code, false, evicted};
}

} // namespace Kyty::Libs::Graphics
