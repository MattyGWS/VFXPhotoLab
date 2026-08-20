#include "CanvasSizeOperations.h"

#include "CropOperations.h"
#include "VectorRasterizer.h"

#include <QRegion>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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
        && cancelRequested->load(std::memory_order_acquire);
}

int anchoredOrigin(const int oldExtent,
                   const int newExtent,
                   const int anchorPosition)
{
    const int difference = newExtent - oldExtent;
    switch (anchorPosition) {
    case 0: return 0;
    case 1:
        // Integer division truncates towards zero. This deliberately places
        // the unmatched odd pixel on the right/bottom for both expansion and
        // contraction: +5 -> two before, three after; -5 -> remove two before,
        // three after.
        return -(difference / 2);
    case 2: return oldExtent - newExtent;
    default: return 0;
    }
}

QPoint anchorPosition(CanvasAnchor anchor)
{
    switch (anchor) {
    case CanvasAnchor::TopLeft: return {0, 0};
    case CanvasAnchor::Top: return {1, 0};
    case CanvasAnchor::TopRight: return {2, 0};
    case CanvasAnchor::Left: return {0, 1};
    case CanvasAnchor::Centre: return {1, 1};
    case CanvasAnchor::Right: return {2, 1};
    case CanvasAnchor::BottomLeft: return {0, 2};
    case CanvasAnchor::Bottom: return {1, 2};
    case CanvasAnchor::BottomRight: return {2, 2};
    }
    return {1, 1};
}

void initialiseReferenceExtents(QVector<LayerNode> &layers,
                                const QSize &oldDocumentSize,
                                const std::atomic_bool *cancelRequested,
                                bool *ok)
{
    if (!*ok) {
        return;
    }
    for (LayerNode &layer : layers) {
        if (cancelled(cancelRequested)) {
            *ok = false;
            return;
        }
        if ((layer.type == LayerType::Raster || layer.type == LayerType::BaseImage)
            && (layer.rasterReferenceSize.isEmpty()
                || !layer.rasterReferenceSize.isValid())) {
            layer.rasterReferenceSize = oldDocumentSize;
            ++layer.revision;
        }
        if (!layer.maskImage.isNull()
            && (layer.maskReferenceSize.isEmpty()
                || !layer.maskReferenceSize.isValid())) {
            layer.maskReferenceSize = oldDocumentSize;
            ++layer.revision;
        }
        initialiseReferenceExtents(layer.children,
                                   oldDocumentSize,
                                   cancelRequested,
                                   ok);
    }
}

void appendDocumentTranslation(QVector<LayerNode> &layers,
                               const QPoint &translation)
{
    if (translation.isNull()) {
        return;
    }
    const QTransform documentMap = QTransform::fromTranslate(
        translation.x(), translation.y());
    // Applying the document-space map at each root preserves every child's
    // local transform and storage origin while translating the complete tree.
    for (LayerNode &layer : layers) {
        layer.transform = layer.transform * documentMap;
        ++layer.revision;
    }
}

SelectionMask::Snapshot translatedSelection(
    const SelectionMask &source,
    const QSize &outputSize,
    const QPoint &translation,
    const std::atomic_bool *cancelRequested)
{
    SelectionMask destination(outputSize);
    if (!source.isActive() || source.isEmpty()) {
        destination.deactivate();
        return destination.snapshot();
    }

    const QRect sourceBounds = source.nonZeroBounds();
    const QRect destinationBounds = sourceBounds.translated(translation);
    const QRect clippedDestination = destinationBounds.intersected(
        QRect(QPoint(), outputSize));
    if (clippedDestination.isEmpty()) {
        destination.deactivate();
        return destination.snapshot();
    }
    if (cancelled(cancelRequested)) {
        return {};
    }

    destination.selectNone();
    if (source.isFull()) {
        if (clippedDestination == QRect(QPoint(), outputSize)) {
            destination.selectAll();
        } else if (!destination.setCoverageRect(clippedDestination, 255)) {
            return {};
        }
    } else {
        constexpr int StripHeight = SelectionMask::TileSize;
        for (int y = clippedDestination.top();
             y <= clippedDestination.bottom();
             y += StripHeight) {
            if (cancelled(cancelRequested)) {
                return {};
            }
            const int height = std::min(
                StripHeight, clippedDestination.bottom() - y + 1);
            const QRect destinationStrip(
                clippedDestination.left(), y,
                clippedDestination.width(), height);
            const QRect sourceStrip = destinationStrip.translated(
                -translation.x(), -translation.y());
            const QImage coverage = source.coverageImage(sourceStrip);
            if (coverage.isNull()
                || !destination.setCoverageImage(destinationStrip, coverage)) {
                return {};
            }
        }
    }
    if (destination.isEmpty()) {
        destination.deactivate();
    }
    return destination.snapshot();
}

