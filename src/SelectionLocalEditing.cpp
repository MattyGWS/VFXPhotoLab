#include "SelectionLocalEditing.h"

#include <QColorSpace>
#include <QRectF>
#include <QRgba64>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace vfx {
namespace {

QImage materialisedMaskImage(const QImage &mask, const QSize &size)
{
    if (mask.isNull() || size.isEmpty()) {
        return {};
    }
    if (mask.size() == QSize(1, 1)) {
        QImage expanded(size, QImage::Format_Grayscale8);
        expanded.fill(qGray(mask.pixel(0, 0)));
        return expanded;
    }
    QImage expanded = mask;
    if (expanded.size() != size) {
        expanded = expanded.scaled(size,
                                   Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
    }
    return expanded.convertToFormat(QImage::Format_Grayscale8);
}

QImage straightLayerPixels(const QImage &source,
                           const QSize &size,
                           const QColorSpace &colourSpace,
                           const bool sixteenBit)
{
    const QImage::Format format = sixteenBit
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    if (source.isNull()) {
        QImage image(size, format);
        image.fill(Qt::transparent);
        image.setColorSpace(colourSpace);
        return image;
    }
    QImage image = source.size() == size
        ? source.convertToFormat(format)
        : source.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
              .convertToFormat(format);
    image.setColorSpace(colourSpace);
    return image;
}

QImage compactMaskCoverage(QImage coverage)
{
    if (coverage.isNull()) {
        return {};
    }
    coverage = coverage.convertToFormat(QImage::Format_Grayscale8);
    const uchar first = coverage.constScanLine(0)[0];
    bool uniform = true;
    for (int y = 0; y < coverage.height() && uniform; ++y) {
        const uchar *row = coverage.constScanLine(y);
        for (int x = 0; x < coverage.width(); ++x) {
            if (row[x] != first) {
                uniform = false;
                break;
            }
        }
    }
    if (!uniform) {
        return coverage;
    }
    QImage compact(1, 1, QImage::Format_Grayscale8);
    compact.fill(first);
    return compact;
}

} // namespace

double sampleSelectionCoverage(const SelectionMask &selection,
                               const QTransform &layerToDocument,
                               const QPointF &layerPixelCentre)
{
    if (!selection.isActive()) {
        return 1.0;
    }
    if (selection.isEmpty()) {
        return 0.0;
    }

    const QPointF documentPoint = layerToDocument.map(layerPixelCentre);
    const double sampleX = documentPoint.x() - 0.5;
    const double sampleY = documentPoint.y() - 0.5;
    const int x0 = static_cast<int>(std::floor(sampleX));
    const int y0 = static_cast<int>(std::floor(sampleY));
    const double fx = sampleX - x0;
    const double fy = sampleY - y0;
    const double c00 = selection.coverageAt(x0, y0) / 255.0;
    constexpr double alignedEpsilon = 1.0e-9;
    if (std::abs(fx) <= alignedEpsilon && std::abs(fy) <= alignedEpsilon) {
        return c00;
    }
    if (std::abs(fy) <= alignedEpsilon) {
        const double c10 = selection.coverageAt(x0 + 1, y0) / 255.0;
        return std::clamp(c00 + (c10 - c00) * fx, 0.0, 1.0);
    }
    if (std::abs(fx) <= alignedEpsilon) {
        const double c01 = selection.coverageAt(x0, y0 + 1) / 255.0;
        return std::clamp(c00 + (c01 - c00) * fy, 0.0, 1.0);
    }
    const double c10 = selection.coverageAt(x0 + 1, y0) / 255.0;
    const double c01 = selection.coverageAt(x0, y0 + 1) / 255.0;
    const double c11 = selection.coverageAt(x0 + 1, y0 + 1) / 255.0;
    const double top = c00 + (c10 - c00) * fx;
    const double bottom = c01 + (c11 - c01) * fx;
    return std::clamp(top + (bottom - top) * fy, 0.0, 1.0);
}

bool clipEditedImageToSelection(QImage *editedImage,
                                const QImage &sourceImage,
                                const QRect &requestedRect,
                                const SelectionMask::Snapshot &selectionSnapshot,
                                const QTransform &layerToDocument,
                                const SelectionEditKind kind,
                                const int channelIndex)
{
    if (!editedImage || editedImage->isNull() || !selectionSnapshot.active) {
        return true;
    }
    SelectionMask selection(selectionSnapshot.size);
    if (!selection.restoreSnapshot(selectionSnapshot, false)) {
        return false;
    }
    if (selection.isFull() && layerToDocument.isIdentity()) {
        return true;
    }

    const QRect rect = requestedRect.intersected(editedImage->rect());
    if (rect.isEmpty()) {
        return true;
    }

    if (kind == SelectionEditKind::Mask) {
        QImage source = materialisedMaskImage(sourceImage, editedImage->size());
        if (source.isNull()) {
            return false;
        }
        *editedImage = editedImage->convertToFormat(QImage::Format_Grayscale8);
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            uchar *afterRow = editedImage->scanLine(y);
            const uchar *beforeRow = source.constScanLine(y);
            for (int x = rect.left(); x <= rect.right(); ++x) {
                const double coverage = sampleSelectionCoverage(
                    selection, layerToDocument, QPointF(x + 0.5, y + 0.5));
                afterRow[x] = static_cast<uchar>(std::clamp(
                    qRound(beforeRow[x] + (afterRow[x] - beforeRow[x]) * coverage),
                    0,
                    255));
            }
        }
        return true;
    }

