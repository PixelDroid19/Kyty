if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED PYTHON_EXECUTABLE OR NOT DEFINED CHECKER OR NOT DEFINED SOURCE_ROOT)
	message(FATAL_ERROR "RunArchitectureBoundaryStrictCase: TEST_EXECUTABLE, PYTHON_EXECUTABLE, CHECKER, SOURCE_ROOT required")
endif()

# add_test only applies these target properties when its command is the target
# itself. This driver needs captured output, so reproduce that prefix here.
set(test_command)
if(DEFINED TEST_LAUNCHER AND NOT "${TEST_LAUNCHER}" STREQUAL "")
	list(APPEND test_command ${TEST_LAUNCHER})
endif()
if(CROSSCOMPILING AND DEFINED CROSSCOMPILING_EMULATOR AND NOT "${CROSSCOMPILING_EMULATOR}" STREQUAL "")
	list(APPEND test_command ${CROSSCOMPILING_EMULATOR})
endif()
list(APPEND test_command "${TEST_EXECUTABLE}" "${PYTHON_EXECUTABLE}" "${CHECKER}" "${SOURCE_ROOT}" --strict)

execute_process(
	COMMAND ${test_command}
	RESULT_VARIABLE actual_exit
	OUTPUT_VARIABLE stdout
	ERROR_VARIABLE stderr
)

string(REPLACE "\r\n" "\n" stdout "${stdout}")
string(REPLACE "\r\n" "\n" stderr "${stderr}")

string(CONCAT expected_stdout
	"emulator/src/Audio.cpp:11: forbidden include (Audio -> Graphics): Emulator/Graphics/GuestTextureLayout.h\n"
	"emulator/src/Audio.cpp:12: forbidden include (Audio -> Graphics): Emulator/Graphics/Objects/GpuMemory.h\n"
)
set(expected_stderr "architecture boundary integration failed: checker exited with 1\n")

string(LENGTH "${expected_stderr}" expected_stderr_length)
string(LENGTH "${stderr}" actual_stderr_length)
set(stderr_has_expected_wrapper_message FALSE)

if(DEFINED TEST_LAUNCHER AND NOT "${TEST_LAUNCHER}" STREQUAL "")
	# A configured CMake test launcher may emit instrumentation text before the
	# wrapper diagnostic. Keep the wrapper message as the exact suffix while
	# allowing that launcher-owned prefix.
	if(actual_stderr_length GREATER_EQUAL expected_stderr_length)
		math(EXPR stderr_suffix_start "${actual_stderr_length} - ${expected_stderr_length}")
		string(SUBSTRING "${stderr}" ${stderr_suffix_start} ${expected_stderr_length} stderr_suffix)
		if("${stderr_suffix}" STREQUAL "${expected_stderr}")
			set(stderr_has_expected_wrapper_message TRUE)
		endif()
	endif()
else()
	# In the normal build there is no launcher-owned output; keep this stream
	# exact so checker warnings, tracebacks, and wrapper failures cannot hide.
	if("${stderr}" STREQUAL "${expected_stderr}")
		set(stderr_has_expected_wrapper_message TRUE)
	endif()
endif()

# Exit code and checker stdout remain exact in both modes.
if(NOT "${actual_exit}" STREQUAL "1" OR NOT "${stdout}" STREQUAL "${expected_stdout}" OR NOT stderr_has_expected_wrapper_message)
	message(FATAL_ERROR
		"strict boundary contract failed\n"
		"expected exit: 1\nactual exit: ${actual_exit}\n"
		"expected stdout:\n${expected_stdout}\nactual stdout:\n${stdout}\n"
		"expected stderr suffix:\n${expected_stderr}\nactual stderr:\n${stderr}"
	)
endif()
