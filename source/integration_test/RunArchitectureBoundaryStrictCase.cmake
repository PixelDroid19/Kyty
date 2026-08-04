if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED PYTHON_EXECUTABLE OR NOT DEFINED CHECKER OR NOT DEFINED SOURCE_ROOT)
	message(FATAL_ERROR "RunArchitectureBoundaryStrictCase: TEST_EXECUTABLE, PYTHON_EXECUTABLE, CHECKER, SOURCE_ROOT required")
endif()

# add_test only applies these target properties when its command is the target
# itself. This driver needs captured output, so reproduce that prefix here.
set(test_command)
if(DEFINED TEST_COMMAND_PREFIX AND NOT "${TEST_COMMAND_PREFIX}" STREQUAL "")
	# Test-only injection for proving that unowned stderr remains forbidden.
	# It intentionally does not enable TEST_LAUNCHER's suffix-only policy.
	list(APPEND test_command ${TEST_COMMAND_PREFIX})
endif()

set(has_test_launcher FALSE)
if(DEFINED TEST_LAUNCHER AND NOT "${TEST_LAUNCHER}" STREQUAL "")
	list(APPEND test_command ${TEST_LAUNCHER})
	set(has_test_launcher TRUE)
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

if(has_test_launcher)
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

set(verify_test_launcher_prefix FALSE)
if(DEFINED VERIFY_TEST_LAUNCHER_PREFIX AND VERIFY_TEST_LAUNCHER_PREFIX)
	set(verify_test_launcher_prefix TRUE)
endif()
if(verify_test_launcher_prefix AND NOT has_test_launcher)
	message(FATAL_ERROR "VERIFY_TEST_LAUNCHER_PREFIX requires TEST_LAUNCHER")
endif()

set(expect_extra_stderr_rejection FALSE)
if(DEFINED EXPECT_EXTRA_STDERR_REJECTION AND EXPECT_EXTRA_STDERR_REJECTION)
	set(expect_extra_stderr_rejection TRUE)
endif()
if(expect_extra_stderr_rejection AND has_test_launcher)
	message(FATAL_ERROR "EXPECT_EXTRA_STDERR_REJECTION requires TEST_LAUNCHER to be unset")
endif()
if(expect_extra_stderr_rejection AND (NOT DEFINED TEST_COMMAND_PREFIX OR "${TEST_COMMAND_PREFIX}" STREQUAL ""))
	message(FATAL_ERROR "EXPECT_EXTRA_STDERR_REJECTION requires TEST_COMMAND_PREFIX")
endif()

# Exit code and checker stdout remain exact in both modes.
set(strict_contract_matches FALSE)
if("${actual_exit}" STREQUAL "1" AND "${stdout}" STREQUAL "${expected_stdout}" AND stderr_has_expected_wrapper_message)
	set(strict_contract_matches TRUE)
endif()

set(test_launcher_stderr_prefix "kyty architecture boundary test launcher: prefix\n")
string(CONCAT expected_test_launcher_stderr "${test_launcher_stderr_prefix}" "${expected_stderr}")

if(verify_test_launcher_prefix AND NOT "${stderr}" STREQUAL "${expected_test_launcher_stderr}")
	message(FATAL_ERROR
		"strict boundary launcher prefix contract failed\n"
		"expected stderr:\n${expected_test_launcher_stderr}\nactual stderr:\n${stderr}"
	)
endif()

if(expect_extra_stderr_rejection)
	if(strict_contract_matches OR NOT "${actual_exit}" STREQUAL "1" OR NOT "${stdout}" STREQUAL "${expected_stdout}" OR
	   NOT "${stderr}" STREQUAL "${expected_test_launcher_stderr}")
		message(FATAL_ERROR
			"strict boundary unexpected stderr rejection failed\n"
			"expected exit: 1\nactual exit: ${actual_exit}\n"
			"expected stdout:\n${expected_stdout}\nactual stdout:\n${stdout}\n"
			"expected stderr:\n${expected_test_launcher_stderr}\nactual stderr:\n${stderr}"
		)
	endif()
elseif(NOT strict_contract_matches)
	message(FATAL_ERROR
		"strict boundary contract failed\n"
		"expected exit: 1\nactual exit: ${actual_exit}\n"
		"expected stdout:\n${expected_stdout}\nactual stdout:\n${stdout}\n"
		"expected stderr suffix:\n${expected_stderr}\nactual stderr:\n${stderr}"
	)
endif()
