// Non-destructive 8-bit adjustment-layer pass used by the tiled WebGPU
// hierarchy compositor. IDs: 0 Exposure, 1 Contrast, 2 Saturation,
// 3 Levels, 4 Curves, 5 Hue/Saturation, 6 Vibrance, 7 White Balance,
// 8 Colour Balance, 9 Channel Mixer, 10 Black and White, 11 Gradient Map,
// 12 Posterise, 13 Threshold, 14 LUT, 15 Shadows/Highlights, 20 Invert,
// 21 Photo Filter and 22 Selective Colour. IDs 16-19 are spatial CPU-reference
// filters. Levels, Curves,
// Gradient Map and imported LUTs share one lookup texture binding.
struct AdjustmentParams {
    opacity: f32,
    blend_mode: u32,
    use_mask: u32,
    adjustment_type: u32,

    exposure: f32,
    exposure_offset: f32,
    exposure_gamma: f32,
    contrast: f32,

    contrast_pivot: f32,
    saturation: f32,
    managed_domain: u32,
    domain_edge_size: u32,

    hue_master: vec4<f32>,
    hue_ranges: array<vec4<f32>, 12>,
    vibrance_params: vec4<f32>,
    photo_filter_params: vec4<f32>,
    white_balance_params: vec4<f32>,
    colour_balance_ranges: array<vec4<f32>, 3>,
    colour_balance_options: vec4<f32>,
    channel_mixer_rows: array<vec4<f32>, 4>,
    channel_mixer_options: vec4<f32>,
    black_white_weights0: vec4<f32>,
    black_white_weights1: vec4<f32>,
    black_white_options: vec4<f32>,
    selective_colour_ranges: array<vec4<f32>, 9>,
    selective_colour_options: vec4<f32>,
    discrete_params: vec4<f32>,
    shadows_highlights0: vec4<f32>,
    shadows_highlights1: vec4<f32>,
    lut_options: vec4<f32>,
    lut_modes: vec4<f32>,
    lut_shaper_domain_min: vec4<f32>,
    lut_shaper_domain_max: vec4<f32>,
    lut_cube_domain_min: vec4<f32>,
    lut_cube_domain_max: vec4<f32>,
};

@group(0) @binding(0) var base_texture: texture_2d<f32>;
@group(0) @binding(1) var mask_texture: texture_2d<f32>;
@group(0) @binding(2) var tonal_lut: texture_2d<f32>;
@group(0) @binding(3) var output_texture: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(4) var<uniform> params: AdjustmentParams;
@group(0) @binding(5) var working_to_domain_lut: texture_2d<f32>;
@group(0) @binding(6) var domain_to_working_lut: texture_2d<f32>;


fn quantize_domain_rgb(value: vec3<f32>) -> vec3<f32> {
    return floor(clamp(value, vec3<f32>(0.0), vec3<f32>(1.0)) * 255.0
                 + vec3<f32>(0.5)) / 255.0;
}

fn domain_lut_coordinate(position: vec3<u32>) -> vec2<i32> {
    let edge = params.domain_edge_size;
    return vec2<i32>(i32(position.x + position.z * edge), i32(position.y));
}

fn working_to_domain_value(position: vec3<u32>) -> vec3<f32> {
    return textureLoad(working_to_domain_lut, domain_lut_coordinate(position), 0).rgb;
}

fn domain_to_working_value(position: vec3<u32>) -> vec3<f32> {
    return textureLoad(domain_to_working_lut, domain_lut_coordinate(position), 0).rgb;
}

fn sample_working_to_domain(value: vec3<f32>) -> vec3<f32> {
    let highest = params.domain_edge_size - 1u;
    let scaled = clamp(value, vec3<f32>(0.0), vec3<f32>(1.0)) * f32(highest);
    let lower = vec3<u32>(floor(scaled));
    let upper = min(lower + vec3<u32>(1u), vec3<u32>(highest));
    let amount = fract(scaled);
    let c000 = working_to_domain_value(vec3<u32>(lower.x, lower.y, lower.z));
    let c100 = working_to_domain_value(vec3<u32>(upper.x, lower.y, lower.z));
    let c010 = working_to_domain_value(vec3<u32>(lower.x, upper.y, lower.z));
    let c110 = working_to_domain_value(vec3<u32>(upper.x, upper.y, lower.z));
    let c001 = working_to_domain_value(vec3<u32>(lower.x, lower.y, upper.z));
    let c101 = working_to_domain_value(vec3<u32>(upper.x, lower.y, upper.z));
    let c011 = working_to_domain_value(vec3<u32>(lower.x, upper.y, upper.z));
    let c111 = working_to_domain_value(vec3<u32>(upper.x, upper.y, upper.z));
    let z0 = mix(mix(c000, c100, amount.x), mix(c010, c110, amount.x), amount.y);
    let z1 = mix(mix(c001, c101, amount.x), mix(c011, c111, amount.x), amount.y);
    return mix(z0, z1, amount.z);
}

fn sample_domain_to_working(value: vec3<f32>) -> vec3<f32> {
    let highest = params.domain_edge_size - 1u;
    let scaled = clamp(value, vec3<f32>(0.0), vec3<f32>(1.0)) * f32(highest);
    let lower = vec3<u32>(floor(scaled));
    let upper = min(lower + vec3<u32>(1u), vec3<u32>(highest));
    let amount = fract(scaled);
    let c000 = domain_to_working_value(vec3<u32>(lower.x, lower.y, lower.z));
    let c100 = domain_to_working_value(vec3<u32>(upper.x, lower.y, lower.z));
    let c010 = domain_to_working_value(vec3<u32>(lower.x, upper.y, lower.z));
    let c110 = domain_to_working_value(vec3<u32>(upper.x, upper.y, lower.z));
    let c001 = domain_to_working_value(vec3<u32>(lower.x, lower.y, upper.z));
    let c101 = domain_to_working_value(vec3<u32>(upper.x, lower.y, upper.z));
    let c011 = domain_to_working_value(vec3<u32>(lower.x, upper.y, upper.z));
    let c111 = domain_to_working_value(vec3<u32>(upper.x, upper.y, upper.z));
    let z0 = mix(mix(c000, c100, amount.x), mix(c010, c110, amount.x), amount.y);
    let z1 = mix(mix(c001, c101, amount.x), mix(c011, c111, amount.x), amount.y);
    return mix(z0, z1, amount.z);
}

