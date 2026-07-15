file(GLOB unit_test_src
    "include/*.h"
    "src/*.cpp"
    "src/core/*.cpp"
    "src/emulator/*.cpp"
)

# DevTools tests are listed explicitly (no recursive glob expansion for new modules).
list(APPEND unit_test_src
	"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsEventRing.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsProgress.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsClassifier.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsProtocol.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsSupervisor.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsBundle.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/devtools/UnitTestDevToolsLifecycle.cpp"
)

if (MSVC AND CLANG)
	set_source_files_properties(${unit_test_src} PROPERTIES COMPILE_FLAGS "-Wno-pragma-pack")
endif()

add_library(unit_test STATIC ${unit_test_src})

target_link_libraries(unit_test core)
target_link_libraries(unit_test math)
target_link_libraries(unit_test kyty_devtools_core)
target_link_libraries(unit_test kyty_devtools_supervisor)
target_include_directories(unit_test PRIVATE "${CMAKE_SOURCE_DIR}/devtools/include")
target_include_directories(unit_test PRIVATE "${CMAKE_SOURCE_DIR}/emulator/include")

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

