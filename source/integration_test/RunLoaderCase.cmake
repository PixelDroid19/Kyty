# Process-isolated loader boundary scenario runner.

if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED SCENARIO OR NOT DEFINED EXPECTED_EXIT)
	message(FATAL_ERROR "RunLoaderCase: TEST_EXECUTABLE, SCENARIO, and EXPECTED_EXIT are required")
endif()

unset(ENV{KYTY_BRINGUP_MODE})
unset(ENV{KYTY_BRINGUP_FEATURES})
unset(ENV{KYTY_BRINGUP_SUBSYSTEMS})
unset(ENV{KYTY_BRINGUP_BURST_LIMIT})
unset(ENV{KYTY_BRINGUP_BURST_WINDOW_MS})
unset(ENV{KYTY_BRINGUP_ALLOW_DIAGNOSTIC})
unset(ENV{KYTY_STUB_MISSING})
unset(ENV{KYTY_GFX_PERMISSIVE})

# Default: strict (discovery off). Specific scenarios enable unsafe + feature.
if(SCENARIO STREQUAL "primary_only_plan")
	# Strict defaults — discovery off.
elseif(SCENARIO STREQUAL "discovery_feature_off")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "not_implemented")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "loader")
elseif(SCENARIO STREQUAL "locator_primary_names")
	# Strict; pure name checks.
elseif(SCENARIO MATCHES "multi_module_stable_order|duplicate_identity_rejected|fail_before_mutate|agent_load_plan_diagnostics|apply_export_conflict_rollback|discovery_capacity_rejects_whole_plan|adjacent_apply_deferred_until_hle")
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "adjacent_module_discovery")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "loader")
elseif(SCENARIO MATCHES "module_export_wins_over_hle|export_conflict_reported")
	# HLE/export scans need a live linker; discovery feature not required.
	set(ENV{KYTY_BRINGUP_MODE} "unsafe")
	set(ENV{KYTY_BRINGUP_FEATURES} "not_implemented")
	set(ENV{KYTY_BRINGUP_SUBSYSTEMS} "loader")
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
	message(FATAL_ERROR
		"scenario ${SCENARIO}: expected exit ${EXPECTED_EXIT}, got ${actual_exit}")
endif()