SelectionMask::Snapshot normalisedSelectionSnapshot(
    const SelectionMask::Snapshot &snapshot)
{
    SelectionMask selection(snapshot.size);
    if (!selection.restoreSnapshot(snapshot)) {
        return {};
    }
    if (!selection.isActive() || selection.isEmpty()) {
        selection.deactivate();
    }
    return selection.snapshot();
}

QColor documentFillColour(const PhotoDocument &document,
                          const QColor &requested)
{
    QColor colour = requested.isValid() ? requested : QColor(Qt::transparent);
    if (document.colourModel() == DocumentColourModel::Grayscale) {
        const int grey = qGray(colour.rgb());
        colour.setRgb(grey, grey, grey, colour.alpha());
    }
    return colour;
}

bool clearStraightImage(QImage *image,
                        const std::atomic_bool *cancelRequested)
{
    if (!image || image->isNull()) {
        return false;
    }
    for (int y = 0; y < image->height(); ++y) {
        if (cancelled(cancelRequested)) {
            return false;
        }
        uchar *line = image->scanLine(y);
        std::fill(line, line + image->bytesPerLine(), static_cast<uchar>(0));
    }
    return true;
}

bool fillStraightRect(QImage *image,
                      const QRect &requestedRect,
                      const QColor &colour,
                      const std::atomic_bool *cancelRequested)
{
    if (!image || image->isNull()) {
        return false;
    }
    const QRect rect = requestedRect.intersected(image->rect());
    if (rect.isEmpty()) {
        return true;
    }

    if (image->format() == QImage::Format_RGBA64) {
        const QRgba64 pixel = colour.rgba64();
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            if (cancelled(cancelRequested)) {
                return false;
            }
            auto *line = reinterpret_cast<QRgba64 *>(image->scanLine(y));
            std::fill(line + rect.left(), line + rect.right() + 1, pixel);
        }
        return true;
    }

    if (image->format() != QImage::Format_RGBA8888) {
        return false;
    }
    const uchar red = static_cast<uchar>(colour.red());
    const uchar green = static_cast<uchar>(colour.green());
    const uchar blue = static_cast<uchar>(colour.blue());
    const uchar alpha = static_cast<uchar>(colour.alpha());
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        if (cancelled(cancelRequested)) {
            return false;
        }
        uchar *line = image->scanLine(y)
            + static_cast<qsizetype>(rect.left()) * 4;
        for (int x = 0; x < rect.width(); ++x) {
            line[x * 4 + 0] = red;
            line[x * 4 + 1] = green;
            line[x * 4 + 2] = blue;
            line[x * 4 + 3] = alpha;
        }
    }
    return true;
}

bool appendCanvasExtensionLayer(const PhotoDocument &document,
                                const CanvasSizeRequest &request,
                                const QRect &oldCanvasInNewCoordinates,
                                QVector<LayerNode> *layers,
                                bool *created,
                                const std::atomic_bool *cancelRequested,
                                QString *errorMessage)
{
    if (created) {
        *created = false;
    }
    if (!layers || request.fillMode == CanvasFillMode::Transparent) {
        return true;
    }

    const QRect newBounds(QPoint(), request.pixelSize);
    QRegion exposed(newBounds);
    exposed = exposed.subtracted(QRegion(oldCanvasInNewCoordinates));
    if (exposed.isEmpty()) {
        return true;
    }

    const bool sixteenBit = document.sourceImage().depth() > 32;
    QImage extension(request.pixelSize,
                     sixteenBit ? QImage::Format_RGBA64
                                : QImage::Format_RGBA8888);
    if (extension.isNull()) {
        setError(errorMessage,
                 QStringLiteral("Could not allocate the Canvas Extension layer."));
        return false;
    }
    if (!clearStraightImage(&extension, cancelRequested)) {
        setError(errorMessage,
                 cancelled(cancelRequested)
                     ? QStringLiteral("Canvas size change cancelled.")
                     : QStringLiteral("Could not initialise the Canvas Extension layer."));
        return false;
    }
    extension.setColorSpace(document.sourceImage().colorSpace());
    extension.setDotsPerMeterX(document.sourceImage().dotsPerMeterX());
    extension.setDotsPerMeterY(document.sourceImage().dotsPerMeterY());
    extension.setDevicePixelRatio(document.sourceImage().devicePixelRatio());

    const QColor colour = documentFillColour(document, request.fillColour);
    for (const QRect &rect : exposed) {
        if (!fillStraightRect(&extension, rect, colour, cancelRequested)) {
            setError(errorMessage,
                     cancelled(cancelRequested)
                         ? QStringLiteral("Canvas size change cancelled.")
                         : QStringLiteral("Could not fill the Canvas Extension layer."));
            return false;
        }
    }

    LayerNode extensionLayer;
    extensionLayer.type = LayerType::Raster;
    extensionLayer.name = QStringLiteral("Canvas Extension");
    extensionLayer.visible = true;
    extensionLayer.opacity = 1.0;
    extensionLayer.blendMode = BlendMode::Copy;
    extensionLayer.rasterImage = std::move(extension);
    extensionLayer.rasterReferenceSize = request.pixelSize;
    extensionLayer.rasterReferenceOrigin = {};
    extensionLayer.transform.reset();
    extensionLayer.revision = 1;
    // Layers are stored top-to-bottom, so append at the root to create a
    // normal editable layer beneath every existing top-level layer/group.
    layers->push_back(std::move(extensionLayer));
    if (created) {
        *created = true;
    }
    return true;
}

