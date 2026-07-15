add_library(kyty_devtools_core STATIC)

target_include_directories(kyty_devtools_core PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_sources(kyty_devtools_core PRIVATE
	src/Telemetry/Event.cpp
	src/Telemetry/EventRing.cpp
	src/Telemetry/WriterRegistry.cpp
	src/Telemetry/ThreadInstanceAllocator.cpp
	src/Telemetry/WorkerTelemetry.cpp
	src/Telemetry/Progress.cpp
	src/Diagnostics/Checksum.cpp
	src/Diagnostics/ProcessStatus.cpp
	src/Diagnostics/StallClassifier.cpp
	src/Protocol/Protocol.cpp
	src/Transport/Bootstrap.cpp
	src/Transport/WorkerSession.cpp
	src/Catalog/ExportCatalog.cpp
	src/Time/MonotonicClockPosix.cpp
)

if (WIN32)
	target_sources(kyty_devtools_core PRIVATE
		src/Time/MonotonicClockWindows.cpp
	)
	# Windows build uses QPC path in MonotonicClockWindows.cpp; exclude POSIX clock_gettime.
	set_source_files_properties(src/Time/MonotonicClockPosix.cpp PROPERTIES
		COMPILE_DEFINITIONS "KYTY_DEVTOOLS_CLOCK_CONVERSION_ONLY=1"
	)
endif()

if (NOT MSVC)
	target_compile_options(kyty_devtools_core PRIVATE -fno-exceptions)
endif()
