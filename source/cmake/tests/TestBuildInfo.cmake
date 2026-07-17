# Deterministic build-info generator test.
# Required -D: TEST_BINARY_DIR, PROJECT_SOURCE_DIR
#
# Creates temporary repos under TEST_BINARY_DIR and never mutates the host tree.

if(NOT DEFINED TEST_BINARY_DIR OR NOT DEFINED PROJECT_SOURCE_DIR)
	message(FATAL_ERROR "BUILD_INFO_TEST_INFRA_ERROR: need TEST_BINARY_DIR and PROJECT_SOURCE_DIR")
endif()

find_package(Git QUIET)
if(NOT GIT_EXECUTABLE)
	message(FATAL_ERROR "BUILD_INFO_TEST_INFRA_ERROR: Git not found")
endif()

set(_gen "${PROJECT_SOURCE_DIR}/generate_version.cmake")
set(_in "${PROJECT_SOURCE_DIR}/KytyBuildInfo.h.in")
if(NOT EXISTS "${_gen}" OR NOT EXISTS "${_in}")
	message(FATAL_ERROR "BUILD_INFO_CONTRACT_MISSING: generator or template not present")
endif()

file(REMOVE_RECURSE "${TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")

function(run_gen source_dir out_file)
	execute_process(
		COMMAND "${CMAKE_COMMAND}"
			-DSOURCE_DIR=${source_dir}
			-DINPUT_FILE=${_in}
			-DOUTPUT_FILE=${out_file}
			-DGIT_EXECUTABLE=${GIT_EXECUTABLE}
			-P ${_gen}
		RESULT_VARIABLE _rc
		OUTPUT_VARIABLE _out
		ERROR_VARIABLE _err
	)
	if(NOT _rc EQUAL 0)
		message(FATAL_ERROR "BUILD_INFO_TEST_INFRA_ERROR: generator failed: ${_err}${_out}")
	endif()
endfunction()

function(assert_file_has path needle)
	file(READ "${path}" _content)
	string(FIND "${_content}" "${needle}" _pos)
	if(_pos EQUAL -1)
		message(FATAL_ERROR "BUILD_INFO_CONTRACT_MISSING: expected '${needle}' in ${path}\n${_content}")
	endif()
endfunction()

function(assert_file_lacks path needle)
	file(READ "${path}" _content)
	string(FIND "${_content}" "${needle}" _pos)
	if(NOT _pos EQUAL -1)
		message(FATAL_ERROR "BUILD_INFO_CONTRACT_MISSING: unexpected '${needle}' in ${path}")
	endif()
endfunction()

# --- clean one-commit Git repo ---
set(_clean "${TEST_BINARY_DIR}/clean_repo")
file(MAKE_DIRECTORY "${_clean}")
execute_process(COMMAND "${GIT_EXECUTABLE}" init WORKING_DIRECTORY "${_clean}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
	message(FATAL_ERROR "BUILD_INFO_TEST_INFRA_ERROR: git init clean")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" config user.email "test@example.com" WORKING_DIRECTORY "${_clean}")
execute_process(COMMAND "${GIT_EXECUTABLE}" config user.name "test" WORKING_DIRECTORY "${_clean}")
file(WRITE "${_clean}/readme.txt" "clean\n")
execute_process(COMMAND "${GIT_EXECUTABLE}" add readme.txt WORKING_DIRECTORY "${_clean}" RESULT_VARIABLE _rc)
execute_process(COMMAND "${GIT_EXECUTABLE}" commit -m "init" WORKING_DIRECTORY "${_clean}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
	message(FATAL_ERROR "BUILD_INFO_TEST_INFRA_ERROR: git commit clean")
endif()
execute_process(
	COMMAND "${GIT_EXECUTABLE}" -C "${_clean}" rev-parse HEAD
	OUTPUT_VARIABLE _clean_rev OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(TOLOWER "${_clean_rev}" _clean_rev)
set(_out_clean "${TEST_BINARY_DIR}/clean_KytyBuildInfo.h")
run_gen("${_clean}" "${_out_clean}")
assert_file_has("${_out_clean}" "Revision[] = \"${_clean_rev}\"")
assert_file_has("${_out_clean}" "Dirty = false")
assert_file_has("${_out_clean}" "RevisionKnown = true")
assert_file_lacks("${_out_clean}" "${TEST_BINARY_DIR}")
assert_file_lacks("${_out_clean}" "codex/")
assert_file_lacks("${_out_clean}" "readme.txt")

# --- same repo with untracked file => dirty ---
file(WRITE "${_clean}/untracked.txt" "x\n")
set(_out_dirty "${TEST_BINARY_DIR}/dirty_KytyBuildInfo.h")
run_gen("${_clean}" "${_out_dirty}")
assert_file_has("${_out_dirty}" "Revision[] = \"${_clean_rev}\"")
assert_file_has("${_out_dirty}" "Dirty = true")
assert_file_has("${_out_dirty}" "RevisionKnown = true")
assert_file_lacks("${_out_dirty}" "untracked.txt")

# --- non-Git directory ---
set(_nongit "${TEST_BINARY_DIR}/nongit")
file(MAKE_DIRECTORY "${_nongit}")
file(WRITE "${_nongit}/f.txt" "y\n")
set(_out_nongit "${TEST_BINARY_DIR}/nongit_KytyBuildInfo.h")
run_gen("${_nongit}" "${_out_nongit}")
assert_file_has("${_out_nongit}" "Revision[] = \"0000000000000000000000000000000000000000\"")
assert_file_has("${_out_nongit}" "Dirty = true")
assert_file_has("${_out_nongit}" "RevisionKnown = false")

message(STATUS "TestBuildInfo: OK")
