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

add_executable(kyty_agent
	src/Main.cpp
)

target_link_libraries(kyty_agent
	kyty_agent_cli
)
