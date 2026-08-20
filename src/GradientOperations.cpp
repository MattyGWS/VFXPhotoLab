#include "GradientOperations.h"

#include "SelectionLocalEditing.h"

#include <QColorSpace>
#include <QRgba64>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace vfx {
namespace {

void includePoint(QRect *bounds, const int x, const int y)
{
    const QRect point(x, y, 1, 1);
    *bounds = bounds->isEmpty() ? point : bounds->united(point);
}

int lerpInteger(const int a, const int b, const double t, const int maximum)
{
    return std::clamp(qRound(a + (b - a) * std::clamp(t, 0.0, 1.0)), 0, maximum);
}

int blendInteger(const int before, const int after, const int coverage, const int maximum)
{
    return std::clamp((before * (255 - coverage) + after * coverage + 127) / 255,
                      0,
                      maximum);
}

void copyMetadata(const QImage &source, QImage *target)
{
    if (!target || target->isNull()) return;
    target->setColorSpace(source.colorSpace());
    target->setDotsPerMeterX(source.dotsPerMeterX());
    target->setDotsPerMeterY(source.dotsPerMeterY());
    target->setDevicePixelRatio(source.devicePixelRatio());
}


} // namespace

QImage buildGradientSelectionCoverage(const GradientCoverageRequest &request,
                                      QString *error)
{
    if (error) error->clear();
    if (request.targetSize.isEmpty() || request.documentSize.isEmpty()) {
        if (error) *error = QStringLiteral("Gradient target extent is invalid");
        return {};
    }
    QImage coverage(request.targetSize, QImage::Format_Grayscale8);
    if (coverage.isNull()) {
        if (error) *error = QStringLiteral("Gradient selection coverage could not be allocated");
        return {};
    }

    if (!request.selectionSnapshot.active) {
        coverage.fill(255);
        return coverage;
    }
    if (request.selectionSnapshot.size != request.documentSize) {
        if (error) *error = QStringLiteral("Gradient selection extent does not match the document");
        return {};
    }
    SelectionMask selection(request.selectionSnapshot.size);
    if (!selection.restoreSnapshot(request.selectionSnapshot, false)) {
        if (error) *error = QStringLiteral("Gradient selection snapshot is invalid");
        return {};
    }
    if (selection.isEmpty()) {
        coverage.fill(0);
        return coverage;
    }
    if (selection.isFull() && request.targetToDocument.isIdentity()
        && request.targetSize == request.documentSize) {
        coverage.fill(255);
        return coverage;
    }

    for (int y = 0; y < coverage.height(); ++y) {
        uchar *row = coverage.scanLine(y);
        for (int x = 0; x < coverage.width(); ++x) {
            row[x] = static_cast<uchar>(std::clamp(
                qRound(sampleSelectionCoverage(selection,
                                               request.targetToDocument,
                                               QPointF(x + 0.5, y + 0.5))
                       * 255.0),
                0,
                255));
        }
    }
    return coverage;
}

double gradientAmountAt(const QPointF &pixelCentre,
                        const QPointF &start,
                        const QPointF &end,
                        const RasterGradientType type,
                        const bool reverse)
{
    const QPointF direction = end - start;
    const double lengthSquared = QPointF::dotProduct(direction, direction);
    const double length = std::sqrt(std::max(0.0, lengthSquared));
    double amount = 0.0;
    if (length <= 1.0e-9) {
        amount = 1.0;
    } else {
        const QPointF relative = pixelCentre - start;
        switch (type) {
        case RasterGradientType::Linear:
            amount = QPointF::dotProduct(relative, direction) / lengthSquared;
            break;
        case RasterGradientType::Radial:
            amount = std::hypot(relative.x(), relative.y()) / length;
            break;
        case RasterGradientType::Angle: {
            const double base = std::atan2(direction.y(), direction.x());
            const double angle = std::atan2(relative.y(), relative.x());
            constexpr double twoPi = 6.28318530717958647692;
            amount = std::fmod(angle - base + twoPi, twoPi) / twoPi;
            break;
        }
        case RasterGradientType::Reflected:
            amount = std::abs(QPointF::dotProduct(relative, direction) / lengthSquared);
            break;
        case RasterGradientType::Diamond: {
            const QPointF axis(direction.x() / length, direction.y() / length);
            const QPointF perpendicular(-axis.y(), axis.x());
            const double along = QPointF::dotProduct(relative, axis);
            const double across = QPointF::dotProduct(relative, perpendicular);
            amount = (std::abs(along) + std::abs(across)) / length;
            break;
        }
        }
    }
    amount = std::clamp(amount, 0.0, 1.0);
    return reverse ? 1.0 - amount : amount;
}

