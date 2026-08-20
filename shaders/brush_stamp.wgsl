// Canonical dirty-tile brush kernel. A whole discrete stamp sequence is applied
// to one immutable straight-RGBA source tile in a single dispatch. Selection
// coverage is applied once to the completed stroke so CPU/GPU paths agree even
// where multiple stamps overlap. Erasing always preserves hidden RGB.
struct BrushParams {
    tile_origin: vec2<f32>,
    radius: f32,
    hardness: f32,
    opacity: f32,
    erasing: u32,
    point_count: u32,
    _padding: u32,
    colour: vec4<f32>,
};

@group(0) @binding(0) var source_tile: texture_2d<f32>;
@group(0) @binding(1) var output_tile: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<storage, read> points: array<vec2<f32>>;
@group(0) @binding(3) var<uniform> params: BrushParams;
@group(0) @binding(4) var selection_tile: texture_2d<f32>;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_tile);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
        return;
    }

    let local = vec2<i32>(gid.xy);
    let layer_point = params.tile_origin + vec2<f32>(gid.xy) + vec2<f32>(0.5);
    let source = textureLoad(source_tile, local, 0);
    var edited = source;
    for (var index: u32 = 0u; index < params.point_count; index = index + 1u) {
        let normalised_distance = distance(layer_point, points[index]) / max(params.radius, 0.5);
        let coverage = 1.0 - smoothstep(params.hardness, 1.0, normalised_distance);
        let amount = clamp(coverage * params.opacity * params.colour.a, 0.0, 1.0);
        if (params.erasing != 0u) {
            edited = vec4<f32>(edited.rgb, edited.a * (1.0 - amount));
        } else {
            let output_alpha = amount + edited.a * (1.0 - amount);
            var output_rgb = edited.rgb;
            if (output_alpha > 0.000001) {
                output_rgb = (params.colour.rgb * amount
                    + edited.rgb * edited.a * (1.0 - amount)) / output_alpha;
            }
            edited = vec4<f32>(output_rgb, output_alpha);
        }
    }

    let selection_amount = clamp(textureLoad(selection_tile, local, 0).r, 0.0, 1.0);
    let output_alpha = source.a + (edited.a - source.a) * selection_amount;
    var output_rgb = source.rgb;
    if (output_alpha > 0.000001) {
        let output_premultiplied = source.rgb * source.a * (1.0 - selection_amount)
            + edited.rgb * edited.a * selection_amount;
        output_rgb = output_premultiplied / output_alpha;
    }
    textureStore(output_tile, local, vec4<f32>(output_rgb, output_alpha));
}
