file(GLOB emulator_src CONFIGURE_DEPENDS
	src/*.cpp
	src/Libs/*.cpp
	src/Hle/*.cpp
	src/Graphics/*.cpp
	src/Graphics/Objects/*.cpp
	src/Kernel/*.cpp
	src/Loader/*.cpp
	include/*.h
)

# Keep the guest-kernel implementation in its own archive.  The public HLE
# adapters still link it through the emulator target, but the CMake ownership
# boundary prevents new Kernel sources from silently becoming part of the
# graphics/audio/loader monolith.
file(GLOB kernel_src CONFIGURE_DEPENDS src/Kernel/*.cpp)
file(GLOB hle_src CONFIGURE_DEPENDS src/Libs/*.cpp)
list(REMOVE_ITEM emulator_src ${kernel_src})
list(REMOVE_ITEM emulator_src ${hle_src})
list(REMOVE_ITEM emulator_src "${CMAKE_CURRENT_SOURCE_DIR}/src/AudioVideoBackend.cpp")
list(REMOVE_ITEM emulator_src "${CMAKE_CURRENT_SOURCE_DIR}/src/AudioPcm.cpp")

if (MSVC AND CLANG)
	set_source_files_properties(${emulator_src} ${hle_src} PROPERTIES COMPILE_FLAGS "-Wno-pragma-pack -Wno-deprecated-declarations -D_TIMESPEC_DEFINED")
endif()

#add_library(emulator_obj OBJECT ${emulator_src})
#add_library(emulator STATIC $<TARGET_OBJECTS:emulator_obj>)
add_library(emulator STATIC ${emulator_src})
add_library(kyty_kernel STATIC ${kernel_src})
add_library(kyty_hle STATIC ${hle_src})
add_library(kyty_audio_video_backend STATIC src/AudioVideoBackend.cpp)

# Fail at configure time if a future glob or explicit source entry collapses
# the kernel back into the emulator archive.
get_target_property(_emulator_sources emulator SOURCES)
foreach(_emulator_source IN LISTS _emulator_sources)
	if(_emulator_source MATCHES "(^|/)src/Kernel/")
		message(FATAL_ERROR "Kernel source leaked into emulator target: ${_emulator_source}")
	endif()
	if(_emulator_source MATCHES "(^|/)src/Libs/")
		message(FATAL_ERROR "HLE source leaked into emulator target: ${_emulator_source}")
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
get_target_property(_hle_sources kyty_hle SOURCES)
if(NOT _hle_sources)
	message(FATAL_ERROR "kyty_hle target has no implementation sources")
endif()
get_target_property(_hle_links kyty_hle LINK_LIBRARIES)
if(_hle_links AND "scripts" IN_LIST _hle_links)
	message(FATAL_ERROR "kyty_hle must not link the script host archive directly")
endif()

target_sources(emulator PRIVATE
	$<TARGET_OBJECTS:imgui_sdl_backend_obj>
	include/Emulator/Profiling.h
	src/Loader/GuestCall.cpp
	include/Emulator/Loader/GuestCall.h
	src/Host/Clock.cpp
	include/Emulator/Host/Clock.h
	src/Host/ImageSurfaceSdl.cpp
	include/Emulator/Host/ImageSurface.h
	src/Host/HostWindowSdl.cpp
	include/Emulator/Host/HostWindow.h
	src/Host/HostVulkanWindowSdl.cpp
	include/Emulator/Host/HostVulkanWindow.h
	src/Host/HostInputSdl.cpp
	include/Emulator/Host/HostInput.h
	src/Host/HostGuiSdl.cpp
	include/Emulator/Host/HostGui.h
	src/Host/CaptureImageCodec.cpp
	include/Emulator/Host/CaptureImageCodec.h
	src/Host/Png.cpp
	include/Emulator/Host/Png.h
	src/Host/Platform.cpp
	include/Emulator/Host/Platform.h
	src/Agent/EventRing.cpp
	src/Agent/DebugSnapshot.cpp
	src/Agent/Protocol.cpp
	src/Agent/StallWatch.cpp
	src/Agent/FrameScore.cpp
	src/Agent/AgentServer.cpp
	src/Agent/AgentSubsystem.cpp
	src/Agent/AgentLifecycle.cpp
	src/GuestRuntimePort.cpp
	include/Emulator/Kernel/Errors.h
	include/Emulator/Kernel/Trace.h
	src/Graphics/PipelineCacheStore.cpp
	src/Graphics/GpuDirtyPageTracker.cpp
	src/Graphics/CommandProcessorSubmissionSlots.cpp
	src/Graphics/Objects/ExactStagingPool.cpp
	src/Graphics/Objects/GpuWritebackPageCache.cpp
	src/Graphics/Objects/LabelSubmissionTracker.cpp
	src/Graphics/GpuSubmissionTracker.cpp
	src/Graphics/GpuSubmissionCoordinator.cpp
	src/Graphics/GpuSubmissionPublicationGate.cpp
	src/Graphics/GpuDeferredDeletionQueue.cpp
	src/Graphics/ShaderTranslationCache.cpp
	src/Graphics/SpirvBinaryCacheStore.cpp
	src/Graphics/RenderResolutionShaderUsageCache.cpp
	src/Graphics/RenderResolutionPolicy.cpp
	src/Graphics/RenderResolutionCoordinator.cpp
	src/Graphics/RenderResolutionPlanner.cpp
	src/Graphics/RenderResolutionAlias.cpp
	src/Graphics/VideoOutFlipLifecycleGate.cpp
	src/Graphics/VideoOutFlipQueueAdmissionGate.cpp
	src/Graphics/VideoOutHostAccessGate.cpp
	src/Graphics/VideoOutMaterializationGate.cpp
	src/Graphics/RenderResolutionImageCapability.cpp
	src/Graphics/RenderResolutionTransform.cpp
	src/Graphics/VulkanRenderResolutionCapability.cpp
	src/Graphics/VulkanQueueIdentity.cpp
	src/Graphics/RenderResolutionShaderScale.cpp
	src/Graphics/PresentationScaler.cpp
	src/Graphics/SampleLocations.cpp
	src/Graphics/VulkanVertexInputLayout.cpp
	src/Graphics/DepthStencilCopy.cpp
	src/Graphics/GuestTextureLayout.cpp
	src/Graphics/Gen5TextureArrayLayout.cpp
	src/Graphics/NativeCapture.cpp
	include/Emulator/Graphics/PipelineCacheStore.h
	include/Emulator/Graphics/GpuDirtyPageTracker.h
	include/Emulator/Graphics/CommandProcessorSubmissionSlots.h
	include/Emulator/Graphics/Objects/ExactStagingPool.h
	include/Emulator/Graphics/Objects/GpuWritebackPageCache.h
	include/Emulator/Graphics/Objects/LabelSubmissionTracker.h
	include/Emulator/Graphics/GpuSubmissionTracker.h
	include/Emulator/Graphics/GpuSubmissionCoordinator.h
	include/Emulator/Graphics/GpuSubmissionPublicationGate.h
	include/Emulator/Graphics/GpuDeferredDeletionQueue.h
	include/Emulator/Graphics/ShaderTranslationCache.h
	include/Emulator/Graphics/SpirvBinaryCacheStore.h
	include/Emulator/Graphics/RenderResolutionShaderUsageCache.h
	include/Emulator/Graphics/RenderResolutionPolicy.h
	include/Emulator/Graphics/RenderResolutionCoordinator.h
	include/Emulator/Graphics/RenderResolutionPlanner.h
	include/Emulator/Graphics/RenderResolutionAlias.h
	include/Emulator/Graphics/VideoOutFlipLifecycleGate.h
	include/Emulator/Graphics/VideoOutFlipQueueAdmissionGate.h
	include/Emulator/Graphics/VideoOutHostAccessGate.h
	include/Emulator/Graphics/VideoOutMaterializationGate.h
	include/Emulator/Graphics/RenderResolutionImageCapability.h
	include/Emulator/Graphics/RenderResolutionTransform.h
	include/Emulator/Graphics/VulkanRenderResolutionCapability.h
	include/Emulator/Graphics/VulkanQueueIdentity.h
	include/Emulator/Graphics/RenderResolutionShaderScale.h
	include/Emulator/Graphics/PresentationScaler.h
	include/Emulator/Graphics/SampleLocations.h
	include/Emulator/Graphics/VulkanVertexInputLayout.h
	include/Emulator/Graphics/DepthStencilCopy.h
	include/Emulator/Graphics/GuestTextureLayout.h
	include/Emulator/Graphics/Gen5TextureArrayLayout.h
	include/Emulator/Graphics/NativeCapture.h
	include/Emulator/Libs/ProcessEnvironment.h
	src/Validation/DomainValidators.cpp
	include/Emulator/Validation/DomainValidators.h
	include/Emulator/Agent/EventRing.h
	include/Emulator/Agent/DebugSnapshot.h
	include/Emulator/Agent/Protocol.h
	include/Emulator/Agent/StallWatch.h
	include/Emulator/Agent/FrameScore.h
	include/Emulator/Agent/AgentServer.h
	include/Emulator/Agent/AgentSubsystem.h
	include/Emulator/Agent/AgentLifecycle.h
	include/Emulator/Kernel/Time.h
	include/Emulator/Kernel/TimePort.h
	include/Emulator/Kernel/AmprPort.h
)
add_dependencies(emulator KytyBuildInfo)

target_link_libraries(emulator
	PUBLIC
	kyty_kernel
	kyty_hle
	kyty_audio_video_backend
	core
	math
	scripts
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

target_include_directories(kyty_kernel PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(kyty_kernel PUBLIC core math sys)

target_include_directories(kyty_hle PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(kyty_hle PUBLIC core math sys kyty_kernel)

target_include_directories(kyty_audio_video_backend PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(kyty_audio_video_backend PUBLIC core)

if(MSVC OR MINGW)
	target_link_libraries(emulator PRIVATE psapi)
endif()

target_include_directories(emulator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

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
		target_link_libraries(emulator PRIVATE PkgConfig::KYTY_FFMPEG)
		target_link_libraries(kyty_audio_video_backend PRIVATE PkgConfig::KYTY_FFMPEG)
		target_compile_definitions(emulator PRIVATE KYTY_HAVE_FFMPEG=1)
		target_compile_definitions(kyty_audio_video_backend PRIVATE KYTY_HAVE_FFMPEG=1)
	endif()
endif()

target_include_directories(emulator PRIVATE ${inc_headers})
target_include_directories(kyty_hle PRIVATE ${inc_headers})

list(APPEND check_headers
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_SOURCE_DIR}/include
)

#clang_tidy_check(emulator_obj "" "${check_headers}" "${inc_headers}")
clang_tidy_check(emulator "" "${check_headers}" "${inc_headers}")

#include_what_you_use(emulator_obj "${inc_headers}")
include_what_you_use(emulator "${inc_headers}")
