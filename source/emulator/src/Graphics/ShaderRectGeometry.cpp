#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/DbgAssert.h"
#include "ShaderSpirvToolchain.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

// Corners of the triangle's bounding box in normalized device coordinates, in strip order with
// the triangle's winding, and the triangle's barycentric weights at each corner.
const char* const kRectBoundingBox = R"(%pos_xy_0 = OpVectorShuffle %v2float %pos_v0 %pos_v0 0 1
%pos_xy_1 = OpVectorShuffle %v2float %pos_v1 %pos_v1 0 1
%pos_xy_2 = OpVectorShuffle %v2float %pos_v2 %pos_v2 0 1
%pos_w_0 = OpCompositeExtract %float %pos_v0 3
%pos_w_1 = OpCompositeExtract %float %pos_v1 3
%pos_w_2 = OpCompositeExtract %float %pos_v2 3
%pos_ww_0 = OpCompositeConstruct %v2float %pos_w_0 %pos_w_0
%pos_ww_1 = OpCompositeConstruct %v2float %pos_w_1 %pos_w_1
%pos_ww_2 = OpCompositeConstruct %v2float %pos_w_2 %pos_w_2
%ndc_0 = OpFDiv %v2float %pos_xy_0 %pos_ww_0
%ndc_1 = OpFDiv %v2float %pos_xy_1 %pos_ww_1
%ndc_2 = OpFDiv %v2float %pos_xy_2 %pos_ww_2
%min_01 = OpExtInst %v2float %glsl FMin %ndc_0 %ndc_1
%corner_0 = OpExtInst %v2float %glsl FMin %min_01 %ndc_2
%max_01 = OpExtInst %v2float %glsl FMax %ndc_0 %ndc_1
%corner_3 = OpExtInst %v2float %glsl FMax %max_01 %ndc_2
%min_x = OpCompositeExtract %float %corner_0 0
%min_y = OpCompositeExtract %float %corner_0 1
%max_x = OpCompositeExtract %float %corner_3 0
%max_y = OpCompositeExtract %float %corner_3 1
%corner_right = OpCompositeConstruct %v2float %max_x %min_y
%corner_left = OpCompositeConstruct %v2float %min_x %max_y
%d1 = OpFSub %v2float %ndc_1 %ndc_0
%d2 = OpFSub %v2float %ndc_2 %ndc_0
%d1_x = OpCompositeExtract %float %d1 0
%d1_y = OpCompositeExtract %float %d1 1
%d2_x = OpCompositeExtract %float %d2 0
%d2_y = OpCompositeExtract %float %d2 1
%area_a = OpFMul %float %d1_x %d2_y
%area_b = OpFMul %float %d1_y %d2_x
%area = OpFSub %float %area_a %area_b
%counter_clockwise = OpFOrdGreaterThanEqual %bool %area %f0
%counter_clockwise2 = OpCompositeConstruct %v2bool %counter_clockwise %counter_clockwise
%corner_1 = OpSelect %v2float %counter_clockwise2 %corner_right %corner_left
%corner_2 = OpSelect %v2float %counter_clockwise2 %corner_left %corner_right
)";

const char* const kRectTypes = R"(%void = OpTypeVoid
%bool = OpTypeBool
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%v2float = OpTypeVector %float 2
%v4float = OpTypeVector %float 4
%v2bool = OpTypeVector %bool 2
%u0 = OpConstant %uint 0
%u1 = OpConstant %uint 1
%u2 = OpConstant %uint 2
%u3 = OpConstant %uint 3
%f0 = OpConstant %float 0
%f1 = OpConstant %float 1
%vertex = OpTypeStruct %v4float
%single_float = OpTypeArray %float %u1
%vertex_input = OpTypeStruct %v4float %float %single_float %single_float
%vertices_type = OpTypeArray %vertex_input %u3
%triangle = OpTypeArray %v4float %u3
%input_vertices = OpTypePointer Input %vertices_type
%input_triangle = OpTypePointer Input %triangle
%input_vector = OpTypePointer Input %v4float
%output_vertex_type = OpTypePointer Output %vertex
%output_vector = OpTypePointer Output %v4float
%function = OpTypeFunction %void
%vertices = OpVariable %input_vertices Input
%output_vertex = OpVariable %output_vertex_type Output
)";

// Barycentric weights of the triangle at a corner; outside the triangle they extrapolate its plane.
const char* const kRectCornerWeights = R"(%q<s> = OpFSub %v2float %corner_<s> %ndc_0
%q<s>_x = OpCompositeExtract %float %q<s> 0
%q<s>_y = OpCompositeExtract %float %q<s> 1
%w1_a<s> = OpFMul %float %q<s>_x %d2_y
%w1_b<s> = OpFMul %float %q<s>_y %d2_x
%w1_n<s> = OpFSub %float %w1_a<s> %w1_b<s>
%w1_<s> = OpFDiv %float %w1_n<s> %area
%w2_a<s> = OpFMul %float %d1_x %q<s>_y
%w2_b<s> = OpFMul %float %d1_y %q<s>_x
%w2_n<s> = OpFSub %float %w2_a<s> %w2_b<s>
%w2_<s> = OpFDiv %float %w2_n<s> %area
%w12_<s> = OpFAdd %float %w1_<s> %w2_<s>
%w0_<s> = OpFSub %float %f1 %w12_<s>
)";

