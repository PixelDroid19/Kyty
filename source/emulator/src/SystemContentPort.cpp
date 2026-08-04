#include "Emulator/SystemContentPort.h"

#include <atomic>

namespace Kyty::Emulator::SystemContentPort {
namespace {

bool EmptyMetadata(Core::String*, Core::String*)
{
	return false;
}

bool EmptyIconPath(Core::String*)
{
	return false;
}

bool EmptyParamString(const char*, char*, size_t)
{
	return false;
}

std::atomic<GetMetadataFunction>    g_get_metadata {EmptyMetadata};
std::atomic<GetIconPathFunction>    g_get_icon_path {EmptyIconPath};
std::atomic<GetParamStringFunction> g_get_param_string {EmptyParamString};

} // namespace

void Install(const Provider& provider) noexcept
{
	g_get_metadata.store(provider.get_metadata != nullptr ? provider.get_metadata : EmptyMetadata, std::memory_order_release);
	g_get_icon_path.store(provider.get_icon_path != nullptr ? provider.get_icon_path : EmptyIconPath, std::memory_order_release);
	g_get_param_string.store(provider.get_param_string != nullptr ? provider.get_param_string : EmptyParamString,
	                         std::memory_order_release);
}

bool GetMetadata(Core::String* title_id, Core::String* app_version) noexcept
{
	return g_get_metadata.load(std::memory_order_acquire)(title_id, app_version);
}

bool GetIconPath(Core::String* path) noexcept
{
	return g_get_icon_path.load(std::memory_order_acquire)(path);
}

bool GetParamString(const char* name, char* value, size_t value_size) noexcept
{
	return g_get_param_string.load(std::memory_order_acquire)(name, value, value_size);
}

} // namespace Kyty::Emulator::SystemContentPort
