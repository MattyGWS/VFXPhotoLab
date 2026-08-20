#include "DocumentTransformOperations.h"


#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace vfx {
namespace {

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

QSize destinationSize(const QSize &source,
                      const OrthogonalDocumentTransform operation)
{
    if (operation == OrthogonalDocumentTransform::Rotate90Clockwise
        || operation == OrthogonalDocumentTransform::Rotate90CounterClockwise) {
        return QSize(source.height(), source.width());
    }
    return source;
}

QPoint destinationPoint(const QPoint &source,
                        const QSize &sourceSize,
                        const OrthogonalDocumentTransform operation)
{
    switch (operation) {
    case OrthogonalDocumentTransform::FlipHorizontal:
        return QPoint(sourceSize.width() - 1 - source.x(), source.y());
    case OrthogonalDocumentTransform::FlipVertical:
        return QPoint(source.x(), sourceSize.height() - 1 - source.y());
    case OrthogonalDocumentTransform::Rotate90Clockwise:
        return QPoint(sourceSize.height() - 1 - source.y(), source.x());
    case OrthogonalDocumentTransform::Rotate90CounterClockwise:
        return QPoint(source.y(), sourceSize.width() - 1 - source.x());
    case OrthogonalDocumentTransform::Rotate180:
        return QPoint(sourceSize.width() - 1 - source.x(),
                      sourceSize.height() - 1 - source.y());
    }
    return source;
}

QImage::Format exactCopyFormat(const QImage &source)
{
    switch (source.format()) {
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGB32:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
    case QImage::Format_Grayscale8:
    case QImage::Format_Grayscale16:
        return source.format();
    default:
        return source.depth() > 32 ? QImage::Format_RGBA64
                                   : QImage::Format_RGBA8888;
    }
}

void transformRootLayers(QVector<LayerNode> &layers,
                         const QTransform &documentTransform)
{
    for (LayerNode &layer : layers) {
        // Layer transforms use the same row-vector composition convention as
        // the Transform tool: local * parent/document.
        layer.transform = layer.transform * documentTransform;
        ++layer.revision;
    }
}

QVector<double> sortedUniqueGuides(QVector<double> values,
                                   const double maximum)
{
    for (double &value : values) {
        value = std::clamp(value, 0.0, maximum);
    }
    std::sort(values.begin(), values.end());
    QVector<double> unique;
    unique.reserve(values.size());
    for (const double value : std::as_const(values)) {
        if (unique.isEmpty() || std::abs(unique.constLast() - value) > 1.0e-6) {
            unique.push_back(value);
        }
    }
    return unique;
}

QRect destinationRect(const QRect &sourceRect,
                      const QSize &sourceSize,
                      const OrthogonalDocumentTransform operation)
{
    switch (operation) {
    case OrthogonalDocumentTransform::FlipHorizontal:
        return QRect(sourceSize.width() - sourceRect.x() - sourceRect.width(),
                     sourceRect.y(), sourceRect.width(), sourceRect.height());
    case OrthogonalDocumentTransform::FlipVertical:
        return QRect(sourceRect.x(),
                     sourceSize.height() - sourceRect.y() - sourceRect.height(),
                     sourceRect.width(), sourceRect.height());
    case OrthogonalDocumentTransform::Rotate90Clockwise:
        return QRect(sourceSize.height() - sourceRect.y() - sourceRect.height(),
                     sourceRect.x(), sourceRect.height(), sourceRect.width());
    case OrthogonalDocumentTransform::Rotate90CounterClockwise:
        return QRect(sourceRect.y(),
                     sourceSize.width() - sourceRect.x() - sourceRect.width(),
                     sourceRect.height(), sourceRect.width());
    case OrthogonalDocumentTransform::Rotate180:
        return QRect(sourceSize.width() - sourceRect.x() - sourceRect.width(),
                     sourceSize.height() - sourceRect.y() - sourceRect.height(),
                     sourceRect.width(), sourceRect.height());
    }
    return sourceRect;
}

SelectionMask::Snapshot transformSelection(
    const SelectionMask &selection,
    const OrthogonalDocumentTransform operation,
    const QSize &destination)
{
    SelectionMask transformed(destination);
    if (!selection.isActive()) {
        transformed.deactivate();
        return transformed.snapshot();
    }

    // Keep the selection sparse. Select All and active-empty remain allocation
    // free, while only explicit 256x256 tiles are copied and reoriented. This
    // avoids constructing a document-sized coverage image for large photos.
    if (selection.implicitCoverage() == 255) {
        transformed.selectAll();
    } else {
        transformed.selectNone();
    }
    const QSize sourceSize = selection.size();
    for (const QPoint &tileIndex : selection.explicitTileIndices()) {
        const QPoint sourceTopLeft(tileIndex.x() * SelectionMask::TileSize,
                                   tileIndex.y() * SelectionMask::TileSize);
        const QRect sourceRect(sourceTopLeft, selection.tilePixelSize(tileIndex));
        const QImage coverage = selection.coverageImage(sourceRect)
            .convertToFormat(QImage::Format_Grayscale8);
        if (coverage.isNull()) {
            SelectionMask invalid;
            return invalid.snapshot();
        }
        const QImage rotated = transformImageOrthogonally(coverage, operation)
            .convertToFormat(QImage::Format_Grayscale8);
        const QRect outputRect = destinationRect(sourceRect, sourceSize, operation);
        if (rotated.isNull() || rotated.size() != outputRect.size()
            || !transformed.setCoverageImage(outputRect, rotated)) {
            // Failure is handled atomically by replaceStructuralState, but an
            // invalid partial snapshot must never be published.
            SelectionMask invalid;
            return invalid.snapshot();
        }
    }
    return transformed.snapshot();
}

} // namespace