bool buildNonDestructiveResult(const PhotoDocument &document,
                               const CanvasSizeRequest &request,
                               const QRect &newCanvasInOldCoordinates,
                               CanvasSizeResult *prepared,
                               const std::atomic_bool *cancelRequested,
                               QString *errorMessage)
{
    const QSize oldSize = document.sourceImage().size();
    const QPoint translation(-newCanvasInOldCoordinates.x(),
                             -newCanvasInOldCoordinates.y());

    prepared->previousCanvasRect = QRect(translation, oldSize);
    prepared->layers = document.layers();
    bool referencesOk = true;
    initialiseReferenceExtents(prepared->layers,
                               oldSize,
                               cancelRequested,
                               &referencesOk);
    if (!referencesOk || cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Canvas size change cancelled."));
        return false;
    }
    appendDocumentTranslation(prepared->layers, translation);

    const QImage &oldCanvas = document.sourceImage();
    prepared->canvasImage = QImage(request.pixelSize, oldCanvas.format());
    if (prepared->canvasImage.isNull()) {
        setError(errorMessage, QStringLiteral("Could not allocate the resized canvas."));
        return false;
    }
    prepared->canvasImage.fill(Qt::transparent);
    prepared->canvasImage.setColorSpace(oldCanvas.colorSpace());
    prepared->canvasImage.setDotsPerMeterX(oldCanvas.dotsPerMeterX());
    prepared->canvasImage.setDotsPerMeterY(oldCanvas.dotsPerMeterY());
    prepared->canvasImage.setDevicePixelRatio(oldCanvas.devicePixelRatio());

    prepared->selection = translatedSelection(document.selectionMask(),
                                               request.pixelSize,
                                               translation,
                                               cancelRequested);
    if (cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Canvas size change cancelled."));
        return false;
    }
    if (prepared->selection.size != request.pixelSize) {
        setError(errorMessage,
                 QStringLiteral("Could not translate the document selection."));
        return false;
    }

    for (const double guide : document.horizontalGuides()) {
        const double shifted = guide + translation.y();
        if (shifted >= 0.0 && shifted <= request.pixelSize.height()) {
            prepared->horizontalGuides.push_back(shifted);
        }
    }
    for (const double guide : document.verticalGuides()) {
        const double shifted = guide + translation.x();
        if (shifted >= 0.0 && shifted <= request.pixelSize.width()) {
            prepared->verticalGuides.push_back(shifted);
        }
    }
    return true;
}

