file(GLOB scripts_src CONFIGURE_DEPENDS
	"src/*.cpp"
)

# Keep the script host in its own archive.  Embedding these objects into every
# consumer (the former OBJECT target) made BuildTools/Lua implementation leak
# into the kernel and emulator archives and forced unrelated linkers to own the
# scripting lifecycle.
add_library(scripts STATIC ${scripts_src})

get_target_property(_scripts_type scripts TYPE)
if(NOT _scripts_type STREQUAL "STATIC_LIBRARY")
	message(FATAL_ERROR "scripts must remain an isolated static archive")
endif()

target_link_libraries(scripts core lua)

target_include_directories(scripts PUBLIC
	"${CMAKE_SOURCE_DIR}/3rdparty/lua/include"
)

get_property(inc_headers TARGET scripts PROPERTY INCLUDE_DIRECTORIES)

list(APPEND inc_headers 
	${CMAKE_SOURCE_DIR}/3rdparty/lua/src 	
)

target_include_directories(scripts PRIVATE ${inc_headers})

list(APPEND check_headers
	${CMAKE_SOURCE_DIR}/include
)

clang_tidy_check(scripts "" "${check_headers}" "${inc_headers}")

include_what_you_use(scripts "${inc_headers}")