    const bool sixteenBit = editedImage->depth() > 32 || sourceImage.depth() > 32;
    const QColorSpace colourSpace = sourceImage.colorSpace().isValid()
        ? sourceImage.colorSpace() : editedImage->colorSpace();
    QImage source = straightLayerPixels(sourceImage,
                                        editedImage->size(),
                                        colourSpace,
                                        sixteenBit);
    QImage after = editedImage->convertToFormat(
        sixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (source.isNull() || after.isNull()) {
        return false;
    }

    if (sixteenBit) {
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            const auto *beforeRow = reinterpret_cast<const QRgba64 *>(
                source.constScanLine(y));
            auto *afterRow = reinterpret_cast<QRgba64 *>(after.scanLine(y));
            for (int x = rect.left(); x <= rect.right(); ++x) {
                const double coverage = sampleSelectionCoverage(
                    selection, layerToDocument, QPointF(x + 0.5, y + 0.5));
                const QRgba64 before = beforeRow[x];
                const QRgba64 edited = afterRow[x];
                if (kind == SelectionEditKind::ComponentChannel) {
                    quint16 channels[4] {before.red(), before.green(),
                                         before.blue(), before.alpha()};
                    const quint16 editedChannels[4] {edited.red(), edited.green(),
                                                     edited.blue(), edited.alpha()};
                    if (channelIndex >= 0 && channelIndex < 4) {
                        channels[channelIndex] = static_cast<quint16>(std::clamp(
                            qRound(channels[channelIndex]
                                   + (editedChannels[channelIndex]
                                      - channels[channelIndex]) * coverage),
                            0,
                            65535));
                    }
                    afterRow[x] = QRgba64::fromRgba64(channels[0], channels[1],
                                                       channels[2], channels[3]);
                } else if (kind == SelectionEditKind::GreyChannel) {
                    const auto mix = [coverage](const quint16 beforeValue,
                                                const quint16 editedValue) {
                        return static_cast<quint16>(std::clamp(
                            qRound(beforeValue
                                   + (editedValue - beforeValue) * coverage),
                            0,
                            65535));
                    };
                    afterRow[x] = QRgba64::fromRgba64(
                        mix(before.red(), edited.red()),
                        mix(before.green(), edited.green()),
                        mix(before.blue(), edited.blue()),
                        before.alpha());
                } else {
                    const double beforeAlpha = before.alpha() / 65535.0;
                    const double editedAlpha = edited.alpha() / 65535.0;
                    const double outputAlpha = beforeAlpha
                        + (editedAlpha - beforeAlpha) * coverage;
                    const auto premultiplied = [coverage](const quint16 beforeColour,
                                                          const quint16 editedColour,
                                                          const double beforeA,
                                                          const double editedA) {
                        return (beforeColour / 65535.0) * beforeA * (1.0 - coverage)
                            + (editedColour / 65535.0) * editedA * coverage;
                    };
                    quint16 output[3] {before.red(), before.green(), before.blue()};
                    if (outputAlpha > 1.0e-9) {
                        output[0] = static_cast<quint16>(std::clamp(
                            qRound(premultiplied(before.red(), edited.red(),
                                                 beforeAlpha, editedAlpha)
                                   / outputAlpha * 65535.0),
                            0,
                            65535));
                        output[1] = static_cast<quint16>(std::clamp(
                            qRound(premultiplied(before.green(), edited.green(),
                                                 beforeAlpha, editedAlpha)
                                   / outputAlpha * 65535.0),
                            0,
                            65535));
                        output[2] = static_cast<quint16>(std::clamp(
                            qRound(premultiplied(before.blue(), edited.blue(),
                                                 beforeAlpha, editedAlpha)
                                   / outputAlpha * 65535.0),
                            0,
                            65535));
                    }
                    afterRow[x] = QRgba64::fromRgba64(
                        output[0], output[1], output[2],
                        static_cast<quint16>(std::clamp(
                            qRound(outputAlpha * 65535.0), 0, 65535)));
                }
            }
        }
    } else {
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            const uchar *beforeRow = source.constScanLine(y);
            uchar *afterRow = after.scanLine(y);
            for (int x = rect.left(); x <= rect.right(); ++x) {
                const double coverage = sampleSelectionCoverage(
                    selection, layerToDocument, QPointF(x + 0.5, y + 0.5));
                const int offset = x * 4;
                if (kind == SelectionEditKind::ComponentChannel) {
                    if (channelIndex >= 0 && channelIndex < 4) {
                        afterRow[offset + channelIndex] = static_cast<uchar>(
                            std::clamp(qRound(beforeRow[offset + channelIndex]
                                              + (afterRow[offset + channelIndex]
                                                 - beforeRow[offset + channelIndex])
                                                    * coverage),
                                       0,
                                       255));
                    }
                    for (int component = 0; component < 4; ++component) {
                        if (component != channelIndex) {
                            afterRow[offset + component] = beforeRow[offset + component];
                        }
                    }
                } else if (kind == SelectionEditKind::GreyChannel) {
                    for (int component = 0; component < 3; ++component) {
                        afterRow[offset + component] = static_cast<uchar>(
                            std::clamp(qRound(beforeRow[offset + component]
                                              + (afterRow[offset + component]
                                                 - beforeRow[offset + component])
                                                    * coverage),
                                       0,
                                       255));
                    }
                    afterRow[offset + 3] = beforeRow[offset + 3];
                } else {
                    const double beforeAlpha = beforeRow[offset + 3] / 255.0;
                    const double editedAlpha = afterRow[offset + 3] / 255.0;
                    const double outputAlpha = beforeAlpha
                        + (editedAlpha - beforeAlpha) * coverage;
                    uchar outputRgb[3] {beforeRow[offset], beforeRow[offset + 1],
                                        beforeRow[offset + 2]};
                    if (outputAlpha > 1.0e-9) {
                        for (int component = 0; component < 3; ++component) {
                            const double premultiplied =
                                (beforeRow[offset + component] / 255.0) * beforeAlpha
                                    * (1.0 - coverage)
                                + (afterRow[offset + component] / 255.0) * editedAlpha
                                    * coverage;
                            outputRgb[component] = static_cast<uchar>(std::clamp(
                                qRound(premultiplied / outputAlpha * 255.0),
                                0,
                                255));
                        }
                    }
                    afterRow[offset] = outputRgb[0];
                    afterRow[offset + 1] = outputRgb[1];
                    afterRow[offset + 2] = outputRgb[2];
                    afterRow[offset + 3] = static_cast<uchar>(std::clamp(
                        qRound(outputAlpha * 255.0), 0, 255));
                }
            }
        }
    }
    after.setColorSpace(colourSpace);
    *editedImage = after;
    return true;
}

