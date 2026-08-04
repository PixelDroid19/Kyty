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

if(NOT "${actual_exit}" STREQUAL "1" OR NOT "${stdout}" STREQUAL "${expected_stdout}" OR NOT "${stderr}" STREQUAL "${expected_stderr}")
	message(FATAL_ERROR
		"strict boundary contract failed\n"
		"expected exit: 1\nactual exit: ${actual_exit}\n"
		"expected stdout:\n${expected_stdout}\nactual stdout:\n${stdout}\n"
		"expected stderr:\n${expected_stderr}\nactual stderr:\n${stderr}"
	)
endif()