// One vertex output at a corner, weighted by the corner's barycentrics.
const char* const kRectCornerValue = R"(%<v>_s<s>_p0 = OpVectorTimesScalar %v4float %<v>_v0 %w0_<s>
%<v>_s<s>_p1 = OpVectorTimesScalar %v4float %<v>_v1 %w1_<s>
%<v>_s<s>_p2 = OpVectorTimesScalar %v4float %<v>_v2 %w2_<s>
%<v>_s<s>_p01 = OpFAdd %v4float %<v>_s<s>_p0 %<v>_s<s>_p1
%<v>_s<s> = OpFAdd %v4float %<v>_s<s>_p01 %<v>_s<s>_p2
)";

String8 RectCorner(const char* text, const String8& value, uint32_t slot)
{
	return String8(text).ReplaceStr("<v>", value).ReplaceStr("<s>", String8::FromPrintf("%u", slot));
}

} // namespace

// A rect primitive rasterizes the screen-space bounding box of its three shaded vertices.
// Smooth outputs extend the triangle's plane over the box; parameters the pixel stage reads flat
// keep the first vertex's raw value, the provoking vertex Kyty uses for every primitive.
Vector<uint32_t> ShaderCompileRectGeometry(uint32_t parameter_count, uint32_t flat_parameters)
{
	EXIT_IF(parameter_count > 32u);
	String8 interface;
	String8 annotations;
	String8 variables;
	String8 body = "%main = OpFunction %void None %function\n%entry = OpLabel\n"
	               "%position_out = OpAccessChain %output_vector %output_vertex %u0\n";
	for (uint32_t vertex = 0; vertex < 3u; ++vertex)
	{
		body += String8::FromPrintf("%%pos_ptr%u = OpAccessChain %%input_vector %%vertices %%u%u %%u0\n"
		                            "%%pos_v%u = OpLoad %%v4float %%pos_ptr%u\n",
		                            vertex, vertex, vertex, vertex);
	}
	body += kRectBoundingBox;
	for (uint32_t slot = 0; slot < 4u; ++slot)
	{
		body += RectCorner(kRectCornerWeights, "", slot);
		body += RectCorner(kRectCornerValue, "pos", slot);
	}
	for (uint32_t param = 0; param < parameter_count; ++param)
	{
		interface += String8::FromPrintf(" %%param_in%u %%param_out%u", param, param);
		annotations += String8::FromPrintf("OpDecorate %%param_in%u Location %u\nOpDecorate %%param_out%u Location %u\n", param, param,
		                                   param, param);
		variables += String8::FromPrintf("%%param_in%u = OpVariable %%input_triangle Input\n"
		                                 "%%param_out%u = OpVariable %%output_vector Output\n",
		                                 param, param);
		const String8 value = String8::FromPrintf("param%u", param);
		for (uint32_t vertex = 0; vertex < 3u; ++vertex)
		{
			body += String8::FromPrintf("%%param%u_ptr%u = OpAccessChain %%input_vector %%param_in%u %%u%u\n"
			                            "%%param%u_v%u = OpLoad %%v4float %%param%u_ptr%u\n",
			                            param, vertex, param, vertex, param, vertex, param, vertex);
		}
		if ((flat_parameters & (1u << param)) != 0) { continue; }
		for (uint32_t slot = 0; slot < 4u; ++slot) { body += RectCorner(kRectCornerValue, value, slot); }
	}
	for (uint32_t slot = 0; slot < 4u; ++slot)
	{
		body += String8::FromPrintf("OpStore %%position_out %%pos_s%u\n", slot);
		for (uint32_t param = 0; param < parameter_count; ++param)
		{
			const bool flat = (flat_parameters & (1u << param)) != 0;
			body += flat ? String8::FromPrintf("OpStore %%param_out%u %%param%u_v0\n", param, param)
			             : String8::FromPrintf("OpStore %%param_out%u %%param%u_s%u\n", param, param, slot);
		}
		body += "OpEmitVertex\n";
	}
	body += "OpEndPrimitive\nOpReturn\nOpFunctionEnd\n";
	// The input block repeats the vertex stage's built-in block, which interface matching requires.
	const String8 header = String8("OpCapability Shader\nOpCapability Geometry\nOpCapability GeometryPointSize\n"
	                               "OpCapability ClipDistance\nOpCapability CullDistance\n%glsl = OpExtInstImport \"GLSL.std.450\"\n"
	                               "OpMemoryModel Logical GLSL450\nOpEntryPoint Geometry %main \"main\" %vertices %output_vertex") +
	                       interface +
	                       "\nOpExecutionMode %main Triangles\nOpExecutionMode %main OutputTriangleStrip\n"
	                       "OpExecutionMode %main OutputVertices 4\nOpExecutionMode %main Invocations 1\n"
	                       "OpMemberDecorate %vertex 0 BuiltIn Position\nOpDecorate %vertex Block\n"
	                       "OpMemberDecorate %vertex_input 0 BuiltIn Position\nOpMemberDecorate %vertex_input 1 BuiltIn PointSize\n"
	                       "OpMemberDecorate %vertex_input 2 BuiltIn ClipDistance\nOpMemberDecorate %vertex_input 3 BuiltIn CullDistance\n"
	                       "OpDecorate %vertex_input Block\n";
	Vector<uint32_t> binary;
	String8          error;
	const auto       assembly = header + annotations + kRectTypes + variables + body;
	EXIT_IF(!ShaderToolchain::Run(assembly, &binary, &error));
	return binary;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
