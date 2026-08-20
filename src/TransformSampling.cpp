#include "TransformSampling.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vfx {
namespace {

constexpr double Pi = 3.141592653589793238462643383279502884;

double cubicWeight(const double x)
{
    // Catmull-Rom cubic convolution (a = -0.5): sharp but interpolating.
    const double ax = std::abs(x);
    if (ax < 1.0) {
        return 1.5 * ax * ax * ax - 2.5 * ax * ax + 1.0;
    }
    if (ax < 2.0) {
        return -0.5 * ax * ax * ax + 2.5 * ax * ax - 4.0 * ax + 2.0;
    }
    return 0.0;
}

double sinc(const double x)
{
    if (std::abs(x) < 1.0e-12) {
        return 1.0;
    }
    const double p = Pi * x;
    return std::sin(p) / p;
}

double lanczos3Weight(const double x)
{
    const double ax = std::abs(x);
    return ax < 3.0 ? sinc(x) * sinc(x / 3.0) : 0.0;
}

QRgba64 rgbaAt(const QImage &image,
               const int x,
               const int y,
               const QRgba64 outside)
{
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
        return outside;
    }
    return reinterpret_cast<const QRgba64 *>(image.constScanLine(y))[x];
}

quint16 greyAt(const QImage &image,
               const int x,
               const int y,
               const quint16 outside)
{
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
        return outside;
    }
    return reinterpret_cast<const quint16 *>(image.constScanLine(y))[x];
}

quint16 roundedComponent(const double value)
{
    return static_cast<quint16>(std::clamp(std::llround(value), 0LL, 65535LL));
}

template <typename Fetch>
double separableSample(const double sx,
                       const double sy,
                       const int firstX,
                       const int lastX,
                       const int firstY,
                       const int lastY,
                       const Fetch &fetch,
                       const bool lanczos)
{
    double total = 0.0;
    double weightTotal = 0.0;
    for (int y = firstY; y <= lastY; ++y) {
        const double wy = lanczos ? lanczos3Weight(sy - y)
                                  : cubicWeight(sy - y);
        if (std::abs(wy) < 1.0e-15) {
            continue;
        }
        for (int x = firstX; x <= lastX; ++x) {
            const double wx = lanczos ? lanczos3Weight(sx - x)
                                      : cubicWeight(sx - x);
            const double weight = wx * wy;
            if (std::abs(weight) < 1.0e-15) {
                continue;
            }
            total += fetch(x, y) * weight;
            weightTotal += weight;
        }
    }
    return std::abs(weightTotal) > 1.0e-12 ? total / weightTotal : 0.0;
}

} // namespace

QRgba64 sampleTransformRgba64(const QImage &source,
                              const QSize &localExtent,
                              const QPointF &localPoint,
                              const TransformInterpolation interpolation,
                              const QRgba64 outside)
{
    if (source.isNull() || localExtent.isEmpty()
        || localPoint.x() < 0.0 || localPoint.y() < 0.0
        || localPoint.x() >= localExtent.width()
        || localPoint.y() >= localExtent.height()) {
        return outside;
    }
    QImage converted;
    const QImage *image = &source;
    if (source.format() != QImage::Format_RGBA64) {
        converted = source.convertToFormat(QImage::Format_RGBA64);
        image = &converted;
    }
    if (image->isNull()) {
        return outside;
    }

    const double sx = localPoint.x() * image->width() / localExtent.width() - 0.5;
    const double sy = localPoint.y() * image->height() / localExtent.height() - 0.5;
    if (interpolation == TransformInterpolation::NearestNeighbour) {
        return rgbaAt(*image,
                      static_cast<int>(std::floor(sx + 0.5)),
                      static_cast<int>(std::floor(sy + 0.5)),
                      outside);
    }
    if (interpolation == TransformInterpolation::Bilinear) {
        const int x0 = static_cast<int>(std::floor(sx));
        const int y0 = static_cast<int>(std::floor(sy));
        const double fx = sx - x0;
        const double fy = sy - y0;
        const QRgba64 p00 = rgbaAt(*image, x0, y0, outside);
        const QRgba64 p10 = rgbaAt(*image, x0 + 1, y0, outside);
        const QRgba64 p01 = rgbaAt(*image, x0, y0 + 1, outside);
        const QRgba64 p11 = rgbaAt(*image, x0 + 1, y0 + 1, outside);
        const auto mix = [fx, fy](const quint16 a00,
                                  const quint16 a10,
                                  const quint16 a01,
                                  const quint16 a11) {
            const double top = a00 + (static_cast<double>(a10) - a00) * fx;
            const double bottom = a01 + (static_cast<double>(a11) - a01) * fx;
            return roundedComponent(top + (bottom - top) * fy);
        };
        return QRgba64::fromRgba64(
            mix(p00.red(), p10.red(), p01.red(), p11.red()),
            mix(p00.green(), p10.green(), p01.green(), p11.green()),
            mix(p00.blue(), p10.blue(), p01.blue(), p11.blue()),
            mix(p00.alpha(), p10.alpha(), p01.alpha(), p11.alpha()));
    }

    const bool lanczos = interpolation == TransformInterpolation::Lanczos3;
    const int radius = lanczos ? 3 : 2;
    const int firstX = static_cast<int>(std::floor(sx)) - (radius - 1);
    const int lastX = static_cast<int>(std::floor(sx)) + radius;
    const int firstY = static_cast<int>(std::floor(sy)) - (radius - 1);
    const int lastY = static_cast<int>(std::floor(sy)) + radius;
    std::array<double, 4> totals = {0.0, 0.0, 0.0, 0.0};
    double weightTotal = 0.0;
    for (int y = firstY; y <= lastY; ++y) {
        const double wy = lanczos ? lanczos3Weight(sy - y)
                                  : cubicWeight(sy - y);
        if (std::abs(wy) < 1.0e-15) {
            continue;
        }
        for (int x = firstX; x <= lastX; ++x) {
            const double wx = lanczos ? lanczos3Weight(sx - x)
                                      : cubicWeight(sx - x);
            const double weight = wx * wy;
            if (std::abs(weight) < 1.0e-15) {
                continue;
            }
            const QRgba64 pixel = rgbaAt(*image, x, y, outside);
            totals[0] += pixel.red() * weight;
            totals[1] += pixel.green() * weight;
            totals[2] += pixel.blue() * weight;
            totals[3] += pixel.alpha() * weight;
            weightTotal += weight;
        }
    }
    if (std::abs(weightTotal) <= 1.0e-12) {
        return outside;
    }
    return QRgba64::fromRgba64(
        roundedComponent(totals[0] / weightTotal),
        roundedComponent(totals[1] / weightTotal),
        roundedComponent(totals[2] / weightTotal),
        roundedComponent(totals[3] / weightTotal));
}

