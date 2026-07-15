# Generate sanitized build provenance into KytyBuildInfo.h.
# Inputs (via -D): SOURCE_DIR, INPUT_FILE, OUTPUT_FILE, GIT_EXECUTABLE (optional).
#
# Contract:
# - Revision is exactly 40 lowercase hex characters when known.
# - Dirty is true if tracked changes or untracked files exist.
# - Without Git/repo: Revision=40 zeros, Dirty=true, RevisionKnown=false.
# - Never embeds paths, branch names, or raw status text.

if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
	message(FATAL_ERROR "generate_version.cmake requires SOURCE_DIR")
endif()
if(NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE)
	message(FATAL_ERROR "generate_version.cmake requires INPUT_FILE and OUTPUT_FILE")
endif()

set(KYTY_BUILD_REVISION "0000000000000000000000000000000000000000")
set(KYTY_BUILD_DIRTY "true")
set(KYTY_BUILD_REVISION_KNOWN "false")

if(GIT_EXECUTABLE)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --verify HEAD
		RESULT_VARIABLE _rev_rc
		OUTPUT_VARIABLE _rev_out
		ERROR_VARIABLE _rev_err
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(_rev_rc EQUAL 0)
		string(TOLOWER "${_rev_out}" _rev_out)
		string(REGEX MATCH "^[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]$" _rev_ok "${_rev_out}")
		if(_rev_ok)
			set(KYTY_BUILD_REVISION "${_rev_out}")
			set(KYTY_BUILD_REVISION_KNOWN "true")
			execute_process(
				COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" status --porcelain --untracked-files=normal
				RESULT_VARIABLE _st_rc
				OUTPUT_VARIABLE _st_out
				ERROR_VARIABLE _st_err
				OUTPUT_STRIP_TRAILING_WHITESPACE
			)
			if(_st_rc EQUAL 0)
				if(_st_out STREQUAL "")
					set(KYTY_BUILD_DIRTY "false")
				else()
					set(KYTY_BUILD_DIRTY "true")
				endif()
			else()
				set(KYTY_BUILD_DIRTY "true")
			endif()
		endif()
	endif()
endif()

configure_file("${INPUT_FILE}" "${OUTPUT_FILE}" @ONLY)
