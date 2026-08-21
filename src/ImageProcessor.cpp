#include "ImageProcessor.h"

#include "VectorRasterizer.h"
#include "TextRasterizer.h"
#include "CubeLut.h"
#include "TonalMapping.h"
#include "SpatialFilter.h"
#include "SmartLayerTileCache.h"
#include "TransformSampling.h"

#include <QtConcurrentMap>

#include <QColorSpace>
#include <QCryptographicHash>
#include <QDataStream>
#include <QHash>
#include <QIODevice>
#include <QPainter>
#include <QPointF>
#include <QRgba64>
#include <QSet>
#include <QSizeF>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <deque>
#include <mutex>
#include <numeric>
#include <numbers>
#include <utility>

namespace vfx {
namespace {

struct FloatPixel {
    double r;
    double g;
    double b;
};

QThreadPool *processingPool()
{
    static QThreadPool pool;
    static std::once_flag configured;
    std::call_once(configured, [] {
        pool.setMaxThreadCount(std::max(1, QThread::idealThreadCount()));
        pool.setExpiryTimeout(30'000);
    });
    return &pool;
}

double clamp01(const double value)
{
    return std::clamp(value, 0.0, 1.0);
}

template <typename Function>
void processRows(const int width, const int height, Function &&function)
{
    // Thread-pool dispatch costs more than it saves for the small dirty tiles
    // produced by a live brush stroke. Settled/full previews still fan out.
    constexpr qint64 ParallelPixelThreshold = 256 * 256;
    if (static_cast<qint64>(width) * height < ParallelPixelThreshold) {
        for (int y = 0; y < height; ++y) {
            function(y);
        }
        return;
    }
    QVector<int> rows(height);
    std::iota(rows.begin(), rows.end(), 0);
    QtConcurrent::blockingMap(processingPool(), rows, std::forward<Function>(function));
}

const SpatialRowProcessor &parallelSpatialRows()
{
    static const SpatialRowProcessor processor = [](
        const int width,
        const int height,
        const std::function<void(int)> &processRow) {
        processRows(width, height, processRow);
    };
    return processor;
}

double srgbToLinear(const double value)
{
    const double clamped = clamp01(value);
    if (clamped <= 0.04045) {
        return clamped / 12.92;
    }
    return std::pow((clamped + 0.055) / 1.055, 2.4);
}

double linearToSrgb(const double value)
{
    const double clamped = std::max(0.0, value);
    if (clamped <= 0.0031308) {
        return clamp01(clamped * 12.92);
    }
    return clamp01(1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055);
}

FloatPixel linearRgb(const FloatPixel &pixel)
{
    return {srgbToLinear(pixel.r), srgbToLinear(pixel.g), srgbToLinear(pixel.b)};
}

FloatPixel encodedRgb(const FloatPixel &pixel)
{
    return {linearToSrgb(pixel.r), linearToSrgb(pixel.g), linearToSrgb(pixel.b)};
}

QColorSpace adjustmentDomainColourSpace(const QColorSpace &workingSpace,
                                        const AdjustmentProcessingDomain domain)
{
    if (!workingSpace.isValid()) return {};
    switch (domain) {
    case AdjustmentProcessingDomain::LinearWorking: {
        QColorSpace linear = workingSpace.withTransferFunction(
            QColorSpace::TransferFunction::Linear);
        if (linear.isValid()) {
            linear.setDescription(workingSpace.description().isEmpty()
                ? QStringLiteral("Linear working space")
                : workingSpace.description() + QStringLiteral(" (Linear)"));
        }
        return linear;
    }
    case AdjustmentProcessingDomain::EncodedSrgb:
        return QColorSpace(QColorSpace::SRgb);
    case AdjustmentProcessingDomain::EncodedWorking:
    case AdjustmentProcessingDomain::RawComponents:
    case AdjustmentProcessingDomain::LutContract:
        return workingSpace;
    }
    return workingSpace;
}

QImage transformAdjustmentDomainImage(const QImage &input,
                                      const QColorSpace &target)
{
    if (input.isNull() || !target.isValid() || !input.colorSpace().isValid()
        || input.colorSpace() == target) {
        return input;
    }
    ColourTransformRequest request;
    request.source = ColourSpaceDescriptor::fromQColorSpace(input.colorSpace());
    request.destination = ColourSpaceDescriptor::fromQColorSpace(target);
    request.purpose = ColourTransformPurpose::AdjustmentDomain;
    const std::optional<QColorTransform> transform =
        ColourTransformService::instance().qtTransform(request);
    if (!transform.has_value()) return {};

    const QImage::Format straightFormat = input.depth() > 32
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    QImage converted = input.convertToFormat(straightFormat);
    converted.applyColorTransform(*transform);
    if (converted.isNull()) return {};
    converted.setColorSpace(target);
    return converted;
}

FloatPixel linearToOklab(const FloatPixel &pixel)
{
    const double l = 0.4122214708 * pixel.r + 0.5363325363 * pixel.g + 0.0514459929 * pixel.b;
    const double m = 0.2119034982 * pixel.r + 0.6806995451 * pixel.g + 0.1073969566 * pixel.b;
    const double s = 0.0883024619 * pixel.r + 0.2817188376 * pixel.g + 0.6299787005 * pixel.b;
    const double lRoot = std::cbrt(std::max(0.0, l));
    const double mRoot = std::cbrt(std::max(0.0, m));
    const double sRoot = std::cbrt(std::max(0.0, s));
    return {
        0.2104542553 * lRoot + 0.7936177850 * mRoot - 0.0040720468 * sRoot,
        1.9779984951 * lRoot - 2.4285922050 * mRoot + 0.4505937099 * sRoot,
        0.0259040371 * lRoot + 0.7827717662 * mRoot - 0.8086757660 * sRoot
    };
}

FloatPixel oklabToLinear(const FloatPixel &lab)
{
    const double lRoot = lab.r + 0.3963377774 * lab.g + 0.2158037573 * lab.b;
    const double mRoot = lab.r - 0.1055613458 * lab.g - 0.0638541728 * lab.b;
    const double sRoot = lab.r - 0.0894841775 * lab.g - 1.2914855480 * lab.b;
    const double l = lRoot * lRoot * lRoot;
    const double m = mRoot * mRoot * mRoot;
    const double s = sRoot * sRoot * sRoot;
    return {
        4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s
    };
}

FloatPixel compressLinearRgbToGamut(const FloatPixel &neutral,
                                    const FloatPixel &target)
{
    double scale = 1.0;
    const auto constrain = [&scale](const double origin, const double destination) {
        const double delta = destination - origin;
        if (delta > 0.0) {
            scale = std::min(scale, (1.0 - origin) / delta);
        } else if (delta < 0.0) {
            scale = std::min(scale, -origin / delta);
        }
    };
    constrain(neutral.r, target.r);
    constrain(neutral.g, target.g);
    constrain(neutral.b, target.b);
    scale = std::clamp(scale, 0.0, 1.0);
    return {neutral.r + (target.r - neutral.r) * scale,
            neutral.g + (target.g - neutral.g) * scale,
            neutral.b + (target.b - neutral.b) * scale};
}

FloatPixel applyPerceptualSaturation(const FloatPixel &input, const double saturation)
{
    const FloatPixel lab = linearToOklab(linearRgb(input));
    const double requested = std::max(0.0, 1.0 + saturation / 100.0);
    const FloatPixel neutral = oklabToLinear({lab.r, 0.0, 0.0});
    const FloatPixel target = oklabToLinear(
        {lab.r, lab.g * requested, lab.b * requested});
    return encodedRgb(compressLinearRgbToGamut(neutral, target));
}

double wrapDegrees(double value)
{
    value = std::fmod(value, 360.0);
    return value < 0.0 ? value + 360.0 : value;
}

double hueDistanceDegrees(const double left, const double right)
{
    const double difference = std::abs(wrapDegrees(left) - wrapDegrees(right));
    return std::min(difference, 360.0 - difference);
}

FloatPixel rgbToHsl(const FloatPixel &rgb)
{
    const double maximum = std::max({rgb.r, rgb.g, rgb.b});
    const double minimum = std::min({rgb.r, rgb.g, rgb.b});
    const double chroma = maximum - minimum;
    const double lightness = (maximum + minimum) * 0.5;
    double hue = 0.0;
    if (chroma > 1.0e-12) {
        if (maximum == rgb.r) hue = 60.0 * std::fmod((rgb.g - rgb.b) / chroma, 6.0);
        else if (maximum == rgb.g) hue = 60.0 * ((rgb.b - rgb.r) / chroma + 2.0);
        else hue = 60.0 * ((rgb.r - rgb.g) / chroma + 4.0);
    }
    const double saturation = chroma <= 1.0e-12
        ? 0.0 : chroma / std::max(1.0e-12, 1.0 - std::abs(2.0 * lightness - 1.0));
    return {wrapDegrees(hue), clamp01(saturation), clamp01(lightness)};
}

FloatPixel hslToRgb(const FloatPixel &hsl)
{
    const double hue = wrapDegrees(hsl.r);
    const double saturation = clamp01(hsl.g);
    const double lightness = clamp01(hsl.b);
    const double chroma = (1.0 - std::abs(2.0 * lightness - 1.0)) * saturation;
    const double x = chroma * (1.0 - std::abs(std::fmod(hue / 60.0, 2.0) - 1.0));
    FloatPixel rgb {0.0, 0.0, 0.0};
    if (hue < 60.0) rgb = {chroma, x, 0.0};
    else if (hue < 120.0) rgb = {x, chroma, 0.0};
    else if (hue < 180.0) rgb = {0.0, chroma, x};
    else if (hue < 240.0) rgb = {0.0, x, chroma};
    else if (hue < 300.0) rgb = {x, 0.0, chroma};
    else rgb = {chroma, 0.0, x};
    const double match = lightness - chroma * 0.5;
    return {clamp01(rgb.r + match), clamp01(rgb.g + match), clamp01(rgb.b + match)};
}

double hueRangeWeight(const double hue, const HueSaturationRangeParameters &range)
{
    const double distance = hueDistanceDegrees(hue, range.centre);
    const double inner = range.width * 0.5;
    if (distance <= inner) return 1.0;
    if (range.feather <= 1.0e-12 || distance >= inner + range.feather) return 0.0;
    const double t = (distance - inner) / range.feather;
    return t * t * (2.0 * t - 3.0) + 1.0;
}

FloatPixel applyHueSaturation(const FloatPixel &input,
                              const HueSaturationParameters &parameters)
{
    FloatPixel hsl = rgbToHsl(input);
    double hueShift = parameters.hue;
    double saturation = parameters.saturation;
    double lightness = parameters.lightness;
    for (const HueSaturationRangeParameters &range : parameters.ranges) {
        const double weight = hueRangeWeight(hsl.r, range);
        hueShift += range.hue * weight;
        saturation += range.saturation * weight;
        lightness += range.lightness * weight;
    }
    hsl.r = wrapDegrees(hsl.r + hueShift);
    const double saturationAmount = saturation / 100.0;
    hsl.g = saturationAmount >= 0.0
        ? hsl.g + (1.0 - hsl.g) * saturationAmount
        : hsl.g * (1.0 + saturationAmount);
    const double lightnessAmount = lightness / 100.0;
    hsl.b = lightnessAmount >= 0.0
        ? hsl.b + (1.0 - hsl.b) * lightnessAmount
        : hsl.b * (1.0 + lightnessAmount);
    return hslToRgb(hsl);
}

FloatPixel applyVibrance(const FloatPixel &input,
                         const VibranceParameters &parameters)
{
    const FloatPixel linear = linearRgb(input);
    const FloatPixel lab = linearToOklab(linear);
    const double chroma = std::hypot(lab.g, lab.b);
    const FloatPixel hsl = rgbToHsl(input);
    const double skinDistance = hueDistanceDegrees(hsl.r, 32.0);
    const double skinWeight = std::clamp(1.0 - skinDistance / 42.0, 0.0, 1.0)
        * std::clamp((hsl.g - 0.08) / 0.42, 0.0, 1.0);
    const double protection = parameters.skinProtection / 100.0;
    const double adaptive = parameters.vibrance >= 0.0
        ? (1.0 - std::clamp(chroma / 0.32, 0.0, 1.0))
            * (1.0 - skinWeight * protection)
        : 1.0;
    const double multiplier = std::max(0.0,
        (1.0 + parameters.saturation / 100.0)
        * (1.0 + parameters.vibrance / 100.0 * adaptive));
    const FloatPixel neutral = oklabToLinear({lab.r, 0.0, 0.0});
    const FloatPixel target = oklabToLinear({lab.r, lab.g * multiplier, lab.b * multiplier});
    return encodedRgb(compressLinearRgbToGamut(neutral, target));
}

FloatPixel applyPhotoFilter(const FloatPixel &input,
                            const PhotoFilterParameters &parameters)
{
    const double density = std::clamp(parameters.density / 100.0, 0.0, 1.0);
    if (density <= 1.0e-12) return input;

    const FloatPixel filterEncoded {parameters.colour.redF(),
                                    parameters.colour.greenF(),
                                    parameters.colour.blueF()};
    const FloatPixel filterLinear = linearRgb(filterEncoded);
    const double maximum = std::max({filterLinear.r, filterLinear.g, filterLinear.b,
                                     1.0e-6});
    FloatPixel scale {0.12 + 0.88 * filterLinear.r / maximum,
                      0.12 + 0.88 * filterLinear.g / maximum,
                      0.12 + 0.88 * filterLinear.b / maximum};
    if (parameters.preserveLuminosity) {
        const double luminance = std::max(1.0e-6,
            scale.r * 0.2126 + scale.g * 0.7152 + scale.b * 0.0722);
        scale.r /= luminance;
        scale.g /= luminance;
        scale.b /= luminance;
    }

    scale.r = std::pow(std::max(scale.r, 1.0e-6), density);
    scale.g = std::pow(std::max(scale.g, 1.0e-6), density);
    scale.b = std::pow(std::max(scale.b, 1.0e-6), density);
    const FloatPixel linear = linearRgb(input);
    const FloatPixel target {linear.r * scale.r,
                             linear.g * scale.g,
                             linear.b * scale.b};
    return encodedRgb(compressLinearRgbToGamut(linear, target));
}

FloatPixel applyWhiteBalance(const FloatPixel &input,
                             const WhiteBalanceParameters &parameters)
{
    const double temperature = parameters.temperature / 100.0;
    const double tint = parameters.tint / 100.0;
    double redScale = std::exp2(temperature * 0.55 - tint * 0.04);
    double greenScale = std::exp2(-std::abs(temperature) * 0.04 - tint * 0.30);
    double blueScale = std::exp2(-temperature * 0.55 - tint * 0.04);
    const double normalisation = 0.2126 * redScale + 0.7152 * greenScale + 0.0722 * blueScale;
    redScale /= normalisation;
    greenScale /= normalisation;
    blueScale /= normalisation;
    const FloatPixel linear = linearRgb(input);
    return encodedRgb({linear.r * redScale, linear.g * greenScale, linear.b * blueScale});
}

FloatPixel applyColourBalance(const FloatPixel &input,
                              const ColourBalanceParameters &parameters)
{
    const FloatPixel linear = linearRgb(input);
    const double luminance = linear.r * 0.2126 + linear.g * 0.7152 + linear.b * 0.0722;
    const double shadows = std::clamp((0.55 - luminance) / 0.55, 0.0, 1.0);
    const double highlights = std::clamp((luminance - 0.45) / 0.55, 0.0, 1.0);
    const double midtones = std::clamp(1.0 - std::max(shadows, highlights), 0.0, 1.0);
    const std::array<double, 3> weights {shadows, midtones, highlights};
    FloatPixel shift {0.0, 0.0, 0.0};
    for (std::size_t index = 0; index < parameters.ranges.size(); ++index) {
        const ColourBalanceRangeParameters &range = parameters.ranges[index];
        const double weight = weights[index] * 0.28 / 100.0;
        shift.r += weight * (range.cyanRed - 0.5 * range.magentaGreen - 0.5 * range.yellowBlue);
        shift.g += weight * (range.magentaGreen - 0.5 * range.cyanRed - 0.5 * range.yellowBlue);
        shift.b += weight * (range.yellowBlue - 0.5 * range.cyanRed - 0.5 * range.magentaGreen);
    }
    FloatPixel adjusted {linear.r + shift.r, linear.g + shift.g, linear.b + shift.b};
    if (parameters.preserveLuminosity) {
        const double adjustedLuminance = adjusted.r * 0.2126 + adjusted.g * 0.7152 + adjusted.b * 0.0722;
        const double correction = luminance - adjustedLuminance;
        adjusted.r += correction;
        adjusted.g += correction;
        adjusted.b += correction;
    }
    const FloatPixel neutral {luminance, luminance, luminance};
    return encodedRgb(compressLinearRgbToGamut(neutral, adjusted));
}


FloatPixel applyChannelMixer(const FloatPixel &input,
                             const ChannelMixerParameters &parameters)
{
    const auto mix = [&input](const ChannelMixerChannelParameters &channel) {
        return clamp01((input.r * channel.red
                        + input.g * channel.green
                        + input.b * channel.blue) / 100.0
                       + channel.constant / 100.0);
    };
    if (parameters.monochromeEnabled) {
        const double grey = mix(parameters.monochrome);
        return {grey, grey, grey};
    }
    return {mix(parameters.outputs[0]),
            mix(parameters.outputs[1]),
            mix(parameters.outputs[2])};
}

double colourFamilyHueWeight(const double hue, const double centre)
{
    return std::max(0.0, 1.0 - hueDistanceDegrees(hue, centre) / 60.0);
}

std::array<double, 6> chromaticFamilyWeights(const double hue)
{
    constexpr std::array<double, 6> centres {0.0, 60.0, 120.0, 180.0, 240.0, 300.0};
    std::array<double, 6> weights {};
    for (std::size_t index = 0; index < centres.size(); ++index) {
        weights[index] = colourFamilyHueWeight(hue, centres[index]);
    }
    return weights;
}

FloatPixel applyBlackAndWhite(const FloatPixel &input,
                              const BlackAndWhiteParameters &parameters)
{
    const FloatPixel hsl = rgbToHsl(input);
    const std::array<double, 6> weights = chromaticFamilyWeights(hsl.r);
    double familyScale = 0.0;
    double totalWeight = 0.0;
    for (std::size_t index = 0; index < weights.size(); ++index) {
        familyScale += weights[index] * parameters.colourWeights[index] / 100.0;
        totalWeight += weights[index];
    }
    if (totalWeight > 1.0e-12) familyScale /= totalWeight;
    else familyScale = 1.0;
    const double encodedLuminance = input.r * 0.2126 + input.g * 0.7152 + input.b * 0.0722;
    const double chromaWeight = hsl.g;
    const double grey = clamp01(encodedLuminance
                                * (1.0 + (familyScale - 1.0) * chromaWeight));
    if (!parameters.tintEnabled || parameters.tintSaturation <= 1.0e-12) {
        return {grey, grey, grey};
    }
    return hslToRgb({parameters.tintHue, parameters.tintSaturation / 100.0, grey});
}

double selectiveColourRangeScale(const FloatPixel &input,
                                 const std::size_t rangeIndex)
{
    const double minimum = std::min({input.r, input.g, input.b});
    const double maximum = std::max({input.r, input.g, input.b});
    const double middle = input.r + input.g + input.b - minimum - maximum;
    switch (static_cast<SelectiveColourRange>(rangeIndex)) {
    case SelectiveColourRange::Reds:
        return input.r == maximum ? maximum - middle : 0.0;
    case SelectiveColourRange::Yellows:
        return input.b == minimum ? middle - minimum : 0.0;
    case SelectiveColourRange::Greens:
        return input.g == maximum ? maximum - middle : 0.0;
    case SelectiveColourRange::Cyans:
        return input.r == minimum ? middle - minimum : 0.0;
    case SelectiveColourRange::Blues:
        return input.b == maximum ? maximum - middle : 0.0;
    case SelectiveColourRange::Magentas:
        return input.g == minimum ? middle - minimum : 0.0;
    case SelectiveColourRange::Whites:
        return input.r > 0.5 && input.g > 0.5 && input.b > 0.5
            ? (minimum - 0.5) * 2.0 : 0.0;
    case SelectiveColourRange::Neutrals:
        if (maximum <= 0.0 || minimum >= 1.0) return 0.0;
        return std::max(0.0, 1.0
            - (std::abs(maximum - 0.5) + std::abs(minimum - 0.5)));
    case SelectiveColourRange::Blacks:
        return input.r < 0.5 && input.g < 0.5 && input.b < 0.5
            ? (0.5 - maximum) * 2.0 : 0.0;
    }
    return 0.0;
}

double selectiveColourComponentAdjustment(const double value,
                                          const double colourAdjustment,
                                          const double blackAdjustment,
                                          const SelectiveColourMethod method)
{
    const double adjustment = colourAdjustment / 100.0;
    const double black = blackAdjustment / 100.0;
    double delta = (-1.0 - adjustment) * black - adjustment;
    if (method == SelectiveColourMethod::Relative) {
        delta *= 1.0 - value;
    }
    return std::clamp(delta, -value, 1.0 - value);
}

FloatPixel applySelectiveColour(const FloatPixel &input,
                                const SelectiveColourParameters &parameters)
{
    FloatPixel delta {0.0, 0.0, 0.0};
    for (std::size_t index = 0; index < parameters.ranges.size(); ++index) {
        const double scale = selectiveColourRangeScale(input, index);
        if (scale <= 1.0e-12) continue;
        const SelectiveColourRangeParameters &range = parameters.ranges[index];
        delta.r += scale * selectiveColourComponentAdjustment(
            input.r, range.cyan, range.black, parameters.method);
        delta.g += scale * selectiveColourComponentAdjustment(
            input.g, range.magenta, range.black, parameters.method);
        delta.b += scale * selectiveColourComponentAdjustment(
            input.b, range.yellow, range.black, parameters.method);
    }
    return {clamp01(input.r + delta.r),
            clamp01(input.g + delta.g),
            clamp01(input.b + delta.b)};
}

QImage::Format workingFormat(const QImage &source);

double smoothStep(const double edge0, const double edge1, const double value)
{
    if (edge1 <= edge0 + 1.0e-12) return value >= edge1 ? 1.0 : 0.0;
    const double t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

FloatPixel applyShadowsHighlightsPixel(const FloatPixel &input,
                                       const double localLuminance,
                                       const ShadowsHighlightsParameters &parameters)
{
    const FloatPixel linear = linearRgb(input);
    const double luminance = std::clamp(linear.r * 0.2126
                                        + linear.g * 0.7152
                                        + linear.b * 0.0722,
                                        0.0, 1.0);
    const double shadowLimit = 0.05 + 0.90 * parameters.shadowTonalWidth / 100.0;
    const double highlightLimit = 0.05 + 0.90 * parameters.highlightTonalWidth / 100.0;
    const double shadowWeight = 1.0 - smoothStep(0.0, shadowLimit, localLuminance);
    const double highlightWeight = smoothStep(1.0 - highlightLimit, 1.0, localLuminance);

    double adjustedLuminance = luminance
        + parameters.shadowAmount / 100.0 * shadowWeight * (1.0 - luminance) * 0.85;
    adjustedLuminance -= parameters.highlightAmount / 100.0
        * highlightWeight * adjustedLuminance * 0.65;

    double midtoneWeight = std::max(0.0, 1.0 - std::abs(2.0 * localLuminance - 1.0));
    midtoneWeight *= midtoneWeight;
    const double contrastScale = std::max(0.05,
        1.0 + parameters.midtoneContrast / 100.0 * 0.65 * midtoneWeight);
    adjustedLuminance = clamp01((adjustedLuminance - 0.5) * contrastScale + 0.5);

    FloatPixel adjustedLinear;
    if (luminance > 1.0e-9) {
        const double scale = adjustedLuminance / luminance;
        adjustedLinear = {linear.r * scale, linear.g * scale, linear.b * scale};
    } else {
        adjustedLinear = {adjustedLuminance, adjustedLuminance, adjustedLuminance};
    }
    FloatPixel result = encodedRgb({clamp01(adjustedLinear.r),
                                    clamp01(adjustedLinear.g),
                                    clamp01(adjustedLinear.b)});
    if (parameters.colourCorrection > 0.0) {
        FloatPixel hsl = rgbToHsl(result);
        const double correction = parameters.colourCorrection / 100.0
            * std::abs(adjustedLuminance - luminance) * 1.5;
        hsl.g = clamp01(hsl.g * (1.0 + correction));
        result = hslToRgb(hsl);
    }
    return result;
}

QImage applyShadowsHighlightsToImage(const QImage &input,
                                     ShadowsHighlightsParameters parameters,
                                     const std::atomic_bool *cancelRequested)
{
    if (input.isNull()) return {};
    parameters.normalise();
    if (std::abs(parameters.shadowAmount) <= 1.0e-12
        && std::abs(parameters.highlightAmount) <= 1.0e-12
        && std::abs(parameters.midtoneContrast) <= 1.0e-12) {
        return input;
    }
    const bool useSixteenBit = input.depth() > 32;
    QImage output = input.convertToFormat(useSixteenBit ? QImage::Format_RGBA64
                                                        : QImage::Format_RGBA8888);
    output.setColorSpace(input.colorSpace());
    const int width = output.width();
    const int height = output.height();
    if (width <= 0 || height <= 0) return {};

    const qsizetype pixelCount = static_cast<qsizetype>(width) * height;
    // Two f32 planes keep large exact-CPU fallback renders bounded while
    // retaining substantially more precision than one 16-bit code step.
    QVector<float> luminance(pixelCount);
    if (useSixteenBit) {
        const QImage straight = output.convertToFormat(QImage::Format_RGBA64);
        processRows(width, height, [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            const auto *row = reinterpret_cast<const QRgba64 *>(straight.constScanLine(y));
            for (int x = 0; x < width; ++x) {
                const FloatPixel linear = linearRgb({row[x].red() / 65535.0,
                                                     row[x].green() / 65535.0,
                                                     row[x].blue() / 65535.0});
                luminance[static_cast<qsizetype>(y) * width + x] = clamp01(
                    linear.r * 0.2126 + linear.g * 0.7152 + linear.b * 0.0722);
            }
        });
    } else {
        const QImage straight = output.convertToFormat(QImage::Format_RGBA8888);
        processRows(width, height, [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            const uchar *row = straight.constScanLine(y);
            for (int x = 0; x < width; ++x) {
                const uchar *rgba = row + x * 4;
                const FloatPixel linear = linearRgb({rgba[0] / 255.0,
                                                     rgba[1] / 255.0,
                                                     rgba[2] / 255.0});
                luminance[static_cast<qsizetype>(y) * width + x] = clamp01(
                    linear.r * 0.2126 + linear.g * 0.7152 + linear.b * 0.0722);
            }
        });
    }
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};

    const int radius = std::clamp(qRound(parameters.radius), 1, 500);
    // The shared 0.13.0a spatial foundation owns the deterministic 13-tap
    // separable reference. The approved WGSL path uses the same offsets,
    // weights, clamp edge mode and RGBA8 horizontal quantisation contract.
    if (!SpatialFilterFoundation::blurSparseThirteenTap(
            &luminance, width, height, QSize(radius, radius),
            SpatialEdgeMode::Clamp, !useSixteenBit, cancelRequested,
            parallelSpatialRows())) {
        return {};
    }

    uchar *const base = output.bits();
    const qsizetype stride = output.bytesPerLine();
    if (useSixteenBit) {
        processRows(width, height, [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            auto *row = reinterpret_cast<QRgba64 *>(base + static_cast<qsizetype>(y) * stride);
            for (int x = 0; x < width; ++x) {
                const QRgba64 original = row[x];
                const FloatPixel result = applyShadowsHighlightsPixel(
                    {original.red() / 65535.0, original.green() / 65535.0, original.blue() / 65535.0},
                    luminance[static_cast<qsizetype>(y) * width + x], parameters);
                row[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::lround(clamp01(result.r) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(result.g) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(result.b) * 65535.0)),
                    original.alpha());
            }
        });
    } else {
        processRows(width, height, [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            uchar *row = base + static_cast<qsizetype>(y) * stride;
            for (int x = 0; x < width; ++x) {
                uchar *rgba = row + x * 4;
                const FloatPixel result = applyShadowsHighlightsPixel(
                    {rgba[0] / 255.0, rgba[1] / 255.0, rgba[2] / 255.0},
                    luminance[static_cast<qsizetype>(y) * width + x], parameters);
                rgba[0] = static_cast<uchar>(std::lround(clamp01(result.r) * 255.0));
                rgba[1] = static_cast<uchar>(std::lround(clamp01(result.g) * 255.0));
                rgba[2] = static_cast<uchar>(std::lround(clamp01(result.b) * 255.0));
            }
        });
    }
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    return output.convertToFormat(input.depth() > 32
        ? QImage::Format_RGBA64_Premultiplied
        : QImage::Format_ARGB32_Premultiplied);
}


QImage applyGaussianBlurToImage(const QImage &input,
                                GaussianBlurParameters parameters,
                                const std::atomic_bool *cancelRequested)
{
    parameters.normalise();
    const int radius = std::clamp(static_cast<int>(std::ceil(parameters.radius)), 0, 500);
    if (radius <= 0) return input;
    QImage adjusted = SpatialFilterFoundation::gaussianBlurReference(
        input, QSize(radius, radius), SpatialEdgeMode::Clamp,
        parameters.affectAlpha ? SpatialAlphaMode::CoverageAwareRgba
                               : SpatialAlphaMode::PreserveSourceAlpha,
        cancelRequested, parallelSpatialRows());
    if (adjusted.isNull()) return {};
    adjusted.setColorSpace(input.colorSpace());
    return adjusted.convertToFormat(input.depth() > 32
        ? QImage::Format_RGBA64_Premultiplied
        : QImage::Format_ARGB32_Premultiplied);
}

QImage applyBoxBlurToImage(const QImage &input,
                           BoxBlurParameters parameters,
                           const std::atomic_bool *cancelRequested)
{
    parameters.normalise();
    const int radius = std::clamp(static_cast<int>(std::ceil(parameters.radius)), 0, 500);
    if (radius <= 0) return input;
    QImage adjusted = SpatialFilterFoundation::boxBlurReference(
        input, QSize(radius, radius), SpatialEdgeMode::Clamp,
        parameters.affectAlpha ? SpatialAlphaMode::CoverageAwareRgba
                               : SpatialAlphaMode::PreserveSourceAlpha,
        cancelRequested, parallelSpatialRows());
    if (adjusted.isNull()) return {};
    adjusted.setColorSpace(input.colorSpace());
    return adjusted.convertToFormat(input.depth() > 32
        ? QImage::Format_RGBA64_Premultiplied
        : QImage::Format_ARGB32_Premultiplied);
}

struct FloatRgbaPixel {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;
};

QImage applyVariableDirectionalBlur(
    const QImage &input,
    const int requestedSamples,
    const bool affectAlpha,
    const QPoint previewOrigin,
    const std::function<QPointF(double, double)> &spanForPreviewPixel,
    const std::atomic_bool *cancelRequested)
{
    if (input.isNull() || !spanForPreviewPixel) return {};
    const bool sixteenBit = input.depth() > 32;
    const QImage::Format format = sixteenBit ? QImage::Format_RGBA64
                                             : QImage::Format_RGBA8888;
    const QImage source = input.convertToFormat(format);
    QImage output(source.size(), format);
    if (source.isNull() || output.isNull()) return {};
    output.setColorSpace(input.colorSpace());
    const int width = source.width();
    const int height = source.height();
    const int maximumSamples = std::clamp(requestedSamples, 4, 64);
    const uchar *const sourceBits = source.constBits();
    uchar *const outputBits = output.bits();
    const qsizetype sourceStride = source.bytesPerLine();
    const qsizetype outputStride = output.bytesPerLine();

    const auto sample8 = [&](double x, double y) {
        x = std::clamp(x, 0.0, static_cast<double>(std::max(0, width - 1)));
        y = std::clamp(y, 0.0, static_cast<double>(std::max(0, height - 1)));
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const int x1 = std::min(width - 1, x0 + 1);
        const int y1 = std::min(height - 1, y0 + 1);
        const double fx = x - x0;
        const double fy = y - y0;
        const uchar *p00 = sourceBits + static_cast<qsizetype>(y0) * sourceStride + x0 * 4;
        const uchar *p10 = sourceBits + static_cast<qsizetype>(y0) * sourceStride + x1 * 4;
        const uchar *p01 = sourceBits + static_cast<qsizetype>(y1) * sourceStride + x0 * 4;
        const uchar *p11 = sourceBits + static_cast<qsizetype>(y1) * sourceStride + x1 * 4;
        const auto channel = [&](const int c) {
            const double top = p00[c] + (p10[c] - p00[c]) * fx;
            const double bottom = p01[c] + (p11[c] - p01[c]) * fx;
            return (top + (bottom - top) * fy) / 255.0;
        };
        return FloatRgbaPixel {channel(0), channel(1), channel(2), channel(3)};
    };
    const auto sample16 = [&](double x, double y) {
        x = std::clamp(x, 0.0, static_cast<double>(std::max(0, width - 1)));
        y = std::clamp(y, 0.0, static_cast<double>(std::max(0, height - 1)));
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const int x1 = std::min(width - 1, x0 + 1);
        const int y1 = std::min(height - 1, y0 + 1);
        const double fx = x - x0;
        const double fy = y - y0;
        const auto *row0 = reinterpret_cast<const QRgba64 *>(
            sourceBits + static_cast<qsizetype>(y0) * sourceStride);
        const auto *row1 = reinterpret_cast<const QRgba64 *>(
            sourceBits + static_cast<qsizetype>(y1) * sourceStride);
        const QRgba64 p00 = row0[x0];
        const QRgba64 p10 = row0[x1];
        const QRgba64 p01 = row1[x0];
        const QRgba64 p11 = row1[x1];
        const auto interpolate = [&](const double a00, const double a10,
                                     const double a01, const double a11) {
            const double top = a00 + (a10 - a00) * fx;
            const double bottom = a01 + (a11 - a01) * fx;
            return (top + (bottom - top) * fy) / 65535.0;
        };
        return FloatRgbaPixel {
            interpolate(p00.red(), p10.red(), p01.red(), p11.red()),
            interpolate(p00.green(), p10.green(), p01.green(), p11.green()),
            interpolate(p00.blue(), p10.blue(), p01.blue(), p11.blue()),
            interpolate(p00.alpha(), p10.alpha(), p01.alpha(), p11.alpha())};
    };

    processRows(width, height, [&](const int y) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
        uchar *destinationBytes = outputBits + static_cast<qsizetype>(y) * outputStride;
        auto *destination16 = sixteenBit
            ? reinterpret_cast<QRgba64 *>(destinationBytes) : nullptr;
        const auto *source16 = sixteenBit
            ? reinterpret_cast<const QRgba64 *>(sourceBits + static_cast<qsizetype>(y) * sourceStride)
            : nullptr;
        const uchar *source8 = !sixteenBit
            ? sourceBits + static_cast<qsizetype>(y) * sourceStride : nullptr;
        for (int x = 0; x < width; ++x) {
            if ((x & 63) == 0 && cancelRequested
                && cancelRequested->load(std::memory_order_relaxed)) return;
            const QPointF span = spanForPreviewPixel(
                previewOrigin.x() + x + 0.5, previewOrigin.y() + y + 0.5);
            const double spanLength = std::hypot(span.x(), span.y());
            if (!std::isfinite(spanLength) || spanLength <= 1.0e-9) {
                if (sixteenBit) destination16[x] = source16[x];
                else std::memcpy(destinationBytes + x * 4, source8 + x * 4, 4);
                continue;
            }
            const int samples = std::clamp(
                static_cast<int>(std::ceil(spanLength)) + 1, 4, maximumSamples);
            double sumStraightR = 0.0;
            double sumStraightG = 0.0;
            double sumStraightB = 0.0;
            double sumPremultipliedR = 0.0;
            double sumPremultipliedG = 0.0;
            double sumPremultipliedB = 0.0;
            double sumAlpha = 0.0;
            for (int sampleIndex = 0; sampleIndex < samples; ++sampleIndex) {
                const double t = samples <= 1 ? 0.0
                    : sampleIndex / static_cast<double>(samples - 1) - 0.5;
                const double sampleX = x + t * span.x();
                const double sampleY = y + t * span.y();
                const FloatRgbaPixel sample = sixteenBit
                    ? sample16(sampleX, sampleY) : sample8(sampleX, sampleY);
                sumStraightR += sample.r;
                sumStraightG += sample.g;
                sumStraightB += sample.b;
                sumAlpha += sample.a;
                sumPremultipliedR += sample.r * sample.a;
                sumPremultipliedG += sample.g * sample.a;
                sumPremultipliedB += sample.b * sample.a;
            }
            const double inverseCount = 1.0 / samples;
            double red = sumStraightR * inverseCount;
            double green = sumStraightG * inverseCount;
            double blue = sumStraightB * inverseCount;
            double alpha = sixteenBit ? source16[x].alpha() / 65535.0
                                      : source8[x * 4 + 3] / 255.0;
            if (affectAlpha) {
                alpha = sumAlpha * inverseCount;
                if (sumAlpha > 1.0e-12) {
                    red = sumPremultipliedR / sumAlpha;
                    green = sumPremultipliedG / sumAlpha;
                    blue = sumPremultipliedB / sumAlpha;
                }
            }
            if (sixteenBit) {
                destination16[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::lround(clamp01(red) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(green) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(blue) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(alpha) * 65535.0)));
            } else {
                uchar *destination = destinationBytes + x * 4;
                destination[0] = static_cast<uchar>(std::lround(clamp01(red) * 255.0));
                destination[1] = static_cast<uchar>(std::lround(clamp01(green) * 255.0));
                destination[2] = static_cast<uchar>(std::lround(clamp01(blue) * 255.0));
                destination[3] = static_cast<uchar>(std::lround(clamp01(alpha) * 255.0));
            }
        }
    });
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    return output.convertToFormat(workingFormat(input));
}

