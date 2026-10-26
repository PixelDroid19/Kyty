find_package(Threads REQUIRED)

add_library(kyty_agent_cli STATIC
	src/Cli.cpp
)

target_include_directories(kyty_agent_cli PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/include"
)

target_link_libraries(kyty_agent_cli
	kyty_agent_transport
	Threads::Threads
)

if (NOT MSVC)
	target_compile_options(kyty_agent_cli PRIVATE -fno-exceptions)
endif()

# Runner sessions and offline capture scoring; std-only apart from stb_image.
add_library(kyty_agent_runner STATIC
	src/runner/CaptureMetrics.cpp
	src/runner/CaptureRun.cpp
	src/runner/ChildEnvironment.cpp
	src/runner/GamesMatrix.cpp
	src/runner/HardwareOracle.cpp
	src/runner/JsonValue.cpp
	src/runner/Md5.cpp
	src/runner/OperationGraph.cpp
	src/runner/PlayableGates.cpp
	src/runner/PlayableSession.cpp
	src/runner/RunnerCli.cpp
	src/runner/RunnerHostPosix.cpp
	src/runner/RunnerSupport.cpp
	src/runner/SceneCheckpoint.cpp
	src/runner/Sha256.cpp
)

target_include_directories(kyty_agent_runner PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_include_directories(kyty_agent_runner PRIVATE
	"${CMAKE_SOURCE_DIR}/3rdparty/stb"
)

if (NOT MSVC)
	target_compile_options(kyty_agent_runner PRIVATE -fno-exceptions)
endif()

# The hardware oracle refuses to write results inside the repository it was built from.
get_filename_component(KYTY_REPO_ROOT "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
set_source_files_properties(src/runner/HardwareOracle.cpp PROPERTIES
	COMPILE_DEFINITIONS "KYTY_REPO_ROOT=\"${KYTY_REPO_ROOT}\""
)

target_link_libraries(kyty_agent_cli
	kyty_agent_runner
)

add_executable(kyty_agent
	src/Main.cpp
)

target_link_libraries(kyty_agent
	kyty_agent_cli
)