QTransform documentTransformMatrix(const OrthogonalDocumentTransform operation,
                                   const QSize &sourceSize)
{
    const double width = sourceSize.width();
    const double height = sourceSize.height();
    switch (operation) {
    case OrthogonalDocumentTransform::FlipHorizontal:
        return QTransform(-1.0, 0.0, 0.0, 1.0, width, 0.0);
    case OrthogonalDocumentTransform::FlipVertical:
        return QTransform(1.0, 0.0, 0.0, -1.0, 0.0, height);
    case OrthogonalDocumentTransform::Rotate90Clockwise:
        return QTransform(0.0, 1.0, -1.0, 0.0, height, 0.0);
    case OrthogonalDocumentTransform::Rotate90CounterClockwise:
        return QTransform(0.0, -1.0, 1.0, 0.0, 0.0, width);
    case OrthogonalDocumentTransform::Rotate180:
        return QTransform(-1.0, 0.0, 0.0, -1.0, width, height);
    }
    return {};
}

QImage transformImageOrthogonally(const QImage &source,
                                  const OrthogonalDocumentTransform operation)
{
    if (source.isNull() || source.size().isEmpty()) {
        return {};
    }
    QImage prepared = source.convertToFormat(exactCopyFormat(source));
    if (prepared.isNull() || prepared.depth() % 8 != 0) {
        return {};
    }
    QImage output(destinationSize(prepared.size(), operation), prepared.format());
    if (output.isNull()) {
        return {};
    }
    output.fill(0);
    output.setColorSpace(prepared.colorSpace());
    output.setDevicePixelRatio(prepared.devicePixelRatio());
    const bool quarterTurn = operation == OrthogonalDocumentTransform::Rotate90Clockwise
        || operation == OrthogonalDocumentTransform::Rotate90CounterClockwise;
    output.setDotsPerMeterX(quarterTurn ? prepared.dotsPerMeterY()
                                       : prepared.dotsPerMeterX());
    output.setDotsPerMeterY(quarterTurn ? prepared.dotsPerMeterX()
                                       : prepared.dotsPerMeterY());

    const int bytesPerPixel = prepared.depth() / 8;
    for (int y = 0; y < prepared.height(); ++y) {
        const uchar *sourceRow = prepared.constScanLine(y);
        for (int x = 0; x < prepared.width(); ++x) {
            const QPoint destination = destinationPoint(QPoint(x, y),
                                                        prepared.size(),
                                                        operation);
            std::memcpy(output.scanLine(destination.y())
                            + destination.x() * bytesPerPixel,
                        sourceRow + x * bytesPerPixel,
                        static_cast<size_t>(bytesPerPixel));
        }
    }
    return output;
}

