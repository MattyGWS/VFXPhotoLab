// Exact separable vector Feather coverage kernel.
// Semantic geometry and authored colour remain outside this filter. The first
// entry point convolves combined fill/stroke coverage horizontally; the second
// convolves vertically and applies the exact CPU-prepared nearest-colour carrier.

struct FloatBuffer {
    values: array<f32>,
};

struct FeatherParams {
    source_size: vec2<u32>,
    output_size: vec2<u32>,
    source_origin: vec2<i32>,
    output_origin: vec2<i32>,
    support_x: u32,
    support_y: u32,
    padding: vec2<u32>,
};

@group(0) @binding(0) var coverage_texture: texture_2d<f32>;
@group(0) @binding(1) var<storage, read> kernel_x: FloatBuffer;
@group(0) @binding(2) var<storage, read_write> horizontal_values: FloatBuffer;
@group(0) @binding(3) var<uniform> params: FeatherParams;

@group(0) @binding(4) var<storage, read> horizontal_input: FloatBuffer;
@group(0) @binding(5) var carrier_texture: texture_2d<f32>;
@group(0) @binding(6) var<storage, read> kernel_y: FloatBuffer;
@group(0) @binding(7) var output_texture: texture_storage_2d<rgba8unorm, write>;

fn quantise4(value: vec4<f32>) -> vec4<f32> {
    return floor(clamp(value, vec4<f32>(0.0), vec4<f32>(1.0)) * 255.0
                 + vec4<f32>(0.5)) / 255.0;
}

@compute @workgroup_size(8, 8, 1)
fn horizontal_main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= params.output_size.x || gid.y >= params.source_size.y) {
        return;
    }
    let global_x = params.output_origin.x + i32(gid.x);
    let centre_x = global_x - params.source_origin.x;
    let support = i32(params.support_x);
    var sum = 0.0;
    var offset = -support;
    loop {
        if (offset > support) {
            break;
        }
        let source_x = centre_x + offset;
        if (source_x >= 0 && source_x < i32(params.source_size.x)) {
            let weight_index = u32(offset + support);
            let coverage = textureLoad(
                coverage_texture, vec2<i32>(source_x, i32(gid.y)), 0).a;
            sum += coverage * kernel_x.values[weight_index];
        }
        offset += 1;
    }
    let index = gid.y * params.output_size.x + gid.x;
    horizontal_values.values[index] = clamp(sum, 0.0, 1.0);
}

@compute @workgroup_size(8, 8, 1)
fn vertical_main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= params.output_size.x || gid.y >= params.output_size.y) {
        return;
    }
    let global_y = params.output_origin.y + i32(gid.y);
    let centre_y = global_y - params.source_origin.y;
    let support = i32(params.support_y);
    var sum = 0.0;
    var offset = -support;
    loop {
        if (offset > support) {
            break;
        }
        let source_y = centre_y + offset;
        if (source_y >= 0 && source_y < i32(params.source_size.y)) {
            let weight_index = u32(offset + support);
            let horizontal_index = u32(source_y) * params.output_size.x + gid.x;
            sum += horizontal_input.values[horizontal_index]
                * kernel_y.values[weight_index];
        }
        offset += 1;
    }
    let local = vec2<i32>(i32(gid.x), i32(gid.y));
    let carrier = textureLoad(carrier_texture, local, 0);
    textureStore(output_texture, local,
                 quantise4(vec4<f32>(carrier.rgb,
                                     clamp(sum, 0.0, 1.0) * carrier.a)));
}