QImage applySurfaceBlurToImage(const QImage &input,
                               SurfaceBlurParameters parameters,
                               const std::atomic_bool *cancelRequested)
{
    parameters.normalise();
    const int radius = std::clamp(static_cast<int>(std::ceil(parameters.radius)), 0, 250);
    const double threshold = parameters.threshold / 255.0;
    if (radius <= 0 || threshold <= 1.0e-12) return input;
    const bool sixteenBit = input.depth() > 32;
    const QImage::Format format = sixteenBit ? QImage::Format_RGBA64
                                             : QImage::Format_RGBA8888;
    const QImage original = input.convertToFormat(format);
    const QImage blurred = SpatialFilterFoundation::gaussianBlurReference(
        original, QSize(radius, radius), SpatialEdgeMode::Clamp,
        SpatialAlphaMode::PreserveSourceAlpha, cancelRequested,
        parallelSpatialRows());
    if (original.isNull() || blurred.isNull()) return {};
    QImage output = original.copy();
    uchar *const outputBits = output.bits();
    const uchar *const originalBits = original.constBits();
    const uchar *const blurredBits = blurred.constBits();
    const qsizetype outputStride = output.bytesPerLine();
    const qsizetype originalStride = original.bytesPerLine();
    const qsizetype blurredStride = blurred.bytesPerLine();
    const double lower = threshold * 0.25;
    if (sixteenBit) {
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            const auto *source = reinterpret_cast<const QRgba64 *>(
                originalBits + static_cast<qsizetype>(y) * originalStride);
            const auto *soft = reinterpret_cast<const QRgba64 *>(
                blurredBits + static_cast<qsizetype>(y) * blurredStride);
            auto *destination = reinterpret_cast<QRgba64 *>(
                outputBits + static_cast<qsizetype>(y) * outputStride);
            for (int x = 0; x < output.width(); ++x) {
                const double sr = source[x].red() / 65535.0;
                const double sg = source[x].green() / 65535.0;
                const double sb = source[x].blue() / 65535.0;
                const double br = soft[x].red() / 65535.0;
                const double bg = soft[x].green() / 65535.0;
                const double bb = soft[x].blue() / 65535.0;
                const double difference = std::max({std::abs(sr - br), std::abs(sg - bg), std::abs(sb - bb)});
                const double weight = 1.0 - smoothStep(lower, threshold, difference);
                destination[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::lround(clamp01(sr + (br - sr) * weight) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(sg + (bg - sg) * weight) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(sb + (bb - sb) * weight) * 65535.0)),
                    source[x].alpha());
            }
        });
    } else {
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            const uchar *source = originalBits + static_cast<qsizetype>(y) * originalStride;
            const uchar *soft = blurredBits + static_cast<qsizetype>(y) * blurredStride;
            uchar *destination = outputBits + static_cast<qsizetype>(y) * outputStride;
            for (int x = 0; x < output.width(); ++x) {
                const int offset = x * 4;
                const double sr = source[offset] / 255.0;
                const double sg = source[offset + 1] / 255.0;
                const double sb = source[offset + 2] / 255.0;
                const double br = soft[offset] / 255.0;
                const double bg = soft[offset + 1] / 255.0;
                const double bb = soft[offset + 2] / 255.0;
                const double difference = std::max({std::abs(sr - br), std::abs(sg - bg), std::abs(sb - bb)});
                const double weight = 1.0 - smoothStep(lower, threshold, difference);
                destination[offset] = static_cast<uchar>(std::lround(clamp01(sr + (br - sr) * weight) * 255.0));
                destination[offset + 1] = static_cast<uchar>(std::lround(clamp01(sg + (bg - sg) * weight) * 255.0));
                destination[offset + 2] = static_cast<uchar>(std::lround(clamp01(sb + (bb - sb) * weight) * 255.0));
                destination[offset + 3] = source[offset + 3];
            }
        });
    }
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    output.setColorSpace(input.colorSpace());
    return output.convertToFormat(workingFormat(input));
}

QImage applyMotionBlurToImage(const QImage &input,
                              MotionBlurParameters parameters,
                              const double spatialScale,
                              const QPoint previewOrigin,
                              const std::atomic_bool *cancelRequested)
{
    parameters.normalise();
    const double distance = parameters.distance * std::max(0.0001, spatialScale);
    if (distance <= 1.0e-12) return input;
    const double radians = parameters.angle * std::numbers::pi / 180.0;
    const QPointF span(std::cos(radians) * distance, std::sin(radians) * distance);
    return applyVariableDirectionalBlur(
        input, parameters.samples, parameters.affectAlpha, previewOrigin,
        [span](double, double) { return span; }, cancelRequested);
}

QImage applyRadialBlurToImage(const QImage &input,
                              RadialBlurParameters parameters,
                              const double spatialScale,
                              const QPoint previewOrigin,
                              const QSize previewSize,
                              const std::atomic_bool *cancelRequested)
{
    parameters.normalise();
    const double amount = parameters.amount * std::max(0.0001, spatialScale);
    if (amount <= 1.0e-12 || previewSize.isEmpty()) return input;
    const double centreX = (0.5 + parameters.centreX / 200.0)
        * std::max(0, previewSize.width() - 1);
    const double centreY = (0.5 + parameters.centreY / 200.0)
        * std::max(0, previewSize.height() - 1);
    const double maximumRadius = std::max({
        std::hypot(centreX, centreY),
        std::hypot(previewSize.width() - 1.0 - centreX, centreY),
        std::hypot(centreX, previewSize.height() - 1.0 - centreY),
        std::hypot(previewSize.width() - 1.0 - centreX,
                   previewSize.height() - 1.0 - centreY), 1.0});
    const RadialBlurMode mode = parameters.mode;
    return applyVariableDirectionalBlur(
        input, parameters.samples, parameters.affectAlpha, previewOrigin,
        [=](const double absoluteX, const double absoluteY) {
            const double dx = absoluteX - centreX;
            const double dy = absoluteY - centreY;
            const double radius = std::hypot(dx, dy);
            if (radius <= 1.0e-9) return QPointF();
            const double localSpan = amount * std::clamp(radius / maximumRadius, 0.0, 1.0);
            if (mode == RadialBlurMode::Zoom) {
                return QPointF(dx / radius * localSpan, dy / radius * localSpan);
            }
            return QPointF(-dy / radius * localSpan, dx / radius * localSpan);
        }, cancelRequested);
}

QImage applyUnsharpMaskToImage(const QImage &input,
                               UnsharpMaskParameters parameters,
                               const std::atomic_bool *cancelRequested)
{
    parameters.normalise();
    const int radius = std::clamp(static_cast<int>(std::ceil(parameters.radius)), 0, 500);
    if (radius <= 0 || parameters.amount <= 1.0e-12) return input;
    const bool sixteenBit = input.depth() > 32;
    const QImage::Format format = sixteenBit ? QImage::Format_RGBA64
                                             : QImage::Format_RGBA8888;
    const QImage original = input.convertToFormat(format);
    QImage blurred = SpatialFilterFoundation::gaussianBlurReference(
        original, QSize(radius, radius), SpatialEdgeMode::Clamp,
        SpatialAlphaMode::PreserveSourceAlpha, cancelRequested,
        parallelSpatialRows());
    if (original.isNull() || blurred.isNull()) return {};
    QImage output = original.copy();
    uchar *const outputBits = output.bits();
    const uchar *const originalBits = original.constBits();
    const uchar *const blurredBits = blurred.constBits();
    const qsizetype outputStride = output.bytesPerLine();
    const qsizetype originalStride = original.bytesPerLine();
    const qsizetype blurredStride = blurred.bytesPerLine();
    const double scale = parameters.amount / 100.0;
    const double threshold = parameters.threshold / 255.0;
    if (sixteenBit) {
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            const auto *sourceRow = reinterpret_cast<const QRgba64 *>(
                originalBits + static_cast<qsizetype>(y) * originalStride);
            const auto *blurRow = reinterpret_cast<const QRgba64 *>(
                blurredBits + static_cast<qsizetype>(y) * blurredStride);
            auto *destination = reinterpret_cast<QRgba64 *>(
                outputBits + static_cast<qsizetype>(y) * outputStride);
            for (int x = 0; x < output.width(); ++x) {
                const double originalR = sourceRow[x].red() / 65535.0;
                const double originalG = sourceRow[x].green() / 65535.0;
                const double originalB = sourceRow[x].blue() / 65535.0;
                const double deltaR = originalR - blurRow[x].red() / 65535.0;
                const double deltaG = originalG - blurRow[x].green() / 65535.0;
                const double deltaB = originalB - blurRow[x].blue() / 65535.0;
                const double magnitude = std::max({std::abs(deltaR), std::abs(deltaG),
                                                   std::abs(deltaB)});
                const double amount = magnitude + 1.0e-12 >= threshold ? scale : 0.0;
                destination[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::lround(clamp01(originalR + deltaR * amount) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(originalG + deltaG * amount) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(originalB + deltaB * amount) * 65535.0)),
                    sourceRow[x].alpha());
            }
        });
    } else {
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            const uchar *sourceRow = originalBits + static_cast<qsizetype>(y) * originalStride;
            const uchar *blurRow = blurredBits + static_cast<qsizetype>(y) * blurredStride;
            uchar *destination = outputBits + static_cast<qsizetype>(y) * outputStride;
            for (int x = 0; x < output.width(); ++x) {
                const int offset = x * 4;
                const double originalR = sourceRow[offset] / 255.0;
                const double originalG = sourceRow[offset + 1] / 255.0;
                const double originalB = sourceRow[offset + 2] / 255.0;
                const double deltaR = originalR - blurRow[offset] / 255.0;
                const double deltaG = originalG - blurRow[offset + 1] / 255.0;
                const double deltaB = originalB - blurRow[offset + 2] / 255.0;
                const double magnitude = std::max({std::abs(deltaR), std::abs(deltaG),
                                                   std::abs(deltaB)});
                const double amount = magnitude + 1.0e-12 >= threshold ? scale : 0.0;
                destination[offset] = static_cast<uchar>(std::lround(
                    clamp01(originalR + deltaR * amount) * 255.0));
                destination[offset + 1] = static_cast<uchar>(std::lround(
                    clamp01(originalG + deltaG * amount) * 255.0));
                destination[offset + 2] = static_cast<uchar>(std::lround(
                    clamp01(originalB + deltaB * amount) * 255.0));
                destination[offset + 3] = sourceRow[offset + 3];
            }
        });
    }
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    output.setColorSpace(input.colorSpace());
    return output.convertToFormat(sixteenBit ? QImage::Format_RGBA64_Premultiplied
                                             : QImage::Format_ARGB32_Premultiplied);
}

QImage applyHighPassToImage(const QImage &input,
                            HighPassParameters parameters,
                            const std::atomic_bool *cancelRequested)
{
    parameters.normalise();
    const int radius = std::clamp(static_cast<int>(std::ceil(parameters.radius)), 0, 500);
    const bool sixteenBit = input.depth() > 32;
    const QImage::Format format = sixteenBit ? QImage::Format_RGBA64
                                             : QImage::Format_RGBA8888;
    const QImage original = input.convertToFormat(format);
    if (original.isNull()) return {};
    QImage blurred = radius > 0
        ? SpatialFilterFoundation::gaussianBlurReference(
              original, QSize(radius, radius), SpatialEdgeMode::Clamp,
              SpatialAlphaMode::PreserveSourceAlpha, cancelRequested,
              parallelSpatialRows())
        : original;
    if (blurred.isNull()) return {};
    QImage output = original.copy();
    uchar *const outputBits = output.bits();
    const uchar *const originalBits = original.constBits();
    const uchar *const blurredBits = blurred.constBits();
    const qsizetype outputStride = output.bytesPerLine();
    const qsizetype originalStride = original.bytesPerLine();
    const qsizetype blurredStride = blurred.bytesPerLine();
    if (sixteenBit) {
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            const auto *sourceRow = reinterpret_cast<const QRgba64 *>(
                originalBits + static_cast<qsizetype>(y) * originalStride);
            const auto *blurRow = reinterpret_cast<const QRgba64 *>(
                blurredBits + static_cast<qsizetype>(y) * blurredStride);
            auto *destination = reinterpret_cast<QRgba64 *>(
                outputBits + static_cast<qsizetype>(y) * outputStride);
            for (int x = 0; x < output.width(); ++x) {
                const double originalR = sourceRow[x].red() / 65535.0;
                const double originalG = sourceRow[x].green() / 65535.0;
                const double originalB = sourceRow[x].blue() / 65535.0;
                const double blurR = blurRow[x].red() / 65535.0;
                const double blurG = blurRow[x].green() / 65535.0;
                const double blurB = blurRow[x].blue() / 65535.0;
                double r = 0.5 + originalR - blurR;
                double g = 0.5 + originalG - blurG;
                double b = 0.5 + originalB - blurB;
                if (parameters.monochrome) {
                    const double value = 0.5
                        + (originalR * 0.2126 + originalG * 0.7152 + originalB * 0.0722)
                        - (blurR * 0.2126 + blurG * 0.7152 + blurB * 0.0722);
                    r = g = b = value;
                }
                destination[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::lround(clamp01(r) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(g) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(b) * 65535.0)),
                    sourceRow[x].alpha());
            }
        });
    } else {
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            const uchar *sourceRow = originalBits + static_cast<qsizetype>(y) * originalStride;
            const uchar *blurRow = blurredBits + static_cast<qsizetype>(y) * blurredStride;
            uchar *destination = outputBits + static_cast<qsizetype>(y) * outputStride;
            for (int x = 0; x < output.width(); ++x) {
                const int offset = x * 4;
                const double originalR = sourceRow[offset] / 255.0;
                const double originalG = sourceRow[offset + 1] / 255.0;
                const double originalB = sourceRow[offset + 2] / 255.0;
                const double blurR = blurRow[offset] / 255.0;
                const double blurG = blurRow[offset + 1] / 255.0;
                const double blurB = blurRow[offset + 2] / 255.0;
                double r = 0.5 + originalR - blurR;
                double g = 0.5 + originalG - blurG;
                double b = 0.5 + originalB - blurB;
                if (parameters.monochrome) {
                    const double value = 0.5
                        + (originalR * 0.2126 + originalG * 0.7152 + originalB * 0.0722)
                        - (blurR * 0.2126 + blurG * 0.7152 + blurB * 0.0722);
                    r = g = b = value;
                }
                destination[offset] = static_cast<uchar>(std::lround(clamp01(r) * 255.0));
                destination[offset + 1] = static_cast<uchar>(std::lround(clamp01(g) * 255.0));
                destination[offset + 2] = static_cast<uchar>(std::lround(clamp01(b) * 255.0));
                destination[offset + 3] = sourceRow[offset + 3];
            }
        });
    }
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    output.setColorSpace(input.colorSpace());
    return output.convertToFormat(sixteenBit ? QImage::Format_RGBA64_Premultiplied
                                             : QImage::Format_ARGB32_Premultiplied);
}


