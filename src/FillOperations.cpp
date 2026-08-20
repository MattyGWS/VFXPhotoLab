#include "FillOperations.h"

#include "SelectionLocalEditing.h"

#include <QColorSpace>
#include <QRgba64>
#include <QVector>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace vfx {
namespace {

struct MatchReference {
    int components[4] {0, 0, 0, 0};
    int scalar = 0;
    bool sixteenBit = false;
};

int grey16(const QRgba64 pixel)
{
    const std::uint64_t weighted = std::uint64_t(pixel.red()) * 19595u
        + std::uint64_t(pixel.green()) * 38470u
        + std::uint64_t(pixel.blue()) * 7471u + 32768u;
    return static_cast<int>(weighted >> 16u);
}

int lerpInteger(const int before, const int after, const int coverage)
{
    return (before * (255 - coverage) + after * coverage + 127) / 255;
}

QImage targetSelectionCoverage(const FillCoverageRequest &request,
                               const SelectionMask *selection)
{
    if (!selection || !selection->isActive()) {
        return {};
    }
    QImage coverage(request.sourceImage.size(), QImage::Format_Grayscale8);
    if (coverage.isNull()) {
        return {};
    }
    if (selection->isEmpty()) {
        coverage.fill(0);
        return coverage;
    }
    if (selection->isFull() && request.targetToDocument.isIdentity()
        && request.sourceImage.size() == request.documentSize) {
        coverage.fill(255);
        return coverage;
    }
    for (int y = 0; y < coverage.height(); ++y) {
        uchar *row = coverage.scanLine(y);
        for (int x = 0; x < coverage.width(); ++x) {
            row[x] = static_cast<uchar>(std::clamp(
                qRound(sampleSelectionCoverage(*selection,
                                               request.targetToDocument,
                                               QPointF(x + 0.5, y + 0.5))
                       * 255.0),
                0,
                255));
        }
    }
    return coverage;
}

int selectionCoverageAt(const QImage &selectionCoverage,
                        const int x,
                        const int y)
{
    return selectionCoverage.isNull()
        ? 255 : selectionCoverage.constScanLine(y)[x];
}

MatchReference referenceAt(const QImage &source,
                           const FillTarget target,
                           const int componentIndex,
                           const QPoint &point)
{
    MatchReference reference;
    reference.sixteenBit = source.depth() > 32;
    if (target == FillTarget::Mask) {
        const QImage mask = source.convertToFormat(QImage::Format_Grayscale8);
        reference.scalar = mask.constScanLine(point.y())[point.x()];
        return reference;
    }
    if (reference.sixteenBit) {
        const QImage rgba = source.convertToFormat(QImage::Format_RGBA64);
        const auto *row = reinterpret_cast<const QRgba64 *>(rgba.constScanLine(point.y()));
        const QRgba64 pixel = row[point.x()];
        reference.components[0] = pixel.red();
        reference.components[1] = pixel.green();
        reference.components[2] = pixel.blue();
        reference.components[3] = pixel.alpha();
        reference.scalar = target == FillTarget::GreyChannel
            ? grey16(pixel)
            : target == FillTarget::ComponentChannel
                ? reference.components[std::clamp(componentIndex, 0, 3)] : 0;
    } else {
        const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);
        const uchar *pixel = rgba.constScanLine(point.y()) + point.x() * 4;
        for (int component = 0; component < 4; ++component) {
            reference.components[component] = pixel[component];
        }
        reference.scalar = target == FillTarget::GreyChannel
            ? qGray(pixel[0], pixel[1], pixel[2])
            : target == FillTarget::ComponentChannel
                ? reference.components[std::clamp(componentIndex, 0, 3)] : 0;
    }
    return reference;
}

class PixelMatcher final {
public:
    PixelMatcher(const QImage &source,
                 const FillTarget target,
                 const int componentIndex,
                 const MatchReference &reference,
                 const int tolerance)
        : m_target(target),
          m_componentIndex(std::clamp(componentIndex, 0, 3)),
          m_reference(reference),
          m_threshold(std::clamp(tolerance, 0, 255)
                      * (reference.sixteenBit ? 257 : 1))
    {
        if (target == FillTarget::Mask) {
            m_mask = source.convertToFormat(QImage::Format_Grayscale8);
        } else if (reference.sixteenBit) {
            m_rgba64 = source.convertToFormat(QImage::Format_RGBA64);
        } else {
            m_rgba8 = source.convertToFormat(QImage::Format_RGBA8888);
        }
    }

