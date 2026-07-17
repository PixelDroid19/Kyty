find_package(Threads REQUIRED)

add_executable(kyty_agent
	src/Main.cpp
	src/Cli.cpp
)

target_include_directories(kyty_agent PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(kyty_agent
	Threads::Threads
)

if (NOT MSVC)
	target_compile_options(kyty_agent PRIVATE -fno-exceptions)
endif()