QImage applyVignetteToImage(const QImage &input,
                            VignetteParameters parameters,
                            const QPoint previewOrigin,
                            const QSize previewSize,
                            const std::atomic_bool *cancelRequested)
{
    parameters.normalise();
    if (input.isNull()) return {};
    if (std::abs(parameters.amount) <= 1.0e-12) return input;

    const bool sixteenBit = input.depth() > 32;
    QImage output = input.convertToFormat(sixteenBit ? QImage::Format_RGBA64
                                                     : QImage::Format_RGBA8888);
    output.detach();
    uchar *const bits = output.bits();
    if (!bits) return {};
    const qsizetype stride = output.bytesPerLine();
    const double fullWidth = std::max(1, previewSize.width());
    const double fullHeight = std::max(1, previewSize.height());
    const double centreX = fullWidth * (0.5 + parameters.centreX / 200.0);
    const double centreY = fullHeight * (0.5 + parameters.centreY / 200.0);
    const double radians = parameters.rotation * std::numbers::pi / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double sizeScale = parameters.size / 100.0;
    const double halfWidth = std::max(0.5, fullWidth * 0.5 * sizeScale);
    const double halfHeight = std::max(0.5, fullHeight * 0.5 * sizeScale);
    const double halfMinimum = std::max(
        0.5, std::min(fullWidth, fullHeight) * 0.5 * sizeScale);
    const double circleMix = std::max(0.0, parameters.roundness) / 100.0;
    const double exponent = 2.0 + std::max(0.0, -parameters.roundness) * 0.06;
    const double start = parameters.midpoint / 100.0 * 0.9;
    const double featherFraction = parameters.feather / 100.0;
    const double minimumFeather = 1.0 / halfMinimum;
    const double end = start + std::max(minimumFeather,
                                        (1.0 - start) * featherFraction);
    const double amount = std::abs(parameters.amount) / 100.0;
    const double highlightProtection = parameters.highlightProtection / 100.0;
    const FloatPixel target = parameters.tintEnabled
        ? FloatPixel {parameters.tint.redF(), parameters.tint.greenF(), parameters.tint.blueF()}
        : (parameters.amount >= 0.0 ? FloatPixel {0.0, 0.0, 0.0}
                                    : FloatPixel {1.0, 1.0, 1.0});

    const auto vignettePixel = [&](const int x, const int y, FloatPixel pixel) {
        const double globalX = previewOrigin.x() + x + 0.5;
        const double globalY = previewOrigin.y() + y + 0.5;
        const double dx = globalX - centreX;
        const double dy = globalY - centreY;
        const double rotatedX = cosine * dx + sine * dy;
        const double rotatedY = -sine * dx + cosine * dy;
        const double ellipseX = rotatedX / halfWidth;
        const double ellipseY = rotatedY / halfHeight;
        const double circleX = rotatedX / halfMinimum;
        const double circleY = rotatedY / halfMinimum;
        const double nx = ellipseX + (circleX - ellipseX) * circleMix;
        const double ny = ellipseY + (circleY - ellipseY) * circleMix;
        const double distance = std::pow(std::pow(std::abs(nx), exponent)
                                           + std::pow(std::abs(ny), exponent),
                                         1.0 / exponent);
        double mask = smoothStep(start, end, distance);
        if (parameters.inverted) mask = 1.0 - mask;
        if (parameters.amount >= 0.0 && highlightProtection > 0.0) {
            const double luminance = pixel.r * 0.2126 + pixel.g * 0.7152 + pixel.b * 0.0722;
            mask *= 1.0 - highlightProtection * smoothStep(0.45, 1.0, luminance);
        }
        const double strength = clamp01(mask * amount);
        pixel.r += (target.r - pixel.r) * strength;
        pixel.g += (target.g - pixel.g) * strength;
        pixel.b += (target.b - pixel.b) * strength;
        return pixel;
    };

    if (sixteenBit) {
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            auto *row = reinterpret_cast<QRgba64 *>(bits + static_cast<qsizetype>(y) * stride);
            for (int x = 0; x < output.width(); ++x) {
                const QRgba64 original = row[x];
                const FloatPixel adjusted = vignettePixel(
                    x, y, {original.red() / 65535.0,
                           original.green() / 65535.0,
                           original.blue() / 65535.0});
                row[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::lround(clamp01(adjusted.r) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(adjusted.g) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(adjusted.b) * 65535.0)),
                    original.alpha());
            }
        });
    } else {
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            uchar *row = bits + static_cast<qsizetype>(y) * stride;
            for (int x = 0; x < output.width(); ++x) {
                uchar *rgba = row + x * 4;
                const FloatPixel adjusted = vignettePixel(
                    x, y, {rgba[0] / 255.0, rgba[1] / 255.0, rgba[2] / 255.0});
                rgba[0] = static_cast<uchar>(std::lround(clamp01(adjusted.r) * 255.0));
                rgba[1] = static_cast<uchar>(std::lround(clamp01(adjusted.g) * 255.0));
                rgba[2] = static_cast<uchar>(std::lround(clamp01(adjusted.b) * 255.0));
            }
        });
    }
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    output.setColorSpace(input.colorSpace());
    return output.convertToFormat(workingFormat(input));
}

template<typename Sample>
double bilinearComponent(const int width,
                         const int height,
                         const double x,
                         const double y,
                         Sample &&sample)
{
    const double clampedX = std::clamp(x, 0.0, std::max(0, width - 1) * 1.0);
    const double clampedY = std::clamp(y, 0.0, std::max(0, height - 1) * 1.0);
    const int x0 = std::clamp(static_cast<int>(std::floor(clampedX)), 0, width - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(clampedY)), 0, height - 1);
    const int x1 = std::min(width - 1, x0 + 1);
    const int y1 = std::min(height - 1, y0 + 1);
    const double tx = clampedX - x0;
    const double ty = clampedY - y0;
    const double c00 = sample(x0, y0);
    const double c10 = sample(x1, y0);
    const double c01 = sample(x0, y1);
    const double c11 = sample(x1, y1);
    const double top = c00 + (c10 - c00) * tx;
    const double bottom = c01 + (c11 - c01) * tx;
    return top + (bottom - top) * ty;
}

QImage applyRgbSplitToImage(const QImage &input,
                            RgbSplitParameters parameters,
                            const double spatialScale,
                            const std::atomic_bool *cancelRequested)
{
    parameters.normalise();
    if (input.isNull()) return {};
    parameters.redOffsetX *= spatialScale;
    parameters.redOffsetY *= spatialScale;
    parameters.blueOffsetX *= spatialScale;
    parameters.blueOffsetY *= spatialScale;
    if (std::abs(parameters.redOffsetX) <= 1.0e-12
        && std::abs(parameters.redOffsetY) <= 1.0e-12
        && std::abs(parameters.blueOffsetX) <= 1.0e-12
        && std::abs(parameters.blueOffsetY) <= 1.0e-12) {
        return input;
    }

    const bool sixteenBit = input.depth() > 32;
    const QImage source = input.convertToFormat(sixteenBit ? QImage::Format_RGBA64
                                                           : QImage::Format_RGBA8888);
    QImage output = source.copy();
    output.detach();
    const uchar *const sourceBits = source.constBits();
    uchar *const outputBits = output.bits();
    if (!sourceBits || !outputBits) return {};
    const qsizetype sourceStride = source.bytesPerLine();
    const qsizetype outputStride = output.bytesPerLine();

    if (sixteenBit) {
        const auto component = [&](const int x, const int y, const int channel) {
            const auto *row = reinterpret_cast<const QRgba64 *>(
                sourceBits + static_cast<qsizetype>(y) * sourceStride);
            const QRgba64 pixel = row[x];
            return channel == 0 ? pixel.red() / 65535.0
                                : (channel == 1 ? pixel.green() / 65535.0
                                                : pixel.blue() / 65535.0);
        };
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            auto *destination = reinterpret_cast<QRgba64 *>(
                outputBits + static_cast<qsizetype>(y) * outputStride);
            const auto *original = reinterpret_cast<const QRgba64 *>(
                sourceBits + static_cast<qsizetype>(y) * sourceStride);
            for (int x = 0; x < output.width(); ++x) {
                const double red = bilinearComponent(output.width(), output.height(),
                    x - parameters.redOffsetX, y - parameters.redOffsetY,
                    [&](const int sx, const int sy) { return component(sx, sy, 0); });
                const double blue = bilinearComponent(output.width(), output.height(),
                    x - parameters.blueOffsetX, y - parameters.blueOffsetY,
                    [&](const int sx, const int sy) { return component(sx, sy, 2); });
                destination[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::lround(clamp01(red) * 65535.0)),
                    original[x].green(),
                    static_cast<quint16>(std::lround(clamp01(blue) * 65535.0)),
                    original[x].alpha());
            }
        });
    } else {
        const auto component = [&](const int x, const int y, const int channel) {
            const uchar *row = sourceBits + static_cast<qsizetype>(y) * sourceStride;
            return row[x * 4 + channel] / 255.0;
        };
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            uchar *destination = outputBits + static_cast<qsizetype>(y) * outputStride;
            const uchar *original = sourceBits + static_cast<qsizetype>(y) * sourceStride;
            for (int x = 0; x < output.width(); ++x) {
                const double red = bilinearComponent(output.width(), output.height(),
                    x - parameters.redOffsetX, y - parameters.redOffsetY,
                    [&](const int sx, const int sy) { return component(sx, sy, 0); });
                const double blue = bilinearComponent(output.width(), output.height(),
                    x - parameters.blueOffsetX, y - parameters.blueOffsetY,
                    [&](const int sx, const int sy) { return component(sx, sy, 2); });
                destination[x * 4] = static_cast<uchar>(std::lround(clamp01(red) * 255.0));
                destination[x * 4 + 1] = original[x * 4 + 1];
                destination[x * 4 + 2] = static_cast<uchar>(std::lround(clamp01(blue) * 255.0));
                destination[x * 4 + 3] = original[x * 4 + 3];
            }
        });
    }
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    output.setColorSpace(input.colorSpace());
    return output.convertToFormat(workingFormat(input));
}

QImage applyChromaticAberrationCorrectionToImage(
    const QImage &input,
    ChromaticAberrationCorrectionParameters parameters,
    const double spatialScale,
    const QPoint previewOrigin,
    const QSize previewSize,
    const std::atomic_bool *cancelRequested)
{
    parameters.normalise();
    if (input.isNull()) return {};
    parameters.redEdgeShift *= spatialScale;
    parameters.blueEdgeShift *= spatialScale;
    if (std::abs(parameters.redEdgeShift) <= 1.0e-12
        && std::abs(parameters.blueEdgeShift) <= 1.0e-12) {
        return input;
    }

    const bool sixteenBit = input.depth() > 32;
    const QImage source = input.convertToFormat(sixteenBit ? QImage::Format_RGBA64
                                                           : QImage::Format_RGBA8888);
    QImage output = source.copy();
    output.detach();
    const uchar *const sourceBits = source.constBits();
    uchar *const outputBits = output.bits();
    if (!sourceBits || !outputBits) return {};
    const qsizetype sourceStride = source.bytesPerLine();
    const qsizetype outputStride = output.bytesPerLine();
    const double fullWidth = std::max(1, previewSize.width());
    const double fullHeight = std::max(1, previewSize.height());
    const double centreX = fullWidth * (0.5 + parameters.centreX / 200.0);
    const double centreY = fullHeight * (0.5 + parameters.centreY / 200.0);
    const double maximumRadius = std::max({std::hypot(centreX, centreY),
        std::hypot(fullWidth - centreX, centreY),
        std::hypot(centreX, fullHeight - centreY),
        std::hypot(fullWidth - centreX, fullHeight - centreY), 1.0});

    const auto samplePositions = [&](const int x, const int y, const double edgeShift) {
        const double globalX = previewOrigin.x() + x + 0.5;
        const double globalY = previewOrigin.y() + y + 0.5;
        const double dx = globalX - centreX;
        const double dy = globalY - centreY;
        const double radius = std::hypot(dx, dy);
        if (radius <= 1.0e-12) return QPointF(x, y);
        const double normalised = clamp01(radius / maximumRadius);
        const double shift = edgeShift * std::pow(normalised, parameters.falloff);
        const double sourceGlobalX = globalX - dx / radius * shift;
        const double sourceGlobalY = globalY - dy / radius * shift;
        return QPointF(sourceGlobalX - previewOrigin.x() - 0.5,
                       sourceGlobalY - previewOrigin.y() - 0.5);
    };

    if (sixteenBit) {
        const auto component = [&](const int x, const int y, const int channel) {
            const auto *row = reinterpret_cast<const QRgba64 *>(
                sourceBits + static_cast<qsizetype>(y) * sourceStride);
            const QRgba64 pixel = row[x];
            return channel == 0 ? pixel.red() / 65535.0
                                : (channel == 1 ? pixel.green() / 65535.0
                                                : pixel.blue() / 65535.0);
        };
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            auto *destination = reinterpret_cast<QRgba64 *>(
                outputBits + static_cast<qsizetype>(y) * outputStride);
            const auto *original = reinterpret_cast<const QRgba64 *>(
                sourceBits + static_cast<qsizetype>(y) * sourceStride);
            for (int x = 0; x < output.width(); ++x) {
                const QPointF redPosition = samplePositions(x, y, parameters.redEdgeShift);
                const QPointF bluePosition = samplePositions(x, y, parameters.blueEdgeShift);
                const double red = bilinearComponent(output.width(), output.height(),
                    redPosition.x(), redPosition.y(),
                    [&](const int sx, const int sy) { return component(sx, sy, 0); });
                const double blue = bilinearComponent(output.width(), output.height(),
                    bluePosition.x(), bluePosition.y(),
                    [&](const int sx, const int sy) { return component(sx, sy, 2); });
                destination[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::lround(clamp01(red) * 65535.0)),
                    original[x].green(),
                    static_cast<quint16>(std::lround(clamp01(blue) * 65535.0)),
                    original[x].alpha());
            }
        });
    } else {
        const auto component = [&](const int x, const int y, const int channel) {
            const uchar *row = sourceBits + static_cast<qsizetype>(y) * sourceStride;
            return row[x * 4 + channel] / 255.0;
        };
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            uchar *destination = outputBits + static_cast<qsizetype>(y) * outputStride;
            const uchar *original = sourceBits + static_cast<qsizetype>(y) * sourceStride;
            for (int x = 0; x < output.width(); ++x) {
                const QPointF redPosition = samplePositions(x, y, parameters.redEdgeShift);
                const QPointF bluePosition = samplePositions(x, y, parameters.blueEdgeShift);
                const double red = bilinearComponent(output.width(), output.height(),
                    redPosition.x(), redPosition.y(),
                    [&](const int sx, const int sy) { return component(sx, sy, 0); });
                const double blue = bilinearComponent(output.width(), output.height(),
                    bluePosition.x(), bluePosition.y(),
                    [&](const int sx, const int sy) { return component(sx, sy, 2); });
                destination[x * 4] = static_cast<uchar>(std::lround(clamp01(red) * 255.0));
                destination[x * 4 + 1] = original[x * 4 + 1];
                destination[x * 4 + 2] = static_cast<uchar>(std::lround(clamp01(blue) * 255.0));
                destination[x * 4 + 3] = original[x * 4 + 3];
            }
        });
    }
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    output.setColorSpace(input.colorSpace());
    return output.convertToFormat(workingFormat(input));
}

FloatPixel applyAdjustment(FloatPixel pixel,
                           const AdjustmentData &data,
                           const LutDocumentTransfer lutDocumentTransfer,
                           const AdjustmentProcessingDomain processingDomain,
                           const bool managedProcessing)
{
    switch (data.type) {
    case AdjustmentType::Exposure: {
        const ExposureParameters &parameters = std::get<ExposureParameters>(data.parameters);
        if (std::abs(parameters.exposure) <= 1.0e-12
            && std::abs(parameters.offset) <= 1.0e-12
            && std::abs(parameters.gamma - 1.0) <= 1.0e-12) {
            break;
        }
        const double factor = std::exp2(parameters.exposure);
        const double inverseGamma = 1.0 / std::max(0.01, parameters.gamma);
        auto adjust = [&](const double value) {
            if (managedProcessing
                && processingDomain == AdjustmentProcessingDomain::LinearWorking) {
                const double linear = std::max(0.0, value * factor + parameters.offset);
                return clamp01(std::pow(linear, inverseGamma));
            }
            const double linear = std::max(0.0,
                                           srgbToLinear(value) * factor
                                               + parameters.offset);
            return linearToSrgb(std::pow(linear, inverseGamma));
        };
        pixel.r = adjust(pixel.r);
        pixel.g = adjust(pixel.g);
        pixel.b = adjust(pixel.b);
        break;
    }
    case AdjustmentType::Contrast: {
        const ContrastParameters &parameters = std::get<ContrastParameters>(data.parameters);
        if (std::abs(parameters.contrast) <= 1.0e-12) break;
        const FloatPixel linear = linearRgb(pixel);
        const double luminance = linear.r * 0.2126 + linear.g * 0.7152 + linear.b * 0.0722;
        const double pivot = srgbToLinear(parameters.pivot);
        const double amount = parameters.contrast / 100.0;
        const double factor = amount < 0.0
            ? std::max(0.0, 1.0 + amount)
            : 1.0 / std::max(0.01, 1.0 - amount * 0.99);
        const double adjustedLuminance = clamp01((luminance - pivot) * factor + pivot);
        FloatPixel adjustedLinear;
        if (luminance > 1.0e-12) {
            const double scale = adjustedLuminance / luminance;
            adjustedLinear = {linear.r * scale, linear.g * scale, linear.b * scale};
        } else {
            adjustedLinear = {adjustedLuminance, adjustedLuminance, adjustedLuminance};
        }
        pixel = encodedRgb(adjustedLinear);
        break;
    }
    case AdjustmentType::Saturation: {
        const SaturationParameters &parameters = std::get<SaturationParameters>(data.parameters);
        if (std::abs(parameters.saturation) <= 1.0e-12) break;
        pixel = applyPerceptualSaturation(pixel, parameters.saturation);
        break;
    }
    case AdjustmentType::HueSaturation: {
        const HueSaturationParameters &parameters = std::get<HueSaturationParameters>(data.parameters);
        bool identity = std::abs(parameters.hue) <= 1.0e-12
            && std::abs(parameters.saturation) <= 1.0e-12
            && std::abs(parameters.lightness) <= 1.0e-12;
        for (const HueSaturationRangeParameters &range : parameters.ranges) {
            identity = identity && std::abs(range.hue) <= 1.0e-12
                && std::abs(range.saturation) <= 1.0e-12
                && std::abs(range.lightness) <= 1.0e-12;
        }
        if (!identity) pixel = applyHueSaturation(pixel, parameters);
        break;
    }
    case AdjustmentType::Vibrance: {
        const VibranceParameters &parameters = std::get<VibranceParameters>(data.parameters);
        if (std::abs(parameters.vibrance) > 1.0e-12
            || std::abs(parameters.saturation) > 1.0e-12) {
            pixel = applyVibrance(pixel, parameters);
        }
        break;
    }
    case AdjustmentType::WhiteBalance: {
        const WhiteBalanceParameters &parameters = std::get<WhiteBalanceParameters>(data.parameters);
        if (std::abs(parameters.temperature) > 1.0e-12
            || std::abs(parameters.tint) > 1.0e-12) {
            pixel = applyWhiteBalance(pixel, parameters);
        }
        break;
    }
    case AdjustmentType::ColourBalance: {
        const ColourBalanceParameters &parameters = std::get<ColourBalanceParameters>(data.parameters);
        bool identity = true;
        for (const ColourBalanceRangeParameters &range : parameters.ranges) {
            identity = identity && std::abs(range.cyanRed) <= 1.0e-12
                && std::abs(range.magentaGreen) <= 1.0e-12
                && std::abs(range.yellowBlue) <= 1.0e-12;
        }
        if (!identity) pixel = applyColourBalance(pixel, parameters);
        break;
    }
    case AdjustmentType::ChannelMixer:
        pixel = applyChannelMixer(pixel, std::get<ChannelMixerParameters>(data.parameters));
        break;
    case AdjustmentType::BlackAndWhite:
        pixel = applyBlackAndWhite(pixel, std::get<BlackAndWhiteParameters>(data.parameters));
        break;
    case AdjustmentType::GradientMap:
        // Luminance-indexed Gradient Map uses the precomputed exact lookup in
        // applyAdjustmentToImage().
        break;
    case AdjustmentType::Posterise: {
        const PosteriseParameters &parameters = std::get<PosteriseParameters>(data.parameters);
        const double scale = std::max(1, parameters.levels - 1);
        pixel.r = clamp01(std::round(pixel.r * scale) / scale);
        pixel.g = clamp01(std::round(pixel.g * scale) / scale);
        pixel.b = clamp01(std::round(pixel.b * scale) / scale);
        break;
    }
    case AdjustmentType::Threshold: {
        const ThresholdParameters &parameters = std::get<ThresholdParameters>(data.parameters);
        double source = pixel.r * 0.2126 + pixel.g * 0.7152 + pixel.b * 0.0722;
        if (parameters.source == ThresholdSource::Red) source = pixel.r;
        else if (parameters.source == ThresholdSource::Green) source = pixel.g;
        else if (parameters.source == ThresholdSource::Blue) source = pixel.b;
        const double value = source >= parameters.threshold ? 1.0 : 0.0;
        pixel = {value, value, value};
        break;
    }
    case AdjustmentType::Invert:
        pixel = {1.0 - pixel.r, 1.0 - pixel.g, 1.0 - pixel.b};
        break;
    case AdjustmentType::PhotoFilter: {
        const PhotoFilterParameters &parameters =
            std::get<PhotoFilterParameters>(data.parameters);
        if (parameters.density > 1.0e-12) {
            pixel = applyPhotoFilter(pixel, parameters);
        }
        break;
    }
    case AdjustmentType::SelectiveColour: {
        const SelectiveColourParameters &parameters =
            std::get<SelectiveColourParameters>(data.parameters);
        bool identity = true;
        for (const SelectiveColourRangeParameters &range : parameters.ranges) {
            identity = identity && std::abs(range.cyan) <= 1.0e-12
                && std::abs(range.magenta) <= 1.0e-12
                && std::abs(range.yellow) <= 1.0e-12
                && std::abs(range.black) <= 1.0e-12;
        }
        if (!identity) pixel = applySelectiveColour(pixel, parameters);
        break;
    }
    case AdjustmentType::Lut: {
        const LutParameters &parameters = std::get<LutParameters>(data.parameters);
        const auto mapped = CubeLut::evaluate(
            parameters, {pixel.r, pixel.g, pixel.b}, lutDocumentTransfer);
        pixel = {mapped[0], mapped[1], mapped[2]};
        break;
    }
    case AdjustmentType::Levels:
    case AdjustmentType::Curves:
    case AdjustmentType::ShadowsHighlights:
    case AdjustmentType::GaussianBlur:
    case AdjustmentType::BoxBlur:
    case AdjustmentType::UnsharpMask:
    case AdjustmentType::HighPass:
    case AdjustmentType::Vignette:
    case AdjustmentType::RgbSplit:
    case AdjustmentType::ChromaticAberrationCorrection:
    case AdjustmentType::SurfaceBlur:
    case AdjustmentType::MotionBlur:
    case AdjustmentType::RadialBlur:
        // Lookup, coordinate-dependent and spatial adjustments are handled by applyAdjustmentToImage().
        break;
    }
    return pixel;
}

QImage::Format workingFormat(const QImage &source)
{
    return source.depth() > 32 ? QImage::Format_RGBA64_Premultiplied
                               : QImage::Format_ARGB32_Premultiplied;
}

QImage transparentImage(const QSize &size, const QImage::Format format, const QColorSpace &space)
{
    QImage image(size, format);
    image.fill(Qt::transparent);
    image.setColorSpace(space);
    return image;
}

bool adjustmentIsExactIdentity(const AdjustmentData &adjustment)
{
    constexpr double eps = 1.0e-12;
    switch (adjustment.type) {
    case AdjustmentType::Exposure: {
        const auto &p = std::get<ExposureParameters>(adjustment.parameters);
        return std::abs(p.exposure) <= eps && std::abs(p.offset) <= eps
            && std::abs(p.gamma - 1.0) <= eps;
    }
    case AdjustmentType::Contrast:
        return std::abs(std::get<ContrastParameters>(adjustment.parameters).contrast) <= eps;
    case AdjustmentType::Saturation:
        return std::abs(std::get<SaturationParameters>(adjustment.parameters).saturation) <= eps;
    case AdjustmentType::Levels: {
        const auto &p = std::get<LevelsParameters>(adjustment.parameters);
        const LevelsChannelParameters identity;
        return std::all_of(p.channels.cbegin(), p.channels.cend(),
                           [&](const LevelsChannelParameters &channel) {
                               return channel == identity;
                           });
    }
    case AdjustmentType::Curves: {
        const auto &p = std::get<CurvesParameters>(adjustment.parameters);
        const CurveChannelParameters identity;
        return std::all_of(p.channels.cbegin(), p.channels.cend(),
                           [&](const CurveChannelParameters &channel) {
                               return channel == identity;
                           });
    }
    case AdjustmentType::HueSaturation: {
        const auto &p = std::get<HueSaturationParameters>(adjustment.parameters);
        if (std::abs(p.hue) > eps || std::abs(p.saturation) > eps
            || std::abs(p.lightness) > eps) {
            return false;
        }
        return std::all_of(p.ranges.cbegin(), p.ranges.cend(),
                           [&](const HueSaturationRangeParameters &range) {
                               return std::abs(range.hue) <= eps
                                   && std::abs(range.saturation) <= eps
                                   && std::abs(range.lightness) <= eps;
                           });
    }
    case AdjustmentType::Vibrance: {
        const auto &p = std::get<VibranceParameters>(adjustment.parameters);
        return std::abs(p.vibrance) <= eps && std::abs(p.saturation) <= eps;
    }
    case AdjustmentType::WhiteBalance: {
        const auto &p = std::get<WhiteBalanceParameters>(adjustment.parameters);
        return std::abs(p.temperature) <= eps && std::abs(p.tint) <= eps;
    }
    case AdjustmentType::ColourBalance: {
        const auto &p = std::get<ColourBalanceParameters>(adjustment.parameters);
        return std::all_of(p.ranges.cbegin(), p.ranges.cend(),
                           [&](const ColourBalanceRangeParameters &range) {
                               return std::abs(range.cyanRed) <= eps
                                   && std::abs(range.magentaGreen) <= eps
                                   && std::abs(range.yellowBlue) <= eps;
                           });
    }
    case AdjustmentType::ChannelMixer: {
        const auto &p = std::get<ChannelMixerParameters>(adjustment.parameters);
        const ChannelMixerParameters identity;
        return p == identity;
    }
    case AdjustmentType::PhotoFilter:
        return std::get<PhotoFilterParameters>(adjustment.parameters).density <= eps;
    case AdjustmentType::SelectiveColour: {
        const auto &p = std::get<SelectiveColourParameters>(adjustment.parameters);
        return std::all_of(p.ranges.cbegin(), p.ranges.cend(),
                           [&](const SelectiveColourRangeParameters &range) {
                               return std::abs(range.cyan) <= eps
                                   && std::abs(range.magenta) <= eps
                                   && std::abs(range.yellow) <= eps
                                   && std::abs(range.black) <= eps;
                           });
    }
    case AdjustmentType::Vignette:
        return std::abs(std::get<VignetteParameters>(adjustment.parameters).amount) <= eps;
    case AdjustmentType::RgbSplit: {
        const auto &p = std::get<RgbSplitParameters>(adjustment.parameters);
        return std::abs(p.redOffsetX) <= eps && std::abs(p.redOffsetY) <= eps
            && std::abs(p.blueOffsetX) <= eps && std::abs(p.blueOffsetY) <= eps;
    }
    case AdjustmentType::ChromaticAberrationCorrection: {
        const auto &p = std::get<ChromaticAberrationCorrectionParameters>(adjustment.parameters);
        return std::abs(p.redEdgeShift) <= eps && std::abs(p.blueEdgeShift) <= eps;
    }
    case AdjustmentType::SurfaceBlur:
        return std::get<SurfaceBlurParameters>(adjustment.parameters).radius <= eps;
    case AdjustmentType::MotionBlur:
        return std::get<MotionBlurParameters>(adjustment.parameters).distance <= eps;
    case AdjustmentType::RadialBlur:
        return std::get<RadialBlurParameters>(adjustment.parameters).amount <= eps;
    case AdjustmentType::ShadowsHighlights: {
        const auto &p = std::get<ShadowsHighlightsParameters>(adjustment.parameters);
        return std::abs(p.shadowAmount) <= eps && std::abs(p.highlightAmount) <= eps
            && std::abs(p.midtoneContrast) <= eps;
    }
    case AdjustmentType::GaussianBlur:
        return std::get<GaussianBlurParameters>(adjustment.parameters).radius <= eps;
    case AdjustmentType::BoxBlur:
        return std::get<BoxBlurParameters>(adjustment.parameters).radius <= eps;
    case AdjustmentType::UnsharpMask: {
        const auto &p = std::get<UnsharpMaskParameters>(adjustment.parameters);
        return p.radius <= eps || p.amount <= eps;
    }
    case AdjustmentType::BlackAndWhite:
    case AdjustmentType::GradientMap:
    case AdjustmentType::Posterise:
    case AdjustmentType::Threshold:
    case AdjustmentType::Invert:
    case AdjustmentType::Lut:
    case AdjustmentType::HighPass:
        return false;
    }
    return false;
}

