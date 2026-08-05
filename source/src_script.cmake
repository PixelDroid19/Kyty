set(KYTY_SCRIPT_SRC
	KytyScripts.cpp
	KytyEmulator.cpp
	KytyProfiler.cpp
)

if(KYTY_BUILD_UNIT_TESTS)
	list(APPEND KYTY_SCRIPT_SRC 3rdparty/gtest/src/gtest-all.cc)
endif()
