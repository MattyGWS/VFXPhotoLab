#include "CropOperations.h"

#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vfx {
namespace {

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

bool cancelled(const std::atomic_bool *cancelRequested)
{
    return cancelRequested
        && cancelRequested->load(std::memory_order_relaxed);
}

QSize effectiveReference(const QSize &stored, const QSize &fallback)
{
    return stored.isValid() && !stored.isEmpty() ? stored : fallback;
}

QTransform pixelToDocumentTransform(const QSize &imageSize,
                                    const QSize &referenceSize,
                                    const QPointF &referenceOrigin,
                                    const QTransform &world)
{
    if (imageSize.isEmpty() || referenceSize.isEmpty()) {
        return world;
    }
    return QTransform::fromScale(
               referenceSize.width() / static_cast<double>(imageSize.width()),
               referenceSize.height() / static_cast<double>(imageSize.height()))
        * QTransform::fromTranslate(referenceOrigin.x(), referenceOrigin.y())
        * world;
}

QImage alphaSafeTransformedRaster(const QImage &source,
                                  const QSize &referenceSize,
                                  const QPointF &referenceOrigin,
                                  const QTransform &world,
                                  const QSize &outputSize,
                                  const std::atomic_bool *cancelRequested)
{
    if (source.isNull() || outputSize.isEmpty()) {
        return {};
    }
    const bool sixteenBit = source.depth() > 32;
    const QImage::Format straightFormat = sixteenBit
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    QImage straight = source.convertToFormat(straightFormat);
    straight.setColorSpace(source.colorSpace());

    // Fast exact path for the overwhelmingly common axis-aligned crop. It
    // copies straight RGBA bytes, including RGB beneath zero Alpha.
    const QTransform mapping = pixelToDocumentTransform(
        straight.size(), referenceSize, referenceOrigin, world);
    const bool exactTranslation = mapping.type() <= QTransform::TxTranslate
        && qFuzzyCompare(mapping.m11(), 1.0)
        && qFuzzyCompare(mapping.m22(), 1.0)
        && qFuzzyIsNull(mapping.m12()) && qFuzzyIsNull(mapping.m21())
        && std::abs(mapping.dx() - std::round(mapping.dx())) < 1.0e-6
        && std::abs(mapping.dy() - std::round(mapping.dy())) < 1.0e-6;
    QImage output(outputSize, straightFormat);
    output.fill(Qt::transparent);
    output.setColorSpace(source.colorSpace());
    if (exactTranslation) {
        const QPoint destinationOrigin(qRound(mapping.dx()), qRound(mapping.dy()));
        const QRect destinationRect(destinationOrigin, straight.size());
        const QRect clipped = destinationRect.intersected(output.rect());
        if (!clipped.isEmpty()) {
            const QRect sourceRect(clipped.topLeft() - destinationOrigin,
                                   clipped.size());
            const int bytesPerPixel = sixteenBit ? 8 : 4;
            for (int y = 0; y < clipped.height(); ++y) {
                if (cancelled(cancelRequested)) {
                    return {};
                }
                std::memcpy(output.scanLine(clipped.y() + y)
                                + static_cast<qsizetype>(clipped.x()) * bytesPerPixel,
                            straight.constScanLine(sourceRect.y() + y)
                                + static_cast<qsizetype>(sourceRect.x()) * bytesPerPixel,
                            static_cast<size_t>(clipped.width() * bytesPerPixel));
            }
        }
        return output;
    }

    // Transform straight RGB and Alpha independently. Making the RGB source
    // opaque prevents QPainter's premultiplication from erasing hidden colour
    // where the source Alpha is zero.
    QImage rgb = straight;
    QImage alpha(source.size(), straightFormat);
    if (sixteenBit) {
        for (int y = 0; y < straight.height(); ++y) {
            if (cancelled(cancelRequested)) {
                return {};
            }
            auto *rgbRow = reinterpret_cast<QRgba64 *>(rgb.scanLine(y));
            auto *alphaRow = reinterpret_cast<QRgba64 *>(alpha.scanLine(y));
            for (int x = 0; x < straight.width(); ++x) {
                const QRgba64 pixel = rgbRow[x];
                alphaRow[x] = QRgba64::fromRgba64(
                    pixel.alpha(), pixel.alpha(), pixel.alpha(), 65535);
                rgbRow[x] = QRgba64::fromRgba64(
                    pixel.red(), pixel.green(), pixel.blue(), 65535);
            }
        }
    } else {
        for (int y = 0; y < straight.height(); ++y) {
            if (cancelled(cancelRequested)) {
                return {};
            }
            uchar *rgbRow = rgb.scanLine(y);
            uchar *alphaRow = alpha.scanLine(y);
            for (int x = 0; x < straight.width(); ++x) {
                const uchar value = rgbRow[x * 4 + 3];
                alphaRow[x * 4 + 0] = value;
                alphaRow[x * 4 + 1] = value;
                alphaRow[x * 4 + 2] = value;
                alphaRow[x * 4 + 3] = 255;
                rgbRow[x * 4 + 3] = 255;
            }
        }
    }

    QImage transformedRgb(outputSize, straightFormat);
    transformedRgb.fill(Qt::transparent);
    QImage transformedAlpha(outputSize, alpha.format());
    transformedAlpha.fill(Qt::black);
    const auto drawInCancelableStrips = [&](QImage *destination,
                                            const QImage &input) {
        QPainter painter(destination);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        constexpr int StripHeight = 256;
        for (int y = 0; y < outputSize.height(); y += StripHeight) {
            if (cancelled(cancelRequested)) {
                painter.end();
                return false;
            }
            painter.save();
            painter.resetTransform();
            painter.setClipRect(QRect(0, y, outputSize.width(),
                                      std::min(StripHeight, outputSize.height() - y)),
                                Qt::ReplaceClip);
            painter.setTransform(mapping);
            painter.drawImage(QPointF(), input);
            painter.restore();
        }
        painter.end();
        return true;
    };
    if (!drawInCancelableStrips(&transformedRgb, rgb)
        || !drawInCancelableStrips(&transformedAlpha, alpha)) {
        return {};
    }
    if (sixteenBit) {
        for (int y = 0; y < output.height(); ++y) {
            if (cancelled(cancelRequested)) {
                return {};
            }
            auto *destination = reinterpret_cast<QRgba64 *>(transformedRgb.scanLine(y));
            const auto *alphaRow = reinterpret_cast<const QRgba64 *>(
                transformedAlpha.constScanLine(y));
            for (int x = 0; x < output.width(); ++x) {
                const QRgba64 pixel = destination[x];
                destination[x] = QRgba64::fromRgba64(
                    pixel.red(), pixel.green(), pixel.blue(), alphaRow[x].red());
            }
        }
    } else {
        for (int y = 0; y < output.height(); ++y) {
            if (cancelled(cancelRequested)) {
                return {};
            }
            uchar *destination = transformedRgb.scanLine(y);
            const uchar *alphaRow = transformedAlpha.constScanLine(y);
            for (int x = 0; x < output.width(); ++x) {
                destination[x * 4 + 3] = alphaRow[x * 4];
            }
        }
    }
    transformedRgb.setColorSpace(source.colorSpace());
    return transformedRgb;
}

QImage transformedMask(const QImage &source,
                       const QSize &referenceSize,
                       const QPointF &referenceOrigin,
                       const QTransform &world,
                       const QSize &outputSize,
                       const std::atomic_bool *cancelRequested)
{
    if (source.isNull() || outputSize.isEmpty()) {
        return {};
    }
    const QImage mask = source.convertToFormat(QImage::Format_Grayscale8);
    if (mask.size() == QSize(1, 1) && mask.constScanLine(0)[0] == 255) {
        // White is also the expansion fill, so it remains compact regardless
        // of how the reference rectangle grows.
        return mask;
    }
    QImage output(outputSize, QImage::Format_Grayscale8);
    output.fill(255);
    QPainter painter(&output);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QTransform mapping = pixelToDocumentTransform(mask.size(), referenceSize,
                                                               referenceOrigin, world);
    constexpr int StripHeight = 256;
    for (int y = 0; y < outputSize.height(); y += StripHeight) {
        if (cancelled(cancelRequested)) {
            painter.end();
            return {};
        }
        painter.save();
        painter.resetTransform();
        painter.setClipRect(QRect(0, y, outputSize.width(),
                                  std::min(StripHeight, outputSize.height() - y)),
                            Qt::ReplaceClip);
        painter.setTransform(mapping);
        painter.drawImage(QPointF(), mask);
        painter.restore();
    }
    painter.end();
    return output;
}

void initialiseReferenceExtents(QVector<LayerNode> &layers,
                                const QSize &oldDocumentSize)
{
    for (LayerNode &layer : layers) {
        if ((layer.type == LayerType::Raster || layer.type == LayerType::BaseImage)
            && layer.rasterReferenceSize.isEmpty()) {
            layer.rasterReferenceSize = oldDocumentSize;
        }
        if (!layer.maskImage.isNull() && layer.maskReferenceSize.isEmpty()) {
            layer.maskReferenceSize = oldDocumentSize;
        }
        initialiseReferenceExtents(layer.children, oldDocumentSize);
    }
}

void appendDocumentMap(QVector<LayerNode> &layers,
                       const QTransform &documentMap,
                       const QSize &oldDocumentSize)
{
    initialiseReferenceExtents(layers, oldDocumentSize);
    for (LayerNode &layer : layers) {
        layer.transform = layer.transform * documentMap;
        ++layer.revision;
    }
}

QRect alignedUnionReferenceRect(const QSize &referenceSize,
                                    const QPointF &referenceOrigin,
                                    const QRectF &requiredLocalBounds)
{
    const QRectF existing(referenceOrigin, QSizeF(referenceSize));
    const QRectF united = existing.united(requiredLocalBounds).normalized();
    const int left = static_cast<int>(std::floor(united.left()));
    const int top = static_cast<int>(std::floor(united.top()));
    const int right = static_cast<int>(std::ceil(united.right()));
    const int bottom = static_cast<int>(std::ceil(united.bottom()));
    return QRect(left, top, std::max(1, right - left), std::max(1, bottom - top));
}

bool expandTreeForCanvas(QVector<LayerNode> &layers,
                         const QImage &source,
                         const QSize &oldDocumentSize,
                         const QSize &newDocumentSize,
                         const QTransform &parentWorld,
                         const std::atomic_bool *cancelRequested)
{
    const QRectF newCanvas(QPointF(0.0, 0.0), QSizeF(newDocumentSize));
    for (LayerNode &layer : layers) {
        if (cancelled(cancelRequested)) {
            return false;
        }
        const QTransform world = layer.transform * parentWorld;
        bool invertible = false;
        const QTransform documentToLocal = world.inverted(&invertible);
        if (!invertible) {
            return false;
        }
        const QRectF requiredLocal = documentToLocal
            .map(QPolygonF(newCanvas)).boundingRect().normalized();

        if (layer.type == LayerType::Raster || layer.type == LayerType::BaseImage) {
            const QSize oldReference = effectiveReference(layer.rasterReferenceSize,
                                                          oldDocumentSize);
            const QRect expanded = alignedUnionReferenceRect(oldReference,
                                                             layer.rasterReferenceOrigin,
                                                             requiredLocal);
            if (expanded.width() > 32768 || expanded.height() > 32768) {
                return false;
            }
            if (expanded.topLeft() != layer.rasterReferenceOrigin.toPoint()
                || expanded.size() != oldReference) {
                if (!layer.rasterImage.isNull()) {
                    layer.rasterImage = alphaSafeTransformedRaster(
                        layer.rasterImage,
                        oldReference,
                        layer.rasterReferenceOrigin,
                        QTransform::fromTranslate(-expanded.x(), -expanded.y()),
                        expanded.size(),
                        cancelRequested);
                    if (layer.rasterImage.isNull()) {
                        return false;
                    }
                }
                layer.rasterReferenceSize = expanded.size();
                layer.rasterReferenceOrigin = expanded.topLeft();
                ++layer.revision;
            }
        }
        if (!layer.maskImage.isNull()) {
            const QSize oldReference = effectiveReference(layer.maskReferenceSize,
                                                          oldDocumentSize);
            const QRect expanded = alignedUnionReferenceRect(oldReference,
                                                             layer.maskReferenceOrigin,
                                                             requiredLocal);
            if (expanded.width() > 32768 || expanded.height() > 32768) {
                return false;
            }
            if (expanded.topLeft() != layer.maskReferenceOrigin.toPoint()
                || expanded.size() != oldReference) {
                layer.maskImage = transformedMask(
                    layer.maskImage,
                    oldReference,
                    layer.maskReferenceOrigin,
                    QTransform::fromTranslate(-expanded.x(), -expanded.y()),
                    expanded.size(),
                    cancelRequested);
                if (layer.maskImage.isNull()) {
                    return false;
                }
                layer.maskReferenceSize = expanded.size();
                layer.maskReferenceOrigin = expanded.topLeft();
                ++layer.revision;
            }
        }
        if (!expandTreeForCanvas(layer.children,
                                 source,
                                 oldDocumentSize,
                                 newDocumentSize,
                                 world,
                                 cancelRequested)) {
            return false;
        }
    }
    Q_UNUSED(source);
    return true;
}

bool bakeTree(QVector<LayerNode> &layers,
              const QSize &oldDocumentSize,
              const QSize &newDocumentSize,
              const QTransform &parentWorld,
              const std::atomic_bool *cancelRequested)
{
    for (LayerNode &layer : layers) {
        if (cancelled(cancelRequested)) {
            return false;
        }
        const QTransform world = layer.transform * parentWorld;
        if (layer.type == LayerType::Raster || layer.type == LayerType::BaseImage) {
            if (!layer.rasterImage.isNull()) {
                layer.rasterImage = alphaSafeTransformedRaster(
                    layer.rasterImage,
                    effectiveReference(layer.rasterReferenceSize, oldDocumentSize),
                    layer.rasterReferenceOrigin,
                    world,
                    newDocumentSize,
                    cancelRequested);
                if (layer.rasterImage.isNull()) {
                    return false;
                }
            }
            // Empty raster layers still need to adopt the destructively cropped
            // document extent so a later brush stroke addresses the new canvas,
            // rather than resurrecting an obsolete off-canvas reference rectangle.
            layer.rasterReferenceSize = newDocumentSize;
            layer.rasterReferenceOrigin = {};
        }
        if (!layer.maskImage.isNull()) {
            layer.maskImage = transformedMask(
                layer.maskImage,
                effectiveReference(layer.maskReferenceSize, oldDocumentSize),
                layer.maskReferenceOrigin,
                world,
                newDocumentSize,
                cancelRequested);
            if (layer.maskImage.isNull()) {
                return false;
            }
            layer.maskReferenceSize = newDocumentSize;
            layer.maskReferenceOrigin = {};
        }
        if (!bakeTree(layer.children,
                      oldDocumentSize,
                      newDocumentSize,
                      world,
                      cancelRequested)) {
            return false;
        }
        layer.transform.reset();
        ++layer.revision;
    }
    return true;
}

SelectionMask::Snapshot transformedSelection(
    const SelectionMask &source,
    const QSize &outputSize,
    const QTransform &documentMap,
    const std::atomic_bool *cancelRequested)
{
    SelectionMask destination(outputSize);
    if (!source.isActive()) {
        destination.deactivate();
        return destination.snapshot();
    }
    if (source.isEmpty()) {
        destination.selectNone();
        return destination.snapshot();
    }
    const QRect sourceBounds = source.nonZeroBounds().isEmpty()
        ? QRect(QPoint(0, 0), source.size()) : source.nonZeroBounds();
    const QImage coverage = source.coverageImage(sourceBounds);
    QImage transformed(outputSize, QImage::Format_Grayscale8);
    if (transformed.isNull()) {
        return {};
    }
    transformed.fill(0);
    QPainter painter(&transformed);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                          documentMap.type() > QTransform::TxTranslate);
    const QTransform mapping = QTransform::fromTranslate(
        sourceBounds.x(), sourceBounds.y()) * documentMap;
    constexpr int StripHeight = 256;
    for (int y = 0; y < outputSize.height(); y += StripHeight) {
        if (cancelled(cancelRequested)) {
            painter.end();
            return {};
        }
        painter.save();
        painter.resetTransform();
        painter.setClipRect(QRect(0, y, outputSize.width(),
                                  std::min(StripHeight, outputSize.height() - y)),
                            Qt::ReplaceClip);
        painter.setTransform(mapping);
        painter.drawImage(QPointF(), coverage);
        painter.restore();
    }
    painter.end();
    destination.selectNone();
    // setCoverageImage() reports whether the sparse selection state changed,
    // not whether the write succeeded. A crop can legitimately move an active
    // selection completely outside the new canvas, leaving the already-empty
    // destination unchanged. Treat that as a valid empty active selection
    // instead of an allocation failure.
    destination.setCoverageImage(transformed.rect(), transformed);
    return destination.snapshot();
}

} // namespace