bool clearImageThroughSelection(QImage *resultImage,
                                const QImage &sourceImage,
                                const SelectionMask::Snapshot &selectionSnapshot,
                                const QSize &targetReferenceSize,
                                const QTransform &layerToDocument,
                                const SelectionEditKind kind,
                                const int channelIndex,
                                QRect *affectedRect)
{
    if (affectedRect) {
        *affectedRect = {};
    }
    if (!resultImage || selectionSnapshot.size.isEmpty()
        || targetReferenceSize.isEmpty() || !selectionSnapshot.active) {
        return false;
    }
    SelectionMask selection(selectionSnapshot.size);
    if (!selection.restoreSnapshot(selectionSnapshot, false)
        || selection.isEmpty()) {
        return false;
    }

    bool invertible = false;
    const QTransform documentToLayer = layerToDocument.inverted(&invertible);
    if (!invertible) {
        return false;
    }
    const QRect layerBounds(QPoint(0, 0), targetReferenceSize);
    const QRect editRect = documentToLayer.mapRect(QRectF(selection.nonZeroBounds()))
                               .toAlignedRect()
                               .adjusted(-2, -2, 2, 2)
                               .intersected(layerBounds);
    if (editRect.isEmpty()) {
        return false;
    }

    int changedLeft = std::numeric_limits<int>::max();
    int changedTop = std::numeric_limits<int>::max();
    int changedRight = -1;
    int changedBottom = -1;
    const auto markChanged = [&](const int x, const int y) {
        changedLeft = std::min(changedLeft, x);
        changedTop = std::min(changedTop, y);
        changedRight = std::max(changedRight, x);
        changedBottom = std::max(changedBottom, y);
    };
    const auto changedRect = [&] {
        return changedRight >= changedLeft && changedBottom >= changedTop
            ? QRect(QPoint(changedLeft, changedTop),
                    QPoint(changedRight, changedBottom))
            : QRect();
    };

    if (kind == SelectionEditKind::Mask) {
        QImage image = materialisedMaskImage(sourceImage, targetReferenceSize);
        if (image.isNull()) {
            return false;
        }
        image.detach();
        for (int y = editRect.top(); y <= editRect.bottom(); ++y) {
            uchar *row = image.scanLine(y);
            for (int x = editRect.left(); x <= editRect.right(); ++x) {
                const double coverage = sampleSelectionCoverage(
                    selection, layerToDocument, QPointF(x + 0.5, y + 0.5));
                if (coverage <= 0.0) {
                    continue;
                }
                const uchar before = row[x];
                const uchar after = static_cast<uchar>(std::clamp(
                    qRound(before * (1.0 - coverage)), 0, 255));
                if (after != before) {
                    row[x] = after;
                    markChanged(x, y);
                }
            }
        }
        *resultImage = compactMaskCoverage(std::move(image));
        if (affectedRect) {
            *affectedRect = changedRect();
        }
        return !changedRect().isEmpty();
    }

    const bool sixteenBit = sourceImage.depth() > 32;
    QImage image = straightLayerPixels(sourceImage,
                                       targetReferenceSize,
                                       sourceImage.colorSpace(),
                                       sixteenBit);
    if (image.isNull()) {
        return false;
    }
    image.detach();
    if (sixteenBit) {
        for (int y = editRect.top(); y <= editRect.bottom(); ++y) {
            auto *row = reinterpret_cast<QRgba64 *>(image.scanLine(y));
            for (int x = editRect.left(); x <= editRect.right(); ++x) {
                const double coverage = sampleSelectionCoverage(
                    selection, layerToDocument, QPointF(x + 0.5, y + 0.5));
                if (coverage <= 0.0) {
                    continue;
                }
                const QRgba64 before = row[x];
                quint16 values[4] {before.red(), before.green(),
                                   before.blue(), before.alpha()};
                if (kind == SelectionEditKind::RasterPixels) {
                    values[3] = static_cast<quint16>(std::clamp(
                        qRound(values[3] * (1.0 - coverage)), 0, 65535));
                } else if (kind == SelectionEditKind::GreyChannel) {
                    values[0] = static_cast<quint16>(std::clamp(
                        qRound(values[0] * (1.0 - coverage)), 0, 65535));
                    values[1] = static_cast<quint16>(std::clamp(
                        qRound(values[1] * (1.0 - coverage)), 0, 65535));
                    values[2] = static_cast<quint16>(std::clamp(
                        qRound(values[2] * (1.0 - coverage)), 0, 65535));
                } else if (channelIndex >= 0 && channelIndex < 4) {
                    values[channelIndex] = static_cast<quint16>(std::clamp(
                        qRound(values[channelIndex] * (1.0 - coverage)),
                        0,
                        65535));
                }
                const QRgba64 after = QRgba64::fromRgba64(
                    values[0], values[1], values[2], values[3]);
                if (after.red() != before.red()
                    || after.green() != before.green()
                    || after.blue() != before.blue()
                    || after.alpha() != before.alpha()) {
                    row[x] = after;
                    markChanged(x, y);
                }
            }
        }
    } else {
        for (int y = editRect.top(); y <= editRect.bottom(); ++y) {
            uchar *row = image.scanLine(y);
            for (int x = editRect.left(); x <= editRect.right(); ++x) {
                const double coverage = sampleSelectionCoverage(
                    selection, layerToDocument, QPointF(x + 0.5, y + 0.5));
                if (coverage <= 0.0) {
                    continue;
                }
                const int offset = x * 4;
                const uchar before[4] {row[offset], row[offset + 1],
                                       row[offset + 2], row[offset + 3]};
                if (kind == SelectionEditKind::RasterPixels) {
                    row[offset + 3] = static_cast<uchar>(std::clamp(
                        qRound(row[offset + 3] * (1.0 - coverage)), 0, 255));
                } else if (kind == SelectionEditKind::GreyChannel) {
                    for (int component = 0; component < 3; ++component) {
                        row[offset + component] = static_cast<uchar>(std::clamp(
                            qRound(row[offset + component] * (1.0 - coverage)),
                            0,
                            255));
                    }
                } else if (channelIndex >= 0 && channelIndex < 4) {
                    row[offset + channelIndex] = static_cast<uchar>(std::clamp(
                        qRound(row[offset + channelIndex] * (1.0 - coverage)),
                        0,
                        255));
                }
                if (row[offset] != before[0] || row[offset + 1] != before[1]
                    || row[offset + 2] != before[2]
                    || row[offset + 3] != before[3]) {
                    markChanged(x, y);
                }
            }
        }
    }
    image.setColorSpace(sourceImage.colorSpace());
    *resultImage = image;
    if (affectedRect) {
        *affectedRect = changedRect();
    }
    return !changedRect().isEmpty();
}

