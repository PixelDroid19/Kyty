#ifndef EMULATOR_INCLUDE_EMULATOR_SYSTEMCONTENTPORT_H_
#define EMULATOR_INCLUDE_EMULATOR_SYSTEMCONTENTPORT_H_

#include "Kyty/Core/String.h"

#include <cstddef>

namespace Kyty::Emulator::SystemContentPort {

using GetMetadataFunction    = bool (*)(Core::String* title_id, Core::String* app_version);
using GetIconPathFunction    = bool (*)(Core::String* path);
using GetParamStringFunction = bool (*)(const char* name, char* value, size_t value_size);

struct Provider
{
	GetMetadataFunction    get_metadata     = nullptr;
	GetIconPathFunction    get_icon_path     = nullptr;
	GetParamStringFunction get_param_string = nullptr;
};

// The Loader composition root installs this provider once its guest metadata
// service is available. Graphics can then query title/icon data without
// importing Loader headers or its mutable runtime state.
void Install(const Provider& provider) noexcept;

[[nodiscard]] bool GetMetadata(Core::String* title_id, Core::String* app_version) noexcept;
[[nodiscard]] bool GetIconPath(Core::String* path) noexcept;
[[nodiscard]] bool GetParamString(const char* name, char* value, size_t value_size) noexcept;

} // namespace Kyty::Emulator::SystemContentPort

#endif // EMULATOR_INCLUDE_EMULATOR_SYSTEMCONTENTPORT_H_
