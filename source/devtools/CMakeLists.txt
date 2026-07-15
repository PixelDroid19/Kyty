find_package(Threads REQUIRED)

add_library(kyty_devtools_supervisor STATIC)

target_include_directories(kyty_devtools_supervisor PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(kyty_devtools_supervisor
	kyty_devtools_core
	Threads::Threads
)

if (WIN32)
	target_sources(kyty_devtools_supervisor PRIVATE
		src/SharedMappingWindows.cpp
		src/ProcessLauncherWindows.cpp
		src/SecureRandomWindows.cpp
		src/ProcessIdentityWindows.cpp
	)
	target_link_libraries(kyty_devtools_supervisor bcrypt)
else()
	target_sources(kyty_devtools_supervisor PRIVATE
		src/SharedMappingPosix.cpp
		src/ProcessLauncherPosix.cpp
		src/SecureRandomPosix.cpp
	)
	# Process identity for Linux is in ProcessLauncherPosix.cpp for now.
endif()

if (NOT MSVC)
	target_compile_options(kyty_devtools_supervisor PRIVATE -fno-exceptions)
endif()
