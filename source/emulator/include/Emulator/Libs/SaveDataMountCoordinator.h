#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMOUNTCOORDINATOR_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMOUNTCOORDINATOR_H_

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace Kyty::Libs::SaveData {

class SaveDataMountCoordinator final
{
public:
	static constexpr size_t SlotCount = 16;

	enum class AcquireResult
	{
		Available,
		AlreadyMounted,
		Full,
	};

	struct Acquisition
	{
		AcquireResult result;
		size_t        slot;
	};

	[[nodiscard]] Acquisition Acquire(std::string_view directory_name) const
	{
		size_t available = SlotCount;
		for (size_t slot = 0; slot < m_directories.size(); ++slot)
		{
			const auto& directory = m_directories[slot];
			if (directory.has_value() && *directory == directory_name)
			{
				return {AcquireResult::AlreadyMounted, slot};
			}
			if (!directory.has_value() && available == SlotCount)
			{
				available = slot;
			}
		}
		return available == SlotCount ? Acquisition {AcquireResult::Full, SlotCount}
		                              : Acquisition {AcquireResult::Available, available};
	}

	void Commit(size_t slot, std::string_view directory_name)
	{
		if (slot < m_directories.size())
		{
			m_directories[slot] = directory_name;
		}
	}

	[[nodiscard]] std::optional<size_t> Find(std::string_view mount_point) const
	{
		for (size_t slot = 0; slot < m_directories.size(); ++slot)
		{
			if (m_directories[slot].has_value() && MountPoint(slot) == mount_point)
			{
				return slot;
			}
		}
		return std::nullopt;
	}

	void Release(size_t slot)
	{
		if (slot < m_directories.size())
		{
			m_directories[slot].reset();
		}
	}

	[[nodiscard]] bool Empty() const
	{
		for (const auto& directory: m_directories)
		{
			if (directory.has_value())
			{
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] static std::string MountPoint(size_t slot)
	{
		return "/savedata" + std::to_string(slot);
	}

private:
	std::array<std::optional<std::string>, SlotCount> m_directories {};
};

} // namespace Kyty::Libs::SaveData

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMOUNTCOORDINATOR_H_
