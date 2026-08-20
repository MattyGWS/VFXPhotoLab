#include "LayerMergeOperations.h"

#include "ImageProcessor.h"
#include "PhotoDocument.h"
#include "TransformSafety.h"

#include <QColorSpace>
#include <QImage>
#include <QRectF>
#include <QRgba64>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vfx {
namespace {

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) *errorMessage = message;
}

bool rasterCompatible(const LayerNode &layer)
{
    return layer.type == LayerType::Raster || layer.type == LayerType::BaseImage;
}

bool vectorCompatible(const LayerNode &layer)
{
    return layer.type == LayerType::Vector;
}

QImage transparentMergeImage(const QImage &reference, const QSize &size)
{
    if (reference.isNull() || size.isEmpty()) return {};
    const bool highPrecision = reference.depth() > 32
        || reference.format() == QImage::Format_Grayscale16;
    const QImage::Format format = highPrecision
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    QImage result(size, format);
    if (result.isNull()) return {};
    result.fill(Qt::transparent);
    result.setDevicePixelRatio(reference.devicePixelRatio());
    result.setColorSpace(reference.colorSpace());
    result.setDotsPerMeterX(reference.dotsPerMeterX());
    result.setDotsPerMeterY(reference.dotsPerMeterY());
    return result;
}

QRect storedPixelBounds(const QImage &image)
{
    if (image.isNull()) return {};
    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;

    if (image.depth() > 32) {
        const QImage rgba = image.format() == QImage::Format_RGBA64
            ? image : image.convertToFormat(QImage::Format_RGBA64);
        for (int y = 0; y < rgba.height(); ++y) {
            const auto *row = reinterpret_cast<const QRgba64 *>(rgba.constScanLine(y));
            for (int x = 0; x < rgba.width(); ++x) {
                const QRgba64 pixel = row[x];
                if (pixel.red() == 0 && pixel.green() == 0
                    && pixel.blue() == 0 && pixel.alpha() == 0) {
                    continue;
                }
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    } else {
        const QImage rgba = image.format() == QImage::Format_RGBA8888
            ? image : image.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < rgba.height(); ++y) {
            const uchar *row = rgba.constScanLine(y);
            for (int x = 0; x < rgba.width(); ++x) {
                const uchar *pixel = row + x * 4;
                if (pixel[0] == 0 && pixel[1] == 0
                    && pixel[2] == 0 && pixel[3] == 0) {
                    continue;
                }
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    }

    return right < left || bottom < top
        ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}

void regeneratePathNodeIds(VectorBezierPath *path)
{
    if (!path) return;
    for (VectorPathNode &node : path->nodes) {
        node.id = QUuid::createUuid();
    }
}

bool mapPath(VectorBezierPath *path, const QTransform &transform)
{
    if (!path || !transformMatrixIsFiniteAndBounded(transform)) return false;
    for (VectorPathNode &node : path->nodes) {
        node.anchor = transform.map(node.anchor);
        node.inHandle = transform.map(node.inHandle);
        node.outHandle = transform.map(node.outHandle);
        if (!std::isfinite(node.anchor.x()) || !std::isfinite(node.anchor.y())
            || !std::isfinite(node.inHandle.x()) || !std::isfinite(node.inHandle.y())
            || !std::isfinite(node.outHandle.x()) || !std::isfinite(node.outHandle.y())) {
            return false;
        }
    }
    path->normalise();
    return path->isSafe();
}

bool findParentChildren(QVector<LayerNode> *layers,
                        const QUuid &parentId,
                        QVector<LayerNode> **children)
{
    if (!layers || !children) return false;
    if (parentId.isNull()) {
        *children = layers;
        return true;
    }
    for (LayerNode &layer : *layers) {
        if (layer.id == parentId) {
            if (layer.type != LayerType::Group) return false;
            *children = &layer.children;
            return true;
        }
        if (findParentChildren(&layer.children, parentId, children)) return true;
    }
    return false;
}

bool buildRasterLayer(const PhotoDocument &document,
                      const LayerMergePlan &plan,
                      LayerNode *mergedLayer,
                      QString *errorMessage)
{
    constexpr int MaximumMergeExtent = 32768;
    constexpr double RasterEdgeMargin = 2.0;

    QVector<LayerNode> isolated;
    isolated.reserve(plan.layerIds.size());
    QRectF worldStorageBounds;
    bool hasStoredPixels = false;
    const QSize documentSize = document.sourceImage().size();

    for (const QUuid &id : plan.layerIds) {
        LayerNode layer = document.layerById(id);
        if (!rasterCompatible(layer)) {
            setError(errorMessage, QStringLiteral("The raster merge contains a non-raster layer."));
            return false;
        }
        if (!layer.visible) {
            setError(errorMessage,
                     QStringLiteral("Hidden raster layers must be made visible before merging so their stored pixels are not discarded."));
            return false;
        }
        const QTransform worldTransform = document.layerWorldTransform(id);
        if (!transformMatrixIsFiniteAndBounded(worldTransform)) {
            setError(errorMessage, QStringLiteral("A selected raster layer has an unsafe transform."));
            return false;
        }
        if (layer.type == LayerType::BaseImage && layer.rasterImage.isNull()) {
            layer.rasterImage = document.sourceImage();
            layer.rasterReferenceSize = documentSize;
            layer.rasterReferenceOrigin = QPointF();
        }
        if (layer.rasterReferenceSize.isEmpty()) {
            layer.rasterReferenceSize = documentSize;
        }
        if (layer.hasMask() && layer.maskReferenceSize.isEmpty()) {
            layer.maskReferenceSize = documentSize;
        }

        if (!layer.rasterImage.isNull()) {
            const QTransform referenceToDocument = QTransform::fromTranslate(
                layer.rasterReferenceOrigin.x(), layer.rasterReferenceOrigin.y())
                * worldTransform;
            const QRectF localReference(QPointF(), QSizeF(layer.rasterReferenceSize));
            QString transformError;
            if (!transformHasSafeDomain(referenceToDocument,
                                        localReference,
                                        1.0e9,
                                        &transformError)) {
                setError(errorMessage,
                         transformError.isEmpty()
                             ? QStringLiteral("A selected raster layer has an unsafe projective domain.")
                             : transformError);
                return false;
            }
            const QRectF mapped = referenceToDocument.mapRect(localReference).normalized();
            if (!mapped.isValid() || mapped.isEmpty()
                || !std::isfinite(mapped.left()) || !std::isfinite(mapped.top())
                || !std::isfinite(mapped.right()) || !std::isfinite(mapped.bottom())) {
                setError(errorMessage, QStringLiteral("A selected raster layer has unsafe storage bounds."));
                return false;
            }
            worldStorageBounds = hasStoredPixels
                ? worldStorageBounds.united(mapped) : mapped;
            hasStoredPixels = true;
        }

        layer.transform = worldTransform;
        isolated.push_back(std::move(layer));
    }

    const LayerNode top = document.layerById(plan.layerIds.constFirst());
    LayerNode result;
    result.id = top.id;
    result.type = LayerType::Raster;
    result.name = top.name;
    result.visible = true;
    result.opacity = 1.0;
    result.blendMode = BlendMode::Copy;
    result.groupCompositeMode = GroupCompositeMode::Isolated;
    result.revision = top.revision == std::numeric_limits<quint64>::max()
        ? top.revision : std::max<quint64>(1, top.revision + 1);

    QTransform parentWorld;
    if (!plan.parentId.isNull()) {
        parentWorld = document.layerWorldTransform(plan.parentId);
    }
    bool invertible = false;
    const QTransform parentInverse = parentWorld.inverted(&invertible);
    if (!invertible || !transformMatrixIsFiniteAndBounded(parentInverse)) {
        setError(errorMessage, QStringLiteral("The destination group transform cannot contain a merged raster layer."));
        return false;
    }
    result.transform = parentInverse;

    if (!hasStoredPixels) {
        result.rasterImage = {};
        result.rasterReferenceSize = documentSize;
        result.rasterReferenceOrigin = QPointF();
        *mergedLayer = std::move(result);
        return true;
    }

    const QRect mergeBounds = worldStorageBounds
        .adjusted(-RasterEdgeMargin, -RasterEdgeMargin,
                  RasterEdgeMargin, RasterEdgeMargin)
        .toAlignedRect();
    if (mergeBounds.isEmpty()
        || mergeBounds.width() > MaximumMergeExtent
        || mergeBounds.height() > MaximumMergeExtent) {
        setError(errorMessage,
                 QStringLiteral("The selected raster layers span an area too large to merge safely."));
        return false;
    }

    const QTransform documentToMerge = QTransform::fromTranslate(
        -static_cast<double>(mergeBounds.x()),
        -static_cast<double>(mergeBounds.y()));
    for (LayerNode &layer : isolated) {
        layer.transform = layer.transform * documentToMerge;
    }

    const QImage transparent = transparentMergeImage(
        document.sourceImage(), mergeBounds.size());
    if (transparent.isNull()) {
        setError(errorMessage, QStringLiteral("Could not allocate the raster merge surface."));
        return false;
    }
    QImage flattened = ImageProcessor::renderPreservingHiddenRgb(
        transparent,
        isolated,
        nullptr,
        mergeBounds.size(),
        document.colourState().processingCompatibility);
    if (flattened.isNull()) {
        setError(errorMessage, QStringLiteral("The selected raster layers could not be composited."));
        return false;
    }
    flattened.setColorSpace(document.sourceImage().colorSpace());
    flattened.setDotsPerMeterX(document.sourceImage().dotsPerMeterX());
    flattened.setDotsPerMeterY(document.sourceImage().dotsPerMeterY());

    const QRect storedBounds = storedPixelBounds(flattened);
    if (storedBounds.isEmpty()) {
        result.rasterImage = {};
        result.rasterReferenceSize = documentSize;
        result.rasterReferenceOrigin = QPointF();
    } else {
        result.rasterImage = flattened.copy(storedBounds);
        result.rasterReferenceSize = storedBounds.size();
        result.rasterReferenceOrigin = QPointF(
            mergeBounds.x() + storedBounds.x(),
            mergeBounds.y() + storedBounds.y());
    }
    if (!result.rasterImage.isNull()) {
        result.rasterImage.setDevicePixelRatio(document.sourceImage().devicePixelRatio());
        result.rasterImage.setColorSpace(document.sourceImage().colorSpace());
        result.rasterImage.setDotsPerMeterX(document.sourceImage().dotsPerMeterX());
        result.rasterImage.setDotsPerMeterY(document.sourceImage().dotsPerMeterY());
    }
    *mergedLayer = std::move(result);
    return true;
}

bool buildVectorLayer(const PhotoDocument &document,
                      const LayerMergePlan &plan,
                      LayerNode *mergedLayer,
                      QString *errorMessage)
{
    qsizetype objectCount = 0;
    for (const QUuid &id : plan.layerIds) {
        const LayerNode layer = document.layerById(id);
        if (!vectorCompatible(layer)) {
            setError(errorMessage, QStringLiteral("The vector merge contains a non-vector layer."));
            return false;
        }
        if (!layer.visible) {
            setError(errorMessage, QStringLiteral("Hidden vector layers must be made visible before merging."));
            return false;
        }
        if (layer.hasMask()) {
            setError(errorMessage,
                     QStringLiteral("Vector layers with masks cannot yet be merged without rasterising the mask."));
            return false;
        }
        if (std::abs(layer.opacity - 1.0) > 1.0e-9
            || layer.blendMode != BlendMode::Copy) {
            setError(errorMessage,
                     QStringLiteral("Vector merging currently requires 100% layer opacity and the Normal/Copy blend mode so the result stays editable and visually exact."));
            return false;
        }
        if (layer.vectorData.featherRadius > 1.0e-12) {
            setError(errorMessage,
                     QStringLiteral("Feathered vector layers cannot be merged into one editable vector layer without changing the per-layer combined-silhouette Feather result. Set Feather to 0 px before Merge Layers, or flatten through an explicit raster/export workflow."));
            return false;
        }
        if (layer.transform.type() == QTransform::TxProject
            || !transformMatrixIsFiniteAndBounded(layer.transform)) {
            setError(errorMessage,
                     QStringLiteral("Projective vector transforms must be applied or simplified before merging."));
            return false;
        }
        objectCount += layer.vectorData.objects.size();
        if (objectCount > VectorLayerData::MaximumObjectCount) {
            setError(errorMessage, QStringLiteral("The merged vector layer would exceed the safe object limit."));
            return false;
        }
    }

    const LayerNode top = document.layerById(plan.layerIds.constFirst());
    LayerNode result;
    result.id = top.id;
    result.type = LayerType::Vector;
    result.name = top.name;
    result.visible = true;
    result.opacity = 1.0;
    result.blendMode = BlendMode::Copy;
    result.groupCompositeMode = GroupCompositeMode::Isolated;
    result.transform.reset();
    result.revision = top.revision == std::numeric_limits<quint64>::max()
        ? top.revision : std::max<quint64>(1, top.revision + 1);
    result.vectorData.featherRadius = 0.0;
    result.vectorData.objects.reserve(objectCount);

    // VectorRasterizer draws objects in array order. Layers are stored top to
    // bottom, so append bottom layers first and top layers last to retain the
    // original visual stacking inside the new single vector layer.
    for (int layerIndex = plan.layerIds.size() - 1; layerIndex >= 0; --layerIndex) {
        const QUuid id = plan.layerIds.at(layerIndex);
        const LayerNode layer = document.layerById(id);
        const QTransform layerWorld = document.layerWorldTransform(id);
        if (!layerWorld.isInvertible()
            || !transformMatrixIsFiniteAndBounded(layerWorld)) {
            setError(errorMessage, QStringLiteral("A selected vector layer has an unsafe world transform."));
            return false;
        }
        for (const VectorShape &sourceShape : layer.vectorData.objects) {
            VectorShape shape = sourceShape;
            if (!shape.convertToPath(layerWorld)) {
                setError(errorMessage,
                         QStringLiteral("A selected vector shape could not be converted to an editable path."));
                return false;
            }
            if (shape.transform.type() == QTransform::TxProject) {
                setError(errorMessage,
                         QStringLiteral("Projective vector object transforms must be applied before merging."));
                return false;
            }
            const QTransform objectToParent = shape.transform * layer.transform;
            if (!mapPath(&shape.bezierPath, objectToParent)) {
                setError(errorMessage, QStringLiteral("A merged vector path exceeded the safe coordinate range."));
                return false;
            }
            for (VectorBezierPath &path : shape.additionalBezierPaths) {
                if (!mapPath(&path, objectToParent)) {
                    setError(errorMessage, QStringLiteral("A merged compound path exceeded the safe coordinate range."));
                    return false;
                }
            }
            shape.id = QUuid::createUuid();
            regeneratePathNodeIds(&shape.bezierPath);
            for (VectorBezierPath &path : shape.additionalBezierPaths) {
                regeneratePathNodeIds(&path);
            }
            shape.transform.reset();
            shape.revision = shape.revision == std::numeric_limits<quint64>::max()
                ? shape.revision : std::max<quint64>(1, shape.revision + 1);
            shape.normalise();
            if (!shape.isSafe()) {
                setError(errorMessage, QStringLiteral("A converted vector path failed validation."));
                return false;
            }
            result.vectorData.objects.push_back(std::move(shape));
        }
    }
    result.vectorData.normalise();
    if (!result.vectorData.isSafe()) {
        setError(errorMessage, QStringLiteral("The merged vector layer failed validation."));
        return false;
    }
    *mergedLayer = std::move(result);
    return true;
}

} // namespace

LayerMergePlan LayerMergeOperations::analyse(
    const PhotoDocument &document,
    const QVector<QUuid> &selectedRootLayerIds,
    QString *errorMessage)
{
    LayerMergePlan plan;
    if (!document.hasImage() || selectedRootLayerIds.size() < 2) {
        setError(errorMessage, QStringLiteral("Select at least two layers to merge."));
        return {};
    }

    struct Entry {
        QUuid id;
        QUuid parentId;
        int index = -1;
    };
    QVector<Entry> entries;
    entries.reserve(selectedRootLayerIds.size());
    QSet<QUuid> seen;
    for (const QUuid &id : selectedRootLayerIds) {
        if (id.isNull() || seen.contains(id) || !document.containsLayer(id)) continue;
        seen.insert(id);
        Entry entry;
        entry.id = id;
        if (!document.layerPlacement(id, &entry.parentId, &entry.index)) {
            setError(errorMessage, QStringLiteral("A selected layer no longer has a valid tree position."));
            return {};
        }
        entries.push_back(entry);
    }
    if (entries.size() < 2) {
        setError(errorMessage, QStringLiteral("Select at least two layers to merge."));
        return {};
    }

    const QUuid parentId = entries.constFirst().parentId;
    for (const Entry &entry : entries) {
        if (entry.parentId != parentId) {
            setError(errorMessage,
                     QStringLiteral("Merge Layers requires contiguous sibling layers inside the same group."));
            return {};
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
        return left.index < right.index;
    });
    for (int i = 1; i < entries.size(); ++i) {
        if (entries.at(i).index != entries.at(i - 1).index + 1) {
            setError(errorMessage,
                     QStringLiteral("Merge Layers requires a contiguous selection so unselected layers do not change stacking position."));
            return {};
        }
    }

    const bool raster = std::all_of(entries.cbegin(), entries.cend(),
                                    [&document](const Entry &entry) {
                                        return rasterCompatible(document.layerById(entry.id));
                                    });
    const bool vector = std::all_of(entries.cbegin(), entries.cend(),
                                    [&document](const Entry &entry) {
                                        return vectorCompatible(document.layerById(entry.id));
                                    });
    if (!raster && !vector) {
        setError(errorMessage,
                 QStringLiteral("Raster layers can merge only with raster layers, and vector layers only with vector layers."));
        return {};
    }

    plan.kind = vector ? LayerMergeKind::Vector : LayerMergeKind::Raster;
    plan.parentId = parentId;
    plan.firstIndex = entries.constFirst().index;
    plan.lastIndex = entries.constLast().index;
    for (const Entry &entry : entries) plan.layerIds.push_back(entry.id);
    return plan;
}

bool LayerMergeOperations::buildMergedLayer(const PhotoDocument &document,
                                            const LayerMergePlan &plan,
                                            LayerNode *mergedLayer,
                                            QString *errorMessage)
{
    if (!mergedLayer || !plan.isValid()) {
        setError(errorMessage, QStringLiteral("The layer merge plan is invalid."));
        return false;
    }
    return plan.kind == LayerMergeKind::Vector
        ? buildVectorLayer(document, plan, mergedLayer, errorMessage)
        : buildRasterLayer(document, plan, mergedLayer, errorMessage);
}

bool LayerMergeOperations::replacePlannedRange(
    const QVector<LayerNode> &sourceLayers,
    const LayerMergePlan &plan,
    const LayerNode &mergedLayer,
    QVector<LayerNode> *resultLayers,
    QString *errorMessage)
{
    if (!resultLayers || !plan.isValid()) {
        setError(errorMessage, QStringLiteral("The layer merge replacement is invalid."));
        return false;
    }
    QVector<LayerNode> result = sourceLayers;
    QVector<LayerNode> *siblings = nullptr;
    if (!findParentChildren(&result, plan.parentId, &siblings) || !siblings
        || plan.firstIndex < 0 || plan.lastIndex >= siblings->size()) {
        setError(errorMessage, QStringLiteral("The destination layer range no longer exists."));
        return false;
    }
    for (int index = 0; index < plan.layerIds.size(); ++index) {
        if (siblings->at(plan.firstIndex + index).id != plan.layerIds.at(index)) {
            setError(errorMessage, QStringLiteral("The selected layer order changed before the merge completed."));
            return false;
        }
    }
    for (int index = plan.lastIndex; index >= plan.firstIndex; --index) {
        siblings->removeAt(index);
    }
    siblings->insert(plan.firstIndex, mergedLayer);
    *resultLayers = std::move(result);
    return true;
}

} // namespace vfx
