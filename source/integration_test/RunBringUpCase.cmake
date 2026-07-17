# Process-isolated BringUp scenario runner.
# Scrubs every bring-up and legacy env key first so a parent shell export cannot
# leak into the child, then applies only the scenario's requested variables.

if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED SCENARIO OR NOT DEFINED EXPECTED_EXIT)
	message(FATAL_ERROR "RunBringUpCase: TEST_EXECUTABLE, SCENARIO, and EXPECTED_EXIT are required")
endif()

# Sanitize bring-up policy keys.
unset(ENV{KYTY_BRINGUP_MODE})
unset(ENV{KYTY_BRINGUP_FEATURES})
unset(ENV{KYTY_BRINGUP_SUBSYSTEMS})
unset(ENV{KYTY_BRINGUP_BURST_LIMIT})
unset(ENV{KYTY_BRINGUP_BURST_WINDOW_MS})
unset(ENV{KYTY_BRINGUP_ALLOW_DIAGNOSTIC})

# Sanitize removed legacy flags (must never act as live policy).
unset(ENV{KYTY_STUB_MISSING})
unset(ENV{KYTY_GFX_PERMISSIVE})

if(SCENARIO STREQUAL "unsafe_not_implemented")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
elseif(SCENARIO STREQUAL "disabled_subsystem")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "loader")
elseif(SCENARIO STREQUAL "invalid_configuration")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "not_implemented,unknown_feature")
elseif(SCENARIO STREQUAL "burst_breaker")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_BURST_LIMIT} "3")
	set(ENV{KYTY_BRINGUP_BURST_WINDOW_MS} "1000")
elseif(SCENARIO STREQUAL "slow_repeats")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_BURST_LIMIT} "2")
	set(ENV{KYTY_BRINGUP_BURST_WINDOW_MS} "1")
elseif(SCENARIO STREQUAL "concurrent_burst_accounting")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "not_implemented")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "core")
	set(ENV{KYTY_BRINGUP_BURST_LIMIT} "1")
	set(ENV{KYTY_BRINGUP_BURST_WINDOW_MS} "1000")
elseif(SCENARIO STREQUAL "missing_function_import")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "missing_function_import")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "loader")
elseif(SCENARIO STREQUAL "missing_import_burst_reuse")
	# Prove already-registered stubs are not re-gated by BringUp Report/burst.
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "missing_function_import")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "loader")
	set(ENV{KYTY_BRINGUP_BURST_LIMIT} "1")
	set(ENV{KYTY_BRINGUP_BURST_WINDOW_MS} "1000")
elseif(SCENARIO MATCHES "real_export_wins|non_function_imports_stay_strict|concurrent_deduplication|long_canonical_identities_stay_distinct|stub_capacity_exhaustion")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "missing_function_import")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "loader")
elseif(SCENARIO STREQUAL "graphics_feature_disabled")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "not_implemented")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "graphics")
elseif(SCENARIO STREQUAL "agent_diagnostics")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "not_implemented,missing_function_import,gfx_permissive")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "core,loader,graphics")
elseif(SCENARIO STREQUAL "removed_stub_flag_rejected")
	set(ENV{KYTY_STUB_MISSING} "1")
elseif(SCENARIO STREQUAL "removed_graphics_flag_rejected")
	set(ENV{KYTY_GFX_PERMISSIVE} "1")
endif()

# Empty-string env must be injected via cmake -E env: set(ENV{VAR} "") does not
# reliably pass a zero-length value into execute_process children on all hosts.
if(SCENARIO STREQUAL "empty_mode_rejected")
	execute_process(
		COMMAND ${CMAKE_COMMAND} -E env
			"KYTY_BRINGUP_MODE="
			"${TEST_EXECUTABLE}" "${SCENARIO}"
		RESULT_VARIABLE actual_exit
		OUTPUT_VARIABLE stdout
		ERROR_VARIABLE stderr
	)
elseif(SCENARIO STREQUAL "empty_features_rejected")
	execute_process(
		COMMAND ${CMAKE_COMMAND} -E env
			"KYTY_BRINGUP_MODE=unsafe"
			"KYTY_BRINGUP_FEATURES="
			"${TEST_EXECUTABLE}" "${SCENARIO}"
		RESULT_VARIABLE actual_exit
		OUTPUT_VARIABLE stdout
		ERROR_VARIABLE stderr
	)
elseif(SCENARIO STREQUAL "empty_burst_limit_rejected")
	execute_process(
		COMMAND ${CMAKE_COMMAND} -E env
			"KYTY_BRINGUP_MODE=unsafe"
			"KYTY_BRINGUP_BURST_LIMIT="
			"${TEST_EXECUTABLE}" "${SCENARIO}"
		RESULT_VARIABLE actual_exit
		OUTPUT_VARIABLE stdout
		ERROR_VARIABLE stderr
	)
else()
	execute_process(
		COMMAND "${TEST_EXECUTABLE}" "${SCENARIO}"
		RESULT_VARIABLE actual_exit
		OUTPUT_VARIABLE stdout
		ERROR_VARIABLE stderr
	)
endif()

if(NOT "${actual_exit}" STREQUAL "${EXPECTED_EXIT}")
	message(FATAL_ERROR
		"Scenario ${SCENARIO}: expected exit ${EXPECTED_EXIT}, got ${actual_exit}\n"
		"stdout:\n${stdout}\n"
		"stderr:\n${stderr}"
	)
endif()

message(STATUS "scenario '${SCENARIO}' exit ${actual_exit} (ok)")
