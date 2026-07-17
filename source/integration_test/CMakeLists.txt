# Process-isolated bring-up policy integration tests.
# Built only when BUILD_TESTING is ON (see source/CMakeLists.txt).

add_executable(kyty_bringup_integration
	src/BringUpIntegration.cpp
)

target_link_libraries(kyty_bringup_integration
	PRIVATE
		core
		emulator
)

target_include_directories(kyty_bringup_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

# Force a rebuild of the integration binary before any scenario runs so a
# stale executable cannot satisfy CTest after source edits.
add_test(
	NAME KytyBringUpIntegration._build
	COMMAND "${CMAKE_COMMAND}"
		--build "${CMAKE_BINARY_DIR}"
		--target kyty_bringup_integration
		--config $<CONFIG>
)
set_tests_properties(KytyBringUpIntegration._build PROPERTIES
	FIXTURES_SETUP KytyBringUpIntegrationBinary
	TIMEOUT 600
)

function(add_kyty_bringup_case name scenario expected_exit)
	add_test(
		NAME "KytyBringUpIntegration.${name}"
		COMMAND
			"${CMAKE_COMMAND}"
			-DTEST_EXECUTABLE=$<TARGET_FILE:kyty_bringup_integration>
			-DSCENARIO=${scenario}
			-DEXPECTED_EXIT=${expected_exit}
			-P "${CMAKE_CURRENT_SOURCE_DIR}/RunBringUpCase.cmake"
	)
	set_tests_properties("KytyBringUpIntegration.${name}" PROPERTIES
		TIMEOUT 120
		FIXTURES_REQUIRED KytyBringUpIntegrationBinary
	)
endfunction()

# Unix wait status is 8-bit: dbg_exit(321) is observed as 321 & 0xff == 65.
set(KYTY_ABORT_RC 65)

add_kyty_bringup_case(StrictNotImplemented strict_not_implemented ${KYTY_ABORT_RC})
add_kyty_bringup_case(UnsafeNotImplemented unsafe_not_implemented 0)
# Policy Halt without process abort (feature/subsystem gate decision + counters).
add_kyty_bringup_case(DisabledSubsystem disabled_subsystem 0)
add_kyty_bringup_case(InvalidConfiguration invalid_configuration 125)
add_kyty_bringup_case(BurstBreaker burst_breaker ${KYTY_ABORT_RC})
add_kyty_bringup_case(SlowRepeats slow_repeats 0)
add_kyty_bringup_case(MissingFunctionImport missing_function_import 0)
# BURST_LIMIT=1: duplicate resolve must reuse registry without re-Report/EXIT.
add_kyty_bringup_case(MissingImportBurstReuse missing_import_burst_reuse 0)
add_kyty_bringup_case(RealExportWins real_export_wins 0)
add_kyty_bringup_case(NonFunctionImportsStayStrict non_function_imports_stay_strict 0)
add_kyty_bringup_case(ConcurrentDeduplication concurrent_deduplication 0)
add_kyty_bringup_case(ConcurrentBurstAccounting concurrent_burst_accounting 0)
add_kyty_bringup_case(LongCanonicalIdentitiesStayDistinct long_canonical_identities_stay_distinct 0)
add_kyty_bringup_case(StubCapacityExhaustion stub_capacity_exhaustion ${KYTY_ABORT_RC})
set_tests_properties(KytyBringUpIntegration.StubCapacityExhaustion PROPERTIES TIMEOUT 300)
add_kyty_bringup_case(GraphicsFeatureDisabled graphics_feature_disabled 0)
add_kyty_bringup_case(AgentDiagnostics agent_diagnostics 0)
add_kyty_bringup_case(RemovedStubFlagRejected removed_stub_flag_rejected 125)
add_kyty_bringup_case(RemovedGraphicsFlagRejected removed_graphics_flag_rejected 125)
# Empty env values are malformed config (fail closed), not "unset".
add_kyty_bringup_case(EmptyModeRejected empty_mode_rejected 125)
add_kyty_bringup_case(EmptyFeaturesRejected empty_features_rejected 125)
add_kyty_bringup_case(EmptyBurstLimitRejected empty_burst_limit_rejected 125)

# --- Domain validation architecture (pure validators, four outcome classes) ---

add_executable(kyty_domain_validation_integration
	src/DomainValidationIntegration.cpp
)

target_link_libraries(kyty_domain_validation_integration
	PRIVATE
		core
		emulator
)

target_include_directories(kyty_domain_validation_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyDomainValidation._build
	COMMAND "${CMAKE_COMMAND}"
		--build "${CMAKE_BINARY_DIR}"
		--target kyty_domain_validation_integration
		--config $<CONFIG>
)
set_tests_properties(KytyDomainValidation._build PROPERTIES
	FIXTURES_SETUP KytyDomainValidationBinary
	TIMEOUT 600
)

add_executable(kyty_kernel_memory_integration
	src/KernelMemoryIntegration.cpp
)

target_link_libraries(kyty_kernel_memory_integration
	PRIVATE
		core
		emulator
)

target_include_directories(kyty_kernel_memory_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyKernelMemoryIntegration._build
	COMMAND "${CMAKE_COMMAND}"
		--build "${CMAKE_BINARY_DIR}"
		--target kyty_kernel_memory_integration
		--config $<CONFIG>
)
set_tests_properties(KytyKernelMemoryIntegration._build PROPERTIES
	FIXTURES_SETUP KytyKernelMemoryIntegrationBinary
	TIMEOUT 600
)

add_test(
	NAME KytyKernelMemoryIntegration.ReserveVirtualRange
	COMMAND $<TARGET_FILE:kyty_kernel_memory_integration>
)
set_tests_properties(KytyKernelMemoryIntegration.ReserveVirtualRange PROPERTIES
	FIXTURES_REQUIRED KytyKernelMemoryIntegrationBinary
	TIMEOUT 120
)

add_executable(kyty_graphics_diagnostics_integration
	src/GraphicsDiagnosticsIntegration.cpp
)

target_link_libraries(kyty_graphics_diagnostics_integration
	PRIVATE
		core
		emulator
)

target_include_directories(kyty_graphics_diagnostics_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyGraphicsDiagnosticsIntegration._build
	COMMAND "${CMAKE_COMMAND}"
		--build "${CMAKE_BINARY_DIR}"
		--target kyty_graphics_diagnostics_integration
		--config $<CONFIG>
)
set_tests_properties(KytyGraphicsDiagnosticsIntegration._build PROPERTIES
	FIXTURES_SETUP KytyGraphicsDiagnosticsIntegrationBinary
	TIMEOUT 600
)

add_test(
	NAME KytyGraphicsDiagnosticsIntegration.SerializationAndBounds
	COMMAND $<TARGET_FILE:kyty_graphics_diagnostics_integration>
)
set_tests_properties(KytyGraphicsDiagnosticsIntegration.SerializationAndBounds PROPERTIES
	FIXTURES_REQUIRED KytyGraphicsDiagnosticsIntegrationBinary
	TIMEOUT 120
)

add_executable(kyty_libc_wide_integration
	src/LibcWideIntegration.cpp
)

target_link_libraries(kyty_libc_wide_integration
	PRIVATE
		core
		emulator
)

target_include_directories(kyty_libc_wide_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyLibcWideIntegration._build
	COMMAND "${CMAKE_COMMAND}"
		--build "${CMAKE_BINARY_DIR}"
		--target kyty_libc_wide_integration
		--config $<CONFIG>
)
set_tests_properties(KytyLibcWideIntegration._build PROPERTIES
	FIXTURES_SETUP KytyLibcWideIntegrationBinary
	TIMEOUT 600
)

add_test(
	NAME KytyLibcWideIntegration.ExactImports
	COMMAND $<TARGET_FILE:kyty_libc_wide_integration>
)
set_tests_properties(KytyLibcWideIntegration.ExactImports PROPERTIES
	FIXTURES_REQUIRED KytyLibcWideIntegrationBinary
	TIMEOUT 120
)

foreach(scenario IN ITEMS reject_class reject_non_ascii)
	if(scenario STREQUAL "reject_class")
		set(expected_diagnostic "character_class != 2")
	else()
		set(expected_diagnostic "character > 0x7f")
	endif()
	add_test(
		NAME "KytyLibcWideIntegration.${scenario}"
		COMMAND "${CMAKE_COMMAND}"
			-DTEST_EXECUTABLE=$<TARGET_FILE:kyty_libc_wide_integration>
			-DSCENARIO=${scenario}
			"-DEXPECTED_DIAGNOSTIC=${expected_diagnostic}"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/RunLibcWideRejection.cmake"
	)
	set_tests_properties("KytyLibcWideIntegration.${scenario}" PROPERTIES
		FIXTURES_REQUIRED KytyLibcWideIntegrationBinary
		TIMEOUT 120
	)
endforeach()

# The real shared host→guest call boundary must terminate the guest RBP chain
# before the native host tail. Exercise both main-style and pthread-style ABI
# argument shapes through that production boundary.
add_executable(kyty_guest_stack_frame_integration
	src/GuestStackFrameIntegration.cpp
)

target_link_libraries(kyty_guest_stack_frame_integration
	PRIVATE
		emulator
)

target_include_directories(kyty_guest_stack_frame_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
)

add_test(
	NAME KytyGuestStackFrameIntegration._build
	COMMAND "${CMAKE_COMMAND}"
		--build "${CMAKE_BINARY_DIR}"
		--target kyty_guest_stack_frame_integration
		--config $<CONFIG>
)
set_tests_properties(KytyGuestStackFrameIntegration._build PROPERTIES
	FIXTURES_SETUP KytyGuestStackFrameIntegrationBinary
	TIMEOUT 600
)

add_test(
	NAME KytyGuestStackFrameIntegration.SharedGuestCallBoundary
	COMMAND $<TARGET_FILE:kyty_guest_stack_frame_integration>
)
set_tests_properties(KytyGuestStackFrameIntegration.SharedGuestCallBoundary PROPERTIES
	FIXTURES_REQUIRED KytyGuestStackFrameIntegrationBinary
	TIMEOUT 60
)

add_executable(kyty_libc_allocator_integration
	src/LibcAllocatorIntegration.cpp
)

target_link_libraries(kyty_libc_allocator_integration
	PRIVATE
		emulator
)

target_include_directories(kyty_libc_allocator_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyLibcAllocatorIntegration._build
	COMMAND "${CMAKE_COMMAND}"
		--build "${CMAKE_BINARY_DIR}"
		--target kyty_libc_allocator_integration
		--config $<CONFIG>
)
set_tests_properties(KytyLibcAllocatorIntegration._build PROPERTIES
	FIXTURES_SETUP KytyLibcAllocatorIntegrationBinary
	TIMEOUT 600
)

add_test(
	NAME KytyLibcAllocatorIntegration.RawAndAlignedOwnership
	COMMAND $<TARGET_FILE:kyty_libc_allocator_integration>
)
set_tests_properties(KytyLibcAllocatorIntegration.RawAndAlignedOwnership PROPERTIES
	FIXTURES_REQUIRED KytyLibcAllocatorIntegrationBinary
	TIMEOUT 120
)

function(add_kyty_domain_validation_case name scenario expected_exit)
	add_test(
		NAME "KytyDomainValidation.${name}"
		COMMAND
			"${CMAKE_COMMAND}"
			-DTEST_EXECUTABLE=$<TARGET_FILE:kyty_domain_validation_integration>
			-DSCENARIO=${scenario}
			-DEXPECTED_EXIT=${expected_exit}
			-P "${CMAKE_CURRENT_SOURCE_DIR}/RunDomainValidationCase.cmake"
	)
	set_tests_properties("KytyDomainValidation.${name}" PROPERTIES
		TIMEOUT 60
		FIXTURES_REQUIRED KytyDomainValidationBinary
	)
endfunction()

# Unix halt from LoadProgram/Resolve EXIT → dbg_exit(321) → 65
set(KYTY_DOMAIN_ABORT_RC 65)

# Config (BringUp::ValidateConfig — pure; also used by InitializeFromEnvironment)
add_kyty_domain_validation_case(ConfigValid config_valid 0)
add_kyty_domain_validation_case(ConfigMalformed config_malformed 0)
add_kyty_domain_validation_case(ConfigEmptyMode config_empty_mode 0)
add_kyty_domain_validation_case(ConfigEmptyFeatures config_empty_features 0)
add_kyty_domain_validation_case(ConfigUnsupported config_unsupported 0)
add_kyty_domain_validation_case(ConfigPolicyDenied config_policy_denied 0)
# Agent: real ParseAndValidateRequestLine
add_kyty_domain_validation_case(AgentValid agent_valid 0)
add_kyty_domain_validation_case(AgentMalformed agent_malformed 0)
add_kyty_domain_validation_case(AgentUnsupported agent_unsupported 0)
add_kyty_domain_validation_case(AgentPolicyDenied agent_policy_denied 0)
# Import: real RuntimeLinker::Resolve (feature env per scenario via runner)
# Guest: real LoadProgram path validation (abort on bad path)
add_kyty_domain_validation_case(GuestValid guest_valid 0)
add_kyty_domain_validation_case(GuestMalformed guest_malformed ${KYTY_DOMAIN_ABORT_RC})
add_kyty_domain_validation_case(GuestUnsupported guest_unsupported ${KYTY_DOMAIN_ABORT_RC})
# Module metadata via real LoadProgram basename rules (.prx)
add_kyty_domain_validation_case(ModuleValid module_valid 0)
add_kyty_domain_validation_case(ModuleMalformed module_malformed ${KYTY_DOMAIN_ABORT_RC})
add_kyty_domain_validation_case(ModuleUnsupported module_unsupported ${KYTY_DOMAIN_ABORT_RC})
# Import resolution
add_kyty_domain_validation_case(ImportValid import_valid 0)
add_kyty_domain_validation_case(ImportMalformed import_malformed ${KYTY_DOMAIN_ABORT_RC})
# import_unsupported proves Object resolution remains typed/unresolved even
# when diagnostic function stubs are enabled; relocation owns weak/strong
# binding policy.
# import_policy_denied needs feature OFF
add_kyty_domain_validation_case(ImportUnsupported import_unsupported 0)
add_kyty_domain_validation_case(ImportPolicyDenied import_policy_denied 0)
# Game-run (ValidateGameRunRequest used by kyty_mount)
add_kyty_domain_validation_case(GameRunValid gamerun_valid 0)
add_kyty_domain_validation_case(GameRunMalformed gamerun_malformed 0)
add_kyty_domain_validation_case(GameRunUnsupported gamerun_unsupported 0)
add_kyty_domain_validation_case(GameRunPolicyDenied gamerun_policy_denied 0)

# --- Loader boundaries: plan-before-mutate adjacent discovery ---

add_executable(kyty_loader_integration
	src/LoaderIntegration.cpp
)

target_link_libraries(kyty_loader_integration
	PRIVATE
		core
		emulator
)

target_include_directories(kyty_loader_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyLoaderIntegration._build
	COMMAND "${CMAKE_COMMAND}"
		--build "${CMAKE_BINARY_DIR}"
		--target kyty_loader_integration
		--config $<CONFIG>
)
set_tests_properties(KytyLoaderIntegration._build PROPERTIES
	FIXTURES_SETUP KytyLoaderIntegrationBinary
	TIMEOUT 600
)

function(add_kyty_loader_case name scenario expected_exit)
	add_test(
		NAME "KytyLoaderIntegration.${name}"
		COMMAND
			"${CMAKE_COMMAND}"
			-DTEST_EXECUTABLE=$<TARGET_FILE:kyty_loader_integration>
			-DSCENARIO=${scenario}
			-DEXPECTED_EXIT=${expected_exit}
			-P "${CMAKE_CURRENT_SOURCE_DIR}/RunLoaderCase.cmake"
	)
	set_tests_properties("KytyLoaderIntegration.${name}" PROPERTIES
		TIMEOUT 60
		FIXTURES_REQUIRED KytyLoaderIntegrationBinary
	)
endfunction()

add_kyty_loader_case(PrimaryOnlyPlan primary_only_plan 0)
add_kyty_loader_case(MultiModuleStableOrder multi_module_stable_order 0)
add_kyty_loader_case(SymlinkEscapeRejected symlink_escape_rejected 0)
add_kyty_loader_case(DuplicateIdentityRejected duplicate_identity_rejected 0)
add_kyty_loader_case(DiscoveryFeatureOff discovery_feature_off 0)
add_kyty_loader_case(FailBeforeMutate fail_before_mutate 0)
add_kyty_loader_case(AgentLoadPlanDiagnostics agent_load_plan_diagnostics 0)
add_kyty_loader_case(LocatorPrimaryNames locator_primary_names 0)
add_kyty_loader_case(HleWinsOverModuleExport hle_wins_over_module_export 0)
add_kyty_loader_case(ExportConflictReported export_conflict_reported 0)
add_kyty_loader_case(LongExportConflictsStayDistinct long_export_conflicts_stay_distinct 0)
add_kyty_loader_case(ExportSnapshotOutlivesModule export_snapshot_outlives_module 0)
add_kyty_loader_case(ApplyExportConflictRollback apply_export_conflict_rollback 0)
add_kyty_loader_case(DiscoveryCapacityRejectsWholePlan discovery_capacity_rejects_whole_plan 0)
add_kyty_loader_case(AdjacentApplyDeferredUntilHle adjacent_apply_deferred_until_hle 0)

# --- Agent protocol: single wire version, events, fail-closed ---

add_executable(kyty_agent_protocol_integration
	src/AgentProtocolIntegration.cpp
)

target_link_libraries(kyty_agent_protocol_integration
	PRIVATE
		core
		emulator
)

target_include_directories(kyty_agent_protocol_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyAgentProtocol._build
	COMMAND "${CMAKE_COMMAND}"
		--build "${CMAKE_BINARY_DIR}"
		--target kyty_agent_protocol_integration
		--config $<CONFIG>
)
set_tests_properties(KytyAgentProtocol._build PROPERTIES
	FIXTURES_SETUP KytyAgentProtocolBinary
	TIMEOUT 600
)

function(add_kyty_agent_protocol_case name scenario expected_exit)
	add_test(
		NAME "KytyAgentProtocol.${name}"
		COMMAND
			"${CMAKE_COMMAND}"
			-DTEST_EXECUTABLE=$<TARGET_FILE:kyty_agent_protocol_integration>
			-DSCENARIO=${scenario}
			-DEXPECTED_EXIT=${expected_exit}
			-P "${CMAKE_CURRENT_SOURCE_DIR}/RunAgentProtocolCase.cmake"
	)
	set_tests_properties("KytyAgentProtocol.${name}" PROPERTIES
		TIMEOUT 60
		FIXTURES_REQUIRED KytyAgentProtocolBinary
	)
endfunction()

add_kyty_agent_protocol_case(ProtocolVersionConsistent protocol_version_consistent 0)
add_kyty_agent_protocol_case(MalformedFailClosed malformed_fail_closed 0)
add_kyty_agent_protocol_case(JsonEscapingComplete json_escaping_complete 0)
add_kyty_agent_protocol_case(DiagnosticsResponseBounded diagnostics_response_bounded 0)
add_kyty_agent_protocol_case(EventRingOverflowVisible event_ring_overflow_visible 0)
add_kyty_agent_protocol_case(LifecycleEventsSanitize lifecycle_events_sanitize 0)
add_kyty_agent_protocol_case(SeqMonotonic seq_monotonic 0)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
	NAME KytyGamesMatrixIntegration.ProcessBoundaries
	COMMAND
		"${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/TestGamesMatrix.py"
)
set_tests_properties(KytyGamesMatrixIntegration.ProcessBoundaries PROPERTIES
	TIMEOUT 30
)

add_test(
	NAME KytyAgentProtocol.WaitReadySingleResponse
	COMMAND
		"${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/TestAgentWaitReady.py"
		$<TARGET_FILE:kyty_agent>
)
set_tests_properties(KytyAgentProtocol.WaitReadySingleResponse PROPERTIES
	TIMEOUT 15
	FIXTURES_REQUIRED KytyAgentProtocolBinary
)

# Every fixture rebuilds a target that ultimately writes shared archives in the
# same build tree. Keep those setup phases mutually exclusive under `ctest -j`;
# scenario processes remain free to run concurrently after their fixture build.
set_tests_properties(
	KytyBringUpIntegration._build
	KytyDomainValidation._build
	KytyKernelMemoryIntegration._build
	KytyGraphicsDiagnosticsIntegration._build
	KytyLibcWideIntegration._build
	KytyGuestStackFrameIntegration._build
	KytyLibcAllocatorIntegration._build
	KytyLoaderIntegration._build
	KytyAgentProtocol._build
	PROPERTIES
		RESOURCE_LOCK KytyIntegrationBuildTree
)
