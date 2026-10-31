#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

bool IsKnownUint(const String8& value)
{
	return !value.IsEmpty() && value != "unknown_uint_constant";
}

// Export target slot inside a vertex record: pos0, pos1, then parameters.
bool GeometryRecordSlot(uint32_t target, const ShaderGeometryComputeInfo& geometry, uint32_t* slot)
{
	if (target == 12u || target == 13u)
	{
		*slot = (target - 12u) * 4u;
		return true;
	}
	if (target >= 32u && target - 32u < geometry.parameter_count)
	{
		*slot = 8u + (target - 32u) * 4u;
		return true;
	}
	return false;
}

} // namespace

// Stores `value` (uint) at dword `word` of the current group of the output buffer.
static String8 GeometryStore(const String8& stem, const String8& word, const String8& value)
{
	return String8(R"(
%<s>_index = OpIAdd %uint %geo_group_words %<word>
%<s>_index64 = OpUConvert %ulong %<s>_index
%<s>_bytes = OpShiftLeftLogical %ulong %<s>_index64 %uint_2
%<s>_address = OpIAdd %ulong %geo_output %<s>_bytes
%<s>_pointer = OpConvertUToPtr %_ptr_PhysicalStorageBuffer_uint %<s>_address
               OpStore %<s>_pointer %<value> Aligned 4
)")
	    .ReplaceStr("<s>", stem)
	    .ReplaceStr("<word>", word)
	    .ReplaceStr("<value>", value);
}

bool Spirv::EmitGeometryProlog(String8* output) const
{
	const auto* bind = GetBindInfo();
	if (output == nullptr || !UsesGeometryCompute() || !UsesSplitComputeWave() || !UsesGuestDeviceAddress() || bind == nullptr ||
	    !bind->geometry_used || m_vs_input_info == nullptr || !m_vs_input_info->gs_prolog)
	{
		return false;
	}
	const auto& geometry = *m_geometry_compute_info;
	const auto  ptr      = String8(bind->vsharp_uniform_buffer ? "_ptr_Uniform_uint" : "_ptr_PushConstant_uint");
	const auto  row0     = GetConstantInt(static_cast<int>(bind->geometry_offset_dw / 4u));
	const auto  row1     = GetConstantInt(static_cast<int>(bind->geometry_offset_dw / 4u + 1u));
	const auto  per      = GetConstantUint(geometry.primitives_per_group);
	const auto  group    = GetConstantUint(geometry.GroupDwords());
	const auto  waves    = GetConstantUint(geometry.waves << 28u);
	const auto  corners  = GetConstantUint(geometry.Corners());
	const auto  lanes    = GetConstantUint(ShaderGeometryComputeInfo::kLanes);
	const auto  scale    = GetConstantUint(ShaderGeometryComputeInfo::kVertexOffsetScale);
	const auto  stride   = GetConstantUint(ShaderGeometryComputeInfo::kVertexOffsetScale * geometry.Corners());
	if (row0 == "unknown_int_constant" || row1 == "unknown_int_constant" || !IsKnownUint(per) || !IsKnownUint(group) ||
	    !IsKnownUint(waves) || !IsKnownUint(corners) || !IsKnownUint(lanes) || !IsKnownUint(scale) || !IsKnownUint(stride))
	{
		return false;
	}

	// One guest subgroup = this workgroup; each wave sees its share of the
	// subgroup's ES and GS threads in s3 = {ES vertices [7:0], GS primitives
	// [15:8], wave id [27:24], wave count [31:28]}.
	*output += String8(R"(
%geo_group_ptr = OpAccessChain %_ptr_Input_uint %gl_WorkGroupID %uint_0
%geo_group = OpLoad %uint %geo_group_ptr
%geo_output_lo_ptr = OpAccessChain %<ptr> %vsharp %int_0 %<row0> %int_0
%geo_output_hi_ptr = OpAccessChain %<ptr> %vsharp %int_0 %<row0> %int_1
%geo_total_ptr = OpAccessChain %<ptr> %vsharp %int_0 %<row0> %int_2
%geo_slices_ptr = OpAccessChain %<ptr> %vsharp %int_0 %<row0> %int_3
%geo_first_vertex_ptr = OpAccessChain %<ptr> %vsharp %int_0 %<row1> %int_0
%geo_first_instance_ptr = OpAccessChain %<ptr> %vsharp %int_0 %<row1> %int_1
%geo_output_lo = OpLoad %uint %geo_output_lo_ptr
%geo_output_hi = OpLoad %uint %geo_output_hi_ptr
%geo_total = OpLoad %uint %geo_total_ptr
%geo_slices = OpLoad %uint %geo_slices_ptr
%geo_first_vertex = OpLoad %uint %geo_first_vertex_ptr
%geo_first_instance = OpLoad %uint %geo_first_instance_ptr
%geo_index_lo_ptr = OpAccessChain %<ptr> %vsharp %int_0 %<row1> %int_2
%geo_index_hi_ptr = OpAccessChain %<ptr> %vsharp %int_0 %<row1> %int_3
%geo_index_lo = OpLoad %uint %geo_index_lo_ptr
%geo_index_hi = OpLoad %uint %geo_index_hi_ptr
%geo_index_lo64 = OpUConvert %ulong %geo_index_lo
%geo_index_hi64 = OpUConvert %ulong %geo_index_hi
%geo_index_his = OpShiftLeftLogical %ulong %geo_index_hi64 %uint_32
%geo_index_base = OpBitwiseOr %ulong %geo_index_his %geo_index_lo64
%geo_output_lo64 = OpUConvert %ulong %geo_output_lo
%geo_output_hi64 = OpUConvert %ulong %geo_output_hi
%geo_output_his = OpShiftLeftLogical %ulong %geo_output_hi64 %uint_32
%geo_output = OpBitwiseOr %ulong %geo_output_his %geo_output_lo64
%geo_group_words = OpIMul %uint %geo_group %<group>
%geo_instance = OpUDiv %uint %geo_group %geo_slices
%geo_slice = OpUMod %uint %geo_group %geo_slices
%geo_base = OpIMul %uint %geo_slice %<per>
%geo_left = OpISub %uint %geo_total %geo_base
%geo_prims = OpExtInst %uint %GLSL_std_450 UMin %geo_left %<per>
%geo_es = OpIMul %uint %geo_prims %<corners>
%geo_wave_first = OpIMul %uint %split_wave %<lanes>
%geo_es_rest = OpExtInst %uint %GLSL_std_450 UMax %geo_es %geo_wave_first
%geo_es_after = OpISub %uint %geo_es_rest %geo_wave_first
%geo_wave_es = OpExtInst %uint %GLSL_std_450 UMin %geo_es_after %<lanes>
%geo_prims_rest = OpExtInst %uint %GLSL_std_450 UMax %geo_prims %geo_wave_first
%geo_prims_after = OpISub %uint %geo_prims_rest %geo_wave_first
%geo_wave_prims = OpExtInst %uint %GLSL_std_450 UMin %geo_prims_after %<lanes>
%geo_prims_field = OpShiftLeftLogical %uint %geo_wave_prims %uint_8
%geo_wave_field = OpShiftLeftLogical %uint %split_wave %uint_24
%geo_counts = OpBitwiseOr %uint %geo_wave_es %geo_prims_field
%geo_ids = OpBitwiseOr %uint %geo_counts %geo_wave_field
%geo_info = OpBitwiseOr %uint %geo_ids %<waves>
               OpStore %s3 %geo_info
%geo_instance_id = OpIAdd %uint %geo_first_instance %geo_instance
%geo_instance_float = OpBitcast %float %geo_instance_id
%geo_zero_float = OpBitcast %float %uint_0
)")
	               .ReplaceStr("<ptr>", ptr)
	               .ReplaceStr("<row0>", row0)
	               .ReplaceStr("<row1>", row1)
	               .ReplaceStr("<group>", group)
	               .ReplaceStr("<per>", per)
	               .ReplaceStr("<corners>", corners)
	               .ReplaceStr("<lanes>", lanes)
	               .ReplaceStr("<waves>", waves);

	// ES thread T computes corner T % corners of input primitive base + T / corners.
	// A strip's odd triangle keeps its first vertex and swaps the other two, the
	// order the guest's own strip output uses with the provoking vertex first.
	String8 vertex = geometry.topology != ShaderGeometryInputTopology::TriangleStrip
	                     ? String8("%geo_first = OpIMul %uint %geo_input %<corners>\n"
	                               "%geo_vertex = OpIAdd %uint %geo_first %geo_corner\n")
	                     : String8("%geo_parity = OpBitwiseAnd %uint %geo_input %uint_1\n"
	                               "%geo_odd = OpINotEqual %bool %geo_parity %uint_0\n"
	                               "%geo_later = OpINotEqual %bool %geo_corner %uint_0\n"
	                               "%geo_swap = OpLogicalAnd %bool %geo_odd %geo_later\n"
	                               "%geo_swapped = OpISub %uint %uint_3 %geo_corner\n"
	                               "%geo_strip_corner = OpSelect %uint %geo_swap %geo_swapped %geo_corner\n"
	                               "%geo_vertex = OpIAdd %uint %geo_input %geo_strip_corner\n");
	// GS thread T reads primitive T; a point has one corner, so its other
	// vertex offsets stay zero.
	String8 offsets = geometry.topology == ShaderGeometryInputTopology::PointList
	                      ? String8("%geo_offsets01 = OpCopyObject %uint %geo_offset0\n"
	                                "%geo_offset2 = OpCopyObject %uint %uint_0\n")
	                      : String8("%geo_offset1 = OpIAdd %uint %geo_offset0 %<scale>\n"
	                                "%geo_offset2 = OpIAdd %uint %geo_offset1 %<scale>\n"
	                                "%geo_offset1_field = OpShiftLeftLogical %uint %geo_offset1 %uint_16\n"
	                                "%geo_offsets01 = OpBitwiseOr %uint %geo_offset0 %geo_offset1_field\n");
	// An indexed draw reads each slot vertex from the guest index buffer at the
	// guest address in row 1, through the device-address table like every other
	// guest load of the pass; the block's first vertex is the draw's base vertex
	// added to every fetched index. A 16-bit index is read from its aligned word,
	// which never leaves the page holding the index.
	String8 vertex_id;
	if (geometry.indexed)
	{
		const String8 element = geometry.index_format == 0u ? String8(R"(%geo_ibytes = OpShiftLeftLogical %ulong %geo_islot64 %uint_2
%geo_iguest = OpIAdd %ulong %geo_index_base %geo_ibytes
)")
		                                                    : String8(R"(%geo_ibytes = OpShiftLeftLogical %ulong %geo_islot64 %uint_1
%geo_ielement = OpIAdd %ulong %geo_index_base %geo_ibytes
%geo_iwords = OpShiftRightLogical %ulong %geo_ielement %uint_2
%geo_iguest = OpShiftLeftLogical %ulong %geo_iwords %uint_2
%geo_ihalf64 = OpShiftRightLogical %ulong %geo_ielement %uint_1
%geo_ihalf32 = OpUConvert %uint %geo_ihalf64
%geo_ihalf = OpBitwiseAnd %uint %geo_ihalf32 %uint_1
%geo_ishift = OpIMul %uint %geo_ihalf %uint_16
)");
		const String8 extract = geometry.index_format == 0u
		                            ? String8("%geo_fetched = OpCopyObject %uint %geo_iword\n")
		                            : String8("%geo_fetched = OpBitFieldUExtract %uint %geo_iword %geo_ishift %uint_16\n");
		vertex_id = "%geo_islot64 = OpUConvert %ulong %geo_vertex\n" + element +
		            "%geo_idevice = OpFunctionCall %ulong %guest_device_address %geo_iguest %gda_u64_4\n"
		            "%geo_ipointer = OpConvertUToPtr %_ptr_PhysicalStorageBuffer_uint %geo_idevice\n"
		            "%geo_iword = OpLoad %uint %geo_ipointer Aligned 4\n" +
		            extract + "%geo_vertex_id = OpIAdd %uint %geo_fetched %geo_first_vertex\n";
	} else
	{
		vertex_id = "%geo_vertex_id = OpIAdd %uint %geo_first_vertex %geo_vertex\n";
	}
	*output += String8(R"(
%geo_local = OpUDiv %uint %native_wave_logical_index %<corners>
%geo_corner = OpUMod %uint %native_wave_logical_index %<corners>
%geo_input = OpIAdd %uint %geo_base %geo_local
<vertex><vertex_id>%geo_vertex_float = OpBitcast %float %geo_vertex_id
               OpStore %v5 %geo_vertex_float
               OpStore %v8 %geo_instance_float
%geo_offset0 = OpIMul %uint %native_wave_logical_index %<stride>
<offsets>%geo_offsets01_float = OpBitcast %float %geo_offsets01
%geo_offsets2_float = OpBitcast %float %geo_offset2
%geo_primitive = OpIAdd %uint %geo_base %native_wave_logical_index
%geo_primitive_float = OpBitcast %float %geo_primitive
               OpStore %v0 %geo_offsets01_float
               OpStore %v1 %geo_offsets2_float
               OpStore %v2 %geo_primitive_float
               OpStore %v3 %geo_zero_float
               OpStore %v4 %geo_zero_float
)")
	               .ReplaceStr("<vertex>", vertex)
	               .ReplaceStr("<vertex_id>", vertex_id)
	               .ReplaceStr("<offsets>", offsets)
	               .ReplaceStr("<corners>", corners)
	               .ReplaceStr("<stride>", stride)
	               .ReplaceStr("<scale>", scale);
	return true;
}

bool Spirv::EmitGeometryExport(const ShaderInstruction& instruction, uint32_t index, String8* output) const
{
	if (output == nullptr || !UsesGeometryCompute() || !UsesSplitComputeWave() ||
	    ShaderClassifyGeometryWaveInstruction(instruction) != ShaderComputeWaveInstructionKind::GeometryExport)
	{
		return false;
	}
	const auto&    geometry = *m_geometry_compute_info;
	const uint32_t target   = (instruction.raw_word >> 4u) & 0x3fu;
	const bool     prim     = target == 20u;
	uint32_t       slot     = 0;
	if (!prim && !GeometryRecordSlot(target, geometry, &slot))
	{
		return false;
	}
	const auto record = GetConstantUint(geometry.RecordDwords());
	const auto first  = GetConstantUint(prim ? ShaderGeometryComputeInfo::kPrimitiveWordsOffset : geometry.RecordsOffset());
	if (!IsKnownUint(record) || !IsKnownUint(first))
	{
		return false;
	}

	// Each invocation is one guest lane; an inactive lane exports nothing.
	const auto stem = String8::FromPrintf("geo_exp_%u", index);
	String8    source = String8("%<s>_exec_lane = OpLoad %uint %exec_lane_lo\n%<s>_exec = OpINotEqual %bool %<s>_exec_lane %uint_0\n")
	                     .ReplaceStr("<s>", stem);
	for (uint32_t component = 0; component < 4u; ++component)
	{
		if ((instruction.exp_enable_mask & (1u << component)) == 0u)
		{
			continue;
		}
		const auto value = operand_variable_to_str(ShaderExportComponentSource(instruction, component));
		if (value.type != SpirvType::Float)
		{
			return false;
		}
		source += String8::FromPrintf("%%%s_c%u_float = OpLoad %%float %%%s\n%%%s_c%u = OpBitcast %%uint %%%s_c%u_float\n", stem.c_str(),
		                              component, value.value.c_str(), stem.c_str(), component, stem.c_str(), component);
	}
	source += String8(R"(
               OpSelectionMerge %<s>_merge None
               OpBranchConditional %<s>_exec %<s>_then %<s>_merge
%<s>_then = OpLabel
)")
	              .ReplaceStr("<s>", stem);
	// A primitive export lands in the lane's primitive word; a vertex export
	// in the lane's record at the target slot.
	source += prim ? String8::FromPrintf("%%%s_word = OpIAdd %%uint %%native_wave_logical_index %%%s\n", stem.c_str(), first.c_str())
	               : String8::FromPrintf("%%%s_record = OpIMul %%uint %%native_wave_logical_index %%%s\n"
	                                     "%%%s_word = OpIAdd %%uint %%%s_record %%%s\n",
	                                     stem.c_str(), record.c_str(), stem.c_str(), stem.c_str(), first.c_str());
	for (uint32_t component = 0; component < 4u; ++component)
	{
		if ((instruction.exp_enable_mask & (1u << component)) == 0u)
		{
			continue;
		}
		const auto component_slot = GetConstantUint(slot + component);
		if (!IsKnownUint(component_slot))
		{
			return false;
		}
		const auto store = String8::FromPrintf("%s_s%u", stem.c_str(), component);
		source += String8::FromPrintf("%%%s_word = OpIAdd %%uint %%%s_word %%%s\n", store.c_str(), stem.c_str(), component_slot.c_str());
		source += GeometryStore(store, store + "_word", String8::FromPrintf("%s_c%u", stem.c_str(), component));
	}
	source += String8("               OpBranch %<s>_merge\n%<s>_merge = OpLabel\n").ReplaceStr("<s>", stem);
	*output += source;
	return true;
}

bool Spirv::EmitGeometryAlloc(uint32_t index, String8* output) const
{
	if (output == nullptr || !UsesGeometryCompute() || !UsesSplitComputeWave())
	{
		return false;
	}
	// The payload is wave-uniform; one invocation records it as the group header.
	const auto stem = String8::FromPrintf("geo_alloc_%u", index);
	*output += String8(R"(
%<s>_payload = OpLoad %uint %m0
%<s>_leader = OpIEqual %bool %split_guest_lane %uint_0
               OpSelectionMerge %<s>_merge None
               OpBranchConditional %<s>_leader %<s>_then %<s>_merge
%<s>_then = OpLabel
)")
	               .ReplaceStr("<s>", stem);
	*output += GeometryStore(stem, "uint_0", stem + "_payload");
	*output += String8("               OpBranch %<s>_merge\n%<s>_merge = OpLabel\n").ReplaceStr("<s>", stem);
	return true;
}

ShaderComputeInputInfo SpirvGeometryComputeHostInfo(const ShaderCode& code, const ShaderVertexInputInfo& vs_input_info,
                                                    const ShaderGeometryComputeInfo& geometry)
{
	ShaderComputeInputInfo host {};
	host.threads_num[0] = geometry.GroupLanes();
	host.threads_num[1] = host.threads_num[2] = 1u;
	host.lds_dwords  = geometry.lds_dwords;
	// One guest lane per invocation: each guest wave64 spans 64 / part_lanes host subgroups.
	host.wave_layout = {ShaderComputeWaveStrategy::Split64On32, {geometry.GroupLanes(), 1u, 1u}, {geometry.GroupLanes(), 1u, 1u}, 64u,
	                    geometry.part_lanes, 1u, geometry.waves, host.lds_dwords};
	host.native_wave_sensitive    = true;
	host.required_subgroup_size   = geometry.part_lanes;
	host.bind        = vs_input_info.bind;
	host.float_mode  = vs_input_info.float_mode;
	host.dx10_clamp  = vs_input_info.dx10_clamp;
	host.ieee_mode   = vs_input_info.ieee_mode;
	host.fp_mode_known = true;
	return host;
}

String8 SpirvGenerateGeometryComputeSource(const ShaderCode& code, const ShaderVertexInputInfo& vs_input_info,
                                           const ShaderGeometryComputeInfo& geometry)
{
	if (code.GetType() != ShaderType::Vertex || !vs_input_info.gs_prolog || vs_input_info.fetch_inline ||
	    !vs_input_info.bind.geometry_used || !vs_input_info.bind.device_address_used || geometry.primitives_per_group == 0u ||
	    geometry.waves == 0u || geometry.waves > ShaderGeometryComputeInfo::kMaxWaves ||
	    geometry.primitives_per_group * geometry.Corners() > geometry.GroupLanes() || geometry.lds_dwords == 0u ||
	    geometry.parameter_count < SpirvResolveVertexParameterCount(code, &vs_input_info))
	{
		return "OpKytyGeometryComputeRejected\n";
	}
	ShaderCode program = code;
	program.SetType(ShaderType::Compute);
	const auto host = SpirvGeometryComputeHostInfo(code, vs_input_info, geometry);
	Spirv spirv;
	spirv.SetCode(program);
	spirv.SetVsInputInfo(&vs_input_info);
	spirv.SetCsInputInfo(&host);
	spirv.SetGeometryComputeInfo(&geometry);
	spirv.GenerateSource();
	return spirv.GetSource();
}

String8 SpirvGenerateGeometryPullSource(const ShaderGeometryComputeInfo& geometry, bool layer)
{
	// A primitive export word holds three 10-bit corners: a 9-bit vertex index under that vertex's edge flag.
	static const char* head = R"(               OpCapability Shader
               OpCapability Int64
               OpCapability PhysicalStorageBufferAddresses
               OpCapability ClipDistance
               OpCapability CullDistance
<layer_capability>               OpExtension "SPV_KHR_physical_storage_buffer"
       %glsl = OpExtInstImport "GLSL.std.450"
               OpMemoryModel PhysicalStorageBuffer64 GLSL450
               OpEntryPoint Vertex %main "main" %gl_VertexIndex %outPerVertex %geometry<interface>
               OpDecorate %gl_VertexIndex BuiltIn VertexIndex
               OpMemberDecorate %gl_PerVertex 0 BuiltIn Position
               OpMemberDecorate %gl_PerVertex 1 BuiltIn PointSize
               OpMemberDecorate %gl_PerVertex 2 BuiltIn ClipDistance
               OpMemberDecorate %gl_PerVertex 3 BuiltIn CullDistance
               OpDecorate %gl_PerVertex Block
               OpMemberDecorate %GeometryBlock 0 Offset 0
               OpMemberDecorate %GeometryBlock 1 Offset 4
               OpDecorate %GeometryBlock Block
<decorations>       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
        %int = OpTypeInt 32 1
      %ulong = OpTypeInt 64 0
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_3 = OpConstant %uint 3
    %uint_10 = OpConstant %uint 10
    %uint_12 = OpConstant %uint 12
    %uint_20 = OpConstant %uint 20
    %uint_32 = OpConstant %uint 32
 %uint_lanes = OpConstant %uint <lanes>
%uint_group_corners = OpConstant %uint <group_corners>
 %uint_index_mask = OpConstant %uint 511
 %uint_layer_mask = OpConstant %uint 2047
 %uint_count_mask = OpConstant %uint 4095
 %uint_null = OpConstant %uint 2147483648
 %uint_group = OpConstant %uint <group>
%uint_record = OpConstant %uint <record>
%uint_records = OpConstant %uint <records_offset>
      %int_0 = OpConstant %int 0
      %int_1 = OpConstant %int 1
    %float_0 = OpConstant %float 0
  %v4float_0 = OpConstantComposite %v4float %float_0 %float_0 %float_0 %float_0
%_ptr_Input_int = OpTypePointer Input %int
%gl_VertexIndex = OpVariable %_ptr_Input_int Input
%single_float = OpTypeArray %float %uint_1
%gl_PerVertex = OpTypeStruct %v4float %float %single_float %single_float
%_ptr_Output_gl_PerVertex = OpTypePointer Output %gl_PerVertex
%outPerVertex = OpVariable %_ptr_Output_gl_PerVertex Output
%_ptr_Output_v4float = OpTypePointer Output %v4float
%_ptr_Output_int = OpTypePointer Output %int
%GeometryBlock = OpTypeStruct %uint %uint
%_ptr_PushConstant_GeometryBlock = OpTypePointer PushConstant %GeometryBlock
%_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
%geometry = OpVariable %_ptr_PushConstant_GeometryBlock PushConstant
%_ptr_PhysicalStorageBuffer_uint = OpTypePointer PhysicalStorageBuffer %uint
<variables>       %main = OpFunction %void None %fn
      %entry = OpLabel
%output_lo_ptr = OpAccessChain %_ptr_PushConstant_uint %geometry %int_0
%output_hi_ptr = OpAccessChain %_ptr_PushConstant_uint %geometry %int_1
  %output_lo = OpLoad %uint %output_lo_ptr
  %output_hi = OpLoad %uint %output_hi_ptr
%output_lo64 = OpUConvert %ulong %output_lo
%output_hi64 = OpUConvert %ulong %output_hi
%output_his = OpShiftLeftLogical %ulong %output_hi64 %uint_32
     %output = OpBitwiseOr %ulong %output_his %output_lo64
%vertex_signed = OpLoad %int %gl_VertexIndex
     %vertex = OpBitcast %uint %vertex_signed
      %group = OpUDiv %uint %vertex %uint_group_corners
      %local = OpUMod %uint %vertex %uint_group_corners
  %primitive = OpUDiv %uint %local %uint_3
     %corner = OpUMod %uint %local %uint_3
%group_words = OpIMul %uint %group %uint_group
<load_header>%vertices = OpBitwiseAnd %uint %header %uint_count_mask
 %primitives = OpShiftRightLogical %uint %header %uint_12
%primitive_word_offset = OpIAdd %uint %primitive %uint_1
<load_primitive>%index_shift = OpIMul %uint %corner %uint_10
%index_bits = OpShiftRightLogical %uint %primitive_word %index_shift
      %index = OpBitwiseAnd %uint %index_bits %uint_index_mask
    %index0 = OpBitwiseAnd %uint %primitive_word %uint_index_mask
%index1_bits = OpShiftRightLogical %uint %primitive_word %uint_10
    %index1 = OpBitwiseAnd %uint %index1_bits %uint_index_mask
%index2_bits = OpShiftRightLogical %uint %primitive_word %uint_20
    %index2 = OpBitwiseAnd %uint %index2_bits %uint_index_mask
%null_bits = OpBitwiseAnd %uint %primitive_word %uint_null
 %not_null = OpIEqual %bool %null_bits %uint_0
%allocated = OpULessThan %bool %primitive %primitives
%vertex_limit = OpExtInst %uint %glsl UMin %vertices %uint_lanes
  %index0_ok = OpULessThan %bool %index0 %vertex_limit
  %index1_ok = OpULessThan %bool %index1 %vertex_limit
  %index2_ok = OpULessThan %bool %index2 %vertex_limit
%indices01_ok = OpLogicalAnd %bool %index0_ok %index1_ok
 %indices_ok = OpLogicalAnd %bool %indices01_ok %index2_ok
%live_primitive = OpLogicalAnd %bool %allocated %not_null
       %live = OpLogicalAnd %bool %live_primitive %indices_ok
%safe_index = OpSelect %uint %live %index %uint_0
%record_offset = OpIMul %uint %safe_index %uint_record
%record_words = OpIAdd %uint %record_offset %uint_records
<records>               OpReturn
               OpFunctionEnd
)";

	// Reads dword `words` (relative to the group) of the output buffer into %name.
	auto load = [](const String8& name, const String8& words)
	{
		return String8(R"(%<n>_index = OpIAdd %uint %group_words %<w>
%<n>_index64 = OpUConvert %ulong %<n>_index
%<n>_bytes = OpShiftLeftLogical %ulong %<n>_index64 %uint_2
%<n>_address = OpIAdd %ulong %output %<n>_bytes
%<n>_pointer = OpConvertUToPtr %_ptr_PhysicalStorageBuffer_uint %<n>_address
%<n> = OpLoad %uint %<n>_pointer Aligned 4
)")
		    .ReplaceStr("<n>", name)
		    .ReplaceStr("<w>", words);
	};
	// Reads a four-dword slot of the selected record as a vec4.
	auto load_vec4 = [&](const String8& name, uint32_t slot)
	{
		String8 text;
		for (uint32_t c = 0; c < 4u; ++c)
		{
			const auto component = String8::FromPrintf("%s_c%u", name.c_str(), c);
			text += String8::FromPrintf("%%%s_word = OpIAdd %%uint %%record_words %%uint_slot_%u\n", component.c_str(), slot + c);
			text += load(component, component + "_word");
			text += String8::FromPrintf("%%%s_f = OpBitcast %%float %%%s\n", component.c_str(), component.c_str());
		}
		text += String8::FromPrintf("%%%s_raw = OpCompositeConstruct %%v4float %%%s_c0_f %%%s_c1_f %%%s_c2_f %%%s_c3_f\n", name.c_str(),
		                            name.c_str(), name.c_str(), name.c_str(), name.c_str());
		text += String8::FromPrintf("%%%s = OpSelect %%v4float %%live %%%s_raw %%v4float_0\n", name.c_str(), name.c_str());
		return text;
	};

	const uint32_t slots = 8u + 4u * geometry.parameter_count;
	String8        interface;
	String8        decorations;
	String8        variables;
	String8        records;
	for (uint32_t slot = 0; slot < slots; ++slot)
	{
		variables += String8::FromPrintf("%%uint_slot_%u = OpConstant %%uint %u\n", slot, slot);
	}
	// A dead primitive collapses to one point at the origin and rasterizes nothing.
	records += load_vec4("position", 0u);
	records += "%position_ptr = OpAccessChain %_ptr_Output_v4float %outPerVertex %int_0\n"
	           "               OpStore %position_ptr %position\n";
	for (uint32_t i = 0; i < geometry.parameter_count; ++i)
	{
		interface += String8::FromPrintf(" %%param%u", i);
		decorations += String8::FromPrintf("               OpDecorate %%param%u Location %u\n", i, i);
		variables += String8::FromPrintf("%%param%u = OpVariable %%_ptr_Output_v4float Output\n", i);
		const auto name = String8::FromPrintf("param%u_value", i);
		records += load_vec4(name, 8u + 4u * i);
		records += String8::FromPrintf("               OpStore %%param%u %%%s\n", i, name.c_str());
	}
	if (layer)
	{
		// The miscellaneous position vector carries the render-target layer in Z[10:0].
		interface += " %gl_Layer";
		decorations += "               OpDecorate %gl_Layer BuiltIn Layer\n";
		variables += "%gl_Layer = OpVariable %_ptr_Output_int Output\n";
		records += "%layer_word = OpIAdd %uint %record_words %uint_slot_6\n";
		records += load("layer_raw", "layer_word");
		records += "%layer_bits = OpBitwiseAnd %uint %layer_raw %uint_layer_mask\n"
		           "%layer_live = OpSelect %uint %live %layer_bits %uint_0\n"
		           "%layer = OpBitcast %int %layer_live\n"
		           "               OpStore %gl_Layer %layer\n";
	}
	return String8(head)
	    .ReplaceStr("<layer_capability>", layer ? "               OpCapability ShaderLayer\n" : "")
	    .ReplaceStr("<interface>", interface)
	    .ReplaceStr("<decorations>", decorations)
	    .ReplaceStr("<variables>", variables)
	    .ReplaceStr("<group>", String8::FromPrintf("%u", geometry.GroupDwords()))
	    .ReplaceStr("<record>", String8::FromPrintf("%u", geometry.RecordDwords()))
	    .ReplaceStr("<records_offset>", String8::FromPrintf("%u", geometry.RecordsOffset()))
	    .ReplaceStr("<lanes>", String8::FromPrintf("%u", geometry.GroupLanes()))
	    .ReplaceStr("<group_corners>", String8::FromPrintf("%u", geometry.GroupLanes() * 3u))
	    .ReplaceStr("<load_header>", load("header", "uint_0"))
	    .ReplaceStr("<load_primitive>", load("primitive_word", "primitive_word_offset"))
	    .ReplaceStr("<records>", records);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
