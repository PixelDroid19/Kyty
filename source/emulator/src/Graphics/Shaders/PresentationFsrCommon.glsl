// Presentation FSR 1.0 passes: shared interface and sRGB transfer functions.
// EASU and RCAS run on display-encoded values; the flags say which side holds linear light.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D u_source;
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D u_target;

layout(push_constant) uniform Parameters
{
	vec2  source_size;   // The region of u_source that holds the image, in texels.
	vec2  texture_size;  // The full extent of u_source.
	vec2  target_size;   // The extent written to u_target.
	float sharpness;     // RCAS attenuation in stops; 0 is the strongest.
	uint  flags;
} u_parameters;

const uint kFlagSourceLinear = 1u; // u_source holds linear light: encode it before filtering.
const uint kFlagTargetLinear = 2u; // u_target must hold linear light: decode the filtered result.

vec4 SrgbEncode(vec4 c)
{
	c = clamp(c, 0.0, 1.0);
	return mix(c * 12.92, 1.055 * pow(c, vec4(1.0 / 2.4)) - 0.055, step(vec4(0.0031308), c));
}

vec3 SrgbDecode(vec3 c)
{
	c = clamp(c, 0.0, 1.0);
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec4 SourceEncoded(vec4 c)
{
	return (u_parameters.flags & kFlagSourceLinear) != 0u ? SrgbEncode(c) : c;
}

void StoreTarget(ivec2 p, vec3 c)
{
	if ((u_parameters.flags & kFlagTargetLinear) != 0u)
	{
		c = SrgbDecode(c);
	}
	imageStore(u_target, p, vec4(c, 1.0));
}
