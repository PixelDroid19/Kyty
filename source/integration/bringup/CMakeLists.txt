# Process-isolated bring-up policy integration tests.

add_executable(kyty_bringup_integration kyty_bringup_integration.cpp)

target_include_directories(kyty_bringup_integration PRIVATE
	${CMAKE_SOURCE_DIR}/include
	${CMAKE_SOURCE_DIR}/emulator/include
	${PROJECT_BINARY_DIR}
)

# Link the same core/emulator stack used by fc_script (minimal set).
target_link_libraries(kyty_bringup_integration
	core
	sys
	math
	emulator
	SDL2-static
	lua
	scripts
	rijndael
	lzma
	sqlite
	zstd
	easy_profiler
	cpuinfo
	vulkan-1
	spirv-tools-opt
	spirv-tools
)

if (CLANG AND NOT MSVC)
	target_link_libraries(kyty_bringup_integration pthread)
endif()
if (LINUX)
	target_link_libraries(kyty_bringup_integration dl)
endif()

# Portable per-scenario runner: sets env, runs binary, checks exact exit code.
# Usage: cmake -DCMD=... -DEXPECT=... -DENV_*=... -P RunBringUpScenario.cmake
set(BRINGUP_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/RunBringUpScenario.cmake")

function(kyty_bringup_test name scenario expect)
	# Optional extra ENV pairs as ARGN: KEY VALUE KEY VALUE ...
	set(env_args "")
	set(i 0)
	list(LENGTH ARGN n)
	while(i LESS n)
		list(GET ARGN ${i} k)
		math(EXPR j "${i}+1")
		list(GET ARGN ${j} v)
		list(APPEND env_args "-DENV_${k}=${v}")
		math(EXPR i "${i}+2")
	endwhile()

	add_test(
		NAME KytyBringUpIntegration.${name}
		COMMAND ${CMAKE_COMMAND}
			-DCMD=$<TARGET_FILE:kyty_bringup_integration>
			-DSCENARIO=${scenario}
			-DEXPECT=${expect}
			${env_args}
			-P ${BRINGUP_RUNNER}
	)
	set_tests_properties(KytyBringUpIntegration.${name} PROPERTIES
		TIMEOUT 120
	)
endfunction()

# --- Matrix ---
# Unix wait status is 8-bit: dbg_exit(321) is observed as 321 & 0xff == 65.
set(KYTY_ABORT_RC 65)

# Strict default aborts on EXIT_NOT_IMPLEMENTED.
kyty_bringup_test(strict_abort strict_abort ${KYTY_ABORT_RC})

# Unsafe continues and exits 0.
kyty_bringup_test(unsafe_continue unsafe_continue 0
	KYTY_BRINGUP_MODE unsafe)

# Unauthorized subsystem (graphics site with only kernel allowed) aborts.
kyty_bringup_test(unauthorized_subsystem unauthorized_subsystem ${KYTY_ABORT_RC}
	KYTY_BRINGUP_MODE unsafe
	KYTY_BRINGUP_FEATURES not_implemented
	KYTY_BRINGUP_SUBSYSTEMS kernel)

# Invalid configuration fails closed before scenario body (_Exit 2).
kyty_bringup_test(invalid_config invalid_config 2
	KYTY_BRINGUP_MODE not_a_mode)

# Burst limit trips circuit-break → halt.
kyty_bringup_test(burst_limit burst_limit ${KYTY_ABORT_RC}
	KYTY_BRINGUP_MODE unsafe
	KYTY_BRINGUP_FEATURES not_implemented
	KYTY_BRINGUP_BURST_LIMIT 5
	KYTY_BRINGUP_BURST_WINDOW_MS 60000)

# Slow repeats across window must not false-trip.
kyty_bringup_test(slow_repeat slow_repeat 0
	KYTY_BRINGUP_MODE unsafe
	KYTY_BRINGUP_FEATURES not_implemented
	KYTY_BRINGUP_BURST_LIMIT 3
	KYTY_BRINGUP_BURST_WINDOW_MS 50)

# Missing import dedupe + call logging.
kyty_bringup_test(missing_import missing_import 0
	KYTY_BRINGUP_MODE unsafe
	KYTY_BRINGUP_FEATURES missing_function_import)

# Real export wins over stub (structural).
kyty_bringup_test(export_wins export_wins 0
	KYTY_BRINGUP_MODE unsafe
	KYTY_BRINGUP_FEATURES missing_function_import)

# Non-Func types are never stubbed by policy surface.
kyty_bringup_test(no_stub_nonfunc no_stub_nonfunc 0
	KYTY_BRINGUP_MODE unsafe
	KYTY_BRINGUP_FEATURES missing_function_import)

# Concurrent same site/symbol → single registry entry.
kyty_bringup_test(concurrent concurrent 0
	KYTY_BRINGUP_MODE unsafe
	KYTY_BRINGUP_FEATURES not_implemented,missing_function_import
	KYTY_BRINGUP_BURST_LIMIT 100000
	KYTY_BRINGUP_BURST_WINDOW_MS 60000)

# Capacity exhaust aborts (no anonymous fallback). Filling 2048 slots is
# intentional stress; allow a longer timeout.
kyty_bringup_test(capacity capacity ${KYTY_ABORT_RC}
	KYTY_BRINGUP_MODE unsafe
	KYTY_BRINGUP_FEATURES missing_function_import)
set_tests_properties(KytyBringUpIntegration.capacity PROPERTIES TIMEOUT 300)

# Diagnostics protocol fields.
kyty_bringup_test(diagnostics diagnostics 0
	KYTY_BRINGUP_MODE unsafe)

# Neighbor PRX discovery under sanitized fixture (unsafe + prx_preload).
kyty_bringup_test(prx_discover prx_discover 0
	KYTY_BRINGUP_MODE unsafe
	KYTY_BRINGUP_FEATURES prx_preload)

# Strict must not auto-preload.
kyty_bringup_test(prx_strict_no_preload prx_strict_no_preload 0)
