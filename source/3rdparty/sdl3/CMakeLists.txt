set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(sdl3 build_sdl3)

# The emulator links SDL statically and must not resolve SDL through a
# replaceable dynamic API table.
target_compile_definitions(SDL3-static PRIVATE KYTY_SDL_STATIC)

if (CLANG AND NOT ANDROID)
	target_compile_options(SDL3-static PRIVATE -Wno-pragma-pack -Wno-unused-command-line-argument -Wno-unused-function
	                       -Wno-unused-const-variable -Wno-unused-variable)
elseif (CMAKE_C_COMPILER_ID MATCHES "GNU")
	target_compile_options(SDL3-static PRIVATE -Wno-unused-variable -Wno-unused-function)
endif()
