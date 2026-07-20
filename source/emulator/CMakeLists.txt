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

if (MSVC AND CLANG)
	set_source_files_properties(${emulator_src} PROPERTIES COMPILE_FLAGS "-Wno-pragma-pack -Wno-deprecated-declarations -D_TIMESPEC_DEFINED")
endif()

#add_library(emulator_obj OBJECT ${emulator_src})
#add_library(emulator STATIC $<TARGET_OBJECTS:emulator_obj>)
add_library(emulator STATIC ${emulator_src})
target_sources(emulator PRIVATE
	src/Loader/GuestCall.cpp
	include/Emulator/Loader/GuestCall.h
	src/DevTools/Runtime.cpp
	src/DevTools/RuntimeSubsystem.cpp
	include/Emulator/DevTools/Runtime.h
	include/Emulator/DevTools/RuntimeSubsystem.h
	src/Agent/EventRing.cpp
	src/Agent/Protocol.cpp
	src/Agent/StallWatch.cpp
	src/Agent/FrameScore.cpp
	src/Agent/AgentServer.cpp
	src/Agent/AgentSubsystem.cpp
	src/Agent/AgentLifecycle.cpp
	src/Kernel/Time.cpp
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
	src/Graphics/ShaderResolutionUsageCache.cpp
	src/Graphics/InternalResolutionPolicy.cpp
	src/Graphics/InternalResolutionRuntime.cpp
	src/Graphics/AttachmentResolutionCohort.cpp
	src/Graphics/ResolutionAliasPolicy.cpp
	src/Graphics/VideoOutFlipLifecycleGate.cpp
	src/Graphics/VideoOutFlipQueueAdmissionGate.cpp
	src/Graphics/VideoOutHostAccessGate.cpp
	src/Graphics/VideoOutMaterializationGate.cpp
	src/Graphics/ResolutionImageCapability.cpp
	src/Graphics/ResolutionCoordinateTransform.cpp
	src/Graphics/VulkanResolutionCapability.cpp
	src/Graphics/VulkanQueueIdentity.cpp
	src/Graphics/ShaderCoordinateScale.cpp
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
	include/Emulator/Graphics/ShaderResolutionUsageCache.h
	include/Emulator/Graphics/InternalResolutionPolicy.h
	include/Emulator/Graphics/InternalResolutionRuntime.h
	include/Emulator/Graphics/AttachmentResolutionCohort.h
	include/Emulator/Graphics/ResolutionAliasPolicy.h
	include/Emulator/Graphics/VideoOutFlipLifecycleGate.h
	include/Emulator/Graphics/VideoOutFlipQueueAdmissionGate.h
	include/Emulator/Graphics/VideoOutHostAccessGate.h
	include/Emulator/Graphics/VideoOutMaterializationGate.h
	include/Emulator/Graphics/ResolutionImageCapability.h
	include/Emulator/Graphics/ResolutionCoordinateTransform.h
	include/Emulator/Graphics/VulkanResolutionCapability.h
	include/Emulator/Graphics/VulkanQueueIdentity.h
	include/Emulator/Graphics/ShaderCoordinateScale.h
	src/Validation/DomainValidators.cpp
	include/Emulator/Validation/DomainValidators.h
	include/Emulator/Agent/EventRing.h
	include/Emulator/Agent/Protocol.h
	include/Emulator/Agent/StallWatch.h
	include/Emulator/Agent/FrameScore.h
	include/Emulator/Agent/AgentServer.h
	include/Emulator/Agent/AgentSubsystem.h
	include/Emulator/Agent/AgentLifecycle.h
	include/Emulator/Kernel/Time.h
)
add_dependencies(emulator KytyBuildInfo)

target_link_libraries(emulator core math scripts lua unit_test kyty_devtools_core vulkan-1 spirv-tools-opt spirv-tools easy_profiler imgui SDL2-static)

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
)

if (MSVC AND CLANG)
	target_link_libraries(emulator winpthread)
	list(APPEND inc_headers 
		${CMAKE_SOURCE_DIR}/3rdparty/winpthread/include
	)
endif()

#target_include_directories(emulator_obj PRIVATE ${inc_headers})
target_include_directories(emulator PRIVATE ${inc_headers})

list(APPEND check_headers
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_SOURCE_DIR}/include
)

#clang_tidy_check(emulator_obj "" "${check_headers}" "${inc_headers}")
clang_tidy_check(emulator "" "${check_headers}" "${inc_headers}")

#include_what_you_use(emulator_obj "${inc_headers}")
include_what_you_use(emulator "${inc_headers}")