fn to_adjustment_domain(value: vec3<f32>) -> vec3<f32> {
    if (params.managed_domain == 0u) { return value; }
    return quantize_domain_rgb(sample_working_to_domain(value));
}

fn from_adjustment_domain(value: vec3<f32>) -> vec3<f32> {
    if (params.managed_domain == 0u) { return value; }
    return quantize_domain_rgb(sample_domain_to_working(value));
}

fn srgb_to_linear(value: f32) -> f32 {
    let clamped = clamp(value, 0.0, 1.0);
    return select(pow((clamped + 0.055) / 1.055, 2.4),
                  clamped / 12.92,
                  clamped <= 0.04045);
}

fn linear_to_srgb(value: f32) -> f32 {
    let clamped = max(0.0, value);
    let converted = select(1.055 * pow(clamped, 1.0 / 2.4) - 0.055,
                           clamped * 12.92,
                           clamped <= 0.0031308);
    return clamp(converted, 0.0, 1.0);
}

fn srgb_to_linear3(value: vec3<f32>) -> vec3<f32> {
    return vec3<f32>(srgb_to_linear(value.r),
                     srgb_to_linear(value.g),
                     srgb_to_linear(value.b));
}

fn linear_to_srgb3(value: vec3<f32>) -> vec3<f32> {
    return vec3<f32>(linear_to_srgb(value.r),
                     linear_to_srgb(value.g),
                     linear_to_srgb(value.b));
}

// LUT processing uses the extended sRGB transfer contract from the CPU
// reference. Negative values remain linear in the toe instead of being
// prematurely clipped by the display-oriented helpers above.
fn lut_srgb_to_linear(value: f32) -> f32 {
    if (value <= 0.04045) { return value / 12.92; }
    return pow((value + 0.055) / 1.055, 2.4);
}

