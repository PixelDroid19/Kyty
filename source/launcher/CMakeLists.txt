
find_package(Qt6 COMPONENTS Widgets LinguistTools REQUIRED)

set(CMAKE_AUTOUIC_SEARCH_PATHS forms)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

file(GLOB launcher_src
	"include/*.h"
	"src/*.cpp"
)

file(GLOB launcher_forms
	"forms/*.ui"
)

file(GLOB launcher_res
	"res/*.qrc"
)

set(launcher_ts "ts/launcher_ru_RU.ts")

add_executable(launcher WIN32 MACOSX_BUNDLE
	${launcher_src}
	${launcher_forms}
	${launcher_res}
	${launcher_ts}
)


target_link_libraries(launcher Qt6::Widgets)
if(MSVC OR MINGW)
target_link_libraries(launcher psapi)
target_link_libraries(launcher setupapi)
endif()
if (CLANG AND NOT MSVC)
target_link_libraries(launcher pthread)
endif()
#if (CLANG AND MSVC)
#target_link_libraries(launcher winpthread)
#endif()
target_include_directories(launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(launcher PROPERTIES AUTOUIC ON AUTOMOC ON AUTORCC ON)

if(MINGW)
	set_target_properties(launcher PROPERTIES LINK_FLAGS "${KYTY_LD_OPTIONS}")
endif()

add_dependencies(launcher KytyBuildInfo)

if(WIN32)
    find_program(QT_WINDEPLOYQT NAMES windeployqt PATHS "${Qt6_DIR}/../../../bin" "${QT6_INSTALL_PREFIX}/bin" NO_DEFAULT_PATH)
    if(NOT QT_WINDEPLOYQT)
        find_program(QT_WINDEPLOYQT NAMES windeployqt)
    endif()
    if(QT_WINDEPLOYQT)
        set(launcher_name "launcher")
        set_target_properties(launcher PROPERTIES OUTPUT_NAME ${launcher_name})
        install(TARGETS launcher DESTINATION .)
        install(CODE "
    		execute_process(
    			COMMAND ${QT_WINDEPLOYQT} \"\${CMAKE_INSTALL_PREFIX}/${launcher_name}.exe\"
                --no-svg
                --no-angle
                --no-opengl
                --no-opengl-sw
                --no-system-d3d-compiler
    				--no-translations
    		)
    ")
    else()
        message(WARNING "windeployqt not found; skipping Windows deployment step")
        set(launcher_name "launcher")
        set_target_properties(launcher PROPERTIES OUTPUT_NAME ${launcher_name})
        install(TARGETS launcher DESTINATION .)
    endif()
elseif(APPLE)
    find_program(QT_MACDEPLOYQT NAMES macdeployqt PATHS "${Qt6_DIR}/../../../bin" "${QT6_INSTALL_PREFIX}/bin" NO_DEFAULT_PATH)
    if(NOT QT_MACDEPLOYQT)
        find_program(QT_MACDEPLOYQT NAMES macdeployqt)
    endif()
    if(QT_MACDEPLOYQT)
        set(launcher_name "launcher")
        set_target_properties(launcher PROPERTIES OUTPUT_NAME ${launcher_name} MACOSX_BUNDLE TRUE)
        install(TARGETS launcher BUNDLE DESTINATION .)
        install(CODE "
            execute_process(
                COMMAND ${QT_MACDEPLOYQT} \"\${CMAKE_INSTALL_PREFIX}/launcher.app\" -verbose=1
            )
        ")
    else()
        message(WARNING "macdeployqt not found; skipping macOS deployment step")
        set(launcher_name "launcher")
        set_target_properties(launcher PROPERTIES OUTPUT_NAME ${launcher_name} MACOSX_BUNDLE TRUE)
        install(TARGETS launcher BUNDLE DESTINATION .)
    endif()
else()
    set(launcher_name "launcher")
    set_target_properties(launcher PROPERTIES OUTPUT_NAME ${launcher_name})
    install(TARGETS launcher DESTINATION .)
endif()

qt6_create_translation(QM_FILES ${launcher_src} ${launcher_forms} ${launcher_ts} OPTIONS -I ${CMAKE_SOURCE_DIR}/launcher/include)

set(tidy_dirs "${CMAKE_SOURCE_DIR}/launcher/include") 
set(iwyu_maps "${CMAKE_SOURCE_DIR}/launcher/utils/qt5_15.imp")

get_property(inc_headers TARGET launcher PROPERTY INCLUDE_DIRECTORIES)

list(APPEND inc_headers
	${CMAKE_SOURCE_DIR}/launcher
	${CMAKE_BINARY_DIR}/launcher/launcher_autogen/include
	${Qt6Widgets_INCLUDE_DIRS}
	${Qt6Core_INCLUDE_DIRS}
	${Qt6Gui_INCLUDE_DIRS}
)

include_what_you_use_with_mappings(launcher "${inc_headers}" "${iwyu_maps}")

clang_tidy_check(launcher "" "${tidy_dirs}" "${inc_headers}")