QImage applyAdjustmentToImage(const QImage &input,
                              const LayerNode &layer,
                              const std::atomic_bool *cancelRequested,
                              const double spatialScale,
                              const QPoint previewOrigin,
                              const QSize previewSize,
                              const ColourProcessingCompatibility processingCompatibility)
{
    if (input.isNull()) {
        return {};
    }

    AdjustmentData adjustment = layer.effectiveAdjustmentData();
    // Preserve exact component values and hidden RGB for semantic no-op
    // adjustments. In particular this avoids a managed-domain decode/encode
    // round trip changing an identity adjustment by one code value on some
    // Qt/platform colour backends.
    if (adjustmentIsExactIdentity(adjustment)) {
        return input;
    }
    const QColorSpace workingSpace = input.colorSpace();
    const AdjustmentProcessingDomain domain = adjustmentProcessingDomain(adjustment);
    const bool managedRequested =
        processingCompatibility == ColourProcessingCompatibility::ManagedV1;
    const QColorSpace domainSpace = managedRequested
        ? adjustmentDomainColourSpace(workingSpace, domain)
        : workingSpace;
    // Some unusual ICC profiles cannot be represented with an alternate Qt
    // transfer function. In that case retain the established component-domain
    // operator rather than accidentally treating encoded values as linear.
    const bool managed = managedRequested && workingSpace.isValid()
        && domainSpace.isValid();
    QImage domainInput = managed
        ? transformAdjustmentDomainImage(input, domainSpace)
        : input;
    if (domainInput.isNull()) return {};

    const bool useSixteenBit = domainInput.depth() > 32;
    QImage output = domainInput.convertToFormat(useSixteenBit ? QImage::Format_RGBA64
                                                              : QImage::Format_RGBA8888);
    output.setColorSpace(domainInput.colorSpace());

    uchar *const base = output.bits();
    const qsizetype stride = output.bytesPerLine();
    const LutDocumentTransfer lutDocumentTransfer =
        CubeLut::documentTransferFor(domainInput.colorSpace());
    if (adjustment.type == AdjustmentType::Vignette) {
        QImage adjusted = applyVignetteToImage(
            domainInput, std::get<VignetteParameters>(adjustment.parameters),
            previewOrigin, previewSize, cancelRequested);
        if (managed && !adjusted.isNull() && domainSpace.isValid()
            && workingSpace.isValid() && domainSpace != workingSpace) {
            adjusted = transformAdjustmentDomainImage(adjusted, workingSpace);
        }
        if (!adjusted.isNull()) adjusted.setColorSpace(workingSpace);
        return adjusted.isNull() ? QImage() : adjusted.convertToFormat(workingFormat(input));
    }
    if (adjustment.type == AdjustmentType::RgbSplit) {
        QImage adjusted = applyRgbSplitToImage(
            domainInput, std::get<RgbSplitParameters>(adjustment.parameters),
            spatialScale, cancelRequested);
        if (!adjusted.isNull()) adjusted.setColorSpace(workingSpace);
        return adjusted.isNull() ? QImage() : adjusted.convertToFormat(workingFormat(input));
    }
    if (adjustment.type == AdjustmentType::ChromaticAberrationCorrection) {
        QImage adjusted = applyChromaticAberrationCorrectionToImage(
            domainInput,
            std::get<ChromaticAberrationCorrectionParameters>(adjustment.parameters),
            spatialScale, previewOrigin, previewSize, cancelRequested);
        if (!adjusted.isNull()) adjusted.setColorSpace(workingSpace);
        return adjusted.isNull() ? QImage() : adjusted.convertToFormat(workingFormat(input));
    }
    if (adjustment.type == AdjustmentType::SurfaceBlur) {
        auto parameters = std::get<SurfaceBlurParameters>(adjustment.parameters);
        parameters.radius *= std::max(0.0001, spatialScale);
        QImage adjusted = applySurfaceBlurToImage(domainInput, parameters, cancelRequested);
        if (!adjusted.isNull()) adjusted.setColorSpace(workingSpace);
        return adjusted.isNull() ? QImage() : adjusted.convertToFormat(workingFormat(input));
    }
    if (adjustment.type == AdjustmentType::MotionBlur) {
        QImage adjusted = applyMotionBlurToImage(
            domainInput, std::get<MotionBlurParameters>(adjustment.parameters),
            spatialScale, previewOrigin, cancelRequested);
        if (!adjusted.isNull()) adjusted.setColorSpace(workingSpace);
        return adjusted.isNull() ? QImage() : adjusted.convertToFormat(workingFormat(input));
    }
    if (adjustment.type == AdjustmentType::RadialBlur) {
        QImage adjusted = applyRadialBlurToImage(
            domainInput, std::get<RadialBlurParameters>(adjustment.parameters),
            spatialScale, previewOrigin, previewSize, cancelRequested);
        if (!adjusted.isNull()) adjusted.setColorSpace(workingSpace);
        return adjusted.isNull() ? QImage() : adjusted.convertToFormat(workingFormat(input));
    }
    if (adjustment.type == AdjustmentType::ShadowsHighlights) {
        ShadowsHighlightsParameters parameters = std::get<ShadowsHighlightsParameters>(adjustment.parameters);
        parameters.radius = std::max(1.0, parameters.radius * std::max(0.0001, spatialScale));
        QImage adjusted = applyShadowsHighlightsToImage(domainInput, parameters, cancelRequested);
        if (managed && !adjusted.isNull() && domainSpace.isValid()
            && workingSpace.isValid() && domainSpace != workingSpace) {
            adjusted = transformAdjustmentDomainImage(adjusted, workingSpace);
        }
        if (!adjusted.isNull()) adjusted.setColorSpace(workingSpace);
        return adjusted.isNull() ? QImage() : adjusted.convertToFormat(workingFormat(input));
    }
    if (adjustment.type == AdjustmentType::GaussianBlur) {
        auto parameters = std::get<GaussianBlurParameters>(adjustment.parameters);
        parameters.radius *= std::max(0.0001, spatialScale);
        QImage adjusted = applyGaussianBlurToImage(domainInput, parameters, cancelRequested);
        if (!adjusted.isNull()) adjusted.setColorSpace(workingSpace);
        return adjusted.isNull() ? QImage() : adjusted.convertToFormat(workingFormat(input));
    }
    if (adjustment.type == AdjustmentType::BoxBlur) {
        auto parameters = std::get<BoxBlurParameters>(adjustment.parameters);
        parameters.radius *= std::max(0.0001, spatialScale);
        QImage adjusted = applyBoxBlurToImage(domainInput, parameters, cancelRequested);
        if (!adjusted.isNull()) adjusted.setColorSpace(workingSpace);
        return adjusted.isNull() ? QImage() : adjusted.convertToFormat(workingFormat(input));
    }
    if (adjustment.type == AdjustmentType::UnsharpMask) {
        auto parameters = std::get<UnsharpMaskParameters>(adjustment.parameters);
        parameters.radius *= std::max(0.0001, spatialScale);
        QImage adjusted = applyUnsharpMaskToImage(domainInput, parameters, cancelRequested);
        if (!adjusted.isNull()) adjusted.setColorSpace(workingSpace);
        return adjusted.isNull() ? QImage() : adjusted.convertToFormat(workingFormat(input));
    }
    if (adjustment.type == AdjustmentType::HighPass) {
        auto parameters = std::get<HighPassParameters>(adjustment.parameters);
        parameters.radius *= std::max(0.0001, spatialScale);
        QImage adjusted = applyHighPassToImage(domainInput, parameters, cancelRequested);
        if (!adjusted.isNull()) adjusted.setColorSpace(workingSpace);
        return adjusted.isNull() ? QImage() : adjusted.convertToFormat(workingFormat(input));
    }
    const bool tonal = adjustmentUsesTonalLookup(adjustment.type);
    const bool luminanceLookup = adjustmentUsesLuminanceLookup(adjustment.type);
    const TonalLookupTable lookup = (tonal || luminanceLookup)
        ? buildTonalLookup(adjustment, useSixteenBit ? 16 : 8)
        : TonalLookupTable {};

    if (useSixteenBit) {
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            auto *pixels = reinterpret_cast<QRgba64 *>(base + static_cast<qsizetype>(y) * stride);
            for (int x = 0; x < output.width(); ++x) {
                const QRgba64 original = pixels[x];
                if (tonal) {
                    pixels[x] = QRgba64::fromRgba64(
                        lookup.map(0, original.red()),
                        lookup.map(1, original.green()),
                        lookup.map(2, original.blue()),
                        original.alpha());
                    continue;
                }
                if (luminanceLookup) {
                    const int luminance = std::clamp(static_cast<int>(std::lround(
                        original.red() * 0.2126 + original.green() * 0.7152
                        + original.blue() * 0.0722)), 0, 65535);
                    pixels[x] = QRgba64::fromRgba64(
                        lookup.map(0, luminance), lookup.map(1, luminance),
                        lookup.map(2, luminance), original.alpha());
                    continue;
                }
                FloatPixel pixel {original.red() / 65535.0,
                                  original.green() / 65535.0,
                                  original.blue() / 65535.0};
                pixel = applyAdjustment(pixel,
                                        adjustment,
                                        lutDocumentTransfer,
                                        domain,
                                        managed);
                pixels[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::lround(clamp01(pixel.r) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(pixel.g) * 65535.0)),
                    static_cast<quint16>(std::lround(clamp01(pixel.b) * 65535.0)),
                    original.alpha());
            }
        });
    } else {
        processRows(output.width(), output.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
            uchar *pixels = base + static_cast<qsizetype>(y) * stride;
            for (int x = 0; x < output.width(); ++x) {
                uchar *rgba = pixels + x * 4;
                if (tonal) {
                    rgba[0] = static_cast<uchar>(lookup.map(0, rgba[0]));
                    rgba[1] = static_cast<uchar>(lookup.map(1, rgba[1]));
                    rgba[2] = static_cast<uchar>(lookup.map(2, rgba[2]));
                    continue;
                }
                if (luminanceLookup) {
                    const int luminance = std::clamp(static_cast<int>(std::lround(
                        rgba[0] * 0.2126 + rgba[1] * 0.7152 + rgba[2] * 0.0722)),
                        0, 255);
                    rgba[0] = static_cast<uchar>(lookup.map(0, luminance));
                    rgba[1] = static_cast<uchar>(lookup.map(1, luminance));
                    rgba[2] = static_cast<uchar>(lookup.map(2, luminance));
                    continue;
                }
                FloatPixel pixel {rgba[0] / 255.0, rgba[1] / 255.0, rgba[2] / 255.0};
                pixel = applyAdjustment(pixel,
                                        adjustment,
                                        lutDocumentTransfer,
                                        domain,
                                        managed);
                rgba[0] = static_cast<uchar>(std::lround(clamp01(pixel.r) * 255.0));
                rgba[1] = static_cast<uchar>(std::lround(clamp01(pixel.g) * 255.0));
                rgba[2] = static_cast<uchar>(std::lround(clamp01(pixel.b) * 255.0));
            }
        });
    }

    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
        return {};
    }
    QImage result = output.convertToFormat(workingFormat(domainInput));
    result.setColorSpace(domainInput.colorSpace());
    if (managed && domainSpace.isValid() && workingSpace.isValid()
        && domainSpace != workingSpace) {
        result = transformAdjustmentDomainImage(result, workingSpace);
    }
    if (!result.isNull()) {
        result = result.convertToFormat(workingFormat(input));
        result.setColorSpace(workingSpace);
    }
    return result;
}


double blendChannel(const double base, const double effect, const BlendMode mode)
{
    switch (mode) {
    case BlendMode::Copy:
        return effect;
    case BlendMode::Multiply:
        return base * effect;
    case BlendMode::Screen:
        return 1.0 - (1.0 - base) * (1.0 - effect);
    case BlendMode::Overlay:
        return base <= 0.5 ? 2.0 * base * effect
                           : 1.0 - 2.0 * (1.0 - base) * (1.0 - effect);
    case BlendMode::Darken:
        return std::min(base, effect);
    case BlendMode::Lighten:
        return std::max(base, effect);
    case BlendMode::ColourDodge:
        return effect >= 1.0 ? 1.0 : std::min(1.0, base / (1.0 - effect));
    case BlendMode::ColourBurn:
        return effect <= 0.0 ? 0.0 : 1.0 - std::min(1.0, (1.0 - base) / effect);
    case BlendMode::Add:
        return std::min(1.0, base + effect);
    case BlendMode::Subtract:
        return std::max(0.0, base - effect);
    case BlendMode::Difference:
        return std::abs(base - effect);
    case BlendMode::Exclusion:
        return base + effect - 2.0 * base * effect;
    }
    return effect;
}

QImage preparedMask(const QImage &mask,
                    const QSize &size,
                    const bool enabled,
                    const bool inverted)
{
    if (!enabled || mask.isNull()) {
        return {};
    }
    if (!inverted && mask.size() == QSize(1, 1) && qGray(mask.pixel(0, 0)) == 255) {
        return {};
    }
    QImage prepared = mask;
    if (prepared.size() != size) {
        prepared = prepared.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    prepared = prepared.convertToFormat(QImage::Format_Grayscale8);
    if (inverted) {
        prepared.invertPixels(QImage::InvertRgb);
    }
    return prepared;
}

void compositeAdjustment(QImage &canvas,
                         const QImage &adjusted,
                         const LayerNode &layer,
                         const std::atomic_bool *cancelRequested)
{
    if (canvas.isNull() || adjusted.isNull() || layer.opacity <= 0.0) {
        return;
    }

    const bool sixteenBit = canvas.depth() > 32;
    QImage base = canvas.convertToFormat(sixteenBit ? QImage::Format_RGBA64
                                                    : QImage::Format_RGBA8888);
    const QImage effect = adjusted.convertToFormat(sixteenBit ? QImage::Format_RGBA64
                                                               : QImage::Format_RGBA8888);
    const QImage mask = preparedMask(layer.maskImage,
                                     canvas.size(),
                                     layer.maskEnabled,
                                     layer.maskInverted);
    const AdjustmentData adjustment = layer.effectiveAdjustmentData();
    bool affectsAlpha = false;
    if (adjustment.type == AdjustmentType::GaussianBlur) {
        affectsAlpha = std::get<GaussianBlurParameters>(adjustment.parameters).affectAlpha;
    } else if (adjustment.type == AdjustmentType::BoxBlur) {
        affectsAlpha = std::get<BoxBlurParameters>(adjustment.parameters).affectAlpha;
    } else if (adjustment.type == AdjustmentType::MotionBlur) {
        affectsAlpha = std::get<MotionBlurParameters>(adjustment.parameters).affectAlpha;
    } else if (adjustment.type == AdjustmentType::RadialBlur) {
        affectsAlpha = std::get<RadialBlurParameters>(adjustment.parameters).affectAlpha;
    }

    uchar *const baseBits = base.bits();
    const uchar *const effectBits = effect.constBits();
    const qsizetype baseStride = base.bytesPerLine();
    const qsizetype effectStride = effect.bytesPerLine();

    if (sixteenBit) {
        processRows(base.width(), base.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
                return;
            }
            auto *basePixels = reinterpret_cast<QRgba64 *>(
                baseBits + static_cast<qsizetype>(y) * baseStride);
            const auto *effectPixels = reinterpret_cast<const QRgba64 *>(
                effectBits + static_cast<qsizetype>(y) * effectStride);
            const uchar *maskPixels = mask.isNull() ? nullptr : mask.constScanLine(y);
            for (int x = 0; x < base.width(); ++x) {
                const double weight = layer.opacity
                    * (maskPixels ? maskPixels[x] / 255.0 : 1.0);
                if (weight <= 0.0) {
                    continue;
                }
                const QRgba64 b = basePixels[x];
                const QRgba64 e = effectPixels[x];
                const auto mix = [&](const quint16 baseValue, const quint16 effectValue) {
                    const double baseChannel = baseValue / 65535.0;
                    const double effectChannel = effectValue / 65535.0;
                    const double blended = clamp01(blendChannel(baseChannel,
                                                                 effectChannel,
                                                                 layer.blendMode));
                    return static_cast<quint16>(std::lround(
                        (baseChannel + (blended - baseChannel) * weight) * 65535.0));
                };
                const quint16 alpha = affectsAlpha
                    ? static_cast<quint16>(std::lround(
                          (b.alpha() + (e.alpha() - b.alpha()) * weight)))
                    : b.alpha();
                basePixels[x] = QRgba64::fromRgba64(mix(b.red(), e.red()),
                                                    mix(b.green(), e.green()),
                                                    mix(b.blue(), e.blue()),
                                                    alpha);
            }
        });
    } else {
        processRows(base.width(), base.height(), [&](const int y) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
                return;
            }
            uchar *basePixels = baseBits + static_cast<qsizetype>(y) * baseStride;
            const uchar *effectPixels = effectBits + static_cast<qsizetype>(y) * effectStride;
            const uchar *maskPixels = mask.isNull() ? nullptr : mask.constScanLine(y);
            for (int x = 0; x < base.width(); ++x) {
                const int offset = x * 4;
                const double weight = layer.opacity
                    * (maskPixels ? maskPixels[x] / 255.0 : 1.0);
                if (weight <= 0.0) {
                    continue;
                }
                for (int channel = 0; channel < 3; ++channel) {
                    const double baseChannel = basePixels[offset + channel] / 255.0;
                    const double effectChannel = effectPixels[offset + channel] / 255.0;
                    const double blended = clamp01(blendChannel(baseChannel,
                                                                 effectChannel,
                                                                 layer.blendMode));
                    basePixels[offset + channel] = static_cast<uchar>(std::lround(
                        (baseChannel + (blended - baseChannel) * weight) * 255.0));
                }
                if (affectsAlpha) {
                    basePixels[offset + 3] = static_cast<uchar>(std::lround(
                        basePixels[offset + 3]
                        + (effectPixels[offset + 3] - basePixels[offset + 3]) * weight));
                }
            }
        });
    }

    if (!(cancelRequested && cancelRequested->load(std::memory_order_relaxed))) {
        canvas = base.convertToFormat(workingFormat(canvas));
        canvas.setColorSpace(adjusted.colorSpace());
    }
}

QPainter::CompositionMode compositionMode(const BlendMode mode)
{
    switch (mode) {
    case BlendMode::Copy:
        return QPainter::CompositionMode_SourceOver;
    case BlendMode::Multiply:
        return QPainter::CompositionMode_Multiply;
    case BlendMode::Screen:
        return QPainter::CompositionMode_Screen;
    case BlendMode::Overlay:
        return QPainter::CompositionMode_Overlay;
    case BlendMode::Darken:
        return QPainter::CompositionMode_Darken;
    case BlendMode::Lighten:
        return QPainter::CompositionMode_Lighten;
    case BlendMode::ColourDodge:
        return QPainter::CompositionMode_ColorDodge;
    case BlendMode::ColourBurn:
        return QPainter::CompositionMode_ColorBurn;
    case BlendMode::Add:
        return QPainter::CompositionMode_Plus;
    case BlendMode::Subtract:
        // Qt has no subtract composition mode; Difference is the closest
        // predictable cross-platform fallback until the GPU blender lands.
        return QPainter::CompositionMode_Difference;
    case BlendMode::Difference:
        return QPainter::CompositionMode_Difference;
    case BlendMode::Exclusion:
        return QPainter::CompositionMode_Exclusion;
    }
    return QPainter::CompositionMode_SourceOver;
}

QImage maskAsAlpha(const QImage &mask, const QSize &size)
{
    if (mask.isNull()) {
        return {};
    }
    QImage scaled = mask;
    if (scaled.size() != size) {
        scaled = scaled.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    scaled = scaled.convertToFormat(QImage::Format_RGBA8888);
    QImage alpha(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        const uchar *src = scaled.constScanLine(y);
        QRgb *dst = reinterpret_cast<QRgb *>(alpha.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const int offset = x * 4;
            const int value = qRound(src[offset] * 0.2126
                                     + src[offset + 1] * 0.7152
                                     + src[offset + 2] * 0.0722);
            dst[x] = qRgba(255, 255, 255, value);
        }
    }
    return alpha;
}

void applyMask(QImage &image,
               const QImage &mask,
               const bool enabled,
               const bool inverted)
{
    if (image.isNull() || !enabled || mask.isNull()) {
        return;
    }
    if (!inverted && mask.size() == QSize(1, 1) && qGray(mask.pixel(0, 0)) == 255) {
        return;
    }
    QImage effective = mask;
    if (inverted) {
        effective = effective.convertToFormat(QImage::Format_Grayscale8);
        effective.invertPixels(QImage::InvertRgb);
    }
    const QImage alpha = maskAsAlpha(effective, image.size());
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.drawImage(QPoint(0, 0), alpha);
}

void composite(QImage &destination,
               const QImage &source,
               const BlendMode mode,
               const double opacity)
{
    if (destination.isNull() || source.isNull() || opacity <= 0.0) {
        return;
    }
    QPainter painter(&destination);
    painter.setCompositionMode(compositionMode(mode));
    painter.setOpacity(std::clamp(opacity, 0.0, 1.0));
    painter.drawImage(QPoint(0, 0), source);
}


QVector<quint16> coveragePlane16(const QImage &coverage)
{
    if (coverage.isNull()) return {};
    const QImage gray = coverage.convertToFormat(QImage::Format_Grayscale16);
    if (gray.isNull()) return {};
    QVector<quint16> result(static_cast<qsizetype>(gray.width()) * gray.height());
    for (int y = 0; y < gray.height(); ++y) {
        const auto *src = reinterpret_cast<const quint16 *>(gray.constScanLine(y));
        std::copy_n(src, gray.width(), result.data() + static_cast<qsizetype>(y) * gray.width());
    }
    return result;
}

QVector<quint16> shiftedCoverage16(const QVector<quint16> &source,
                                   const int width,
                                   const int height,
                                   const double dx,
                                   const double dy,
                                   const std::atomic_bool *cancelRequested)
{
    if (width <= 0 || height <= 0
        || source.size() != static_cast<qsizetype>(width) * height) return {};
    QVector<quint16> result(source.size(), 0);
    const auto sample = [&](const int x, const int y) -> quint16 {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0;
        return source[static_cast<qsizetype>(y) * width + x];
    };
    processRows(width, height, [&](const int y) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
        quint16 *dst = result.data() + static_cast<qsizetype>(y) * width;
        for (int x = 0; x < width; ++x) {
            const double sx = x - dx;
            const double sy = y - dy;
            const int x0 = static_cast<int>(std::floor(sx));
            const int y0 = static_cast<int>(std::floor(sy));
            const double fx = sx - x0;
            const double fy = sy - y0;
            const double top = sample(x0, y0) * (1.0 - fx) + sample(x0 + 1, y0) * fx;
            const double bottom = sample(x0, y0 + 1) * (1.0 - fx)
                + sample(x0 + 1, y0 + 1) * fx;
            dst[x] = static_cast<quint16>(std::clamp(
                std::lround(top * (1.0 - fy) + bottom * fy), 0L, 65535L));
        }
    });
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    return result;
}

QVector<quint16> maximumCoverage16(const QVector<quint16> &source,
                                   const int width,
                                   const int height,
                                   const QSize radius,
                                   const std::atomic_bool *cancelRequested)
{
    if (width <= 0 || height <= 0
        || source.size() != static_cast<qsizetype>(width) * height) return {};
    const int rx = std::max(0, radius.width());
    const int ry = std::max(0, radius.height());
    if (rx == 0 && ry == 0) return source;
    QVector<quint16> horizontal(source.size(), 0);
    QVector<quint16> result(source.size(), 0);
    processRows(width, height, [&](const int y) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
        std::deque<int> deque;
        const quint16 *row = source.constData() + static_cast<qsizetype>(y) * width;
        quint16 *dst = horizontal.data() + static_cast<qsizetype>(y) * width;
        for (int x = -rx; x < width + rx; ++x) {
            const int add = x + rx;
            if (add >= 0 && add < width) {
                while (!deque.empty() && row[deque.back()] <= row[add]) deque.pop_back();
                deque.push_back(add);
            }
            const int removeBefore = x - rx;
            while (!deque.empty() && deque.front() < removeBefore) deque.pop_front();
            if (x >= 0 && x < width) dst[x] = deque.empty() ? 0 : row[deque.front()];
        }
    });
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    for (int x = 0; x < width; ++x) {
        std::deque<int> deque;
        for (int y = -ry; y < height + ry; ++y) {
            const int add = y + ry;
            if (add >= 0 && add < height) {
                const quint16 value = horizontal[static_cast<qsizetype>(add) * width + x];
                while (!deque.empty()
                       && horizontal[static_cast<qsizetype>(deque.back()) * width + x] <= value) {
                    deque.pop_back();
                }
                deque.push_back(add);
            }
            const int removeBefore = y - ry;
            while (!deque.empty() && deque.front() < removeBefore) deque.pop_front();
            if (y >= 0 && y < height) {
                result[static_cast<qsizetype>(y) * width + x] = deque.empty()
                    ? 0 : horizontal[static_cast<qsizetype>(deque.front()) * width + x];
            }
        }
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    }
    return result;
}

QVector<quint16> minimumCoverage16(const QVector<quint16> &source,
                                   const int width,
                                   const int height,
                                   const QSize radius,
                                   const std::atomic_bool *cancelRequested)
{
    if (width <= 0 || height <= 0
        || source.size() != static_cast<qsizetype>(width) * height) return {};
    const int rx = std::max(0, radius.width());
    const int ry = std::max(0, radius.height());
    if (rx == 0 && ry == 0) return source;
    QVector<quint16> horizontal(source.size(), 0);
    QVector<quint16> result(source.size(), 0);
    processRows(width, height, [&](const int y) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
        const quint16 *row = source.constData() + static_cast<qsizetype>(y) * width;
        quint16 *dst = horizontal.data() + static_cast<qsizetype>(y) * width;
        std::deque<int> deque;
        int nextAdd = 0;
        for (int x = 0; x < width; ++x) {
            const int targetAdd = std::min(width - 1, x + rx);
            while (nextAdd <= targetAdd) {
                while (!deque.empty() && row[deque.back()] >= row[nextAdd]) deque.pop_back();
                deque.push_back(nextAdd++);
            }
            const int removeBefore = x - rx;
            while (!deque.empty() && deque.front() < removeBefore) deque.pop_front();
            dst[x] = (x - rx < 0 || x + rx >= width || deque.empty())
                ? 0 : row[deque.front()];
        }
    });
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    for (int x = 0; x < width; ++x) {
        std::deque<int> deque;
        int nextAdd = 0;
        for (int y = 0; y < height; ++y) {
            const int targetAdd = std::min(height - 1, y + ry);
            while (nextAdd <= targetAdd) {
                const quint16 value = horizontal[static_cast<qsizetype>(nextAdd) * width + x];
                while (!deque.empty()
                       && horizontal[static_cast<qsizetype>(deque.back()) * width + x] >= value) {
                    deque.pop_back();
                }
                deque.push_back(nextAdd++);
            }
            const int removeBefore = y - ry;
            while (!deque.empty() && deque.front() < removeBefore) deque.pop_front();
            result[static_cast<qsizetype>(y) * width + x] =
                (y - ry < 0 || y + ry >= height || deque.empty())
                ? 0 : horizontal[static_cast<qsizetype>(deque.front()) * width + x];
        }
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
    }
    return result;
}

