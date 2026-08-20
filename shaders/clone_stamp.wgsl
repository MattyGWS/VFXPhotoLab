// Native dirty-tile Clone Stamp kernel. Source and destination are immutable
// straight-RGBA snapshots for the dispatch. Manual bilinear filtering keeps
// transparent neighbouring texels from contaminating visible edge colour.
struct CloneParams {
    destination_tile_origin: vec2<f32>,
    source_patch_origin: vec2<f32>,
    source_image_size: vec2<f32>,
    source_offset_document: vec2<f32>,
    radius: f32,
    hardness: f32,
    opacity: f32,
    target_mode: u32,
    sample_mode: u32,
    component_index: i32,
    point_count: u32,
    source_is_grey: u32,
    target_pixel_to_layer_0: vec4<f32>,
    target_pixel_to_layer_1: vec4<f32>,
    target_layer_to_document_0: vec4<f32>,
    target_layer_to_document_1: vec4<f32>,
    source_document_to_layer_0: vec4<f32>,
    source_document_to_layer_1: vec4<f32>,
    source_layer_to_pixel_0: vec4<f32>,
    source_layer_to_pixel_1: vec4<f32>,
};

struct SourceSample {
    rgba: vec4<f32>,
    valid: u32,
};

@group(0) @binding(0) var destination_tile: texture_2d<f32>;
@group(0) @binding(1) var source_patch: texture_2d<f32>;
@group(0) @binding(2) var output_tile: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var<storage, read> points: array<vec2<f32>>;
@group(0) @binding(4) var<uniform> params: CloneParams;
@group(0) @binding(5) var selection_tile: texture_2d<f32>;

fn affine(point: vec2<f32>, row0: vec4<f32>, row1: vec4<f32>) -> vec2<f32> {
    let homogeneous = vec3<f32>(point, 1.0);
    return vec2<f32>(dot(homogeneous, row0.xyz), dot(homogeneous, row1.xyz));
}

fn quantise_unorm8(value: vec4<f32>) -> vec4<f32> {
    return floor(clamp(value, vec4<f32>(0.0), vec4<f32>(1.0)) * 255.0 + 0.5) / 255.0;
}

fn sample_source(full_position: vec2<f32>) -> SourceSample {
    if (full_position.x < 0.0 || full_position.y < 0.0
        || full_position.x > params.source_image_size.x - 1.0
        || full_position.y > params.source_image_size.y - 1.0) {
        return SourceSample(vec4<f32>(0.0), 0u);
    }

    let lower = vec2<i32>(floor(full_position));
    let upper = min(lower + vec2<i32>(1), vec2<i32>(params.source_image_size) - vec2<i32>(1));
    let fraction = clamp(full_position - vec2<f32>(lower), vec2<f32>(0.0), vec2<f32>(1.0));
    let patch_origin = vec2<i32>(params.source_patch_origin);
    let patch_dimensions = vec2<i32>(textureDimensions(source_patch));
    let p00_position = vec2<i32>(lower.x, lower.y) - patch_origin;
    let p10_position = vec2<i32>(upper.x, lower.y) - patch_origin;
    let p01_position = vec2<i32>(lower.x, upper.y) - patch_origin;
    let p11_position = vec2<i32>(upper.x, upper.y) - patch_origin;
    if (any(p00_position < vec2<i32>(0)) || any(p11_position >= patch_dimensions)
        || any(p10_position < vec2<i32>(0)) || any(p10_position >= patch_dimensions)
        || any(p01_position < vec2<i32>(0)) || any(p01_position >= patch_dimensions)) {
        return SourceSample(vec4<f32>(0.0), 0u);
    }

    let p00 = textureLoad(source_patch, p00_position, 0);
    let p10 = textureLoad(source_patch, p10_position, 0);
    let p01 = textureLoad(source_patch, p01_position, 0);
    let p11 = textureLoad(source_patch, p11_position, 0);
    let weights = vec4<f32>((1.0 - fraction.x) * (1.0 - fraction.y),
                            fraction.x * (1.0 - fraction.y),
                            (1.0 - fraction.x) * fraction.y,
                            fraction.x * fraction.y);

    if (params.source_is_grey != 0u) {
        return SourceSample(p00 * weights.x + p10 * weights.y
                            + p01 * weights.z + p11 * weights.w, 1u);
    }

    let alpha = dot(vec4<f32>(p00.a, p10.a, p01.a, p11.a), weights);
    var rgb = p00.rgb * weights.x + p10.rgb * weights.y
        + p01.rgb * weights.z + p11.rgb * weights.w;
    if (alpha > 1.0e-12) {
        rgb = (p00.rgb * p00.a * weights.x + p10.rgb * p10.a * weights.y
               + p01.rgb * p01.a * weights.z + p11.rgb * p11.a * weights.w) / alpha;
    }
    return SourceSample(vec4<f32>(clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0)),
                                  clamp(alpha, 0.0, 1.0)), 1u);
}

