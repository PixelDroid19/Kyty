#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_KHR_shader_subgroup_quad : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#define FRAGMENT_CAPTURE
#include "fragment_wave_layout.glsl"

layout(location=0) in vec4 capture_stub;
layout(std430, set=0, binding=0) buffer Records { uint records[]; } record_buffer;
layout(std430, set=0, binding=1) buffer Control { uint control[]; } control_buffer;
layout(std430, set=0, binding=2) buffer References { uint references[]; } reference_buffer;

// Replaced by the renderer's admitted pixel input layout before assembly.
uint read_lane_word(uint word) { return floatBitsToUint(capture_stub[word & 3u]); }

uint hash_key(uint primitive, uint xy) {
    uint h = xy * 0x9e3779b9u ^ primitive * 0x85ebca6bu;
    h ^= h >> 16u;
    h *= 0x7feb352du;
    return h ^ (h >> 15u);
}

// Claims a lookup slot for one quad. The record is reserved afterwards for the
// whole subgroup at once.
uint claim_slot(uint primitive, uint xy) {
    if (primitive >= transport.primitive_count) {
        atomicOr(control_buffer.control[1], 4u);
        return invalid_reference;
    }
    uint hash = hash_key(primitive, xy);
    for (uint probe=0u; probe < min(transport.quad_lookup_capacity, 128u); ++probe) {
        uint slot = (hash + probe) & (transport.quad_lookup_capacity - 1u);
        uint base = 16u + transport.primitive_count * 2u + slot * 4u;
        if (atomicCompSwap(control_buffer.control[base], 0u, 1u) == 0u) return slot;
    }
    atomicOr(control_buffer.control[1], 2u);
    return invalid_reference;
}

// One atomic add per subgroup: every covered quad of the band adds to this
// single counter, so a per-quad add serializes them. The leader is a claiming
// lane, never a helper, because a helper's atomic has no effect. The count may
// pass the capacity only after an overflow, which fails the whole band; readers
// bound records by quad_capacity.
uint reserve_record(bool claimed) {
    uint claim = claimed ? 1u : 0u;
    uint count = subgroupAdd(claim);
    if (count == 0u) return invalid_reference;
    uint leader = subgroupMin(claimed ? gl_SubgroupInvocationID : 0xffffffffu);
    uint first_record = 0u;
    if (gl_SubgroupInvocationID == leader) first_record = atomicAdd(control_buffer.control[0], count);
    first_record = subgroupMax(first_record);
    if (!claimed) return invalid_reference;
    uint record = first_record + subgroupExclusiveAdd(claim);
    if (record < transport.quad_capacity) return record;
    atomicOr(control_buffer.control[1], 1u);
    return invalid_reference;
}

// Numbers the reserved quads of each primitive with one add per primitive
// present in the subgroup, from a reserving lane. Only reserved records take
// an ordinal, so a counter cannot exceed quad_capacity or wrap in this draw.
uint reserve_ordinal(bool reserved, uint primitive) {
    uint ordinal = 0u;
    bool pending = reserved;
    while (subgroupAdd(pending ? 1u : 0u) != 0u) {
        uint leader = subgroupMin(pending ? gl_SubgroupInvocationID : 0xffffffffu);
        uint leader_primitive = subgroupMax(gl_SubgroupInvocationID == leader ? primitive : 0u);
        uint member = pending && primitive == leader_primitive ? 1u : 0u;
        uint count = subgroupAdd(member);
        uint rank = subgroupExclusiveAdd(member);
        uint first_ordinal = 0u;
        if (gl_SubgroupInvocationID == leader) first_ordinal = atomicAdd(control_buffer.control[17u + leader_primitive * 2u], count);
        first_ordinal = subgroupMax(first_ordinal);
        if (member != 0u) {
            ordinal = first_ordinal + rank;
            pending = false;
        }
    }
    return ordinal;
}

void store_quad(uint slot, uint record, uint ordinal, uint primitive, uint xy, uint mask) {
    uint header = record * quad_words();
    record_buffer.records[header] = mask;
    record_buffer.records[header + 1u] = 1u;
    record_buffer.records[header + 2u] = primitive;
    record_buffer.records[header + 3u] = ordinal;
    uint key = ((primitive << 16u) | (ordinal / 16u)) + 1u;
    uint wave_hash = hash_key(primitive, ordinal / 16u);
    bool found = false;
    for (uint attempt=0u; attempt < min(transport.wave_lookup_capacity, 128u); ++attempt) {
        uint wave_slot = (wave_hash + attempt) & (transport.wave_lookup_capacity - 1u);
        uint previous = atomicCompSwap(control_buffer.control[wave_table_offset() + wave_slot], 0u, key);
        if (previous != 0u && previous != key) continue;
        reference_buffer.references[wave_slot * 16u + (ordinal & 15u)] = record;
        found = true;
        break;
    }
    if (!found) atomicOr(control_buffer.control[1], 64u);
    uint base = 16u + transport.primitive_count * 2u + slot * 4u;
    control_buffer.control[base + 1u] = primitive;
    control_buffer.control[base + 2u] = xy;
    control_buffer.control[base + 3u] = record;
    atomicExchange(control_buffer.control[base], 2u);
}

void main() {
    // Quad operations precede any helper/writer branch. A helper cannot write
    // storage, so one covered invocation exports every member's raw inputs.
    uint covered = gl_HelperInvocation ? 0u : 1u;
    uint mask = 0u;
    for (uint member=0u; member < 4u; ++member) mask |= subgroupQuadBroadcast(covered, member) << member;
    uint first = mask == 0u ? 0u : uint(findLSB(mask));
    uint lane = gl_SubgroupInvocationID & 3u;
    vec2 origin = subgroupQuadBroadcast(gl_FragCoord.xy, 0u);
    uvec2 quad = uvec2(origin) / 2u;
    bool writer = covered != 0u && lane == first;
    uint primitive = uint(gl_PrimitiveID);
    uint xy = quad.x | (quad.y << 16u);
    uint slot = invalid_reference;
    if (writer) {
        if (control_buffer.control.length() < wave_table_offset() + transport.wave_lookup_capacity ||
            record_buffer.records.length() < transport.quad_capacity * quad_words() ||
            reference_buffer.references.length() < output_reference_offset() + transport.quad_capacity) {
            if (control_buffer.control.length() >= 16u) atomicOr(control_buffer.control[1], buffer_error);
        } else if (quad.x >= 32768u || quad.y >= 32768u) {
            atomicOr(control_buffer.control[1], 4u);
        } else {
            slot = claim_slot(primitive, xy);
        }
    }
    // Uniform control flow: the reservation is a subgroup operation.
    uint record = reserve_record(slot != invalid_reference);
    uint ordinal = reserve_ordinal(record != invalid_reference, primitive);
    if (record != invalid_reference) store_quad(slot, record, ordinal, primitive, xy, mask);
    record = subgroupQuadBroadcast(record, first);
    for (uint word=0u; word < transport.lane_words; ++word) {
        uint own = read_lane_word(word);
        for (uint member=0u; member < 4u; ++member) {
            uint value = subgroupQuadBroadcast(own, member);
            if (writer && record != invalid_reference) {
                record_buffer.records[record * quad_words() + 4u + member * transport.lane_words + word] = value;
            }
        }
    }
}
