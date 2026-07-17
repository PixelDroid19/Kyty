# Portable bring-up scenario runner.
# Required: CMD, SCENARIO, EXPECT
# Optional: ENV_<NAME>=value for each environment variable to set for the child.
# Clears legacy diagnostic vars so they cannot leak into the child.

if(NOT DEFINED CMD OR NOT DEFINED SCENARIO OR NOT DEFINED EXPECT)
	message(FATAL_ERROR "RunBringUpScenario: CMD, SCENARIO, and EXPECT are required")
endif()

# Scrub legacy flags (removed API).
unset(ENV{KYTY_STUB_MISSING})
unset(ENV{KYTY_GFX_PERMISSIVE})

# Clear all bring-up keys first so a previous shell export cannot leak.
unset(ENV{KYTY_BRINGUP_MODE})
unset(ENV{KYTY_BRINGUP_FEATURES})
unset(ENV{KYTY_BRINGUP_SUBSYSTEMS})
unset(ENV{KYTY_BRINGUP_BURST_LIMIT})
unset(ENV{KYTY_BRINGUP_BURST_WINDOW_MS})
unset(ENV{KYTY_BRINGUP_ALLOW_DIAGNOSTIC})

# Apply requested ENV_* macros from -DENV_NAME=value.
get_cmake_property(_vars VARIABLES)
foreach(_v ${_vars})
	if(_v MATCHES "^ENV_(.+)$")
		set(_name "${CMAKE_MATCH_1}")
		set(ENV{${_name}} "${${_v}}")
	endif()
endforeach()

execute_process(
	COMMAND "${CMD}" "${SCENARIO}"
	RESULT_VARIABLE _rc
	OUTPUT_VARIABLE _out
	ERROR_VARIABLE _err
)

if(NOT _rc EQUAL EXPECT)
	message(STATUS "stdout:\n${_out}")
	message(STATUS "stderr:\n${_err}")
	message(FATAL_ERROR
		"scenario '${SCENARIO}' exit ${_rc}, expected ${EXPECT}")
endif()

message(STATUS "scenario '${SCENARIO}' exit ${_rc} (ok)")
