#include "SelectionTransformOperations.h"

#include <QColorSpace>
#include <QPoint>
#include <QRect>
#include <QRgba64>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace vfx {
namespace {

SelectionMask restoredSelection(const SelectionMask::Snapshot &snapshot)
{
    SelectionMask selection(snapshot.size);
    if (!snapshot.size.isEmpty()) {
        selection.restoreSnapshot(snapshot, false);
    }
    return selection;
}

QImage materialisedMask(const QImage &source, const QSize &extent)
{
    if (extent.isEmpty()) {
        return {};
    }
    if (source.isNull()) {
        QImage image(extent, QImage::Format_Grayscale8);
        image.fill(0);
        return image;
    }
    if (source.size() == QSize(1, 1)) {
        QImage image(extent, QImage::Format_Grayscale8);
        image.fill(qGray(source.pixel(0, 0)));
        return image;
    }
    return source.size() == extent
        ? source.convertToFormat(QImage::Format_Grayscale8)
        : source.scaled(extent, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
              .convertToFormat(QImage::Format_Grayscale8);
}

QImage straightPixels(const QImage &source, const QSize &extent)
{
    const bool sixteenBit = source.depth() > 32;
    const QImage::Format format = sixteenBit
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    if (extent.isEmpty()) {
        return {};
    }
    if (source.isNull()) {
        QImage image(extent, format);
        image.fill(Qt::transparent);
        return image;
    }
    QImage image = source.size() == extent
        ? source.convertToFormat(format)
        : source.scaled(extent, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
              .convertToFormat(format);
    image.setColorSpace(source.colorSpace());
    return image;
}

double bilinearSelectionCoverage(const SelectionMask &selection,
                                 const QPointF &documentPoint)
{
    const double sx = documentPoint.x() - 0.5;
    const double sy = documentPoint.y() - 0.5;
    const int x0 = static_cast<int>(std::floor(sx));
    const int y0 = static_cast<int>(std::floor(sy));
    const double fx = sx - x0;
    const double fy = sy - y0;
    const double c00 = selection.coverageAt(x0, y0);
    const double c10 = selection.coverageAt(x0 + 1, y0);
    const double c01 = selection.coverageAt(x0, y0 + 1);
    const double c11 = selection.coverageAt(x0 + 1, y0 + 1);
    const double top = c00 + (c10 - c00) * fx;
    const double bottom = c01 + (c11 - c01) * fx;
    return std::clamp(top + (bottom - top) * fy, 0.0, 255.0);
}

} // namespace

QImage selectedOnlyImageThroughSelection(
    const QImage &sourceImage,
    const QSize &targetExtent,
    const SelectionMask::Snapshot &selectionSnapshot,
    const QTransform &targetToDocument,
    const SelectionEditKind kind,
    const int channelIndex)
{
    if (targetExtent.isEmpty() || !selectionSnapshot.active) {
        return {};
    }
    const SelectionMask selection = restoredSelection(selectionSnapshot);
    if (selection.isEmpty()) {
        return {};
    }
    bool invertible = false;
    const QTransform documentToTarget = targetToDocument.inverted(&invertible);
    if (!invertible) {
        return {};
    }
    const QRect candidate = documentToTarget
        .mapRect(QRectF(selection.nonZeroBounds()))
        .toAlignedRect().adjusted(-2, -2, 2, 2)
        .intersected(QRect(QPoint(0, 0), targetExtent));
    if (candidate.isEmpty()) {
        return {};
    }

    if (kind == SelectionEditKind::Mask) {
        const QImage source = materialisedMask(sourceImage, targetExtent);
        if (source.isNull()) {
            return {};
        }
        QImage output(targetExtent, QImage::Format_Grayscale8);
        output.fill(0);
        for (int y = candidate.top(); y <= candidate.bottom(); ++y) {
            const uchar *src = source.constScanLine(y);
            uchar *dst = output.scanLine(y);
            for (int x = candidate.left(); x <= candidate.right(); ++x) {
                const double coverage = sampleSelectionCoverage(
                    selection, targetToDocument, QPointF(x + 0.5, y + 0.5));
                dst[x] = static_cast<uchar>(std::clamp(
                    qRound(src[x] * coverage), 0, 255));
            }
        }
        return output;
    }

    const QImage source = straightPixels(sourceImage, targetExtent);
    if (source.isNull()) {
        return {};
    }
    const bool sixteenBit = source.depth() > 32;
    QImage output(targetExtent, sixteenBit
                                ? QImage::Format_RGBA64
                                : QImage::Format_RGBA8888);
    output.fill(Qt::transparent);
    output.setColorSpace(source.colorSpace());

    if (sixteenBit) {
        for (int y = candidate.top(); y <= candidate.bottom(); ++y) {
            const auto *src = reinterpret_cast<const QRgba64 *>(source.constScanLine(y));
            auto *dst = reinterpret_cast<QRgba64 *>(output.scanLine(y));
            for (int x = candidate.left(); x <= candidate.right(); ++x) {
                const double coverage = sampleSelectionCoverage(
                    selection, targetToDocument, QPointF(x + 0.5, y + 0.5));
                const QRgba64 pixel = src[x];
                if (kind == SelectionEditKind::RasterPixels) {
                    dst[x] = QRgba64::fromRgba64(
                        pixel.red(), pixel.green(), pixel.blue(),
                        static_cast<quint16>(std::clamp(
                            qRound(pixel.alpha() * coverage), 0, 65535)));
                } else {
                    quint16 value = 0;
                    if (kind == SelectionEditKind::GreyChannel) {
                        value = static_cast<quint16>((pixel.red() * 299ull
                                                     + pixel.green() * 587ull
                                                     + pixel.blue() * 114ull + 500ull)
                                                    / 1000ull);
                    } else {
                        switch (channelIndex) {
                        case 0: value = pixel.red(); break;
                        case 1: value = pixel.green(); break;
                        case 2: value = pixel.blue(); break;
                        case 3: value = pixel.alpha(); break;
                        default: break;
                        }
                    }
                    const quint16 alpha = static_cast<quint16>(std::clamp(
                        qRound(65535.0 * coverage), 0, 65535));
                    dst[x] = QRgba64::fromRgba64(value, value, value, alpha);
                }
            }
        }
    } else {
        for (int y = candidate.top(); y <= candidate.bottom(); ++y) {
            const uchar *src = source.constScanLine(y);
            uchar *dst = output.scanLine(y);
            for (int x = candidate.left(); x <= candidate.right(); ++x) {
                const double coverage = sampleSelectionCoverage(
                    selection, targetToDocument, QPointF(x + 0.5, y + 0.5));
                const uchar *pixel = src + x * 4;
                uchar *result = dst + x * 4;
                if (kind == SelectionEditKind::RasterPixels) {
                    result[0] = pixel[0];
                    result[1] = pixel[1];
                    result[2] = pixel[2];
                    result[3] = static_cast<uchar>(std::clamp(
                        qRound(pixel[3] * coverage), 0, 255));
                } else {
                    int value = 0;
                    if (kind == SelectionEditKind::GreyChannel) {
                        value = (pixel[0] * 299 + pixel[1] * 587
                                 + pixel[2] * 114 + 500) / 1000;
                    } else if (channelIndex >= 0 && channelIndex < 4) {
                        value = pixel[channelIndex];
                    }
                    result[0] = static_cast<uchar>(value);
                    result[1] = static_cast<uchar>(value);
                    result[2] = static_cast<uchar>(value);
                    result[3] = static_cast<uchar>(std::clamp(
                        qRound(255.0 * coverage), 0, 255));
                }
            }
        }
    }
    return output;
}

SelectionMask::Snapshot transformedSelectionSnapshot(
    const SelectionMask::Snapshot &sourceSnapshot,
    const QTransform &documentTransform,
    const QSize &documentSize)
{
    SelectionMask output(documentSize);
    output.selectNone();
    if (!sourceSnapshot.active || documentSize.isEmpty()) {
        if (!sourceSnapshot.active) {
            output.deactivate();
        }
        return output.snapshot();
    }

    const SelectionMask source = restoredSelection(sourceSnapshot);
    if (source.isEmpty()) {
        return output.snapshot();
    }
    bool invertible = false;
    const QTransform inverse = documentTransform.inverted(&invertible);
    if (!invertible) {
        return output.snapshot();
    }

    const QRect documentRect(QPoint(0, 0), documentSize);
    const QRect transformedBounds = documentTransform
        .mapRect(QRectF(source.nonZeroBounds()))
        .toAlignedRect().adjusted(-2, -2, 2, 2)
        .intersected(documentRect);
    if (transformedBounds.isEmpty()) {
        return output.snapshot();
    }

    QImage coverage(transformedBounds.size(), QImage::Format_Grayscale8);
    coverage.fill(0);
    for (int y = 0; y < coverage.height(); ++y) {
        uchar *row = coverage.scanLine(y);
        const double documentY = transformedBounds.y() + y + 0.5;
        for (int x = 0; x < coverage.width(); ++x) {
            const QPointF sourceDocumentPoint = inverse.map(
                QPointF(transformedBounds.x() + x + 0.5, documentY));
            row[x] = static_cast<uchar>(std::clamp(
                qRound(bilinearSelectionCoverage(source, sourceDocumentPoint)),
                0,
                255));
        }
    }
    output.setCoverageImage(transformedBounds, coverage);
    return output.snapshot();
}

bool transformIsEffectivelyIdentity(const QTransform &transform,
                                    const double epsilon)
{
    const QTransform identity;
    const double values[] = {
        transform.m11() - identity.m11(),
        transform.m12() - identity.m12(),
        transform.m13() - identity.m13(),
        transform.m21() - identity.m21(),
        transform.m22() - identity.m22(),
        transform.m23() - identity.m23(),
        transform.m31() - identity.m31(),
        transform.m32() - identity.m32(),
        transform.m33() - identity.m33(),
    };
    return std::all_of(std::begin(values), std::end(values), [epsilon](double value) {
        return std::abs(value) <= epsilon;
    });
}

} // namespace vfx