    bool valid() const
    {
        return !m_mask.isNull() || !m_rgba8.isNull() || !m_rgba64.isNull();
    }

    bool matches(const int x, const int y) const
    {
        if (m_target == FillTarget::Mask) {
            return std::abs(int(m_mask.constScanLine(y)[x]) - m_reference.scalar)
                <= m_threshold;
        }
        if (!m_rgba64.isNull()) {
            const auto *row = reinterpret_cast<const QRgba64 *>(m_rgba64.constScanLine(y));
            const QRgba64 pixel = row[x];
            if (m_target == FillTarget::GreyChannel) {
                return std::abs(grey16(pixel) - m_reference.scalar) <= m_threshold;
            }
            if (m_target == FillTarget::ComponentChannel) {
                const int values[] {pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
                return std::abs(values[m_componentIndex] - m_reference.scalar)
                    <= m_threshold;
            }
            const int values[] {pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
            const bool bothTransparent = values[3] == 0 && m_reference.components[3] == 0;
            int maximum = std::abs(values[3] - m_reference.components[3]);
            if (!bothTransparent) {
                for (int component = 0; component < 3; ++component) {
                    maximum = std::max(maximum,
                                       std::abs(values[component]
                                                - m_reference.components[component]));
                }
            }
            return maximum <= m_threshold;
        }
        const uchar *pixel = m_rgba8.constScanLine(y) + x * 4;
        if (m_target == FillTarget::GreyChannel) {
            return std::abs(qGray(pixel[0], pixel[1], pixel[2])
                            - m_reference.scalar) <= m_threshold;
        }
        if (m_target == FillTarget::ComponentChannel) {
            return std::abs(int(pixel[m_componentIndex]) - m_reference.scalar)
                <= m_threshold;
        }
        const bool bothTransparent = pixel[3] == 0 && m_reference.components[3] == 0;
        int maximum = std::abs(int(pixel[3]) - m_reference.components[3]);
        if (!bothTransparent) {
            for (int component = 0; component < 3; ++component) {
                maximum = std::max(maximum,
                                   std::abs(int(pixel[component])
                                            - m_reference.components[component]));
            }
        }
        return maximum <= m_threshold;
    }

private:
    FillTarget m_target = FillTarget::RasterPixels;
    int m_componentIndex = 0;
    MatchReference m_reference;
    int m_threshold = 0;
    QImage m_mask;
    QImage m_rgba8;
    QImage m_rgba64;
};

void includePoint(QRect *bounds, const int x, const int y)
{
    if (!bounds) return;
    const QRect pointRect(x, y, 1, 1);
    *bounds = bounds->isEmpty() ? pointRect : bounds->united(pointRect);
}

void copyImageMetadata(const QImage &source, QImage *target)
{
    if (!target || target->isNull()) return;
    target->setColorSpace(source.colorSpace());
    target->setDevicePixelRatio(source.devicePixelRatio());
    target->setDotsPerMeterX(source.dotsPerMeterX());
    target->setDotsPerMeterY(source.dotsPerMeterY());
}

} // namespace

FillCoverageResult buildFillCoverage(const FillCoverageRequest &request)
{
    FillCoverageResult result;
    if (request.sourceImage.isNull() || request.sourceImage.size().isEmpty()
        || request.documentSize.isEmpty()) {
        result.error = QStringLiteral("Fill target pixels are empty");
        return result;
    }
    if (request.target == FillTarget::ComponentChannel
        && (request.componentIndex < 0 || request.componentIndex > 3)) {
        result.error = QStringLiteral("Fill component target is invalid");
        return result;
    }

    bool inverseOk = false;
    const QTransform documentToTarget = request.targetToDocument.inverted(&inverseOk);
    if (!inverseOk) {
        result.error = QStringLiteral("Fill target transform is not invertible");
        return result;
    }
    const QPointF local = documentToTarget.map(request.documentPosition);
    result.seedPixel = QPoint(qFloor(local.x()), qFloor(local.y()));
    if (!request.sourceImage.rect().contains(result.seedPixel)) {
        result.error = QStringLiteral("Fill click is outside the editable target");
        return result;
    }

    SelectionMask restoredSelection(request.selectionSnapshot.size);
    const SelectionMask *selection = nullptr;
    if (request.selectionSnapshot.active) {
        if (request.selectionSnapshot.size != request.documentSize
            || !restoredSelection.restoreSnapshot(request.selectionSnapshot, false)) {
            result.error = QStringLiteral("The active selection could not be restored for Fill");
            return result;
        }
        selection = &restoredSelection;
    }
    const QImage selectionCoverage = targetSelectionCoverage(request, selection);
    if (selection && selectionCoverage.isNull()) {
        result.error = QStringLiteral("The active selection coverage could not be prepared for Fill");
        return result;
    }
    if (selectionCoverageAt(selectionCoverage,
                            result.seedPixel.x(),
                            result.seedPixel.y()) == 0) {
        result.error = QStringLiteral("Fill click is outside the active selection");
        return result;
    }

    const MatchReference reference = referenceAt(request.sourceImage,
                                                 request.target,
                                                 request.componentIndex,
                                                 result.seedPixel);
    const PixelMatcher matcher(request.sourceImage,
                               request.target,
                               request.componentIndex,
                               reference,
                               request.tolerance);
    if (!matcher.valid()) {
        result.error = QStringLiteral("Fill target could not be converted for matching");
        return result;
    }

    result.coverage = QImage(request.sourceImage.size(), QImage::Format_Grayscale8);
    if (result.coverage.isNull()) {
        result.error = QStringLiteral("Fill coverage allocation failed");
        return result;
    }
    result.coverage.fill(0);

    const auto candidate = [&](const int x, const int y) {
        return x >= 0 && y >= 0
            && x < request.sourceImage.width() && y < request.sourceImage.height()
            && selectionCoverageAt(selectionCoverage, x, y) > 0
            && matcher.matches(x, y);
    };

    if (!request.contiguous) {
        for (int y = 0; y < result.coverage.height(); ++y) {
            uchar *output = result.coverage.scanLine(y);
            for (int x = 0; x < result.coverage.width(); ++x) {
                if (!candidate(x, y)) continue;
                output[x] = static_cast<uchar>(selectionCoverageAt(selectionCoverage, x, y));
                includePoint(&result.affectedRect, x, y);
                ++result.matchedPixelCount;
            }
        }
        return result;
    }

    QVector<QPoint> pending;
    pending.reserve(1024);
    pending.push_back(result.seedPixel);
    while (!pending.isEmpty()) {
        const QPoint seed = pending.takeLast();
        if (!candidate(seed.x(), seed.y())
            || result.coverage.constScanLine(seed.y())[seed.x()] != 0) {
            continue;
        }

        int left = seed.x();
        while (left > 0
               && result.coverage.constScanLine(seed.y())[left - 1] == 0
               && candidate(left - 1, seed.y())) {
            --left;
        }

        bool spanAbove = false;
        bool spanBelow = false;
        for (int x = left; x < result.coverage.width(); ++x) {
            uchar *row = result.coverage.scanLine(seed.y());
            if (row[x] != 0 || !candidate(x, seed.y())) break;
            row[x] = static_cast<uchar>(selectionCoverageAt(selectionCoverage,
                                                            x,
                                                            seed.y()));
            includePoint(&result.affectedRect, x, seed.y());
            ++result.matchedPixelCount;

            if (seed.y() > 0) {
                const bool above = result.coverage.constScanLine(seed.y() - 1)[x] == 0
                    && candidate(x, seed.y() - 1);
                if (above && !spanAbove) pending.push_back(QPoint(x, seed.y() - 1));
                spanAbove = above;
            }
            if (seed.y() + 1 < result.coverage.height()) {
                const bool below = result.coverage.constScanLine(seed.y() + 1)[x] == 0
                    && candidate(x, seed.y() + 1);
                if (below && !spanBelow) pending.push_back(QPoint(x, seed.y() + 1));
                spanBelow = below;
            }
        }
    }
    return result;
}

FillApplyResult applyFillCoverageCpu(const QImage &sourceImage,
                                     const QImage &coverageImage,
                                     const FillTarget target,
                                     const int componentIndex,
                                     const QColor &colour,
                                     const bool preserveTransparency)
{
    FillApplyResult result;
    if (sourceImage.isNull() || coverageImage.isNull()
        || sourceImage.size() != coverageImage.size()) {
        result.error = QStringLiteral("Fill application images are invalid");
        return result;
    }
    if (target == FillTarget::ComponentChannel
        && (componentIndex < 0 || componentIndex > 3)) {
        result.error = QStringLiteral("Fill component target is invalid");
        return result;
    }
    const QImage coverage = coverageImage.convertToFormat(QImage::Format_Grayscale8);
    if (coverage.isNull()) {
        result.error = QStringLiteral("Fill coverage could not be converted");
        return result;
    }

    if (target == FillTarget::Mask) {
        result.image = sourceImage.convertToFormat(QImage::Format_Grayscale8);
        if (result.image.isNull()) {
            result.error = QStringLiteral("Fill mask could not be materialised");
            return result;
        }
        result.image.detach();
        const int desired = qGray(colour.rgb());
        for (int y = 0; y < result.image.height(); ++y) {
            uchar *output = result.image.scanLine(y);
            const uchar *amounts = coverage.constScanLine(y);
            for (int x = 0; x < result.image.width(); ++x) {
                const int amount = amounts[x];
                if (amount == 0) continue;
                const int before = output[x];
                const int after = lerpInteger(before, desired, amount);
                if (after == before) continue;
                output[x] = static_cast<uchar>(after);
                includePoint(&result.affectedRect, x, y);
                ++result.changedPixelCount;
            }
        }
        copyImageMetadata(sourceImage, &result.image);
        return result;
    }

    const bool sixteenBit = sourceImage.depth() > 32;
    if (sixteenBit) {
        result.image = sourceImage.convertToFormat(QImage::Format_RGBA64);
        if (result.image.isNull()) {
            result.error = QStringLiteral("16-bit Fill target could not be materialised");
            return result;
        }
        result.image.detach();
        const QRgba64 desired = colour.rgba64();
        const int desiredScalar = qGray(colour.rgb()) * 257;
        for (int y = 0; y < result.image.height(); ++y) {
            auto *output = reinterpret_cast<QRgba64 *>(result.image.scanLine(y));
            const uchar *amounts = coverage.constScanLine(y);
            for (int x = 0; x < result.image.width(); ++x) {
                const int amount = amounts[x];
                if (amount == 0) continue;
                const QRgba64 before = output[x];
                int values[] {before.red(), before.green(), before.blue(), before.alpha()};
                int after[4] {values[0], values[1], values[2], values[3]};
                if (target == FillTarget::RasterPixels) {
                    const int desiredValues[] {desired.red(), desired.green(), desired.blue(), desired.alpha()};
                    if (preserveTransparency) {
                        for (int component = 0; component < 3; ++component) {
                            after[component] = lerpInteger(values[component],
                                                           desiredValues[component],
                                                           amount);
                        }
                    } else {
                        const double coverage = amount / 255.0;
                        const double beforeAlpha = values[3] / 65535.0;
                        const double desiredAlpha = desiredValues[3] / 65535.0;
                        const double outputAlpha = beforeAlpha
                            + (desiredAlpha - beforeAlpha) * coverage;
                        after[3] = std::clamp(qRound(outputAlpha * 65535.0),
                                              0,
                                              65535);
                        if (outputAlpha > 1.0e-9) {
                            for (int component = 0; component < 3; ++component) {
                                const double premultiplied =
                                    (values[component] / 65535.0) * beforeAlpha
                                        * (1.0 - coverage)
                                    + (desiredValues[component] / 65535.0) * desiredAlpha
                                        * coverage;
                                after[component] = std::clamp(
                                    qRound(premultiplied / outputAlpha * 65535.0),
                                    0,
                                    65535);
                            }
                        }
                    }
                } else if (target == FillTarget::GreyChannel) {
                    for (int component = 0; component < 3; ++component) {
                        after[component] = lerpInteger(values[component], desiredScalar, amount);
                    }
                } else {
                    after[componentIndex] = lerpInteger(values[componentIndex],
                                                        desiredScalar,
                                                        amount);
                }
                if (after[0] == values[0] && after[1] == values[1]
                    && after[2] == values[2] && after[3] == values[3]) {
                    continue;
                }
                output[x] = QRgba64::fromRgba64(static_cast<quint16>(after[0]),
                                                static_cast<quint16>(after[1]),
                                                static_cast<quint16>(after[2]),
                                                static_cast<quint16>(after[3]));
                includePoint(&result.affectedRect, x, y);
                ++result.changedPixelCount;
            }
        }
    } else {
        result.image = sourceImage.convertToFormat(QImage::Format_RGBA8888);
        if (result.image.isNull()) {
            result.error = QStringLiteral("Fill target could not be materialised");
            return result;
        }
        result.image.detach();
        const int desired[] {colour.red(), colour.green(), colour.blue(), colour.alpha()};
        const int desiredScalar = qGray(colour.rgb());
        for (int y = 0; y < result.image.height(); ++y) {
            uchar *output = result.image.scanLine(y);
            const uchar *amounts = coverage.constScanLine(y);
            for (int x = 0; x < result.image.width(); ++x) {
                const int amount = amounts[x];
                if (amount == 0) continue;
                uchar *pixel = output + x * 4;
                const int before[] {pixel[0], pixel[1], pixel[2], pixel[3]};
                int after[4] {before[0], before[1], before[2], before[3]};
                if (target == FillTarget::RasterPixels) {
                    if (preserveTransparency) {
                        for (int component = 0; component < 3; ++component) {
                            after[component] = lerpInteger(before[component],
                                                           desired[component],
                                                           amount);
                        }
                    } else {
                        const double coverage = amount / 255.0;
                        const double beforeAlpha = before[3] / 255.0;
                        const double desiredAlpha = desired[3] / 255.0;
                        const double outputAlpha = beforeAlpha
                            + (desiredAlpha - beforeAlpha) * coverage;
                        after[3] = std::clamp(qRound(outputAlpha * 255.0), 0, 255);
                        if (outputAlpha > 1.0e-9) {
                            for (int component = 0; component < 3; ++component) {
                                const double premultiplied =
                                    (before[component] / 255.0) * beforeAlpha
                                        * (1.0 - coverage)
                                    + (desired[component] / 255.0) * desiredAlpha
                                        * coverage;
                                after[component] = std::clamp(
                                    qRound(premultiplied / outputAlpha * 255.0),
                                    0,
                                    255);
                            }
                        }
                    }
                } else if (target == FillTarget::GreyChannel) {
                    for (int component = 0; component < 3; ++component) {
                        after[component] = lerpInteger(before[component], desiredScalar, amount);
                    }
                } else {
                    after[componentIndex] = lerpInteger(before[componentIndex],
                                                        desiredScalar,
                                                        amount);
                }
                if (after[0] == before[0] && after[1] == before[1]
                    && after[2] == before[2] && after[3] == before[3]) {
                    continue;
                }
                for (int component = 0; component < 4; ++component) {
                    pixel[component] = static_cast<uchar>(after[component]);
                }
                includePoint(&result.affectedRect, x, y);
                ++result.changedPixelCount;
            }
        }
    }
    copyImageMetadata(sourceImage, &result.image);
    return result;
}

QImage compactUniformFillMask(const QImage &maskImage)
{
    QImage mask = maskImage.convertToFormat(QImage::Format_Grayscale8);
    if (mask.isNull() || mask.size().isEmpty()) return {};
    const uchar first = mask.constScanLine(0)[0];
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            if (row[x] != first) return mask;
        }
    }
    QImage compact(1, 1, QImage::Format_Grayscale8);
    compact.fill(first);
    copyImageMetadata(maskImage, &compact);
    return compact;
}

} // namespace vfx
