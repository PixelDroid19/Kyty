file(GLOB libatrac9_src
    "src/*.c"
)

add_library(libatrac9 STATIC ${libatrac9_src})

target_include_directories(libatrac9 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Without COMPILING_DLL the header declares the functions dllimport under MSVC.
if (MSVC)
	target_compile_definitions(libatrac9 PUBLIC COMPILING_DLL)
endif()

if (NOT MSVC)
	target_link_libraries(libatrac9 PRIVATE m)
	target_compile_options(libatrac9 PRIVATE -w)
endif()
