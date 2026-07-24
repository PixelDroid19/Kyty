add_library(kyty_agent_transport STATIC
	src/LocalTransport.cpp
)

target_include_directories(kyty_agent_transport PUBLIC
	"${CMAKE_SOURCE_DIR}/include"
)

if (NOT MSVC)
	target_compile_options(kyty_agent_transport PRIVATE -fno-exceptions)
endif()
