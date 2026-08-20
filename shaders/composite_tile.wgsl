// Shared layer compositor for live brush tiles and settled preview tiles.
// Blend mode values are deliberately serialised by the host rather than tied
// to UI enum ordinals.
struct CompositeParams {
    opacity: f32,
    blend_mode: u32,
    use_mask: u32,
    _padding: u32,
};

@group(0) @binding(0) var base_texture: texture_2d<f32>;
@group(0) @binding(1) var layer_texture: texture_2d<f32>;
@group(0) @binding(2) var mask_texture: texture_2d<f32>;
@group(0) @binding(3) var output_texture: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(4) var<uniform> params: CompositeParams;

fn blend_channel(base: f32, effect: f32, mode: u32) -> f32 {
    switch (mode) {
        case 1u: { return base * effect; } // multiply
        case 2u: { return 1.0 - (1.0 - base) * (1.0 - effect); } // screen
        case 3u: { // overlay
            return select(2.0 * base * effect,
                          1.0 - 2.0 * (1.0 - base) * (1.0 - effect),
                          base > 0.5);
        }
        case 4u: { return min(base, effect); }
        case 5u: { return max(base, effect); }
        case 6u: { return select(min(1.0, base / max(1.0 - effect, 0.00001)), 1.0, effect >= 1.0); }
        case 7u: { return select(1.0 - min(1.0, (1.0 - base) / max(effect, 0.00001)), 0.0, effect <= 0.0); }
        case 8u: { return min(1.0, base + effect); }
        case 9u: { return max(0.0, base - effect); }
        case 10u: { return abs(base - effect); }
        case 11u: { return base + effect - 2.0 * base * effect; }
        default: { return effect; } // copy / replace
    }
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let size = textureDimensions(output_texture);
    if (gid.x >= size.x || gid.y >= size.y) {
        return;
    }
    let position = vec2<i32>(gid.xy);
    let base = textureLoad(base_texture, position, 0);
    let layer = textureLoad(layer_texture, position, 0);
    let mask = select(1.0,
                      textureLoad(mask_texture, position, 0).r,
                      params.use_mask != 0u);
    let source_alpha = clamp(layer.a * params.opacity * mask, 0.0, 1.0);
    let backdrop_alpha = clamp(base.a, 0.0, 1.0);
    let blended = vec3<f32>(blend_channel(base.r, layer.r, params.blend_mode),
                            blend_channel(base.g, layer.g, params.blend_mode),
                            blend_channel(base.b, layer.b, params.blend_mode));
    let output_alpha = source_alpha + backdrop_alpha * (1.0 - source_alpha);
    let output_premultiplied = (1.0 - source_alpha) * base.rgb * backdrop_alpha
        + (1.0 - backdrop_alpha) * layer.rgb * source_alpha
        + backdrop_alpha * source_alpha * blended;
    let output_rgb = select(vec3<f32>(0.0),
                            output_premultiplied / max(output_alpha, 0.000001),
                            output_alpha > 0.0);
    textureStore(output_texture, position, vec4<f32>(output_rgb, output_alpha));
}