QVector<quint16> subtractCoverage16(const QVector<quint16> &left,
                                    const QVector<quint16> &right)
{
    if (left.size() != right.size()) return {};
    QVector<quint16> result(left.size());
    for (qsizetype i = 0; i < left.size(); ++i) {
        result[i] = left[i] > right[i]
            ? static_cast<quint16>(left[i] - right[i]) : 0;
    }
    return result;
}

QVector<quint16> gaussianCoverage16(const QVector<quint16> &source,
                                    const int width,
                                    const int height,
                                    const QSize radius,
                                    const std::atomic_bool *cancelRequested)
{
    if (width <= 0 || height <= 0
        || source.size() != static_cast<qsizetype>(width) * height) return {};
    if (radius.isNull()) return source;
    QImage rgba(QSize(width, height), QImage::Format_RGBA64);
    if (rgba.isNull()) return {};
    for (int y = 0; y < height; ++y) {
        auto *dst = reinterpret_cast<QRgba64 *>(rgba.scanLine(y));
        const quint16 *src = source.constData() + static_cast<qsizetype>(y) * width;
        for (int x = 0; x < width; ++x) {
            dst[x] = QRgba64::fromRgba64(0, 0, 0, src[x]);
        }
    }
    QImage blurred = SpatialFilterFoundation::gaussianBlurReference(
        rgba, radius, SpatialEdgeMode::Transparent, SpatialAlphaMode::StraightRgba,
        cancelRequested, [](const int widthValue, const int heightValue,
                            const std::function<void(int)> &row) {
            processRows(widthValue, heightValue, row);
        });
    if (blurred.isNull()) return {};
    blurred = blurred.convertToFormat(QImage::Format_RGBA64);
    QVector<quint16> result(source.size(), 0);
    for (int y = 0; y < height; ++y) {
        const auto *src = reinterpret_cast<const QRgba64 *>(blurred.constScanLine(y));
        quint16 *dst = result.data() + static_cast<qsizetype>(y) * width;
        for (int x = 0; x < width; ++x) dst[x] = src[x].alpha();
    }
    return result;
}

void squaredDistanceTransform1D(const QVector<double> &input, QVector<double> *output)
{
    if (!output) return;
    const int count = input.size();
    output->resize(count);
    if (count <= 0) return;
    QVector<int> sites(count);
    QVector<double> boundaries(count + 1);
    int k = 0;
    sites[0] = 0;
    boundaries[0] = -1.0e30;
    boundaries[1] = 1.0e30;
    for (int q = 1; q < count; ++q) {
        double intersection = 0.0;
        for (;;) {
            const int p = sites[k];
            intersection = ((input[q] + static_cast<double>(q) * q)
                            - (input[p] + static_cast<double>(p) * p))
                / (2.0 * (q - p));
            if (intersection > boundaries[k] || k == 0) break;
            --k;
        }
        ++k;
        sites[k] = q;
        boundaries[k] = intersection;
        boundaries[k + 1] = 1.0e30;
    }
    k = 0;
    for (int q = 0; q < count; ++q) {
        while (boundaries[k + 1] < q) ++k;
        const double delta = q - sites[k];
        (*output)[q] = delta * delta + input[sites[k]];
    }
}

QVector<double> squaredDistanceToCoverageClass(const QVector<quint16> &coverage,
                                               const int width,
                                               const int height,
                                               const bool featureIsInside,
                                               const std::atomic_bool *cancelRequested)
{
    if (width <= 0 || height <= 0
        || coverage.size() != static_cast<qsizetype>(width) * height) return {};
    constexpr double Far = 1.0e15;
    QVector<double> temporary(static_cast<qsizetype>(width) * height, Far);
    QVector<double> rowInput(width), rowOutput;
    for (int y = 0; y < height; ++y) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
        const quint16 *src = coverage.constData() + static_cast<qsizetype>(y) * width;
        for (int x = 0; x < width; ++x) {
            const bool inside = src[x] >= 32768u;
            rowInput[x] = (inside == featureIsInside) ? 0.0 : Far;
        }
        squaredDistanceTransform1D(rowInput, &rowOutput);
        std::copy(rowOutput.cbegin(), rowOutput.cend(),
                  temporary.begin() + static_cast<qsizetype>(y) * width);
    }
    QVector<double> result(static_cast<qsizetype>(width) * height, Far);
    QVector<double> columnInput(height), columnOutput;
    for (int x = 0; x < width; ++x) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
        for (int y = 0; y < height; ++y) {
            columnInput[y] = temporary[static_cast<qsizetype>(y) * width + x];
        }
        squaredDistanceTransform1D(columnInput, &columnOutput);
        for (int y = 0; y < height; ++y) {
            result[static_cast<qsizetype>(y) * width + x] = columnOutput[y];
        }
    }
    return result;
}

bool bevelLightingCoverage16(const QVector<quint16> &baseCoverage,
                             const int width,
                             const int height,
                             const double scaleX,
                             const double scaleY,
                             const LayerEffect &effect,
                             QVector<quint16> *highlight,
                             QVector<quint16> *shadow,
                             const std::atomic_bool *cancelRequested)
{
    if (!highlight || !shadow || width <= 0 || height <= 0
        || baseCoverage.size() != static_cast<qsizetype>(width) * height) return false;
    const QVector<double> distanceToInside = squaredDistanceToCoverageClass(
        baseCoverage, width, height, true, cancelRequested);
    const QVector<double> distanceToOutside = squaredDistanceToCoverageClass(
        baseCoverage, width, height, false, cancelRequested);
    if (distanceToInside.isEmpty() || distanceToOutside.isEmpty()) return false;

    const double scale = std::max(1.0e-6, 0.5 * (scaleX + scaleY));
    const double sizePixels = std::max(0.5, effect.size * scale);
    QVector<double> heightField(baseCoverage.size(), 0.0);
    QVector<double> influence(baseCoverage.size(), 0.0);
    for (qsizetype i = 0; i < baseCoverage.size(); ++i) {
        const bool inside = baseCoverage[i] >= 32768u;
        const double edgeDistance = std::max(
            0.0, std::sqrt(inside ? distanceToOutside[i] : distanceToInside[i]) - 0.5);
        const double signedDistance = inside ? edgeDistance : -edgeDistance;
        const double band = std::clamp(1.0 - std::abs(signedDistance) / sizePixels, 0.0, 1.0);
        double h = 0.0;
        double a = 0.0;
        switch (effect.bevelStyle) {
        case LayerEffectBevelStyle::InnerBevel:
            h = std::clamp(signedDistance / sizePixels, 0.0, 1.0);
            a = baseCoverage[i] / 65535.0;
            break;
        case LayerEffectBevelStyle::OuterBevel:
            h = std::clamp(1.0 + signedDistance / sizePixels, 0.0, 1.0);
            a = (65535u - baseCoverage[i]) / 65535.0;
            break;
        case LayerEffectBevelStyle::Emboss:
            h = std::clamp(0.5 + signedDistance / (2.0 * sizePixels), 0.0, 1.0);
            a = band;
            break;
        case LayerEffectBevelStyle::PillowEmboss:
            h = std::clamp(std::abs(signedDistance) / sizePixels, 0.0, 1.0);
            a = band;
            break;
        }
        if (effect.bevelDirection == LayerEffectBevelDirection::Down) h = 1.0 - h;
        heightField[i] = h;
        influence[i] = a;
    }

    if (effect.bevelSoften > 0.0) {
        QVector<quint16> encoded(heightField.size(), 0);
        for (qsizetype i = 0; i < heightField.size(); ++i) {
            encoded[i] = static_cast<quint16>(std::lround(
                std::clamp(heightField[i], 0.0, 1.0) * 65535.0));
        }
        const QSize softenRadius(
            std::clamp(qCeil(effect.bevelSoften * scaleX), 0,
                       SpatialFilterContract::DefaultMaximumRadius),
            std::clamp(qCeil(effect.bevelSoften * scaleY), 0,
                       SpatialFilterContract::DefaultMaximumRadius));
        encoded = gaussianCoverage16(encoded, width, height, softenRadius, cancelRequested);
        if (encoded.isEmpty()) return false;
        for (qsizetype i = 0; i < heightField.size(); ++i) {
            heightField[i] = encoded[i] / 65535.0;
        }
    }

    highlight->fill(0, baseCoverage.size());
    shadow->fill(0, baseCoverage.size());
    quint16 *highlightData = highlight->data();
    quint16 *shadowData = shadow->data();
    const double radians = effect.angleDegrees * std::numbers::pi / 180.0;
    const double altitude = effect.bevelAltitudeDegrees * std::numbers::pi / 180.0;
    const double cosAltitude = std::cos(altitude);
    const double lightX = std::cos(radians) * cosAltitude;
    const double lightY = -std::sin(radians) * cosAltitude;
    const double lightZ = std::sin(altitude);
    const double depthScale = (effect.bevelDepth / 100.0) * std::max(1.0, sizePixels * 0.5);
    const auto sampleHeight = [&](const int x, const int y) {
        const int sx = std::clamp(x, 0, width - 1);
        const int sy = std::clamp(y, 0, height - 1);
        return heightField[static_cast<qsizetype>(sy) * width + sx];
    };
    processRows(width, height, [&](const int y) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return;
        for (int x = 0; x < width; ++x) {
            const qsizetype index = static_cast<qsizetype>(y) * width + x;
            if (influence[index] <= 0.0) continue;
            const double dx = (sampleHeight(x + 1, y) - sampleHeight(x - 1, y)) * 0.5;
            const double dy = (sampleHeight(x, y + 1) - sampleHeight(x, y - 1)) * 0.5;
            double nx = -dx * depthScale;
            double ny = -dy * depthScale;
            double nz = 1.0;
            const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (length > 1.0e-12) { nx /= length; ny /= length; nz /= length; }
            const double dot = nx * lightX + ny * lightY + nz * lightZ;
            const double hiDenominator = std::max(1.0e-6, 1.0 - lightZ);
            const double shDenominator = std::max(1.0e-6, 1.0 + lightZ);
            const double hi = std::clamp((dot - lightZ) / hiDenominator, 0.0, 1.0)
                * influence[index];
            const double sh = std::clamp((lightZ - dot) / shDenominator, 0.0, 1.0)
                * influence[index];
            highlightData[index] = static_cast<quint16>(std::lround(hi * 65535.0));
            shadowData[index] = static_cast<quint16>(std::lround(sh * 65535.0));
        }
    });
    return !(cancelRequested && cancelRequested->load(std::memory_order_relaxed));
}

QImage colourisedCoverageRegion(const QVector<quint16> &coverage,
                                const int width,
                                const int height,
                                const QRect &expandedRegion,
                                const QRect &targetRegion,
                                const QColor &colour,
                                const QImage::Format format,
                                const QColorSpace &colourSpace)
{
    if (coverage.size() != static_cast<qsizetype>(width) * height
        || expandedRegion.isEmpty() || targetRegion.isEmpty()) return {};
    const QRect local(targetRegion.topLeft() - expandedRegion.topLeft(), targetRegion.size());
    if (!QRect(QPoint(0, 0), QSize(width, height)).contains(local)) return {};
    QImage straight(targetRegion.size(), QImage::Format_RGBA64);
    if (straight.isNull()) return {};
    const quint16 red = static_cast<quint16>(colour.red() * 257);
    const quint16 green = static_cast<quint16>(colour.green() * 257);
    const quint16 blue = static_cast<quint16>(colour.blue() * 257);
    for (int y = 0; y < local.height(); ++y) {
        auto *dst = reinterpret_cast<QRgba64 *>(straight.scanLine(y));
        const quint16 *src = coverage.constData()
            + static_cast<qsizetype>(local.y() + y) * width + local.x();
        for (int x = 0; x < local.width(); ++x) {
            dst[x] = QRgba64::fromRgba64(red, green, blue, src[x]);
        }
    }
    straight.setColorSpace(colourSpace);
    QImage result = straight.convertToFormat(format);
    result.setColorSpace(colourSpace);
    return result;
}

QColor layerEffectGradientColourAt(const LayerEffect &effect, double position)
{
    position = std::clamp(position, 0.0, 1.0);
    if (effect.gradientReverse) position = 1.0 - position;
    const QVector<GradientStop> &stops = effect.gradientStops;
    if (stops.isEmpty()) return Qt::transparent;
    if (position <= stops.constFirst().position) return stops.constFirst().colour;
    if (position >= stops.constLast().position) return stops.constLast().colour;
    int right = 1;
    while (right < stops.size() && stops.at(right).position < position) ++right;
    const GradientStop &a = stops.at(std::max(0, right - 1));
    const GradientStop &b = stops.at(std::min(right, static_cast<int>(stops.size()) - 1));
    const double span = std::max(1.0e-9, b.position - a.position);
    double t = (position - a.position) / span;
    if (effect.gradientInterpolation == GradientInterpolation::Constant) {
        t = position >= b.position ? 1.0 : 0.0;
    } else if (effect.gradientInterpolation == GradientInterpolation::Smooth) {
        t = t * t * (3.0 - 2.0 * t);
    }
    const auto mix = [t](const double x, const double y) {
        return x + (y - x) * t;
    };
    return QColor::fromRgbF(
        mix(a.colour.redF(), b.colour.redF()),
        mix(a.colour.greenF(), b.colour.greenF()),
        mix(a.colour.blueF(), b.colour.blueF()), 1.0);
}

QImage gradientCoverageRegion(const QVector<quint16> &coverage,
                              const int width,
                              const int height,
                              const QRect &expandedRegion,
                              const QRect &targetRegion,
                              const QRectF &documentBounds,
                              const double scaleX,
                              const double scaleY,
                              const LayerEffect &effect,
                              const QImage::Format format,
                              const QColorSpace &colourSpace,
                              const bool grayscale)
{
    if (coverage.size() != static_cast<qsizetype>(width) * height
        || expandedRegion.isEmpty() || targetRegion.isEmpty()
        || documentBounds.isEmpty() || scaleX <= 0.0 || scaleY <= 0.0) return {};
    const QRect local(targetRegion.topLeft() - expandedRegion.topLeft(), targetRegion.size());
    if (!QRect(QPoint(0, 0), QSize(width, height)).contains(local)) return {};

    const QPointF centre = documentBounds.center();
    const double radians = effect.gradientAngleDegrees * std::numbers::pi / 180.0;
    const QPointF direction(std::cos(radians), -std::sin(radians));
    const double halfProjectedExtent = std::max(
        0.5,
        std::abs(direction.x()) * documentBounds.width() * 0.5
            + std::abs(direction.y()) * documentBounds.height() * 0.5);
    const double linearSpan = std::max(1.0, 2.0 * halfProjectedExtent
        * effect.gradientScale / 100.0);
    const double radialX = std::max(0.5, documentBounds.width() * 0.5
        * effect.gradientScale / 100.0);
    const double radialY = std::max(0.5, documentBounds.height() * 0.5
        * effect.gradientScale / 100.0);

    QImage straight(targetRegion.size(), QImage::Format_RGBA64);
    if (straight.isNull()) return {};
    for (int y = 0; y < local.height(); ++y) {
        auto *dst = reinterpret_cast<QRgba64 *>(straight.scanLine(y));
        const quint16 *src = coverage.constData()
            + static_cast<qsizetype>(local.y() + y) * width + local.x();
        const double documentY = (targetRegion.y() + y + 0.5) / scaleY;
        for (int x = 0; x < local.width(); ++x) {
            const double documentX = (targetRegion.x() + x + 0.5) / scaleX;
            const double dx = documentX - centre.x();
            const double dy = documentY - centre.y();
            double t = 0.0;
            if (effect.gradientStyle == LayerEffectGradientStyle::Radial) {
                const double nx = dx / radialX;
                const double ny = dy / radialY;
                t = std::sqrt(nx * nx + ny * ny);
            } else {
                t = 0.5 + (dx * direction.x() + dy * direction.y()) / linearSpan;
            }
            QColor colour = layerEffectGradientColourAt(effect, t);
            if (grayscale) {
                const double grey = colour.redF() * 0.2126
                    + colour.greenF() * 0.7152 + colour.blueF() * 0.0722;
                colour = QColor::fromRgbF(grey, grey, grey, 1.0);
            }
            dst[x] = QRgba64::fromRgba64(
                static_cast<quint16>(std::lround(colour.redF() * 65535.0)),
                static_cast<quint16>(std::lround(colour.greenF() * 65535.0)),
                static_cast<quint16>(std::lround(colour.blueF() * 65535.0)),
                src[x]);
        }
    }
    straight.setColorSpace(colourSpace);
    QImage result = straight.convertToFormat(format);
    result.setColorSpace(colourSpace);
    return result;
}


struct CachedLayerEffectPasses {
    QVector<LayerEffectRenderPass> passes;
    QUuid ownerLayerId;
    qint64 bytes = 0;
    quint64 lastUse = 0;
};

class LayerEffectPassCache final {
public:
    static constexpr qint64 MaximumBytes = 192LL * 1024LL * 1024LL;

    bool lookup(const QByteArray &key, QVector<LayerEffectRenderPass> *passes)
    {
        if (key.isEmpty() || !passes) return false;
        std::lock_guard<std::mutex> guard(m_mutex);
        auto found = m_entries.find(key);
        if (found == m_entries.end()) return false;
        found->lastUse = ++m_serial;
        *passes = found->passes;
        return true;
    }

