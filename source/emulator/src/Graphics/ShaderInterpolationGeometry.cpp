#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/DbgAssert.h"
#include "ShaderSpirvToolchain.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

struct GeometrySource
{
	String8 interface;
	String8 annotations;
	String8 variables;
	String8 body;
	uint32_t source_mask = 0;
};

bool GeometryInput(const ShaderPixelInputInfo& info, uint32_t input, ShaderPixelInterpolator* decoded)
{
	if (!ShaderPixelInputActive(info, input) || ShaderPixelCanonicalInterpolator(info, input) != input) { return false; }
	EXIT_IF(!ShaderDecodePixelInterpolator(info.interpolator_settings[input], decoded));
	return decoded->source != ShaderPixelInterpolatorSource::Default;
}

void GeometryDeclareParameter(GeometrySource* source, uint32_t location)
{
	if ((source->source_mask & (1u << location)) != 0) { return; }
	source->source_mask |= 1u << location;
	source->interface += String8::FromPrintf(" %%param%u", location);
	source->annotations += String8::FromPrintf("OpDecorate %%param%u Location %u\n", location, location);
	source->variables += String8::FromPrintf("%%param%u = OpVariable %%input_triangle Input\n", location);
}

void GeometryDeclareInputs(const ShaderPixelInputInfo& info, GeometrySource* source)
{
	for (uint32_t input = 0; input < info.input_num; ++input)
	{
		ShaderPixelInterpolator decoded {};
		if (!GeometryInput(info, input, &decoded)) { continue; }
		GeometryDeclareParameter(source, decoded.location);
		source->interface += String8::FromPrintf(" %%attr%u", input);
		source->annotations += String8::FromPrintf("OpDecorate %%attr%u Location %u\n", input,
		                                          info.custom_interpolation.locations[input]);
		const bool raw = decoded.source == ShaderPixelInterpolatorSource::PerVertex;
		if (raw) { source->annotations += String8::FromPrintf("OpDecorate %%attr%u Flat\n", input); }
		source->variables += String8::FromPrintf("%%attr%u = OpVariable %%%s Output\n", input,
		                                        raw ? "output_raw_triangle" : "output_vector");
	}
	for (uint32_t field: {1u, 2u, 5u, 6u})
	{
		if ((info.system_input_enable & (1u << field)) == 0) { continue; }
		source->interface += String8::FromPrintf(" %%bary%u", field);
		source->annotations += String8::FromPrintf("OpDecorate %%bary%u Location %u\n", field,
		                                          info.custom_interpolation.barycentric_locations[field]);
		source->variables += String8::FromPrintf("%%bary%u = OpVariable %%output_pair Output\n", field);
	}
}

void GeometryLoadRaw(const ShaderPixelInputInfo& info, GeometrySource* source)
{
	for (uint32_t input = 0; input < info.input_num; ++input)
	{
		ShaderPixelInterpolator decoded {};
		if (!GeometryInput(info, input, &decoded) || decoded.source != ShaderPixelInterpolatorSource::PerVertex) { continue; }
		for (uint32_t vertex = 0; vertex < 3u; ++vertex)
		{
			source->body += String8::FromPrintf(
			    "%%raw_ptr_%u_%u = OpAccessChain %%input_vector %%param%u %%u%u\n"
			    "%%raw_float_%u_%u = OpLoad %%v4float %%raw_ptr_%u_%u\n"
			    "%%raw_%u_%u = OpBitcast %%v4uint %%raw_float_%u_%u\n",
			    input, vertex, decoded.location, vertex, input, vertex, input, vertex, input, vertex, input, vertex);
		}
		source->body += String8::FromPrintf("%%raw_all_%u = OpCompositeConstruct %%raw_triangle %%raw_%u_0 %%raw_%u_1 %%raw_%u_2\n",
		                                      input, input, input, input);
	}
}

