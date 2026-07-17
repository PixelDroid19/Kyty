if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED SCENARIO OR NOT DEFINED EXPECTED_EXIT)
	message(FATAL_ERROR "RunAgentProtocolCase: TEST_EXECUTABLE, SCENARIO, EXPECTED_EXIT required")
endif()

unset(ENV{KYTY_STUB_MISSING})
unset(ENV{KYTY_GFX_PERMISSIVE})
unset(ENV{KYTY_BRINGUP_MODE})
unset(ENV{KYTY_BRINGUP_FEATURES})
unset(ENV{KYTY_BRINGUP_SUBSYSTEMS})

# Lifecycle bringup_continue needs unsafe + not_implemented.
if(SCENARIO STREQUAL "lifecycle_events_sanitize")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "not_implemented")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "core,loader")
else()
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "not_implemented")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "core")
endif()

execute_process(
	COMMAND "${TEST_EXECUTABLE}" "${SCENARIO}"
	RESULT_VARIABLE actual_exit
	OUTPUT_VARIABLE stdout
	ERROR_VARIABLE stderr
)

if(NOT actual_exit EQUAL EXPECTED_EXIT)
	message(STATUS "stdout:\n${stdout}")
	message(STATUS "stderr:\n${stderr}")
	message(FATAL_ERROR "scenario ${SCENARIO}: expected ${EXPECTED_EXIT}, got ${actual_exit}")
endif()
