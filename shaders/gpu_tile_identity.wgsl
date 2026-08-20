// 0.3.0a diagnostic shader. This is intentionally isolated from document
// rendering: it proves RGBA8 texture upload, compute dispatch and readback.
@group(0) @binding(0) var source_tile: texture_2d<f32>;
@group(0) @binding(1) var destination_tile: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(source_tile);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
        return;
    }
    textureStore(destination_tile,
                 vec2<i32>(gid.xy),
                 textureLoad(source_tile, vec2<i32>(gid.xy), 0));
}