    void insert(const QByteArray &key, const QUuid &ownerLayerId,
                const QVector<LayerEffectRenderPass> &passes)
    {
        if (key.isEmpty()) return;
        qint64 bytes = 0;
        for (const LayerEffectRenderPass &pass : passes) {
            bytes += pass.image.sizeInBytes();
            if (bytes > MaximumBytes) return;
        }
        std::lock_guard<std::mutex> guard(m_mutex);
        if (auto existing = m_entries.find(key); existing != m_entries.end()) {
            m_bytes -= existing->bytes;
            m_entries.erase(existing);
        }
        CachedLayerEffectPasses entry;
        entry.passes = passes;
        entry.ownerLayerId = ownerLayerId;
        entry.bytes = bytes;
        entry.lastUse = ++m_serial;
        m_entries.insert(key, std::move(entry));
        m_bytes += bytes;
        while (m_bytes > MaximumBytes && !m_entries.isEmpty()) {
            auto victim = m_entries.begin();
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
                if (it->lastUse < victim->lastUse) victim = it;
            }
            m_bytes -= victim->bytes;
            m_entries.erase(victim);
        }
    }

    void invalidateLayers(const QSet<QUuid> &layerIds)
    {
        if (layerIds.isEmpty()) return;
        std::lock_guard<std::mutex> guard(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (layerIds.contains(it->ownerLayerId)) {
                m_bytes -= it->bytes;
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    std::mutex m_mutex;
    QHash<QByteArray, CachedLayerEffectPasses> m_entries;
    qint64 m_bytes = 0;
    quint64 m_serial = 0;
};

LayerEffectPassCache &layerEffectPassCache()
{
    static LayerEffectPassCache cache;
    return cache;
}

QByteArray layerEffectPassCacheKey(const QImage &source,
                                   const LayerNode &layer,
                                   const QRect &previewRegion,
                                   const QSize &documentSize,
                                   const QTransform &worldTransform,
                                   const ColourProcessingCompatibility processingCompatibility)
{
    QByteArray semantic;
    QDataStream stream(&semantic, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << QByteArrayLiteral("layer-effect-pass-cache-v4")
           << layer.id << static_cast<qint32>(layer.type)
           << previewRegion << source.size() << documentSize
           << worldTransform.m11() << worldTransform.m12() << worldTransform.m13()
           << worldTransform.m21() << worldTransform.m22() << worldTransform.m23()
           << worldTransform.m31() << worldTransform.m32() << worldTransform.m33()
           << static_cast<qint32>(source.depth())
           << static_cast<qint32>(processingCompatibility);
    // Only a legacy BaseImage that directly sources document pixels depends on
    // the source image's pixel identity. Raster/Vector/Text/Smart fx must not
    // be evicted just because content underneath them changed. Dimensions,
    // precision and colour-space identity remain part of every key.
    const bool directlyUsesDocumentSource = layer.type == LayerType::BaseImage
        && layer.rasterImage.isNull();
    stream << directlyUsesDocumentSource;
    if (directlyUsesDocumentSource) {
        stream << static_cast<quint64>(source.cacheKey());
    }
    stream << static_cast<quint64>(layer.rasterImage.cacheKey())
           << layer.rasterReferenceSize << layer.rasterReferenceOrigin
           << static_cast<quint64>(layer.maskImage.cacheKey())
           << layer.maskReferenceSize << layer.maskReferenceOrigin
           << layer.maskEnabled << layer.maskInverted
           << static_cast<quint64>(layer.vectorData.fingerprint())
           << static_cast<quint64>(layer.textData.fingerprint())
           << static_cast<quint64>(layer.smartPresentationImage.cacheKey())
           << layer.smartPresentationReferenceSize << layer.smartPresentationReferenceOrigin
           << static_cast<qint32>(layer.smartTransform.interpolation)
           << layer.smartSource.sourceId
           << std::max<quint64>(1, layer.smartSource.observedSourceRevision);
    for (const LiveFilter &filter : layer.liveFilters) {
        stream << filter.enabled;
        if (!filter.enabled) continue;
        stream << adjustmentRenderIdentity(filter.adjustment)
               << filter.maskEnabled << filter.maskInverted
               << static_cast<quint64>(filter.maskImage.cacheKey())
               << filter.maskReferenceSize << filter.maskReferenceOrigin;
    }
    stream << layerEffectStackRenderIdentity(layer.layerEffects);
    if (source.colorSpace().isValid()) {
        stream << QCryptographicHash::hash(source.colorSpace().iccProfile(),
                                           QCryptographicHash::Sha256);
    } else {
        stream << QByteArray();
    }
    return QCryptographicHash::hash(semantic, QCryptographicHash::Sha256);
}

bool layerEffectCompositesBehind(const LayerEffect &effect)
{
    if (effect.type == LayerEffectType::Stroke) {
        return effect.strokePosition == LayerEffectStrokePosition::Outside;
    }
    return effect.type == LayerEffectType::DropShadow
        || effect.type == LayerEffectType::OuterGlow;
}

QImage alphaCoverageFromRenderedLayer(const QImage &image)
{
    if (image.isNull()) return {};
    if (image.depth() > 32) {
        const QImage straight = image.convertToFormat(QImage::Format_RGBA64);
        QImage coverage(image.size(), QImage::Format_Grayscale16);
        if (coverage.isNull()) return {};
        for (int y = 0; y < image.height(); ++y) {
            const auto *src = reinterpret_cast<const QRgba64 *>(straight.constScanLine(y));
            auto *dst = reinterpret_cast<quint16 *>(coverage.scanLine(y));
            for (int x = 0; x < image.width(); ++x) dst[x] = src[x].alpha();
        }
        return coverage;
    }
    const QImage straight = image.convertToFormat(QImage::Format_RGBA8888);
    QImage coverage(image.size(), QImage::Format_Grayscale8);
    if (coverage.isNull()) return {};
    for (int y = 0; y < image.height(); ++y) {
        const uchar *src = straight.constScanLine(y);
        uchar *dst = coverage.scanLine(y);
        for (int x = 0; x < image.width(); ++x) dst[x] = src[x * 4 + 3];
    }
    return coverage;
}

QTransform imageToRegionTransform(const QSize &imageSize,
                                      const QSize &referenceSize,
                                      const QPointF &referenceOrigin,
                                      const QSize &previewSize,
                                      const QTransform &worldTransform,
                                      const QSize &documentSize,
                                      const QRect &region)
{
    if (imageSize.isEmpty() || referenceSize.isEmpty()
        || previewSize.isEmpty() || documentSize.isEmpty()) {
        return {};
    }
    const QTransform imageToReference = QTransform::fromScale(
        referenceSize.width() / static_cast<double>(imageSize.width()),
        referenceSize.height() / static_cast<double>(imageSize.height()));
    const QTransform documentToPreview = QTransform::fromScale(
        previewSize.width() / static_cast<double>(documentSize.width()),
        previewSize.height() / static_cast<double>(documentSize.height()));
    return imageToReference
        * QTransform::fromTranslate(referenceOrigin.x(), referenceOrigin.y())
        * worldTransform
        * documentToPreview
        * QTransform::fromTranslate(-region.left(), -region.top());
}

QImage imageRegion(const QImage &image,
                   const QSize &referenceSize,
                   const QPointF &referenceOrigin,
                   const QSize &previewSize,
                   const QTransform &worldTransform,
                   const QSize &documentSize,
                   const QRect &region,
                   const QImage::Format format,
                   const QColorSpace &space,
                   const bool forceOpaquePixelAlpha = false)
{
    if (image.isNull() || region.isEmpty()) {
        return transparentImage(region.size(), format, space);
    }

    const QSize effectiveReference = referenceSize.isValid() && !referenceSize.isEmpty()
        ? referenceSize : documentSize;
    const QTransform imageToRegion = imageToRegionTransform(image.size(),
                                                             effectiveReference,
                                                             referenceOrigin,
                                                             previewSize,
                                                             worldTransform,
                                                             documentSize,
                                                             region);
    // Preserve exact pixels for the overwhelmingly common 1:1 integer
    // translation case instead of sending them through QPainter's platform
    // raster pipeline. This makes full-frame and tiled CPU rendering byte
    // stable across Qt backends while retaining the general transformed path
    // below for scaling/rotation/projective layers.
    if (imageToRegion.type() <= QTransform::TxTranslate) {
        const double roundedDx = std::round(imageToRegion.dx());
        const double roundedDy = std::round(imageToRegion.dy());
        if (std::abs(imageToRegion.dx() - roundedDx) <= 1.0e-9
            && std::abs(imageToRegion.dy() - roundedDy) <= 1.0e-9) {
            const int dx = static_cast<int>(roundedDx);
            const int dy = static_cast<int>(roundedDy);
            const QRect sourceRect = QRect(-dx, -dy, region.width(), region.height())
                .intersected(image.rect());
            QImage output = transparentImage(region.size(), format, space);
            if (sourceRect.isEmpty()) return output;

            QImage patch = image.copy(sourceRect);
            if (forceOpaquePixelAlpha) {
                patch = patch.convertToFormat(
                    image.depth() > 32 ? QImage::Format_RGBA64
                                       : QImage::Format_RGBA8888);
                patch.detach();
                if (patch.depth() > 32) {
                    for (int y = 0; y < patch.height(); ++y) {
                        auto *row = reinterpret_cast<QRgba64 *>(patch.scanLine(y));
                        for (int x = 0; x < patch.width(); ++x) {
                            const QRgba64 pixel = row[x];
                            row[x] = QRgba64::fromRgba64(
                                pixel.red(), pixel.green(), pixel.blue(), 65535);
                        }
                    }
                } else {
                    for (int y = 0; y < patch.height(); ++y) {
                        uchar *row = patch.scanLine(y);
                        for (int x = 0; x < patch.width(); ++x) row[x * 4 + 3] = 255;
                    }
                }
            }
            patch = patch.convertToFormat(format);
            patch.setColorSpace(space);
            const QPoint destinationTopLeft(sourceRect.left() + dx,
                                             sourceRect.top() + dy);
            const int bytesPerPixel = output.depth() / 8;
            const qsizetype copyBytes = static_cast<qsizetype>(patch.width())
                * bytesPerPixel;
            for (int y = 0; y < patch.height(); ++y) {
                std::memcpy(output.scanLine(destinationTopLeft.y() + y)
                                + static_cast<qsizetype>(destinationTopLeft.x())
                                    * bytesPerPixel,
                            patch.constScanLine(y),
                            static_cast<std::size_t>(copyBytes));
            }
            return output;
        }
    }

    QImage drawImage = image;
    QTransform drawTransform = imageToRegion;
    if (forceOpaquePixelAlpha) {
        // Make only the source pixels needed by this tile opaque. This keeps
        // hidden RGB available without cloning every full-resolution layer for
        // every channel-preview tile.
        bool invertible = false;
        const QTransform regionToImage = imageToRegion.inverted(&invertible);
        if (!invertible) {
            return transparentImage(region.size(), format, space);
        }
        QRect sourceRect = regionToImage
                               .mapRect(QRectF(QPointF(0.0, 0.0), QSizeF(region.size())))
                               .normalized()
                               .adjusted(-2.0, -2.0, 2.0, 2.0)
                               .toAlignedRect()
                               .intersected(image.rect());
        if (sourceRect.isEmpty()) {
            return transparentImage(region.size(), format, space);
        }
        drawImage = image.copy(sourceRect).convertToFormat(
            image.depth() > 32 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        if (drawImage.isNull()) {
            return {};
        }
        drawImage.detach();
        if (drawImage.depth() > 32) {
            for (int y = 0; y < drawImage.height(); ++y) {
                auto *row = reinterpret_cast<QRgba64 *>(drawImage.scanLine(y));
                for (int x = 0; x < drawImage.width(); ++x) {
                    const QRgba64 pixel = row[x];
                    row[x] = QRgba64::fromRgba64(pixel.red(),
                                                 pixel.green(),
                                                 pixel.blue(),
                                                 65535);
                }
            }
        } else {
            for (int y = 0; y < drawImage.height(); ++y) {
                uchar *row = drawImage.scanLine(y);
                for (int x = 0; x < drawImage.width(); ++x) {
                    row[x * 4 + 3] = 255;
                }
            }
        }
        drawImage.setColorSpace(image.colorSpace());
        drawTransform = QTransform::fromTranslate(sourceRect.x(), sourceRect.y())
            * imageToRegion;
    }

    QImage output = transparentImage(region.size(), format, space);
    QPainter painter(&output);
    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                          image.size() != previewSize
                              || worldTransform.type() > QTransform::TxTranslate);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setTransform(drawTransform);
    painter.drawImage(QPointF(), drawImage);
    return output;
}

QImage smartImageRegion(const QImage &image,
                        const QUuid &sourceId,
                        const quint64 sourceRevision,
                        const QSize &referenceSize,
                        const QPointF &referenceOrigin,
                        const QSize &previewSize,
                        const QTransform &worldTransform,
                        const QSize &documentSize,
                        const QRect &region,
                        const QImage::Format format,
                        const QColorSpace &space,
                        const SmartTransformState &smartTransform,
                        const bool forceOpaquePixelAlpha,
                        const std::atomic_bool *cancelRequested)
{
    if (image.isNull() || region.isEmpty()) {
        return transparentImage(region.size(), format, space);
    }

    const QSize effectiveReference = referenceSize.isValid() && !referenceSize.isEmpty()
        ? referenceSize : documentSize;
    const QTransform imageToRegion = imageToRegionTransform(
        image.size(), effectiveReference, referenceOrigin, previewSize,
        worldTransform, documentSize, region);
    bool invertible = false;
    const QTransform regionToImage = imageToRegion.inverted(&invertible);
    if (!invertible) {
        return transparentImage(region.size(), format, space);
    }

    // Every transformed Smart tile is addressed by the exact source footprint
    // it can sample, rather than only by the source's global revision. A distant
    // Edit Contents change can therefore advance the authoritative revision
    // without forcing an unchanged transformed tile to be regenerated.
    const int halo = smartTransform.interpolation == TransformInterpolation::Lanczos3
        ? 4
        : (smartTransform.interpolation == TransformInterpolation::Bicubic
               ? 3
               : (smartTransform.interpolation == TransformInterpolation::Bilinear ? 2 : 1));
    const QRectF mappedBounds = regionToImage.mapRect(
        QRectF(QPointF(0.0, 0.0), QSizeF(region.size()))).normalized();
    const QRect sourceRect = mappedBounds.adjusted(-halo, -halo, halo, halo)
        .toAlignedRect().intersected(image.rect());
    if (sourceRect.isEmpty()) {
        return transparentImage(region.size(), format, space);
    }

    SmartLayerTileCache &cache = SmartLayerTileCache::instance();
    const quint64 sourceFingerprint = cache.sourceRegionFingerprint(
        image, sourceId, sourceRevision, sourceRect);
    const QByteArray transformedKey = SmartLayerTileCache::transformedKey(
        image, sourceId, sourceRevision, referenceSize, referenceOrigin,
        previewSize, worldTransform, documentSize, region, format, space,
        smartTransform, forceOpaquePixelAlpha, sourceFingerprint);
    if (const auto cached = cache.lookupTransformed(transformedKey)) {
        return *cached;
    }

    // Bilinear is the exact 0.14.0a-c compositor path, preserving appearance
    // for migrated Smart Layers. It is cached using the same source-footprint
    // identity as the exact high-quality reference modes below.
    if (smartTransform.interpolation == TransformInterpolation::Bilinear) {
        QImage output = imageRegion(image, referenceSize, referenceOrigin, previewSize,
                                    worldTransform, documentSize, region, format, space,
                                    forceOpaquePixelAlpha);
        if (!output.isNull()) {
            cache.storeTransformed(transformedKey, sourceId, sourceRevision, output);
        }
        return output;
    }

    // Convert only the source footprint required by this requested render
    // region. Cached 256x256 straight-RGBA64 source tiles make overlapping
    // Bicubic/Lanczos requests share conversion work without baking transforms.
    QImage source64 = cache.sourcePatch(image, sourceId, sourceRevision,
                                        sourceRect, cancelRequested);
    if (source64.isNull()) return {};
    source64.setColorSpace(image.colorSpace());

    // TransformSampling works in straight, unassociated 16-bit RGBA. RGB is
    // therefore never multiplied by Alpha and hidden colour survives repeated
    // Smart transforms as editable data.
    QImage sampled(region.size(), QImage::Format_RGBA64);
    if (sampled.isNull()) return {};
    sampled.fill(Qt::transparent);
    sampled.setColorSpace(space);
    const QPointF sourceOffset(sourceRect.left(), sourceRect.top());
    for (int y = 0; y < sampled.height(); ++y) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
            return {};
        }
        auto *row = reinterpret_cast<QRgba64 *>(sampled.scanLine(y));
        for (int x = 0; x < sampled.width(); ++x) {
            const QPointF sourcePoint = regionToImage.map(QPointF(x + 0.5, y + 0.5));
            QRgba64 pixel = sampleTransformRgba64(
                source64, source64.size(), sourcePoint - sourceOffset,
                smartTransform.interpolation);
            const bool insideSource = sourcePoint.x() >= 0.0
                && sourcePoint.y() >= 0.0
                && sourcePoint.x() < image.width()
                && sourcePoint.y() < image.height();
            if (forceOpaquePixelAlpha && insideSource) {
                pixel = QRgba64::fromRgba64(pixel.red(), pixel.green(),
                                            pixel.blue(), 65535);
            }
            row[x] = pixel;
        }
    }
    QImage output = sampled.convertToFormat(format);
    if (!output.isNull()) {
        output.setColorSpace(space);
        cache.storeTransformed(transformedKey, sourceId, sourceRevision, output);
    }
    return output;
}

QImage smartMaskRegion(const QImage &mask,
                       const QUuid &cacheIdentity,
                       const QSize &referenceSize,
                       const QPointF &referenceOrigin,
                       const QSize &previewSize,
                       const QTransform &worldTransform,
                       const QSize &documentSize,
                       const QRect &region,
                       bool enabled,
                       bool inverted,
                       const SmartTransformState &smartTransform,
                       const std::atomic_bool *cancelRequested);

QByteArray liveFilterStageCacheKey(const QImage &input,
                                   const LiveFilter &filter,
                                   const QRect &expandedRegion,
                                   const QSize &previewSize,
                                   const QSize &documentSize,
                                   const ColourProcessingCompatibility compatibility)
{
    const QByteArray filterBytes = adjustmentRenderIdentity(filter.adjustment);
    if (filterBytes.isEmpty()) return {};
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << QByteArrayLiteral("live-filter-stage-v1")
           << static_cast<qint64>(input.cacheKey())
           << static_cast<qint32>(input.width())
           << static_cast<qint32>(input.height())
           << static_cast<qint32>(input.format())
           << expandedRegion << previewSize << documentSize
           << static_cast<qint32>(compatibility)
           << filterBytes
           << filter.maskEnabled << filter.maskInverted
           << filter.maskReferenceSize << filter.maskReferenceOrigin
           << static_cast<qint64>(filter.maskImage.cacheKey());
    return QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
}

QImage smartLiveFilteredRegion(const LayerNode &layer,
                               const QImage &previewSource,
                               const QSize &documentSize,
                               const QTransform &worldTransform,
                               const QRect &region,
                               const QImage::Format format,
                               const QColorSpace &space,
                               const bool forceOpaquePixelAlpha,
                               const ColourProcessingCompatibility processingCompatibility,
                               const std::atomic_bool *cancelRequested)
{
    if (layer.liveFilters.isEmpty()) {
        return smartImageRegion(layer.smartPresentationImage,
                                layer.smartSource.sourceId,
                                layer.smartSource.observedSourceRevision,
                                layer.smartPresentationReferenceSize,
                                layer.smartPresentationReferenceOrigin,
                                previewSource.size(),
                                worldTransform,
                                documentSize,
                                region,
                                format,
                                space,
                                layer.smartTransform,
                                forceOpaquePixelAlpha,
                                cancelRequested);
    }

    bool anyEnabled = false;
    for (const LiveFilter &filter : layer.liveFilters) {
        anyEnabled = anyEnabled || filter.enabled;
    }
    if (!anyEnabled) {
        return smartImageRegion(layer.smartPresentationImage,
                                layer.smartSource.sourceId,
                                layer.smartSource.observedSourceRevision,
                                layer.smartPresentationReferenceSize,
                                layer.smartPresentationReferenceOrigin,
                                previewSource.size(),
                                worldTransform,
                                documentSize,
                                region,
                                format,
                                space,
                                layer.smartTransform,
                                forceOpaquePixelAlpha,
                                cancelRequested);
    }

    const QSize documentRadius = liveFilterStackSpatialRadius2D(layer.liveFilters);
    QRect expanded = region;
    if (!documentRadius.isEmpty()) {
        // Live Filters operate on the Smart Layer's transformed presentation, which may
        // legitimately extend outside the parent canvas. Expand in preview space without
        // clipping to the document bounds so off-canvas source pixels can still feed a
        // blur/sharpen/filter halo back into an in-canvas result.
        const double scaleX = previewSource.width()
            / static_cast<double>(std::max(1, documentSize.width()));
        const double scaleY = previewSource.height()
            / static_cast<double>(std::max(1, documentSize.height()));
        const int haloX = static_cast<int>(std::ceil(documentRadius.width() * scaleX))
            + SpatialFilterContract::DefaultSafetyPadding;
        const int haloY = static_cast<int>(std::ceil(documentRadius.height() * scaleY))
            + SpatialFilterContract::DefaultSafetyPadding;
        expanded = region.adjusted(-haloX, -haloY, haloX, haloY);
    }

    QImage current = smartImageRegion(layer.smartPresentationImage,
                                      layer.smartSource.sourceId,
                                      layer.smartSource.observedSourceRevision,
                                      layer.smartPresentationReferenceSize,
                                      layer.smartPresentationReferenceOrigin,
                                      previewSource.size(),
                                      worldTransform,
                                      documentSize,
                                      expanded,
                                      format,
                                      space,
                                      layer.smartTransform,
                                      forceOpaquePixelAlpha,
                                      cancelRequested);
    if (current.isNull()) return {};

    const double spatialScale = std::min(
        previewSource.width() / static_cast<double>(std::max(1, documentSize.width())),
        previewSource.height() / static_cast<double>(std::max(1, documentSize.height())));
    SmartLayerTileCache &cache = SmartLayerTileCache::instance();
    for (const LiveFilter &filter : layer.liveFilters) {
        if (!filter.enabled) continue;
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
        const QByteArray key = liveFilterStageCacheKey(
            current, filter, expanded, previewSource.size(), documentSize,
            processingCompatibility);
        if (!key.isEmpty()) {
            if (const auto cached = cache.lookupTransformed(key)) {
                current = *cached;
                continue;
            }
        }

        LayerNode operation;
        operation.type = LayerType::Adjustment;
        operation.setAdjustmentData(filter.adjustment);
        QImage filtered = applyAdjustmentToImage(current,
                                                 operation,
                                                 cancelRequested,
                                                 spatialScale,
                                                 expanded.topLeft(),
                                                 previewSource.size(),
                                                 processingCompatibility);
        if (filtered.isNull()) return {};
        filtered.setColorSpace(space);

        if (filter.hasMask() && filter.maskEnabled) {
            const QImage filterMask = smartMaskRegion(
                filter.maskImage, filter.id, filter.maskReferenceSize,
                filter.maskReferenceOrigin, previewSource.size(), worldTransform,
                documentSize, expanded, filter.maskEnabled, filter.maskInverted,
                layer.smartTransform, cancelRequested);
            if (filterMask.isNull() && !(filter.maskImage.size() == QSize(1, 1)
                                         && !filter.maskInverted
                                         && qGray(filter.maskImage.pixel(0, 0)) == 255)) {
                return {};
            }
            LayerNode maskOperation;
            maskOperation.type = LayerType::Adjustment;
            maskOperation.opacity = 1.0;
            maskOperation.blendMode = BlendMode::Copy;
            maskOperation.setAdjustmentData(filter.adjustment);
            maskOperation.maskImage = filterMask;
            maskOperation.maskReferenceSize = expanded.size();
            maskOperation.maskReferenceOrigin = QPointF();
            maskOperation.maskEnabled = true;
            maskOperation.maskInverted = false;
            compositeAdjustment(current, filtered, maskOperation, cancelRequested);
        } else {
            current = std::move(filtered);
        }
        if (!key.isEmpty()) {
            cache.storeTransformed(key,
                                   layer.smartSource.sourceId,
                                   layer.smartSource.observedSourceRevision,
                                   current);
        }
    }

    if (expanded == region) return current;
    const QRect crop(region.topLeft() - expanded.topLeft(), region.size());
    if (!QRect(QPoint(0, 0), current.size()).contains(crop)) return {};
    QImage result = current.copy(crop);
    result.setColorSpace(space);
    return result;
}

QImage maskRegion(const QImage &mask,
                  const QSize &referenceSize,
                  const QPointF &referenceOrigin,
                  const QSize &previewSize,
                  const QTransform &worldTransform,
                  const QSize &documentSize,
                  const QRect &region,
                  const bool enabled,
                  const bool inverted)
{
    if (!enabled || mask.isNull()) {
        return {};
    }
    if (!inverted
        && mask.size() == QSize(1, 1)
        && qGray(mask.pixel(0, 0)) == 255) {
        return {};
    }
    QImage effective = mask.convertToFormat(QImage::Format_Grayscale8);
    if (inverted) {
        effective.invertPixels(QImage::InvertRgb);
    }
    QImage extracted = imageRegion(effective,
                                   referenceSize,
                                   referenceOrigin,
                                   previewSize,
                                   worldTransform,
                                   documentSize,
                                   region,
                                   QImage::Format_ARGB32_Premultiplied,
                                   {});
    return extracted.isNull() ? QImage()
                              : extracted.convertToFormat(QImage::Format_Grayscale8);
}

QImage smartMaskRegion(const QImage &mask,
                       const QUuid &cacheIdentity,
                       const QSize &referenceSize,
                       const QPointF &referenceOrigin,
                       const QSize &previewSize,
                       const QTransform &worldTransform,
                       const QSize &documentSize,
                       const QRect &region,
                       const bool enabled,
                       const bool inverted,
                       const SmartTransformState &smartTransform,
                       const std::atomic_bool *cancelRequested)
{
    if (!enabled || mask.isNull()) return {};
    if (!inverted
        && mask.size() == QSize(1, 1)
        && qGray(mask.pixel(0, 0)) == 255) {
        return {};
    }
    auto &cache = SmartLayerTileCache::instance();
    const QByteArray transformedKey = SmartLayerTileCache::transformedKey(
        mask, cacheIdentity, 1,
        referenceSize, referenceOrigin, previewSize, worldTransform,
        documentSize, region, QImage::Format_Grayscale8, QColorSpace(),
        smartTransform, inverted);
    if (const auto cachedMask = cache.lookupTransformed(transformedKey)) {
        return *cachedMask;
    }
    // Bilinear is the exact 0.14.0a-c compatibility path. Other persisted
    // Smart sampling modes use the same straight 16-bit reference sampler as
    // source pixels so source and mask edges stay registered after transforms.
    if (smartTransform.interpolation == TransformInterpolation::Bilinear) {
        QImage output = maskRegion(mask, referenceSize, referenceOrigin, previewSize,
                                   worldTransform, documentSize, region, enabled, inverted);
        if (!output.isNull()) {
            cache.storeTransformed(transformedKey, cacheIdentity, 1, output);
        }
        return output;
    }
    const QSize effectiveReference = referenceSize.isValid() && !referenceSize.isEmpty()
        ? referenceSize : documentSize;
    const QTransform imageToRegion = imageToRegionTransform(
        mask.size(), effectiveReference, referenceOrigin, previewSize,
        worldTransform, documentSize, region);
    bool invertible = false;
    const QTransform regionToImage = imageToRegion.inverted(&invertible);
    if (!invertible) {
        QImage empty(region.size(), QImage::Format_Grayscale8);
        if (!empty.isNull()) empty.fill(0);
        return empty;
    }
    const int halo = smartTransform.interpolation == TransformInterpolation::Lanczos3
        ? 4 : (smartTransform.interpolation == TransformInterpolation::Bicubic ? 3 : 1);
    const QRectF mappedBounds = regionToImage.mapRect(
        QRectF(QPointF(0.0, 0.0), QSizeF(region.size()))).normalized();
    const QRect sourceRect = mappedBounds.adjusted(-halo, -halo, halo, halo)
        .toAlignedRect().intersected(mask.rect());
    if (sourceRect.isEmpty()) {
        QImage empty(region.size(), QImage::Format_Grayscale8);
        if (!empty.isNull()) empty.fill(0);
        return empty;
    }
    QImage effective = mask.copy(sourceRect).convertToFormat(QImage::Format_Grayscale16);
    if (effective.isNull()) return {};
    if (inverted) effective.invertPixels(QImage::InvertRgb);

    QImage sampled(region.size(), QImage::Format_Grayscale16);
    if (sampled.isNull()) return {};
    sampled.fill(0);
    const QPointF sourceOffset(sourceRect.left(), sourceRect.top());
    for (int y = 0; y < sampled.height(); ++y) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
            return {};
        }
        auto *row = reinterpret_cast<quint16 *>(sampled.scanLine(y));
        for (int x = 0; x < sampled.width(); ++x) {
            const QPointF sourcePoint = regionToImage.map(QPointF(x + 0.5, y + 0.5));
            row[x] = sampleTransformGrey16(
                effective, effective.size(), sourcePoint - sourceOffset,
                smartTransform.interpolation, 0);
        }
    }
    QImage output = sampled.convertToFormat(QImage::Format_Grayscale8);
    if (!output.isNull()) {
        cache.storeTransformed(transformedKey, cacheIdentity, 1, output);
    }
    return output;
}

bool mixPassThroughResult(QImage &canvas,
                          const QImage &before,
                          const QImage &after,
                          const QImage &mask,
                          const double opacity,
                          const std::atomic_bool *cancelRequested)
{
    const auto cancelled = [&] {
        return cancelRequested
            && cancelRequested->load(std::memory_order_relaxed);
    };
    if (cancelled()) {
        return false;
    }
    if (canvas.isNull() || before.isNull() || after.isNull()
        || before.size() != after.size() || opacity <= 0.0) {
        return opacity <= 0.0 && !canvas.isNull();
    }
    const double groupOpacity = std::clamp(opacity, 0.0, 1.0);
    if (groupOpacity >= 1.0 && mask.isNull()) {
        canvas = after;
        return !canvas.isNull() && !cancelled();
    }

    const bool sixteenBit = before.depth() > 32 || after.depth() > 32;
    const QImage::Format mixFormat = sixteenBit
        ? QImage::Format_RGBA64_Premultiplied
        : QImage::Format_ARGB32_Premultiplied;
    QImage mixed = before.convertToFormat(mixFormat);
    const QImage effect = after.convertToFormat(mixFormat);
    const QImage coverage = mask.isNull()
        ? QImage()
        : mask.convertToFormat(QImage::Format_Grayscale8);
    if (mixed.isNull() || effect.isNull() || mixed.size() != effect.size()
        || (!mask.isNull() && (coverage.isNull() || coverage.size() != mixed.size()))) {
        return false;
    }

    // convertToFormat() may return an implicitly shared image when the input
    // already has the requested format. Calling non-const scanLine() from
    // several QtConcurrent workers would then race the one-time detach and
    // corrupt QImage's allocation metadata. Detach once on the calling thread
    // and capture stable row pointers before dispatching any workers.
    mixed.detach();
    uchar *const mixedBits = mixed.bits();
    const uchar *const effectBits = effect.constBits();
    const uchar *const coverageBits = coverage.isNull() ? nullptr : coverage.constBits();
    if (!mixedBits || !effectBits || (!coverage.isNull() && !coverageBits)) {
        return false;
    }
    const qsizetype mixedStride = mixed.bytesPerLine();
    const qsizetype effectStride = effect.bytesPerLine();
    const qsizetype coverageStride = coverage.isNull() ? 0 : coverage.bytesPerLine();

    if (sixteenBit) {
        processRows(mixed.width(), mixed.height(), [&](const int y) {
            if (cancelled()) {
                return;
            }
            auto *destination = reinterpret_cast<QRgba64 *>(
                mixedBits + static_cast<qsizetype>(y) * mixedStride);
            const auto *source = reinterpret_cast<const QRgba64 *>(
                effectBits + static_cast<qsizetype>(y) * effectStride);
            const uchar *maskRow = coverageBits
                ? coverageBits + static_cast<qsizetype>(y) * coverageStride
                : nullptr;
            for (int x = 0; x < mixed.width(); ++x) {
                const double weight = groupOpacity
                    * (maskRow ? maskRow[x] / 255.0 : 1.0);
                if (weight <= 0.0) {
                    continue;
                }
                const QRgba64 basePixel = destination[x];
                const QRgba64 effectPixel = source[x];
                const auto interpolate = [weight](const quint16 baseValue,
                                                   const quint16 effectValue) {
                    return static_cast<quint16>(std::lround(
                        baseValue + (effectValue - static_cast<double>(baseValue)) * weight));
                };
                destination[x] = QRgba64::fromRgba64(
                    interpolate(basePixel.red(), effectPixel.red()),
                    interpolate(basePixel.green(), effectPixel.green()),
                    interpolate(basePixel.blue(), effectPixel.blue()),
                    interpolate(basePixel.alpha(), effectPixel.alpha()));
            }
        });
    } else {
        processRows(mixed.width(), mixed.height(), [&](const int y) {
            if (cancelled()) {
                return;
            }
            auto *destination = reinterpret_cast<QRgb *>(
                mixedBits + static_cast<qsizetype>(y) * mixedStride);
            const auto *source = reinterpret_cast<const QRgb *>(
                effectBits + static_cast<qsizetype>(y) * effectStride);
            const uchar *maskRow = coverageBits
                ? coverageBits + static_cast<qsizetype>(y) * coverageStride
                : nullptr;
            for (int x = 0; x < mixed.width(); ++x) {
                const double weight = groupOpacity
                    * (maskRow ? maskRow[x] / 255.0 : 1.0);
                if (weight <= 0.0) {
                    continue;
                }
                const QRgb basePixel = destination[x];
                const QRgb effectPixel = source[x];
                const auto interpolate = [weight](const int baseValue,
                                                   const int effectValue) {
                    return std::clamp(qRound(baseValue
                                             + (effectValue - baseValue) * weight),
                                      0,
                                      255);
                };
                destination[x] = qRgba(
                    interpolate(qRed(basePixel), qRed(effectPixel)),
                    interpolate(qGreen(basePixel), qGreen(effectPixel)),
                    interpolate(qBlue(basePixel), qBlue(effectPixel)),
                    interpolate(qAlpha(basePixel), qAlpha(effectPixel)));
            }
        });
    }

    if (cancelled()) {
        return false;
    }
    QImage converted = mixed.convertToFormat(workingFormat(before));
    if (converted.isNull()) {
        return false;
    }
    converted.setColorSpace(before.colorSpace());
    canvas = std::move(converted);
    return true;
}

struct AdjustmentInputCapture {
    QUuid targetLayerId;
    QImage image;
    bool found = false;
};