bool buildDestructiveResult(const PhotoDocument &document,
                            const CanvasSizeRequest &request,
                            const QRect &newCanvasInOldCoordinates,
                            CanvasSizeResult *prepared,
                            const std::atomic_bool *cancelRequested,
                            QString *errorMessage)
{
    CropRequest cropRequest;
    cropRequest.documentRect = newCanvasInOldCoordinates;
    cropRequest.straightenAngle = 0.0;
    cropRequest.deleteCroppedPixels = true;

    CropResult cropResult;
    QString cropError;
    if (!buildCropResult(document,
                         cropRequest,
                         &cropResult,
                         cancelRequested,
                         &cropError)) {
        setError(errorMessage,
                 cropError.isEmpty()
                     ? QStringLiteral("Could not destructively clip the document bounds.")
                     : cropError);
        return false;
    }

    const QPoint translation(-newCanvasInOldCoordinates.x(),
                             -newCanvasInOldCoordinates.y());
    prepared->previousCanvasRect = QRect(translation,
                                         document.sourceImage().size());
    prepared->canvasImage = std::move(cropResult.canvasImage);
    prepared->canvasImage.setDevicePixelRatio(
        document.sourceImage().devicePixelRatio());
    prepared->layers = std::move(cropResult.layers);
    prepared->selection = normalisedSelectionSnapshot(cropResult.selection);
    if (prepared->selection.size != request.pixelSize) {
        setError(errorMessage,
                 QStringLiteral("Could not translate the destructively clipped selection."));
        return false;
    }
    prepared->horizontalGuides = std::move(cropResult.horizontalGuides);
    prepared->verticalGuides = std::move(cropResult.verticalGuides);
    prepared->destructiveClippingApplied = true;
    return true;
}

