// Selection-aware straight-RGBA raster Gradient kernel. Geometry is expressed
// in target-local pixels so adjacent 256x256 tiles evaluate exactly the same
// continuous gradient. Stored pixels remain straight RGBA; feathered raster
// coverage blends in premultiplied space to avoid transparent dark fringes.
struct GradientParams {
    start_colour: vec4<f32>,
    end_colour: vec4<f32>,
    geometry: vec4<f32>,
    tile_origin: vec2<f32>,
    target_mode: u32,
    component_index: i32,
    gradient_type: u32,
    reverse: u32,
    _padding: vec2<u32>,
};

@group(0) @binding(0) var source_tile: texture_2d<f32>;
@group(0) @binding(1) var coverage_tile: texture_2d<f32>;
@group(0) @binding(2) var output_tile: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var<uniform> params: GradientParams;

fn quantise(value: f32) -> f32 {
    return floor(clamp(value, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
}

fn quantise4(value: vec4<f32>) -> vec4<f32> {
    return vec4<f32>(quantise(value.r), quantise(value.g),
                     quantise(value.b), quantise(value.a));
}

fn gradient_amount(point: vec2<f32>) -> f32 {
    let start = params.geometry.xy;
    let finish = params.geometry.zw;
    let direction = finish - start;
    let length_squared = dot(direction, direction);
    let length_value = sqrt(max(length_squared, 0.0));
    var amount = 1.0;
    if (length_value > 0.000000001) {
        let relative = point - start;
        if (params.gradient_type == 0u) {
            amount = dot(relative, direction) / length_squared;
        } else if (params.gradient_type == 1u) {
            amount = length(relative) / length_value;
        } else if (params.gradient_type == 2u) {
            let two_pi = 6.28318530717958647692;
            let base = atan2(direction.y, direction.x);
            let angle = atan2(relative.y, relative.x);
            amount = (angle - base) / two_pi;
            amount = amount - floor(amount);
        } else if (params.gradient_type == 3u) {
            amount = abs(dot(relative, direction) / length_squared);
        } else {
            let axis = direction / length_value;
            let perpendicular = vec2<f32>(-axis.y, axis.x);
            amount = (abs(dot(relative, axis))
                      + abs(dot(relative, perpendicular))) / length_value;
        }
    }
    amount = clamp(amount, 0.0, 1.0);
    if (params.reverse != 0u) {
        amount = 1.0 - amount;
    }
    return amount;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_tile);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
        return;
    }
    let local = vec2<i32>(gid.xy);
    let source_value = textureLoad(source_tile, local, 0);
    let coverage = clamp(textureLoad(coverage_tile, local, 0).r, 0.0, 1.0);
    let point = params.tile_origin + vec2<f32>(f32(gid.x) + 0.5,
                                               f32(gid.y) + 0.5);
    let gradient_colour = quantise4(mix(params.start_colour,
                                        params.end_colour,
                                        gradient_amount(point)));
    var edited = source_value;

    if (params.target_mode == 0u) {
        let mathematical_alpha = mix(source_value.a, gradient_colour.a, coverage);
        let stored_alpha = quantise(mathematical_alpha);
        var output_rgb = source_value.rgb;
        if (stored_alpha > 0.0 && mathematical_alpha > 0.000000001) {
            output_rgb = (source_value.rgb * source_value.a * (1.0 - coverage)
                          + gradient_colour.rgb * gradient_colour.a * coverage)
                / mathematical_alpha;
        }
        edited = vec4<f32>(output_rgb, stored_alpha);
    } else {
        let scalar = dot(gradient_colour.rgb,
                         vec3<f32>(0.299, 0.587, 0.114));
        if (params.target_mode == 1u) {
            let output_rgb = mix(source_value.rgb, vec3<f32>(scalar), coverage);
            edited = vec4<f32>(output_rgb, source_value.a);
        } else if (params.target_mode == 2u) {
            if (params.component_index == 0) {
                edited.r = mix(source_value.r, scalar, coverage);
            } else if (params.component_index == 1) {
                edited.g = mix(source_value.g, scalar, coverage);
            } else if (params.component_index == 2) {
                edited.b = mix(source_value.b, scalar, coverage);
            } else if (params.component_index == 3) {
                edited.a = mix(source_value.a, scalar, coverage);
            }
        } else {
            let value = mix(source_value.r, scalar, coverage);
            edited = vec4<f32>(value, value, value, 1.0);
        }
    }
    textureStore(output_tile, local, quantise4(edited));
}
