// Presentation-only projective transform pass. The inverse matrix uses
// QTransform's row-vector mapping:
// x' = (m11*x + m21*y + m31) / (m13*x + m23*y + m33)
// y' = (m12*x + m22*y + m32) / (m13*x + m23*y + m33)
// The document model is still committed once when Apply is chosen.
struct TransformParams {
    inverse_x: vec4<f32>, // m11, m21, m31, unused
    inverse_y: vec4<f32>, // m12, m22, m32, unused
    inverse_w: vec4<f32>, // m13, m23, m33, unused
    output_size: vec2<u32>,
    foreground_size: vec2<u32>,
};

@group(0) @binding(0) var background_texture: texture_2d<f32>;
@group(0) @binding(1) var foreground_texture: texture_2d<f32>;
@group(0) @binding(2) var output_texture: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var<uniform> params: TransformParams;

fn map_to_source(point: vec2<f32>) -> vec2<f32> {
    let denominator = dot(params.inverse_w.xyz, vec3<f32>(point, 1.0));
    if (abs(denominator) < 1.0e-7) {
        return vec2<f32>(-1.0e20);
    }
    return vec2<f32>(
        dot(params.inverse_x.xyz, vec3<f32>(point, 1.0)) / denominator,
        dot(params.inverse_y.xyz, vec3<f32>(point, 1.0)) / denominator
    );
}

fn load_foreground(position: vec2<i32>) -> vec4<f32> {
    let dimensions = vec2<i32>(params.foreground_size);
    if (any(position < vec2<i32>(0)) || any(position >= dimensions)) {
        return vec4<f32>(0.0);
    }
    return textureLoad(foreground_texture, position, 0);
}

fn bilinear_foreground(position: vec2<f32>) -> vec4<f32> {
    let base = vec2<i32>(floor(position));
    let fraction = fract(position);
    let top = mix(load_foreground(base),
                  load_foreground(base + vec2<i32>(1, 0)),
                  fraction.x);
    let bottom = mix(load_foreground(base + vec2<i32>(0, 1)),
                     load_foreground(base + vec2<i32>(1, 1)),
                     fraction.x);
    return mix(top, bottom, fraction.y);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= params.output_size.x || gid.y >= params.output_size.y) {
        return;
    }

    let output_position = vec2<i32>(gid.xy);
    let background = textureLoad(background_texture, output_position, 0);
    let source_position = map_to_source(vec2<f32>(gid.xy) + vec2<f32>(0.5))
        - vec2<f32>(0.5);
    let foreground = bilinear_foreground(source_position);
    let result = foreground + background * (1.0 - foreground.a);
    textureStore(output_texture, output_position, result);
}
