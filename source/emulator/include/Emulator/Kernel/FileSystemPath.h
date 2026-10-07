#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEMPATH_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEMPATH_H_

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace Kyty::Kernel::FileSystem::Path {

// Resolve guest components before choosing a mount. Never pass a guest '..'
// component to the host filesystem, including after a backslash separator.
inline std::string NormalizeGuest(std::string path)
{
	if (path.empty() || path.find('\0') != std::string::npos) { return {}; }
	std::replace(path.begin(), path.end(), '\\', '/');
	std::vector<std::string> components;
	for (size_t first = 0; first < path.size();)
	{
		const auto end = path.find('/', first);
		const auto component = path.substr(first, end == std::string::npos ? end : end - first);
		first = end == std::string::npos ? path.size() : end + 1;
		if (component.empty() || component == ".") { continue; }
		if (component == "..")
		{
			if (components.empty()) { return {}; }
			components.pop_back();
			continue;
		}
		components.push_back(component);
	}
	std::string result = "/";
	for (const auto& component: components)
	{
		if (result.size() > 1) { result += '/'; }
		result += component;
	}
	return result;
}

inline std::filesystem::path Canonical(const std::filesystem::path& path)
{
	if (path.empty()) { return {}; }
	// weakly_canonical alone leaves a dangling symlink unresolved. Such a
	// filename can still escape on a later O_CREAT, so reject it explicitly.
	std::filesystem::path prefix;
	for (const auto& component: path)
	{
		prefix /= component;
		std::error_code error;
		const auto status = std::filesystem::symlink_status(prefix, error);
		if (error == std::errc::no_such_file_or_directory) { break; }
		if (error) { return {}; }
		if (std::filesystem::is_symlink(status))
		{
			(void)std::filesystem::canonical(prefix, error);
			if (error) { return {}; }
		}
	}
	std::error_code error;
	auto result = std::filesystem::weakly_canonical(path, error);
	if (error || !result.is_absolute()) { return {}; }
	if (result != result.root_path() && result.filename().empty()) { result = result.parent_path(); }
	return result;
}

inline bool Contains(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
	if (root.empty() || candidate.empty()) { return false; }
	return std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end()).first == root.end();
}

inline std::string ResolveContained(const std::string& root, const std::string& relative)
{
	const auto suffix = std::filesystem::u8path(relative);
	if (suffix.has_root_path() || relative.find('\0') != std::string::npos) { return {}; }
	const auto base = Canonical(std::filesystem::u8path(root));
	if (base.empty()) { return {}; }
	const auto candidate = Canonical(base / suffix);
	return Contains(base, candidate) ? candidate.u8string() : std::string();
}

inline bool EqualsIgnoringAsciiCase(const std::string& a, const std::string& b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y)
	                                          { return std::tolower(x) == std::tolower(y); });
}

// The host entry of `directory` named `name`: the exact entry, otherwise the
// only entry equal to it ignoring ASCII case. Empty when there is none or when
// several entries differ only in case (the host copy then has no guest image).
inline std::string FindEntryIgnoringCase(const std::filesystem::path& directory, const std::string& name)
{
	std::error_code error;
	if (std::filesystem::exists(directory / std::filesystem::u8path(name), error)) { return name; }
	std::string found;
	for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
	{
		const auto entry = it->path().filename().u8string();
		if (!EqualsIgnoringAsciiCase(entry, name)) { continue; }
		if (!found.empty()) { return {}; }
		found = entry;
	}
	return found;
}

// Guest filesystems match names ignoring case: a title opens
// "Il2CppUserAssemblies.prx" that its package stores as
// "Il2cppUserAssemblies.prx". Spells each component of `relative` as the host
// entry below `root` it names; from the first component without one the rest
// stays verbatim, so creation and ENOENT follow the guest.
inline std::string MatchCaseInsensitive(const std::string& root, const std::string& relative)
{
	std::error_code error;
	const auto      base = std::filesystem::u8path(root);
	if (relative.empty() || std::filesystem::exists(base / std::filesystem::u8path(relative), error)) { return relative; }
	std::string           result;
	std::filesystem::path directory = base;
	bool                  matching  = true;
	for (const auto& component: std::filesystem::u8path(relative))
	{
		auto name = component.u8string();
		if (matching)
		{
			const auto entry = FindEntryIgnoringCase(directory, name);
			matching         = !entry.empty();
			name             = matching ? entry : name;
			directory /= std::filesystem::u8path(name);
		}
		result += (result.empty() ? "" : "/") + name;
	}
	return result;
}

inline bool Allows(const std::string& root, const std::string& candidate)
{
	return Contains(Canonical(std::filesystem::u8path(root)), Canonical(std::filesystem::u8path(candidate)));
}

} // namespace Kyty::Kernel::FileSystem::Path

#endif // EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEMPATH_H_
