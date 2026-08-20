#include "SmudgeBrush.h"

#include <QColorSpace>
#include <QRgba64>
#include <QRectF>
#include <QSizeF>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace vfx {
namespace {

constexpr double Epsilon = 1.0e-12;

struct RgbaF {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;
};

double stampCoverageSquared(const double distanceSquared,
                            const double radius,
                            const double hardness)
{
    const double radiusSquared = radius * radius;
    if (distanceSquared >= radiusSquared) {
        return 0.0;
    }
    const double safeHardness = std::clamp(hardness, 0.0, 0.9999);
    const double hardRadius = radius * safeHardness;
    if (distanceSquared <= hardRadius * hardRadius) {
        return 1.0;
    }
    const double normalisedDistance = std::sqrt(distanceSquared) / radius;
    const double t = (normalisedDistance - safeHardness)
        / (1.0 - safeHardness);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

bool hasNormalisedFormat(const QImage &image, const SmudgeBrushTarget target)
{
    if (target == SmudgeBrushTarget::Mask) {
        return image.format() == QImage::Format_Grayscale8;
    }
    return image.format() == QImage::Format_RGBA8888
        || image.format() == QImage::Format_RGBA64;
}

QImage normalisedImage(const QImage &image, const SmudgeBrushTarget target)
{
    if (target == SmudgeBrushTarget::Mask) {
        return image.convertToFormat(QImage::Format_Grayscale8);
    }
    QImage converted = image.convertToFormat(
        image.depth() > 32 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    converted.setColorSpace(image.colorSpace());
    return converted;
}

bool copyRectToScratch(const QImage &source,
                       const QRect &rect,
                       QImage *scratch,
                       QSize *dataSize)
{
    if (!scratch || !dataSize || source.isNull() || rect.isEmpty()
        || !source.rect().contains(rect)
        || (source.format() != QImage::Format_RGBA8888
            && source.format() != QImage::Format_RGBA64
            && source.format() != QImage::Format_Grayscale8)) {
        return false;
    }
    if (scratch->format() != source.format()
        || scratch->width() < rect.width()
        || scratch->height() < rect.height()) {
        const QSize capacity(std::max(scratch->width(), rect.width()),
                             std::max(scratch->height(), rect.height()));
        *scratch = QImage(capacity, source.format());
    }
    if (scratch->isNull()) {
        return false;
    }
    // Scratch pixels are sampled numerically; carrying colour-space metadata
    // here only adds per-dab metadata work and can trigger unnecessary QImage
    // detachment on some Qt builds.
    *dataSize = rect.size();
    const qsizetype bytesPerPixel = source.depth() / 8;
    const qsizetype rowBytes = static_cast<qsizetype>(rect.width())
        * bytesPerPixel;
    if (bytesPerPixel <= 0 || rowBytes <= 0
        || rowBytes > scratch->bytesPerLine()
        || static_cast<qsizetype>(rect.x()) * bytesPerPixel + rowBytes
            > source.bytesPerLine()) {
        return false;
    }
    for (int row = 0; row < rect.height(); ++row) {
        const uchar *sourceRow = source.constScanLine(rect.y() + row)
            + static_cast<qsizetype>(rect.x()) * bytesPerPixel;
        std::memcpy(scratch->scanLine(row), sourceRow,
                    static_cast<size_t>(rowBytes));
    }
    return true;
}

QString requestError(const SmudgeBrushRequest &request)
{
    if (request.destination.isNull()) {
        return QStringLiteral("The Smudge destination is empty");
    }
    if (request.diameterPixels <= 0.0) {
        return QStringLiteral("The Smudge brush size is invalid");
    }
    if (request.target == SmudgeBrushTarget::ComponentChannel
        && (request.componentIndex < 0 || request.componentIndex > 3)) {
        return QStringLiteral("The selected Smudge component channel is invalid");
    }
    if (!request.selectionCoverage.isNull()
        && request.selectionCoverage.size() != QSize(1, 1)
        && request.selectionCoverage.size() != request.destination.size()) {
        return QStringLiteral("The Smudge selection coverage size is invalid");
    }
    return {};
}

double selectionCoverageAt(const QImage &coverage, const int x, const int y)
{
    if (coverage.isNull()) {
        return 1.0;
    }
    if (coverage.size() == QSize(1, 1)) {
        return coverage.constScanLine(0)[0] / 255.0;
    }
    if (x < 0 || y < 0 || x >= coverage.width() || y >= coverage.height()) {
        return 0.0;
    }
    return coverage.constScanLine(y)[x] / 255.0;
}

RgbaF pixelAt(const QImage &image, const int x, const int y,
              const QSize &sampleSize = {})
{
    const int sampleWidth = sampleSize.isEmpty()
        ? image.width() : std::min(sampleSize.width(), image.width());
    const int sampleHeight = sampleSize.isEmpty()
        ? image.height() : std::min(sampleSize.height(), image.height());
    if (sampleWidth <= 0 || sampleHeight <= 0) {
        return {};
    }
    const int safeX = std::clamp(x, 0, sampleWidth - 1);
    const int safeY = std::clamp(y, 0, sampleHeight - 1);
    if (image.depth() > 32) {
        const auto *row = reinterpret_cast<const QRgba64 *>(image.constScanLine(safeY));
        const QRgba64 pixel = row[safeX];
        return {pixel.red() / 65535.0,
                pixel.green() / 65535.0,
                pixel.blue() / 65535.0,
                pixel.alpha() / 65535.0};
    }
    const uchar *pixel = image.constScanLine(safeY) + safeX * 4;
    return {pixel[0] / 255.0,
            pixel[1] / 255.0,
            pixel[2] / 255.0,
            pixel[3] / 255.0};
}

double scalarFromRgba(const RgbaF &pixel)
{
    return std::clamp(0.2126 * pixel.r + 0.7152 * pixel.g + 0.0722 * pixel.b,
                      0.0, 1.0);
}

double scalarAt(const QImage &image,
                const SmudgeBrushTarget target,
                const int componentIndex,
                const int x,
                const int y,
                const QSize &sampleSize = {})
{
    const int sampleWidth = sampleSize.isEmpty()
        ? image.width() : std::min(sampleSize.width(), image.width());
    const int sampleHeight = sampleSize.isEmpty()
        ? image.height() : std::min(sampleSize.height(), image.height());
    if (sampleWidth <= 0 || sampleHeight <= 0) {
        return 0.0;
    }
    if (target == SmudgeBrushTarget::Mask) {
        const int safeX = std::clamp(x, 0, sampleWidth - 1);
        const int safeY = std::clamp(y, 0, sampleHeight - 1);
        return image.constScanLine(safeY)[safeX] / 255.0;
    }
    const RgbaF pixel = pixelAt(image, x, y, sampleSize);
    if (target == SmudgeBrushTarget::ComponentChannel) {
        const double channels[4] {pixel.r, pixel.g, pixel.b, pixel.a};
        return channels[componentIndex];
    }
    return scalarFromRgba(pixel);
}

RgbaF bilinearRgbaFixedOffset(const QImage &image,
                                const int x,
                                const int y,
                                const int offsetX0,
                                const int offsetY0,
                                const double fx,
                                const double fy,
                                const QSize &sampleSize)
{
    const int width = std::min(sampleSize.width(), image.width());
    const int height = std::min(sampleSize.height(), image.height());
    if (width <= 0 || height <= 0) {
        return {};
    }
    const int rawX0 = x + offsetX0;
    const int rawY0 = y + offsetY0;
    const int x0 = std::clamp(rawX0, 0, width - 1);
    const int y0 = std::clamp(rawY0, 0, height - 1);
    const int x1 = rawX0 < 0 || rawX0 >= width - 1 ? x0 : x0 + 1;
    const int y1 = rawY0 < 0 || rawY0 >= height - 1 ? y0 : y0 + 1;
    const double sampleFx = x1 == x0 ? 0.0 : fx;
    const double sampleFy = y1 == y0 ? 0.0 : fy;
    const double w00 = (1.0 - sampleFx) * (1.0 - sampleFy);
    const double w10 = sampleFx * (1.0 - sampleFy);
    const double w01 = (1.0 - sampleFx) * sampleFy;
    const double w11 = sampleFx * sampleFy;
    const std::array<RgbaF, 4> pixels {
        pixelAt(image, x0, y0, sampleSize),
        pixelAt(image, x1, y0, sampleSize),
        pixelAt(image, x0, y1, sampleSize),
        pixelAt(image, x1, y1, sampleSize)
    };
    const std::array<double, 4> weights {w00, w10, w01, w11};

    double alpha = 0.0;
    RgbaF associated;
    RgbaF hidden;
    for (int index = 0; index < 4; ++index) {
        const double weight = weights[index];
        const RgbaF pixel = pixels[index];
        alpha += pixel.a * weight;
        associated.r += pixel.r * pixel.a * weight;
        associated.g += pixel.g * pixel.a * weight;
        associated.b += pixel.b * pixel.a * weight;
        hidden.r += pixel.r * weight;
        hidden.g += pixel.g * weight;
        hidden.b += pixel.b * weight;
    }
    if (alpha > Epsilon) {
        return {associated.r / alpha,
                associated.g / alpha,
                associated.b / alpha,
                alpha};
    }
    return {hidden.r, hidden.g, hidden.b, 0.0};
}

double bilinearScalarFixedOffset(const QImage &image,
                                 const SmudgeBrushTarget target,
                                 const int componentIndex,
                                 const int x,
                                 const int y,
                                 const int offsetX0,
                                 const int offsetY0,
                                 const double fx,
                                 const double fy,
                                 const QSize &sampleSize)
{
    const int width = std::min(sampleSize.width(), image.width());
    const int height = std::min(sampleSize.height(), image.height());
    if (width <= 0 || height <= 0) {
        return 0.0;
    }
    const int rawX0 = x + offsetX0;
    const int rawY0 = y + offsetY0;
    const int x0 = std::clamp(rawX0, 0, width - 1);
    const int y0 = std::clamp(rawY0, 0, height - 1);
    const int x1 = rawX0 < 0 || rawX0 >= width - 1 ? x0 : x0 + 1;
    const int y1 = rawY0 < 0 || rawY0 >= height - 1 ? y0 : y0 + 1;
    const double sampleFx = x1 == x0 ? 0.0 : fx;
    const double sampleFy = y1 == y0 ? 0.0 : fy;
    const double value00 = scalarAt(image, target, componentIndex, x0, y0, sampleSize);
    const double value10 = scalarAt(image, target, componentIndex, x1, y0, sampleSize);
    const double value01 = scalarAt(image, target, componentIndex, x0, y1, sampleSize);
    const double value11 = scalarAt(image, target, componentIndex, x1, y1, sampleSize);
    const double top = value00 + (value10 - value00) * sampleFx;
    const double bottom = value01 + (value11 - value01) * sampleFx;
    return std::clamp(top + (bottom - top) * sampleFy, 0.0, 1.0);
}

RgbaF blendStraightRgba(const RgbaF &destination,
                        const RgbaF &source,
                        const double amount)
{
    const double coverage = std::clamp(amount, 0.0, 1.0);
    const double inverse = 1.0 - coverage;
    const double alpha = destination.a * inverse + source.a * coverage;
    if (alpha > Epsilon) {
        return {(destination.r * destination.a * inverse
                 + source.r * source.a * coverage) / alpha,
                (destination.g * destination.a * inverse
                 + source.g * source.a * coverage) / alpha,
                (destination.b * destination.a * inverse
                 + source.b * source.a * coverage) / alpha,
                alpha};
    }
    return {destination.r * inverse + source.r * coverage,
            destination.g * inverse + source.g * coverage,
            destination.b * inverse + source.b * coverage,
            0.0};
}

bool validWritablePixel(const QImage &image,
                        const int x,
                        const int y,
                        const qsizetype bytesPerPixel)
{
    if (image.isNull() || x < 0 || y < 0
        || x >= image.width() || y >= image.height()
        || bytesPerPixel <= 0) {
        return false;
    }
    const qsizetype offset = static_cast<qsizetype>(x) * bytesPerPixel;
    return offset >= 0 && offset + bytesPerPixel <= image.bytesPerLine();
}

bool writeRgba(QImage *image, const int x, const int y, const RgbaF &value)
{
    if (!image) {
        return false;
    }
    if (image->format() == QImage::Format_RGBA64) {
        if (!validWritablePixel(*image, x, y, 8)) {
            return false;
        }
        auto *row = reinterpret_cast<QRgba64 *>(image->scanLine(y));
        row[x] = QRgba64::fromRgba64(
            static_cast<quint16>(std::clamp(qRound(value.r * 65535.0), 0, 65535)),
            static_cast<quint16>(std::clamp(qRound(value.g * 65535.0), 0, 65535)),
            static_cast<quint16>(std::clamp(qRound(value.b * 65535.0), 0, 65535)),
            static_cast<quint16>(std::clamp(qRound(value.a * 65535.0), 0, 65535)));
        return true;
    }
    if (image->format() != QImage::Format_RGBA8888
        || !validWritablePixel(*image, x, y, 4)) {
        return false;
    }
    uchar *pixel = image->scanLine(y) + static_cast<qsizetype>(x) * 4;
    pixel[0] = static_cast<uchar>(std::clamp(qRound(value.r * 255.0), 0, 255));
    pixel[1] = static_cast<uchar>(std::clamp(qRound(value.g * 255.0), 0, 255));
    pixel[2] = static_cast<uchar>(std::clamp(qRound(value.b * 255.0), 0, 255));
    pixel[3] = static_cast<uchar>(std::clamp(qRound(value.a * 255.0), 0, 255));
    return true;
}

bool writeScalar(QImage *image,
                 const SmudgeBrushTarget target,
                 const int componentIndex,
                 const int x,
                 const int y,
                 const double value)
{
    if (!image) {
        return false;
    }
    const double clamped = std::clamp(value, 0.0, 1.0);
    if (target == SmudgeBrushTarget::Mask) {
        if (image->format() != QImage::Format_Grayscale8
            || !validWritablePixel(*image, x, y, 1)) {
            return false;
        }
        image->scanLine(y)[x] = static_cast<uchar>(
            std::clamp(qRound(clamped * 255.0), 0, 255));
        return true;
    }
    if (image->format() == QImage::Format_RGBA64) {
        if (!validWritablePixel(*image, x, y, 8)) {
            return false;
        }
        auto *row = reinterpret_cast<QRgba64 *>(image->scanLine(y));
        const QRgba64 pixel = row[x];
        quint16 channels[4] {pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
        const quint16 encoded = static_cast<quint16>(
            std::clamp(qRound(clamped * 65535.0), 0, 65535));
        if (target == SmudgeBrushTarget::GreyChannel) {
            channels[0] = encoded;
            channels[1] = encoded;
            channels[2] = encoded;
        } else if (componentIndex >= 0 && componentIndex < 4) {
            channels[componentIndex] = encoded;
        } else {
            return false;
        }
        row[x] = QRgba64::fromRgba64(channels[0], channels[1],
                                      channels[2], channels[3]);
        return true;
    }
    if (image->format() != QImage::Format_RGBA8888
        || !validWritablePixel(*image, x, y, 4)) {
        return false;
    }
    uchar *pixel = image->scanLine(y) + static_cast<qsizetype>(x) * 4;
    const uchar encoded = static_cast<uchar>(
        std::clamp(qRound(clamped * 255.0), 0, 255));
    if (target == SmudgeBrushTarget::GreyChannel) {
        pixel[0] = encoded;
        pixel[1] = encoded;
        pixel[2] = encoded;
    } else if (componentIndex >= 0 && componentIndex < 4) {
        pixel[componentIndex] = encoded;
    } else {
        return false;
    }
    return true;
}

QRect dabRect(const QPointF &centre, const double radius, const QRect &imageRect)
{
    return QRectF(centre - QPointF(radius, radius),
                  QSizeF(radius * 2.0, radius * 2.0))
        .adjusted(-1.0, -1.0, 1.0, 1.0)
        .toAlignedRect()
        .intersected(imageRect);
}

RgbaF fingerPixel(const QColor &colour)
{
    return {colour.redF(), colour.greenF(), colour.blueF(), colour.alphaF()};
}

void includeChangedPixel(const int x, const int y,
                         int *left, int *top, int *right, int *bottom)
{
    *left = std::min(*left, x);
    *top = std::min(*top, y);
    *right = std::max(*right, x);
    *bottom = std::max(*bottom, y);
}

QRect changedBounds(const int left, const int top,
                    const int right, const int bottom)
{
    if (right < left || bottom < top) {
        return {};
    }
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

QRect applyFingerDab(const SmudgeBrushRequest &request,
                     const QPointF &centre,
                     QImage *working)
{
    if (!request.fingerPainting || !working || working->isNull()) {
        return {};
    }
    const double radius = std::max(0.5, request.diameterPixels * 0.5);
    const QRect rect = dabRect(centre, radius, working->rect());
    if (rect.isEmpty()) {
        return {};
    }
    const RgbaF finger = fingerPixel(request.fingerColour);
    const double fingerScalar = scalarFromRgba(finger);
    const double strength = std::clamp(request.strength, 0.0, 1.0);
    int changedLeft = std::numeric_limits<int>::max();
    int changedTop = std::numeric_limits<int>::max();
    int changedRight = std::numeric_limits<int>::min();
    int changedBottom = std::numeric_limits<int>::min();
    working->detach();
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        for (int x = rect.left(); x <= rect.right(); ++x) {
            const double dx = x + 0.5 - centre.x();
            const double dy = y + 0.5 - centre.y();
            const double amount = stampCoverageSquared(dx * dx + dy * dy,
                                                       radius, request.hardness)
                * strength
                * selectionCoverageAt(request.selectionCoverage, x, y);
            if (amount <= Epsilon) {
                continue;
            }
            if (request.target == SmudgeBrushTarget::RasterRgba) {
                if (!writeRgba(working, x, y,
                               blendStraightRgba(pixelAt(*working, x, y),
                                                 finger, amount))) {
                    continue;
                }
            } else {
                const double before = scalarAt(*working, request.target,
                                               request.componentIndex, x, y);
                if (!writeScalar(working, request.target, request.componentIndex,
                                 x, y, before + (fingerScalar - before) * amount)) {
                    continue;
                }
            }
            includeChangedPixel(x, y, &changedLeft, &changedTop,
                                &changedRight, &changedBottom);
        }
    }
    return changedBounds(changedLeft, changedTop,
                         changedRight, changedBottom);
}

QRect applyTransportDab(const SmudgeBrushRequest &request,
                        const QPointF &previous,
                        const QPointF &centre,
                        SmudgeBrushStrokeState *state,
                        QImage *working)
{
    if (!state || !working || working->isNull()) {
        return {};
    }
    const QPointF movement = centre - previous;
    if (std::hypot(movement.x(), movement.y()) <= 1.0e-7) {
        return {};
    }
    const double radius = std::max(0.5, request.diameterPixels * 0.5);
    const QRect destinationRect = dabRect(centre, radius, working->rect());
    if (destinationRect.isEmpty()) {
        return {};
    }

    // Read every destination and upstream sample from one immutable pre-dab
    // snapshot. This prevents horizontal/vertical scan order from changing the
    // smear and still bounds the copied memory to the active brush footprint.
    QRect readRect = destinationRect.united(
        destinationRect.translated(qFloor(-movement.x()), qFloor(-movement.y())))
        .adjusted(-2, -2, 2, 2)
        .intersected(working->rect());
    if (readRect.isEmpty()) {
        return {};
    }
    if (!copyRectToScratch(*working, readRect, &state->scratchImage,
                           &state->scratchDataSize)) {
        return {};
    }
    const QImage &snapshot = state->scratchImage;
    const QSize scratchDataSize = state->scratchDataSize;
    const double sourceOffsetX = -movement.x();
    const double sourceOffsetY = -movement.y();
    const int sourceOffsetX0 = static_cast<int>(std::floor(sourceOffsetX));
    const int sourceOffsetY0 = static_cast<int>(std::floor(sourceOffsetY));
    const double sourceFx = sourceOffsetX - sourceOffsetX0;
    const double sourceFy = sourceOffsetY - sourceOffsetY0;

    int changedLeft = std::numeric_limits<int>::max();
    int changedTop = std::numeric_limits<int>::max();
    int changedRight = std::numeric_limits<int>::min();
    int changedBottom = std::numeric_limits<int>::min();
    const double strength = std::clamp(request.strength, 0.0, 1.0);
    working->detach();
    for (int y = destinationRect.top(); y <= destinationRect.bottom(); ++y) {
        for (int x = destinationRect.left(); x <= destinationRect.right(); ++x) {
            const double dx = x + 0.5 - centre.x();
            const double dy = y + 0.5 - centre.y();
            const double amount = stampCoverageSquared(
                                      dx * dx + dy * dy,
                                      radius, request.hardness)
                * strength
                * selectionCoverageAt(request.selectionCoverage, x, y);
            if (amount <= Epsilon) {
                continue;
            }
            const int localX = x - readRect.x();
            const int localY = y - readRect.y();
            if (request.target == SmudgeBrushTarget::RasterRgba) {
                // Destination is an integer sample. Only the transported
                // upstream value needs bilinear interpolation; the previous
                // implementation performed two four-tap filters per pixel.
                const RgbaF before = pixelAt(snapshot, localX, localY,
                                             scratchDataSize);
                const RgbaF source = bilinearRgbaFixedOffset(
                    snapshot, localX, localY, sourceOffsetX0, sourceOffsetY0,
                    sourceFx, sourceFy, scratchDataSize);
                if (!writeRgba(working, x, y,
                               blendStraightRgba(before, source, amount))) {
                    continue;
                }
            } else {
                const double before = scalarAt(snapshot, request.target,
                                               request.componentIndex,
                                               localX, localY, scratchDataSize);
                const double source = bilinearScalarFixedOffset(
                    snapshot, request.target, request.componentIndex,
                    localX, localY, sourceOffsetX0, sourceOffsetY0,
                    sourceFx, sourceFy, scratchDataSize);
                if (!writeScalar(working, request.target, request.componentIndex,
                                 x, y, before + (source - before) * amount)) {
                    continue;
                }
            }
            includeChangedPixel(x, y, &changedLeft, &changedTop,
                                &changedRight, &changedBottom);
        }
    }
    return changedBounds(changedLeft, changedTop,
                         changedRight, changedBottom);
}

void uniteRect(QRect *destination, const QRect &rect)
{
    if (!destination || rect.isEmpty()) {
        return;
    }
    *destination = destination->isEmpty() ? rect : destination->united(rect);
}

SmudgeBrushResult appendSegments(const SmudgeBrushRequest &request,
                                 SmudgeBrushStrokeState *state,
                                 QImage *working,
                                 const bool includeImage,
                                 const bool finishStroke)
{
    SmudgeBrushResult result;
    if (!state || !working) {
        result.error = QStringLiteral("The Smudge stroke state is unavailable");
        return result;
    }
    if (working->isNull()) {
        *working = normalisedImage(request.destination, request.target);
    } else if (!hasNormalisedFormat(*working, request.target)) {
        if (state->initialised) {
            result.error = QStringLiteral(
                "The Smudge working-image format changed during the stroke");
            return result;
        }
        *working = normalisedImage(*working, request.target);
    }
    if (working->isNull() || !hasNormalisedFormat(*working, request.target)) {
        result.error = QStringLiteral("The Smudge working image could not be prepared");
        return result;
    }
    if (state->imageSize.isEmpty()) {
        state->imageSize = working->size();
    } else if (state->imageSize != working->size()) {
        result.error = QStringLiteral("The Smudge stroke image size changed");
        return result;
    }

    const double radius = std::max(0.5, request.diameterPixels * 0.5);
    const double spacing = std::max(1.0, radius * 0.16);
    QRect changed;

    const auto initialiseAt = [&](const QPointF &point) {
        state->initialised = true;
        state->lastCentre = point;
        state->lastInputPoint = point;
        state->distanceUntilNextDab = spacing;
        uniteRect(&changed, applyFingerDab(request, point, working));
        if (request.fingerPainting) {
            ++state->appliedDabCount;
        }
    };

    const auto appendLine = [&](const QPointF &rawEnd) {
        const QPointF start = state->lastInputPoint;
        const QPointF delta = rawEnd - start;
        const double length = std::hypot(delta.x(), delta.y());
        if (length <= 1.0e-7) {
            state->lastInputPoint = rawEnd;
            return;
        }

        const QPointF direction(delta.x() / length, delta.y() / length);
        double consumed = 0.0;
        double untilNext = state->distanceUntilNextDab > 1.0e-7
            ? state->distanceUntilNextDab : spacing;
        while (length - consumed + 1.0e-9 >= untilNext) {
            consumed += untilNext;
            const QPointF centre = start + direction * consumed;
            uniteRect(&changed,
                      applyTransportDab(request,
                                        state->lastCentre,
                                        centre,
                                        state,
                                        working));
            state->lastCentre = centre;
            ++state->appliedDabCount;
            untilNext = spacing;
        }
        state->distanceUntilNextDab = untilNext - (length - consumed);
        if (state->distanceUntilNextDab <= 1.0e-7) {
            state->distanceUntilNextDab = spacing;
        }
        state->lastInputPoint = rawEnd;
    };

    for (const QLineF &segment : request.targetSegments) {
        if (!state->initialised) {
            initialiseAt(segment.p1());
        } else {
            const double gap = QLineF(state->lastInputPoint, segment.p1()).length();
            if (gap > 1.0e-7) {
                // Preserve non-contiguous input deterministically rather than
                // resetting dab spacing at every pointer event.
                appendLine(segment.p1());
            }
        }
        appendLine(segment.p2());
    }

    if (finishStroke && state->initialised) {
        const double tailDistance = QLineF(state->lastCentre,
                                           state->lastInputPoint).length();
        if (tailDistance > 1.0e-7) {
            uniteRect(&changed,
                      applyTransportDab(request,
                                        state->lastCentre,
                                        state->lastInputPoint,
                                        state,
                                        working));
            state->lastCentre = state->lastInputPoint;
            state->distanceUntilNextDab = spacing;
            ++state->appliedDabCount;
        }
    }

    uniteRect(&state->affectedRect, changed);
    if (includeImage) {
        result.image = *working;
    }
    result.affectedRect = changed;
    result.appliedDabCount = state->appliedDabCount;
    return result;
}

} // namespace

void SmudgeBrushStrokeState::reset()
{
    imageSize = {};
    initialised = false;
    lastCentre = {};
    lastInputPoint = {};
    distanceUntilNextDab = 0.0;
    appliedDabCount = 0;
    affectedRect = {};
    scratchImage = {};
    scratchDataSize = {};
}

SmudgeBrushResult applySmudgeBrush(const SmudgeBrushRequest &request)
{
    SmudgeBrushResult result;
    result.error = requestError(request);
    if (!result.error.isEmpty()) {
        return result;
    }
    QImage working = normalisedImage(request.destination, request.target);
    if (working.isNull()) {
        result.error = QStringLiteral("The Smudge destination could not be normalised");
        return result;
    }
    SmudgeBrushStrokeState state;
    result = appendSegments(request, &state, &working, true, true);
    result.affectedRect = state.affectedRect;
    result.appliedDabCount = state.appliedDabCount;
    return result;
}

SmudgeBrushResult applySmudgeBrushIncremental(const SmudgeBrushRequest &request,
                                              SmudgeBrushStrokeState *state,
                                              QImage *workingImage)
{
    SmudgeBrushResult result;
    result.error = requestError(request);
    if (!result.error.isEmpty()) {
        return result;
    }
    // The live caller already owns workingImage. Returning another full-frame
    // QImage and then mutating its metadata forced a complete detach/copy on
    // every pointer event. Incremental results therefore carry only status and
    // the newly affected rectangle.
    result = appendSegments(request, state, workingImage, false, request.finishStroke);
    return result;
}

} // namespace vfx
