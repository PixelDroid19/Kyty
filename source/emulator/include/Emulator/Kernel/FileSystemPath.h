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

// Resolves `relative` below `root`, which must already be canonical, so only
// the components of `relative` touch the host filesystem. Each existing
// symlink is resolved; a dangling one is refused because the filename could
// still escape on a later O_CREAT. From the first missing component the rest
// is appended verbatim. The result may lie outside `root`; callers check
// containment.
inline std::filesystem::path CanonicalBelow(const std::filesystem::path& root, const std::filesystem::path& relative)
{
	if (root.empty() || relative.has_root_path()) { return {}; }
	auto result   = root;
	bool existing = true;
	for (const auto& component: relative)
	{
		if (component.empty() || component == ".") { continue; }
		if (component == "..")
		{
			// `result` is canonical while every component exists, so its
			// lexical parent is its physical parent.
			result   = result.parent_path();
			existing = true;
			continue;
		}
		result /= component;
		if (!existing) { continue; }
		std::error_code error;
		const auto      status = std::filesystem::symlink_status(result, error);
		if (error == std::errc::no_such_file_or_directory)
		{
			existing = false;
			continue;
		}
		if (error) { return {}; }
		if (!std::filesystem::is_symlink(status)) { continue; }
		result = std::filesystem::canonical(result, error);
		if (error) { return {}; }
	}
	return result;
}

inline std::filesystem::path Canonical(const std::filesystem::path& path)
{
	if (path.empty()) { return {}; }
	std::error_code error;
	const auto      absolute = std::filesystem::absolute(path, error);
	if (error) { return {}; }
	return CanonicalBelow(absolute.root_path(), absolute.relative_path());
}

inline bool Contains(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
	if (root.empty() || candidate.empty()) { return false; }
	return std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end()).first == root.end();
}

// `canonical_root` must come from Canonical().
inline std::string ResolveContained(const std::filesystem::path& canonical_root, const std::string& relative)
{
	const auto suffix = std::filesystem::u8path(relative);
	if (suffix.has_root_path() || relative.find('\0') != std::string::npos) { return {}; }
	const auto candidate = CanonicalBelow(canonical_root, suffix);
	return Contains(canonical_root, candidate) ? candidate.u8string() : std::string();
}

// Canonical form of a host path. When it lies lexically below one of the
// canonical `roots`, only the components below that root are resolved.
inline std::filesystem::path CanonicalFrom(const std::vector<std::filesystem::path>& roots, const std::filesystem::path& path)
{
	for (const auto& root: roots)
	{
		if (Contains(root, path)) { return CanonicalBelow(root, path.lexically_relative(root)); }
	}
	return Canonical(path);
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

} // namespace Kyty::Kernel::FileSystem::Path

#endif // EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEMPATH_H_
