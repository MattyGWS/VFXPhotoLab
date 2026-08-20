// Presentation-only display transform. The CPU colour-management chain bakes
// a 65^3 half-float lattice; this kernel performs deterministic trilinear
// evaluation and preserves source alpha. A second lattice optionally provides
// the proof round trip used by the gamut-warning overlay.
struct DisplayTransformParams {
    edge_size: u32,
    gamut_warning: u32,
    gamut_threshold: f32,
    _padding: f32,
};

@group(0) @binding(0) var input_texture: texture_2d<f32>;
@group(0) @binding(1) var forward_lut: texture_2d<f32>;
@group(0) @binding(2) var round_trip_lut: texture_2d<f32>;
@group(0) @binding(3) var output_rgba8: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(4) var output_rgba16: texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var<uniform> params: DisplayTransformParams;

fn forward_value(position: vec3<u32>) -> vec3<f32> {
    let edge = params.edge_size;
    let coordinate = vec2<i32>(
        i32(position.x + position.z * edge),
        i32(position.y));
    return textureLoad(forward_lut, coordinate, 0).rgb;
}

fn round_trip_value(position: vec3<u32>) -> vec3<f32> {
    let edge = params.edge_size;
    let coordinate = vec2<i32>(
        i32(position.x + position.z * edge),
        i32(position.y));
    return textureLoad(round_trip_lut, coordinate, 0).rgb;
}

fn sample_forward(value: vec3<f32>) -> vec3<f32> {
    let highest = params.edge_size - 1u;
    let scaled = clamp(value, vec3<f32>(0.0), vec3<f32>(1.0)) * f32(highest);
    let lower = vec3<u32>(floor(scaled));
    let upper = min(lower + vec3<u32>(1u), vec3<u32>(highest));
    let amount = fract(scaled);

    let c000 = forward_value(vec3<u32>(lower.x, lower.y, lower.z));
    let c100 = forward_value(vec3<u32>(upper.x, lower.y, lower.z));
    let c010 = forward_value(vec3<u32>(lower.x, upper.y, lower.z));
    let c110 = forward_value(vec3<u32>(upper.x, upper.y, lower.z));
    let c001 = forward_value(vec3<u32>(lower.x, lower.y, upper.z));
    let c101 = forward_value(vec3<u32>(upper.x, lower.y, upper.z));
    let c011 = forward_value(vec3<u32>(lower.x, upper.y, upper.z));
    let c111 = forward_value(vec3<u32>(upper.x, upper.y, upper.z));

    let z0 = mix(mix(c000, c100, amount.x),
                 mix(c010, c110, amount.x), amount.y);
    let z1 = mix(mix(c001, c101, amount.x),
                 mix(c011, c111, amount.x), amount.y);
    return mix(z0, z1, amount.z);
}

fn sample_round_trip(value: vec3<f32>) -> vec3<f32> {
    let highest = params.edge_size - 1u;
    let scaled = clamp(value, vec3<f32>(0.0), vec3<f32>(1.0)) * f32(highest);
    let lower = vec3<u32>(floor(scaled));
    let upper = min(lower + vec3<u32>(1u), vec3<u32>(highest));
    let amount = fract(scaled);

    let c000 = round_trip_value(vec3<u32>(lower.x, lower.y, lower.z));
    let c100 = round_trip_value(vec3<u32>(upper.x, lower.y, lower.z));
    let c010 = round_trip_value(vec3<u32>(lower.x, upper.y, lower.z));
    let c110 = round_trip_value(vec3<u32>(upper.x, upper.y, lower.z));
    let c001 = round_trip_value(vec3<u32>(lower.x, lower.y, upper.z));
    let c101 = round_trip_value(vec3<u32>(upper.x, lower.y, upper.z));
    let c011 = round_trip_value(vec3<u32>(lower.x, upper.y, upper.z));
    let c111 = round_trip_value(vec3<u32>(upper.x, upper.y, upper.z));

    let z0 = mix(mix(c000, c100, amount.x),
                 mix(c010, c110, amount.x), amount.y);
    let z1 = mix(mix(c001, c101, amount.x),
                 mix(c011, c111, amount.x), amount.y);
    return mix(z0, z1, amount.z);
}

fn transformed_rgb(input_rgb: vec3<f32>) -> vec3<f32> {
    var mapped = sample_forward(input_rgb);
    if (params.gamut_warning != 0u) {
        let round_trip = sample_round_trip(input_rgb);
        // The CPU reference compares RGBA8 code values even for a 16-bit
        // presentation surface. Mirror that rule before applying the warning.
        let input_code = floor(clamp(input_rgb, vec3<f32>(0.0),
                                     vec3<f32>(1.0)) * 255.0 + 0.5);
        let round_trip_code = floor(clamp(round_trip, vec3<f32>(0.0),
                                          vec3<f32>(1.0)) * 255.0 + 0.5);
        let difference = max(abs(input_code.r - round_trip_code.r),
                             max(abs(input_code.g - round_trip_code.g),
                                 abs(input_code.b - round_trip_code.b)));
        if (difference > params.gamut_threshold) {
            mapped = vec3<f32>(1.0, 0.0, 1.0);
        }
    }
    return clamp(mapped, vec3<f32>(0.0), vec3<f32>(1.0));
}

@compute @workgroup_size(8, 8)
fn apply_rgba8(@builtin(global_invocation_id) invocation: vec3<u32>) {
    let dimensions = textureDimensions(output_rgba8);
    if (invocation.x >= dimensions.x || invocation.y >= dimensions.y) {
        return;
    }
    let coordinate = vec2<i32>(i32(invocation.x), i32(invocation.y));
    let input_value = textureLoad(input_texture, coordinate, 0);
    textureStore(output_rgba8, coordinate,
                 vec4<f32>(transformed_rgb(input_value.rgb), input_value.a));
}

@compute @workgroup_size(8, 8)
fn apply_rgba16(@builtin(global_invocation_id) invocation: vec3<u32>) {
    let dimensions = textureDimensions(output_rgba16);
    if (invocation.x >= dimensions.x || invocation.y >= dimensions.y) {
        return;
    }
    let coordinate = vec2<i32>(i32(invocation.x), i32(invocation.y));
    let input_value = textureLoad(input_texture, coordinate, 0);
    textureStore(output_rgba16, coordinate,
                 vec4<f32>(transformed_rgb(input_value.rgb), input_value.a));
}
