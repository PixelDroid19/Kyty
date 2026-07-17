if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED SCENARIO OR NOT DEFINED EXPECTED_DIAGNOSTIC)
	message(FATAL_ERROR "RunLibcWideRejection: TEST_EXECUTABLE, SCENARIO, EXPECTED_DIAGNOSTIC required")
endif()

unset(ENV{KYTY_BRINGUP_MODE})
unset(ENV{KYTY_BRINGUP_FEATURES})
unset(ENV{KYTY_BRINGUP_SUBSYSTEMS})
unset(ENV{KYTY_BRINGUP_ALLOW_DIAGNOSTIC})

execute_process(
	COMMAND "${TEST_EXECUTABLE}" "${SCENARIO}"
	RESULT_VARIABLE actual_exit
	OUTPUT_VARIABLE stdout
	ERROR_VARIABLE stderr
)

if("${actual_exit}" STREQUAL "0")
	message(FATAL_ERROR "Scenario ${SCENARIO}: unsupported input returned success")
endif()

set(diagnostic_output "${stdout}\n${stderr}")
string(FIND "${diagnostic_output}" "${EXPECTED_DIAGNOSTIC}" diagnostic_offset)
if(diagnostic_offset EQUAL -1)
	message(FATAL_ERROR
		"Scenario ${SCENARIO}: missing diagnostic '${EXPECTED_DIAGNOSTIC}'\n"
		"exit: ${actual_exit}\nstdout:\n${stdout}\nstderr:\n${stderr}"
	)
endif()
