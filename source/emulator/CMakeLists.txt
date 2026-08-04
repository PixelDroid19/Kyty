file(GLOB emulator_src
	src/*.cpp
	src/*.cpp
	src/Libs/*.cpp
	src/Graphics/*.cpp
	src/Graphics/Objects/*.cpp
	src/Kernel/*.cpp
	src/Loader/*.cpp
	include/*.h
)
list(REMOVE_ITEM emulator_src "${CMAKE_CURRENT_SOURCE_DIR}/src/AudioPcm.cpp")

if (MSVC AND CLANG)
	set_source_files_properties(${emulator_src} PROPERTIES COMPILE_FLAGS "-Wno-pragma-pack -Wno-deprecated-declarations -D_TIMESPEC_DEFINED")
endif()

#add_library(emulator_obj OBJECT ${emulator_src})
#add_library(emulator STATIC $<TARGET_OBJECTS:emulator_obj>)
add_library(emulator STATIC ${emulator_src})
target_sources(emulator PRIVATE
	$<TARGET_OBJECTS:imgui_sdl_backend_obj>
	src/Loader/GuestCall.cpp
	include/Emulator/Loader/GuestCall.h
	src/DevTools/Runtime.cpp
	src/DevTools/RuntimeSubsystem.cpp
	include/Emulator/DevTools/Runtime.h
	include/Emulator/DevTools/RuntimeSubsystem.h
	src/Agent/EventRing.cpp
	src/Agent/DebugSnapshot.cpp
	src/Agent/Protocol.cpp
	src/Agent/StallWatch.cpp
	src/Agent/FrameScore.cpp
	src/Agent/AgentServer.cpp
	src/Agent/AgentSubsystem.cpp
	src/Agent/AgentLifecycle.cpp
	src/Kernel/Time.cpp
	src/Libs/ProcessEnvironment.cpp
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
)
add_dependencies(emulator KytyBuildInfo)

target_link_libraries(emulator
	PUBLIC
		core
		math
		scripts
		lua
		unit_test
		kyty_agent_transport
		kyty_devtools_core
		vulkan-1
		spirv-tools-opt
		spirv-tools
		easy_profiler
		imgui
		SDL2-static
	PRIVATE
		kyty_audio_pcm
)

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
		target_compile_definitions(emulator PRIVATE KYTY_HAVE_FFMPEG=1)
	endif()
endif()

target_include_directories(emulator PRIVATE ${inc_headers})

list(APPEND check_headers
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_SOURCE_DIR}/include
)

#clang_tidy_check(emulator_obj "" "${check_headers}" "${inc_headers}")
clang_tidy_check(emulator "" "${check_headers}" "${inc_headers}")

#include_what_you_use(emulator_obj "${inc_headers}")
include_what_you_use(emulator "${inc_headers}")