QImage selectionAsLayerMask(const SelectionMask &selection,
                            const QTransform &layerToDocument,
                            const QSize &documentSize)
{
    if (!selection.isActive() || documentSize.isEmpty()) {
        return {};
    }
    if (selection.isFull() && layerToDocument.isIdentity()) {
        QImage full(1, 1, QImage::Format_Grayscale8);
        full.fill(255);
        return full;
    }
    if (selection.isEmpty()) {
        QImage empty(1, 1, QImage::Format_Grayscale8);
        empty.fill(0);
        return empty;
    }
    if (layerToDocument.isIdentity() && selection.size() == documentSize) {
        return compactMaskCoverage(selection.coverageImage());
    }

    QImage coverage(documentSize, QImage::Format_Grayscale8);
    if (coverage.isNull()) {
        return {};
    }
    coverage.fill(0);

    bool invertible = false;
    const QTransform documentToLayer = layerToDocument.inverted(&invertible);
    if (!invertible) {
        return {};
    }
    const QRect localBounds = documentToLayer
        .mapRect(QRectF(selection.nonZeroBounds()))
        .toAlignedRect()
        .adjusted(-2, -2, 2, 2)
        .intersected(QRect(QPoint(0, 0), documentSize));
    if (localBounds.isEmpty()) {
        QImage empty(1, 1, QImage::Format_Grayscale8);
        empty.fill(0);
        return empty;
    }

    int minimumCoverage = localBounds == QRect(QPoint(0, 0), documentSize)
        ? 255 : 0;
    int maximumCoverage = 0;
    for (int y = localBounds.top(); y <= localBounds.bottom(); ++y) {
        uchar *row = coverage.scanLine(y);
        for (int x = localBounds.left(); x <= localBounds.right(); ++x) {
            const int value = std::clamp(
                qRound(sampleSelectionCoverage(selection,
                                               layerToDocument,
                                               QPointF(x + 0.5, y + 0.5))
                       * 255.0),
                0,
                255);
            row[x] = static_cast<uchar>(value);
            minimumCoverage = std::min(minimumCoverage, value);
            maximumCoverage = std::max(maximumCoverage, value);
        }
    }
    if (minimumCoverage == maximumCoverage) {
        QImage compact(1, 1, QImage::Format_Grayscale8);
        compact.fill(minimumCoverage);
        return compact;
    }
    return coverage;
}

} // namespace vfx
