// Host transport buffers follow FragmentTransportLayout. These are not
// architectural guest descriptors or a guest parameter-cache allocation.
layout(push_constant) uniform TransportParameters {
    uint quad_capacity;
    uint primitive_count;
    uint lane_words;
    uint wave_header_words;
    uint quad_lookup_capacity;
    uint wave_capacity;
    uint wave_lookup_capacity;
} transport;

const uint invalid_reference = 0xffffffffu;
const uint capacity_error = 8u;
const uint reference_error = 32u;
const uint buffer_error = 128u;

uint quad_words() { return 4u + 4u * transport.lane_words; }
uint wave_words() { return transport.wave_header_words + 64u * transport.lane_words; }
uint wave_table_offset() { return 16u + 2u * transport.primitive_count + 4u * transport.quad_lookup_capacity; }
uint active_reference_offset() { return 16u * transport.wave_lookup_capacity; }
uint output_reference_offset() { return active_reference_offset() + transport.wave_capacity; }
