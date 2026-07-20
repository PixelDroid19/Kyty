#include "Emulator/Graphics/SpirvBinaryCacheStore.h"

#include "Kyty/Core/DbgAssert.h"

#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <system_error>
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

std::string KeyName(XXH128_hash_t key)
{
	std::ostringstream name;
	name << std::hex << std::setfill('0') << std::setw(16) << key.high64 << std::setw(16) << key.low64 << ".spvbin";
	return name.str();
}

size_t CacheUsage(const std::filesystem::path& root)
{
	std::error_code error;
	size_t          total = 0;
	for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
	{
		if (!it->is_regular_file(error) || error || it->path().extension() != ".spvbin")
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
	std::filesystem::path root;
	SpirvBinaryCacheLimits limits;
	size_t                 session_bytes_attempted = 0;
	mutable std::mutex     mutex;
	std::atomic<uint64_t>  temporary_sequence {0};
};

SpirvBinaryCacheStore::SpirvBinaryCacheStore(std::filesystem::path root, SpirvBinaryCacheLimits limits): m_state(new State)
{
	m_state->root   = std::move(root);
	m_state->limits = limits;
}

SpirvBinaryCacheStore::~SpirvBinaryCacheStore()
{
	delete m_state;
	m_state = nullptr;
}

SpirvBinaryCacheLoadResult SpirvBinaryCacheStore::Load(const String8& source, uint32_t optimization, bool validation_enabled,
                                                       Vector<uint32_t>* binary)
{
	EXIT_IF(m_state == nullptr);
	EXIT_IF(binary == nullptr);
	binary->Clear();

	const auto key  = SourceKey(source, optimization, validation_enabled);
	const auto path = m_state->root / KeyName(key);
	std::lock_guard<std::mutex> lock(m_state->mutex);

	std::error_code error;
	const auto      file_size = std::filesystem::file_size(path, error);
	if (error)
	{
		return SpirvBinaryCacheLoadResult::Miss;
	}
	if (file_size < k_header_size || file_size > m_state->limits.max_entry_bytes)
	{
		return SpirvBinaryCacheLoadResult::Corrupt;
	}

	std::vector<uint8_t> data(static_cast<size_t>(file_size));
	std::ifstream        file(path, std::ios::binary);
	if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
	{
		return SpirvBinaryCacheLoadResult::Corrupt;
	}

	const uint8_t* header = data.data();
	if (memcmp(header, k_magic, sizeof(k_magic)) != 0 || ReadU32(header + 8) != k_header_size ||
	    ReadU32(header + 12) != k_format_version || ReadU32(header + 16) != kSpirvBinaryCacheSchemaVersion ||
	    ReadU32(header + 20) != k_target_vulkan_12 || ReadU32(header + 24) != optimization ||
	    ReadU32(header + 28) != (validation_enabled ? 1u : 0u))
	{
		return SpirvBinaryCacheLoadResult::Corrupt;
	}

	const uint64_t source_size = ReadU64(header + 32);
	const uint64_t word_count  = ReadU64(header + 40);
	const uint64_t payload_size = ReadU64(header + 72);
	if (source_size != source.Size() || word_count == 0 || word_count > (m_state->limits.max_entry_bytes / sizeof(uint32_t)) ||
	    source_size > m_state->limits.max_entry_bytes || word_count > (UINT64_MAX - source_size) / sizeof(uint32_t) ||
	    payload_size != source_size + word_count * sizeof(uint32_t) || payload_size > UINT64_MAX - k_header_size ||
	    file_size != k_header_size + payload_size || ReadU64(header + 48) != key.low64 || ReadU64(header + 56) != key.high64)
	{
		return SpirvBinaryCacheLoadResult::Corrupt;
	}

	const uint8_t* stored_source = data.data() + k_header_size;
	const uint8_t* stored_binary = stored_source + source_size;
	if (memcmp(stored_source, source.GetDataConst(), source.Size()) != 0 ||
	    ReadU64(header + 64) != XXH3_64bits(stored_binary, static_cast<size_t>(word_count * sizeof(uint32_t))) ||
	    ReadU32(stored_binary) != k_spirv_magic)
	{
		return SpirvBinaryCacheLoadResult::Corrupt;
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
	EXIT_IF(m_state == nullptr);
	if (binary.IsEmpty() || binary[0] != k_spirv_magic)
	{
		return SpirvBinaryCacheStoreResult::Failed;
	}

	const size_t payload_size = static_cast<size_t>(source.Size()) + static_cast<size_t>(binary.Size()) * sizeof(uint32_t);
	const size_t entry_size   = k_header_size + payload_size;
	if (entry_size > m_state->limits.max_entry_bytes || entry_size > m_state->limits.max_total_bytes)
	{
		return SpirvBinaryCacheStoreResult::TooLarge;
	}

	const auto key  = SourceKey(source, optimization, validation_enabled);
	const auto path = m_state->root / KeyName(key);
	std::lock_guard<std::mutex> lock(m_state->mutex);
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

	struct Candidate
	{
		std::filesystem::path path;
		std::filesystem::file_time_type time;
		size_t size;
	};
	std::vector<Candidate> candidates;
	for (std::filesystem::directory_iterator it(m_state->root, error), end; !error && it != end; it.increment(error))
	{
		if (!it->is_regular_file(error) || error || it->path().extension() != ".spvbin" || it->path() == path)
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
	AppendU64(&data, source.Size());
	AppendU64(&data, binary.Size());
	AppendU64(&data, key.low64);
	AppendU64(&data, key.high64);
	AppendU64(&data, XXH3_64bits(binary.GetDataConst(), static_cast<size_t>(binary.Size()) * sizeof(uint32_t)));
	AppendU64(&data, payload_size);
	data.insert(data.end(), source.GetDataConst(), source.GetDataConst() + source.Size());
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
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return CacheUsage(m_state->root);
}

size_t SpirvBinaryCacheStore::SessionBytesAttempted() const
{
	EXIT_IF(m_state == nullptr);
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return m_state->session_bytes_attempted;
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
