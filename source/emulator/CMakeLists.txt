file(GLOB emulator_src CONFIGURE_DEPENDS
	src/*.cpp
	src/Hle/*.cpp
	src/Graphics/*.cpp
	src/Graphics/Objects/*.cpp
	src/Kernel/*.cpp
	src/Loader/*.cpp
	include/*.h
)

# Every domain implementation lives in its own static archive; the emulator
# target is the composition root (host entry points, agent server, profiler
# wrapper) plus the interface headers. The archive boundaries below are the
# CMake-side projection of the domain boundaries enforced by
# scripts/check_emulator_boundaries.py.
file(GLOB kernel_src CONFIGURE_DEPENDS src/Kernel/*.cpp)
file(GLOB hle_src CONFIGURE_DEPENDS src/Libs/*.cpp)
file(GLOB loader_src CONFIGURE_DEPENDS src/Loader/*.cpp)
file(GLOB host_src CONFIGURE_DEPENDS src/Host/*.cpp)
file(GLOB graphics_src CONFIGURE_DEPENDS src/Graphics/*.cpp src/Graphics/Objects/*.cpp)
file(GLOB diagnostics_src CONFIGURE_DEPENDS src/Agent/EventRing.cpp src/Agent/AgentLifecycle.cpp)
file(GLOB hle_domain_src CONFIGURE_DEPENDS
	src/Network.cpp
	src/NetworkHttpUri.cpp
	src/Audio.cpp
	src/Audio3d.cpp
	src/AudioAjm.cpp
	src/AudioAvPlayer.cpp
	src/AudioHost.cpp
	src/AudioNgs2.cpp
	src/Dialog.cpp
	src/Controller.cpp
)
file(GLOB runtime_core_src CONFIGURE_DEPENDS
	src/Log.cpp
	src/Config.cpp
	src/GuestRuntimePort.cpp
	src/GuestMemory.cpp
	src/GpuMemoryFault.cpp
	src/AtomicFile.cpp
	src/Validation/DomainValidators.cpp
	src/SystemContentPort.cpp
	src/PresentationStats.cpp
	src/VideoFrameMemory.cpp
	src/Ports/*.cpp
)

list(REMOVE_ITEM emulator_src
	${kernel_src}
	${hle_src}
	${loader_src}
	${host_src}
	${graphics_src}
	${hle_domain_src}
	${runtime_core_src}
	${diagnostics_src}
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Hle/LibGraphicsDriver.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Hle/LibVideoOut.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Hle/Registration.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/AudioVideoBackend.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/AudioPcm.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/SystemContentPort.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/VideoFrameMemory.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/PresentationStats.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Kyty.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Profiler.cpp"
)

if (MSVC AND CLANG)
	set_source_files_properties(${emulator_src} ${hle_src} ${hle_domain_src} ${runtime_core_src} PROPERTIES COMPILE_FLAGS "-Wno-pragma-pack -Wno-deprecated-declarations -D_TIMESPEC_DEFINED")
endif()

add_library(kyty_runtime_core STATIC ${runtime_core_src})
add_library(kyty_kernel STATIC ${kernel_src})
add_library(kyty_diagnostics STATIC ${diagnostics_src})
add_library(kyty_loader STATIC ${loader_src})
add_library(kyty_host STATIC ${host_src})
add_library(kyty_graphics STATIC
	${graphics_src}
	src/Hle/LibGraphicsDriver.cpp
	src/Hle/LibVideoOut.cpp
	src/Hle/Registration.cpp
)
add_library(kyty_hle STATIC ${hle_src} ${hle_domain_src})
add_library(kyty_audio_video_backend STATIC src/AudioVideoBackend.cpp)
add_library(emulator STATIC ${emulator_src})

target_sources(kyty_host PRIVATE
	$<TARGET_OBJECTS:imgui_sdl_backend_obj>
)
target_sources(emulator PRIVATE
	include/Emulator/Profiling.h
	src/Agent/DebugSnapshot.cpp
	src/Agent/Protocol.cpp
	src/Agent/StallWatch.cpp
	src/Agent/FrameScore.cpp
	src/Agent/AgentServer.cpp
	src/Agent/AgentSubsystem.cpp
	include/Emulator/Kernel/Errors.h
	include/Emulator/Kernel/Trace.h
	include/Emulator/Agent/EventRing.h
	include/Emulator/Agent/DebugSnapshot.h
	include/Emulator/Agent/Protocol.h
	include/Emulator/Agent/StallWatch.h
	include/Emulator/Agent/FrameScore.h
	include/Emulator/Agent/AgentServer.h
	include/Emulator/Agent/AgentSubsystem.h
	include/Emulator/Agent/AgentLifecycle.h
	include/Emulator/Libs/ProcessEnvironment.h
)
add_dependencies(emulator KytyBuildInfo)
add_dependencies(kyty_graphics KytyBuildInfo)

# Fail at configure time if a future glob or explicit source entry collapses
# a domain back into the wrong archive.
get_target_property(_emulator_sources emulator SOURCES)
foreach(_emulator_source IN LISTS _emulator_sources)
	if(_emulator_source MATCHES "(^|/)src/Kernel/")
		message(FATAL_ERROR "Kernel source leaked into emulator target: ${_emulator_source}")
	endif()
	if(_emulator_source MATCHES "(^|/)src/Libs/")
		message(FATAL_ERROR "HLE source leaked into emulator target: ${_emulator_source}")
	endif()
	if(_emulator_source MATCHES "(^|/)src/Graphics/")
		message(FATAL_ERROR "Graphics source leaked into emulator target: ${_emulator_source}")
	endif()
	if(_emulator_source MATCHES "(^|/)src/Loader/")
		message(FATAL_ERROR "Loader source leaked into emulator target: ${_emulator_source}")
	endif()
	if(_emulator_source MATCHES "(^|/)src/Host/")
		message(FATAL_ERROR "Host source leaked into emulator target: ${_emulator_source}")
	endif()
endforeach()
get_target_property(_kernel_sources kyty_kernel SOURCES)
if(NOT _kernel_sources)
	message(FATAL_ERROR "kyty_kernel target has no implementation sources")
endif()
get_target_property(_kernel_links kyty_kernel LINK_LIBRARIES)
if(_kernel_links AND "scripts" IN_LIST _kernel_links)
	message(FATAL_ERROR "kyty_kernel must not inherit the script host archive")
endif()
if(_kernel_links AND "kyty_hle" IN_LIST _kernel_links)
	message(FATAL_ERROR "kyty_kernel must not link the HLE archive")
endif()
get_target_property(_hle_sources kyty_hle SOURCES)
if(NOT _hle_sources)
	message(FATAL_ERROR "kyty_hle target has no implementation sources")
endif()
get_target_property(_hle_links kyty_hle LINK_LIBRARIES)
if(_hle_links AND "scripts" IN_LIST _hle_links)
	message(FATAL_ERROR "kyty_hle must not link the script host archive directly")
endif()
get_target_property(_loader_sources kyty_loader SOURCES)
if(NOT _loader_sources)
	message(FATAL_ERROR "kyty_loader target has no implementation sources")
endif()
get_target_property(_loader_links kyty_loader LINK_LIBRARIES)
if(_loader_links AND "kyty_hle" IN_LIST _loader_links)
	message(FATAL_ERROR "kyty_loader must not link the HLE archive")
endif()
get_target_property(_graphics_sources kyty_graphics SOURCES)
if(NOT _graphics_sources)
	message(FATAL_ERROR "kyty_graphics target has no implementation sources")
endif()
get_target_property(_graphics_links kyty_graphics LINK_LIBRARIES)
if(_graphics_links AND "kyty_hle" IN_LIST _graphics_links)
	message(FATAL_ERROR "kyty_graphics must not link the HLE archive")
endif()
get_target_property(_host_links kyty_host LINK_LIBRARIES)
if(_host_links AND "kyty_graphics" IN_LIST _host_links)
	message(FATAL_ERROR "kyty_host must not link the graphics archive")
endif()
if(_host_links AND "kyty_hle" IN_LIST _host_links)
	message(FATAL_ERROR "kyty_host must not link the HLE archive")
endif()
get_target_property(_core_links kyty_runtime_core LINK_LIBRARIES)
if(_core_links AND "kyty_kernel" IN_LIST _core_links)
	message(FATAL_ERROR "kyty_runtime_core must not link the kernel archive")
endif()
if(_core_links AND "kyty_hle" IN_LIST _core_links)
	message(FATAL_ERROR "kyty_runtime_core must not link the HLE archive")
endif()

target_include_directories(kyty_runtime_core PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(kyty_runtime_core PUBLIC core math sys)
if(MINGW)
	target_link_libraries(kyty_runtime_core PUBLIC atomic)
endif()

target_include_directories(kyty_kernel PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(kyty_kernel PUBLIC core math sys kyty_runtime_core)

target_include_directories(kyty_diagnostics PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(kyty_diagnostics PUBLIC core sys kyty_runtime_core)

target_include_directories(kyty_loader PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(kyty_loader PUBLIC core math sys kyty_runtime_core kyty_kernel kyty_diagnostics easy_profiler)

target_include_directories(kyty_host PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(kyty_host PUBLIC core sys kyty_runtime_core SDL2-static vulkan-1 imgui)

target_include_directories(kyty_graphics PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(kyty_graphics PUBLIC core math sys kyty_runtime_core kyty_kernel kyty_host kyty_diagnostics
	vulkan-1 spirv-tools-opt spirv-tools easy_profiler imgui SDL2-static)

target_include_directories(kyty_hle PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(kyty_hle PUBLIC core math sys kyty_runtime_core kyty_kernel kyty_loader kyty_graphics
	kyty_host kyty_diagnostics kyty_audio_video_backend kyty_audio_pcm)

target_include_directories(kyty_audio_video_backend PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(kyty_audio_video_backend PUBLIC core)

if(MSVC OR MINGW)
	target_link_libraries(emulator PRIVATE psapi)
endif()

target_include_directories(emulator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(emulator
	PUBLIC
	kyty_hle
	kyty_loader
	kyty_graphics
	kyty_host
	kyty_kernel
	kyty_runtime_core
	kyty_diagnostics
	kyty_audio_video_backend
	core
	math
	vulkan-1
	spirv-tools-opt
	spirv-tools
	easy_profiler
	imgui
	SDL2-static
	PRIVATE
		# The runtime agent owns its local transport. Keep it out of the
		# emulator's transitive link interface; host executables that expose
		# the agent endpoint opt in explicitly below.
		kyty_agent_transport
		kyty_audio_pcm
)

get_property(inc_headers TARGET emulator PROPERTY INCLUDE_DIRECTORIES)

list(APPEND inc_headers
	${CMAKE_SOURCE_DIR}/3rdparty/sdl2/sdl2/include
	${CMAKE_SOURCE_DIR}/3rdparty/vulkan/include
	${CMAKE_SOURCE_DIR}/3rdparty/easy_profiler/include
	${CMAKE_SOURCE_DIR}/3rdparty/imgui
	${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends
	${CMAKE_SOURCE_DIR}/3rdparty/xxhash/include
	${CMAKE_SOURCE_DIR}/3rdparty/cpuinfo/include
	${CMAKE_SOURCE_DIR}/3rdparty/stb
	${CMAKE_SOURCE_DIR}/3rdparty/miniz/src
)

target_include_directories(emulator PRIVATE ${inc_headers})
target_include_directories(kyty_graphics PRIVATE ${inc_headers})
target_include_directories(kyty_host PRIVATE ${inc_headers})
target_include_directories(kyty_hle PRIVATE ${inc_headers})
target_include_directories(kyty_loader PRIVATE ${inc_headers})

if (MSVC AND CLANG)
	target_link_libraries(emulator PRIVATE winpthread)
	list(APPEND inc_headers 
		${CMAKE_SOURCE_DIR}/3rdparty/winpthread/include
	)
endif()

# FFmpeg is optional at configure time. The AvPlayer keeps its synthetic
# diagnostic stream when the host does not provide the demux/codec libraries.
if (LINUX)
	find_package(PkgConfig QUIET)
	if (PkgConfig_FOUND)
		pkg_check_modules(KYTY_FFMPEG QUIET IMPORTED_TARGET
			libavformat libavcodec libavutil libswscale libswresample)
	endif()
	if (KYTY_FFMPEG_FOUND)
		target_link_libraries(kyty_audio_video_backend PRIVATE PkgConfig::KYTY_FFMPEG)
		target_compile_definitions(kyty_audio_video_backend PRIVATE KYTY_HAVE_FFMPEG=1)
	endif()
endif()

list(APPEND check_headers
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_SOURCE_DIR}/include
)

#clang_tidy_check(emulator_obj "" "${check_headers}" "${inc_headers}")
clang_tidy_check(emulator "" "${check_headers}" "${inc_headers}")

#include_what_you_use(emulator_obj "${inc_headers}")
include_what_you_use(emulator "${inc_headers}")