void GeometryEmitVertex(const ShaderPixelInputInfo& info, uint32_t vertex, GeometrySource* source)
{
	source->body += String8::FromPrintf(
	    "%%position_ptr_%u = OpAccessChain %%input_vector %%vertices %%u%u %%u0\n"
	    "%%position_%u = OpLoad %%v4float %%position_ptr_%u\n"
	    "OpStore %%position_out %%position_%u\n", vertex, vertex, vertex, vertex, vertex);
	for (uint32_t input = 0; input < info.input_num; ++input)
	{
		ShaderPixelInterpolator decoded {};
		if (!GeometryInput(info, input, &decoded)) { continue; }
		if (decoded.source == ShaderPixelInterpolatorSource::PerVertex)
		{
			source->body += String8::FromPrintf("OpStore %%attr%u %%raw_all_%u\n", input, input);
			continue;
		}
		source->body += String8::FromPrintf(
		    "%%smooth_ptr_%u_%u = OpAccessChain %%input_vector %%param%u %%u%u\n"
		    "%%smooth_%u_%u = OpLoad %%v4float %%smooth_ptr_%u_%u\n"
		    "OpStore %%attr%u %%smooth_%u_%u\n", input, vertex, decoded.location, vertex,
		    input, vertex, input, vertex, input, input, vertex);
	}
	for (uint32_t field: {1u, 2u, 5u, 6u})
	{
		if ((info.system_input_enable & (1u << field)) != 0)
		{
			source->body += String8::FromPrintf("OpStore %%bary%u %%weight%u\n", field, vertex);
		}
	}
	source->body += "OpEmitVertex\n";
}

} // namespace

Vector<uint32_t> ShaderCompileInterpolationGeometry(const ShaderPixelInputInfo& info)
{
	EXIT_IF(!info.custom_interpolation.Enabled());
	GeometrySource source;
	GeometryDeclareInputs(info, &source);
	source.body = "%main = OpFunction %void None %function\n%entry = OpLabel\n"
	              "%position_out = OpAccessChain %output_vector %output_vertex %u0\n";
	GeometryLoadRaw(info, &source);
	for (uint32_t vertex = 0; vertex < 3u; ++vertex) { GeometryEmitVertex(info, vertex, &source); }
	source.body += "OpEndPrimitive\nOpReturn\nOpFunctionEnd\n";
	const String8 header = "OpCapability Shader\nOpCapability Geometry\nOpMemoryModel Logical GLSL450\n"
	                       "OpEntryPoint Geometry %main \"main\" %vertices %output_vertex";
	const String8 modes = "\nOpExecutionMode %main Triangles\nOpExecutionMode %main OutputTriangleStrip\n"
	                      "OpExecutionMode %main OutputVertices 3\nOpExecutionMode %main Invocations 1\n"
	                      "OpMemberDecorate %vertex 0 BuiltIn Position\nOpDecorate %vertex Block\n";
	const String8 types = R"(
%void = OpTypeVoid
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%v2float = OpTypeVector %float 2
%v4float = OpTypeVector %float 4
%v4uint = OpTypeVector %uint 4
%u0 = OpConstant %uint 0
%u1 = OpConstant %uint 1
%u2 = OpConstant %uint 2
%u3 = OpConstant %uint 3
%f0 = OpConstant %float 0
%f1 = OpConstant %float 1
%weight0 = OpConstantComposite %v2float %f0 %f0
%weight1 = OpConstantComposite %v2float %f1 %f0
%weight2 = OpConstantComposite %v2float %f0 %f1
%vertex = OpTypeStruct %v4float
%vertices_type = OpTypeArray %vertex %u3
%triangle = OpTypeArray %v4float %u3
%raw_triangle = OpTypeArray %v4uint %u3
%input_vertices = OpTypePointer Input %vertices_type
%input_triangle = OpTypePointer Input %triangle
%input_vector = OpTypePointer Input %v4float
%output_vertex_type = OpTypePointer Output %vertex
%output_vector = OpTypePointer Output %v4float
%output_pair = OpTypePointer Output %v2float
%output_raw_triangle = OpTypePointer Output %raw_triangle
%function = OpTypeFunction %void
%vertices = OpVariable %input_vertices Input
%output_vertex = OpVariable %output_vertex_type Output
)";
	Vector<uint32_t> binary;
	String8 error;
	EXIT_IF(!ShaderToolchain::Run(header + source.interface + modes + source.annotations + types + source.variables + source.body,
	                            &binary, &error));
	return binary;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
