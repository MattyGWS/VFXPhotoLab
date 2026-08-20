// VFX Photo Lab 0.7.0d.4
// Bounded tiled straight-component Image Size resampling. The host supplies
// global source/destination extents and tile origins so every output tile uses
// the same pixel-centre mapping across tile boundaries.
struct ResampleParams {
    source_size: vec2<u32>,
    destination_size: vec2<u32>,
    source_origin: vec2<i32>,
    destination_origin: vec2<u32>,
    method: u32,
    _padding0: u32,
    _padding1: u32,
    _padding2: u32,
};

@group(0) @binding(0) var source_patch: texture_2d<f32>;
@group(0) @binding(1) var output_tile: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: ResampleParams;

fn nearest_index(destination: u32, source_extent: u32, destination_extent: u32) -> i32 {
    let numerator = (2u * destination + 1u) * source_extent;
    return i32(numerator / (2u * destination_extent));
}

fn linear_position(destination: u32, source_extent: u32, destination_extent: u32) -> vec2<f32> {
    let denominator = i32(2u * destination_extent);
    let numerator = i32((2u * destination + 1u) * source_extent)
        - i32(destination_extent);
    var base = numerator / denominator;
    var remainder = numerator % denominator;
    if (remainder < 0) {
        base = base - 1;
        remainder = remainder + denominator;
    }
    return vec2<f32>(f32(base), f32(remainder) / f32(denominator));
}

fn clamped_index(value: i32, extent: u32) -> i32 {
    return clamp(value, 0, i32(extent) - 1);
}

fn patch_load(global_source: vec2<i32>) -> vec4<f32> {
    return textureLoad(source_patch, global_source - params.source_origin, 0);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let output_dimensions = textureDimensions(output_tile);
    if (gid.x >= output_dimensions.x || gid.y >= output_dimensions.y) {
        return;
    }

    let destination = params.destination_origin + gid.xy;

    if (params.method == 0u) {
        let nearest = vec2<i32>(
            clamped_index(nearest_index(destination.x,
                                        params.source_size.x,
                                        params.destination_size.x),
                          params.source_size.x),
            clamped_index(nearest_index(destination.y,
                                        params.source_size.y,
                                        params.destination_size.y),
                          params.source_size.y));
        textureStore(output_tile, vec2<i32>(gid.xy), patch_load(nearest));
        return;
    }

    let source_x = linear_position(destination.x,
                                   params.source_size.x,
                                   params.destination_size.x);
    let source_y = linear_position(destination.y,
                                   params.source_size.y,
                                   params.destination_size.y);
    let base_x = i32(source_x.x);
    let base_y = i32(source_y.x);
    let x0 = clamped_index(base_x, params.source_size.x);
    let x1 = clamped_index(base_x + 1, params.source_size.x);
    let y0 = clamped_index(base_y, params.source_size.y);
    let y1 = clamped_index(base_y + 1, params.source_size.y);
    let fraction = vec2<f32>(source_x.y, source_y.y);
    let top = mix(patch_load(vec2<i32>(x0, y0)),
                  patch_load(vec2<i32>(x1, y0)), fraction.x);
    let bottom = mix(patch_load(vec2<i32>(x0, y1)),
                     patch_load(vec2<i32>(x1, y1)), fraction.x);
    textureStore(output_tile, vec2<i32>(gid.xy), mix(top, bottom, fraction.y));
}
