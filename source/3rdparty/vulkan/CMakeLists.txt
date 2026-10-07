# Vulkan 1.4 is the minimum runtime and shader-toolchain contract.
# Use a matching SDK or development packages; the older bundled headers and
# SPIRV-Tools sources are not part of the active build.
if(DEFINED ENV{VULKAN_SDK})
    list(PREPEND CMAKE_PREFIX_PATH "$ENV{VULKAN_SDK}")
endif()

find_package(Vulkan 1.4 REQUIRED GLOBAL)
find_package(SPIRV-Tools CONFIG REQUIRED GLOBAL)
find_package(SPIRV-Tools-opt CONFIG REQUIRED GLOBAL)

add_library(vulkan-1 ALIAS Vulkan::Vulkan)

# SDK distributions differ in the core library's exported target name.
if(TARGET SPIRV-Tools-static)
    set(KYTY_SPIRV_TOOLS_TARGET SPIRV-Tools-static)
elseif(TARGET SPIRV-Tools)
    set(KYTY_SPIRV_TOOLS_TARGET SPIRV-Tools)
else()
    message(FATAL_ERROR "SPIRV-Tools must export SPIRV-Tools-static or SPIRV-Tools")
endif()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES ${KYTY_SPIRV_TOOLS_TARGET})
check_cxx_source_compiles("
#include <spirv-tools/libspirv.h>
int main() {
    auto context = spvContextCreate(SPV_ENV_VULKAN_1_4);
    spvContextDestroy(context);
    return 0;
}" KYTY_SPIRV_TOOLS_HAS_VULKAN_14)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT KYTY_SPIRV_TOOLS_HAS_VULKAN_14)
    message(FATAL_ERROR "SPIRV-Tools with Vulkan 1.4 support is required; install a Vulkan 1.4 SDK or newer development package")
endif()

add_library(spirv-tools INTERFACE)
target_link_libraries(spirv-tools INTERFACE ${KYTY_SPIRV_TOOLS_TARGET})
add_library(spirv-tools-opt INTERFACE)
target_link_libraries(spirv-tools-opt INTERFACE SPIRV-Tools-opt spirv-tools)