bool buildCropResult(const PhotoDocument &document,
                     const CropRequest &request,
                     CropResult *result,
                     const std::atomic_bool *cancelRequested,
                     QString *errorMessage)
{
    if (!result || !document.hasImage()) {
        setError(errorMessage, QStringLiteral("There is no document to crop."));
        return false;
    }
    const QRect cropRect = request.documentRect.normalized();
    if (cropRect.width() < 1 || cropRect.height() < 1
        || cropRect.width() > 32768 || cropRect.height() > 32768) {
        setError(errorMessage,
                 QStringLiteral("Crop dimensions must be between 1 and 32768 pixels."));
        return false;
    }
    const QSize oldSize = document.sourceImage().size();
    const QSize newSize = cropRect.size();
    if (oldSize.isEmpty()) {
        setError(errorMessage, QStringLiteral("The document canvas is invalid."));
        return false;
    }

    QTransform rotation;
    const QPointF centre(oldSize.width() * 0.5, oldSize.height() * 0.5);
    rotation.translate(centre.x(), centre.y());
    rotation.rotate(request.straightenAngle);
    rotation.translate(-centre.x(), -centre.y());
    const QTransform documentMap = rotation
        * QTransform::fromTranslate(-cropRect.x(), -cropRect.y());

    CropResult prepared;
    prepared.layers = document.layers();
    appendDocumentMap(prepared.layers, documentMap, oldSize);
    if (!request.deleteCroppedPixels
        && !expandTreeForCanvas(prepared.layers,
                                document.sourceImage(),
                                oldSize,
                                newSize,
                                QTransform(),
                                cancelRequested)) {
        setError(errorMessage,
                 cancelled(cancelRequested)
                     ? QStringLiteral("Crop cancelled.")
                     : QStringLiteral("The expanded crop exceeds safe editable layer bounds."));
        return false;
    }
    if (request.deleteCroppedPixels
        && !bakeTree(prepared.layers,
                     oldSize,
                     newSize,
                     QTransform(),
                     cancelRequested)) {
        setError(errorMessage,
                 cancelled(cancelRequested)
                     ? QStringLiteral("Crop cancelled.")
                     : QStringLiteral("Could not allocate the destructively cropped layer pixels."));
        return false;
    }
    if (cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Crop cancelled."));
        return false;
    }

    const bool sixteenBit = document.sourceImage().depth() > 32;
    prepared.canvasImage = QImage(newSize, sixteenBit
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (prepared.canvasImage.isNull()) {
        setError(errorMessage, QStringLiteral("Could not allocate the cropped canvas."));
        return false;
    }
    prepared.canvasImage.fill(Qt::transparent);
    prepared.canvasImage.setColorSpace(document.sourceImage().colorSpace());
    prepared.canvasImage.setDotsPerMeterX(document.sourceImage().dotsPerMeterX());
    prepared.canvasImage.setDotsPerMeterY(document.sourceImage().dotsPerMeterY());
    prepared.selection = transformedSelection(
        document.selectionMask(), newSize, documentMap, cancelRequested);
    if (cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Crop cancelled."));
        return false;
    }
    if (prepared.selection.size != newSize) {
        setError(errorMessage,
                 QStringLiteral("Could not allocate the transformed crop selection."));
        return false;
    }

    // The current guide model stores only horizontal and vertical guides. An
    // arbitrary straighten would turn them into angled guides, so retain exact
    // guides for an ordinary crop and discard unrepresentable ones.
    if (std::abs(request.straightenAngle) < 1.0e-6) {
        for (const double guide : document.horizontalGuides()) {
            const double shifted = guide - cropRect.y();
            if (shifted >= 0.0 && shifted <= newSize.height()) {
                prepared.horizontalGuides.push_back(shifted);
            }
        }
        for (const double guide : document.verticalGuides()) {
            const double shifted = guide - cropRect.x();
            if (shifted >= 0.0 && shifted <= newSize.width()) {
                prepared.verticalGuides.push_back(shifted);
            }
        }
    }

    *result = std::move(prepared);
    return true;
}

} // namespace vfx