quint16 sampleTransformGrey16(const QImage &source,
                              const QSize &localExtent,
                              const QPointF &localPoint,
                              const TransformInterpolation interpolation,
                              const quint16 outside,
                              const bool compactConstant)
{
    if (source.isNull() || localExtent.isEmpty()
        || localPoint.x() < 0.0 || localPoint.y() < 0.0
        || localPoint.x() >= localExtent.width()
        || localPoint.y() >= localExtent.height()) {
        return outside;
    }
    QImage converted;
    const QImage *image = &source;
    if (source.format() != QImage::Format_Grayscale16) {
        converted = source.convertToFormat(QImage::Format_Grayscale16);
        image = &converted;
    }
    if (image->isNull()) {
        return outside;
    }
    if (compactConstant && image->size() == QSize(1, 1)) {
        return reinterpret_cast<const quint16 *>(image->constScanLine(0))[0];
    }

    const double sx = localPoint.x() * image->width() / localExtent.width() - 0.5;
    const double sy = localPoint.y() * image->height() / localExtent.height() - 0.5;
    if (interpolation == TransformInterpolation::NearestNeighbour) {
        return greyAt(*image,
                      static_cast<int>(std::floor(sx + 0.5)),
                      static_cast<int>(std::floor(sy + 0.5)),
                      outside);
    }
    if (interpolation == TransformInterpolation::Bilinear) {
        const int x0 = static_cast<int>(std::floor(sx));
        const int y0 = static_cast<int>(std::floor(sy));
        const double fx = sx - x0;
        const double fy = sy - y0;
        const double top = greyAt(*image, x0, y0, outside)
            + (static_cast<double>(greyAt(*image, x0 + 1, y0, outside))
               - greyAt(*image, x0, y0, outside)) * fx;
        const double bottom = greyAt(*image, x0, y0 + 1, outside)
            + (static_cast<double>(greyAt(*image, x0 + 1, y0 + 1, outside))
               - greyAt(*image, x0, y0 + 1, outside)) * fx;
        return roundedComponent(top + (bottom - top) * fy);
    }

    const bool lanczos = interpolation == TransformInterpolation::Lanczos3;
    const int radius = lanczos ? 3 : 2;
    const int firstX = static_cast<int>(std::floor(sx)) - (radius - 1);
    const int lastX = static_cast<int>(std::floor(sx)) + radius;
    const int firstY = static_cast<int>(std::floor(sy)) - (radius - 1);
    const int lastY = static_cast<int>(std::floor(sy)) + radius;
    return roundedComponent(separableSample(
        sx, sy, firstX, lastX, firstY, lastY,
        [&](const int x, const int y) {
            return static_cast<double>(greyAt(*image, x, y, outside));
        },
        lanczos));
}

} // namespace vfx
