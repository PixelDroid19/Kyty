file(GLOB unit_test_src CONFIGURE_DEPENDS
    "include/*.h"
    "src/*.cpp"
    "src/core/*.cpp"
    "src/emulator/*.cpp"
)

# The disabled implementation is selected by the top-level CMake file only
# when KYTY_BUILD_UNIT_TESTS is OFF.  Keep it out of the test target when the
# source glob is active so the enabled and disabled subsystem definitions
# cannot be linked together.
list(FILTER unit_test_src EXCLUDE REGEX "/UnitTestDisabled\\.cpp$")
# The standalone runner owns main(); keep it out of the static archive so
# fc_script (which also defines main) can link unit_test without a clash.
list(FILTER unit_test_src EXCLUDE REGEX "/UnitTestMain\\.cpp$")

# DevTools tests are listed explicitly (no recursive glob expansion for new
# modules). They exercise the process-isolated supervisor and its transport,
# which only exist when KYTY_BUILD_DEVTOOLS is enabled; the tooling-off build
# must not link the empty kyty_devtools_core archive.
if(KYTY_BUILD_DEVTOOLS)
	list(APPEND unit_test_src
		"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsEventRing.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsProgress.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsClassifier.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsProtocol.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsExportCatalog.cpp"
	)
endif()
	list(APPEND unit_test_src
		"${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/UnitTestEmulatorGraphicsDirtyTracking.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/UnitTestEmulatorExactStagingPool.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/UnitTestEmulatorVideoOutResolution.cpp"
	)

# These suites exercise POSIX process, descriptor and environment primitives
# and the process-isolated supervisor. Their Windows counterparts need a
# dedicated port; the supervisor suites also require KYTY_BUILD_DEVTOOLS.
if(NOT WIN32 AND KYTY_BUILD_DEVTOOLS)
	list(APPEND unit_test_src
		"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsSupervisor.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsBundle.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsLifecycle.cpp"
	)
endif()

if (MSVC AND CLANG)
	set_source_files_properties(${unit_test_src} PROPERTIES COMPILE_FLAGS "-Wno-pragma-pack")
endif()

add_library(unit_test STATIC ${unit_test_src})

target_link_libraries(unit_test core)
if(KYTY_BUILD_DEVTOOLS)
	target_link_libraries(unit_test kyty_devtools_core)
	target_compile_definitions(unit_test PRIVATE KYTY_HAVE_DEVTOOLS_TESTS=1)
endif()
target_link_libraries(unit_test math)
target_link_libraries(unit_test kyty_devtools_core)
target_include_directories(unit_test PRIVATE "${CMAKE_SOURCE_DIR}/devtools/include")
target_include_directories(unit_test PRIVATE "${CMAKE_SOURCE_DIR}/emulator/include")
target_include_directories(unit_test PRIVATE "${CMAKE_SOURCE_DIR}/3rdparty/vulkan/include")

#target_include_directories(unit_test PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

list(APPEND inc_headers 
	${PROJECT_BINARY_DIR}
	${PROJECT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/include
	${CMAKE_SOURCE_DIR}/3rdparty/gtest/include
	${CMAKE_SOURCE_DIR}/3rdparty/gtest
)

list(APPEND check_headers
	${CMAKE_SOURCE_DIR}/include
)

clang_tidy_check(unit_test "" "${check_headers}" "${inc_headers}")

include_what_you_use(unit_test "${inc_headers}")