bool buildOrthogonalDocumentTransform(
    const PhotoDocument &document,
    const OrthogonalDocumentTransform operation,
    OrthogonalDocumentTransformResult *result,
    QString *errorMessage)
{
    if (!result || !document.hasImage()) {
        setError(errorMessage, QStringLiteral("No document is available to transform."));
        return false;
    }
    const QSize oldSize = document.sourceImage().size();
    const QSize newSize = destinationSize(oldSize, operation);
    QImage transformedCanvas = transformImageOrthogonally(document.sourceImage(),
                                                           operation);
    if (transformedCanvas.isNull() || transformedCanvas.size() != newSize) {
        setError(errorMessage,
                 QStringLiteral("The transformed canvas image could not be allocated."));
        return false;
    }

    QVector<LayerNode> layers = document.layers();
    transformRootLayers(layers, documentTransformMatrix(operation, oldSize));

    QVector<double> horizontal;
    QVector<double> vertical;
    const double width = oldSize.width();
    const double height = oldSize.height();
    switch (operation) {
    case OrthogonalDocumentTransform::FlipHorizontal:
        horizontal = document.horizontalGuides();
        for (const double x : document.verticalGuides()) {
            vertical.push_back(width - x);
        }
        break;
    case OrthogonalDocumentTransform::FlipVertical:
        vertical = document.verticalGuides();
        for (const double y : document.horizontalGuides()) {
            horizontal.push_back(height - y);
        }
        break;
    case OrthogonalDocumentTransform::Rotate90Clockwise:
        for (const double x : document.verticalGuides()) {
            horizontal.push_back(x);
        }
        for (const double y : document.horizontalGuides()) {
            vertical.push_back(height - y);
        }
        break;
    case OrthogonalDocumentTransform::Rotate90CounterClockwise:
        for (const double x : document.verticalGuides()) {
            horizontal.push_back(width - x);
        }
        for (const double y : document.horizontalGuides()) {
            vertical.push_back(y);
        }
        break;
    case OrthogonalDocumentTransform::Rotate180:
        for (const double y : document.horizontalGuides()) {
            horizontal.push_back(height - y);
        }
        for (const double x : document.verticalGuides()) {
            vertical.push_back(width - x);
        }
        break;
    }

    const SelectionMask::Snapshot transformedSelection = transformSelection(
        document.selectionMask(), operation, newSize);
    if (transformedSelection.size != newSize) {
        setError(errorMessage,
                 QStringLiteral("The transformed selection could not be allocated."));
        return false;
    }

    result->canvasImage = std::move(transformedCanvas);
    result->layers = std::move(layers);
    result->selection = transformedSelection;
    result->horizontalGuides = sortedUniqueGuides(std::move(horizontal), newSize.height());
    result->verticalGuides = sortedUniqueGuides(std::move(vertical), newSize.width());
    const bool quarterTurn = operation == OrthogonalDocumentTransform::Rotate90Clockwise
        || operation == OrthogonalDocumentTransform::Rotate90CounterClockwise;
    result->resolutionX = quarterTurn ? document.resolutionY() : document.resolutionX();
    result->resolutionY = quarterTurn ? document.resolutionX() : document.resolutionY();
    return true;
}

} // namespace vfx