GradientApplyResult applyGradientCpu(const GradientApplyRequest &request)
{
    GradientApplyResult result;
    if (request.sourceImage.isNull() || request.selectionCoverage.isNull()
        || request.sourceImage.size() != request.selectionCoverage.size()) {
        result.error = QStringLiteral("Gradient source or selection coverage is invalid");
        return result;
    }
    if (request.target == FillTarget::ComponentChannel
        && (request.componentIndex < 0 || request.componentIndex > 3)) {
        result.error = QStringLiteral("Gradient component target is invalid");
        return result;
    }
    const QImage coverage = request.selectionCoverage.convertToFormat(QImage::Format_Grayscale8);
    if (coverage.isNull()) {
        result.error = QStringLiteral("Gradient selection coverage could not be normalised");
        return result;
    }

    if (request.target == FillTarget::Mask) {
        result.image = request.sourceImage.convertToFormat(QImage::Format_Grayscale8);
        if (result.image.isNull()) {
            result.error = QStringLiteral("Gradient mask could not be materialised");
            return result;
        }
        result.image.detach();
        const int startValue = qGray(request.startColour.rgb());
        const int endValue = qGray(request.endColour.rgb());
        for (int y = 0; y < result.image.height(); ++y) {
            uchar *row = result.image.scanLine(y);
            const uchar *selection = coverage.constScanLine(y);
            for (int x = 0; x < result.image.width(); ++x) {
                const int mask = selection[x];
                if (mask == 0) continue;
                const double t = gradientAmountAt(QPointF(x + 0.5, y + 0.5),
                                                  request.start,
                                                  request.end,
                                                  request.type,
                                                  request.reverse);
                const int desired = lerpInteger(startValue, endValue, t, 255);
                const int after = blendInteger(row[x], desired, mask, 255);
                if (after == row[x]) continue;
                row[x] = static_cast<uchar>(after);
                includePoint(&result.affectedRect, x, y);
                ++result.changedPixelCount;
            }
        }
        copyMetadata(request.sourceImage, &result.image);
        return result;
    }

    const bool sixteenBit = request.sourceImage.depth() > 32;
    result.image = request.sourceImage.convertToFormat(
        sixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (result.image.isNull()) {
        result.error = QStringLiteral("Gradient target could not be materialised");
        return result;
    }
    result.image.detach();

    if (sixteenBit) {
        const QRgba64 a = request.startColour.rgba64();
        const QRgba64 b = request.endColour.rgba64();
        const int aValues[] {a.red(), a.green(), a.blue(), a.alpha()};
        const int bValues[] {b.red(), b.green(), b.blue(), b.alpha()};
        const int aGrey = qGray(request.startColour.rgb()) * 257;
        const int bGrey = qGray(request.endColour.rgb()) * 257;
        for (int y = 0; y < result.image.height(); ++y) {
            auto *row = reinterpret_cast<QRgba64 *>(result.image.scanLine(y));
            const uchar *selection = coverage.constScanLine(y);
            for (int x = 0; x < result.image.width(); ++x) {
                const int mask = selection[x];
                if (mask == 0) continue;
                const double t = gradientAmountAt(QPointF(x + 0.5, y + 0.5),
                                                  request.start,
                                                  request.end,
                                                  request.type,
                                                  request.reverse);
                const QRgba64 beforePixel = row[x];
                const int before[] {beforePixel.red(), beforePixel.green(),
                                    beforePixel.blue(), beforePixel.alpha()};
                int after[] {before[0], before[1], before[2], before[3]};
                if (request.target == FillTarget::RasterPixels) {
                    int desired[4];
                    for (int c = 0; c < 4; ++c) desired[c] = lerpInteger(aValues[c], bValues[c], t, 65535);
                    const double coverageAmount = mask / 255.0;
                    const double beforeAlpha = before[3] / 65535.0;
                    const double desiredAlpha = desired[3] / 65535.0;
                    const double outputAlpha = beforeAlpha
                        + (desiredAlpha - beforeAlpha) * coverageAmount;
                    after[3] = std::clamp(qRound(outputAlpha * 65535.0), 0, 65535);
                    if (after[3] > 0) {
                        for (int c = 0; c < 3; ++c) {
                            const double premultiplied =
                                (before[c] / 65535.0) * beforeAlpha * (1.0 - coverageAmount)
                                + (desired[c] / 65535.0) * desiredAlpha * coverageAmount;
                            after[c] = std::clamp(qRound(premultiplied / outputAlpha * 65535.0), 0, 65535);
                        }
                    }
                } else {
                    const int desired = lerpInteger(aGrey, bGrey, t, 65535);
                    if (request.target == FillTarget::GreyChannel) {
                        for (int c = 0; c < 3; ++c) after[c] = blendInteger(before[c], desired, mask, 65535);
                    } else {
                        after[request.componentIndex] = blendInteger(before[request.componentIndex], desired, mask, 65535);
                    }
                }
                if (after[0] == before[0] && after[1] == before[1]
                    && after[2] == before[2] && after[3] == before[3]) continue;
                row[x] = QRgba64::fromRgba64(after[0], after[1], after[2], after[3]);
                includePoint(&result.affectedRect, x, y);
                ++result.changedPixelCount;
            }
        }
    } else {
        const int a[] {request.startColour.red(), request.startColour.green(),
                       request.startColour.blue(), request.startColour.alpha()};
        const int b[] {request.endColour.red(), request.endColour.green(),
                       request.endColour.blue(), request.endColour.alpha()};
        const int aGrey = qGray(request.startColour.rgb());
        const int bGrey = qGray(request.endColour.rgb());
        for (int y = 0; y < result.image.height(); ++y) {
            uchar *row = result.image.scanLine(y);
            const uchar *selection = coverage.constScanLine(y);
            for (int x = 0; x < result.image.width(); ++x) {
                const int mask = selection[x];
                if (mask == 0) continue;
                const double t = gradientAmountAt(QPointF(x + 0.5, y + 0.5),
                                                  request.start,
                                                  request.end,
                                                  request.type,
                                                  request.reverse);
                uchar *pixel = row + x * 4;
                const int before[] {pixel[0], pixel[1], pixel[2], pixel[3]};
                int after[] {before[0], before[1], before[2], before[3]};
                if (request.target == FillTarget::RasterPixels) {
                    int desired[4];
                    for (int c = 0; c < 4; ++c) desired[c] = lerpInteger(a[c], b[c], t, 255);
                    const double coverageAmount = mask / 255.0;
                    const double beforeAlpha = before[3] / 255.0;
                    const double desiredAlpha = desired[3] / 255.0;
                    const double outputAlpha = beforeAlpha
                        + (desiredAlpha - beforeAlpha) * coverageAmount;
                    after[3] = std::clamp(qRound(outputAlpha * 255.0), 0, 255);
                    if (after[3] > 0) {
                        for (int c = 0; c < 3; ++c) {
                            const double premultiplied =
                                (before[c] / 255.0) * beforeAlpha * (1.0 - coverageAmount)
                                + (desired[c] / 255.0) * desiredAlpha * coverageAmount;
                            after[c] = std::clamp(qRound(premultiplied / outputAlpha * 255.0), 0, 255);
                        }
                    }
                } else {
                    const int desired = lerpInteger(aGrey, bGrey, t, 255);
                    if (request.target == FillTarget::GreyChannel) {
                        for (int c = 0; c < 3; ++c) after[c] = blendInteger(before[c], desired, mask, 255);
                    } else {
                        after[request.componentIndex] = blendInteger(before[request.componentIndex], desired, mask, 255);
                    }
                }
                if (after[0] == before[0] && after[1] == before[1]
                    && after[2] == before[2] && after[3] == before[3]) continue;
                for (int c = 0; c < 4; ++c) pixel[c] = static_cast<uchar>(after[c]);
                includePoint(&result.affectedRect, x, y);
                ++result.changedPixelCount;
            }
        }
    }
    copyMetadata(request.sourceImage, &result.image);
    return result;
}

QString rasterGradientTypeDisplayName(const RasterGradientType type)
{
    switch (type) {
    case RasterGradientType::Linear: return QStringLiteral("Linear");
    case RasterGradientType::Radial: return QStringLiteral("Radial");
    case RasterGradientType::Angle: return QStringLiteral("Angle");
    case RasterGradientType::Reflected: return QStringLiteral("Reflected");
    case RasterGradientType::Diamond: return QStringLiteral("Diamond");
    }
    return QStringLiteral("Linear");
}

} // namespace vfx
