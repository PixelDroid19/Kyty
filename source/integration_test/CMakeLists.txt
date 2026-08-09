# Process-isolated bring-up policy integration tests.
# Built only when BUILD_TESTING is ON (see source/CMakeLists.txt).

add_executable(kyty_bringup_integration
	src/BringUpIntegration.cpp
)

target_link_libraries(kyty_bringup_integration
	PRIVATE
		core
		emulator)

target_include_directories(kyty_bringup_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
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
		FIXTURES_REQUIRED KytyIntegrationBinaries
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
		emulator)

target_include_directories(kyty_domain_validation_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_executable(kyty_kernel_memory_integration
	src/KernelMemoryIntegration.cpp
)

target_link_libraries(kyty_kernel_memory_integration
	PRIVATE
		core
		emulator)

target_include_directories(kyty_kernel_memory_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyKernelMemoryIntegration.ReserveVirtualRange
	COMMAND $<TARGET_FILE:kyty_kernel_memory_integration>
)
set_tests_properties(KytyKernelMemoryIntegration.ReserveVirtualRange PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
	TIMEOUT 120
)

add_executable(kyty_graphics_diagnostics_integration
	src/GraphicsDiagnosticsIntegration.cpp
)

target_link_libraries(kyty_graphics_diagnostics_integration
	PRIVATE
		core
		emulator)

target_include_directories(kyty_graphics_diagnostics_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyGraphicsDiagnosticsIntegration.SerializationAndBounds
	COMMAND $<TARGET_FILE:kyty_graphics_diagnostics_integration>
)
set_tests_properties(KytyGraphicsDiagnosticsIntegration.SerializationAndBounds PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
	TIMEOUT 120
)

add_executable(kyty_libc_wide_integration
	src/LibcWideIntegration.cpp
)

target_link_libraries(kyty_libc_wide_integration
	PRIVATE
		core
		emulator)

target_include_directories(kyty_libc_wide_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyLibcWideIntegration.ExactImports
	COMMAND $<TARGET_FILE:kyty_libc_wide_integration>
)
set_tests_properties(KytyLibcWideIntegration.ExactImports PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
	TIMEOUT 120
)

add_executable(kyty_libc_time_integration
	src/LibcTimeIntegration.cpp
)

target_link_libraries(kyty_libc_time_integration
	PRIVATE
		emulator)

target_include_directories(kyty_libc_time_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyLibcTimeIntegration.GuestAbi
	COMMAND $<TARGET_FILE:kyty_libc_time_integration>
)
set_tests_properties(KytyLibcTimeIntegration.GuestAbi PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
	TIMEOUT 120
)

add_executable(kyty_libc_locale_integration
	src/LibcLocaleIntegration.cpp
)

target_link_libraries(kyty_libc_locale_integration
	PRIVATE
		emulator)

target_include_directories(kyty_libc_locale_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyLibcLocaleIntegration.ClassificationContract
	COMMAND $<TARGET_FILE:kyty_libc_locale_integration>
)
set_tests_properties(KytyLibcLocaleIntegration.ClassificationContract PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
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
		FIXTURES_REQUIRED KytyIntegrationBinaries
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
		emulator)

target_include_directories(kyty_guest_stack_frame_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
)

add_test(
	NAME KytyGuestStackFrameIntegration.SharedGuestCallBoundary
	COMMAND $<TARGET_FILE:kyty_guest_stack_frame_integration>
)
set_tests_properties(KytyGuestStackFrameIntegration.SharedGuestCallBoundary PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
	TIMEOUT 60
)

add_executable(kyty_libc_allocator_integration
	src/LibcAllocatorIntegration.cpp
)

target_link_libraries(kyty_libc_allocator_integration
	PRIVATE
		emulator)

target_include_directories(kyty_libc_allocator_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyLibcAllocatorIntegration.RawAndAlignedOwnership
	COMMAND $<TARGET_FILE:kyty_libc_allocator_integration>
)
set_tests_properties(KytyLibcAllocatorIntegration.RawAndAlignedOwnership PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
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
		FIXTURES_REQUIRED KytyIntegrationBinaries
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
		emulator)

target_include_directories(kyty_loader_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
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
		FIXTURES_REQUIRED KytyIntegrationBinaries
	)
endfunction()

add_kyty_loader_case(PrimaryOnlyPlan primary_only_plan 0)
add_kyty_loader_case(MultiModuleStableOrder multi_module_stable_order 0)
add_kyty_loader_case(SymlinkEscapeRejected symlink_escape_rejected 0)
add_kyty_loader_case(DuplicateIdentityRejected duplicate_identity_rejected 0)
add_kyty_loader_case(DiscoveryFeatureOff discovery_feature_off 0)
add_kyty_loader_case(FailBeforeMutate fail_before_mutate 0)
add_kyty_loader_case(PlatformRevalidationRejectsChange platform_revalidation_rejects_change 0)
add_kyty_loader_case(AgentLoadPlanDiagnostics agent_load_plan_diagnostics 0)
add_kyty_loader_case(LocatorPrimaryNames locator_primary_names 0)
add_kyty_loader_case(ModuleExportWinsOverHle module_export_wins_over_hle 0)
add_kyty_loader_case(ExportConflictReported export_conflict_reported 0)
add_kyty_loader_case(LongExportConflictsStayDistinct long_export_conflicts_stay_distinct 0)
add_kyty_loader_case(ExportSnapshotOutlivesModule export_snapshot_outlives_module 0)
add_kyty_loader_case(ApplyExportConflictRollback apply_export_conflict_rollback 0)
add_kyty_loader_case(PluginExportConflictSkipsCandidate plugin_export_conflict_skips_candidate 0)
add_kyty_loader_case(DiscoveryCapacityRejectsWholePlan discovery_capacity_rejects_whole_plan 0)
add_kyty_loader_case(AdjacentApplyDeferredUntilHle adjacent_apply_deferred_until_hle 0)
add_kyty_loader_case(HleModuleOwnershipIsExact hle_module_ownership_is_exact 0)

# --- Agent protocol: single wire version, events, fail-closed ---

add_executable(kyty_agent_protocol_integration
	src/AgentProtocolIntegration.cpp
)

target_link_libraries(kyty_agent_protocol_integration
	PRIVATE
		core
		emulator)

target_include_directories(kyty_agent_protocol_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
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
		FIXTURES_REQUIRED KytyIntegrationBinaries
	)
endfunction()

add_kyty_agent_protocol_case(ProtocolVersionConsistent protocol_version_consistent 0)
add_kyty_agent_protocol_case(DebugSnapshotBounded debug_snapshot_bounded 0)
add_kyty_agent_protocol_case(MalformedFailClosed malformed_fail_closed 0)
add_kyty_agent_protocol_case(JsonEscapingComplete json_escaping_complete 0)
add_kyty_agent_protocol_case(DiagnosticsResponseBounded diagnostics_response_bounded 0)
add_kyty_agent_protocol_case(EventRingOverflowVisible event_ring_overflow_visible 0)
add_kyty_agent_protocol_case(LifecycleEventsSanitize lifecycle_events_sanitize 0)
add_kyty_agent_protocol_case(SeqMonotonic seq_monotonic 0)

add_executable(kyty_agent_transport_integration
	src/AgentTransportIntegration.cpp
)

target_link_libraries(kyty_agent_transport_integration
	PRIVATE
		kyty_agent_cli
		kyty_agent_transport
		emulator)

target_include_directories(kyty_agent_transport_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyAgentTransport.NativeRoundTrip
	COMMAND $<TARGET_FILE:kyty_agent_transport_integration>
)

add_executable(kyty_audio_pcm_integration
	src/AudioPcmIntegration.cpp
)

target_link_libraries(kyty_audio_pcm_integration
	PRIVATE
		kyty_audio_pcm
)

add_test(
	NAME KytyAudioPcmIntegration.Contract
	COMMAND $<TARGET_FILE:kyty_audio_pcm_integration>
)
set_tests_properties(KytyAudioPcmIntegration.Contract PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
	TIMEOUT 15
)

add_executable(kyty_audio_host_integration
	src/AudioHostIntegration.cpp
)

target_link_libraries(kyty_audio_host_integration
	PRIVATE
		emulator)

target_include_directories(kyty_audio_host_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyAudioHostIntegration.PacingAndLifecycle
	COMMAND $<TARGET_FILE:kyty_audio_host_integration>
)
set_tests_properties(KytyAudioHostIntegration.PacingAndLifecycle PROPERTIES
	ENVIRONMENT "SDL_AUDIODRIVER=disk;SDL_DISKAUDIOFILE=/tmp/kyty-audio-host-integration.raw"
	FIXTURES_REQUIRED KytyIntegrationBinaries
	TIMEOUT 30
)

add_executable(kyty_audio_video_memory_integration
	src/AudioVideoMemoryIntegration.cpp
)

target_link_libraries(kyty_audio_video_memory_integration
	PRIVATE
		emulator)

target_include_directories(kyty_audio_video_memory_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyAudioVideoMemoryIntegration.CallbackContract
	COMMAND $<TARGET_FILE:kyty_audio_video_memory_integration>
)
set_tests_properties(KytyAudioVideoMemoryIntegration.CallbackContract PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
	TIMEOUT 15
)

add_executable(kyty_host_platform_integration
	src/HostPlatformIntegration.cpp
)

target_link_libraries(kyty_host_platform_integration
	PRIVATE
		emulator)

target_include_directories(kyty_host_platform_integration
	PRIVATE
		"${CMAKE_SOURCE_DIR}/include"
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

add_test(
	NAME KytyHostPlatformIntegration.CaptureQueries
	COMMAND $<TARGET_FILE:kyty_host_platform_integration>
)
set_tests_properties(KytyHostPlatformIntegration.CaptureQueries PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
	TIMEOUT 15
)

set_tests_properties(KytyAgentTransport.NativeRoundTrip PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
	TIMEOUT 15
)

add_executable(kyty_architecture_boundary_integration
	src/ArchitectureBoundaryIntegration.cpp
)
if(WIN32)
	target_link_libraries(kyty_architecture_boundary_integration PRIVATE shell32)
endif()

set(KYTY_ARCHITECTURE_BOUNDARY_CHECKER "${CMAKE_SOURCE_DIR}/../scripts/check_emulator_boundaries.py")

add_test(
	NAME KytyArchitectureBoundaryChecker.SyntheticFixtures
	COMMAND "${Python3_EXECUTABLE}" "${KYTY_ARCHITECTURE_BOUNDARY_CHECKER}" --self-test
)
set_tests_properties(KytyArchitectureBoundaryChecker.SyntheticFixtures PROPERTIES TIMEOUT 15)

add_test(
	NAME KytyGraphicsTables.Provenance
	COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../scripts/check_graphics_tables.py"
		"${CMAKE_SOURCE_DIR}/emulator/src/Graphics/Tables/manifest.sha256"
)
set_tests_properties(KytyGraphicsTables.Provenance PROPERTIES TIMEOUT 15)

add_test(
	NAME KytyGraphicsTables.SyntheticFixtures
	COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../scripts/check_graphics_tables.py" --self-test
)
set_tests_properties(KytyGraphicsTables.SyntheticFixtures PROPERTIES TIMEOUT 15)

set(KYTY_GRAPHICS_TABLES_GENERATOR "${CMAKE_SOURCE_DIR}/../scripts/generate_graphics_tables.py")

add_test(
	NAME KytyGraphicsTables.Regeneration
	COMMAND "${Python3_EXECUTABLE}" "${KYTY_GRAPHICS_TABLES_GENERATOR}" --self-test
)
set_tests_properties(KytyGraphicsTables.Regeneration PROPERTIES TIMEOUT 15)

add_test(
	NAME KytyGraphicsTables.DerivedOutputsMatch
	COMMAND "${Python3_EXECUTABLE}" "${KYTY_GRAPHICS_TABLES_GENERATOR}" --check
)
set_tests_properties(KytyGraphicsTables.DerivedOutputsMatch PROPERTIES TIMEOUT 15)

# The normal build consumes the checked-in SPIR-V include and intentionally
# does not require shader tools. When a developer or CI host has both tools,
# expose exact source/blob validation through CTest instead.
find_program(KYTY_GEN5_DETILE_GLSLC NAMES glslc)
find_program(KYTY_GEN5_DETILE_SPIRV_VAL NAMES spirv-val)
if(KYTY_GEN5_DETILE_GLSLC AND KYTY_GEN5_DETILE_SPIRV_VAL)
	add_test(
		NAME KytyGen5DetileShader.CheckedInSpirv
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../scripts/check_gen5_detile_shader.py" --check
			--compiler "${KYTY_GEN5_DETILE_GLSLC}" --validator "${KYTY_GEN5_DETILE_SPIRV_VAL}"
	)
	set_tests_properties(KytyGen5DetileShader.CheckedInSpirv PROPERTIES TIMEOUT 30)
else()
	message(STATUS "Gen5 detile shader CTest disabled: glslc and/or spirv-val not found")
endif()

add_test(
	NAME KytyArchitectureBoundaryIntegration.Repository
	COMMAND $<TARGET_FILE:kyty_architecture_boundary_integration>
		"${Python3_EXECUTABLE}" "${KYTY_ARCHITECTURE_BOUNDARY_CHECKER}" "${CMAKE_SOURCE_DIR}" --strict
)
set_tests_properties(KytyArchitectureBoundaryIntegration.Repository PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
	TIMEOUT 15
)

set(KYTY_ARCHIVE_DAG_CHECKER "${CMAKE_SOURCE_DIR}/../scripts/check_archive_dag.py")

add_test(
	NAME KytyArchiveDagIntegration.Repository
	COMMAND "${Python3_EXECUTABLE}" "${KYTY_ARCHIVE_DAG_CHECKER}" "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(KytyArchiveDagIntegration.Repository PROPERTIES
	FIXTURES_REQUIRED KytyIntegrationBinaries
	TIMEOUT 15
)

set(KYTY_INTEGRATION_TEST_TARGETS
	kyty_bringup_integration
	kyty_domain_validation_integration
	kyty_kernel_memory_integration
	kyty_graphics_diagnostics_integration
	kyty_libc_wide_integration
	kyty_libc_time_integration
	kyty_libc_locale_integration
	kyty_guest_stack_frame_integration
	kyty_libc_allocator_integration
	kyty_loader_integration
	kyty_agent_protocol_integration
	kyty_agent_transport_integration
	kyty_audio_pcm_integration
	kyty_audio_host_integration
	kyty_audio_video_memory_integration
	kyty_host_platform_integration
	kyty_architecture_boundary_integration
)

add_test(
	NAME KytyIntegration._build
	COMMAND "${CMAKE_COMMAND}"
		--build "${CMAKE_BINARY_DIR}"
		--config $<CONFIG>
		--parallel
		--target ${KYTY_INTEGRATION_TEST_TARGETS}
)
set_tests_properties(KytyIntegration._build PROPERTIES
	FIXTURES_SETUP KytyIntegrationBinaries
	TIMEOUT 600
)
