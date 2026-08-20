// Coverage-driven straight-RGBA Fill kernel. Region matching and contiguous
// discovery are deterministic CPU work; this native path accelerates bounded
// dirty-tile application while preserving hidden RGB and exact channel/mask
// semantics. Partial raster coverage blends in premultiplied space while
// the stored output remains straight RGBA.
struct FillParams {
    colour: vec4<f32>,
    target_mode: u32,
    component_index: i32,
    preserve_transparency: u32,
    _padding: u32,
};

@group(0) @binding(0) var source_tile: texture_2d<f32>;
@group(0) @binding(1) var coverage_tile: texture_2d<f32>;
@group(0) @binding(2) var output_tile: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var<uniform> params: FillParams;

fn quantise(value: f32) -> f32 {
    return floor(clamp(value, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
}

fn quantise4(value: vec4<f32>) -> vec4<f32> {
    return vec4<f32>(quantise(value.r), quantise(value.g),
                     quantise(value.b), quantise(value.a));
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_tile);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
        return;
    }
    let local = vec2<i32>(gid.xy);
    let source_value = textureLoad(source_tile, local, 0);
    let amount = clamp(textureLoad(coverage_tile, local, 0).r, 0.0, 1.0);
    var edited = source_value;

    if (params.target_mode == 0u) {
        if (params.preserve_transparency != 0u) {
            let output_rgb = mix(source_value.rgb, params.colour.rgb, amount);
            edited = vec4<f32>(output_rgb, source_value.a);
        } else {
            let output_alpha = mix(source_value.a, params.colour.a, amount);
            var output_rgb = source_value.rgb;
            if (output_alpha > 0.000000001) {
                output_rgb = (source_value.rgb * source_value.a * (1.0 - amount)
                              + params.colour.rgb * params.colour.a * amount)
                    / output_alpha;
            }
            edited = vec4<f32>(output_rgb, output_alpha);
        }
    } else if (params.target_mode == 1u) {
        let output_rgb = mix(source_value.rgb, vec3<f32>(params.colour.r), amount);
        edited = vec4<f32>(output_rgb, source_value.a);
    } else if (params.target_mode == 2u) {
        if (params.component_index == 0) {
            edited.r = mix(source_value.r, params.colour.r, amount);
        } else if (params.component_index == 1) {
            edited.g = mix(source_value.g, params.colour.r, amount);
        } else if (params.component_index == 2) {
            edited.b = mix(source_value.b, params.colour.r, amount);
        } else if (params.component_index == 3) {
            edited.a = mix(source_value.a, params.colour.r, amount);
        }
    } else {
        let value = mix(source_value.r, params.colour.r, amount);
        edited = vec4<f32>(value, value, value, 1.0);
    }
    textureStore(output_tile, local, quantise4(edited));
}