fn lut_linear_to_srgb(value: f32) -> f32 {
    if (value <= 0.0031308) { return value * 12.92; }
    return 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

fn lut_srgb_to_linear3(value: vec3<f32>) -> vec3<f32> {
    return vec3<f32>(lut_srgb_to_linear(value.r),
                     lut_srgb_to_linear(value.g),
                     lut_srgb_to_linear(value.b));
}

fn lut_linear_to_srgb3(value: vec3<f32>) -> vec3<f32> {
    return vec3<f32>(lut_linear_to_srgb(value.r),
                     lut_linear_to_srgb(value.g),
                     lut_linear_to_srgb(value.b));
}

fn linear_to_oklab(value: vec3<f32>) -> vec3<f32> {
    let l = 0.4122214708 * value.r + 0.5363325363 * value.g + 0.0514459929 * value.b;
    let m = 0.2119034982 * value.r + 0.6806995451 * value.g + 0.1073969566 * value.b;
    let s = 0.0883024619 * value.r + 0.2817188376 * value.g + 0.6299787005 * value.b;
    let lr = pow(max(0.0, l), 1.0 / 3.0);
    let mr = pow(max(0.0, m), 1.0 / 3.0);
    let sr = pow(max(0.0, s), 1.0 / 3.0);
    return vec3<f32>(
        0.2104542553 * lr + 0.7936177850 * mr - 0.0040720468 * sr,
        1.9779984951 * lr - 2.4285922050 * mr + 0.4505937099 * sr,
        0.0259040371 * lr + 0.7827717662 * mr - 0.8086757660 * sr);
}

fn oklab_to_linear(value: vec3<f32>) -> vec3<f32> {
    let lr = value.r + 0.3963377774 * value.g + 0.2158037573 * value.b;
    let mr = value.r - 0.1055613458 * value.g - 0.0638541728 * value.b;
    let sr = value.r - 0.0894841775 * value.g - 1.2914855480 * value.b;
    let l = lr * lr * lr;
    let m = mr * mr * mr;
    let s = sr * sr * sr;
    return vec3<f32>(
        4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}

fn gamut_limit(origin: f32, destination: f32) -> f32 {
    let delta = destination - origin;
    if (delta > 0.0) { return (1.0 - origin) / delta; }
    if (delta < 0.0) { return -origin / delta; }
    return 1.0;
}

fn apply_saturation(input: vec3<f32>) -> vec3<f32> {
    let lab = linear_to_oklab(srgb_to_linear3(input));
    let requested = max(0.0, 1.0 + params.saturation / 100.0);
    let neutral = oklab_to_linear(vec3<f32>(lab.r, 0.0, 0.0));
    let gamut_target = oklab_to_linear(vec3<f32>(lab.r,
                                                   lab.g * requested,
                                                   lab.b * requested));
    let limit = clamp(min(gamut_limit(neutral.r, gamut_target.r),
                          min(gamut_limit(neutral.g, gamut_target.g),
                              gamut_limit(neutral.b, gamut_target.b))),
                      0.0, 1.0);
    return linear_to_srgb3(neutral + (gamut_target - neutral) * limit);
}


fn wrap_degrees(value: f32) -> f32 {
    let wrapped = value - floor(value / 360.0) * 360.0;
    return select(wrapped + 360.0, wrapped, wrapped >= 0.0);
}

fn hue_distance(left: f32, right: f32) -> f32 {
    let difference = abs(wrap_degrees(left) - wrap_degrees(right));
    return min(difference, 360.0 - difference);
}

fn rgb_to_hsl(rgb: vec3<f32>) -> vec3<f32> {
    let maximum = max(rgb.r, max(rgb.g, rgb.b));
    let minimum = min(rgb.r, min(rgb.g, rgb.b));
    let chroma = maximum - minimum;
    let lightness = (maximum + minimum) * 0.5;
    var hue = 0.0;
    if (chroma > 0.0000001) {
        if (maximum == rgb.r) {
            hue = 60.0 * ((rgb.g - rgb.b) / chroma);
        } else if (maximum == rgb.g) {
            hue = 60.0 * ((rgb.b - rgb.r) / chroma + 2.0);
        } else {
            hue = 60.0 * ((rgb.r - rgb.g) / chroma + 4.0);
        }
    }
    let saturation = select(chroma / max(0.0000001, 1.0 - abs(2.0 * lightness - 1.0)),
                            0.0,
                            chroma <= 0.0000001);
    return vec3<f32>(wrap_degrees(hue), clamp(saturation, 0.0, 1.0), clamp(lightness, 0.0, 1.0));
}

fn hsl_to_rgb(hsl: vec3<f32>) -> vec3<f32> {
    let hue = wrap_degrees(hsl.r);
    let saturation = clamp(hsl.g, 0.0, 1.0);
    let lightness = clamp(hsl.b, 0.0, 1.0);
    let chroma = (1.0 - abs(2.0 * lightness - 1.0)) * saturation;
    let sector = hue / 60.0;
    let x = chroma * (1.0 - abs((sector - floor(sector / 2.0) * 2.0) - 1.0));
    var rgb = vec3<f32>(0.0);
    if (hue < 60.0) { rgb = vec3<f32>(chroma, x, 0.0); }
    else if (hue < 120.0) { rgb = vec3<f32>(x, chroma, 0.0); }
    else if (hue < 180.0) { rgb = vec3<f32>(0.0, chroma, x); }
    else if (hue < 240.0) { rgb = vec3<f32>(0.0, x, chroma); }
    else if (hue < 300.0) { rgb = vec3<f32>(x, 0.0, chroma); }
    else { rgb = vec3<f32>(chroma, 0.0, x); }
    let lightness_offset = lightness - chroma * 0.5;
    return clamp(rgb + vec3<f32>(lightness_offset), vec3<f32>(0.0), vec3<f32>(1.0));
}

fn hue_range_weight(hue: f32, index: u32) -> f32 {
    let primary = params.hue_ranges[index * 2u];
    let secondary = params.hue_ranges[index * 2u + 1u];
    let distance = hue_distance(hue, primary.w);
    let inner = secondary.x * 0.5;
    if (distance <= inner) { return 1.0; }
    if (secondary.y <= 0.0000001 || distance >= inner + secondary.y) { return 0.0; }
    let t = (distance - inner) / secondary.y;
    return t * t * (2.0 * t - 3.0) + 1.0;
}

fn apply_hue_saturation(input: vec3<f32>) -> vec3<f32> {
    var hsl = rgb_to_hsl(input);
    var hue_shift = params.hue_master.x;
    var saturation_shift = params.hue_master.y;
    var lightness_shift = params.hue_master.z;
    for (var index: u32 = 0u; index < 6u; index = index + 1u) {
        let range = params.hue_ranges[index * 2u];
        let weight = hue_range_weight(hsl.r, index);
        hue_shift = hue_shift + range.x * weight;
        saturation_shift = saturation_shift + range.y * weight;
        lightness_shift = lightness_shift + range.z * weight;
    }
    hsl.r = wrap_degrees(hsl.r + hue_shift);
    let saturation_amount = saturation_shift / 100.0;
    hsl.g = select(hsl.g * (1.0 + saturation_amount),
                   hsl.g + (1.0 - hsl.g) * saturation_amount,
                   saturation_amount >= 0.0);
    let lightness_amount = lightness_shift / 100.0;
    hsl.b = select(hsl.b * (1.0 + lightness_amount),
                   hsl.b + (1.0 - hsl.b) * lightness_amount,
                   lightness_amount >= 0.0);
    return hsl_to_rgb(hsl);
}

fn apply_vibrance(input: vec3<f32>) -> vec3<f32> {
    let lab = linear_to_oklab(srgb_to_linear3(input));
    let chroma = length(lab.yz);
    let hsl = rgb_to_hsl(input);
    let skin_distance = hue_distance(hsl.r, 32.0);
    let skin_weight = clamp(1.0 - skin_distance / 42.0, 0.0, 1.0)
        * clamp((hsl.g - 0.08) / 0.42, 0.0, 1.0);
    let protection = params.vibrance_params.z / 100.0;
    let adaptive_positive = (1.0 - clamp(chroma / 0.32, 0.0, 1.0))
        * (1.0 - skin_weight * protection);
    let adaptive = select(1.0, adaptive_positive, params.vibrance_params.x >= 0.0);
    let multiplier = max(0.0,
        (1.0 + params.vibrance_params.y / 100.0)
        * (1.0 + params.vibrance_params.x / 100.0 * adaptive));
    let neutral = oklab_to_linear(vec3<f32>(lab.x, 0.0, 0.0));
    let gamut_target = oklab_to_linear(vec3<f32>(lab.x, lab.y * multiplier, lab.z * multiplier));
    let limit = clamp(min(gamut_limit(neutral.r, gamut_target.r),
                          min(gamut_limit(neutral.g, gamut_target.g),
                              gamut_limit(neutral.b, gamut_target.b))),
                      0.0, 1.0);
    return linear_to_srgb3(neutral + (gamut_target - neutral) * limit);
}

fn apply_photo_filter(input: vec3<f32>) -> vec3<f32> {
    let density = clamp(params.photo_filter_params.w, 0.0, 1.0);
    if (density <= 0.0000001) { return input; }
    let filter_linear = srgb_to_linear3(params.photo_filter_params.xyz);
    let maximum = max(max(filter_linear.r, filter_linear.g),
                      max(filter_linear.b, 0.000001));
    var scale = vec3<f32>(0.12) + 0.88 * filter_linear / maximum;
    if (params.discrete_params.w >= 0.5) {
        let luminance = max(0.000001, dot(scale, vec3<f32>(0.2126, 0.7152, 0.0722)));
        scale = scale / luminance;
    }
    scale = pow(max(scale, vec3<f32>(0.000001)), vec3<f32>(density));
    let linear = srgb_to_linear3(input);
    let filtered_linear = linear * scale;
    let limit = clamp(min(gamut_limit(linear.r, filtered_linear.r),
                          min(gamut_limit(linear.g, filtered_linear.g),
                              gamut_limit(linear.b, filtered_linear.b))),
                      0.0, 1.0);
    return linear_to_srgb3(linear + (filtered_linear - linear) * limit);
}

fn apply_white_balance(input: vec3<f32>) -> vec3<f32> {
    let temperature = params.white_balance_params.x / 100.0;
    let tint = params.white_balance_params.y / 100.0;
    var scales = vec3<f32>(exp2(temperature * 0.55 - tint * 0.04),
                           exp2(-abs(temperature) * 0.04 - tint * 0.30),
                           exp2(-temperature * 0.55 - tint * 0.04));
    let normalisation = dot(scales, vec3<f32>(0.2126, 0.7152, 0.0722));
    scales = scales / normalisation;
    return linear_to_srgb3(srgb_to_linear3(input) * scales);
}

fn apply_colour_balance(input: vec3<f32>) -> vec3<f32> {
    let linear = srgb_to_linear3(input);
    let luminance = dot(linear, vec3<f32>(0.2126, 0.7152, 0.0722));
    let weights = vec3<f32>(clamp((0.55 - luminance) / 0.55, 0.0, 1.0),
                            0.0,
                            clamp((luminance - 0.45) / 0.55, 0.0, 1.0));
    let tonal_weights = vec3<f32>(weights.x,
                                  clamp(1.0 - max(weights.x, weights.z), 0.0, 1.0),
                                  weights.z);
    var shift = vec3<f32>(0.0);
    for (var index: u32 = 0u; index < 3u; index = index + 1u) {
        let range = params.colour_balance_ranges[index];
        let weight = tonal_weights[index] * 0.28 / 100.0;
        shift.r = shift.r + weight * (range.x - 0.5 * range.y - 0.5 * range.z);
        shift.g = shift.g + weight * (range.y - 0.5 * range.x - 0.5 * range.z);
        shift.b = shift.b + weight * (range.z - 0.5 * range.x - 0.5 * range.y);
    }
    var adjusted = linear + shift;
    if (params.colour_balance_options.x >= 0.5) {
        let adjusted_luminance = dot(adjusted, vec3<f32>(0.2126, 0.7152, 0.0722));
        adjusted = adjusted + vec3<f32>(luminance - adjusted_luminance);
    }
    let neutral = vec3<f32>(luminance);
    let limit = clamp(min(gamut_limit(neutral.r, adjusted.r),
                          min(gamut_limit(neutral.g, adjusted.g),
                              gamut_limit(neutral.b, adjusted.b))),
                      0.0, 1.0);
    return linear_to_srgb3(neutral + (adjusted - neutral) * limit);
}


fn apply_channel_mixer(input: vec3<f32>) -> vec3<f32> {
    if (params.channel_mixer_options.x >= 0.5) {
        let row = params.channel_mixer_rows[3];
        let grey = clamp(dot(input, row.xyz) / 100.0 + row.w / 100.0, 0.0, 1.0);
        return vec3<f32>(grey);
    }
    let red_row = params.channel_mixer_rows[0];
    let green_row = params.channel_mixer_rows[1];
    let blue_row = params.channel_mixer_rows[2];
    return clamp(vec3<f32>(dot(input, red_row.xyz) / 100.0 + red_row.w / 100.0,
                           dot(input, green_row.xyz) / 100.0 + green_row.w / 100.0,
                           dot(input, blue_row.xyz) / 100.0 + blue_row.w / 100.0),
                 vec3<f32>(0.0), vec3<f32>(1.0));
}

fn black_white_family_weight(hue: f32, centre: f32) -> f32 {
    return max(0.0, 1.0 - hue_distance(hue, centre) / 60.0);
}

fn black_white_weight(index: u32) -> f32 {
    if (index < 4u) { return params.black_white_weights0[index]; }
    return params.black_white_weights1[index - 4u];
}

fn apply_black_and_white(input: vec3<f32>) -> vec3<f32> {
    let hsl = rgb_to_hsl(input);
    var family_scale = 0.0;
    var total_weight = 0.0;
    for (var index: u32 = 0u; index < 6u; index = index + 1u) {
        let centre = f32(index) * 60.0;
        let weight = black_white_family_weight(hsl.x, centre);
        family_scale = family_scale + weight * black_white_weight(index) / 100.0;
        total_weight = total_weight + weight;
    }
    family_scale = select(1.0, family_scale / total_weight, total_weight > 0.0000001);
    let luminance = dot(input, vec3<f32>(0.2126, 0.7152, 0.0722));
    let grey = clamp(luminance * (1.0 + (family_scale - 1.0) * hsl.y), 0.0, 1.0);
    if (params.black_white_weights1.z < 0.5 || params.black_white_options.x <= 0.0000001) {
        return vec3<f32>(grey);
    }
    return hsl_to_rgb(vec3<f32>(params.black_white_weights1.w,
                                params.black_white_options.x / 100.0, grey));
}

fn selective_colour_range_scale(index: u32, input: vec3<f32>,
                                      minimum: f32, maximum: f32,
                                      middle: f32) -> f32 {
    switch index {
        case 0u: { return select(0.0, maximum - middle, input.r == maximum); }
        case 1u: { return select(0.0, middle - minimum, input.b == minimum); }
        case 2u: { return select(0.0, maximum - middle, input.g == maximum); }
        case 3u: { return select(0.0, middle - minimum, input.r == minimum); }
        case 4u: { return select(0.0, maximum - middle, input.b == maximum); }
        case 5u: { return select(0.0, middle - minimum, input.g == minimum); }
        case 6u: {
            return select(0.0, (minimum - 0.5) * 2.0,
                          input.r > 0.5 && input.g > 0.5 && input.b > 0.5);
        }
        case 7u: {
            return select(0.0,
                          max(0.0, 1.0 - (abs(maximum - 0.5)
                                           + abs(minimum - 0.5))),
                          maximum > 0.0 && minimum < 1.0);
        }
        case 8u: {
            return select(0.0, (0.5 - maximum) * 2.0,
                          input.r < 0.5 && input.g < 0.5 && input.b < 0.5);
        }
        default: { return 0.0; }
    }
}

fn selective_colour_component_adjustment(value: f32, adjustment: f32,
                                          black_adjustment: f32) -> f32 {
    let amount = adjustment / 100.0;
    let black = black_adjustment / 100.0;
    var delta = (-1.0 - amount) * black - amount;
    if (params.selective_colour_options.x < 0.5) {
        delta = delta * (1.0 - value);
    }
    return clamp(delta, -value, 1.0 - value);
}

fn apply_selective_colour(input: vec3<f32>) -> vec3<f32> {
    let minimum = min(min(input.r, input.g), input.b);
    let maximum = max(max(input.r, input.g), input.b);
    let middle = input.r + input.g + input.b - minimum - maximum;
    var delta = vec3<f32>(0.0);
    for (var index: u32 = 0u; index < 9u; index = index + 1u) {
        let scale = selective_colour_range_scale(index, input, minimum, maximum, middle);
        let range = params.selective_colour_ranges[index];
        delta.r = delta.r + scale * selective_colour_component_adjustment(
            input.r, range.r, range.w);
        delta.g = delta.g + scale * selective_colour_component_adjustment(
            input.g, range.g, range.w);
        delta.b = delta.b + scale * selective_colour_component_adjustment(
            input.b, range.b, range.w);
    }
    return clamp(input + delta, vec3<f32>(0.0), vec3<f32>(1.0));
}

fn apply_gradient_lut(input: vec3<f32>) -> vec3<f32> {
    let luminance = clamp(dot(input, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    let index = i32(clamp(floor(luminance * 255.0 + 0.5), 0.0, 255.0));
    return textureLoad(tonal_lut, vec2<i32>(index, 0), 0).rgb;
}

fn apply_tonal_lut(input: vec3<f32>) -> vec3<f32> {
    let indices = vec3<i32>(clamp(floor(input * 255.0 + vec3<f32>(0.5)),
                                  vec3<f32>(0.0), vec3<f32>(255.0)));
    return vec3<f32>(textureLoad(tonal_lut, vec2<i32>(indices.r, 0), 0).r,
                     textureLoad(tonal_lut, vec2<i32>(indices.g, 0), 0).g,
                     textureLoad(tonal_lut, vec2<i32>(indices.b, 0), 0).b);
}

fn apply_posterise(input: vec3<f32>) -> vec3<f32> {
    let levels = max(2.0, params.discrete_params.x);
    let scale = levels - 1.0;
    return clamp(floor(input * scale + vec3<f32>(0.5)) / scale,
                 vec3<f32>(0.0), vec3<f32>(1.0));
}

fn apply_threshold(input: vec3<f32>) -> vec3<f32> {
    var source = dot(input, vec3<f32>(0.2126, 0.7152, 0.0722));
    let selector = u32(max(0.0, params.discrete_params.z));
    if (selector == 1u) { source = input.r; }
    else if (selector == 2u) { source = input.g; }
    else if (selector == 3u) { source = input.b; }
    let value = select(0.0, 1.0, source >= params.discrete_params.y);
    return vec3<f32>(value);
}

fn sample_shaper_channel(value: f32, channel: i32) -> f32 {
    let size = i32(params.lut_options.y + 0.5);
    if (size < 2) { return value; }
    let minimum = params.lut_shaper_domain_min[channel];
    let maximum = params.lut_shaper_domain_max[channel];
    let coordinate = clamp((value - minimum) / max(0.0000001, maximum - minimum), 0.0, 1.0)
        * f32(size - 1);
    let left = clamp(i32(floor(coordinate)), 0, size - 1);
    let right = min(size - 1, left + 1);
    let fraction = coordinate - f32(left);
    let row = i32(params.lut_options.w + 0.5);
    let a = textureLoad(tonal_lut, vec2<i32>(left, row), 0)[channel];
    let b = textureLoad(tonal_lut, vec2<i32>(right, row), 0)[channel];
    return mix(a, b, fraction);
}

fn lut_cube_entry(red: i32, green: i32, blue: i32, size: i32) -> vec3<f32> {
    return textureLoad(tonal_lut, vec2<i32>(red + blue * size, green), 0).rgb;
}

fn sample_cube(input: vec3<f32>, interpolation: u32) -> vec3<f32> {
    let size = i32(params.lut_options.z + 0.5);
    if (size < 2) { return input; }
    let coordinate = clamp((input - params.lut_cube_domain_min.xyz)
                               / max(vec3<f32>(0.0000001),
                                     params.lut_cube_domain_max.xyz - params.lut_cube_domain_min.xyz),
                           vec3<f32>(0.0), vec3<f32>(1.0)) * f32(size - 1);
    let low = vec3<i32>(clamp(floor(coordinate), vec3<f32>(0.0), vec3<f32>(f32(size - 1))));
    let high = min(vec3<i32>(size - 1), low + vec3<i32>(1));
    let fraction = coordinate - vec3<f32>(low);
    let c000 = lut_cube_entry(low.x, low.y, low.z, size);
    let c100 = lut_cube_entry(high.x, low.y, low.z, size);
    let c010 = lut_cube_entry(low.x, high.y, low.z, size);
    let c110 = lut_cube_entry(high.x, high.y, low.z, size);
    let c001 = lut_cube_entry(low.x, low.y, high.z, size);
    let c101 = lut_cube_entry(high.x, low.y, high.z, size);
    let c011 = lut_cube_entry(low.x, high.y, high.z, size);
    let c111 = lut_cube_entry(high.x, high.y, high.z, size);

    if (interpolation != 0u) {
        let red = fraction.x;
        let green = fraction.y;
        let blue = fraction.z;
        // Match the CPU evaluator's stable tie precedence exactly.
        if (red >= green) {
            if (green >= blue) {
                return c000 + red * (c100 - c000)
                    + green * (c110 - c100) + blue * (c111 - c110);
            }
            if (red >= blue) {
                return c000 + red * (c100 - c000)
                    + blue * (c101 - c100) + green * (c111 - c101);
            }
            return c000 + blue * (c001 - c000)
                + red * (c101 - c001) + green * (c111 - c101);
        }
        if (blue >= green) {
            return c000 + blue * (c001 - c000)
                + green * (c011 - c001) + red * (c111 - c011);
        }
        if (blue >= red) {
            return c000 + green * (c010 - c000)
                + blue * (c011 - c010) + red * (c111 - c011);
        }
        return c000 + green * (c010 - c000)
            + red * (c110 - c010) + blue * (c111 - c110);
    }

    let xr00 = mix(c000, c100, fraction.x);
    let xr10 = mix(c010, c110, fraction.x);
    let xr01 = mix(c001, c101, fraction.x);
    let xr11 = mix(c011, c111, fraction.x);
    return mix(mix(xr00, xr10, fraction.y),
               mix(xr01, xr11, fraction.y), fraction.z);
}

fn sample_imported_lut(input: vec3<f32>, interpolation: u32) -> vec3<f32> {
    let shaped = vec3<f32>(sample_shaper_channel(input.r, 0),
                           sample_shaper_channel(input.g, 1),
                           sample_shaper_channel(input.b, 2));
    return sample_cube(shaped, interpolation);
}

fn lut_document_to_linear(input: vec3<f32>, document_transfer: u32) -> vec3<f32> {
    return select(input, lut_srgb_to_linear3(input), document_transfer == 0u);
}

fn lut_linear_to_document(input: vec3<f32>, document_transfer: u32) -> vec3<f32> {
    return select(input, lut_linear_to_srgb3(input), document_transfer == 0u);
}

fn tony_allocate(value: f32) -> f32 {
    let denominator = value + 1.0;
    if (abs(denominator) <= 0.0000000000000002220446) { return 0.0; }
    return value / denominator;
}

fn apply_tony_mcmapface(input: vec3<f32>, document_transfer: u32) -> vec3<f32> {
    let linear = lut_document_to_linear(input, document_transfer);
    let allocated = vec3<f32>(tony_allocate(linear.r),
                               tony_allocate(linear.g),
                               tony_allocate(linear.b));
    let mapped = sample_imported_lut(allocated, 1u);
    return lut_linear_to_document(mapped, document_transfer);
}

fn agx_allocate(value: f32) -> f32 {
    if (value <= 0.0) { return 0.0; }
    let minimum_exposure = -12.47393;
    let exposure_span = 12.5260688117 - minimum_exposure;
    return clamp((log2(value) - minimum_exposure) / exposure_span, 0.0, 1.0);
}

fn apply_agx_base_srgb(input: vec3<f32>, document_transfer: u32) -> vec3<f32> {
    let linear = lut_document_to_linear(input, document_transfer);
    let film_light = vec3<f32>(
        linear.r * 0.5594630473276861 + linear.g * 0.3047758110283366 + linear.b * 0.1358129414038276,
        linear.r * 0.0762332608733703 + linear.g * 0.7879523952184488 + linear.b * 0.1357748488287584,
        linear.r * 0.0655375095152927 + linear.g * 0.1645427298716744 + linear.b * 0.7697415276874705);
    let allocated = vec3<f32>(agx_allocate(film_light.r),
                               agx_allocate(film_light.g),
                               agx_allocate(film_light.b));
    let table_output = sample_imported_lut(allocated, 1u);
    let linear_output = pow(max(vec3<f32>(0.0), table_output), vec3<f32>(2.4));
    return lut_linear_to_document(linear_output, document_transfer);
}

fn apply_imported_lut(input: vec3<f32>) -> vec3<f32> {
    if (params.lut_options.y < 1.5 && params.lut_options.z < 1.5) { return input; }
    let interpolation = u32(params.lut_modes.x + 0.5);
    let processing_mode = u32(params.lut_modes.y + 0.5);
    let operator_profile = u32(params.lut_modes.z + 0.5);
    let document_transfer = u32(params.lut_modes.w + 0.5);
    var mapped = input;

    if (operator_profile == 1u) {
        mapped = apply_tony_mcmapface(input, document_transfer);
    } else if (operator_profile == 2u) {
        mapped = apply_agx_base_srgb(input, document_transfer);
    } else {
        var table_input = input;
        let encoded_table_on_linear_document = processing_mode == 0u
            && document_transfer == 1u;
        let linear_table_on_encoded_document = processing_mode == 1u
            && document_transfer == 0u;
        if (encoded_table_on_linear_document) {
            table_input = lut_linear_to_srgb3(input);
        } else if (linear_table_on_encoded_document) {
            table_input = lut_srgb_to_linear3(input);
        }
        mapped = sample_imported_lut(table_input, interpolation);
        if (encoded_table_on_linear_document) {
            mapped = lut_srgb_to_linear3(mapped);
        } else if (linear_table_on_encoded_document) {
            mapped = lut_linear_to_srgb3(mapped);
        }
    }
    return mix(input, mapped, clamp(params.lut_options.x, 0.0, 1.0));
}

fn shadow_sample_offset(index: u32, radius: i32) -> i32 {
    let centred = i32(index) - 6;
    let scaled = f32(radius) * f32(centred) / 6.0;
    return select(i32(ceil(scaled - 0.5)),
                  i32(floor(scaled + 0.5)),
                  scaled >= 0.0);
}

fn shadow_sample_weight(index: u32) -> f32 {
    switch (index) {
        case 0u: { return 1.0; }
        case 1u: { return 4.0; }
        case 2u: { return 11.0; }
        case 3u: { return 25.0; }
        case 4u: { return 44.0; }
        case 5u: { return 58.0; }
        case 6u: { return 64.0; }
        case 7u: { return 58.0; }
        case 8u: { return 44.0; }
        case 9u: { return 25.0; }
        case 10u: { return 11.0; }
        case 11u: { return 4.0; }
        default: { return 1.0; }
    }
}

fn local_adaptation_fallback(position: vec2<i32>, dimensions: vec2<u32>) -> f32 {
    let radius = clamp(i32(floor(params.shadows_highlights1.x + 0.5)), 1, 500);
    let maximum_position = vec2<i32>(dimensions) - vec2<i32>(1);
    var sum = 0.0;
    var total_weight = 0.0;
    // This direct path is retained only as a defensive fallback. Production
    // Shadows/Highlights rendering uses the two separable entry points below.
    for (var y_index: u32 = 0u; y_index < 5u; y_index = y_index + 1u) {
        let mapped_y = y_index * 3u;
        let y_offset = shadow_sample_offset(mapped_y, radius);
        let y_weight = shadow_sample_weight(mapped_y);
        for (var x_index: u32 = 0u; x_index < 5u; x_index = x_index + 1u) {
            let mapped_x = x_index * 3u;
            let x_offset = shadow_sample_offset(mapped_x, radius);
            let weight = y_weight * shadow_sample_weight(mapped_x);
            let sample_position = clamp(position + vec2<i32>(x_offset, y_offset),
                                        vec2<i32>(0), maximum_position);
            let sampled = to_adjustment_domain(
                textureLoad(base_texture, sample_position, 0).rgb);
            let linear = srgb_to_linear3(sampled);
            sum = sum + dot(linear, vec3<f32>(0.2126, 0.7152, 0.0722)) * weight;
            total_weight = total_weight + weight;
        }
    }
    return sum / max(total_weight, 0.000001);
}

fn apply_shadows_highlights_local(input: vec3<f32>, local_luminance_input: f32) -> vec3<f32> {
    let linear = srgb_to_linear3(input);
    let luminance = clamp(dot(linear, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    let local_luminance = clamp(local_luminance_input, 0.0, 1.0);
    let shadow_limit = 0.05 + 0.90 * params.shadows_highlights0.y / 100.0;
    let highlight_limit = 0.05 + 0.90 * params.shadows_highlights0.w / 100.0;
    let shadow_weight = 1.0 - smoothstep(0.0, shadow_limit, local_luminance);
    let highlight_weight = smoothstep(1.0 - highlight_limit, 1.0, local_luminance);

    var adjusted_luminance = luminance
        + params.shadows_highlights0.x / 100.0 * shadow_weight * (1.0 - luminance) * 0.85;
    adjusted_luminance = adjusted_luminance
        - params.shadows_highlights0.z / 100.0 * highlight_weight * adjusted_luminance * 0.65;
    var midtone_weight = max(0.0, 1.0 - abs(2.0 * local_luminance - 1.0));
    midtone_weight = midtone_weight * midtone_weight;
    let contrast_scale = max(0.05,
        1.0 + params.shadows_highlights1.y / 100.0 * 0.65 * midtone_weight);
    adjusted_luminance = clamp((adjusted_luminance - 0.5) * contrast_scale + 0.5, 0.0, 1.0);

    let adjusted_linear = select(vec3<f32>(adjusted_luminance),
        linear * (adjusted_luminance / max(luminance, 0.000000001)),
        luminance > 0.000000001);
    var result = linear_to_srgb3(clamp(adjusted_linear, vec3<f32>(0.0), vec3<f32>(1.0)));
    if (params.shadows_highlights1.z > 0.0) {
        var hsl = rgb_to_hsl(result);
        let correction = params.shadows_highlights1.z / 100.0
            * abs(adjusted_luminance - luminance) * 1.5;
        hsl.g = clamp(hsl.g * (1.0 + correction), 0.0, 1.0);
        result = hsl_to_rgb(hsl);
    }
    return result;
}

fn apply_shadows_highlights(input: vec3<f32>,
                            position: vec2<i32>,
                            dimensions: vec2<u32>) -> vec3<f32> {
    return apply_shadows_highlights_local(
        input, local_adaptation_fallback(position, dimensions));
}

fn apply_adjustment(input: vec3<f32>, position: vec2<i32>, dimensions: vec2<u32>) -> vec3<f32> {
    switch (params.adjustment_type) {
        case 0u: {
            if (abs(params.exposure) <= 0.0000001
                && abs(params.exposure_offset) <= 0.0000001
                && abs(params.exposure_gamma - 1.0) <= 0.0000001) {
                return input;
            }
            let factor = exp2(clamp(params.exposure, -16.0, 16.0));
            let inverse_gamma = 1.0 / max(0.01, params.exposure_gamma);
            if (params.managed_domain == 1u) {
                let linear = max(vec3<f32>(0.0),
                                 input * factor + vec3<f32>(params.exposure_offset));
                return clamp(pow(linear, vec3<f32>(inverse_gamma)),
                             vec3<f32>(0.0), vec3<f32>(1.0));
            }
            let linear = max(vec3<f32>(0.0),
                             srgb_to_linear3(input) * factor
                                 + vec3<f32>(params.exposure_offset));
            return linear_to_srgb3(pow(linear, vec3<f32>(inverse_gamma)));
        }
        case 1u: {
            if (abs(params.contrast) <= 0.0000001) { return input; }
            let linear = srgb_to_linear3(input);
            let luminance = dot(linear, vec3<f32>(0.2126, 0.7152, 0.0722));
            let pivot = srgb_to_linear(clamp(params.contrast_pivot, 0.0, 1.0));
            let amount = clamp(params.contrast / 100.0, -1.0, 1.0);
            let factor = select(max(0.0, 1.0 + amount),
                                1.0 / max(0.01, 1.0 - amount * 0.99),
                                amount >= 0.0);
            let adjusted_luminance = clamp((luminance - pivot) * factor + pivot, 0.0, 1.0);
            let adjusted = select(vec3<f32>(adjusted_luminance),
                                  linear * (adjusted_luminance / max(luminance, 0.0000001)),
                                  luminance > 0.0000001);
            return linear_to_srgb3(adjusted);
        }
        case 2u: {
            if (abs(params.saturation) <= 0.0000001) { return input; }
            return apply_saturation(input);
        }
        case 3u: { return apply_tonal_lut(input); }
        case 4u: { return apply_tonal_lut(input); }
        case 5u: {
            var magnitude = abs(params.hue_master.x)
                + abs(params.hue_master.y) + abs(params.hue_master.z);
            for (var index: u32 = 0u; index < 6u; index = index + 1u) {
                let range = params.hue_ranges[index * 2u];
                magnitude = magnitude + abs(range.x) + abs(range.y) + abs(range.z);
            }
            return select(apply_hue_saturation(input), input, magnitude <= 0.0000001);
        }
        case 6u: {
            return select(apply_vibrance(input), input,
                          abs(params.vibrance_params.x) <= 0.0000001
                          && abs(params.vibrance_params.y) <= 0.0000001);
        }
        case 7u: {
            return select(apply_white_balance(input), input,
                          abs(params.white_balance_params.x) <= 0.0000001
                          && abs(params.white_balance_params.y) <= 0.0000001);
        }
        case 8u: {
            var magnitude = 0.0;
            for (var index: u32 = 0u; index < 3u; index = index + 1u) {
                let range = params.colour_balance_ranges[index];
                magnitude = magnitude + abs(range.x) + abs(range.y) + abs(range.z);
            }
            return select(apply_colour_balance(input), input, magnitude <= 0.0000001);
        }
        case 9u: { return apply_channel_mixer(input); }
        case 10u: { return apply_black_and_white(input); }
        case 11u: { return apply_gradient_lut(input); }
        case 12u: { return apply_posterise(input); }
        case 13u: { return apply_threshold(input); }
        case 14u: { return apply_imported_lut(input); }
        case 15u: {
            if (abs(params.shadows_highlights0.x) <= 0.0000001
                && abs(params.shadows_highlights0.z) <= 0.0000001
                && abs(params.shadows_highlights1.y) <= 0.0000001) {
                return input;
            }
            return apply_shadows_highlights(input, position, dimensions);
        }
        case 20u: { return vec3<f32>(1.0) - input; }
        case 21u: { return apply_photo_filter(input); }
        case 22u: { return apply_selective_colour(input); }
        default: { return input; }
    }
}

fn quantize_rgba8(value: vec3<f32>) -> vec3<f32> {
    return floor(clamp(value, vec3<f32>(0.0), vec3<f32>(1.0)) * 255.0
                 + vec3<f32>(0.5)) / 255.0;
}

fn blend_channel(base: f32, effect: f32, mode: u32) -> f32 {
    switch (mode) {
        case 1u: { return base * effect; }
        case 2u: { return 1.0 - (1.0 - base) * (1.0 - effect); }
        case 3u: { return select(2.0 * base * effect,
                                 1.0 - 2.0 * (1.0 - base) * (1.0 - effect),
                                 base > 0.5); }
        case 4u: { return min(base, effect); }
        case 5u: { return max(base, effect); }
        case 6u: { return select(min(1.0, base / max(1.0 - effect, 0.00001)),
                                 1.0, effect >= 1.0); }
        case 7u: { return select(1.0 - min(1.0, (1.0 - base) / max(effect, 0.00001)),
                                 0.0, effect <= 0.0); }
        case 8u: { return min(1.0, base + effect); }
        case 9u: { return max(0.0, base - effect); }
        case 10u: { return abs(base - effect); }
        case 11u: { return base + effect - 2.0 * base * effect; }
        default: { return effect; }
    }
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_texture);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) { return; }
    let position = vec2<i32>(gid.xy);
    let base = textureLoad(base_texture, position, 0);
    let domain_input = to_adjustment_domain(base.rgb);
    let domain_adjusted = quantize_rgba8(
        apply_adjustment(domain_input, position, dimensions));
    let adjusted = from_adjustment_domain(domain_adjusted);
    var mask = 1.0;
    if (params.use_mask != 0u) {
        mask = textureLoad(mask_texture, position, 0).r;
    }
    let weight = clamp(params.opacity * mask, 0.0, 1.0);
    let blended = vec3<f32>(blend_channel(base.r, adjusted.r, params.blend_mode),
                            blend_channel(base.g, adjusted.g, params.blend_mode),
                            blend_channel(base.b, adjusted.b, params.blend_mode));
    let mixed_rgb = clamp(base.rgb + (blended - base.rgb) * weight,
                          vec3<f32>(0.0), vec3<f32>(1.0));
    // Straight-alpha storage deliberately preserves hidden RGB beneath zero alpha.
    textureStore(output_texture, position, vec4<f32>(mixed_rgb, base.a));
}

@compute @workgroup_size(8, 8, 1)
fn shadows_horizontal(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_texture);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) { return; }
    let position = vec2<i32>(gid.xy);
    let maximum_position = vec2<i32>(dimensions) - vec2<i32>(1);
    let radius = clamp(i32(floor(params.shadows_highlights1.x + 0.5)), 1, 500);
    var sum = 0.0;
    var total_weight = 0.0;
    for (var index: u32 = 0u; index < 13u; index = index + 1u) {
        let sample_position = clamp(
            position + vec2<i32>(shadow_sample_offset(index, radius), 0),
            vec2<i32>(0), maximum_position);
        let sampled = to_adjustment_domain(
            textureLoad(base_texture, sample_position, 0).rgb);
        let linear = srgb_to_linear3(sampled);
        let weight = shadow_sample_weight(index);
        sum = sum + dot(linear, vec3<f32>(0.2126, 0.7152, 0.0722)) * weight;
        total_weight = total_weight + weight;
    }
    let adaptation = floor(clamp(sum / max(total_weight, 0.000001), 0.0, 1.0)
                           * 255.0 + 0.5) / 255.0;
    textureStore(output_texture, position,
                 vec4<f32>(adaptation, adaptation, adaptation, 1.0));
}

@compute @workgroup_size(8, 8, 1)
fn shadows_vertical_apply(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_texture);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) { return; }
    let position = vec2<i32>(gid.xy);
    let maximum_position = vec2<i32>(dimensions) - vec2<i32>(1);
    let radius = clamp(i32(floor(params.shadows_highlights1.x + 0.5)), 1, 500);
    var sum = 0.0;
    var total_weight = 0.0;
    for (var index: u32 = 0u; index < 13u; index = index + 1u) {
        let sample_position = clamp(
            position + vec2<i32>(0, shadow_sample_offset(index, radius)),
            vec2<i32>(0), maximum_position);
        let weight = shadow_sample_weight(index);
        sum = sum + textureLoad(tonal_lut, sample_position, 0).r * weight;
        total_weight = total_weight + weight;
    }
    let local_luminance = sum / max(total_weight, 0.000001);
    let base = textureLoad(base_texture, position, 0);
    let domain_input = to_adjustment_domain(base.rgb);
    let domain_adjusted = quantize_rgba8(
        apply_shadows_highlights_local(domain_input, local_luminance));
    let adjusted = from_adjustment_domain(domain_adjusted);
    var mask = 1.0;
    if (params.use_mask != 0u) {
        mask = textureLoad(mask_texture, position, 0).r;
    }
    let weight = clamp(params.opacity * mask, 0.0, 1.0);
    let blended = vec3<f32>(blend_channel(base.r, adjusted.r, params.blend_mode),
                            blend_channel(base.g, adjusted.g, params.blend_mode),
                            blend_channel(base.b, adjusted.b, params.blend_mode));
    let mixed_rgb = clamp(base.rgb + (blended - base.rgb) * weight,
                          vec3<f32>(0.0), vec3<f32>(1.0));
    textureStore(output_texture, position, vec4<f32>(mixed_rgb, base.a));
}