fn scalar_sample(sampled: vec4<f32>) -> f32 {
    if (params.sample_mode == 1u) {
        return clamp(dot(sampled.rgb, vec3<f32>(0.299, 0.587, 0.114)), 0.0, 1.0);
    }
    if (params.sample_mode == 2u) {
        return clamp(sampled.a, 0.0, 1.0);
    }
    if (params.sample_mode == 3u) {
        if (params.component_index == 0) { return clamp(sampled.r, 0.0, 1.0); }
        if (params.component_index == 1) { return clamp(sampled.g, 0.0, 1.0); }
        if (params.component_index == 2) { return clamp(sampled.b, 0.0, 1.0); }
        if (params.component_index == 3) { return clamp(sampled.a, 0.0, 1.0); }
        return 0.0;
    }
    return clamp(sampled.r, 0.0, 1.0);
}

fn blend_straight(destination: vec4<f32>, sampled: vec4<f32>, amount: f32) -> vec4<f32> {
    let inverse_amount = 1.0 - amount;
    let output_alpha = sampled.a * amount + destination.a * inverse_amount;
    var output_rgb = sampled.rgb * amount + destination.rgb * inverse_amount;
    if (output_alpha > 1.0e-12) {
        output_rgb = (sampled.rgb * sampled.a * amount
                      + destination.rgb * destination.a * inverse_amount) / output_alpha;
    }
    return vec4<f32>(clamp(output_rgb, vec3<f32>(0.0), vec3<f32>(1.0)),
                     clamp(output_alpha, 0.0, 1.0));
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_tile);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
        return;
    }

    let local = vec2<i32>(gid.xy);
    let target_pixel = params.destination_tile_origin + vec2<f32>(gid.xy) + vec2<f32>(0.5);
    let original = textureLoad(destination_tile, local, 0);
    var edited = original;

    let target_layer = affine(target_pixel,
                              params.target_pixel_to_layer_0,
                              params.target_pixel_to_layer_1);
    let destination_document = affine(target_layer,
                                      params.target_layer_to_document_0,
                                      params.target_layer_to_document_1);
    let source_document = destination_document + params.source_offset_document;
    let source_layer = affine(source_document,
                              params.source_document_to_layer_0,
                              params.source_document_to_layer_1);
    let source_pixel = affine(source_layer,
                              params.source_layer_to_pixel_0,
                              params.source_layer_to_pixel_1) - vec2<f32>(0.5);
    let sampled = sample_source(source_pixel);

    if (sampled.valid != 0u) {
        // A Clone source is fixed for this target pixel throughout the stroke.
        // Combine every overlapping dab into one floating-point coverage value
        // and quantise the completed edit only once. Quantising each
        // very soft dab independently creates channel-specific contour bands.
        var remaining_coverage = 1.0;
        for (var index: u32 = 0u; index < params.point_count; index = index + 1u) {
            let normalised_distance = distance(target_pixel, points[index]) / max(params.radius, 0.5);
            let coverage = 1.0 - smoothstep(min(params.hardness, 0.9999), 1.0, normalised_distance);
            let dab_amount = clamp(coverage * params.opacity, 0.0, 1.0);
            remaining_coverage = remaining_coverage * (1.0 - dab_amount);
        }
        let amount = clamp(1.0 - remaining_coverage, 0.0, 1.0);

        if (amount > 0.0) {
            if (params.target_mode == 0u) {
                edited = blend_straight(original, sampled.rgba, amount);
            } else {
                let value = scalar_sample(sampled.rgba);
                if (params.target_mode == 1u) {
                    edited = vec4<f32>(mix(original.rgb, vec3<f32>(value), amount), original.a);
                } else if (params.target_mode == 2u) {
                    if (params.component_index == 0) { edited.r = mix(original.r, value, amount); }
                    if (params.component_index == 1) { edited.g = mix(original.g, value, amount); }
                    if (params.component_index == 2) { edited.b = mix(original.b, value, amount); }
                    if (params.component_index == 3) { edited.a = mix(original.a, value, amount); }
                } else {
                    let scalar = mix(original.r, value, amount);
                    edited = vec4<f32>(scalar, scalar, scalar, 1.0);
                }
            }
            // Match the authoritative QImage result before selection clipping,
            // but quantise only once for the completed stroke rather than once
            // per overlapping dab.
            edited = quantise_unorm8(edited);
        }
    }

    let selection_amount = clamp(textureLoad(selection_tile, local, 0).r, 0.0, 1.0);
    var output = mix(original, edited, selection_amount);
    if (params.target_mode == 0u) {
        output = blend_straight(original, edited, selection_amount);
    }
    textureStore(output_tile, local, output);
}
