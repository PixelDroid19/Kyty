#version 460
#extension GL_GOOGLE_include_directive : require
#include "fragment_wave_layout.glsl"

layout(location=0) out vec4 resolve_stub;
layout(constant_id=0) const uint required_components = 15u;
layout(std430, set=0, binding=0) readonly buffer Records { uint records[]; } record_buffer;
layout(std430, set=0, binding=1) buffer Control { uint control[]; } control_buffer;
layout(std430, set=0, binding=2) readonly buffer References { uint references[]; } reference_buffer;
layout(std430, set=0, binding=4) readonly buffer Results { uint results[]; } result_buffer;

uint hash_key(uint primitive, uint xy) {
    uint h = xy * 0x9e3779b9u ^ primitive * 0x85ebca6bu;
    h ^= h >> 16u;
    h *= 0x7feb352du;
    return h ^ (h >> 15u);
}

void write_colors(uint base) {
    resolve_stub = uintBitsToFloat(uvec4(result_buffer.results[base+2u], result_buffer.results[base+3u],
                                       result_buffer.results[base+4u], result_buffer.results[base+5u]));
}

void fail(uint reason) { atomicOr(control_buffer.control[1], reason); }

void main() {
    if (gl_HelperInvocation) discard;
    if (control_buffer.control.length() < 16u) discard;
    if (atomicAdd(control_buffer.control[1], 0u) != 0u) discard;
    if (control_buffer.control.length() < wave_table_offset() + transport.wave_lookup_capacity ||
        record_buffer.records.length() < transport.quad_capacity * quad_words() ||
        reference_buffer.references.length() < output_reference_offset() + transport.quad_capacity ||
        result_buffer.results.length() < transport.wave_capacity * 64u * 34u) {
        fail(buffer_error);
        discard;
    }
    uvec2 pixel = uvec2(gl_FragCoord.xy), quad = pixel / 2u;
    uint primitive = uint(gl_PrimitiveID);
    if (primitive >= transport.primitive_count || quad.x >= 32768u || quad.y >= 32768u) {
        fail(4u);
        discard;
    }
    uint xy = quad.x | (quad.y << 16u), hash = hash_key(primitive, xy), record = invalid_reference;
    for (uint attempt=0u; attempt < min(transport.quad_lookup_capacity, 128u); ++attempt) {
        uint base = 16u + transport.primitive_count * 2u + ((hash + attempt) & (transport.quad_lookup_capacity - 1u)) * 4u;
        if (control_buffer.control[base] == 0u) break;
        if (control_buffer.control[base] == 2u && control_buffer.control[base+1u] == primitive &&
            control_buffer.control[base+2u] == xy) {
            record = control_buffer.control[base+3u];
            break;
        }
    }
    if (record >= transport.quad_capacity || record >= control_buffer.control[0]) {
        fail(16u);
        discard;
    }
    uint lane = (pixel.y & 1u) * 2u + (pixel.x & 1u);
    uint wave_lane = reference_buffer.references[output_reference_offset() + record];
    if (wave_lane >= transport.wave_capacity * 64u || (wave_lane & 3u) != 0u ||
        record_buffer.records[record * quad_words()+2u] != primitive ||
        (record_buffer.records[record * quad_words()] & (1u << lane)) == 0u) {
        fail(reference_error);
        discard;
    }
    uint base = (wave_lane + lane) * 34u;
    if (result_buffer.results[base] == 0u) discard;
    if ((result_buffer.results[base+1u] & required_components) != required_components) {
        fail(256u);
        discard;
    }
    write_colors(base);
}
