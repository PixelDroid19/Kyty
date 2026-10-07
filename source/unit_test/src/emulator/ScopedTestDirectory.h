#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

// Own only a newly created directory under the caller's scratch cwd. Never
// remove a pre-existing path, including when concurrent/repeated tests collide.
class ScopedTestDirectory final
{
public:
	explicit ScopedTestDirectory(const char* prefix)
	{
		static std::atomic_uint64_t serial {0};
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		for (unsigned attempt = 0; attempt < 64; ++attempt)
		{
			const auto path = std::filesystem::current_path() /
			                  (std::string(prefix) + '-' + std::to_string(stamp) + '-' + std::to_string(serial.fetch_add(1)));
			std::error_code error;
			if (std::filesystem::create_directory(path, error))
			{
				m_path = path;
				break;
			}
			if (error && error != std::errc::file_exists)
			{
				break;
			}
		}
	}
	~ScopedTestDirectory()
	{
		if (!m_path.empty())
		{
			std::error_code error;
			std::filesystem::remove_all(m_path, error);
		}
	}
	ScopedTestDirectory(const ScopedTestDirectory&) = delete;
	ScopedTestDirectory& operator=(const ScopedTestDirectory&) = delete;
	[[nodiscard]] const std::filesystem::path& Path() const { return m_path; }

private:
	std::filesystem::path m_path;
};
