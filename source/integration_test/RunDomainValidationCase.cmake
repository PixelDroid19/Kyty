# Process-isolated domain validation scenario runner.
# No bring-up env leakage required for pure validators; scrub anyway.

if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED SCENARIO OR NOT DEFINED EXPECTED_EXIT)
	message(FATAL_ERROR "RunDomainValidationCase: TEST_EXECUTABLE, SCENARIO, EXPECTED_EXIT required")
endif()

unset(ENV{KYTY_BRINGUP_MODE})
unset(ENV{KYTY_BRINGUP_FEATURES})
unset(ENV{KYTY_BRINGUP_SUBSYSTEMS})
unset(ENV{KYTY_BRINGUP_BURST_LIMIT})
unset(ENV{KYTY_BRINGUP_BURST_WINDOW_MS})
unset(ENV{KYTY_BRINGUP_ALLOW_DIAGNOSTIC})
unset(ENV{KYTY_STUB_MISSING})
unset(ENV{KYTY_GFX_PERMISSIVE})

# Scenario-specific policy env (after scrub).
if(SCENARIO STREQUAL "import_unsupported")
	# Func stubs on → non-Func unresolved is unsupported → process EXIT.
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "missing_function_import")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "loader")
elseif(SCENARIO STREQUAL "import_valid" OR SCENARIO STREQUAL "import_malformed")
	# Allow Resolve host path; malformed identity remains terminal.
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "missing_function_import")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "loader")
elseif(SCENARIO STREQUAL "import_policy_denied")
	# Strict: missing Func is policy_denied (vaddr remains 0).
	# Leave mode unset → strict default.
elseif(SCENARIO STREQUAL "guest_malformed" OR SCENARIO STREQUAL "guest_unsupported")
	# Host init only; no special bring-up.
endif()

execute_process(
	COMMAND "${TEST_EXECUTABLE}" "${SCENARIO}"
	RESULT_VARIABLE actual_exit
	OUTPUT_VARIABLE stdout
	ERROR_VARIABLE stderr
)

if(NOT "${actual_exit}" STREQUAL "${EXPECTED_EXIT}")
	message(FATAL_ERROR
		"Scenario ${SCENARIO}: expected exit ${EXPECTED_EXIT}, got ${actual_exit}\n"
		"stdout:\n${stdout}\n"
		"stderr:\n${stderr}"
	)
endif()

message(STATUS "scenario '${SCENARIO}' exit ${actual_exit} (ok)")