bool imageContentBounds(const QImage &source,
                        const bool alphaOnly,
                        QRect *bounds,
                        const std::atomic_bool *cancelRequested)
{
    if (!bounds) {
        return false;
    }
    *bounds = {};
    if (source.isNull()) {
        return true;
    }

    QImage converted;
    const QImage *image = &source;
    if (source.format() != QImage::Format_RGBA8888
        && source.format() != QImage::Format_RGBA64) {
        converted = source.convertToFormat(source.depth() > 32
                                               ? QImage::Format_RGBA64
                                               : QImage::Format_RGBA8888);
        if (converted.isNull()) {
            return false;
        }
        image = &converted;
    }

    int left = image->width();
    int top = image->height();
    int right = -1;
    int bottom = -1;
    if (image->format() == QImage::Format_RGBA64) {
        for (int y = 0; y < image->height(); ++y) {
            if (cancelled(cancelRequested)) {
                return false;
            }
            const auto *row = reinterpret_cast<const QRgba64 *>(
                image->constScanLine(y));
            for (int x = 0; x < image->width(); ++x) {
                const QRgba64 pixel = row[x];
                const bool occupied = alphaOnly
                    ? pixel.alpha() != 0
                    : pixel.red() != 0 || pixel.green() != 0
                        || pixel.blue() != 0 || pixel.alpha() != 0;
                if (!occupied) {
                    continue;
                }
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    } else {
        for (int y = 0; y < image->height(); ++y) {
            if (cancelled(cancelRequested)) {
                return false;
            }
            const uchar *row = image->constScanLine(y);
            for (int x = 0; x < image->width(); ++x) {
                const uchar *pixel = row + static_cast<qsizetype>(x) * 4;
                const bool occupied = alphaOnly
                    ? pixel[3] != 0
                    : pixel[0] != 0 || pixel[1] != 0
                        || pixel[2] != 0 || pixel[3] != 0;
                if (!occupied) {
                    continue;
                }
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }

    if (right >= left && bottom >= top) {
        *bounds = QRect(QPoint(left, top), QPoint(right, bottom));
    }
    return true;
}

bool maskCoverageBounds(const QImage &source,
                        const bool inverted,
                        QRect *bounds,
                        const std::atomic_bool *cancelRequested)
{
    if (!bounds) {
        return false;
    }
    *bounds = {};
    if (source.isNull()) {
        return true;
    }
    const QImage image = source.format() == QImage::Format_Grayscale8
        ? source : source.convertToFormat(QImage::Format_Grayscale8);
    if (image.isNull()) {
        return false;
    }

    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        if (cancelled(cancelRequested)) {
            return false;
        }
        const uchar *row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const int coverage = inverted ? 255 - row[x] : row[x];
            if (coverage == 0) {
                continue;
            }
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    if (right >= left && bottom >= top) {
        *bounds = QRect(QPoint(left, top), QPoint(right, bottom));
    }
    return true;
}

QRectF boundsInReferenceSpace(const QRect &pixelBounds,
                              const QSize &imageSize,
                              const QSize &referenceSize,
                              const QPointF &referenceOrigin)
{
    if (pixelBounds.isEmpty() || imageSize.isEmpty()
        || referenceSize.isEmpty()) {
        return {};
    }
    const double scaleX = referenceSize.width()
        / static_cast<double>(imageSize.width());
    const double scaleY = referenceSize.height()
        / static_cast<double>(imageSize.height());
    return QRectF(referenceOrigin.x() + pixelBounds.x() * scaleX,
                  referenceOrigin.y() + pixelBounds.y() * scaleY,
                  pixelBounds.width() * scaleX,
                  pixelBounds.height() * scaleY);
}

void uniteBounds(QRectF *destination, const QRectF &candidate)
{
    if (!destination || candidate.isEmpty()) {
        return;
    }
    *destination = destination->isEmpty()
        ? candidate : destination->united(candidate);
}

bool hasEffectiveFiniteMask(const LayerNode &layer)
{
    if (!layer.maskEnabled || layer.maskImage.isNull()) {
        return false;
    }
    return layer.maskInverted
        || layer.maskImage.size() != QSize(1, 1)
        || qGray(layer.maskImage.pixel(0, 0)) != 255;
}

bool applyFiniteLayerMask(const LayerNode &layer,
                          const QSize &documentSize,
                          const QTransform &worldTransform,
                          QRectF *bounds,
                          const std::atomic_bool *cancelRequested)
{
    if (!bounds || bounds->isEmpty() || !hasEffectiveFiniteMask(layer)) {
        return true;
    }
    QRect maskPixels;
    if (!maskCoverageBounds(layer.maskImage,
                            layer.maskInverted,
                            &maskPixels,
                            cancelRequested)) {
        return false;
    }
    if (maskPixels.isEmpty()) {
        *bounds = {};
        return true;
    }
    const QSize referenceSize = layer.maskReferenceSize.isEmpty()
        ? documentSize : layer.maskReferenceSize;
    const QRectF localMask = boundsInReferenceSpace(maskPixels,
                                                     layer.maskImage.size(),
                                                     referenceSize,
                                                     layer.maskReferenceOrigin);
    const QRectF documentMask = worldTransform.mapRect(localMask).normalized();
    *bounds = bounds->intersected(documentMask);
    return true;
}

bool collectRevealStorageBounds(const QImage &source,
                                const QVector<LayerNode> &layers,
                                const QSize &documentSize,
                                const QTransform &parentTransform,
                                QRectF *bounds,
                                const std::atomic_bool *cancelRequested)
{
    for (const LayerNode &layer : layers) {
        if (cancelled(cancelRequested)) {
            return false;
        }
        const QTransform worldTransform = layer.transform * parentTransform;
        if (layer.type == LayerType::Raster || layer.type == LayerType::BaseImage) {
            const QImage &pixels = layer.type == LayerType::BaseImage
                    && layer.rasterImage.isNull()
                ? source : layer.rasterImage;
            QRect pixelBounds;
            if (!imageContentBounds(pixels,
                                    false,
                                    &pixelBounds,
                                    cancelRequested)) {
                return false;
            }
            const QSize referenceSize = layer.rasterReferenceSize.isEmpty()
                ? documentSize : layer.rasterReferenceSize;
            const QRectF local = boundsInReferenceSpace(pixelBounds,
                                                        pixels.size(),
                                                        referenceSize,
                                                        layer.rasterReferenceOrigin);
            uniteBounds(bounds, worldTransform.mapRect(local).normalized());
        } else if (layer.type == LayerType::Vector) {
            const QRectF rendered = VectorRasterizer::contentBounds(
                layer, worldTransform);
            if (!rendered.isEmpty()) {
                uniteBounds(bounds, rendered.normalized());
            }
        } else if (layer.type == LayerType::Group) {
            if (!collectRevealStorageBounds(source,
                                            layer.children,
                                            documentSize,
                                            worldTransform,
                                            bounds,
                                            cancelRequested)) {
                return false;
            }
        }
    }
    return true;
}

bool selectedNodeBounds(const QImage &source,
                        const LayerNode &layer,
                        const QSize &documentSize,
                        const QTransform &parentTransform,
                        QRectF *bounds,
                        const std::atomic_bool *cancelRequested)
{
    if (!bounds) {
        return false;
    }
    *bounds = {};
    const QTransform worldTransform = layer.transform * parentTransform;

    if (layer.type == LayerType::Raster || layer.type == LayerType::BaseImage) {
        const QImage &pixels = layer.type == LayerType::BaseImage
                && layer.rasterImage.isNull()
            ? source : layer.rasterImage;
        QRect pixelBounds;
        if (!imageContentBounds(pixels,
                                true,
                                &pixelBounds,
                                cancelRequested)) {
            return false;
        }
        const QSize referenceSize = layer.rasterReferenceSize.isEmpty()
            ? documentSize : layer.rasterReferenceSize;
        const QRectF local = boundsInReferenceSpace(pixelBounds,
                                                    pixels.size(),
                                                    referenceSize,
                                                    layer.rasterReferenceOrigin);
        *bounds = worldTransform.mapRect(local).normalized();
        return applyFiniteLayerMask(layer,
                                    documentSize,
                                    worldTransform,
                                    bounds,
                                    cancelRequested);
    }

    if (layer.type == LayerType::Vector) {
        const QRectF rendered = VectorRasterizer::contentBounds(
            layer, worldTransform);
        if (!rendered.isEmpty()) {
            *bounds = rendered.normalized();
        }
        return applyFiniteLayerMask(layer,
                                    documentSize,
                                    worldTransform,
                                    bounds,
                                    cancelRequested);
    }

    if (layer.type == LayerType::Adjustment) {
        if (!hasEffectiveFiniteMask(layer)) {
            return true;
        }
        QRect maskPixels;
        if (!maskCoverageBounds(layer.maskImage,
                                layer.maskInverted,
                                &maskPixels,
                                cancelRequested)) {
            return false;
        }
        const QSize referenceSize = layer.maskReferenceSize.isEmpty()
            ? documentSize : layer.maskReferenceSize;
        const QRectF local = boundsInReferenceSpace(maskPixels,
                                                    layer.maskImage.size(),
                                                    referenceSize,
                                                    layer.maskReferenceOrigin);
        *bounds = worldTransform.mapRect(local).normalized();
        return true;
    }

    for (const LayerNode &child : layer.children) {
        QRectF childBounds;
        if (!selectedNodeBounds(source,
                                child,
                                documentSize,
                                worldTransform,
                                &childBounds,
                                cancelRequested)) {
            return false;
        }
        uniteBounds(bounds, childBounds);
    }
    return applyFiniteLayerMask(layer,
                                documentSize,
                                worldTransform,
                                bounds,
                                cancelRequested);
}

bool collectSelectedLayerBounds(const QImage &source,
                                const QVector<LayerNode> &layers,
                                const QSize &documentSize,
                                const QTransform &parentTransform,
                                const QSet<QUuid> &selected,
                                QRectF *bounds,
                                const std::atomic_bool *cancelRequested)
{
    for (const LayerNode &layer : layers) {
        if (cancelled(cancelRequested)) {
            return false;
        }
        const QTransform worldTransform = layer.transform * parentTransform;
        if (selected.contains(layer.id)) {
            QRectF selectedBounds;
            if (!selectedNodeBounds(source,
                                    layer,
                                    documentSize,
                                    parentTransform,
                                    &selectedBounds,
                                    cancelRequested)) {
                return false;
            }
            uniteBounds(bounds, selectedBounds);
            continue;
        }
        if (!collectSelectedLayerBounds(source,
                                        layer.children,
                                        documentSize,
                                        worldTransform,
                                        selected,
                                        bounds,
                                        cancelRequested)) {
            return false;
        }
    }
    return true;
}

QRect alignedDocumentRect(const QRectF &source)
{
    if (source.isEmpty() || !std::isfinite(source.x())
        || !std::isfinite(source.y()) || !std::isfinite(source.width())
        || !std::isfinite(source.height())) {
        return {};
    }
    const double left = std::floor(source.x());
    const double top = std::floor(source.y());
    const double right = std::ceil(source.x() + source.width());
    const double bottom = std::ceil(source.y() + source.height());
    const double width = right - left;
    const double height = bottom - top;
    if (width < 1.0 || height < 1.0
        || left < std::numeric_limits<int>::min()
        || top < std::numeric_limits<int>::min()
        || left > std::numeric_limits<int>::max()
        || top > std::numeric_limits<int>::max()
        || right < std::numeric_limits<int>::min()
        || bottom < std::numeric_limits<int>::min()
        || right > std::numeric_limits<int>::max()
        || bottom > std::numeric_limits<int>::max()
        || width > std::numeric_limits<int>::max()
        || height > std::numeric_limits<int>::max()) {
        return {};
    }
    return QRect(static_cast<int>(left),
                 static_cast<int>(top),
                 static_cast<int>(width),
                 static_cast<int>(height));
}

bool validCanvasDocumentRect(const QRect &rect)
{
    constexpr qint64 SafeOrigin = 1073741824;
    return !rect.isEmpty()
        && rect.width() >= 1 && rect.height() >= 1
        && rect.width() <= 32768 && rect.height() <= 32768
        && std::abs(static_cast<qint64>(rect.x())) <= SafeOrigin
        && std::abs(static_cast<qint64>(rect.y())) <= SafeOrigin;
}

bool canvasSurfaceFitsPersistenceLimit(const QSize &size,
                                       const int imageDepth)
{
    // Private Hot/Warm/Cold snapshots encode one exact image payload with a
    // 32-bit-safe byte ceiling. Reject an operation before QImage allocation
    // when its canvas alone could not survive residency or save/reopen.
    constexpr quint64 MaximumPersistentImageBytes = 0xfffffffeULL;
    if (size.isEmpty()) {
        return false;
    }
    const quint64 bytesPerPixel = imageDepth > 32 ? 8u : 4u;
    const quint64 pixels = static_cast<quint64>(size.width())
        * static_cast<quint64>(size.height());
    return pixels <= MaximumPersistentImageBytes / bytesPerPixel;
}

} // namespace

QRect canvasSizeDocumentRect(const QSize &oldSize,
                             const QSize &newSize,
                             const CanvasAnchor anchor)
{
    if (oldSize.isEmpty() || newSize.isEmpty()) {
        return {};
    }
    const QPoint position = anchorPosition(anchor);
    return QRect(
        anchoredOrigin(oldSize.width(), newSize.width(), position.x()),
        anchoredOrigin(oldSize.height(), newSize.height(), position.y()),
        newSize.width(),
        newSize.height());
}

bool buildCanvasBoundsResult(const PhotoDocument &document,
                             const QRect &newCanvasInOldCoordinates,
                             const CanvasFillMode fillMode,
                             const QColor &fillColour,
                             const bool deleteOutsideCanvas,
                             CanvasSizeResult *result,
                             const std::atomic_bool *cancelRequested,
                             QString *errorMessage)
{
    if (!result || !document.hasImage()) {
        setError(errorMessage, QStringLiteral("There is no document canvas to resize."));
        return false;
    }
    if (!validCanvasDocumentRect(newCanvasInOldCoordinates)) {
        setError(errorMessage,
                 QStringLiteral("Canvas dimensions must be between 1 and 32768 pixels."));
        return false;
    }
    if (!canvasSurfaceFitsPersistenceLimit(
            newCanvasInOldCoordinates.size(), document.sourceImage().depth())) {
        setError(errorMessage,
                 QStringLiteral("The requested canvas would exceed the exact "
                                "Hot/Warm/Cold snapshot image limit. Choose "
                                "smaller dimensions."));
        return false;
    }
    if (cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Canvas size change cancelled."));
        return false;
    }

    CanvasSizeRequest request;
    request.pixelSize = newCanvasInOldCoordinates.size();
    request.fillMode = fillMode;
    request.fillColour = fillColour;
    request.deleteOutsideCanvas = deleteOutsideCanvas;

    CanvasSizeResult prepared;
    const bool preparedBounds = deleteOutsideCanvas
        ? buildDestructiveResult(document,
                                 request,
                                 newCanvasInOldCoordinates,
                                 &prepared,
                                 cancelRequested,
                                 errorMessage)
        : buildNonDestructiveResult(document,
                                    request,
                                    newCanvasInOldCoordinates,
                                    &prepared,
                                    cancelRequested,
                                    errorMessage);
    if (!preparedBounds) {
        return false;
    }
    if (cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Canvas size change cancelled."));
        return false;
    }

    if (!appendCanvasExtensionLayer(document,
                                    request,
                                    prepared.previousCanvasRect,
                                    &prepared.layers,
                                    &prepared.extensionLayerCreated,
                                    cancelRequested,
                                    errorMessage)) {
        return false;
    }
    if (cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Canvas size change cancelled."));
        return false;
    }

    *result = std::move(prepared);
    return true;
}

bool buildCanvasSizeResult(const PhotoDocument &document,
                           const CanvasSizeRequest &request,
                           CanvasSizeResult *result,
                           const std::atomic_bool *cancelRequested,
                           QString *errorMessage)
{
    if (!document.hasImage()) {
        setError(errorMessage, QStringLiteral("There is no document canvas to resize."));
        return false;
    }
    if (request.pixelSize.width() < 1 || request.pixelSize.height() < 1
        || request.pixelSize.width() > 32768
        || request.pixelSize.height() > 32768) {
        setError(errorMessage,
                 QStringLiteral("Canvas dimensions must be between 1 and 32768 pixels."));
        return false;
    }
    const QSize oldSize = document.sourceImage().size();
    if (oldSize.isEmpty()) {
        setError(errorMessage, QStringLiteral("The document canvas is invalid."));
        return false;
    }

    const QRect newCanvasInOldCoordinates = canvasSizeDocumentRect(
        oldSize, request.pixelSize, request.anchor);
    if (newCanvasInOldCoordinates.isEmpty()) {
        setError(errorMessage, QStringLiteral("The requested canvas bounds are invalid."));
        return false;
    }
    return buildCanvasBoundsResult(document,
                                   newCanvasInOldCoordinates,
                                   request.fillMode,
                                   request.fillColour,
                                   request.deleteOutsideCanvas,
                                   result,
                                   cancelRequested,
                                   errorMessage);
}

bool buildCanvasFitResult(const PhotoDocument &document,
                          const CanvasFitRequest &request,
                          CanvasFitResult *result,
                          const std::atomic_bool *cancelRequested,
                          QString *errorMessage)
{
    if (!result || !document.hasImage()) {
        setError(errorMessage, QStringLiteral("There is no document canvas to fit."));
        return false;
    }
    if (cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Canvas fit cancelled."));
        return false;
    }

    const QSize documentSize = document.sourceImage().size();
    const QRect currentCanvas(QPoint(), documentSize);
    QRect target;
    QString noChangeMessage;

    switch (request.mode) {
    case CanvasFitMode::RevealAll: {
        QRectF storedBounds;
        if (!collectRevealStorageBounds(document.sourceImage(),
                                        document.layers(),
                                        documentSize,
                                        QTransform(),
                                        &storedBounds,
                                        cancelRequested)) {
            setError(errorMessage,
                     cancelled(cancelRequested)
                         ? QStringLiteral("Reveal All cancelled.")
                         : QStringLiteral("Could not inspect stored layer bounds."));
            return false;
        }
        const QRect storedRect = alignedDocumentRect(storedBounds);
        target = storedRect.isEmpty()
            ? currentCanvas : currentCanvas.united(storedRect);
        noChangeMessage = QStringLiteral(
            "All stored raster pixels are already inside the canvas.");
        break;
    }
    case CanvasFitMode::Selection: {
        const SelectionMask &selection = document.selectionMask();
        if (!selection.isActive() || selection.isEmpty()) {
            result->noChange = true;
            result->noChangeMessage = QStringLiteral(
                "There is no non-empty selection to fit.");
            return true;
        }
        target = selection.nonZeroBounds();
        noChangeMessage = QStringLiteral(
            "The canvas already matches the selection bounds.");
        break;
    }
    case CanvasFitMode::SelectedLayers: {
        QSet<QUuid> selected;
        for (const QUuid &id : request.layerIds) {
            if (!id.isNull() && document.containsLayer(id)) {
                selected.insert(id);
            }
        }
        if (selected.isEmpty()) {
            result->noChange = true;
            result->noChangeMessage = QStringLiteral(
                "Select at least one layer to fit the canvas.");
            return true;
        }
        QRectF selectedBounds;
        if (!collectSelectedLayerBounds(document.sourceImage(),
                                        document.layers(),
                                        documentSize,
                                        QTransform(),
                                        selected,
                                        &selectedBounds,
                                        cancelRequested)) {
            setError(errorMessage,
                     cancelled(cancelRequested)
                         ? QStringLiteral("Fit Canvas to Selected Layers cancelled.")
                         : QStringLiteral("Could not inspect the selected layer bounds."));
            return false;
        }
        target = alignedDocumentRect(selectedBounds);
        if (target.isEmpty()) {
            result->noChange = true;
            result->noChangeMessage = QStringLiteral(
                "The selected layers have no finite editable bounds to fit.");
            return true;
        }
        noChangeMessage = QStringLiteral(
            "The canvas already matches the selected layer bounds.");
        break;
    }
    }

    if (!validCanvasDocumentRect(target)) {
        setError(errorMessage,
                 QStringLiteral("The fitted canvas must be between 1 and 32768 pixels on each side."));
        return false;
    }
    result->documentRect = target;
    if (target == currentCanvas) {
        result->noChange = true;
        result->noChangeMessage = noChangeMessage;
        return true;
    }

    if (!buildCanvasBoundsResult(document,
                                 target,
                                 CanvasFillMode::Transparent,
                                 QColor(Qt::transparent),
                                 false,
                                 &result->canvas,
                                 cancelRequested,
                                 errorMessage)) {
        return false;
    }
    return true;
}

} // namespace vfx
