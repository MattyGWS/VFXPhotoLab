#include "ToneBrush.h"

#include <QColorSpace>
#include <QRgba64>
#include <QRectF>
#include <QSizeF>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace vfx {
namespace {

constexpr double Epsilon = 1.0e-12;

struct Rgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

QImage normalisedToneImage(const QImage &image, ToneBrushTarget target);

double smoothStep(const double edge0, const double edge1, const double value)
{
    if (edge0 == edge1) {
        return value < edge0 ? 0.0 : 1.0;
    }
    const double t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double decodeColour(const double value, const bool linearEncoding)
{
    const double v = std::clamp(value, 0.0, 1.0);
    if (linearEncoding) {
        return v;
    }
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double encodeColour(const double value, const bool linearEncoding)
{
    const double v = std::clamp(value, 0.0, 1.0);
    if (linearEncoding) {
        return v;
    }
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

double luminance(const Rgb &rgb)
{
    return 0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b;
}

double toneRangeWeight(const double value, const ToneBrushRange range)
{
    const double y = std::clamp(value, 0.0, 1.0);
    switch (range) {
    case ToneBrushRange::Shadows:
        return 1.0 - smoothStep(0.08, 0.68, y);
    case ToneBrushRange::Highlights:
        return smoothStep(0.32, 0.94, y);
    case ToneBrushRange::Midtones:
        return smoothStep(0.08, 0.42, y)
            * (1.0 - smoothStep(0.58, 0.92, y));
    }
    return 1.0;
}

double adjustedScalar(const double input,
                      const double coverage,
                      const ToneBrushOperation operation,
                      const ToneBrushRange range,
                      const bool protectTones)
{
    const double value = std::clamp(input, 0.0, 1.0);
    double effect = std::clamp(coverage, 0.0, 1.0)
        * toneRangeWeight(value, range);
    if (protectTones) {
        if (operation == ToneBrushOperation::Dodge) {
            effect *= 0.12 + 0.88 * (1.0 - smoothStep(0.78, 0.995, value));
        } else if (operation == ToneBrushOperation::Burn) {
            effect *= 0.12 + 0.88 * smoothStep(0.005, 0.22, value);
        }
    }
    if (effect <= Epsilon) {
        return value;
    }
    constexpr double Response = 2.65;
    if (operation == ToneBrushOperation::Dodge) {
        return 1.0 - (1.0 - value) * std::exp(-Response * effect);
    }
    if (operation == ToneBrushOperation::Burn) {
        return value * std::exp(-Response * effect);
    }
    return value;
}

Rgb toneAdjustedRgb(const Rgb &encoded,
                    const double coverage,
                    const ToneBrushOperation operation,
                    const ToneBrushRange range,
                    const bool protectTones,
                    const bool linearEncoding)
{
    Rgb linear {decodeColour(encoded.r, linearEncoding),
                decodeColour(encoded.g, linearEncoding),
                decodeColour(encoded.b, linearEncoding)};
    const double beforeY = luminance(linear);
    const double afterY = adjustedScalar(beforeY, coverage, operation,
                                         range, protectTones);
    if (std::abs(afterY - beforeY) <= Epsilon) {
        return encoded;
    }

    Rgb adjusted;
    if (beforeY <= Epsilon) {
        adjusted = {afterY, afterY, afterY};
    } else {
        const double desiredScale = afterY / beforeY;
        double availableScale = desiredScale;
        if (desiredScale > 1.0) {
            const double maximum = std::max({linear.r, linear.g, linear.b});
            if (maximum > Epsilon) {
                availableScale = std::min(desiredScale, 1.0 / maximum);
            }
        }
        adjusted = {linear.r * availableScale,
                    linear.g * availableScale,
                    linear.b * availableScale};
        const double scaledY = luminance(adjusted);
        if (afterY > scaledY + Epsilon && scaledY < 1.0 - Epsilon) {
            const double whiteMix = std::clamp(
                (afterY - scaledY) / (1.0 - scaledY), 0.0, 1.0);
            adjusted.r += (1.0 - adjusted.r) * whiteMix;
            adjusted.g += (1.0 - adjusted.g) * whiteMix;
            adjusted.b += (1.0 - adjusted.b) * whiteMix;
        }
    }
    return {encodeColour(adjusted.r, linearEncoding),
            encodeColour(adjusted.g, linearEncoding),
            encodeColour(adjusted.b, linearEncoding)};
}

Rgb spongeAdjustedRgb(const Rgb &encoded,
                      const double coverage,
                      const ToneBrushOperation operation,
                      const bool vibranceProtection,
                      const bool linearEncoding)
{
    Rgb linear {decodeColour(encoded.r, linearEncoding),
                decodeColour(encoded.g, linearEncoding),
                decodeColour(encoded.b, linearEncoding)};
    const double y = luminance(linear);
    const double maximum = std::max({linear.r, linear.g, linear.b});
    const double minimum = std::min({linear.r, linear.g, linear.b});
    const double saturation = maximum <= Epsilon
        ? 0.0 : (maximum - minimum) / maximum;
    double effect = std::clamp(coverage, 0.0, 1.0);
    double scale = 1.0;
    if (operation == ToneBrushOperation::SpongeSaturate) {
        if (vibranceProtection) {
            effect *= std::clamp(1.0 - 0.78 * saturation, 0.18, 1.0);
        }
        scale = 1.0 + 2.8 * effect;
    } else {
        if (vibranceProtection) {
            effect *= 0.35 + 0.65 * saturation;
        }
        scale = std::exp(-2.5 * effect);
    }

    double maximumScale = std::numeric_limits<double>::infinity();
    const std::array<double, 3> components {linear.r, linear.g, linear.b};
    for (const double component : components) {
        const double delta = component - y;
        if (delta > Epsilon) {
            maximumScale = std::min(maximumScale, (1.0 - y) / delta);
        } else if (delta < -Epsilon) {
            maximumScale = std::min(maximumScale, (0.0 - y) / delta);
        }
    }
    if (std::isfinite(maximumScale)) {
        scale = std::min(scale, std::max(0.0, maximumScale));
    }
    linear.r = y + (linear.r - y) * scale;
    linear.g = y + (linear.g - y) * scale;
    linear.b = y + (linear.b - y) * scale;
    return {encodeColour(linear.r, linearEncoding),
            encodeColour(linear.g, linearEncoding),
            encodeColour(linear.b, linearEncoding)};
}


bool isDetailOperation(const ToneBrushOperation operation)
{
    return operation == ToneBrushOperation::Blur
        || operation == ToneBrushOperation::Sharpen;
}

QVector<float> boxBlurPass(const QVector<float> &input,
                           const int width,
                           const int height,
                           const int radius,
                           const bool horizontal)
{
    if (input.isEmpty() || width <= 0 || height <= 0 || radius <= 0) {
        return input;
    }
    QVector<float> output(input.size(), 0.0f);
    const int diameter = radius * 2 + 1;
    if (horizontal) {
        for (int y = 0; y < height; ++y) {
            double sum = 0.0;
            for (int offset = -radius; offset <= radius; ++offset) {
                const int x = std::clamp(offset, 0, width - 1);
                sum += input.at(y * width + x);
            }
            for (int x = 0; x < width; ++x) {
                output[y * width + x] = static_cast<float>(sum / diameter);
                const int removeX = std::clamp(x - radius, 0, width - 1);
                const int addX = std::clamp(x + radius + 1, 0, width - 1);
                sum += input.at(y * width + addX)
                    - input.at(y * width + removeX);
            }
        }
    } else {
        for (int x = 0; x < width; ++x) {
            double sum = 0.0;
            for (int offset = -radius; offset <= radius; ++offset) {
                const int y = std::clamp(offset, 0, height - 1);
                sum += input.at(y * width + x);
            }
            for (int y = 0; y < height; ++y) {
                output[y * width + x] = static_cast<float>(sum / diameter);
                const int removeY = std::clamp(y - radius, 0, height - 1);
                const int addY = std::clamp(y + radius + 1, 0, height - 1);
                sum += input.at(addY * width + x)
                    - input.at(removeY * width + x);
            }
        }
    }
    return output;
}

QVector<float> gaussianStyleBlur(QVector<float> values,
                                 const int width,
                                 const int height,
                                 const int radius)
{
    // Two separable box passes give a smooth Gaussian-like response while
    // retaining O(pixel-count) cost independent of the requested radius.
    for (int pass = 0; pass < 2; ++pass) {
        values = boxBlurPass(values, width, height, radius, true);
        values = boxBlurPass(values, width, height, radius, false);
    }
    return values;
}

double detailTarget(const double base,
                    const double blurred,
                    const ToneBrushOperation operation,
                    const bool protectHighlights,
                    const double highlightReference)
{
    if (operation == ToneBrushOperation::Blur) {
        return std::clamp(blurred, 0.0, 1.0);
    }
    double factor = 1.8;
    const double detail = base - blurred;
    if (protectHighlights && detail > 0.0) {
        factor *= 0.12 + 0.88
            * (1.0 - smoothStep(0.72, 0.995, highlightReference));
    }
    return std::clamp(base + detail * factor, 0.0, 1.0);
}

void applyDetailCoverage(const ToneBrushRequest &request,
                         const QRect &affected,
                         const QVector<float> &coverage,
                         QImage *output)
{
    if (!output || output->isNull() || affected.isEmpty() || coverage.isEmpty()) {
        return;
    }

    const QImage base = normalisedToneImage(request.destination, request.target);
    const bool linearEncoding = request.destination.colorSpace()
        == QColorSpace(QColorSpace::SRgbLinear);
    const int radius = std::clamp(qRound(request.radiusPixels), 1, 64);
    // gaussianStyleBlur performs two box cycles, so provide two radii of halo.
    const int halo = radius * 2;
    const QRect workRect = affected.adjusted(-halo, -halo, halo, halo)
                               .intersected(base.rect());
    if (workRect.isEmpty()) {
        return;
    }
    const int width = workRect.width();
    const int height = workRect.height();
    const int pixelCount = width * height;

    const auto coverageAt = [&coverage, &affected](const int x, const int y) {
        return static_cast<double>(coverage.at((y - affected.y()) * affected.width()
                                               + (x - affected.x())));
    };

    if (request.target == ToneBrushTarget::Mask
        || request.target == ToneBrushTarget::GreyChannel
        || request.target == ToneBrushTarget::ComponentChannel) {
        QVector<float> scalar(pixelCount, 0.0f);
        const bool sixteenBit = base.depth() > 32;
        for (int y = workRect.top(); y <= workRect.bottom(); ++y) {
            const int localY = y - workRect.y();
            if (request.target == ToneBrushTarget::Mask) {
                const uchar *row = base.constScanLine(y);
                for (int x = workRect.left(); x <= workRect.right(); ++x) {
                    scalar[localY * width + x - workRect.x()] = row[x] / 255.0f;
                }
            } else if (sixteenBit) {
                const auto *row = reinterpret_cast<const QRgba64 *>(base.constScanLine(y));
                for (int x = workRect.left(); x <= workRect.right(); ++x) {
                    const QRgba64 pixel = row[x];
                    double value = 0.0;
                    if (request.target == ToneBrushTarget::ComponentChannel) {
                        const quint16 channels[4] {pixel.red(), pixel.green(),
                                                   pixel.blue(), pixel.alpha()};
                        value = channels[request.componentIndex] / 65535.0;
                    } else {
                        value = luminance({decodeColour(pixel.red() / 65535.0, linearEncoding),
                                           decodeColour(pixel.green() / 65535.0, linearEncoding),
                                           decodeColour(pixel.blue() / 65535.0, linearEncoding)});
                    }
                    scalar[localY * width + x - workRect.x()]
                        = static_cast<float>(value);
                }
            } else {
                const uchar *row = base.constScanLine(y);
                for (int x = workRect.left(); x <= workRect.right(); ++x) {
                    const uchar *pixel = row + x * 4;
                    double value = 0.0;
                    if (request.target == ToneBrushTarget::ComponentChannel) {
                        value = pixel[request.componentIndex] / 255.0;
                    } else {
                        value = luminance({decodeColour(pixel[0] / 255.0, linearEncoding),
                                           decodeColour(pixel[1] / 255.0, linearEncoding),
                                           decodeColour(pixel[2] / 255.0, linearEncoding)});
                    }
                    scalar[localY * width + x - workRect.x()]
                        = static_cast<float>(value);
                }
            }
        }
        const QVector<float> blurred = gaussianStyleBlur(
            std::move(scalar), width, height, radius);

        for (int y = affected.top(); y <= affected.bottom(); ++y) {
            const int localY = y - workRect.y();
            if (request.target == ToneBrushTarget::Mask) {
                const uchar *baseRow = base.constScanLine(y);
                uchar *outputRow = output->scanLine(y);
                for (int x = affected.left(); x <= affected.right(); ++x) {
                    const double amount = coverageAt(x, y);
                    if (amount <= Epsilon) continue;
                    const double original = baseRow[x] / 255.0;
                    const double soft = blurred.at(localY * width + x - workRect.x());
                    const double target = detailTarget(original, soft, request.operation,
                                                       request.protectHighlights, original);
                    outputRow[x] = static_cast<uchar>(std::clamp(
                        qRound((original + (target - original) * amount) * 255.0),
                        0, 255));
                }
            } else if (base.depth() > 32) {
                const auto *baseRow = reinterpret_cast<const QRgba64 *>(base.constScanLine(y));
                auto *outputRow = reinterpret_cast<QRgba64 *>(output->scanLine(y));
                for (int x = affected.left(); x <= affected.right(); ++x) {
                    const double amount = coverageAt(x, y);
                    if (amount <= Epsilon) continue;
                    const QRgba64 pixel = baseRow[x];
                    const double soft = blurred.at(localY * width + x - workRect.x());
                    if (request.target == ToneBrushTarget::ComponentChannel) {
                        quint16 channels[4] {pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
                        const double original = channels[request.componentIndex] / 65535.0;
                        const double target = detailTarget(original, soft, request.operation,
                                                           request.protectHighlights, original);
                        channels[request.componentIndex] = static_cast<quint16>(std::clamp(
                            qRound((original + (target - original) * amount) * 65535.0),
                            0, 65535));
                        outputRow[x] = QRgba64::fromRgba64(channels[0], channels[1],
                                                           channels[2], channels[3]);
                    } else {
                        const Rgb original {
                            decodeColour(pixel.red() / 65535.0, linearEncoding),
                            decodeColour(pixel.green() / 65535.0, linearEncoding),
                            decodeColour(pixel.blue() / 65535.0, linearEncoding)};
                        const double originalY = luminance(original);
                        const double targetY = detailTarget(originalY, soft, request.operation,
                                                            request.protectHighlights, originalY);
                        const double finalY = originalY + (targetY - originalY) * amount;
                        const quint16 value = static_cast<quint16>(std::clamp(
                            qRound(encodeColour(finalY, linearEncoding) * 65535.0),
                            0, 65535));
                        outputRow[x] = QRgba64::fromRgba64(value, value, value, pixel.alpha());
                    }
                }
            } else {
                const uchar *baseRow = base.constScanLine(y);
                uchar *outputRow = output->scanLine(y);
                for (int x = affected.left(); x <= affected.right(); ++x) {
                    const double amount = coverageAt(x, y);
                    if (amount <= Epsilon) continue;
                    const uchar *pixel = baseRow + x * 4;
                    uchar *destination = outputRow + x * 4;
                    if (request.target == ToneBrushTarget::ComponentChannel) {
                        std::copy(pixel, pixel + 4, destination);
                        const double original = pixel[request.componentIndex] / 255.0;
                        const double soft = blurred.at(localY * width + x - workRect.x());
                        const double target = detailTarget(original, soft, request.operation,
                                                           request.protectHighlights, original);
                        destination[request.componentIndex] = static_cast<uchar>(std::clamp(
                            qRound((original + (target - original) * amount) * 255.0),
                            0, 255));
                    } else {
                        const Rgb original {
                            decodeColour(pixel[0] / 255.0, linearEncoding),
                            decodeColour(pixel[1] / 255.0, linearEncoding),
                            decodeColour(pixel[2] / 255.0, linearEncoding)};
                        const double originalY = luminance(original);
                        const double soft = blurred.at(localY * width + x - workRect.x());
                        const double targetY = detailTarget(originalY, soft, request.operation,
                                                            request.protectHighlights, originalY);
                        const uchar value = static_cast<uchar>(std::clamp(
                            qRound(encodeColour(originalY + (targetY - originalY) * amount,
                                                linearEncoding) * 255.0),
                            0, 255));
                        destination[0] = value;
                        destination[1] = value;
                        destination[2] = value;
                        destination[3] = pixel[3];
                    }
                }
            }
        }
        return;
    }

    // Raster RGB uses associated colour for visible neighbourhood samples,
    // so fully transparent hidden colour cannot bleed into opaque edges. A
    // parallel unassociated field is retained only for neighbourhoods whose
    // blurred alpha is also zero, preserving meaningful hidden RGB beneath
    // complete transparency. Destination Alpha itself is never modified.
    QVector<float> associatedR(pixelCount, 0.0f);
    QVector<float> associatedG(pixelCount, 0.0f);
    QVector<float> associatedB(pixelCount, 0.0f);
    QVector<float> alphaWeights(pixelCount, 0.0f);
    QVector<float> hiddenR(pixelCount, 0.0f);
    QVector<float> hiddenG(pixelCount, 0.0f);
    QVector<float> hiddenB(pixelCount, 0.0f);
    const bool sixteenBit = base.depth() > 32;
    for (int y = workRect.top(); y <= workRect.bottom(); ++y) {
        const int localY = y - workRect.y();
        if (sixteenBit) {
            const auto *row = reinterpret_cast<const QRgba64 *>(base.constScanLine(y));
            for (int x = workRect.left(); x <= workRect.right(); ++x) {
                const QRgba64 pixel = row[x];
                const double alpha = pixel.alpha() / 65535.0;
                const double red = decodeColour(pixel.red() / 65535.0, linearEncoding);
                const double green = decodeColour(pixel.green() / 65535.0, linearEncoding);
                const double blue = decodeColour(pixel.blue() / 65535.0, linearEncoding);
                const int index = localY * width + x - workRect.x();
                associatedR[index] = static_cast<float>(red * alpha);
                associatedG[index] = static_cast<float>(green * alpha);
                associatedB[index] = static_cast<float>(blue * alpha);
                alphaWeights[index] = static_cast<float>(alpha);
                hiddenR[index] = static_cast<float>(red);
                hiddenG[index] = static_cast<float>(green);
                hiddenB[index] = static_cast<float>(blue);
            }
        } else {
            const uchar *row = base.constScanLine(y);
            for (int x = workRect.left(); x <= workRect.right(); ++x) {
                const uchar *pixel = row + x * 4;
                const double alpha = pixel[3] / 255.0;
                const double red = decodeColour(pixel[0] / 255.0, linearEncoding);
                const double green = decodeColour(pixel[1] / 255.0, linearEncoding);
                const double blue = decodeColour(pixel[2] / 255.0, linearEncoding);
                const int index = localY * width + x - workRect.x();
                associatedR[index] = static_cast<float>(red * alpha);
                associatedG[index] = static_cast<float>(green * alpha);
                associatedB[index] = static_cast<float>(blue * alpha);
                alphaWeights[index] = static_cast<float>(alpha);
                hiddenR[index] = static_cast<float>(red);
                hiddenG[index] = static_cast<float>(green);
                hiddenB[index] = static_cast<float>(blue);
            }
        }
    }
    associatedR = gaussianStyleBlur(std::move(associatedR), width, height, radius);
    associatedG = gaussianStyleBlur(std::move(associatedG), width, height, radius);
    associatedB = gaussianStyleBlur(std::move(associatedB), width, height, radius);
    alphaWeights = gaussianStyleBlur(std::move(alphaWeights), width, height, radius);
    hiddenR = gaussianStyleBlur(std::move(hiddenR), width, height, radius);
    hiddenG = gaussianStyleBlur(std::move(hiddenG), width, height, radius);
    hiddenB = gaussianStyleBlur(std::move(hiddenB), width, height, radius);

    for (int y = affected.top(); y <= affected.bottom(); ++y) {
        const int localY = y - workRect.y();
        if (sixteenBit) {
            const auto *baseRow = reinterpret_cast<const QRgba64 *>(base.constScanLine(y));
            auto *outputRow = reinterpret_cast<QRgba64 *>(output->scanLine(y));
            for (int x = affected.left(); x <= affected.right(); ++x) {
                const double amount = coverageAt(x, y);
                if (amount <= Epsilon) continue;
                const int index = localY * width + x - workRect.x();
                const double alpha = alphaWeights.at(index);
                const QRgba64 pixel = baseRow[x];
                const Rgb original {
                    decodeColour(pixel.red() / 65535.0, linearEncoding),
                    decodeColour(pixel.green() / 65535.0, linearEncoding),
                    decodeColour(pixel.blue() / 65535.0, linearEncoding)};
                const Rgb soft = alpha > Epsilon
                    ? Rgb {associatedR.at(index) / alpha,
                           associatedG.at(index) / alpha,
                           associatedB.at(index) / alpha}
                    : Rgb {hiddenR.at(index), hiddenG.at(index), hiddenB.at(index)};
                const double highlight = luminance(original);
                const Rgb target {
                    detailTarget(original.r, soft.r, request.operation,
                                 request.protectHighlights, highlight),
                    detailTarget(original.g, soft.g, request.operation,
                                 request.protectHighlights, highlight),
                    detailTarget(original.b, soft.b, request.operation,
                                 request.protectHighlights, highlight)};
                outputRow[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::clamp(qRound(encodeColour(
                        original.r + (target.r - original.r) * amount,
                        linearEncoding) * 65535.0), 0, 65535)),
                    static_cast<quint16>(std::clamp(qRound(encodeColour(
                        original.g + (target.g - original.g) * amount,
                        linearEncoding) * 65535.0), 0, 65535)),
                    static_cast<quint16>(std::clamp(qRound(encodeColour(
                        original.b + (target.b - original.b) * amount,
                        linearEncoding) * 65535.0), 0, 65535)),
                    pixel.alpha());
            }
        } else {
            const uchar *baseRow = base.constScanLine(y);
            uchar *outputRow = output->scanLine(y);
            for (int x = affected.left(); x <= affected.right(); ++x) {
                const double amount = coverageAt(x, y);
                if (amount <= Epsilon) continue;
                const int index = localY * width + x - workRect.x();
                const double alpha = alphaWeights.at(index);
                const uchar *pixel = baseRow + x * 4;
                uchar *destination = outputRow + x * 4;
                const Rgb original {decodeColour(pixel[0] / 255.0, linearEncoding),
                                    decodeColour(pixel[1] / 255.0, linearEncoding),
                                    decodeColour(pixel[2] / 255.0, linearEncoding)};
                const Rgb soft = alpha > Epsilon
                    ? Rgb {associatedR.at(index) / alpha,
                           associatedG.at(index) / alpha,
                           associatedB.at(index) / alpha}
                    : Rgb {hiddenR.at(index), hiddenG.at(index), hiddenB.at(index)};
                const double highlight = luminance(original);
                const Rgb target {
                    detailTarget(original.r, soft.r, request.operation,
                                 request.protectHighlights, highlight),
                    detailTarget(original.g, soft.g, request.operation,
                                 request.protectHighlights, highlight),
                    detailTarget(original.b, soft.b, request.operation,
                                 request.protectHighlights, highlight)};
                destination[0] = static_cast<uchar>(std::clamp(qRound(encodeColour(
                    original.r + (target.r - original.r) * amount,
                    linearEncoding) * 255.0), 0, 255));
                destination[1] = static_cast<uchar>(std::clamp(qRound(encodeColour(
                    original.g + (target.g - original.g) * amount,
                    linearEncoding) * 255.0), 0, 255));
                destination[2] = static_cast<uchar>(std::clamp(qRound(encodeColour(
                    original.b + (target.b - original.b) * amount,
                    linearEncoding) * 255.0), 0, 255));
                destination[3] = pixel[3];
            }
        }
    }
}

double stampCoverage(const double normalisedDistance, const double hardness)
{
    const double safeHardness = std::clamp(hardness, 0.0, 0.9999);
    if (normalisedDistance <= safeHardness) {
        return 1.0;
    }
    if (normalisedDistance >= 1.0) {
        return 0.0;
    }
    const double t = (normalisedDistance - safeHardness)
        / (1.0 - safeHardness);
    const double smooth = t * t * (3.0 - 2.0 * t);
    return 1.0 - smooth;
}

QRect segmentBounds(const QVector<QLineF> &segments,
                    const double radius,
                    const QRect &imageRect)
{
    QRect affected;
    for (const QLineF &segment : segments) {
        const QRect rect = QRectF(
            QPointF(std::min(segment.p1().x(), segment.p2().x()) - radius - 1.0,
                    std::min(segment.p1().y(), segment.p2().y()) - radius - 1.0),
            QPointF(std::max(segment.p1().x(), segment.p2().x()) + radius + 1.0,
                    std::max(segment.p1().y(), segment.p2().y()) + radius + 1.0))
                               .normalized().toAlignedRect().intersected(imageRect);
        if (!rect.isEmpty()) {
            affected = affected.isEmpty() ? rect : affected.united(rect);
        }
    }
    return affected;
}

QVector<float> accumulatedCoverage(const QVector<QLineF> &segments,
                                   const QRect &affected,
                                   const double diameter,
                                   const double strength,
                                   const double hardness)
{
    if (affected.isEmpty()) {
        return {};
    }
    QVector<float> coverage(affected.width() * affected.height(), 0.0f);
    const double radius = std::max(0.5, diameter * 0.5);
    const double spacing = std::max(1.0, radius * 0.22);
    const double safeStrength = std::clamp(strength, 0.0, 1.0);
    for (const QLineF &line : segments) {
        const int steps = line.length() <= 1.0e-6
            ? 1 : std::max(1, static_cast<int>(std::ceil(line.length() / spacing)));
        for (int step = 1; step <= steps; ++step) {
            const double amount = step / static_cast<double>(steps);
            const QPointF centre = line.length() <= 1.0e-6
                ? line.p2() : line.p1() + (line.p2() - line.p1()) * amount;
            const QRect stampRect = QRectF(centre - QPointF(radius, radius),
                                           QSizeF(radius * 2.0, radius * 2.0))
                                        .adjusted(-1.0, -1.0, 1.0, 1.0)
                                        .toAlignedRect().intersected(affected);
            for (int y = stampRect.top(); y <= stampRect.bottom(); ++y) {
                for (int x = stampRect.left(); x <= stampRect.right(); ++x) {
                    const double dx = x + 0.5 - centre.x();
                    const double dy = y + 0.5 - centre.y();
                    const double dab = stampCoverage(std::sqrt(dx * dx + dy * dy) / radius,
                                                     hardness) * safeStrength;
                    if (dab <= Epsilon) {
                        continue;
                    }
                    const int index = (y - affected.y()) * affected.width()
                        + (x - affected.x());
                    // Exposure/Flow is applied once to the geometric stroke
                    // footprint. Taking the maximum dab contribution makes one
                    // smooth pass independent of pointer-event frequency and
                    // brush-spacing overlap; a separate stroke builds further.
                    coverage[index] = std::max(
                        coverage.at(index), static_cast<float>(dab));
                }
            }
        }
    }
    return coverage;
}

QString requestError(const ToneBrushRequest &request)
{
    if (request.destination.isNull()) {
        return QStringLiteral("The tone-brush destination is empty");
    }
    if (request.operation == ToneBrushOperation::None) {
        return QStringLiteral("No tone-brush operation was selected");
    }
    if ((request.operation == ToneBrushOperation::SpongeSaturate
         || request.operation == ToneBrushOperation::SpongeDesaturate)
        && request.target != ToneBrushTarget::RasterRgb) {
        return QStringLiteral("Sponge supports raster RGB targets only");
    }
    if (request.target == ToneBrushTarget::ComponentChannel
        && (request.componentIndex < 0 || request.componentIndex > 3)) {
        return QStringLiteral("The selected component channel is invalid");
    }
    return {};
}

QImage normalisedToneImage(const QImage &image, const ToneBrushTarget target)
{
    if (target == ToneBrushTarget::Mask) {
        return image.convertToFormat(QImage::Format_Grayscale8);
    }
    QImage converted = image.convertToFormat(
        image.depth() > 32 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    converted.setColorSpace(image.colorSpace());
    return converted;
}

void applyCoverage(const ToneBrushRequest &request,
                   const QRect &affected,
                   const QVector<float> &coverage,
                   QImage *output)
{
    if (!output || output->isNull() || affected.isEmpty() || coverage.isEmpty()) {
        return;
    }

    if (isDetailOperation(request.operation)) {
        applyDetailCoverage(request, affected, coverage, output);
        return;
    }

    const QImage base = normalisedToneImage(request.destination, request.target);
    const bool linearEncoding = request.destination.colorSpace()
        == QColorSpace(QColorSpace::SRgbLinear);

    if (request.target == ToneBrushTarget::Mask) {
        for (int y = affected.top(); y <= affected.bottom(); ++y) {
            const uchar *baseRow = base.constScanLine(y);
            uchar *outputRow = output->scanLine(y);
            for (int x = affected.left(); x <= affected.right(); ++x) {
                const double amount = coverage.at((y - affected.y()) * affected.width()
                                                  + (x - affected.x()));
                if (amount <= Epsilon) {
                    continue;
                }
                outputRow[x] = static_cast<uchar>(std::clamp(
                    qRound(adjustedScalar(baseRow[x] / 255.0, amount,
                                          request.operation, request.range,
                                          request.protectTones) * 255.0),
                    0, 255));
            }
        }
        return;
    }

    const bool sixteenBit = base.depth() > 32;
    for (int y = affected.top(); y <= affected.bottom(); ++y) {
        if (sixteenBit) {
            const auto *baseRow = reinterpret_cast<const QRgba64 *>(base.constScanLine(y));
            auto *outputRow = reinterpret_cast<QRgba64 *>(output->scanLine(y));
            for (int x = affected.left(); x <= affected.right(); ++x) {
                const double amount = coverage.at((y - affected.y()) * affected.width()
                                                  + (x - affected.x()));
                if (amount <= Epsilon) {
                    continue;
                }
                const QRgba64 pixel = baseRow[x];
                if (request.target == ToneBrushTarget::ComponentChannel) {
                    quint16 channels[4] {pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
                    channels[request.componentIndex] = static_cast<quint16>(std::clamp(
                        qRound(adjustedScalar(channels[request.componentIndex] / 65535.0,
                                              amount, request.operation, request.range,
                                              request.protectTones) * 65535.0),
                        0, 65535));
                    outputRow[x] = QRgba64::fromRgba64(channels[0], channels[1],
                                                       channels[2], channels[3]);
                    continue;
                }
                if (request.target == ToneBrushTarget::GreyChannel) {
                    const Rgb linear {decodeColour(pixel.red() / 65535.0, linearEncoding),
                                      decodeColour(pixel.green() / 65535.0, linearEncoding),
                                      decodeColour(pixel.blue() / 65535.0, linearEncoding)};
                    const quint16 value = static_cast<quint16>(std::clamp(
                        qRound(encodeColour(adjustedScalar(luminance(linear), amount,
                                                         request.operation, request.range,
                                                         request.protectTones),
                                            linearEncoding) * 65535.0),
                        0, 65535));
                    outputRow[x] = QRgba64::fromRgba64(value, value, value, pixel.alpha());
                    continue;
                }
                const Rgb encoded {pixel.red() / 65535.0,
                                   pixel.green() / 65535.0,
                                   pixel.blue() / 65535.0};
                const Rgb adjusted = request.operation == ToneBrushOperation::Dodge
                        || request.operation == ToneBrushOperation::Burn
                    ? toneAdjustedRgb(encoded, amount, request.operation,
                                      request.range, request.protectTones,
                                      linearEncoding)
                    : spongeAdjustedRgb(encoded, amount, request.operation,
                                        request.vibranceProtection,
                                        linearEncoding);
                outputRow[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::clamp(qRound(adjusted.r * 65535.0), 0, 65535)),
                    static_cast<quint16>(std::clamp(qRound(adjusted.g * 65535.0), 0, 65535)),
                    static_cast<quint16>(std::clamp(qRound(adjusted.b * 65535.0), 0, 65535)),
                    pixel.alpha());
            }
        } else {
            const uchar *baseRow = base.constScanLine(y);
            uchar *outputRow = output->scanLine(y);
            for (int x = affected.left(); x <= affected.right(); ++x) {
                const double amount = coverage.at((y - affected.y()) * affected.width()
                                                  + (x - affected.x()));
                if (amount <= Epsilon) {
                    continue;
                }
                const uchar *pixel = baseRow + x * 4;
                uchar *destination = outputRow + x * 4;
                if (request.target == ToneBrushTarget::ComponentChannel) {
                    destination[0] = pixel[0];
                    destination[1] = pixel[1];
                    destination[2] = pixel[2];
                    destination[3] = pixel[3];
                    destination[request.componentIndex] = static_cast<uchar>(std::clamp(
                        qRound(adjustedScalar(pixel[request.componentIndex] / 255.0,
                                              amount, request.operation, request.range,
                                              request.protectTones) * 255.0),
                        0, 255));
                    continue;
                }
                if (request.target == ToneBrushTarget::GreyChannel) {
                    const Rgb linear {decodeColour(pixel[0] / 255.0, linearEncoding),
                                      decodeColour(pixel[1] / 255.0, linearEncoding),
                                      decodeColour(pixel[2] / 255.0, linearEncoding)};
                    const uchar value = static_cast<uchar>(std::clamp(
                        qRound(encodeColour(adjustedScalar(luminance(linear), amount,
                                                         request.operation, request.range,
                                                         request.protectTones),
                                            linearEncoding) * 255.0),
                        0, 255));
                    destination[0] = value;
                    destination[1] = value;
                    destination[2] = value;
                    destination[3] = pixel[3];
                    continue;
                }
                const Rgb encoded {pixel[0] / 255.0, pixel[1] / 255.0,
                                   pixel[2] / 255.0};
                const Rgb adjusted = request.operation == ToneBrushOperation::Dodge
                        || request.operation == ToneBrushOperation::Burn
                    ? toneAdjustedRgb(encoded, amount, request.operation,
                                      request.range, request.protectTones,
                                      linearEncoding)
                    : spongeAdjustedRgb(encoded, amount, request.operation,
                                        request.vibranceProtection,
                                        linearEncoding);
                destination[0] = static_cast<uchar>(std::clamp(qRound(adjusted.r * 255.0), 0, 255));
                destination[1] = static_cast<uchar>(std::clamp(qRound(adjusted.g * 255.0), 0, 255));
                destination[2] = static_cast<uchar>(std::clamp(qRound(adjusted.b * 255.0), 0, 255));
                destination[3] = pixel[3];
                // Raster Dodge/Burn/Sponge preserve straight Alpha and hidden RGB semantics.
            }
        }
    }
}

constexpr int CoverageTileSize = 128;

quint64 coverageTileKey(const int tileX, const int tileY)
{
    return (static_cast<quint64>(static_cast<quint32>(tileY)) << 32)
        | static_cast<quint32>(tileX);
}

float retainedCoverageAt(const ToneBrushStrokeAccumulator &accumulator,
                         const int x,
                         const int y)
{
    const int tileX = x / CoverageTileSize;
    const int tileY = y / CoverageTileSize;
    const auto iterator = accumulator.coverageTiles.constFind(
        coverageTileKey(tileX, tileY));
    if (iterator == accumulator.coverageTiles.cend()) {
        return 0.0f;
    }
    const int localX = x - tileX * CoverageTileSize;
    const int localY = y - tileY * CoverageTileSize;
    return iterator.value().at(localY * CoverageTileSize + localX);
}

QRect appendCoverage(const ToneBrushRequest &request,
                     ToneBrushStrokeAccumulator *accumulator)
{
    if (!accumulator || request.targetSegments.isEmpty()) {
        return {};
    }
    if (accumulator->imageSize != request.destination.size()) {
        accumulator->reset();
        accumulator->imageSize = request.destination.size();
    }

    const double radius = std::max(0.5, request.diameterPixels * 0.5);
    const double spacing = std::max(1.0, radius * 0.22);
    const double safeStrength = std::clamp(request.strength, 0.0, 1.0);
    const QRect imageRect = request.destination.rect();
    QRect changed;

    for (const QLineF &line : request.targetSegments) {
        const int steps = line.length() <= 1.0e-6
            ? 1 : std::max(1, static_cast<int>(std::ceil(line.length() / spacing)));
        for (int step = 1; step <= steps; ++step) {
            const double amount = step / static_cast<double>(steps);
            const QPointF centre = line.length() <= 1.0e-6
                ? line.p2() : line.p1() + (line.p2() - line.p1()) * amount;
            const QRect stampRect = QRectF(centre - QPointF(radius, radius),
                                           QSizeF(radius * 2.0, radius * 2.0))
                                        .adjusted(-1.0, -1.0, 1.0, 1.0)
                                        .toAlignedRect().intersected(imageRect);
            for (int y = stampRect.top(); y <= stampRect.bottom(); ++y) {
                for (int x = stampRect.left(); x <= stampRect.right(); ++x) {
                    const double dx = x + 0.5 - centre.x();
                    const double dy = y + 0.5 - centre.y();
                    const float dab = static_cast<float>(
                        stampCoverage(std::sqrt(dx * dx + dy * dy) / radius,
                                      request.hardness) * safeStrength);
                    if (dab <= Epsilon) {
                        continue;
                    }
                    const int tileX = x / CoverageTileSize;
                    const int tileY = y / CoverageTileSize;
                    QVector<float> &tile = accumulator->coverageTiles[
                        coverageTileKey(tileX, tileY)];
                    if (tile.isEmpty()) {
                        tile = QVector<float>(CoverageTileSize * CoverageTileSize, 0.0f);
                    }
                    const int localX = x - tileX * CoverageTileSize;
                    const int localY = y - tileY * CoverageTileSize;
                    float &retained = tile[localY * CoverageTileSize + localX];
                    if (dab <= retained + 1.0e-7f) {
                        continue;
                    }
                    retained = dab;
                    const QRect pixelRect(x, y, 1, 1);
                    changed = changed.isEmpty() ? pixelRect : changed.united(pixelRect);
                }
            }
        }
    }
    return changed;
}

} // namespace

void ToneBrushStrokeAccumulator::reset()
{
    imageSize = {};
    coverageTiles.clear();
}

ToneBrushResult applyToneBrush(const ToneBrushRequest &request)
{
    ToneBrushResult result;
    result.error = requestError(request);
    if (!result.error.isEmpty()) {
        return result;
    }
    if (request.targetSegments.isEmpty()) {
        result.image = request.destination;
        return result;
    }

    const double radius = std::max(0.5, request.diameterPixels * 0.5);
    const QRect affected = segmentBounds(request.targetSegments, radius,
                                         request.destination.rect());
    result.affectedRect = affected;
    result.image = normalisedToneImage(request.destination, request.target);
    if (affected.isEmpty()) {
        return result;
    }
    const QVector<float> coverage = accumulatedCoverage(
        request.targetSegments, affected, request.diameterPixels,
        request.strength, request.hardness);
    if (coverage.isEmpty()) {
        return result;
    }

    result.image.detach();
    applyCoverage(request, affected, coverage, &result.image);
    return result;
}

ToneBrushResult applyToneBrushIncremental(const ToneBrushRequest &request,
                                          ToneBrushStrokeAccumulator *accumulator,
                                          QImage *workingImage)
{
    ToneBrushResult result;
    result.error = requestError(request);
    if (!result.error.isEmpty()) {
        return result;
    }
    if (!accumulator || !workingImage) {
        result.error = QStringLiteral("The live tone-brush accumulator is unavailable");
        return result;
    }
    if (request.targetSegments.isEmpty()) {
        return result;
    }

    const QImage normalisedBase = normalisedToneImage(request.destination,
                                                       request.target);
    if (workingImage->size() != normalisedBase.size()
        || workingImage->format() != normalisedBase.format()) {
        *workingImage = normalisedBase;
    }

    const QRect changed = appendCoverage(request, accumulator);
    result.affectedRect = changed;
    if (changed.isEmpty()) {
        return result;
    }

    QVector<float> coverage(changed.width() * changed.height(), 0.0f);
    for (int y = changed.top(); y <= changed.bottom(); ++y) {
        for (int x = changed.left(); x <= changed.right(); ++x) {
            coverage[(y - changed.y()) * changed.width() + (x - changed.x())]
                = retainedCoverageAt(*accumulator, x, y);
        }
    }

    workingImage->detach();
    applyCoverage(request, changed, coverage, workingImage);
    return result;
}

} // namespace vfx
