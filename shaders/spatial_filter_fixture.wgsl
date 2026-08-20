// 0.13.0a spatial-filter host/WGSL contract fixture.
// This intentionally performs only edge-mapped identity sampling. Public blur
// and sharpen kernels arrive in 0.13.0b, reusing this exact 64-byte layout.
struct SpatialFilterContract {
    source_extent: vec2<u32>,
    output_origin: vec2<i32>,
    output_extent: vec2<u32>,
    radius: vec2<u32>,
    mode_flags: vec4<u32>,
    sampling_origin: vec2<i32>,
    sampling_extent: vec2<u32>,
};

struct MappedCoordinate {
    value: i32,
    inside: u32,
};

@group(0) @binding(0) var source_texture: texture_2d<f32>;
@group(0) @binding(1) var output_texture: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> contract: SpatialFilterContract;

fn positive_modulo(value: i32, modulus: i32) -> i32 {
    let remainder = value % modulus;
    return select(remainder, remainder + modulus, remainder < 0);
}

fn map_coordinate(coordinate: i32, extent: i32, edge_mode: u32) -> MappedCoordinate {
    if (coordinate >= 0 && coordinate < extent) {
        return MappedCoordinate(coordinate, 1u);
    }
    switch (edge_mode) {
        case 0u: {
            return MappedCoordinate(clamp(coordinate, 0, extent - 1), 1u);
        }
        case 1u: {
            if (extent <= 1) { return MappedCoordinate(0, 1u); }
            let period = extent * 2;
            let folded = positive_modulo(coordinate, period);
            let mirrored = select(period - 1 - folded, folded, folded < extent);
            return MappedCoordinate(mirrored, 1u);
        }
        case 2u: {
            return MappedCoordinate(positive_modulo(coordinate, extent), 1u);
        }
        default: {
            return MappedCoordinate(0, 0u);
        }
    }
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) invocation: vec3<u32>) {
    let output_extent = contract.output_extent;
    if (invocation.x >= output_extent.x || invocation.y >= output_extent.y) {
        return;
    }
    let source_extent = vec2<i32>(contract.source_extent);
    let output_origin = contract.output_origin;
    let document_position = output_origin + vec2<i32>(invocation.xy);
    let mapped_x = map_coordinate(document_position.x, source_extent.x,
                                  contract.mode_flags.x);
    let mapped_y = map_coordinate(document_position.y, source_extent.y,
                                  contract.mode_flags.x);
    var sampled = vec4<f32>(0.0);
    if (mapped_x.inside != 0u && mapped_y.inside != 0u) {
        sampled = textureLoad(source_texture,
                              vec2<i32>(mapped_x.value, mapped_y.value), 0);
    }
    textureStore(output_texture, vec2<i32>(invocation.xy), sampled);
}