bool compositeLayersRegion(QImage &canvas,
                           const QImage &source,
                           const QVector<LayerNode> &layers,
                           const QRect &region,
                           const QSize &documentSize,
                           const QTransform &parentTransform,
                           const std::atomic_bool *cancelRequested,
                           const bool forceOpaquePixelAlpha,
                           const QImage::Format format,
                           const ColourProcessingCompatibility processingCompatibility,
                           AdjustmentInputCapture *capture = nullptr)
{

    for (auto iterator = layers.crbegin(); iterator != layers.crend(); ++iterator) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
            return false;
        }
        const LayerNode &layer = *iterator;
        if (capture && layer.type == LayerType::Adjustment
            && layer.id == capture->targetLayerId) {
            capture->image = canvas;
            capture->found = !capture->image.isNull();
            return capture->found;
        }
        if (!layer.visible || layer.opacity <= 0.0) {
            continue;
        }

        const QTransform worldTransform = layer.transform * parentTransform;
        if (layer.type == LayerType::Adjustment) {
            const double spatialScale = std::min(
                source.width() / static_cast<double>(std::max(1, documentSize.width())),
                source.height() / static_cast<double>(std::max(1, documentSize.height())));
            QImage adjusted = applyAdjustmentToImage(canvas,
                                                     layer,
                                                     cancelRequested,
                                                     spatialScale,
                                                     region.topLeft(),
                                                     source.size(),
                                                     processingCompatibility);
            if (!adjusted.isNull()) {
                LayerNode local = layer;
                local.maskImage = maskRegion(layer.maskImage,
                                             layer.maskReferenceSize,
                                             layer.maskReferenceOrigin,
                                             source.size(),
                                             worldTransform,
                                             documentSize,
                                             region,
                                             layer.maskEnabled,
                                             layer.maskInverted);
                local.maskEnabled = true;
                local.maskInverted = false;
                compositeAdjustment(canvas, adjusted, local, cancelRequested);
            }
            continue;
        }

        if (layer.type == LayerType::Group
            && layer.groupCompositeMode == GroupCompositeMode::PassThrough) {
            const QImage before = canvas;
            QImage after = canvas;
            if (!compositeLayersRegion(after,
                                       source,
                                       layer.children,
                                       region,
                                       documentSize,
                                       worldTransform,
                                       cancelRequested,
                                       forceOpaquePixelAlpha,
                                       format,
                                       processingCompatibility,
                                       capture)) {
                return false;
            }
            if (capture && capture->found) {
                return true;
            }
            const QImage localMask = maskRegion(layer.maskImage,
                                                layer.maskReferenceSize,
                                                layer.maskReferenceOrigin,
                                                source.size(),
                                                worldTransform,
                                                documentSize,
                                                region,
                                                layer.maskEnabled,
                                                layer.maskInverted);
            if (!mixPassThroughResult(canvas,
                                      before,
                                      after,
                                      localMask,
                                      layer.opacity,
                                      cancelRequested)) {
                return false;
            }
            continue;
        }

        QImage layerImage;
        switch (layer.type) {
        case LayerType::BaseImage: {
            const QImage &basePixels = layer.rasterImage.isNull() ? source : layer.rasterImage;
            layerImage = imageRegion(basePixels,
                                     layer.rasterReferenceSize,
                                     layer.rasterReferenceOrigin,
                                     source.size(),
                                     worldTransform,
                                     documentSize,
                                     region,
                                     format,
                                     source.colorSpace(),
                                     forceOpaquePixelAlpha);
            break;
        }
        case LayerType::Raster:
            layerImage = imageRegion(layer.rasterImage,
                                     layer.rasterReferenceSize,
                                     layer.rasterReferenceOrigin,
                                     source.size(),
                                     worldTransform,
                                     documentSize,
                                     region,
                                     format,
                                     source.colorSpace(),
                                     forceOpaquePixelAlpha);
            break;
        case LayerType::Vector:
            layerImage = VectorRasterizer::renderLayerRegion(
                layer, source.size(), region, documentSize, worldTransform, format,
                source.colorSpace(), forceOpaquePixelAlpha,
                source.format() == QImage::Format_Grayscale8 || source.format() == QImage::Format_Grayscale16,
                cancelRequested);
            break;
        case LayerType::Text:
            layerImage = TextRasterizer::renderLayerRegion(
                layer, source.size(), region, documentSize, worldTransform, format,
                source.colorSpace(), forceOpaquePixelAlpha,
                source.format() == QImage::Format_Grayscale8 || source.format() == QImage::Format_Grayscale16,
                cancelRequested);
            break;
        case LayerType::Smart:
            layerImage = smartLiveFilteredRegion(layer, source, documentSize,
                                                 worldTransform, region, format,
                                                 source.colorSpace(),
                                                 forceOpaquePixelAlpha,
                                                 processingCompatibility,
                                                 cancelRequested);
            break;
        case LayerType::Group:
            layerImage = transparentImage(region.size(), format, source.colorSpace());
            if (!compositeLayersRegion(layerImage,
                                       source,
                                       layer.children,
                                       region,
                                       documentSize,
                                       worldTransform,
                                       cancelRequested,
                                       forceOpaquePixelAlpha,
                                       format,
                                       processingCompatibility,
                                       capture)) {
                return false;
            }
            if (capture && capture->found) {
                return true;
            }
            break;
        case LayerType::Adjustment:
            break;
        }

        if (layerImage.isNull()) {
            continue;
        }
        QVector<LayerEffectRenderPass> effectPasses;
        const qsizetype renderableEffectCount = std::count_if(
            layer.layerEffects.cbegin(), layer.layerEffects.cend(),
            [](const LayerEffect &effect) {
                return effect.enabled && layerEffectTypeHasRenderer(effect.type)
                    && effect.effectOpacity > 0.0;
            });
        // Full-resolution CPU export/recovery renders can cover tens of millions
        // of pixels. Holding four RGBA64 fx passes at once would multiply peak
        // memory for no semantic benefit. Interactive/tiled requests keep the
        // shared multi-pass cache; large regions stream one effect at a time.
        const qint64 regionPixels = static_cast<qint64>(region.width()) * region.height();
        const bool hasRenderableBevel = std::any_of(
            layer.layerEffects.cbegin(), layer.layerEffects.cend(),
            [](const LayerEffect &effect) {
                return effect.enabled && effect.type == LayerEffectType::BevelEmboss
                    && layerEffectTypeHasRenderer(effect.type) && effect.effectOpacity > 0.0
                    && (effect.bevelHighlightOpacity > 0.0
                        || effect.bevelShadowOpacity > 0.0);
            });
        const bool streamLargeEffectPasses = (renderableEffectCount > 1 || hasRenderableBevel)
            && regionPixels > 4LL * 1024LL * 1024LL;
        const auto compositeSingleEffect = [&](const LayerEffect &effect,
                                               const bool behind) -> bool {
            if (!effect.enabled || !layerEffectTypeHasRenderer(effect.type)
                || effect.effectOpacity <= 0.0
                || (effect.type == LayerEffectType::BevelEmboss
                    && effect.bevelHighlightOpacity <= 0.0
                    && effect.bevelShadowOpacity <= 0.0)
                || layerEffectCompositesBehind(effect) != behind) {
                return true;
            }
            LayerNode singleEffectLayer = layer;
            singleEffectLayer.layerEffects = {effect};
            const auto compositePassRegion = [&](const QRect &effectRegion) -> bool {
                const QVector<LayerEffectRenderPass> singlePasses =
                    ImageProcessor::renderLayerEffectPasses(
                        source, singleEffectLayer, effectRegion, documentSize, worldTransform,
                        cancelRequested, processingCompatibility);
                if (singlePasses.isEmpty()) return effect.effectOpacity <= 0.0;
                const QRect localRect(effectRegion.topLeft() - region.topLeft(),
                                      effectRegion.size());
                if (!QRect(QPoint(0, 0), canvas.size()).contains(localRect)) return false;
                QImage destination = localRect == canvas.rect() ? canvas : canvas.copy(localRect);
                for (const LayerEffectRenderPass &pass : singlePasses) {
                    if (pass.behindSource != behind) continue;
                    composite(destination, pass.image, pass.blendMode,
                              pass.opacity * layer.opacity);
                }
                if (localRect != canvas.rect()) {
                    QPainter copyPainter(&canvas);
                    copyPainter.setCompositionMode(QPainter::CompositionMode_Source);
                    copyPainter.drawImage(localRect.topLeft(), destination);
                } else {
                    canvas = std::move(destination);
                }
                return !(cancelRequested
                    && cancelRequested->load(std::memory_order_relaxed));
            };
            // Signed-distance Bevel evaluation carries several full-resolution
            // scalar fields. Stream large exports/recovery renders in bounded
            // tiles while preserving each tile's full bevel halo.
            if (effect.type == LayerEffectType::BevelEmboss
                && regionPixels > 4LL * 1024LL * 1024LL) {
                constexpr int EffectStreamTile = 512;
                for (int y = region.top(); y <= region.bottom(); y += EffectStreamTile) {
                    for (int x = region.left(); x <= region.right(); x += EffectStreamTile) {
                        const QRect tile(x, y,
                            std::min(EffectStreamTile, region.right() - x + 1),
                            std::min(EffectStreamTile, region.bottom() - y + 1));
                        if (!compositePassRegion(tile)) return false;
                    }
                }
                return true;
            }
            return compositePassRegion(region);
        };
        if (!layer.layerEffects.isEmpty()) {
            if (streamLargeEffectPasses) {
                for (const LayerEffect &effect : layer.layerEffects) {
                    if (!compositeSingleEffect(effect, true)) return false;
                }
            } else {
                effectPasses = ImageProcessor::renderLayerEffectPasses(
                    source, layer, region, documentSize, worldTransform,
                    cancelRequested, processingCompatibility);
                if (cancelRequested
                    && cancelRequested->load(std::memory_order_relaxed)) {
                    return false;
                }
                for (const LayerEffectRenderPass &pass : effectPasses) {
                    if (pass.behindSource) {
                        composite(canvas, pass.image, pass.blendMode,
                                  pass.opacity * layer.opacity);
                    }
                }
            }
        }

        const QImage localMask = layer.type == LayerType::Smart
            ? smartMaskRegion(layer.maskImage,
                              layer.id,
                              layer.maskReferenceSize,
                              layer.maskReferenceOrigin,
                              source.size(),
                              worldTransform,
                              documentSize,
                              region,
                              layer.maskEnabled,
                              layer.maskInverted,
                              layer.smartTransform,
                              cancelRequested)
            : maskRegion(layer.maskImage,
                         layer.maskReferenceSize,
                         layer.maskReferenceOrigin,
                         source.size(),
                         worldTransform,
                         documentSize,
                         region,
                         layer.maskEnabled,
                         layer.maskInverted);
        applyMask(layerImage, localMask, true, false);
        composite(canvas, layerImage, layer.blendMode, layer.opacity);
        if (streamLargeEffectPasses) {
            for (const LayerEffect &effect : layer.layerEffects) {
                if (!compositeSingleEffect(effect, false)) return false;
            }
        } else {
            for (const LayerEffectRenderPass &pass : effectPasses) {
                if (!pass.behindSource) {
                    composite(canvas, pass.image, pass.blendMode,
                              pass.opacity * layer.opacity);
                }
            }
        }
    }
    return !(cancelRequested && cancelRequested->load(std::memory_order_relaxed));
}

QImage renderLayersRegion(const QImage &source,
                          const QVector<LayerNode> &layers,
                          const QRect &region,
                          const QSize &documentSize,
                          const QTransform &parentTransform,
                          const std::atomic_bool *cancelRequested,
                          const bool forceOpaquePixelAlpha,
                          const bool forceHighPrecision,
                          const ColourProcessingCompatibility processingCompatibility)
{
    const QImage::Format format = forceHighPrecision
        ? QImage::Format_RGBA64_Premultiplied
        : workingFormat(source);
    QImage canvas = transparentImage(region.size(), format, source.colorSpace());
    if (!compositeLayersRegion(canvas,
                               source,
                               layers,
                               region,
                               documentSize,
                               parentTransform,
                               cancelRequested,
                               forceOpaquePixelAlpha,
                               format,
                               processingCompatibility)) {
        return {};
    }
    return canvas;
}

SpatialFilterContract stackSpatialContract(const QVector<LayerNode> &layers)
{
    SpatialFilterContract contract;
    contract.documentRadius = maximumSpatialAdjustmentRadius2D(layers);
    contract.edgeMode = SpatialEdgeMode::Clamp;
    contract.alphaMode = SpatialAlphaMode::PreserveSourceAlpha;
    contract.quality = SpatialPreviewQuality::Final;
    contract.safetyPadding = SpatialFilterContract::DefaultSafetyPadding;
    contract.maximumRadius = SpatialFilterContract::DefaultMaximumRadius;
    return contract;
}

QRect regionWithSpatialHalo(const QImage &source,
                            const QVector<LayerNode> &layers,
                            const QRect &requested,
                            const QSize &documentSize)
{
    const QRect clipped = requested.intersected(source.rect());
    if (clipped.isEmpty()) return {};
    const SpatialFilterTilePlan spatialPlan = SpatialFilterFoundation::plan(
        clipped, source.size(), documentSize, stackSpatialContract(layers));
    // Extremely large cumulative stacks deliberately fall back to the full
    // source dependency rather than allocating an unbounded explicit halo.
    return spatialPlan.valid && !spatialPlan.dependencyBounds.isEmpty()
        ? spatialPlan.dependencyBounds : source.rect();
}

QImage renderLayersRegionWithSpatialHalo(const QImage &source,
                                         const QVector<LayerNode> &layers,
                                         const QRect &requested,
                                         const QSize &documentSize,
                                         const QTransform &parentTransform,
                                         const std::atomic_bool *cancelRequested,
                                         const bool forceOpaquePixelAlpha,
                                         const bool forceHighPrecision,
                                         const ColourProcessingCompatibility processingCompatibility)
{
    const QRect clipped = requested.intersected(source.rect());
    if (clipped.isEmpty()) return {};
    const QRect expanded = regionWithSpatialHalo(source, layers, clipped, documentSize);
    QImage rendered = renderLayersRegion(source, layers, expanded, documentSize,
                                         parentTransform, cancelRequested,
                                         forceOpaquePixelAlpha, forceHighPrecision,
                                         processingCompatibility);
    if (rendered.isNull()) return {};
    if (expanded == clipped) return rendered;
    QImage cropped = rendered.copy(QRect(clipped.topLeft() - expanded.topLeft(), clipped.size()));
    cropped.setColorSpace(rendered.colorSpace());
    return cropped;
}

QRect alphaBounds(const QImage &image)
{
    if (image.isNull()) {
        return {};
    }
    if (!image.hasAlphaChannel()) {
        return image.rect();
    }

    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    int left = rgba.width();
    int top = rgba.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar *row = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x) {
            if (row[x * 4 + 3] == 0) {
                continue;
            }
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    return right >= left && bottom >= top
        ? QRect(QPoint(left, top), QPoint(right, bottom))
        : QRect();
}

QRect maskCoverageBounds(const QImage &mask, const bool inverted)
{
    if (mask.isNull()) {
        return {};
    }
    if (!inverted
        && mask.size() == QSize(1, 1)
        && qGray(mask.pixel(0, 0)) == 255) {
        return {};
    }
    const QImage gray = mask.convertToFormat(QImage::Format_Grayscale8);
    int left = gray.width();
    int top = gray.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < gray.height(); ++y) {
        const uchar *row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            const int value = inverted ? 255 - row[x] : row[x];
            if (value == 0) {
                continue;
            }
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    return right >= left && bottom >= top
        ? QRect(QPoint(left, top), QPoint(right, bottom))
        : QRect();
}

QRectF scaleBoundsToReference(const QRect &bounds,
                              const QSize &imageSize,
                              const QSize &referenceSize,
                              const QPointF &referenceOrigin)
{
    if (bounds.isEmpty() || imageSize.isEmpty() || referenceSize.isEmpty()) {
        return {};
    }
    const double sx = referenceSize.width() / static_cast<double>(imageSize.width());
    const double sy = referenceSize.height() / static_cast<double>(imageSize.height());
    return QRectF(referenceOrigin.x() + bounds.x() * sx,
                  referenceOrigin.y() + bounds.y() * sy,
                  bounds.width() * sx,
                  bounds.height() * sy);
}

QRectF layerContentBounds(const QImage &source,
                          const LayerNode &layer,
                          const QSize &documentSize,
                          const QTransform &parentTransform)
{
    if (!layer.visible || layer.opacity <= 0.0) {
        return {};
    }
    const QTransform worldTransform = layer.transform * parentTransform;
    QRectF result;

    switch (layer.type) {
    case LayerType::BaseImage: {
        const QImage &basePixels = layer.rasterImage.isNull() ? source : layer.rasterImage;
        const QRectF local = scaleBoundsToReference(alphaBounds(basePixels),
                                                    basePixels.size(),
                                                    layer.rasterReferenceSize.isEmpty()
                                                        ? documentSize
                                                        : layer.rasterReferenceSize,
                                                    layer.rasterReferenceOrigin);
        result = worldTransform.mapRect(local);
        break;
    }
    case LayerType::Raster: {
        const QRectF local = scaleBoundsToReference(alphaBounds(layer.rasterImage),
                                                    layer.rasterImage.size(),
                                                    layer.rasterReferenceSize.isEmpty()
                                                        ? documentSize
                                                        : layer.rasterReferenceSize,
                                                    layer.rasterReferenceOrigin);
        result = worldTransform.mapRect(local);
        break;
    }
    case LayerType::Adjustment:
        return {};
    case LayerType::Vector:
        result = VectorRasterizer::contentBounds(layer, worldTransform);
        break;
    case LayerType::Text:
        result = TextRasterizer::contentBounds(layer, worldTransform);
        break;
    case LayerType::Smart:
        if (!layer.smartPresentationImage.isNull()) {
            const QRectF local = scaleBoundsToReference(
                alphaBounds(layer.smartPresentationImage),
                layer.smartPresentationImage.size(),
                layer.smartPresentationReferenceSize.isEmpty()
                    ? layer.smartPresentationImage.size()
                    : layer.smartPresentationReferenceSize,
                layer.smartPresentationReferenceOrigin);
            result = worldTransform.mapRect(local);
            const QSize liveRadius = liveFilterStackSpatialRadius2D(layer.liveFilters);
            if (!result.isEmpty() && !liveRadius.isEmpty()) {
                result.adjust(-liveRadius.width(), -liveRadius.height(),
                              liveRadius.width(), liveRadius.height());
            }
        }
        break;
    case LayerType::Group:
        for (const LayerNode &child : layer.children) {
            const QRectF childBounds = layerContentBounds(source,
                                                          child,
                                                          documentSize,
                                                          worldTransform);
            if (!childBounds.isEmpty()) {
                result = result.isEmpty() ? childBounds : result.united(childBounds);
            }
        }
        break;
    }

    const bool hasEffectiveMask = layer.maskEnabled
        && !layer.maskImage.isNull()
        && (layer.maskInverted
            || !(layer.maskImage.size() == QSize(1, 1)
                 && qGray(layer.maskImage.pixel(0, 0)) == 255));
    if (hasEffectiveMask) {
        const QRect maskBounds = maskCoverageBounds(layer.maskImage, layer.maskInverted);
        if (maskBounds.isEmpty()) {
            return {};
        }
        const QRectF localMask = scaleBoundsToReference(maskBounds,
                                                        layer.maskImage.size(),
                                                        layer.maskReferenceSize.isEmpty()
                                                            ? documentSize
                                                            : layer.maskReferenceSize,
                                                        layer.maskReferenceOrigin);
        const QRectF documentMask = worldTransform.mapRect(localMask);
        result = result.intersected(documentMask);
    }
    if (!result.isEmpty() && layerTypeSupportsLayerEffects(layer.type)) {
        const QSize effectRadius = layerEffectStackSpatialRadius2D(layer.layerEffects);
        if (!effectRadius.isEmpty()) {
            result.adjust(-effectRadius.width(), -effectRadius.height(),
                          effectRadius.width(), effectRadius.height());
        }
    }
    return result;
}

bool findLayerWithWorldTransform(const QVector<LayerNode> &layers,
                                 const QUuid &layerId,
                                 const QTransform &parentTransform,
                                 const LayerNode **layerResult,
                                 QTransform *worldTransformResult)
{
    for (const LayerNode &layer : layers) {
        const QTransform worldTransform = layer.transform * parentTransform;
        if (layer.id == layerId) {
            if (layerResult) {
                *layerResult = &layer;
            }
            if (worldTransformResult) {
                *worldTransformResult = worldTransform;
            }
            return true;
        }
        if (findLayerWithWorldTransform(layer.children,
                                        layerId,
                                        worldTransform,
                                        layerResult,
                                        worldTransformResult)) {
            return true;
        }
    }
    return false;
}

void collectSelectedBounds(const QImage &source,
                           const QVector<LayerNode> &layers,
                           const QSize &documentSize,
                           const QTransform &parentTransform,
                           const QSet<QUuid> &selected,
                           QRectF *bounds)
{
    for (const LayerNode &layer : layers) {
        const QTransform worldTransform = layer.transform * parentTransform;
        if (selected.contains(layer.id)) {
            const QRectF layerBounds = layerContentBounds(source,
                                                          layer,
                                                          documentSize,
                                                          parentTransform);
            if (!layerBounds.isEmpty()) {
                *bounds = bounds->isEmpty() ? layerBounds : bounds->united(layerBounds);
            }
            continue;
        }
        collectSelectedBounds(source,
                              layer.children,
                              documentSize,
                              worldTransform,
                              selected,
                              bounds);
    }
}

} // namespace

QImage ImageProcessor::renderSmartMaskRegion(
    const QImage &mask,
    const QUuid &cacheIdentity,
    const QSize &referenceSize,
    const QPointF &referenceOrigin,
    const QSize &previewSize,
    const QTransform &worldTransform,
    const QSize &documentSize,
    const QRect &region,
    const bool enabled,
    const bool inverted,
    const SmartTransformState &smartTransform,
    const std::atomic_bool *cancelRequested)
{
    return smartMaskRegion(mask, cacheIdentity, referenceSize, referenceOrigin,
                           previewSize, worldTransform, documentSize, region,
                           enabled, inverted, smartTransform, cancelRequested);
}

QImage ImageProcessor::render(const QImage &source,
                              const QVector<LayerNode> &layers,
                              const std::atomic_bool *cancelRequested,
                              const QSize &documentSize,
                              const ColourProcessingCompatibility processingCompatibility)
{
    if (source.isNull()) {
        return {};
    }
    const QSize reference = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize
        : source.size();
    return renderLayersRegion(source,
                              layers,
                              source.rect(),
                              reference,
                              QTransform(),
                              cancelRequested,
                              false,
                              source.depth() > 32,
                              processingCompatibility);
}

QImage ImageProcessor::renderRegion(const QImage &source,
                                    const QVector<LayerNode> &layers,
                                    const QRect &previewRegion,
                                    const QSize &documentSize,
                                    const std::atomic_bool *cancelRequested,
                                    const ColourProcessingCompatibility processingCompatibility)
{
    if (source.isNull()) {
        return {};
    }
    const QRect region = previewRegion.intersected(source.rect());
    if (region.isEmpty()) {
        return {};
    }
    const QSize reference = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize
        : source.size();
    return renderLayersRegionWithSpatialHalo(source,
                                             layers,
                                             region,
                                             reference,
                                             QTransform(),
                                             cancelRequested,
                                             false,
                                             source.depth() > 32,
                                             processingCompatibility);
}

QImage ImageProcessor::renderUnclippedRegion(
    const QImage &source,
    const QVector<LayerNode> &layers,
    const QRect &previewRegion,
    const QSize &documentSize,
    const std::atomic_bool *cancelRequested,
    const ColourProcessingCompatibility processingCompatibility)
{
    if (source.isNull() || previewRegion.isEmpty()) {
        return {};
    }
    const QSize reference = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize
        : source.size();

    // renderLayersRegionWithSpatialHalo() intentionally clamps ordinary tile
    // requests to source.rect(). Transform foregrounds are different: an
    // editable raster can have a negative/off-canvas reference origin, and the
    // source document merely defines the document-to-preview scale. Expand the
    // arbitrary request by the complete stack dependency instead, render it in
    // one exact CPU pass, then crop back to the requested surface.
    const QSize documentRadius = maximumSpatialAdjustmentRadius2D(layers);
    const double scaleX = source.width()
        / static_cast<double>(std::max(1, reference.width()));
    const double scaleY = source.height()
        / static_cast<double>(std::max(1, reference.height()));
    const int padding = SpatialFilterContract::DefaultSafetyPadding;
    const int haloX = std::max(0, static_cast<int>(std::ceil(documentRadius.width() * scaleX))
                                  + padding);
    const int haloY = std::max(0, static_cast<int>(std::ceil(documentRadius.height() * scaleY))
                                  + padding);
    const QRect expanded = previewRegion.adjusted(-haloX, -haloY,
                                                   haloX, haloY);
    if (expanded.isEmpty()) {
        return {};
    }

    QImage rendered = renderLayersRegion(source,
                                         layers,
                                         expanded,
                                         reference,
                                         QTransform(),
                                         cancelRequested,
                                         false,
                                         source.depth() > 32,
                                         processingCompatibility);
    if (rendered.isNull()) {
        return {};
    }
    if (expanded == previewRegion) {
        return rendered;
    }
    const QRect crop(previewRegion.topLeft() - expanded.topLeft(),
                     previewRegion.size());
    QImage result = rendered.copy(crop);
    result.setColorSpace(rendered.colorSpace());
    return result;
}

QImage ImageProcessor::renderUnclippedRegionPreservingHiddenRgb(
    const QImage &source,
    const QVector<LayerNode> &layers,
    const QRect &previewRegion,
    const QSize &documentSize,
    const std::atomic_bool *cancelRequested,
    const ColourProcessingCompatibility processingCompatibility)
{
    if (source.isNull() || previewRegion.isEmpty()) return {};
    const QSize reference = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize : source.size();
    const QSize documentRadius = maximumSpatialAdjustmentRadius2D(layers);
    const double scaleX = source.width()
        / static_cast<double>(std::max(1, reference.width()));
    const double scaleY = source.height()
        / static_cast<double>(std::max(1, reference.height()));
    const int padding = SpatialFilterContract::DefaultSafetyPadding;
    const int haloX = std::max(0, static_cast<int>(std::ceil(documentRadius.width() * scaleX))
                                  + padding);
    const int haloY = std::max(0, static_cast<int>(std::ceil(documentRadius.height() * scaleY))
                                  + padding);
    const QRect expanded = previewRegion.adjusted(-haloX, -haloY, haloX, haloY);
    if (expanded.isEmpty()) return {};

    const QImage composite = renderLayersRegion(source, layers, expanded, reference,
                                                 QTransform(), cancelRequested,
                                                 false, true, processingCompatibility);
    if (composite.isNull() || (cancelRequested && cancelRequested->load())) return composite;
    const QImage rgbReference = renderLayersRegion(source, layers, expanded, reference,
                                                    QTransform(), cancelRequested,
                                                    true, true, processingCompatibility);
    if (rgbReference.isNull() || rgbReference.size() != composite.size()) return {};

    QImage output;
    if (source.depth() > 32) {
        output = composite.convertToFormat(QImage::Format_RGBA64);
        const QImage rgb = rgbReference.convertToFormat(QImage::Format_RGBA64);
        output.detach();
        for (int y = 0; y < output.height(); ++y) {
            auto *dst = reinterpret_cast<QRgba64 *>(output.scanLine(y));
            const auto *src = reinterpret_cast<const QRgba64 *>(rgb.constScanLine(y));
            for (int x = 0; x < output.width(); ++x) {
                if (dst[x].alpha() == 0) {
                    dst[x] = QRgba64::fromRgba64(src[x].red(), src[x].green(), src[x].blue(), 0);
                }
            }
        }
    } else {
        output = composite.convertToFormat(QImage::Format_RGBA8888);
        const QImage rgb = rgbReference.convertToFormat(QImage::Format_RGBA8888);
        output.detach();
        for (int y = 0; y < output.height(); ++y) {
            uchar *dst = output.scanLine(y);
            const uchar *src = rgb.constScanLine(y);
            for (int x = 0; x < output.width(); ++x) {
                const int offset = x * 4;
                if (dst[offset + 3] == 0) {
                    dst[offset] = src[offset];
                    dst[offset + 1] = src[offset + 1];
                    dst[offset + 2] = src[offset + 2];
                }
            }
        }
    }
    if (expanded != previewRegion) {
        output = output.copy(QRect(previewRegion.topLeft() - expanded.topLeft(),
                                   previewRegion.size()));
    }
    output.setColorSpace(composite.colorSpace());
    output.setDevicePixelRatio(source.devicePixelRatio());
    output.setDotsPerMeterX(source.dotsPerMeterX());
    output.setDotsPerMeterY(source.dotsPerMeterY());
    return output;
}


