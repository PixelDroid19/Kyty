#include "Emulator/Graphics/Objects/DepthMeta.h"

#include <mutex>
#include <unordered_set>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

std::mutex                   g_mutex;
std::unordered_set<uint64_t> g_cleared;

} // namespace

bool DepthMetaIsClearPattern(const void* data, uint64_t size)
{
	if (data == nullptr || size == 0 || (size % sizeof(uint32_t)) != 0)
	{
		return false;
	}

	const auto* words = static_cast<const uint32_t*>(data);
	const auto  count = size / sizeof(uint32_t);
	for (uint64_t i = 0; i < count; i++)
	{
		if (words[i] != 0xfffffff0u)
		{
			return false;
		}
	}
	return true;
}

bool DepthMetaMatchesStorageRange(uint64_t storage_address, uint64_t storage_size, uint64_t htile_address, uint64_t htile_size)
{
	return storage_address != 0 && htile_address != 0 && storage_size != 0 && storage_address == htile_address &&
	       storage_size == htile_size;
}

void DepthMetaMarkClear(uint64_t address)
{
	if (address == 0)
	{
		return;
	}
	std::lock_guard lock(g_mutex);
	g_cleared.insert(address);
}

bool DepthMetaConsumeClear(uint64_t address)
{
	std::lock_guard lock(g_mutex);
	return g_cleared.erase(address) != 0;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
