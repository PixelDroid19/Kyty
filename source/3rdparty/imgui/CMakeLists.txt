# Dear ImGui v1.91.8 (MIT) — core + Vulkan/SDL2 backends for the host debug HUD.
set(imgui_src
	imgui.cpp
	imgui_draw.cpp
	imgui_tables.cpp
	imgui_widgets.cpp
	backends/imgui_impl_vulkan.cpp
	backends/imgui_impl_sdl2.cpp
)

add_library(imgui STATIC ${imgui_src})

target_include_directories(imgui PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/backends"
	"${CMAKE_SOURCE_DIR}/3rdparty/vulkan/include"
	"${CMAKE_SOURCE_DIR}/3rdparty/sdl2/sdl2/include"
)

# Silence noisy third-party warnings; do not alter guest semantics.
if (CLANG OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
	target_compile_options(imgui PRIVATE
		-Wno-unused-parameter
		-Wno-unused-function
		-Wno-missing-field-initializers
	)
endif()

target_compile_definitions(imgui PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