QVector<LayerEffectRenderPass> ImageProcessor::renderLayerEffectPasses(
    const QImage &source,
    const LayerNode &layer,
    const QRect &previewRegion,
    const QSize &documentSize,
    const QTransform &worldTransform,
    const std::atomic_bool *cancelRequested,
    const ColourProcessingCompatibility processingCompatibility)
{
    QVector<LayerEffectRenderPass> passes;
    if (source.isNull() || previewRegion.isEmpty()
        || !layerTypeSupportsLayerEffects(layer.type) || layer.layerEffects.isEmpty()) {
        return passes;
    }
    bool hasRenderable = false;
    for (const LayerEffect &effect : layer.layerEffects) {
        hasRenderable = hasRenderable
            || (effect.enabled && layerEffectTypeHasRenderer(effect.type));
    }
    if (!hasRenderable) return passes;

    const QSize resolvedDocumentSize = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize : source.size();
    const QByteArray cacheKey = layerEffectPassCacheKey(
        source, layer, previewRegion, resolvedDocumentSize, worldTransform,
        processingCompatibility);
    if (layerEffectPassCache().lookup(cacheKey, &passes)) return passes;
    const double scaleX = source.width()
        / static_cast<double>(std::max(1, resolvedDocumentSize.width()));
    const double scaleY = source.height()
        / static_cast<double>(std::max(1, resolvedDocumentSize.height()));
    const QSize documentHalo = layerEffectStackSpatialRadius2D(layer.layerEffects);
    const int haloX = std::max(2, qCeil(documentHalo.width() * scaleX) + 2);
    const int haloY = std::max(2, qCeil(documentHalo.height() * scaleY) + 2);
    const QRect expandedRegion = previewRegion.adjusted(-haloX, -haloY, haloX, haloY);
    if (expandedRegion.isEmpty()) return passes;

    LayerNode coverageLayer = layer;
    coverageLayer.opacity = 1.0;
    coverageLayer.blendMode = BlendMode::Copy;
    coverageLayer.transform = worldTransform;
    coverageLayer.layerEffects.clear();
    QImage masked = renderUnclippedRegion(
        source, {coverageLayer}, expandedRegion, resolvedDocumentSize,
        cancelRequested, processingCompatibility);
    if (masked.isNull() || (cancelRequested
        && cancelRequested->load(std::memory_order_relaxed))) return {};
    const QImage coverageImage = alphaCoverageFromRenderedLayer(masked);
    QVector<quint16> baseCoverage = coveragePlane16(coverageImage);
    if (baseCoverage.isEmpty()) return {};
    const int width = expandedRegion.width();
    const int height = expandedRegion.height();
    const qint64 pixelCount = static_cast<qint64>(width) * height;
    if (pixelCount <= 0 || pixelCount > 128LL * 1024LL * 1024LL) return {};

    LayerNode boundsLayer = layer;
    boundsLayer.transform = worldTransform;
    boundsLayer.layerEffects.clear();
    const QRectF effectDocumentBounds = layerContentBounds(
        source, boundsLayer, resolvedDocumentSize, QTransform());

    passes.reserve(layer.layerEffects.size());
    for (const LayerEffect &effect : layer.layerEffects) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) return {};
        if (!effect.enabled || !layerEffectTypeHasRenderer(effect.type)
            || effect.effectOpacity <= 0.0) continue;

        const QSize blurRadius(
            std::clamp(qCeil(effect.size * scaleX), 0,
                       SpatialFilterContract::DefaultMaximumRadius),
            std::clamp(qCeil(effect.size * scaleY), 0,
                       SpatialFilterContract::DefaultMaximumRadius));
        const QSize spreadRadius(
            std::clamp(qCeil(effect.size * effect.spread * 0.01 * scaleX), 0,
                       SpatialFilterContract::DefaultMaximumRadius),
            std::clamp(qCeil(effect.size * effect.spread * 0.01 * scaleY), 0,
                       SpatialFilterContract::DefaultMaximumRadius));
        QVector<quint16> alpha;
        const bool behind = layerEffectCompositesBehind(effect);

        switch (effect.type) {
        case LayerEffectType::DropShadow: {
            const double radians = effect.angleDegrees * std::numbers::pi / 180.0;
            const double dx = -std::cos(radians) * effect.distance * scaleX;
            const double dy = std::sin(radians) * effect.distance * scaleY;
            alpha = shiftedCoverage16(baseCoverage, width, height, dx, dy, cancelRequested);
            if (alpha.isEmpty()) return {};
            alpha = maximumCoverage16(alpha, width, height, spreadRadius, cancelRequested);
            if (alpha.isEmpty()) return {};
            alpha = gaussianCoverage16(alpha, width, height, blurRadius, cancelRequested);
            break;
        }
        case LayerEffectType::OuterGlow: {
            alpha = maximumCoverage16(baseCoverage, width, height, spreadRadius,
                                      cancelRequested);
            if (alpha.isEmpty()) return {};
            alpha = gaussianCoverage16(alpha, width, height, blurRadius, cancelRequested);
            if (alpha.isEmpty()) return {};
            for (qsizetype i = 0; i < alpha.size(); ++i) {
                const quint32 outside = 65535u - baseCoverage[i];
                alpha[i] = static_cast<quint16>(
                    (static_cast<quint32>(alpha[i]) * outside + 32767u) / 65535u);
            }
            break;
        }
        case LayerEffectType::InnerShadow: {
            const double radians = effect.angleDegrees * std::numbers::pi / 180.0;
            const double dx = -std::cos(radians) * effect.distance * scaleX;
            const double dy = std::sin(radians) * effect.distance * scaleY;
            QVector<quint16> shifted = shiftedCoverage16(
                baseCoverage, width, height, dx, dy, cancelRequested);
            if (shifted.isEmpty()) return {};
            for (quint16 &value : shifted) value = static_cast<quint16>(65535u - value);
            alpha = maximumCoverage16(shifted, width, height, spreadRadius,
                                      cancelRequested);
            if (alpha.isEmpty()) return {};
            alpha = gaussianCoverage16(alpha, width, height, blurRadius, cancelRequested);
            if (alpha.isEmpty()) return {};
            for (qsizetype i = 0; i < alpha.size(); ++i) {
                alpha[i] = static_cast<quint16>(
                    (static_cast<quint32>(alpha[i]) * baseCoverage[i] + 32767u) / 65535u);
            }
            break;
        }
        case LayerEffectType::InnerGlow: {
            alpha.resize(baseCoverage.size());
            for (qsizetype i = 0; i < baseCoverage.size(); ++i) {
                alpha[i] = static_cast<quint16>(65535u - baseCoverage[i]);
            }
            alpha = maximumCoverage16(alpha, width, height, spreadRadius,
                                      cancelRequested);
            if (alpha.isEmpty()) return {};
            alpha = gaussianCoverage16(alpha, width, height, blurRadius, cancelRequested);
            if (alpha.isEmpty()) return {};
            for (qsizetype i = 0; i < alpha.size(); ++i) {
                alpha[i] = static_cast<quint16>(
                    (static_cast<quint32>(alpha[i]) * baseCoverage[i] + 32767u) / 65535u);
            }
            break;
        }
        case LayerEffectType::Stroke: {
            const QSize fullRadius(
                std::clamp(qCeil(effect.size * scaleX), 0,
                           SpatialFilterContract::DefaultMaximumRadius),
                std::clamp(qCeil(effect.size * scaleY), 0,
                           SpatialFilterContract::DefaultMaximumRadius));
            if (fullRadius.isNull()) continue;
            if (effect.strokePosition == LayerEffectStrokePosition::Outside) {
                QVector<quint16> dilated = maximumCoverage16(
                    baseCoverage, width, height, fullRadius, cancelRequested);
                if (dilated.isEmpty()) return {};
                alpha = subtractCoverage16(dilated, baseCoverage);
            } else if (effect.strokePosition == LayerEffectStrokePosition::Inside) {
                QVector<quint16> eroded = minimumCoverage16(
                    baseCoverage, width, height, fullRadius, cancelRequested);
                if (eroded.isEmpty()) return {};
                alpha = subtractCoverage16(baseCoverage, eroded);
            } else {
                const QSize halfRadius(
                    std::clamp(qCeil(effect.size * 0.5 * scaleX), 1,
                               SpatialFilterContract::DefaultMaximumRadius),
                    std::clamp(qCeil(effect.size * 0.5 * scaleY), 1,
                               SpatialFilterContract::DefaultMaximumRadius));
                QVector<quint16> dilated = maximumCoverage16(
                    baseCoverage, width, height, halfRadius, cancelRequested);
                QVector<quint16> eroded = minimumCoverage16(
                    baseCoverage, width, height, halfRadius, cancelRequested);
                if (dilated.isEmpty() || eroded.isEmpty()) return {};
                alpha = subtractCoverage16(dilated, eroded);
            }
            break;
        }
        case LayerEffectType::ColourOverlay:
        case LayerEffectType::GradientOverlay:
            alpha = baseCoverage;
            break;
        case LayerEffectType::BevelEmboss: {
            QVector<quint16> highlightAlpha;
            QVector<quint16> shadowAlpha;
            if (!bevelLightingCoverage16(baseCoverage, width, height, scaleX, scaleY,
                                         effect, &highlightAlpha, &shadowAlpha,
                                         cancelRequested)) return {};
            const bool grayscale = source.format() == QImage::Format_Grayscale8
                || source.format() == QImage::Format_Grayscale16;
            const QImage::Format passFormat = source.depth() > 32
                ? QImage::Format_RGBA64_Premultiplied
                : QImage::Format_ARGB32_Premultiplied;
            QColor highlightColour = effect.bevelHighlightColour;
            QColor shadowColour = effect.bevelShadowColour;
            if (grayscale) {
                const int hiGrey = qGray(highlightColour.rgb());
                const int shGrey = qGray(shadowColour.rgb());
                highlightColour = QColor(hiGrey, hiGrey, hiGrey);
                shadowColour = QColor(shGrey, shGrey, shGrey);
            }
            LayerEffectRenderPass highlightPass;
            highlightPass.image = colourisedCoverageRegion(
                highlightAlpha, width, height, expandedRegion, previewRegion,
                highlightColour, passFormat, source.colorSpace());
            highlightPass.blendMode = effect.bevelHighlightBlendMode;
            highlightPass.opacity = effect.effectOpacity * effect.bevelHighlightOpacity;
            highlightPass.behindSource = false;
            highlightPass.effectId = effect.id;
            LayerEffectRenderPass shadowPass;
            shadowPass.image = colourisedCoverageRegion(
                shadowAlpha, width, height, expandedRegion, previewRegion,
                shadowColour, passFormat, source.colorSpace());
            shadowPass.blendMode = effect.bevelShadowBlendMode;
            shadowPass.opacity = effect.effectOpacity * effect.bevelShadowOpacity;
            shadowPass.behindSource = false;
            shadowPass.effectId = effect.id;
            if (highlightPass.image.isNull() || shadowPass.image.isNull()) return {};
            if (highlightPass.opacity > 0.0) passes.push_back(std::move(highlightPass));
            if (shadowPass.opacity > 0.0) passes.push_back(std::move(shadowPass));
            continue;
        }
        }
        if (alpha.isEmpty()) continue;
        LayerEffectRenderPass pass;
        const bool grayscale = source.format() == QImage::Format_Grayscale8
            || source.format() == QImage::Format_Grayscale16;
        const QImage::Format passFormat = source.depth() > 32
            ? QImage::Format_RGBA64_Premultiplied
            : QImage::Format_ARGB32_Premultiplied;
        if (effect.type == LayerEffectType::GradientOverlay) {
            pass.image = gradientCoverageRegion(
                alpha, width, height, expandedRegion, previewRegion,
                effectDocumentBounds, scaleX, scaleY, effect,
                passFormat, source.colorSpace(), grayscale);
        } else {
            QColor effectColour = effect.colour;
            if (grayscale) {
                const int grey = qGray(effectColour.rgb());
                effectColour = QColor(grey, grey, grey);
            }
            pass.image = colourisedCoverageRegion(
                alpha, width, height, expandedRegion, previewRegion, effectColour,
                passFormat, source.colorSpace());
        }
        if (pass.image.isNull()) return {};
        pass.blendMode = effect.effectBlendMode;
        pass.opacity = effect.effectOpacity;
        pass.behindSource = behind;
        pass.effectId = effect.id;
        passes.push_back(std::move(pass));
    }
    layerEffectPassCache().insert(cacheKey, layer.id, passes);
    return passes;
}

void ImageProcessor::invalidateLayerEffectCaches(const QSet<QUuid> &layerIds)
{
    layerEffectPassCache().invalidateLayers(layerIds);
}

LayerEffectInputRegion ImageProcessor::renderLayerEffectInputRegion(
    const QImage &source,
    const QVector<LayerNode> &layers,
    const QUuid &layerId,
    const QRect &previewRegion,
    const QSize &documentSize,
    const std::atomic_bool *cancelRequested,
    const ColourProcessingCompatibility processingCompatibility)
{
    LayerEffectInputRegion result;
    if (source.isNull() || layerId.isNull() || previewRegion.isEmpty()) return result;
    const QSize resolvedDocumentSize = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize : source.size();
    const LayerNode *layer = nullptr;
    QTransform worldTransform;
    if (!findLayerWithWorldTransform(layers, layerId, QTransform(),
                                     &layer, &worldTransform)
        || !layer || !layerTypeSupportsLayerEffects(layer->type)) {
        return result;
    }

    LayerNode contentLayer = *layer;
    contentLayer.opacity = 1.0;
    contentLayer.blendMode = BlendMode::Copy;
    contentLayer.transform = worldTransform;
    contentLayer.layerEffects.clear();
    contentLayer.maskImage = {};
    contentLayer.maskReferenceSize = {};
    contentLayer.maskReferenceOrigin = {};
    contentLayer.maskEnabled = true;
    contentLayer.maskInverted = false;
    result.content = renderUnclippedRegionPreservingHiddenRgb(
        source, {contentLayer}, previewRegion, resolvedDocumentSize,
        cancelRequested, processingCompatibility);
    if (result.content.isNull()) return {};

    // Coverage deliberately uses the layer's real mask while preserving the
    // unmasked content separately above. This is the silhouette contract that
    // the 0.14.0i shadows/glows consume.
    LayerNode coverageLayer = *layer;
    coverageLayer.opacity = 1.0;
    coverageLayer.blendMode = BlendMode::Copy;
    coverageLayer.transform = worldTransform;
    coverageLayer.layerEffects.clear();
    QImage masked = renderUnclippedRegion(
        source, {coverageLayer}, previewRegion, resolvedDocumentSize,
        cancelRequested, processingCompatibility);
    if (masked.isNull()) return {};

    if (masked.depth() > 32) {
        QImage coverage(masked.size(), QImage::Format_Grayscale16);
        if (coverage.isNull()) return {};
        const QImage straight = masked.convertToFormat(QImage::Format_RGBA64);
        for (int y = 0; y < straight.height(); ++y) {
            const QRgba64 *src = reinterpret_cast<const QRgba64 *>(straight.constScanLine(y));
            quint16 *dst = reinterpret_cast<quint16 *>(coverage.scanLine(y));
            for (int x = 0; x < straight.width(); ++x) dst[x] = src[x].alpha();
        }
        result.coverage = std::move(coverage);
    } else {
        QImage coverage(masked.size(), QImage::Format_Grayscale8);
        if (coverage.isNull()) return {};
        const QImage straight = masked.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < straight.height(); ++y) {
            const uchar *src = straight.constScanLine(y);
            uchar *dst = coverage.scanLine(y);
            for (int x = 0; x < straight.width(); ++x) dst[x] = src[x * 4 + 3];
        }
        result.coverage = std::move(coverage);
    }
    return result;
}

QImage ImageProcessor::renderLiveFilterInput(
    const QImage &source,
    const QVector<LayerNode> &layers,
    const QUuid &smartLayerId,
    const QUuid &liveFilterId,
    const QSize &documentSize,
    const std::atomic_bool *cancelRequested,
    const ColourProcessingCompatibility processingCompatibility)
{
    if (source.isNull() || smartLayerId.isNull() || liveFilterId.isNull()) return {};
    const LayerNode *smart = nullptr;
    QTransform worldTransform;
    if (!findLayerWithWorldTransform(layers, smartLayerId, QTransform(),
                                     &smart, &worldTransform)
        || !smart || smart->type != LayerType::Smart) {
        return {};
    }
    int targetIndex = -1;
    for (int index = 0; index < smart->liveFilters.size(); ++index) {
        if (smart->liveFilters.at(index).id == liveFilterId) {
            targetIndex = index;
            break;
        }
    }
    if (targetIndex < 0) return {};

    LayerNode prefix = *smart;
    prefix.liveFilters.resize(targetIndex);
    prefix.opacity = 1.0;
    prefix.blendMode = BlendMode::Copy;
    prefix.maskImage = {};
    prefix.maskReferenceSize = {};
    prefix.maskReferenceOrigin = {};
    prefix.maskEnabled = true;
    prefix.maskInverted = false;

    const QSize reference = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize : source.size();
    const QImage::Format format = workingFormat(source);
    QImage input = smartLiveFilteredRegion(prefix, source, reference,
                                           worldTransform, source.rect(), format,
                                           source.colorSpace(), false,
                                           processingCompatibility,
                                           cancelRequested);
    if (input.isNull()
        || (cancelRequested
            && cancelRequested->load(std::memory_order_relaxed))) {
        return {};
    }
    QImage straight = input.convertToFormat(
        source.depth() > 32 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    straight.setColorSpace(source.colorSpace());
    return straight;
}


QImage ImageProcessor::renderAdjustmentInput(
    const QImage &source,
    const QVector<LayerNode> &layers,
    const QUuid &adjustmentLayerId,
    const QSize &documentSize,
    const std::atomic_bool *cancelRequested,
    const ColourProcessingCompatibility processingCompatibility)
{
    if (source.isNull() || adjustmentLayerId.isNull()) {
        return {};
    }
    const QSize reference = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize
        : source.size();
    const QImage::Format format = workingFormat(source);
    QImage canvas = transparentImage(source.size(), format, source.colorSpace());
    AdjustmentInputCapture capture;
    capture.targetLayerId = adjustmentLayerId;
    if (!compositeLayersRegion(canvas,
                               source,
                               layers,
                               source.rect(),
                               reference,
                               QTransform(),
                               cancelRequested,
                               false,
                               format,
                               processingCompatibility,
                               &capture)
        || !capture.found
        || capture.image.isNull()
        || (cancelRequested
            && cancelRequested->load(std::memory_order_relaxed))) {
        return {};
    }
    QImage straight = capture.image.convertToFormat(
        source.depth() > 32 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    straight.setColorSpace(source.colorSpace());
    return straight;
}

QImage ImageProcessor::renderRgbReference(const QImage &source,
                                          const QVector<LayerNode> &layers,
                                          const std::atomic_bool *cancelRequested,
                                          const QSize &documentSize,
                                          const ColourProcessingCompatibility processingCompatibility)
{
    if (source.isNull()) {
        return {};
    }
    const QSize reference = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize
        : source.size();
    return renderLayersRegion(source,
                              layers,
                              source.rect(),
                              reference,
                              QTransform(),
                              cancelRequested,
                              true,
                              source.depth() > 32,
                              processingCompatibility);
}

QImage ImageProcessor::renderRegionRgbReference(const QImage &source,
                                                const QVector<LayerNode> &layers,
                                                const QRect &previewRegion,
                                                const QSize &documentSize,
                                                const std::atomic_bool *cancelRequested,
                                                const ColourProcessingCompatibility processingCompatibility)
{
    if (source.isNull()) {
        return {};
    }
    const QRect region = previewRegion.intersected(source.rect());
    if (region.isEmpty()) {
        return {};
    }
    const QSize reference = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize
        : source.size();
    return renderLayersRegionWithSpatialHalo(source,
                                             layers,
                                             region,
                                             reference,
                                             QTransform(),
                                             cancelRequested,
                                             true,
                                             source.depth() > 32,
                                             processingCompatibility);
}

QImage ImageProcessor::renderLayerRegionChannelReference(
    const QImage &source,
    const QVector<LayerNode> &layers,
    const QUuid &layerId,
    const QRect &previewRegion,
    const QSize &documentSize,
    const bool forceOpaquePixelAlpha,
    const std::atomic_bool *cancelRequested,
    const ColourProcessingCompatibility processingCompatibility)
{
    Q_UNUSED(processingCompatibility);
    if (source.isNull() || layerId.isNull()) {
        return {};
    }
    const QRect region = previewRegion.intersected(source.rect());
    if (region.isEmpty()) {
        return {};
    }
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
        return {};
    }

    const LayerNode *layer = nullptr;
    QTransform worldTransform;
    if (!findLayerWithWorldTransform(layers,
                                     layerId,
                                     QTransform(),
                                     &layer,
                                     &worldTransform)
        || !layer
        || (layer->type != LayerType::BaseImage && layer->type != LayerType::Raster)) {
        return {};
    }

    const QSize reference = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize
        : source.size();
    const QImage &pixels = layer->type == LayerType::BaseImage && layer->rasterImage.isNull()
        ? source
        : layer->rasterImage;
    if (pixels.isNull()) {
        // An untouched Raster layer has implicit transparent-black storage.
        // Return a complete tile instead of leaving the previous layer's
        // channel presentation on the canvas.
        QImage empty = transparentImage(region.size(),
                                        workingFormat(source),
                                        source.colorSpace());
        if (forceOpaquePixelAlpha && !empty.isNull()) {
            empty.fill(QColor(0, 0, 0, 255));
        }
        return empty;
    }

    // Force the selected layer's pixel alpha opaque only for presentation so
    // hidden RGB remains inspectable. This does not mutate stored straight RGBA.
    return imageRegion(pixels,
                       layer->rasterReferenceSize,
                       layer->rasterReferenceOrigin,
                       source.size(),
                       worldTransform,
                       reference,
                       region,
                       workingFormat(source),
                       source.colorSpace(),
                       forceOpaquePixelAlpha);
}

QImage ImageProcessor::renderPreservingHiddenRgb(const QImage &source,
                                                 const QVector<LayerNode> &layers,
                                                 const std::atomic_bool *cancelRequested,
                                                 const QSize &documentSize,
                                                 const ColourProcessingCompatibility processingCompatibility)
{
    if (source.isNull()) {
        return {};
    }
    return renderRegionPreservingHiddenRgb(source,
                                           layers,
                                           source.rect(),
                                           documentSize,
                                           cancelRequested,
                                           processingCompatibility);
}

QImage ImageProcessor::renderRegionPreservingHiddenRgb(
    const QImage &source,
    const QVector<LayerNode> &layers,
    const QRect &previewRegion,
    const QSize &documentSize,
    const std::atomic_bool *cancelRequested,
    const ColourProcessingCompatibility processingCompatibility)
{
    if (source.isNull()) {
        return {};
    }
    const QRect requestedRegion = previewRegion.intersected(source.rect());
    if (requestedRegion.isEmpty()) {
        return {};
    }
    const QSize referenceSize = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize
        : source.size();
    // Spatial adjustment layers require neighbouring pixels beyond the
    // requested export/analysis tile. Render the complete dependency halo and
    // crop only after both the composited-alpha and hidden-RGB reference paths
    // have been evaluated. This keeps tiled export and analysis seam-free.
    const QRect renderRegion = regionWithSpatialHalo(source,
                                                      layers,
                                                      requestedRegion,
                                                      referenceSize);
    // Export and analysis use a 16-bit premultiplied working surface even for
    // 8-bit documents. This retains straight RGB precision at low non-zero
    // alpha while the independent reference defines RGB at exactly zero alpha.
    const QImage composite = renderLayersRegion(source,
                                                 layers,
                                                 renderRegion,
                                                 referenceSize,
                                                 QTransform(),
                                                 cancelRequested,
                                                 false,
                                                 true,
                                                 processingCompatibility);
    if (composite.isNull() || (cancelRequested && cancelRequested->load())) {
        return composite;
    }
    const QImage reference = renderLayersRegion(source,
                                                 layers,
                                                 renderRegion,
                                                 referenceSize,
                                                 QTransform(),
                                                 cancelRequested,
                                                 true,
                                                 true,
                                                 processingCompatibility);
    if (reference.isNull() || reference.size() != composite.size()) {
        return {};
    }

    const auto cropToRequested = [&](QImage output) {
        if (renderRegion != requestedRegion) {
            output = output.copy(QRect(requestedRegion.topLeft() - renderRegion.topLeft(),
                                       requestedRegion.size()));
        }
        output.setColorSpace(composite.colorSpace());
        output.setDevicePixelRatio(source.devicePixelRatio());
        output.setDotsPerMeterX(source.dotsPerMeterX());
        output.setDotsPerMeterY(source.dotsPerMeterY());
        return output;
    };

    if (source.depth() > 32) {
        QImage output = composite.convertToFormat(QImage::Format_RGBA64);
        const QImage rgb = reference.convertToFormat(QImage::Format_RGBA64);
        output.detach();
        for (int y = 0; y < output.height(); ++y) {
            auto *dst = reinterpret_cast<QRgba64 *>(output.scanLine(y));
            const auto *src = reinterpret_cast<const QRgba64 *>(rgb.constScanLine(y));
            for (int x = 0; x < output.width(); ++x) {
                if (dst[x].alpha() == 0) {
                    dst[x] = QRgba64::fromRgba64(src[x].red(),
                                                src[x].green(),
                                                src[x].blue(),
                                                0);
                }
            }
        }
        return cropToRequested(std::move(output));
    }

    QImage output = composite.convertToFormat(QImage::Format_RGBA8888);
    const QImage rgb = reference.convertToFormat(QImage::Format_RGBA8888);
    output.detach();
    for (int y = 0; y < output.height(); ++y) {
        uchar *dst = output.scanLine(y);
        const uchar *src = rgb.constScanLine(y);
        for (int x = 0; x < output.width(); ++x) {
            const int offset = x * 4;
            if (dst[offset + 3] == 0) {
                dst[offset] = src[offset];
                dst[offset + 1] = src[offset + 1];
                dst[offset + 2] = src[offset + 2];
            }
        }
    }
    return cropToRequested(std::move(output));
}

QRectF ImageProcessor::contentBounds(const QImage &source,
                                     const QVector<LayerNode> &layers,
                                     const QVector<QUuid> &layerIds,
                                     const QSize &documentSize)
{
    if (source.isNull() || layerIds.isEmpty()) {
        return {};
    }
    QSet<QUuid> selected;
    for (const QUuid &id : layerIds) {
        if (!id.isNull()) {
            selected.insert(id);
        }
    }
    const QSize reference = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize
        : source.size();
    QRectF bounds;
    collectSelectedBounds(source, layers, reference, QTransform(), selected, &bounds);
    return bounds;
}

} // namespace vfx
