# Regenerates or verifies the checked-in host shader includes from their sources:
#   cmake -DKYTY_SHADER_MODE=check|write -DKYTY_SHADER_WORK_DIR=<scratch> -P source/cmake/HostShaderBlobs.cmake
# check compiles every shader into the work directory and fails when a checked-in
# include differs from its source; write refreshes the includes in place. The
# normal build consumes the checked-in includes and needs none of these tools.

cmake_minimum_required(VERSION 3.20)

if(NOT KYTY_SHADER_MODE MATCHES "^(check|write)$")
	message(FATAL_ERROR "KYTY_SHADER_MODE must be check or write")
endif()
if(NOT KYTY_SHADER_WORK_DIR)
	message(FATAL_ERROR "KYTY_SHADER_WORK_DIR must name an owned scratch directory")
endif()
file(MAKE_DIRECTORY "${KYTY_SHADER_WORK_DIR}")

get_filename_component(source_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(host_shaders "${source_root}/emulator/src/Graphics/host_shaders")
set(fsr_shaders "${source_root}/emulator/src/Graphics/Shaders")
set(fsr_sdk "${source_root}/3rdparty/fidelityfx_fsr1")

find_program(glslc NAMES glslc)
find_program(spirv_val NAMES spirv-val)
find_program(spirv_dis NAMES spirv-dis)
find_program(glslang NAMES glslangValidator)
foreach(tool glslc spirv_val spirv_dis glslang)
	if(NOT ${tool})
		message(FATAL_ERROR "required tool not found: ${tool}")
	endif()
endforeach()

function(kyty_run)
	execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_QUIET)
	if(NOT result EQUAL 0)
		message(FATAL_ERROR "command failed (${result}): ${ARGN}")
	endif()
endfunction()

function(kyty_emit output text)
	if(KYTY_SHADER_MODE STREQUAL "write")
		file(WRITE "${output}" "${text}")
		return()
	endif()
	file(READ "${output}" checked_in)
	if(NOT checked_in STREQUAL text)
		message(FATAL_ERROR "${output}: checked-in include differs from its shader source")
	endif()
endfunction()

# Formats a SPIR-V binary as little-endian uint32 initializer lines, six words per line.
function(kyty_spirv_words spirv out_lines out_count)
	file(READ "${spirv}" hex HEX)
	string(LENGTH "${hex}" length)
	math(EXPR remainder "${length} % 8")
	if(length EQUAL 0 OR NOT remainder EQUAL 0)
		message(FATAL_ERROR "SPIR-V must be a non-empty multiple of four bytes: ${spirv}")
	endif()
	if(NOT hex MATCHES "^03022307")
		message(FATAL_ERROR "invalid SPIR-V magic: ${spirv}")
	endif()
	set(byte "[0-9a-f][0-9a-f]")
	set(word "0x[0-9a-f]+u, ")
	string(REGEX REPLACE "(${byte})(${byte})(${byte})(${byte})" "0x\\4\\3\\2\\1u, " words "${hex}")
	string(REGEX REPLACE "(${word}${word}${word}${word}${word}${word})" "\t\\1\n" lines "${words}")
	string(REGEX REPLACE "([^\n]+)$" "\t\\1\n" lines "${lines}")
	string(REPLACE ", \n" ",\n" lines "${lines}")
	math(EXPR count "${length} / 8")
	set(${out_lines} "${lines}" PARENT_SCOPE)
	set(${out_count} ${count} PARENT_SCOPE)
endfunction()

function(kyty_compute_words name array)
	set(source "${host_shaders}/${name}.comp")
	set(binary "${KYTY_SHADER_WORK_DIR}/${name}.spv")
	kyty_run("${glslc}" --target-env=vulkan1.4 -fshader-stage=compute -O -o "${binary}" "${source}")
	kyty_run("${spirv_val}" --target-env vulkan1.4 "${binary}")
	kyty_spirv_words("${binary}" lines count)
	kyty_emit("${host_shaders}/${name}_comp.inc"
		"// Auto-generated from ${name}.comp with source/cmake/HostShaderBlobs.cmake.\n// Do not edit by hand.\nconstexpr uint32_t ${array}[${count}] = {\n${lines}};\n")
endfunction()

function(kyty_splice_first text needle replacement out_text)
	string(FIND "${text}" "${needle}" position)
	if(position EQUAL -1)
		set(${out_text} "${text}" PARENT_SCOPE)
		return()
	endif()
	string(LENGTH "${needle}" needle_length)
	math(EXPR after_position "${position} + ${needle_length}")
	string(SUBSTRING "${text}" 0 ${position} before)
	string(SUBSTRING "${text}" ${after_position} -1 after)
	set(${out_text} "${before}${replacement}${after}" PARENT_SCOPE)
endfunction()

