struct PassThroughParams {
    opacity: f32,
    use_mask: u32,
    _padding0: u32,
    _padding1: u32,
};

@group(0) @binding(0) var before_texture: texture_2d<f32>;
@group(0) @binding(1) var after_texture: texture_2d<f32>;
@group(0) @binding(2) var mask_texture: texture_2d<f32>;
@group(0) @binding(3) var output_texture: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(4) var<uniform> params: PassThroughParams;

fn quantize8(value: vec4<f32>) -> vec4<f32> {
    return floor(clamp(value, vec4<f32>(0.0), vec4<f32>(1.0)) * 255.0
                 + vec4<f32>(0.5)) / 255.0;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_texture);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
        return;
    }

    let position = vec2<i32>(gid.xy);
    let before = textureLoad(before_texture, position, 0);
    let after = textureLoad(after_texture, position, 0);
    let mask = select(1.0,
                      textureLoad(mask_texture, position, 0).r,
                      params.use_mask != 0u);
    let weight = clamp(params.opacity * mask, 0.0, 1.0);

    let before_premultiplied = quantize8(vec4<f32>(before.rgb * before.a, before.a));
    let after_premultiplied = quantize8(vec4<f32>(after.rgb * after.a, after.a));
    let mixed = quantize8(before_premultiplied
                          + (after_premultiplied - before_premultiplied) * weight);
    let output_rgb = select(vec3<f32>(0.0),
                            mixed.rgb / max(mixed.a, 0.000001),
                            mixed.a > 0.0);
    textureStore(output_texture, position, vec4<f32>(output_rgb, mixed.a));
}