# Inserts marker as its own line before the first line that matches line_regex.
function(kyty_insert_line_before text line_regex marker message out_text)
	string(REGEX MATCH "\n${line_regex}" line "${text}")
	if(NOT line)
		message(FATAL_ERROR "${message}")
	endif()
	string(SUBSTRING "${line}" 1 -1 line)
	kyty_splice_first("${text}" "\n${line}" "\n${marker}\n${line}" text)
	set(${out_text} "${text}" PARENT_SCOPE)
endfunction()

# Disassembles a fragment kernel into the core the translator splices around a
# guest-specific function: that function, the stub interface, the annotations
# and the declarations become placeholders. An optional spec constant is
# frozen into a placeholder constant.
function(kyty_fragment_core name symbol role function)
	string(TOLOWER "${symbol}" prefix)
	string(TOLOWER "${role}" role_marker)
	set(stub "%${prefix}_stub")
	set(binary "${KYTY_SHADER_WORK_DIR}/${name}.spv")
	kyty_run("${glslc}" --target-env=vulkan1.4 -O0 -o "${binary}" "${host_shaders}/${name}.frag")
	kyty_run("${spirv_val}" --target-env vulkan1.4 "${binary}")
	execute_process(COMMAND "${spirv_dis}" "${binary}" OUTPUT_VARIABLE text RESULT_VARIABLE result)
	if(NOT result EQUAL 0)
		message(FATAL_ERROR "spirv-dis failed (${result}): ${binary}")
	endif()

	string(FIND "${text}" "\n%${function} = OpFunction" begin)
	if(begin EQUAL -1)
		message(FATAL_ERROR "${prefix} core ${role_marker} function missing")
	endif()
	math(EXPR begin "${begin} + 1")
	string(SUBSTRING "${text}" 0 ${begin} before)
	string(SUBSTRING "${text}" ${begin} -1 tail)
	string(FIND "${tail}" "OpFunctionEnd\n" end)
	if(end EQUAL -1)
		message(FATAL_ERROR "${prefix} core ${role_marker} function missing")
	endif()
	math(EXPR end "${end} + 14")
	string(SUBSTRING "${tail}" 0 ${end} body)
	string(SUBSTRING "${tail}" ${end} -1 after)
	string(REGEX MATCH "^[^\n]*\n[^\n]*" signature "${body}")
	set(text "${before}<${prefix}_${role_marker}>\n${after}")

	string(REGEX REPLACE "\n[^\n]*(OpName ${stub}|OpDecorate ${stub}|${stub} = OpVariable)[^\n]*" "" text "${text}")
	kyty_splice_first("${text}" " ${stub} " " <${prefix}_interface> " text)
	if(ARGC GREATER 4)
		set(spec "${ARGV4}")
		string(REGEX REPLACE "\n[^\n]*OpDecorate %${spec} SpecId[^\n]*" "" text "${text}")
		string(REPLACE "%${spec} = OpSpecConstant %uint 15" "%${spec} = OpConstant %uint <${prefix}_mask>" text "${text}")
	endif()
	kyty_insert_line_before("${text}" " *%void = OpTypeVoid" "<${prefix}_annotations>" "${prefix} core types missing" text)
	kyty_insert_line_before("${text}" " *%main = OpFunction" "<${prefix}_declarations>" "${prefix} core entry missing" text)

	kyty_emit("${host_shaders}/${name}_asm.inc"
		"// Generated from the public ${prefix} kernel. Do not edit by hand.\nconstexpr const char* kFragment${symbol}${role}Signature = R\"CORE(${signature}\n)CORE\";\nconstexpr const char* kFragment${symbol}Core = R\"CORE(${text})CORE\";\n")
endfunction()

function(kyty_fsr_words program out_text)
	set(binary "${KYTY_SHADER_WORK_DIR}/${program}.spv")
	kyty_run("${glslang}" -V --target-env vulkan1.1 "-I${fsr_sdk}" "-I${fsr_shaders}" "${fsr_shaders}/${program}.comp" -o "${binary}")
	kyty_spirv_words("${binary}" lines count)
	set(${out_text} "\nconstexpr uint32_t k${program}[] = {\n${lines}};\n" PARENT_SCOPE)
endfunction()

kyty_compute_words(gen5_detile kGen5DetileCompSpirv)
kyty_compute_words(fragment_wave_scan kFragmentWaveScanSpirv)
kyty_compute_words(fragment_wave_pack kFragmentWavePackSpirv)
kyty_fragment_core(fragment_wave_capture Capture Reader read_lane_word_u1_)
kyty_fragment_core(fragment_wave_resolve Resolve Writer write_colors_u1_ required_components)

kyty_fsr_words(PresentationFsrEasu easu)
kyty_fsr_words(PresentationFsrRcas rcas)
set(fsr_header [=[
// Generated by source/cmake/HostShaderBlobs.cmake; do not edit.
// SPIR-V of source/emulator/src/Graphics/Shaders, compiled against AMD FidelityFX FSR 1.0
// (source/3rdparty/fidelityfx_fsr1).
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
]=])
kyty_emit("${source_root}/emulator/src/Graphics/PresentationFsrShaders.inc" "${fsr_header}${easu}${rcas}")
