#include "gpu/TiledCanvasEngine.h"

#include "CubeLut.h"
#include "ImageProcessor.h"
#include "TonalMapping.h"
#include "SpatialFilter.h"
#include "SmartLayerTileCache.h"
#include "SelectionLocalEditing.h"
#include "VectorRasterizer.h"
#include "gpu/WebGpuContext.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QPainter>
#include <QPolygonF>
#include <QRgba64>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace vfx {
namespace {

constexpr quint64 FnvOffset = 1469598103934665603ULL;
constexpr quint64 FnvPrime = 1099511628211ULL;
constexpr qsizetype MaximumCloneSourcePatchPixels =
    qsizetype(TiledCanvasEngine::TileSize) * TiledCanvasEngine::TileSize * 16;
constexpr qsizetype MaximumClonePendingBytes = qsizetype(256) * 1024 * 1024;

void hashBytes(quint64 &hash, const void *data, const qsizetype size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (qsizetype index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= FnvPrime;
    }
}

template<typename T>
void hashValue(quint64 &hash, const T &value)
{
    hashBytes(hash, &value, sizeof(T));
}

void hashString(quint64 &hash, const QByteArray &bytes)
{
    hashBytes(hash, bytes.constData(), bytes.size());
}

quint64 nonZero(quint64 value)
{
    return value == 0 ? 1 : value;
}

QImage::Format tiledWorkingFormat(const QImage &source)
{
    return source.depth() > 32 ? QImage::Format_RGBA64_Premultiplied
                               : QImage::Format_ARGB32_Premultiplied;
}

QRect sourcePixelsForPreviewTile(const QImage &image,
                                 const QSize &referenceSize,
                                 const QPointF &referenceOrigin,
                                 const QSize &previewSize,
                                 const QTransform &worldTransform,
                                 const QSize &documentSize,
                                 const QRect &previewRect,
                                 const int halo = 2)
{
    if (image.isNull() || referenceSize.isEmpty()
        || previewSize.isEmpty() || documentSize.isEmpty()
        || previewRect.isEmpty()) {
        return {};
    }
    bool invertible = false;
    const QTransform inverse = worldTransform.inverted(&invertible);
    if (!invertible) {
        return {};
    }

    const double previewToDocumentX = documentSize.width()
        / static_cast<double>(std::max(1, previewSize.width()));
    const double previewToDocumentY = documentSize.height()
        / static_cast<double>(std::max(1, previewSize.height()));
    const QRectF documentRect(previewRect.x() * previewToDocumentX,
                              previewRect.y() * previewToDocumentY,
                              previewRect.width() * previewToDocumentX,
                              previewRect.height() * previewToDocumentY);
    const QRectF localDocumentRect = inverse.map(QPolygonF(documentRect)).boundingRect();
    const double imageScaleX = image.width()
        / static_cast<double>(std::max(1, referenceSize.width()));
    const double imageScaleY = image.height()
        / static_cast<double>(std::max(1, referenceSize.height()));
    QRect pixels = QRectF((localDocumentRect.x() - referenceOrigin.x()) * imageScaleX,
                          (localDocumentRect.y() - referenceOrigin.y()) * imageScaleY,
                          localDocumentRect.width() * imageScaleX,
                          localDocumentRect.height() * imageScaleY)
                       .normalized()
                       .adjusted(-halo, -halo, halo, halo)
                       .toAlignedRect();
    return pixels.intersected(image.rect());
}

QSize liveFilterPreviewRadius(const QVector<LiveFilter> &filters,
                              const QSize &previewSize,
                              const QSize &documentSize,
                              const bool includeSafetyPadding)
{
    const QSize documentRadius = liveFilterStackSpatialRadius2D(filters);
    if (documentRadius.isEmpty()) return {};
    const double scaleX = previewSize.width()
        / static_cast<double>(std::max(1, documentSize.width()));
    const double scaleY = previewSize.height()
        / static_cast<double>(std::max(1, documentSize.height()));
    const qint64 maximum = std::numeric_limits<int>::max() / 4;
    const qint64 padding = includeSafetyPadding
        ? SpatialFilterContract::DefaultSafetyPadding : 0;
    const qint64 haloX = std::min<qint64>(
        maximum, static_cast<qint64>(std::ceil(documentRadius.width() * scaleX)) + padding);
    const qint64 haloY = std::min<qint64>(
        maximum, static_cast<qint64>(std::ceil(documentRadius.height() * scaleY)) + padding);
    return QSize(static_cast<int>(haloX), static_cast<int>(haloY));
}

QRect liveFilterDependencyPreviewRect(const LayerNode &layer,
                                      const QSize &previewSize,
                                      const QSize &documentSize,
                                      const QRect &previewRect)
{
    const QSize halo = liveFilterPreviewRadius(
        layer.liveFilters, previewSize, documentSize, true);
    return halo.isEmpty() ? previewRect
        : previewRect.adjusted(-halo.width(), -halo.height(),
                               halo.width(), halo.height());
}

QSize layerEffectPreviewRadius(const QVector<LayerEffect> &effects,
                               const QSize &previewSize,
                               const QSize &documentSize)
{
    const QSize documentRadius = layerEffectStackSpatialRadius2D(effects);
    if (documentRadius.isEmpty()) return {};
    const double scaleX = previewSize.width()
        / static_cast<double>(std::max(1, documentSize.width()));
    const double scaleY = previewSize.height()
        / static_cast<double>(std::max(1, documentSize.height()));
    const qint64 maximum = std::numeric_limits<int>::max() / 4;
    const qint64 haloX = std::min<qint64>(
        maximum, static_cast<qint64>(std::ceil(documentRadius.width() * scaleX)) + 2);
    const qint64 haloY = std::min<qint64>(
        maximum, static_cast<qint64>(std::ceil(documentRadius.height() * scaleY)) + 2);
    return QSize(static_cast<int>(haloX), static_cast<int>(haloY));
}

QRect layerEffectDependencyPreviewRect(const LayerNode &layer,
                                       const QSize &previewSize,
                                       const QSize &documentSize,
                                       const QRect &previewRect)
{
    // Empty identity means every entry is disabled or definition-only and thus
    // contributes no pixels. Do not widen dirty dependencies in that case.
    if (layerEffectStackRenderIdentity(layer.layerEffects).isEmpty()) return previewRect;
    const QSize halo = layerEffectPreviewRadius(layer.layerEffects, previewSize, documentSize);
    return halo.isEmpty() ? previewRect
        : previewRect.adjusted(-halo.width(), -halo.height(),
                               halo.width(), halo.height());
}

bool layerEffectUsesGlobalOwnerBounds(const QVector<LayerEffect> &effects)
{
    return std::any_of(effects.cbegin(), effects.cend(), [](const LayerEffect &effect) {
        return effect.enabled && layerEffectTypeHasRenderer(effect.type)
            && effect.effectOpacity > 0.0
            && effect.type == LayerEffectType::GradientOverlay;
    });
}

QRectF rasterReferencePreviewBounds(const QImage &image,
                                    const QSize &referenceSize,
                                    const QPointF &referenceOrigin,
                                    const QSize &previewSize,
                                    const QTransform &worldTransform,
                                    const QSize &documentSize)
{
    if (image.isNull() || previewSize.isEmpty() || documentSize.isEmpty()) return {};
    const QSize effectiveReference = referenceSize.isValid() && !referenceSize.isEmpty()
        ? referenceSize : documentSize;
    const QRectF localBounds(referenceOrigin, QSizeF(effectiveReference));
    const QTransform documentToPreview = QTransform::fromScale(
        previewSize.width() / static_cast<double>(std::max(1, documentSize.width())),
        previewSize.height() / static_cast<double>(std::max(1, documentSize.height())));
    return documentToPreview.mapRect(worldTransform.mapRect(localBounds));
}

quint64 liveFilterStackFingerprint(const QVector<LiveFilter> &filters)
{
    quint64 hash = FnvOffset;
    hashValue(hash, filters.size());
    for (const LiveFilter &filter : filters) {
        hashValue(hash, filter.enabled);
        if (!filter.enabled) continue;
        const QByteArray identity = adjustmentRenderIdentity(filter.adjustment);
        hashValue(hash, !identity.isEmpty());
        if (!identity.isEmpty()) hashString(hash, identity);
        hashValue(hash, filter.maskEnabled);
        hashValue(hash, filter.maskInverted);
        hashValue(hash, static_cast<qint64>(filter.maskImage.cacheKey()));
        hashValue(hash, filter.maskReferenceSize.width());
        hashValue(hash, filter.maskReferenceSize.height());
        hashValue(hash, filter.maskReferenceOrigin.x());
        hashValue(hash, filter.maskReferenceOrigin.y());
    }
    return nonZero(hash);
}

QByteArray appendLiveFilterIdentity(QByteArray key, const QVector<LiveFilter> &filters)
{
    key += QByteArrayLiteral("/live-filters-v1/");
    for (const LiveFilter &filter : filters) {
        key += filter.enabled ? QByteArrayLiteral("1:") : QByteArrayLiteral("0:");
        if (filter.enabled) {
            const QByteArray identity = adjustmentRenderIdentity(filter.adjustment);
            if (identity.isEmpty()) return {};
            key += identity;
            QByteArray maskIdentity;
            QDataStream maskStream(&maskIdentity, QIODevice::WriteOnly);
            maskStream.setVersion(QDataStream::Qt_6_0);
            maskStream << filter.maskEnabled << filter.maskInverted
                       << static_cast<qint64>(filter.maskImage.cacheKey())
                       << filter.maskReferenceSize << filter.maskReferenceOrigin;
            key.append(maskIdentity);
        }
        key += '\0';
    }
    return QCryptographicHash::hash(key, QCryptographicHash::Sha256);
}

QRect smartSourcePixelsForPreviewTile(const LayerNode &layer,
                                      const QSize &previewSize,
                                      const QTransform &worldTransform,
                                      const QSize &documentSize,
                                      const QRect &previewRect)
{
    const QSize referenceSize = layer.smartPresentationReferenceSize.isValid()
            && !layer.smartPresentationReferenceSize.isEmpty()
        ? layer.smartPresentationReferenceSize
        : layer.smartPresentationImage.size();
    const int halo = layer.smartTransform.interpolation == TransformInterpolation::Lanczos3
        ? 4
        : (layer.smartTransform.interpolation == TransformInterpolation::Bicubic
               ? 3
               : (layer.smartTransform.interpolation == TransformInterpolation::Bilinear ? 2 : 1));
    const QRect dependencyRect = liveFilterDependencyPreviewRect(
        layer, previewSize, documentSize, previewRect);
    return sourcePixelsForPreviewTile(layer.smartPresentationImage,
                                      referenceSize,
                                      layer.smartPresentationReferenceOrigin,
                                      previewSize,
                                      worldTransform,
                                      documentSize,
                                      dependencyRect,
                                      halo);
}

void collectPreparedResidentKeys(const QVector<PreparedTileLayer> &layers,
                                QSet<quint64> *keys)
{
    if (!keys) return;
    for (const PreparedTileLayer &layer : layers) {
        if (layer.residentTileKey != 0) keys->insert(layer.residentTileKey);
        if (!layer.children.isEmpty()) collectPreparedResidentKeys(layer.children, keys);
    }
}

QUuid smartIntermediateResidentSurfaceId()
{
    static const QUuid id(QStringLiteral("{715f90b3-cd6c-4b70-a64f-0140e0000001}"));
    return id;
}

quint64 layerEffectResidentTileKey(const LayerNode &layer,
                                   const LayerEffectRenderPass &pass,
                                   const QRect &tileRect)
{
    quint64 hash = FnvOffset;
    hashString(hash, QByteArrayLiteral("layer-effect-resident-v2"));
    hashString(hash, layer.id.toRfc4122());
    hashString(hash, pass.effectId.toRfc4122());
    hashValue(hash, static_cast<quint64>(pass.image.cacheKey()));
    hashValue(hash, tileRect.x());
    hashValue(hash, tileRect.y());
    hashValue(hash, tileRect.width());
    hashValue(hash, tileRect.height());
    hashValue(hash, static_cast<int>(pass.blendMode));
    hashValue(hash, pass.opacity);
    return nonZero(hash);
}

void hashImageRegion(quint64 &hash,
                     const QImage &image,
                     const QSize &referenceSize,
                     const QPointF &referenceOrigin,
                     const QSize &previewSize,
                     const QTransform &worldTransform,
                     const QSize &documentSize,
                     const QRect &previewRect,
                     const int halo = 2)
{
    const bool isNull = image.isNull();
    hashValue(hash, isNull);
    if (isNull) {
        return;
    }
    const QSize effectiveReference = referenceSize.isValid() && !referenceSize.isEmpty()
        ? referenceSize : documentSize;
    hashValue(hash, referenceOrigin.x());
    hashValue(hash, referenceOrigin.y());
    const QRect pixels = sourcePixelsForPreviewTile(image,
                                                    effectiveReference,
                                                    referenceOrigin,
                                                    previewSize,
                                                    worldTransform,
                                                    documentSize,
                                                    previewRect,
                                                    halo);
    hashValue(hash, pixels.x());
    hashValue(hash, pixels.y());
    hashValue(hash, pixels.width());
    hashValue(hash, pixels.height());
    hashValue(hash, image.width());
    hashValue(hash, image.height());
    hashValue(hash, static_cast<int>(image.format()));
    if (pixels.isEmpty()) {
        return;
    }

    const QImage cropped = image.copy(pixels).convertToFormat(
        image.depth() > 32 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    for (int y = 0; y < cropped.height(); ++y) {
        hashBytes(hash, cropped.constScanLine(y), cropped.bytesPerLine());
    }
}

void hashTransform(quint64 &hash, const QTransform &transform)
{
    const double values[] = {transform.m11(), transform.m12(), transform.m13(),
                             transform.m21(), transform.m22(), transform.m23(),
                             transform.m31(), transform.m32(), transform.m33()};
    hashBytes(hash, values, sizeof(values));
}

void hashLayerTree(quint64 &hash,
                   const QImage &source,
                   const QVector<LayerNode> &layers,
                   const QRect &tileRect,
                   const QSize &documentSize,
                   const QTransform &parentTransform)
{
    hashValue(hash, layers.size());
    for (const LayerNode &layer : layers) {
        hashString(hash, layer.id.toRfc4122());
        hashValue(hash, static_cast<int>(layer.type));
        hashValue(hash, layer.visible);
        hashValue(hash, layer.opacity);
        hashValue(hash, static_cast<int>(layer.blendMode));
        hashValue(hash, static_cast<int>(layer.groupCompositeMode));
        hashValue(hash, layer.maskEnabled);
        hashValue(hash, layer.maskInverted);
        const QTransform worldTransform = layer.transform * parentTransform;
        // Vector geometry and Feather revisions are tile-local. A transform or
        // geometry edit wholly outside this tile must not evict its composite.
        // The current contribution flag below still changes when geometry moves
        // into or out of the tile, and contributing vectors hash the full world
        // transform. Other layer types retain their established global transform
        // revision behaviour.
        if (layer.type != LayerType::Vector && layer.type != LayerType::Smart) {
            hashTransform(hash, layer.transform);
        }

        if (!layer.visible || layer.opacity <= 0.0) {
            continue;
        }

        const QByteArray effectIdentity = layerEffectStackRenderIdentity(layer.layerEffects);
        const QRect effectDependencyRect = effectIdentity.isEmpty()
            ? tileRect
            : layerEffectDependencyPreviewRect(layer, source.size(), documentSize, tileRect);
        bool effectMayContribute = false;

        switch (layer.type) {
        case LayerType::BaseImage: {
            const QImage &basePixels = layer.rasterImage.isNull() ? source : layer.rasterImage;
            hashImageRegion(hash, basePixels, layer.rasterReferenceSize, layer.rasterReferenceOrigin,
                            source.size(), worldTransform, documentSize, effectDependencyRect);
            effectMayContribute = !effectIdentity.isEmpty()
                && rasterReferencePreviewBounds(basePixels, layer.rasterReferenceSize,
                                                layer.rasterReferenceOrigin, source.size(),
                                                worldTransform, documentSize)
                       .intersects(QRectF(effectDependencyRect));
            break;
        }
        case LayerType::Raster:
            hashImageRegion(hash,
                            layer.rasterImage,
                            layer.rasterReferenceSize,
                            layer.rasterReferenceOrigin,
                            source.size(),
                            worldTransform,
                            documentSize,
                            effectDependencyRect);
            effectMayContribute = !effectIdentity.isEmpty()
                && rasterReferencePreviewBounds(layer.rasterImage, layer.rasterReferenceSize,
                                                layer.rasterReferenceOrigin, source.size(),
                                                worldTransform, documentSize)
                       .intersects(QRectF(effectDependencyRect));
            break;
        case LayerType::Vector: {
            const QTransform documentToPreview = QTransform::fromScale(
                source.width() / static_cast<double>(std::max(1, documentSize.width())),
                source.height() / static_cast<double>(std::max(1, documentSize.height())));
            const QRectF vectorBounds = documentToPreview.mapRect(
                VectorRasterizer::contentBounds(layer, worldTransform));
            const bool contributes = !vectorBounds.isEmpty()
                && vectorBounds.intersects(QRectF(effectDependencyRect));
            effectMayContribute = !effectIdentity.isEmpty() && contributes;
            hashValue(hash, contributes);
            if (contributes) {
                hashTransform(hash, worldTransform);
                hashValue(hash, layer.vectorData.fingerprint());
                hashValue(hash, layer.revision);
            }
            break;
        }
        case LayerType::Text:
            hashValue(hash, layer.textData.fingerprint());
            hashValue(hash, layer.revision);
            // Text rendering was already conservatively revision-addressed for
            // every tile. Preserve that contract for fx until a cheap exact text
            // bounds query is shared with the rasterizer.
            effectMayContribute = !effectIdentity.isEmpty();
            break;
        case LayerType::Smart: {
            const QSize referenceSize = layer.smartPresentationReferenceSize.isValid()
                && !layer.smartPresentationReferenceSize.isEmpty()
                ? layer.smartPresentationReferenceSize
                : layer.smartPresentationImage.size();
            const QRectF localBounds(layer.smartPresentationReferenceOrigin,
                                     QSizeF(referenceSize));
            const QTransform documentToPreview = QTransform::fromScale(
                source.width() / static_cast<double>(std::max(1, documentSize.width())),
                source.height() / static_cast<double>(std::max(1, documentSize.height())));
            QRectF smartBounds = documentToPreview.mapRect(
                worldTransform.mapRect(localBounds));
            const QSize liveRadius = liveFilterPreviewRadius(
                layer.liveFilters, source.size(), documentSize, false);
            if (!smartBounds.isEmpty() && !liveRadius.isEmpty()) {
                smartBounds.adjust(-liveRadius.width(), -liveRadius.height(),
                                   liveRadius.width(), liveRadius.height());
            }
            const bool contributes = !layer.smartPresentationImage.isNull()
                && !smartBounds.isEmpty()
                && smartBounds.intersects(QRectF(effectDependencyRect));
            effectMayContribute = !effectIdentity.isEmpty() && contributes;
            hashValue(hash, contributes);
            if (contributes) {
                hashTransform(hash, worldTransform);
                hashString(hash, layer.smartSource.sourceId.toRfc4122());
                hashValue(hash, layer.smartPresentationImage.width());
                hashValue(hash, layer.smartPresentationImage.height());
                hashValue(hash, static_cast<int>(layer.smartPresentationImage.format()));
                hashValue(hash, referenceSize.width());
                hashValue(hash, referenceSize.height());
                hashValue(hash, layer.smartPresentationReferenceOrigin.x());
                hashValue(hash, layer.smartPresentationReferenceOrigin.y());
                hashValue(hash, static_cast<int>(layer.smartTransform.interpolation));
                hashValue(hash, liveFilterStackFingerprint(layer.liveFilters));
                const QRect sourcePixels = smartSourcePixelsForPreviewTile(
                    layer, source.size(), worldTransform, documentSize, effectDependencyRect);
                hashValue(hash, sourcePixels.x());
                hashValue(hash, sourcePixels.y());
                hashValue(hash, sourcePixels.width());
                hashValue(hash, sourcePixels.height());
                const quint64 sourceFingerprint = SmartLayerTileCache::instance()
                    .sourceRegionFingerprint(layer.smartPresentationImage,
                                             layer.smartSource.sourceId,
                                             layer.smartSource.observedSourceRevision,
                                             sourcePixels);
                hashValue(hash, sourceFingerprint);
            }
            break;
        }
        case LayerType::Adjustment: {
            const AdjustmentData data = layer.effectiveAdjustmentData();
            hashValue(hash, static_cast<int>(data.type));
            switch (data.type) {
            case AdjustmentType::Exposure: {
                const auto &parameters = std::get<ExposureParameters>(data.parameters);
                hashValue(hash, parameters.exposure);
                hashValue(hash, parameters.offset);
                hashValue(hash, parameters.gamma);
                break;
            }
            case AdjustmentType::Contrast: {
                const auto &parameters = std::get<ContrastParameters>(data.parameters);
                hashValue(hash, parameters.contrast);
                hashValue(hash, parameters.pivot);
                break;
            }
            case AdjustmentType::Saturation:
                hashValue(hash, std::get<SaturationParameters>(data.parameters).saturation);
                break;
            case AdjustmentType::Levels: {
                const LevelsParameters &levels = std::get<LevelsParameters>(data.parameters);
                for (int channelIndex = 0; channelIndex < 4; ++channelIndex) {
                    const auto &channel = levels.channel(static_cast<AdjustmentChannel>(channelIndex));
                    hashValue(hash, channel.inputBlack);
                    hashValue(hash, channel.inputWhite);
                    hashValue(hash, channel.gamma);
                    hashValue(hash, channel.outputBlack);
                    hashValue(hash, channel.outputWhite);
                }
                break;
            }
            case AdjustmentType::Curves: {
                const CurvesParameters &curves = std::get<CurvesParameters>(data.parameters);
                hashValue(hash, static_cast<int>(curves.interpolation));
                for (int channelIndex = 0; channelIndex < 4; ++channelIndex) {
                    const auto &points = curves.channel(static_cast<AdjustmentChannel>(channelIndex)).points;
                    hashValue(hash, points.size());
                    for (const CurvePoint &point : points) {
                        hashValue(hash, point.input);
                        hashValue(hash, point.output);
                    }
                }
                break;
            }
            case AdjustmentType::HueSaturation: {
                const auto &parameters = std::get<HueSaturationParameters>(data.parameters);
                hashValue(hash, parameters.hue);
                hashValue(hash, parameters.saturation);
                hashValue(hash, parameters.lightness);
                for (const auto &range : parameters.ranges) {
                    hashValue(hash, range.hue);
                    hashValue(hash, range.saturation);
                    hashValue(hash, range.lightness);
                    hashValue(hash, range.centre);
                    hashValue(hash, range.width);
                    hashValue(hash, range.feather);
                }
                break;
            }
            case AdjustmentType::Vibrance: {
                const auto &parameters = std::get<VibranceParameters>(data.parameters);
                hashValue(hash, parameters.vibrance);
                hashValue(hash, parameters.saturation);
                hashValue(hash, parameters.skinProtection);
                break;
            }
            case AdjustmentType::WhiteBalance: {
                const auto &parameters = std::get<WhiteBalanceParameters>(data.parameters);
                hashValue(hash, parameters.temperature);
                hashValue(hash, parameters.tint);
                break;
            }
            case AdjustmentType::ColourBalance: {
                const auto &parameters = std::get<ColourBalanceParameters>(data.parameters);
                hashValue(hash, parameters.preserveLuminosity);
                for (const auto &range : parameters.ranges) {
                    hashValue(hash, range.cyanRed);
                    hashValue(hash, range.magentaGreen);
                    hashValue(hash, range.yellowBlue);
                }
                break;
            }
            case AdjustmentType::ChannelMixer: {
                const auto &parameters = std::get<ChannelMixerParameters>(data.parameters);
                hashValue(hash, parameters.monochromeEnabled);
                for (const auto &output : parameters.outputs) {
                    hashValue(hash, output.red);
                    hashValue(hash, output.green);
                    hashValue(hash, output.blue);
                    hashValue(hash, output.constant);
                }
                hashValue(hash, parameters.monochrome.red);
                hashValue(hash, parameters.monochrome.green);
                hashValue(hash, parameters.monochrome.blue);
                hashValue(hash, parameters.monochrome.constant);
                break;
            }
            case AdjustmentType::BlackAndWhite: {
                const auto &parameters = std::get<BlackAndWhiteParameters>(data.parameters);
                for (const double weight : parameters.colourWeights) hashValue(hash, weight);
                hashValue(hash, parameters.tintEnabled);
                hashValue(hash, parameters.tintHue);
                hashValue(hash, parameters.tintSaturation);
                break;
            }
            case AdjustmentType::GradientMap: {
                const auto &parameters = std::get<GradientMapParameters>(data.parameters);
                hashValue(hash, parameters.reverse);
                hashValue(hash, static_cast<int>(parameters.interpolation));
                hashValue(hash, parameters.stops.size());
                for (const GradientStop &stop : parameters.stops) {
                    hashValue(hash, stop.position);
                    hashValue(hash, stop.colour.rgba());
                }
                break;
            }
            case AdjustmentType::Posterise: {
                hashValue(hash, std::get<PosteriseParameters>(data.parameters).levels);
                break;
            }
            case AdjustmentType::Threshold: {
                const auto &parameters = std::get<ThresholdParameters>(data.parameters);
                hashValue(hash, parameters.threshold);
                hashValue(hash, static_cast<int>(parameters.source));
                break;
            }
            case AdjustmentType::Invert:
                break;
            case AdjustmentType::PhotoFilter: {
                const auto &parameters = std::get<PhotoFilterParameters>(data.parameters);
                hashValue(hash, parameters.colour.rgba());
                hashValue(hash, parameters.density);
                hashValue(hash, parameters.preserveLuminosity);
                break;
            }
            case AdjustmentType::SelectiveColour: {
                const auto &parameters = std::get<SelectiveColourParameters>(data.parameters);
                hashValue(hash, static_cast<int>(parameters.method));
                for (const auto &range : parameters.ranges) {
                    hashValue(hash, range.cyan);
                    hashValue(hash, range.magenta);
                    hashValue(hash, range.yellow);
                    hashValue(hash, range.black);
                }
                break;
            }
            case AdjustmentType::Vignette: {
                const auto &parameters = std::get<VignetteParameters>(data.parameters);
                hashValue(hash, parameters.amount);
                hashValue(hash, parameters.size);
                hashValue(hash, parameters.midpoint);
                hashValue(hash, parameters.roundness);
                hashValue(hash, parameters.feather);
                hashValue(hash, parameters.centreX);
                hashValue(hash, parameters.centreY);
                hashValue(hash, parameters.rotation);
                hashValue(hash, parameters.highlightProtection);
                hashValue(hash, parameters.inverted);
                hashValue(hash, parameters.tintEnabled);
                hashValue(hash, parameters.tint.rgba());
                break;
            }
            case AdjustmentType::RgbSplit: {
                const auto &parameters = std::get<RgbSplitParameters>(data.parameters);
                hashValue(hash, parameters.redOffsetX);
                hashValue(hash, parameters.redOffsetY);
                hashValue(hash, parameters.blueOffsetX);
                hashValue(hash, parameters.blueOffsetY);
                break;
            }
            case AdjustmentType::ChromaticAberrationCorrection: {
                const auto &parameters =
                    std::get<ChromaticAberrationCorrectionParameters>(data.parameters);
                hashValue(hash, parameters.redEdgeShift);
                hashValue(hash, parameters.blueEdgeShift);
                hashValue(hash, parameters.centreX);
                hashValue(hash, parameters.centreY);
                hashValue(hash, parameters.falloff);
                break;
            }
            case AdjustmentType::SurfaceBlur: {
                const auto &parameters = std::get<SurfaceBlurParameters>(data.parameters);
                hashValue(hash, parameters.radius);
                hashValue(hash, parameters.threshold);
                break;
            }
            case AdjustmentType::MotionBlur: {
                const auto &parameters = std::get<MotionBlurParameters>(data.parameters);
                hashValue(hash, parameters.distance);
                hashValue(hash, parameters.angle);
                hashValue(hash, parameters.samples);
                hashValue(hash, parameters.affectAlpha);
                break;
            }
            case AdjustmentType::RadialBlur: {
                const auto &parameters = std::get<RadialBlurParameters>(data.parameters);
                hashValue(hash, static_cast<int>(parameters.mode));
                hashValue(hash, parameters.amount);
                hashValue(hash, parameters.centreX);
                hashValue(hash, parameters.centreY);
                hashValue(hash, parameters.samples);
                hashValue(hash, parameters.affectAlpha);
                break;
            }
            case AdjustmentType::Lut: {
                const auto &parameters = std::get<LutParameters>(data.parameters);
                hashValue(hash, parameters.strength);
                hashValue(hash, static_cast<int>(parameters.interpolation));
                hashValue(hash, static_cast<int>(parameters.processingMode));
                hashValue(hash, static_cast<int>(parameters.operatorProfile));
                hashValue(hash, parameters.shaperSize);
                hashValue(hash, parameters.cubeSize);
                for (const double value : parameters.shaperDomainMin) hashValue(hash, value);
                for (const double value : parameters.shaperDomainMax) hashValue(hash, value);
                for (const double value : parameters.cubeDomainMin) hashValue(hash, value);
                for (const double value : parameters.cubeDomainMax) hashValue(hash, value);
                hashValue(hash, parameters.shaperData.size());
                hashValue(hash, parameters.cubeData.size());
                hashValue(hash, parameters.tableFingerprint);
                break;
            }
            case AdjustmentType::ShadowsHighlights: {
                const auto &parameters = std::get<ShadowsHighlightsParameters>(data.parameters);
                hashValue(hash, parameters.shadowAmount);
                hashValue(hash, parameters.shadowTonalWidth);
                hashValue(hash, parameters.highlightAmount);
                hashValue(hash, parameters.highlightTonalWidth);
                hashValue(hash, parameters.radius);
                hashValue(hash, parameters.midtoneContrast);
                hashValue(hash, parameters.colourCorrection);
                break;
            }
            case AdjustmentType::GaussianBlur: {
                const auto &parameters = std::get<GaussianBlurParameters>(data.parameters);
                hashValue(hash, parameters.radius);
                hashValue(hash, parameters.affectAlpha);
                break;
            }
            case AdjustmentType::BoxBlur: {
                const auto &parameters = std::get<BoxBlurParameters>(data.parameters);
                hashValue(hash, parameters.radius);
                hashValue(hash, parameters.affectAlpha);
                break;
            }
            case AdjustmentType::UnsharpMask: {
                const auto &parameters = std::get<UnsharpMaskParameters>(data.parameters);
                hashValue(hash, parameters.radius);
                hashValue(hash, parameters.amount);
                hashValue(hash, parameters.threshold);
                break;
            }
            case AdjustmentType::HighPass: {
                const auto &parameters = std::get<HighPassParameters>(data.parameters);
                hashValue(hash, parameters.radius);
                hashValue(hash, parameters.monochrome);
                break;
            }
            }
            break;
        }
        case LayerType::Group:
            hashLayerTree(hash,
                          source,
                          layer.children,
                          tileRect,
                          documentSize,
                          worldTransform);
            break;
        }
        if (effectMayContribute && !effectIdentity.isEmpty()) {
            hashString(hash, effectIdentity);
            // Gradient Overlay coordinates are anchored to the owner's complete
            // effective coverage bounds. A distant owner edit can therefore move
            // the gradient origin/span even when this tile's local source pixels
            // are unchanged. Conservatively include the owner's global identity
            // only for that effect; local shadows/glows/strokes/Colour Overlay
            // retain the selective region hashing above.
            if (layerEffectUsesGlobalOwnerBounds(layer.layerEffects)) {
                hashValue(hash, layer.revision);
                hashValue(hash, static_cast<quint64>(layer.rasterImage.cacheKey()));
                hashValue(hash, static_cast<quint64>(layer.maskImage.cacheKey()));
                hashValue(hash, layer.maskReferenceSize.width());
                hashValue(hash, layer.maskReferenceSize.height());
                hashValue(hash, layer.maskReferenceOrigin.x());
                hashValue(hash, layer.maskReferenceOrigin.y());
                hashValue(hash, static_cast<quint64>(layer.vectorData.fingerprint()));
                hashValue(hash, static_cast<quint64>(layer.textData.fingerprint()));
                hashValue(hash, static_cast<quint64>(layer.smartPresentationImage.cacheKey()));
                hashString(hash, layer.smartSource.sourceId.toRfc4122());
                hashValue(hash, layer.smartSource.observedSourceRevision);
                hashValue(hash, liveFilterStackFingerprint(layer.liveFilters));
                if (layer.type == LayerType::BaseImage && layer.rasterImage.isNull()) {
                    hashValue(hash, static_cast<quint64>(source.cacheKey()));
                }
            }
        }
        const int maskHalo = layer.type == LayerType::Smart
            ? (layer.smartTransform.interpolation == TransformInterpolation::Lanczos3
                   ? 4
                   : (layer.smartTransform.interpolation == TransformInterpolation::Bicubic
                          ? 3
                          : (layer.smartTransform.interpolation == TransformInterpolation::Bilinear ? 2 : 1)))
            : 2;
        hashImageRegion(hash,
                        layer.maskImage,
                        layer.maskReferenceSize,
                        layer.maskReferenceOrigin,
                        source.size(),
                        worldTransform,
                        documentSize,
                        effectDependencyRect,
                        maskHalo);
    }
}

struct HierarchySummary {
    int passThroughGroups = 0;
    int isolatedGroups = 0;
    int maximumDepth = 0;
    int visibleNodes = 0;
    qsizetype estimatedTextureCount = 1; // shared transparent input
};

void analyseHierarchy(const QVector<LayerNode> &layers, HierarchySummary *summary)
{
    if (!summary) {
        return;
    }
    struct PendingStack {
        const QVector<LayerNode> *layers = nullptr;
        int depth = 0;
    };
    QVector<PendingStack> pending;
    pending.push_back({&layers, 1});
    while (!pending.isEmpty()) {
        const PendingStack current = pending.takeLast();
        if (!current.layers) {
            continue;
        }
        for (const LayerNode &layer : *current.layers) {
            if (!layer.visible || layer.opacity <= 0.0) {
                continue;
            }
            ++summary->visibleNodes;
            if (layer.maskEnabled && !layer.maskImage.isNull()) {
                ++summary->estimatedTextureCount;
            }
            for (const LayerEffect &effect : layer.layerEffects) {
                if (effect.enabled && layerEffectTypeHasRenderer(effect.type)) {
                    // Generated fx image plus the composited output that follows it.
                    summary->estimatedTextureCount += 2;
                }
            }
            if (layer.type == LayerType::Adjustment) {
                ++summary->estimatedTextureCount;
                continue;
            }
            if (layer.type != LayerType::Group) {
                summary->estimatedTextureCount += 2; // uploaded image + composite output
                continue;
            }
            ++summary->estimatedTextureCount; // isolated composite or Pass Through mix
            if (layer.groupCompositeMode == GroupCompositeMode::PassThrough) {
                ++summary->passThroughGroups;
            } else {
                ++summary->isolatedGroups;
            }
            summary->maximumDepth = std::max(summary->maximumDepth, current.depth);
            pending.push_back({&layer.children, current.depth + 1});
        }
    }
}


QUuid compositeSurfaceId(const QImage &source, const QSize &documentSize)
{
    QCryptographicHash hash(QCryptographicHash::Md5);
    const quint64 key = source.cacheKey();
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(&key), sizeof(key)));
    const int values[] = {source.width(), source.height(), documentSize.width(), documentSize.height(),
                          static_cast<int>(source.format())};
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(values), sizeof(values)));
    return QUuid::fromRfc4122(hash.result());
}

quint64 tileResidencyKey(const TileAddress &address)
{
    quint64 hash = FnvOffset;
    hashString(hash, address.documentSessionId.toRfc4122());
    hashString(hash, address.surfaceId.toRfc4122());
    hashValue(hash, address.x);
    hashValue(hash, address.y);
    hashValue(hash, address.level);
    hashValue(hash, static_cast<quint8>(address.domain));
    return nonZero(hash);
}

QVector<QPointF> strokeStampPoints(const QVector<QLineF> &documentSegments,
                                   const QTransform &documentToLayer,
                                   const double spacing)
{
    QVector<QPointF> stamps;
    for (const QLineF &line : documentSegments) {
        const double length = line.length();
        if (length <= 1.0e-6) {
            stamps.push_back(documentToLayer.map(line.p1()));
            continue;
        }
        const int steps = std::max(1, static_cast<int>(std::ceil(length / spacing)));
        stamps.reserve(stamps.size() + steps);
        for (int step = 1; step <= steps; ++step) {
            const double amount = step / static_cast<double>(steps);
            stamps.push_back(documentToLayer.map(line.p1() + (line.p2() - line.p1()) * amount));
        }
    }
    return stamps;
}

QRect stampBounds(const QVector<QPointF> &stamps, const double radius, const QSize &size)
{
    if (stamps.isEmpty() || size.isEmpty()) {
        return {};
    }
    QRectF bounds(stamps.constFirst() - QPointF(radius, radius), QSizeF(radius * 2.0, radius * 2.0));
    for (const QPointF &point : stamps) {
        bounds = bounds.united(QRectF(point - QPointF(radius, radius),
                                     QSizeF(radius * 2.0, radius * 2.0)));
    }
    return bounds.adjusted(-2.0, -2.0, 2.0, 2.0)
        .toAlignedRect()
        .intersected(QRect(QPoint(0, 0), size));
}


QRect cloneStampBounds(const QVector<QPointF> &stamps,
                       const double radius,
                       const QSize &size)
{
    if (stamps.isEmpty() || size.isEmpty()) {
        return {};
    }
    const QRect extent(QPoint(0, 0), size);
    QRect total;
    for (const QPointF &stamp : stamps) {
        const QRect affected = QRectF(stamp - QPointF(radius, radius),
                                      QSizeF(radius * 2.0, radius * 2.0))
                                   .adjusted(-1.0, -1.0, 1.0, 1.0)
                                   .toAlignedRect()
                                   .intersected(extent);
        if (!affected.isEmpty()) {
            total = total.isEmpty() ? affected : total.united(affected);
        }
    }
    return total;
}


QImage materialisedMask(const QImage &sourceMask, const QSize &documentSize)
{
    if (sourceMask.isNull() || documentSize.isEmpty()) {
        return {};
    }
    if (sourceMask.size() == QSize(1, 1)) {
        QImage mask(documentSize, QImage::Format_Grayscale8);
        mask.fill(qGray(sourceMask.pixel(0, 0)));
        return mask;
    }
    QImage mask = sourceMask;
    if (mask.size() != documentSize) {
        mask = mask.scaled(documentSize,
                           Qt::IgnoreAspectRatio,
                           Qt::SmoothTransformation);
    }
    return mask.convertToFormat(QImage::Format_Grayscale8);
}

QImage opaqueGreyscaleRgba(const QImage &mask)
{
    if (mask.isNull()) {
        return {};
    }
    const QImage grey = mask.convertToFormat(QImage::Format_Grayscale8);
    QImage rgba(grey.size(), QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < grey.height(); ++y) {
        const uchar *source = grey.constScanLine(y);
        uchar *target = rgba.scanLine(y);
        for (int x = 0; x < grey.width(); ++x) {
            const uchar value = source[x];
            target[x * 4] = value;
            target[x * 4 + 1] = value;
            target[x * 4 + 2] = value;
            target[x * 4 + 3] = 255;
        }
    }
    return rgba;
}

QImage stampMaskTileCpu(const QImage &sourceTile,
                        const QPoint &tileOrigin,
                        const QVector<QPointF> &stampPoints,
                        const double radius,
                        const double hardness,
                        const double opacity,
                        const int value)
{
    const QImage source = sourceTile.convertToFormat(QImage::Format_Grayscale8);
    if (source.isNull()) {
        return {};
    }
    const int width = source.width();
    const int height = source.height();
    QVector<float> values(width * height);
    for (int y = 0; y < height; ++y) {
        const uchar *row = source.constScanLine(y);
        for (int x = 0; x < width; ++x) {
            values[y * width + x] = row[x] / 255.0f;
        }
    }

    const double safeRadius = std::max(0.5, radius);
    const double clampedHardness = std::clamp(hardness, 0.0, 0.9999);
    const double clampedOpacity = std::clamp(opacity, 0.0, 1.0);
    const double paintValue = std::clamp(value, 0, 255) / 255.0;
    const QRect tileRect(tileOrigin, source.size());

    for (const QPointF &stamp : stampPoints) {
        const QRect affected = QRectF(stamp - QPointF(safeRadius, safeRadius),
                                      QSizeF(safeRadius * 2.0, safeRadius * 2.0))
                                   .adjusted(-1.0, -1.0, 1.0, 1.0)
                                   .toAlignedRect()
                                   .intersected(tileRect);
        for (int documentY = affected.top(); documentY <= affected.bottom(); ++documentY) {
            for (int documentX = affected.left(); documentX <= affected.right(); ++documentX) {
                const QPointF pixelCentre(documentX + 0.5, documentY + 0.5);
                const double normalisedDistance = QLineF(pixelCentre, stamp).length() / safeRadius;
                double smooth = 0.0;
                if (normalisedDistance <= clampedHardness) {
                    smooth = 0.0;
                } else if (normalisedDistance >= 1.0) {
                    smooth = 1.0;
                } else {
                    const double t = (normalisedDistance - clampedHardness)
                        / (1.0 - clampedHardness);
                    smooth = t * t * (3.0 - 2.0 * t);
                }
                const double amount = std::clamp((1.0 - smooth) * clampedOpacity, 0.0, 1.0);
                if (amount <= 0.0) {
                    continue;
                }
                const int localX = documentX - tileOrigin.x();
                const int localY = documentY - tileOrigin.y();
                float &current = values[localY * width + localX];
                current = static_cast<float>(paintValue * amount + current * (1.0 - amount));
            }
        }
    }

    QImage tile(source.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y) {
        uchar *row = tile.scanLine(y);
        for (int x = 0; x < width; ++x) {
            row[x] = static_cast<uchar>(std::clamp(qRound(values[y * width + x] * 255.0f),
                                                   0,
                                                   255));
        }
    }
    return tile;
}


QImage stampRasterTileCpu(const QImage &sourceTile,
                          const QPoint &tileOrigin,
                          const QVector<QPointF> &stampPoints,
                          const double radius,
                          const double hardness,
                          const double opacity,
                          const QColor &colour,
                          const bool erasing)
{
    if (sourceTile.isNull()) {
        return {};
    }
    const bool sixteenBit = sourceTile.depth() > 32;
    QImage output = sourceTile.convertToFormat(sixteenBit
                                                  ? QImage::Format_RGBA64
                                                  : QImage::Format_RGBA8888);
    if (output.isNull()) {
        return {};
    }
    output.detach();
    const double safeRadius = std::max(0.5, radius);
    const double clampedHardness = std::clamp(hardness, 0.0, 0.9999);
    const double clampedOpacity = std::clamp(opacity, 0.0, 1.0);
    const double colourAlpha = std::clamp(static_cast<double>(colour.alphaF()), 0.0, 1.0);
    const double paintRgb[3] {std::clamp(static_cast<double>(colour.redF()), 0.0, 1.0),
                              std::clamp(static_cast<double>(colour.greenF()), 0.0, 1.0),
                              std::clamp(static_cast<double>(colour.blueF()), 0.0, 1.0)};
    const QRect tileRect(tileOrigin, output.size());

    const auto stampAmount = [&](const int documentX,
                                 const int documentY,
                                 const QPointF &stamp) {
        const double dx = documentX + 0.5 - stamp.x();
        const double dy = documentY + 0.5 - stamp.y();
        const double normalisedDistance = std::sqrt(dx * dx + dy * dy) / safeRadius;
        double smooth = 0.0;
        if (normalisedDistance <= clampedHardness) {
            smooth = 0.0;
        } else if (normalisedDistance >= 1.0) {
            smooth = 1.0;
        } else {
            const double t = (normalisedDistance - clampedHardness)
                / (1.0 - clampedHardness);
            smooth = t * t * (3.0 - 2.0 * t);
        }
        return std::clamp((1.0 - smooth) * clampedOpacity * colourAlpha,
                          0.0,
                          1.0);
    };

    for (const QPointF &stamp : stampPoints) {
        const QRect affected = QRectF(stamp - QPointF(safeRadius, safeRadius),
                                      QSizeF(safeRadius * 2.0, safeRadius * 2.0))
                                   .adjusted(-1.0, -1.0, 1.0, 1.0)
                                   .toAlignedRect()
                                   .intersected(tileRect);
        for (int documentY = affected.top(); documentY <= affected.bottom(); ++documentY) {
            const int localY = documentY - tileOrigin.y();
            if (sixteenBit) {
                auto *row = reinterpret_cast<QRgba64 *>(output.scanLine(localY));
                for (int documentX = affected.left(); documentX <= affected.right(); ++documentX) {
                    const double amount = stampAmount(documentX, documentY, stamp);
                    if (amount <= 0.0) {
                        continue;
                    }
                    const int localX = documentX - tileOrigin.x();
                    const QRgba64 before = row[localX];
                    const double beforeAlpha = before.alpha() / 65535.0;
                    if (erasing) {
                        const quint16 outputAlpha = static_cast<quint16>(std::clamp(
                            qRound(beforeAlpha * (1.0 - amount) * 65535.0),
                            0,
                            65535));
                        // Straight storage deliberately retains hidden RGB.
                        row[localX] = QRgba64::fromRgba64(before.red(),
                                                         before.green(),
                                                         before.blue(),
                                                         outputAlpha);
                        continue;
                    }
                    const double outputAlpha = amount + beforeAlpha * (1.0 - amount);
                    quint16 outputRgb[3] {before.red(), before.green(), before.blue()};
                    if (outputAlpha > 1.0e-12) {
                        const double beforeRgb[3] {before.red() / 65535.0,
                                                   before.green() / 65535.0,
                                                   before.blue() / 65535.0};
                        for (int component = 0; component < 3; ++component) {
                            const double premultiplied = paintRgb[component] * amount
                                + beforeRgb[component] * beforeAlpha * (1.0 - amount);
                            outputRgb[component] = static_cast<quint16>(std::clamp(
                                qRound(premultiplied / outputAlpha * 65535.0),
                                0,
                                65535));
                        }
                    }
                    row[localX] = QRgba64::fromRgba64(
                        outputRgb[0], outputRgb[1], outputRgb[2],
                        static_cast<quint16>(std::clamp(qRound(outputAlpha * 65535.0),
                                                       0,
                                                       65535)));
                }
            } else {
                uchar *row = output.scanLine(localY);
                for (int documentX = affected.left(); documentX <= affected.right(); ++documentX) {
                    const double amount = stampAmount(documentX, documentY, stamp);
                    if (amount <= 0.0) {
                        continue;
                    }
                    const int offset = (documentX - tileOrigin.x()) * 4;
                    const double beforeAlpha = row[offset + 3] / 255.0;
                    if (erasing) {
                        row[offset + 3] = static_cast<uchar>(std::clamp(
                            qRound(beforeAlpha * (1.0 - amount) * 255.0),
                            0,
                            255));
                        continue;
                    }
                    const double outputAlpha = amount + beforeAlpha * (1.0 - amount);
                    if (outputAlpha > 1.0e-12) {
                        for (int component = 0; component < 3; ++component) {
                            const double beforeValue = row[offset + component] / 255.0;
                            const double premultiplied = paintRgb[component] * amount
                                + beforeValue * beforeAlpha * (1.0 - amount);
                            row[offset + component] = static_cast<uchar>(std::clamp(
                                qRound(premultiplied / outputAlpha * 255.0),
                                0,
                                255));
                        }
                    }
                    row[offset + 3] = static_cast<uchar>(std::clamp(
                        qRound(outputAlpha * 255.0), 0, 255));
                }
            }
        }
    }
    output.setColorSpace(sourceTile.colorSpace());
    return output;
}

QImage stampChannelTileCpu(const QImage &sourceTile,
                           const QPoint &tileOrigin,
                           const QVector<QPointF> &stampPoints,
                           const double radius,
                           const double hardness,
                           const double opacity,
                           const int channelIndex,
                           const int channelValue)
{
    if (sourceTile.isNull() || channelIndex < 0 || channelIndex > 3) {
        return {};
    }
    const bool sixteenBit = sourceTile.depth() > 32;
    QImage output = sourceTile.convertToFormat(sixteenBit
                                                  ? QImage::Format_RGBA64
                                                  : QImage::Format_RGBA8888);
    output.detach();
    const double safeRadius = std::max(0.5, radius);
    const double clampedHardness = std::clamp(hardness, 0.0, 0.9999);
    const double clampedOpacity = std::clamp(opacity, 0.0, 1.0);
    const double paintValue = std::clamp(channelValue, 0, 255) / 255.0;
    const QRect tileRect(tileOrigin, output.size());

    const auto coverageAt = [&](const QPointF &pixelCentre, const QPointF &stamp) {
        const double normalisedDistance = QLineF(pixelCentre, stamp).length() / safeRadius;
        double smooth = 0.0;
        if (normalisedDistance <= clampedHardness) {
            smooth = 0.0;
        } else if (normalisedDistance >= 1.0) {
            smooth = 1.0;
        } else {
            const double t = (normalisedDistance - clampedHardness)
                / (1.0 - clampedHardness);
            smooth = t * t * (3.0 - 2.0 * t);
        }
        return std::clamp((1.0 - smooth) * clampedOpacity, 0.0, 1.0);
    };

    for (const QPointF &stamp : stampPoints) {
        const QRect affected = QRectF(stamp - QPointF(safeRadius, safeRadius),
                                      QSizeF(safeRadius * 2.0, safeRadius * 2.0))
                                   .adjusted(-1.0, -1.0, 1.0, 1.0)
                                   .toAlignedRect()
                                   .intersected(tileRect);
        for (int documentY = affected.top(); documentY <= affected.bottom(); ++documentY) {
            const int localY = documentY - tileOrigin.y();
            if (sixteenBit) {
                auto *row = reinterpret_cast<QRgba64 *>(output.scanLine(localY));
                for (int documentX = affected.left(); documentX <= affected.right(); ++documentX) {
                    const double amount = coverageAt(QPointF(documentX + 0.5, documentY + 0.5), stamp);
                    if (amount <= 0.0) {
                        continue;
                    }
                    const int localX = documentX - tileOrigin.x();
                    const QRgba64 pixel = row[localX];
                    quint16 channels[4] {pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
                    const double current = channels[channelIndex] / 65535.0;
                    channels[channelIndex] = static_cast<quint16>(std::clamp(
                        qRound((paintValue * amount + current * (1.0 - amount)) * 65535.0),
                        0,
                        65535));
                    row[localX] = QRgba64::fromRgba64(channels[0], channels[1], channels[2], channels[3]);
                }
            } else {
                uchar *row = output.scanLine(localY);
                for (int documentX = affected.left(); documentX <= affected.right(); ++documentX) {
                    const double amount = coverageAt(QPointF(documentX + 0.5, documentY + 0.5), stamp);
                    if (amount <= 0.0) {
                        continue;
                    }
                    const int localX = documentX - tileOrigin.x();
                    uchar &component = row[localX * 4 + channelIndex];
                    component = static_cast<uchar>(std::clamp(
                        qRound(paintValue * amount * 255.0 + component * (1.0 - amount)),
                        0,
                        255));
                }
            }
        }
    }
    output.setColorSpace(sourceTile.colorSpace());
    return output;
}

QImage selectionCoverageTile(const SelectionMask *selection,
                             const QTransform &layerToDocument,
                             const QRect &tileRect)
{
    if (tileRect.isEmpty()) {
        return {};
    }
    QImage coverage(tileRect.size(), QImage::Format_Grayscale8);
    if (!selection || !selection->isActive()) {
        coverage.fill(255);
        return coverage;
    }
    if (selection->isEmpty()) {
        coverage.fill(0);
        return coverage;
    }
    if (layerToDocument.isIdentity()) {
        coverage = selection->coverageImage(tileRect, tileRect.size());
        if (!coverage.isNull()) {
            return coverage.convertToFormat(QImage::Format_Grayscale8);
        }
    }
    coverage.fill(0);
    for (int y = 0; y < coverage.height(); ++y) {
        uchar *row = coverage.scanLine(y);
        for (int x = 0; x < coverage.width(); ++x) {
            const double value = sampleSelectionCoverage(
                *selection,
                layerToDocument,
                QPointF(tileRect.x() + x + 0.5, tileRect.y() + y + 0.5));
            row[x] = static_cast<uchar>(std::clamp(qRound(value * 255.0), 0, 255));
        }
    }
    return coverage;
}

QImage cloneSelectionCoverageTile(const SelectionMask *selection,
                                  const CloneStampRequest &request,
                                  const QRect &tileRect)
{
    if (request.targetPixelToLayer.isIdentity()) {
        return selectionCoverageTile(selection,
                                     request.targetLayerToDocument,
                                     tileRect);
    }
    if (tileRect.isEmpty()) {
        return {};
    }
    QImage coverage(tileRect.size(), QImage::Format_Grayscale8);
    if (!selection || !selection->isActive()) {
        coverage.fill(255);
        return coverage;
    }
    if (selection->isEmpty()) {
        coverage.fill(0);
        return coverage;
    }
    coverage.fill(0);
    for (int y = 0; y < coverage.height(); ++y) {
        uchar *row = coverage.scanLine(y);
        for (int x = 0; x < coverage.width(); ++x) {
            const QPointF targetPixelCentre(tileRect.x() + x + 0.5,
                                            tileRect.y() + y + 0.5);
            const QPointF targetLayerPoint = request.targetPixelToLayer.map(
                targetPixelCentre);
            const double value = sampleSelectionCoverage(
                *selection, request.targetLayerToDocument, targetLayerPoint);
            row[x] = static_cast<uchar>(std::clamp(qRound(value * 255.0),
                                                   0, 255));
        }
    }
    return coverage;
}

QImage coverageAsRgba(const QImage &coverage)
{
    const QImage gray = coverage.convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull()) {
        return {};
    }
    QImage rgba(gray.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < gray.height(); ++y) {
        const uchar *source = gray.constScanLine(y);
        uchar *target = rgba.scanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            const uchar value = source[x];
            target[x * 4] = value;
            target[x * 4 + 1] = value;
            target[x * 4 + 2] = value;
            target[x * 4 + 3] = 255;
        }
    }
    return rgba;
}

QImage applyRasterSelectionCoverage(const QImage &sourceTile,
                                    const QImage &editedTile,
                                    const QImage &coverageTile)
{
    const QImage source = sourceTile.convertToFormat(QImage::Format_RGBA8888);
    QImage edited = editedTile.convertToFormat(QImage::Format_RGBA8888);
    const QImage coverage = coverageTile.convertToFormat(QImage::Format_Grayscale8);
    if (source.isNull() || edited.isNull() || coverage.isNull()
        || source.size() != edited.size() || source.size() != coverage.size()) {
        return {};
    }
    for (int y = 0; y < edited.height(); ++y) {
        const uchar *before = source.constScanLine(y);
        uchar *after = edited.scanLine(y);
        const uchar *selection = coverage.constScanLine(y);
        for (int x = 0; x < edited.width(); ++x) {
            const double amount = selection[x] / 255.0;
            if (amount >= 1.0) {
                continue;
            }
            const int offset = x * 4;
            if (amount <= 0.0) {
                for (int component = 0; component < 4; ++component) {
                    after[offset + component] = before[offset + component];
                }
                continue;
            }
            const double beforeAlpha = before[offset + 3] / 255.0;
            const double editedAlpha = after[offset + 3] / 255.0;
            const double outputAlpha = beforeAlpha + (editedAlpha - beforeAlpha) * amount;
            uchar outputRgb[3] {before[offset], before[offset + 1], before[offset + 2]};
            if (outputAlpha > 1.0e-9) {
                for (int component = 0; component < 3; ++component) {
                    const double premultiplied =
                        (before[offset + component] / 255.0) * beforeAlpha * (1.0 - amount)
                        + (after[offset + component] / 255.0) * editedAlpha * amount;
                    outputRgb[component] = static_cast<uchar>(std::clamp(
                        qRound(premultiplied / outputAlpha * 255.0), 0, 255));
                }
            }
            after[offset] = outputRgb[0];
            after[offset + 1] = outputRgb[1];
            after[offset + 2] = outputRgb[2];
            after[offset + 3] = static_cast<uchar>(std::clamp(
                qRound(outputAlpha * 255.0), 0, 255));
        }
    }
    edited.setColorSpace(sourceTile.colorSpace());
    return edited;
}

QImage applyMaskSelectionCoverage(const QImage &sourceTile,
                                  const QImage &editedTile,
                                  const QImage &coverageTile)
{
    const QImage source = sourceTile.convertToFormat(QImage::Format_Grayscale8);
    QImage edited = editedTile.convertToFormat(QImage::Format_Grayscale8);
    const QImage coverage = coverageTile.convertToFormat(QImage::Format_Grayscale8);
    if (source.isNull() || edited.isNull() || coverage.isNull()
        || source.size() != edited.size() || source.size() != coverage.size()) {
        return {};
    }
    for (int y = 0; y < edited.height(); ++y) {
        const uchar *before = source.constScanLine(y);
        uchar *after = edited.scanLine(y);
        const uchar *selection = coverage.constScanLine(y);
        for (int x = 0; x < edited.width(); ++x) {
            const double amount = selection[x] / 255.0;
            after[x] = static_cast<uchar>(std::clamp(
                qRound(before[x] + (after[x] - before[x]) * amount), 0, 255));
        }
    }
    return edited;
}

quint64 tileContentRevision(const QImage &image)
{
    quint64 hash = FnvOffset;
    hashValue(hash, image.width());
    hashValue(hash, image.height());
    hashValue(hash, static_cast<int>(image.format()));
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < rgba.height(); ++y) {
        hashBytes(hash, rgba.constScanLine(y), rgba.bytesPerLine());
    }
    return nonZero(hash);
}

quint64 strokeRevision(const QImage &inputTile,
                       const QPoint &origin,
                       const QVector<QPointF> &stamps,
                       const double diameter,
                       const double opacity,
                       const double hardness,
                       const QColor &colour,
                       const bool erasing,
                       const quint64 layerRevision,
                       const quint64 selectionRevision,
                       const QImage &selectionCoverage)
{
    quint64 hash = FnvOffset;
    hashValue(hash, layerRevision);
    hashValue(hash, selectionRevision);
    hashValue(hash, origin.x());
    hashValue(hash, origin.y());
    hashValue(hash, diameter);
    hashValue(hash, opacity);
    hashValue(hash, hardness);
    hashValue(hash, colour.rgba64().toArgb32());
    hashValue(hash, erasing);
    for (const QPointF &point : stamps) {
        const double x = point.x();
        const double y = point.y();
        hashValue(hash, x);
        hashValue(hash, y);
    }
    const auto hashImage = [&hash](const QImage &image) {
        hashValue(hash, image.width());
        hashValue(hash, image.height());
        hashValue(hash, static_cast<int>(image.format()));
        hashValue(hash, image.depth());
        const qsizetype rowBytes = static_cast<qsizetype>(image.width())
            * static_cast<qsizetype>(std::max(1, image.depth() / 8));
        for (int y = 0; y < image.height(); ++y) {
            hashBytes(hash, image.constScanLine(y), rowBytes);
        }
    };
    hashImage(inputTile);
    hashImage(selectionCoverage);
    return nonZero(hash);
}

struct CloneSourcePatch {
    QImage image;
    QPoint origin;
    bool exceededGuard = false;
};

QPointF cloneSourcePixelForTarget(const CloneStampRequest &request,
                                  const QPointF &targetPixel)
{
    const QPointF targetLayerPoint = request.targetPixelToLayer.map(targetPixel);
    const QPointF destinationDocumentPoint = request.targetLayerToDocument.map(
        targetLayerPoint);
    const QPointF sourceDocumentPoint = destinationDocumentPoint
        + request.sourceOffsetDocument;
    const QPointF sourceLayerPoint = request.sourceDocumentToLayer.map(
        sourceDocumentPoint);
    return request.sourceLayerToPixel.map(sourceLayerPoint) - QPointF(0.5, 0.5);
}

CloneSourcePatch cloneSourcePatchForTile(const CloneStampRequest &request,
                                         const QRect &tileRect)
{
    CloneSourcePatch patch;
    if (request.source.isNull() || tileRect.isEmpty()) {
        return patch;
    }

    QPolygonF sourceFootprint;
    sourceFootprint.reserve(4);
    sourceFootprint << cloneSourcePixelForTarget(
                           request, QPointF(tileRect.left() + 0.5,
                                            tileRect.top() + 0.5))
                    << cloneSourcePixelForTarget(
                           request, QPointF(tileRect.right() + 0.5,
                                            tileRect.top() + 0.5))
                    << cloneSourcePixelForTarget(
                           request, QPointF(tileRect.right() + 0.5,
                                            tileRect.bottom() + 0.5))
                    << cloneSourcePixelForTarget(
                           request, QPointF(tileRect.left() + 0.5,
                                            tileRect.bottom() + 0.5));
    const QRectF sourceBounds = sourceFootprint.boundingRect();
    const double sourceMaximumX = request.source.width() - 1.0;
    const double sourceMaximumY = request.source.height() - 1.0;
    if (std::isfinite(sourceBounds.left())
        && std::isfinite(sourceBounds.right())
        && std::isfinite(sourceBounds.top())
        && std::isfinite(sourceBounds.bottom())
        && sourceBounds.right() >= 0.0 && sourceBounds.bottom() >= 0.0
        && sourceBounds.left() <= sourceMaximumX
        && sourceBounds.top() <= sourceMaximumY) {
        // Include every floor/upper texel required by bilinear reads plus one
        // safety pixel around that exact footprint. Explicit inclusive integer
        // bounds avoid dropping the upper neighbour when an affine corner lands
        // exactly on an integer source coordinate.
        const int left = std::max(
            0, static_cast<int>(std::floor(std::max(0.0, sourceBounds.left()))) - 1);
        const int top = std::max(
            0, static_cast<int>(std::floor(std::max(0.0, sourceBounds.top()))) - 1);
        const int right = std::min(
            request.source.width() - 1,
            static_cast<int>(std::floor(std::min(sourceMaximumX,
                                                  sourceBounds.right()))) + 2);
        const int bottom = std::min(
            request.source.height() - 1,
            static_cast<int>(std::floor(std::min(sourceMaximumY,
                                                  sourceBounds.bottom()))) + 2);
        const QRect sourceRect(QPoint(left, top), QPoint(right, bottom));
        if (!sourceRect.isEmpty()) {
            patch.origin = sourceRect.topLeft();
            const qsizetype sourcePatchPixels = qsizetype(sourceRect.width())
                * qsizetype(sourceRect.height());
            if (sourcePatchPixels > MaximumCloneSourcePatchPixels) {
                patch.exceededGuard = true;
                return patch;
            }
            patch.image = request.source.copy(sourceRect);
            return patch;
        }
    } else if (!std::isfinite(sourceBounds.left())
               || !std::isfinite(sourceBounds.right())
               || !std::isfinite(sourceBounds.top())
               || !std::isfinite(sourceBounds.bottom())) {
        patch.exceededGuard = true;
        return patch;
    }

    const bool grey = request.source.format() == QImage::Format_Grayscale8
        || request.source.format() == QImage::Format_Grayscale16;
    patch.image = QImage(1, 1, grey ? QImage::Format_Grayscale8
                                    : QImage::Format_RGBA8888);
    if (grey) {
        patch.image.fill(0);
    } else {
        patch.image.fill(Qt::transparent);
    }
    patch.origin = {};
    return patch;
}

quint64 cloneStrokeRevision(const QImage &destinationTile,
                            const CloneSourcePatch &sourcePatch,
                            const QRect &tileRect,
                            const QVector<QPointF> &stamps,
                            const CloneStampRequest &request,
                            const quint64 layerRevision,
                            const quint64 selectionRevision,
                            const QImage &selectionCoverage)
{
    quint64 hash = FnvOffset;
    hashValue(hash, layerRevision);
    hashValue(hash, selectionRevision);
    hashValue(hash, tileRect.x());
    hashValue(hash, tileRect.y());
    hashValue(hash, request.diameterPixels);
    hashValue(hash, request.opacity);
    hashValue(hash, request.hardness);
    hashValue(hash, static_cast<int>(request.target));
    hashValue(hash, static_cast<int>(request.sample));
    hashValue(hash, request.componentIndex);
    hashValue(hash, request.sourceOffsetDocument.x());
    hashValue(hash, request.sourceOffsetDocument.y());
    hashTransform(hash, request.targetPixelToLayer);
    hashTransform(hash, request.targetLayerToDocument);
    hashTransform(hash, request.sourceDocumentToLayer);
    hashTransform(hash, request.sourceLayerToPixel);
    hashValue(hash, sourcePatch.origin.x());
    hashValue(hash, sourcePatch.origin.y());
    hashValue(hash, request.source.width());
    hashValue(hash, request.source.height());
    for (const QPointF &point : stamps) {
        hashValue(hash, point.x());
        hashValue(hash, point.y());
    }
    const auto hashImage = [&hash](const QImage &image) {
        hashValue(hash, image.width());
        hashValue(hash, image.height());
        hashValue(hash, static_cast<int>(image.format()));
        const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < rgba.height(); ++y) {
            hashBytes(hash, rgba.constScanLine(y), rgba.bytesPerLine());
        }
    };
    hashImage(destinationTile);
    hashImage(sourcePatch.image);
    hashImage(selectionCoverage);
    return nonZero(hash);
}

QImage transformedMaskTile(const QImage &source,
                           const LayerNode &layer,
                           const QTransform &worldTransform,
                           const QRect &tileRect,
                           const QSize &documentSize)
{
    if (!layer.maskEnabled || layer.maskImage.isNull() || tileRect.isEmpty()
        || source.isNull() || documentSize.isEmpty()) {
        return {};
    }
    if (!layer.maskInverted
        && layer.maskImage.size() == QSize(1, 1)
        && qGray(layer.maskImage.pixel(0, 0)) == 255) {
        return {};
    }
    if (layer.type == LayerType::Smart) {
        return ImageProcessor::renderSmartMaskRegion(
            layer.maskImage, layer.id, layer.maskReferenceSize,
            layer.maskReferenceOrigin, source.size(), worldTransform,
            documentSize, tileRect, layer.maskEnabled, layer.maskInverted,
            layer.smartTransform);
    }

    // Prepare the exact greyscale coverage consumed by the WGSL compositor.
    // The previous implementation rendered a white raster through the CPU
    // compositor and then called createAlphaMask(). That 1-bit conversion is
    // implementation-dependent for fully transparent images and could turn an
    // inverted white mask back into an all-white GPU mask. Rendering the mask
    // directly keeps black=0 and white=255 throughout the tiled path.
    QImage effective = layer.maskImage.convertToFormat(QImage::Format_Grayscale8);
    if (layer.maskInverted) {
        effective.invertPixels(QImage::InvertRgb);
    }

    QImage tile(tileRect.size(), QImage::Format_ARGB32_Premultiplied);
    tile.fill(Qt::black);
    QPainter painter(&tile);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                          effective.size() != source.size()
                              || worldTransform.type() > QTransform::TxTranslate);

    const QSize reference = layer.maskReferenceSize.isValid()
            && !layer.maskReferenceSize.isEmpty()
        ? layer.maskReferenceSize : documentSize;
    const QTransform imageToDocument = QTransform::fromScale(
        reference.width() / static_cast<double>(std::max(1, effective.width())),
        reference.height() / static_cast<double>(std::max(1, effective.height())))
        * QTransform::fromTranslate(layer.maskReferenceOrigin.x(),
                                    layer.maskReferenceOrigin.y());
    const QTransform documentToPreview = QTransform::fromScale(
        source.width() / static_cast<double>(std::max(1, documentSize.width())),
        source.height() / static_cast<double>(std::max(1, documentSize.height())));
    painter.setTransform(imageToDocument
                         * worldTransform
                         * documentToPreview
                         * QTransform::fromTranslate(-tileRect.left(), -tileRect.top()));
    painter.drawImage(QPointF(), effective);
    painter.end();
    return tile.convertToFormat(QImage::Format_Grayscale8);
}

bool prepareAdjustmentItem(const QImage &source,
                           const QSize &documentSize,
                           const AdjustmentData &data,
                           const ColourProcessingCompatibility processingCompatibility,
                           PreparedTileLayer *item)
{
    if (!item) return false;
    item->kind = PreparedTileLayer::Kind::Adjustment;
    item->adjustmentType = data.type;
    item->processingDomain = adjustmentProcessingDomain(data);
    if (processingCompatibility == ColourProcessingCompatibility::ManagedV1
        && adjustmentRequiresManagedDomainTransform(data)) {
        QString domainError;
        item->managedDomainLut = createManagedAdjustmentGpuLut(
            source.colorSpace(), item->processingDomain, &domainError);
        if (!item->managedDomainLut) return false;
    }
    if (data.type == AdjustmentType::Exposure) {
        item->exposureParameters = std::get<ExposureParameters>(data.parameters);
        item->exposure = item->exposureParameters.exposure;
    } else if (data.type == AdjustmentType::Contrast) {
        item->contrastParameters = std::get<ContrastParameters>(data.parameters);
        item->contrast = item->contrastParameters.contrast;
    } else if (data.type == AdjustmentType::Saturation) {
        item->saturationParameters = std::get<SaturationParameters>(data.parameters);
        item->saturation = item->saturationParameters.saturation;
    } else if (data.type == AdjustmentType::Levels) {
        item->levels = std::get<LevelsParameters>(data.parameters);
        const auto &rgb = item->levels.channel(AdjustmentChannel::Rgb);
        item->blackPoint = rgb.inputBlack;
        item->whitePoint = rgb.inputWhite;
        item->gamma = rgb.gamma;
        item->tonalLookup = buildTonalLookup(data, 8).toRgba8Image();
    } else if (data.type == AdjustmentType::Curves) {
        item->curves = std::get<CurvesParameters>(data.parameters);
        item->tonalLookup = buildTonalLookup(data, 8).toRgba8Image();
    } else if (data.type == AdjustmentType::HueSaturation) {
        item->hueSaturationParameters = std::get<HueSaturationParameters>(data.parameters);
    } else if (data.type == AdjustmentType::Vibrance) {
        item->vibranceParameters = std::get<VibranceParameters>(data.parameters);
    } else if (data.type == AdjustmentType::WhiteBalance) {
        item->whiteBalanceParameters = std::get<WhiteBalanceParameters>(data.parameters);
    } else if (data.type == AdjustmentType::ColourBalance) {
        item->colourBalanceParameters = std::get<ColourBalanceParameters>(data.parameters);
    } else if (data.type == AdjustmentType::ChannelMixer) {
        item->channelMixerParameters = std::get<ChannelMixerParameters>(data.parameters);
    } else if (data.type == AdjustmentType::BlackAndWhite) {
        item->blackAndWhiteParameters = std::get<BlackAndWhiteParameters>(data.parameters);
    } else if (data.type == AdjustmentType::GradientMap) {
        item->gradientMapParameters = std::get<GradientMapParameters>(data.parameters);
        item->tonalLookup = buildTonalLookup(data, 8).toRgba8Image();
    } else if (data.type == AdjustmentType::Posterise) {
        item->posteriseParameters = std::get<PosteriseParameters>(data.parameters);
    } else if (data.type == AdjustmentType::Threshold) {
        item->thresholdParameters = std::get<ThresholdParameters>(data.parameters);
    } else if (data.type == AdjustmentType::Invert) {
        item->invertParameters = std::get<InvertParameters>(data.parameters);
    } else if (data.type == AdjustmentType::PhotoFilter) {
        item->photoFilterParameters = std::get<PhotoFilterParameters>(data.parameters);
    } else if (data.type == AdjustmentType::SelectiveColour) {
        item->selectiveColourParameters = std::get<SelectiveColourParameters>(data.parameters);
    } else if (data.type == AdjustmentType::Lut) {
        item->lutParameters = std::get<LutParameters>(data.parameters);
        item->lutDocumentTransfer = CubeLut::documentTransferFor(source.colorSpace());
        item->lutLookup = CubeLut::buildGpuTextureData(item->lutParameters);
        if (item->lutParameters.hasData() && !item->lutLookup.isValid()) return false;
    } else if (data.type == AdjustmentType::ShadowsHighlights) {
        item->shadowsHighlightsParameters = std::get<ShadowsHighlightsParameters>(data.parameters);
        const double previewScale = std::min(
            source.width() / static_cast<double>(std::max(1, documentSize.width())),
            source.height() / static_cast<double>(std::max(1, documentSize.height())));
        item->shadowsHighlightsParameters.radius = std::max(
            1.0, item->shadowsHighlightsParameters.radius * previewScale);
    }
    return true;
}

bool prepareTree(const QImage &source,
                 const QVector<LayerNode> &layers,
                 const QRect &tileRect,
                 const QSize &documentSize,
                 const QTransform &parentTransform,
                 QVector<PreparedTileLayer> *prepared,
                 WebGpuContext *webGpu,
                 const std::atomic_bool *cancelRequested,
                 const ColourProcessingCompatibility processingCompatibility,
                 QString *error)
{
    if (!prepared || (cancelRequested
                      && cancelRequested->load(std::memory_order_acquire))) {
        return false;
    }
    prepared->clear();
    prepared->reserve(layers.size());
    for (const LayerNode &layer : layers) {
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            return false;
        }
        if (!layer.visible || layer.opacity <= 0.0) {
            continue;
        }
        const QTransform worldTransform = layer.transform * parentTransform;
        PreparedTileLayer item;
        item.opacity = layer.opacity;
        item.blendMode = layer.blendMode;
        item.groupCompositeMode = layer.groupCompositeMode;
        item.mask = transformedMaskTile(source, layer, worldTransform, tileRect, documentSize);
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            return false;
        }

        if (layer.type == LayerType::Group) {
            item.kind = PreparedTileLayer::Kind::Group;
            if (!prepareTree(source,
                             layer.children,
                             tileRect,
                             documentSize,
                             worldTransform,
                             &item.children,
                             webGpu,
                             cancelRequested,
                             processingCompatibility,
                             error)) {
                return false;
            }
        } else if (layer.type == LayerType::Adjustment) {
            if (!prepareAdjustmentItem(source, documentSize,
                                       layer.effectiveAdjustmentData(),
                                       processingCompatibility, &item)) {
                return false;
            }
        } else if (layer.type == LayerType::Smart && !layer.liveFilters.isEmpty()) {
            bool allGpuApproved = webGpu && webGpu->deviceReady();
            for (const LiveFilter &filter : layer.liveFilters) {
                if (!filter.enabled) continue;
                allGpuApproved = allGpuApproved
                    && webGpu->adjustmentGpuApproved(filter.adjustment.type);
            }
            if (allGpuApproved) {
                // Represent the Smart instance as one isolated native subgroup:
                // transformed source at the bottom, then the ordered Live
                // Filters as adjustment passes. This reuses the established
                // per-adjustment startup parity gate while keeping Live Filter
                // ownership distinct from Adjustment Layers in the document.
                item.kind = PreparedTileLayer::Kind::Group;
                item.groupCompositeMode = GroupCompositeMode::Isolated;
                item.children.clear();
                item.children.reserve(layer.liveFilters.size() + 1);

                for (auto filterIt = layer.liveFilters.crbegin();
                     filterIt != layer.liveFilters.crend(); ++filterIt) {
                    if (!filterIt->enabled) continue;
                    PreparedTileLayer filterItem;
                    filterItem.opacity = 1.0;
                    filterItem.blendMode = BlendMode::Copy;
                    if (!prepareAdjustmentItem(source, documentSize,
                                               filterIt->adjustment,
                                               processingCompatibility,
                                               &filterItem)) {
                        return false;
                    }
                    if (filterIt->hasMask() && filterIt->maskEnabled) {
                        filterItem.mask = ImageProcessor::renderSmartMaskRegion(
                            filterIt->maskImage, filterIt->id,
                            filterIt->maskReferenceSize, filterIt->maskReferenceOrigin,
                            source.size(), worldTransform, documentSize, tileRect,
                            filterIt->maskEnabled, filterIt->maskInverted,
                            layer.smartTransform);
                        if (cancelRequested
                            && cancelRequested->load(std::memory_order_acquire)) {
                            return false;
                        }
                    }
                    item.children.push_back(std::move(filterItem));
                }

                LayerNode sourceOnly = layer;
                sourceOnly.transform = QTransform();
                sourceOnly.opacity = 1.0;
                sourceOnly.blendMode = BlendMode::Copy;
                sourceOnly.maskImage = {};
                sourceOnly.maskReferenceSize = {};
                sourceOnly.maskReferenceOrigin = {};
                sourceOnly.maskEnabled = true;
                sourceOnly.maskInverted = false;
                sourceOnly.liveFilters.clear();
                sourceOnly.layerEffects.clear();
                QVector<PreparedTileLayer> sourcePrepared;
                if (!prepareTree(source, {sourceOnly}, tileRect, documentSize,
                                 worldTransform, &sourcePrepared, webGpu,
                                 cancelRequested, processingCompatibility, error)
                    || sourcePrepared.size() != 1) {
                    return false;
                }
                item.children.push_back(std::move(sourcePrepared.front()));
            } else {
                item.kind = PreparedTileLayer::Kind::Image;
                LayerNode isolated = layer;
                isolated.opacity = 1.0;
                isolated.blendMode = BlendMode::Copy;
                isolated.transform = worldTransform;
                isolated.maskImage = {};
                isolated.layerEffects.clear();
                item.image = ImageProcessor::renderRegion(source, {isolated}, tileRect,
                                                          documentSize, cancelRequested,
                                                          processingCompatibility);
                if (item.image.isNull()) {
                    if (cancelRequested
                        && cancelRequested->load(std::memory_order_acquire)) {
                        return false;
                    }
                    if (error && error->isEmpty()) {
                        *error = QStringLiteral(
                            "Live Filter CPU fallback failed to render the requested Smart tile");
                    }
                    return false;
                }
                {
                    const QRect sourcePixels = smartSourcePixelsForPreviewTile(
                        layer, source.size(), worldTransform, documentSize, tileRect);
                    const quint64 sourceFingerprint = SmartLayerTileCache::instance()
                        .sourceRegionFingerprint(layer.smartPresentationImage,
                                                 layer.smartSource.sourceId,
                                                 layer.smartSource.observedSourceRevision,
                                                 sourcePixels);
                    QByteArray smartKey = SmartLayerTileCache::transformedKey(
                        layer.smartPresentationImage,
                        layer.smartSource.sourceId,
                        layer.smartSource.observedSourceRevision,
                        layer.smartPresentationReferenceSize,
                        layer.smartPresentationReferenceOrigin,
                        source.size(), worldTransform, documentSize, tileRect,
                        item.image.format(), source.colorSpace(),
                        layer.smartTransform, false, sourceFingerprint);
                    smartKey = appendLiveFilterIdentity(std::move(smartKey), layer.liveFilters);
                    item.residentTileKey = SmartLayerTileCache::gpuResidencyKey(smartKey);
                    item.residentTileRevision = sourceFingerprint;
                    if (webGpu && item.residentTileKey != 0) {
                        webGpu->cacheResidentTile(item.residentTileKey,
                                                  item.residentTileRevision,
                                                  item.image);
                    }
                }
            }
        } else {
            item.kind = PreparedTileLayer::Kind::Image;
            const auto transparentTile = [&]() {
                QImage transparent(tileRect.size(), QImage::Format_ARGB32_Premultiplied);
                if (!transparent.isNull()) {
                    transparent.fill(Qt::transparent);
                    transparent.setColorSpace(source.colorSpace());
                }
                return transparent;
            };

            if (layer.type == LayerType::Vector
                && layer.vectorData.featherRadius > 0.0) {
                if (!webGpu || !webGpu->vectorFeatherGpuApproved()) {
                    if (error && error->isEmpty()) {
                        *error = QStringLiteral(
                            "Vector Feather uses the exact CPU fallback because "
                            "the native coverage kernel did not pass the startup parity gate");
                    }
                    return false;
                }
                const QTransform documentToPreview = QTransform::fromScale(
                    source.width() / static_cast<double>(std::max(1, documentSize.width())),
                    source.height() / static_cast<double>(std::max(1, documentSize.height())));
                const QRectF featherBounds = documentToPreview.mapRect(
                    VectorRasterizer::contentBounds(layer, worldTransform));
                if (featherBounds.isEmpty()
                    || !featherBounds.intersects(QRectF(tileRect))) {
                    item.image = transparentTile();
                } else {
                    VectorFeatherGpuTileData featherTile;
                    QString featherError;
                    if (!VectorRasterizer::prepareGpuFeatherTile(
                            layer, source.size(), tileRect, documentSize,
                            worldTransform, source.colorSpace(), false,
                            source.format() == QImage::Format_Grayscale8
                                || source.format() == QImage::Format_Grayscale16,
                            &featherTile, &featherError, cancelRequested)) {
                        if (error && error->isEmpty()) *error = featherError;
                        return false;
                    }
                    item.image = webGpu->featherVectorCoverageTile(
                        featherTile, &featherError);
                    if (item.image.isNull()) {
                        if (error && error->isEmpty()) *error = featherError;
                        return false;
                    }
                }
            } else {
                LayerNode isolated = layer;
                isolated.opacity = 1.0;
                isolated.blendMode = BlendMode::Copy;
                isolated.transform = worldTransform;
                isolated.maskImage = {};
                item.image = ImageProcessor::renderRegion(source,
                                                          {isolated},
                                                          tileRect,
                                                          documentSize,
                                                          cancelRequested);
                if (layer.type == LayerType::Smart && !item.image.isNull()) {
                    const QRect sourcePixels = smartSourcePixelsForPreviewTile(
                        layer, source.size(), worldTransform, documentSize, tileRect);
                    const quint64 sourceFingerprint = SmartLayerTileCache::instance()
                        .sourceRegionFingerprint(layer.smartPresentationImage,
                                                 layer.smartSource.sourceId,
                                                 layer.smartSource.observedSourceRevision,
                                                 sourcePixels);
                    QByteArray smartKey = SmartLayerTileCache::transformedKey(
                        layer.smartPresentationImage,
                        layer.smartSource.sourceId,
                        layer.smartSource.observedSourceRevision,
                        layer.smartPresentationReferenceSize,
                        layer.smartPresentationReferenceOrigin,
                        source.size(),
                        worldTransform,
                        documentSize,
                        tileRect,
                        item.image.format(),
                        source.colorSpace(),
                        layer.smartTransform,
                        false,
                        sourceFingerprint);
                    smartKey = appendLiveFilterIdentity(std::move(smartKey), layer.liveFilters);
                    item.residentTileKey = SmartLayerTileCache::gpuResidencyKey(smartKey);
                    // Residency revision follows the sampled source footprint,
                    // not the Smart Source's global revision. A distant source
                    // edit can therefore retain the exact same resident tile.
                    item.residentTileRevision = sourceFingerprint;
                    if (webGpu && item.residentTileKey != 0) {
                        webGpu->cacheResidentTile(item.residentTileKey,
                                                  item.residentTileRevision,
                                                  item.image);
                    }
                }
            }
            if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
                return false;
            }
            if (item.image.isNull()) {
                item.image = transparentTile();
                if (item.image.isNull()) return false;
            }
        }
        QVector<LayerEffectRenderPass> effectPasses;
        if (!layer.layerEffects.isEmpty()) {
            effectPasses = ImageProcessor::renderLayerEffectPasses(
                source, layer, tileRect, documentSize, worldTransform,
                cancelRequested, processingCompatibility);
            if (cancelRequested
                && cancelRequested->load(std::memory_order_acquire)) {
                return false;
            }
        }
        // Prepared stacks are stored top-to-bottom and encoded bottom-to-top.
        // Reverse each front/behind subset so the authored fx order matches the
        // exact CPU compositor while every pass retains its own blend mode.
        for (auto effectIt = effectPasses.crbegin(); effectIt != effectPasses.crend(); ++effectIt) {
            if (effectIt->behindSource) continue;
            PreparedTileLayer effectItem;
            effectItem.kind = PreparedTileLayer::Kind::Image;
            effectItem.image = effectIt->image;
            effectItem.opacity = effectIt->opacity * layer.opacity;
            effectItem.blendMode = effectIt->blendMode;
            effectItem.residentTileKey = layerEffectResidentTileKey(layer, *effectIt, tileRect);
            effectItem.residentTileRevision = static_cast<quint64>(effectIt->image.cacheKey());
            if (webGpu && effectItem.residentTileKey != 0) {
                webGpu->cacheResidentTile(effectItem.residentTileKey,
                                          effectItem.residentTileRevision,
                                          effectItem.image);
            }
            prepared->push_back(std::move(effectItem));
        }
        prepared->push_back(std::move(item));
        for (auto effectIt = effectPasses.crbegin(); effectIt != effectPasses.crend(); ++effectIt) {
            if (!effectIt->behindSource) continue;
            PreparedTileLayer effectItem;
            effectItem.kind = PreparedTileLayer::Kind::Image;
            effectItem.image = effectIt->image;
            effectItem.opacity = effectIt->opacity * layer.opacity;
            effectItem.blendMode = effectIt->blendMode;
            effectItem.residentTileKey = layerEffectResidentTileKey(layer, *effectIt, tileRect);
            effectItem.residentTileRevision = static_cast<quint64>(effectIt->image.cacheKey());
            if (webGpu && effectItem.residentTileKey != 0) {
                webGpu->cacheResidentTile(effectItem.residentTileKey,
                                          effectItem.residentTileRevision,
                                          effectItem.image);
            }
            prepared->push_back(std::move(effectItem));
        }
    }
    return true;
}

bool treeContainsFeatheredVector(const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (!layer.visible || layer.opacity <= 0.0) continue;
        if (layer.type == LayerType::Vector
            && layer.vectorData.featherRadius > 0.0) {
            return true;
        }
        if (layer.type == LayerType::Group
            && treeContainsFeatheredVector(layer.children)) {
            return true;
        }
    }
    return false;
}

SpatialFilterContract stackSpatialContract(const QVector<LayerNode> &layers)
{
    SpatialFilterContract contract;
    contract.documentRadius = maximumSpatialAdjustmentRadius2D(layers);
    contract.edgeMode = SpatialEdgeMode::Clamp;
    contract.alphaMode = SpatialAlphaMode::PreserveSourceAlpha;
    contract.quality = SpatialPreviewQuality::Final;
    contract.safetyPadding = SpatialFilterContract::DefaultSafetyPadding;
    contract.maximumRadius = SpatialFilterContract::DefaultMaximumRadius;
    return contract;
}

SpatialFilterTilePlan spatialPlanForRegion(const QImage &source,
                                           const QVector<LayerNode> &layers,
                                           const QRect &region,
                                           const QSize &documentSize)
{
    return SpatialFilterFoundation::plan(region, source.size(), documentSize,
                                         stackSpatialContract(layers));
}

QRect spatialDependencyRect(const QImage &source,
                            const QVector<LayerNode> &layers,
                            const QRect &region,
                            const QSize &documentSize)
{
    const SpatialFilterTilePlan plan = spatialPlanForRegion(
        source, layers, region, documentSize);
    return plan.valid && !plan.dependencyBounds.isEmpty()
        ? plan.dependencyBounds : source.rect();
}

} // namespace

TiledCanvasEngine::TiledCanvasEngine(WebGpuContext *webGpu)
    : m_webGpu(webGpu)
{
}

QVector<QRect> TiledCanvasEngine::tileRectsForRegion(const QRect &region, const QSize &extent) const
{
    QVector<QRect> result;
    const QRect clipped = region.intersected(QRect(QPoint(0, 0), extent));
    if (clipped.isEmpty()) {
        return result;
    }
    const int firstX = clipped.left() / TileSize;
    const int lastX = clipped.right() / TileSize;
    const int firstY = clipped.top() / TileSize;
    const int lastY = clipped.bottom() / TileSize;
    result.reserve((lastX - firstX + 1) * (lastY - firstY + 1));
    for (int y = firstY; y <= lastY; ++y) {
        for (int x = firstX; x <= lastX; ++x) {
            result.push_back(QRect(x * TileSize,
                                   y * TileSize,
                                   TileSize,
                                   TileSize)
                                 .intersected(QRect(QPoint(0, 0), extent)));
        }
    }
    return result;
}

quint64 TiledCanvasEngine::compositeRevision(const QImage &source,
                                             const QVector<LayerNode> &layers,
                                             const QRect &tileRect,
                                             const QSize &documentSize,
                                             const int level,
                                             const quint64 colourStateRevision,
                                             const quint64 spatialPlanFingerprint,
                                             const ColourProcessingCompatibility processingCompatibility) const
{
    quint64 hash = FnvOffset;
    hashValue(hash, level);
    hashValue(hash, tileRect.x());
    hashValue(hash, tileRect.y());
    hashValue(hash, tileRect.width());
    hashValue(hash, tileRect.height());
    hashValue(hash, documentSize.width());
    hashValue(hash, documentSize.height());
    hashValue(hash, colourStateRevision);
    hashValue(hash, spatialPlanFingerprint);
    hashValue(hash, static_cast<int>(processingCompatibility));
    hashLayerTree(hash, source, layers, tileRect, documentSize, QTransform());
    return nonZero(hash);
}

bool TiledCanvasEngine::prepareGpuHierarchy(const QImage &source,
                                            const QVector<LayerNode> &layers,
                                            const QRect &tileRect,
                                            const QSize &documentSize,
                                            QVector<PreparedTileLayer> *prepared,
                                            const std::atomic_bool *cancelRequested,
                                            const ColourProcessingCompatibility processingCompatibility,
                                            QString *error) const
{
    if (error) error->clear();
    return prepareTree(source,
                       layers,
                       tileRect,
                       documentSize,
                       QTransform(),
                       prepared,
                       m_webGpu,
                       cancelRequested,
                       processingCompatibility,
                       error);
}

QImage TiledCanvasEngine::renderInteractiveRegion(
    const QImage &source,
    const QVector<LayerNode> &layers,
    const QRect &previewRegion,
    const QSize &documentSize,
    const bool allowGpu,
    const int level,
    const std::atomic_bool *cancelRequested,
    RenderInfo *renderInfo,
    const QUuid &documentSessionId,
    const quint64 colourStateRevision,
    const ColourProcessingCompatibility processingCompatibility,
    const bool skipPersistentCacheFallback)
{
    RenderInfo localInfo;
    const auto cancelled = [&] {
        return cancelRequested
            && cancelRequested->load(std::memory_order_acquire);
    };
    const auto publishInfo = [&](const RenderInfo &info) {
        if (renderInfo) {
            *renderInfo = info;
        }
    };
    if (cancelled()) {
        localInfo.cancelled = true;
        localInfo.path = QStringLiteral(
            "Cancelled obsolete interactive composite request");
        publishInfo(localInfo);
        return {};
    }
    if (source.isNull() || previewRegion.isEmpty()) {
        localInfo.fallbackReason = QStringLiteral(
            "The interactive composite source or visible region is empty");
        publishInfo(localInfo);
        return {};
    }
    const QRect clipped = previewRegion.intersected(source.rect());
    if (clipped.isEmpty()) {
        localInfo.fallbackReason = QStringLiteral(
            "The interactive visible region does not intersect the preview source");
        publishInfo(localInfo);
        return {};
    }

    HierarchySummary hierarchy;
    try {
        analyseHierarchy(layers, &hierarchy);
    } catch (const std::bad_alloc &) {
        hierarchy = {};
    }
    localInfo.visiblePassThroughGroups = hierarchy.passThroughGroups;
    localInfo.visibleIsolatedGroups = hierarchy.isolatedGroups;
    localInfo.maximumGroupDepth = hierarchy.maximumDepth;

    QString fastPathError;
    const bool gpuEligible = allowGpu && m_webGpu && m_webGpu->deviceReady()
        && source.depth() <= 32;
    if (gpuEligible) {
        try {
            QVector<PreparedTileLayer> prepared;
            const QRect renderRect = spatialDependencyRect(source, layers, clipped, documentSize);
            if (prepareGpuHierarchy(source,
                                    layers,
                                    renderRect,
                                    documentSize,
                                    &prepared,
                                    cancelRequested,
                                    processingCompatibility,
                                    &fastPathError)) {
                trackSmartResidentTiles(documentSessionId, prepared);
                if (cancelled()) {
                    localInfo.cancelled = true;
                    localInfo.path = QStringLiteral(
                        "Cancelled obsolete interactive composite request");
                    publishInfo(localInfo);
                    return {};
                }
                QImage result = m_webGpu->compositeHierarchyTile(
                    renderRect.size(), source.colorSpace(), prepared, &fastPathError);
                if (!result.isNull()) {
                    if (renderRect != clipped) {
                        result = result.copy(QRect(clipped.topLeft() - renderRect.topLeft(),
                                                   clipped.size()));
                    }
                    result.setColorSpace(source.colorSpace());
                    localInfo.usedGpu = true;
                    localInfo.path = hierarchy.passThroughGroups > 0
                        ? QStringLiteral(
                              "Native WebGPU single-submit interactive hierarchy + Pass Through groups")
                        : QStringLiteral(
                              "Native WebGPU single-submit interactive hierarchy");
                    m_lastBackend = localInfo.path;
                    publishInfo(localInfo);
                    return result;
                }
            } else if (!cancelled() && fastPathError.isEmpty()) {
                fastPathError = treeContainsFeatheredVector(layers)
                    && (!m_webGpu || !m_webGpu->vectorFeatherGpuApproved())
                    ? QStringLiteral(
                          "Vector Feather uses the exact CPU fallback because "
                          "the native coverage kernel did not pass the startup parity gate")
                    : QStringLiteral(
                          "Interactive GPU hierarchy preparation failed; exact CPU fallback used");
            }
        } catch (const std::bad_alloc &) {
            fastPathError = QStringLiteral(
                "Interactive GPU hierarchy preparation exceeded available memory");
        }
    } else if (source.depth() > 32) {
        fastPathError = QStringLiteral(
            "16-bit documents use the exact bounded CPU compositor");
    } else if (!allowGpu
               && processingCompatibility == ColourProcessingCompatibility::ManagedV1
               && layerTreeRequiresManagedDomainTransform(layers)) {
        fastPathError = QStringLiteral(
            "Managed colour-domain adjustment uses the exact CPU reference because its GPU transform was unavailable or did not pass parity validation");
    } else if (!allowGpu) {
        fastPathError = QStringLiteral(
            "The selected adjustment hierarchy is not approved by the GPU parity gate");
    } else {
        fastPathError = QStringLiteral("The native WebGPU device is unavailable");
    }

    if (cancelled()) {
        localInfo.cancelled = true;
        localInfo.path = QStringLiteral(
            "Cancelled obsolete interactive composite request");
        publishInfo(localInfo);
        return {};
    }

    if (skipPersistentCacheFallback) {
        // Live paint requests are transient generations. Falling back through
        // the persistent tile renderer would copy/hash every contributing layer
        // region, expand small brush dabs to complete cache tiles and then
        // immediately obsolete that cache entry on the next pointer event.
        // Render the exact bounded region directly on the CPU instead.
        // ImageProcessor retains the same spatial halo, Alpha-safe hidden-RGB,
        // mask/group and 16-bit contracts.
        QImage fallback = ImageProcessor::renderRegion(
            source,
            layers,
            clipped,
            documentSize,
            cancelRequested,
            processingCompatibility);
        if (cancelled()) {
            localInfo.cancelled = true;
            localInfo.path = QStringLiteral(
                "Cancelled obsolete interactive composite request");
            publishInfo(localInfo);
            return {};
        }
        localInfo.usedCpu = !fallback.isNull();
        localInfo.path = QStringLiteral("CPU exact bounded interactive compositor");
        localInfo.fallbackReason = fastPathError;
        m_lastBackend = localInfo.path;
        publishInfo(localInfo);
        return fallback;
    }

    // Large/deep viewports can exceed the bounded one-submit working-set guard.
    // Non-transient interactive callers retain the established tiled fallback,
    // which can still use native WebGPU tile by tile under resource pressure.
    RenderInfo fallbackInfo;
    QImage fallback = renderRegion(source,
                                   layers,
                                   clipped,
                                   documentSize,
                                   allowGpu,
                                   level,
                                   cancelRequested,
                                   &fallbackInfo,
                                   documentSessionId,
                                   colourStateRevision,
                                   processingCompatibility);
    if (!fastPathError.isEmpty()) {
        if (fallbackInfo.fallbackReason.isEmpty()) {
            fallbackInfo.fallbackReason = fastPathError;
        } else if (!fallbackInfo.fallbackReason.contains(fastPathError)) {
            fallbackInfo.fallbackReason = fastPathError
                + QStringLiteral("; ") + fallbackInfo.fallbackReason;
        }
    }
    publishInfo(fallbackInfo);
    return fallback;
}

QImage TiledCanvasEngine::renderRegion(const QImage &source,
                                       const QVector<LayerNode> &layers,
                                       const QRect &previewRegion,
                                       const QSize &documentSize,
                                       const bool allowGpu,
                                       const int level,
                                       const std::atomic_bool *cancelRequested,
                                       RenderInfo *renderInfo,
                                       const QUuid &documentSessionId,
                                       const quint64 colourStateRevision,
                                       const ColourProcessingCompatibility processingCompatibility,
                                       const bool forceExactCpuReference)
{
    RenderInfo localInfo;
    const auto cancelled = [&] {
        return cancelRequested
            && cancelRequested->load(std::memory_order_acquire);
    };
    const auto publishInfo = [&](const RenderInfo &info) {
        if (renderInfo) {
            *renderInfo = info;
        }
    };
    const auto abandonCancelled = [&]() -> QImage {
        ++m_cancelledCompositeTiles;
        localInfo.cancelled = true;
        localInfo.path = QStringLiteral("Cancelled obsolete composite request");
        publishInfo(localInfo);
        return {};
    };

    if (cancelled()) {
        return abandonCancelled();
    }
    if (source.isNull() || previewRegion.isEmpty()) {
        localInfo.fallbackReason = QStringLiteral("The composite source or requested region is empty");
        publishInfo(localInfo);
        return {};
    }
    const QRect clipped = previewRegion.intersected(source.rect());
    if (clipped.isEmpty()) {
        localInfo.fallbackReason = QStringLiteral("The requested region does not intersect the preview source");
        publishInfo(localInfo);
        return {};
    }

    HierarchySummary hierarchy;
    try {
        analyseHierarchy(layers, &hierarchy);
    } catch (const std::bad_alloc &) {
        localInfo.fallbackReason = QStringLiteral(
            "Hierarchy analysis exceeded available memory; CPU reference fallback attempted");
        QImage fallback;
        try {
            fallback = ImageProcessor::renderRegion(source,
                                                    layers,
                                                    previewRegion,
                                                    documentSize,
                                                    cancelRequested,
                                                    processingCompatibility);
        } catch (const std::bad_alloc &) {
            fallback = {};
        }
        if (cancelled()) {
            return abandonCancelled();
        }
        localInfo.usedCpu = !fallback.isNull();
        localInfo.path = QStringLiteral("CPU tiled reference compositor");
        m_lastBackend = localInfo.path;
        publishInfo(localInfo);
        return fallback;
    }
    localInfo.visiblePassThroughGroups = hierarchy.passThroughGroups;
    localInfo.visibleIsolatedGroups = hierarchy.isolatedGroups;
    localInfo.maximumGroupDepth = hierarchy.maximumDepth;

    if (forceExactCpuReference) {
        QImage fallback;
        try {
            fallback = ImageProcessor::renderRegion(source,
                                                    layers,
                                                    clipped,
                                                    documentSize,
                                                    cancelRequested,
                                                    processingCompatibility);
        } catch (const std::bad_alloc &) {
            fallback = {};
        }
        if (cancelled()) {
            return abandonCancelled();
        }
        localInfo.usedCpu = !fallback.isNull();
        localInfo.path = QStringLiteral("CPU exact reference compositor");
        localInfo.fallbackReason = QStringLiteral(
            "Fractional adjustment or Live Filter coverage uses the exact CPU "
            "reference until that compositing path passes dedicated GPU parity");
        m_lastBackend = localInfo.path;
        publishInfo(localInfo);
        return fallback;
    }

    QImage result(clipped.size(), tiledWorkingFormat(source));
    if (result.isNull()) {
        localInfo.fallbackReason = QStringLiteral("The composite output allocation failed");
        publishInfo(localInfo);
        return {};
    }
    result.fill(Qt::transparent);
    result.setColorSpace(source.colorSpace());
    QPainter painter(&result);
    if (!painter.isActive()) {
        localInfo.fallbackReason = QStringLiteral("The composite output painter could not be created");
        publishInfo(localInfo);
        return {};
    }

    const QUuid surfaceId = compositeSurfaceId(source, documentSize);
    bool anyGpu = false;
    bool anyCpu = false;
    const bool passThroughPresent = hierarchy.passThroughGroups > 0;
    const bool gpuEligible = allowGpu && m_webGpu && m_webGpu->deviceReady()
        && source.depth() <= 32;
    if (!gpuEligible) {
        if (source.depth() > 32) {
            localInfo.fallbackReason = QStringLiteral("16-bit documents deliberately use the CPU reference compositor");
        } else if (!allowGpu
                   && processingCompatibility == ColourProcessingCompatibility::ManagedV1
                   && layerTreeRequiresManagedDomainTransform(layers)) {
            localInfo.fallbackReason = QStringLiteral(
                "Managed colour-domain adjustment uses the exact CPU reference because its GPU transform was unavailable or did not pass parity validation");
        } else if (!allowGpu) {
            localInfo.fallbackReason = QStringLiteral("Native canvas compositing is not approved by the startup parity gate");
        } else if (!m_webGpu || !m_webGpu->deviceReady()) {
            localInfo.fallbackReason = QStringLiteral("The native WebGPU device is unavailable");
        }
    }

    for (const QRect &tileRect : tileRectsForRegion(clipped, source.size())) {
        const SpatialFilterTilePlan spatialPlan = spatialPlanForRegion(
            source, layers, tileRect, documentSize);
        const QRect renderRect = spatialPlan.valid && !spatialPlan.dependencyBounds.isEmpty()
            ? spatialPlan.dependencyBounds : source.rect();
        if (cancelled()) {
            painter.end();
            return abandonCancelled();
        }
        const TileAddress address {surfaceId,
                                   tileRect.x() / TileSize,
                                   tileRect.y() / TileSize,
                                   std::max(0, level),
                                   TileDomain::Composite,
                                   documentSessionId};
        const quint64 revision = compositeRevision(source,
                                                     layers,
                                                     renderRect,
                                                     documentSize,
                                                     level,
                                                     colourStateRevision,
                                                     spatialPlan.cacheFingerprint,
                                                     processingCompatibility);
        QImage tile;
        if (const auto cached = m_cache.lookup(address, revision)) {
            tile = cached->image;
            anyGpu = anyGpu || cached->gpuResident;
            anyCpu = anyCpu || !cached->gpuResident;
        } else {
            m_cache.beginUpdate(address, revision);
            bool tileGpu = false;
            QString nativeError;
            if (gpuEligible) {
                constexpr int MaximumHierarchyNodes = 1024;
                constexpr int MaximumHierarchyDepth = 64;
                constexpr qsizetype MaximumHierarchyWorkingBytes = qsizetype(256) * 1024 * 1024;
                const qsizetype tileBytes = static_cast<qsizetype>(renderRect.width())
                    * static_cast<qsizetype>(renderRect.height()) * 4;
                const bool workingSetOverflow = hierarchy.estimatedTextureCount > 0
                    && tileBytes > std::numeric_limits<qsizetype>::max()
                        / hierarchy.estimatedTextureCount;
                const qsizetype estimatedWorkingBytes = workingSetOverflow
                    ? std::numeric_limits<qsizetype>::max()
                    : hierarchy.estimatedTextureCount * tileBytes;
                if (hierarchy.visibleNodes > MaximumHierarchyNodes
                    || hierarchy.maximumDepth > MaximumHierarchyDepth
                    || estimatedWorkingBytes > MaximumHierarchyWorkingBytes) {
                    nativeError = QStringLiteral(
                        "Native hierarchy resource guard rejected %1 visible nodes at depth %2 (%3 MiB estimated working textures); CPU fallback used")
                                      .arg(hierarchy.visibleNodes)
                                      .arg(hierarchy.maximumDepth)
                                      .arg(estimatedWorkingBytes / (1024.0 * 1024.0), 0, 'f', 1);
                } else {
                    try {
                        QVector<PreparedTileLayer> prepared;
                        if (prepareGpuHierarchy(source,
                                                layers,
                                                renderRect,
                                                documentSize,
                                                &prepared,
                                                cancelRequested,
                                                processingCompatibility,
                                                &nativeError)) {
                            trackSmartResidentTiles(address.documentSessionId, prepared);
                            if (cancelled()) {
                                m_cache.cancelUpdate(address, revision);
                                painter.end();
                                return abandonCancelled();
                            }
                            tile = m_webGpu->compositeHierarchyTile(renderRect.size(),
                                                                    source.colorSpace(),
                                                                    prepared,
                                                                    &nativeError);
                            if (!tile.isNull() && renderRect != tileRect) {
                                tile = tile.copy(QRect(tileRect.topLeft() - renderRect.topLeft(),
                                                      tileRect.size()));
                            }
                            tileGpu = !tile.isNull();
                        } else if (!cancelled() && nativeError.isEmpty()) {
                            nativeError = treeContainsFeatheredVector(layers)
                                && (!m_webGpu || !m_webGpu->vectorFeatherGpuApproved())
                                ? QStringLiteral(
                                      "Vector Feather uses the exact CPU fallback because "
                                      "the native coverage kernel did not pass the startup parity gate")
                                : QStringLiteral(
                                      "Native hierarchy preparation failed; exact CPU fallback used");
                        }
                    } catch (const std::bad_alloc &) {
                        nativeError = QStringLiteral("Native hierarchy preparation exceeded available memory");
                    }
                }
            }
            if (cancelled()) {
                m_cache.cancelUpdate(address, revision);
                painter.end();
                return abandonCancelled();
            }
            if (tile.isNull()) {
                if (localInfo.fallbackReason.isEmpty() && !nativeError.isEmpty()) {
                    localInfo.fallbackReason = nativeError;
                }
                tile = ImageProcessor::renderRegion(source,
                                                    layers,
                                                    tileRect,
                                                    documentSize,
                                                    cancelRequested,
                                                    processingCompatibility);
                tileGpu = false;
            }
            if (cancelled()) {
                m_cache.cancelUpdate(address, revision);
                painter.end();
                return abandonCancelled();
            }
            if (tile.isNull() || !m_cache.publish(address,
                                                   revision,
                                                   tile,
                                                   tileGpu,
                                                   tileGpu ? tile.sizeInBytes() : 0)) {
                m_cache.cancelUpdate(address, revision);
                painter.end();
                if (localInfo.fallbackReason.isEmpty()) {
                    localInfo.fallbackReason = tile.isNull()
                        ? QStringLiteral("Both native and CPU composite tile rendering failed")
                        : QStringLiteral("A newer composite tile generation superseded this request");
                }
                publishInfo(localInfo);
                return {};
            }
            if (!m_cache.markSynchronized(address, revision)) {
                m_cache.cancelUpdate(address, revision);
                painter.end();
                localInfo.fallbackReason = QStringLiteral("A newer composite tile generation superseded synchronization");
                publishInfo(localInfo);
                return {};
            }
            if (tileGpu) {
                cacheResidentTile(address, revision, tile);
                anyGpu = true;
            } else {
                anyCpu = true;
            }
        }

        if (tile.isNull()) {
            painter.end();
            localInfo.fallbackReason = QStringLiteral("A cached composite tile was unavailable");
            publishInfo(localInfo);
            return {};
        }
        const QRect overlap = tileRect.intersected(clipped);
        painter.drawImage(overlap.topLeft() - clipped.topLeft(),
                          tile,
                          QRect(overlap.topLeft() - tileRect.topLeft(), overlap.size()));
    }
    painter.end();
    if (cancelled()) {
        return abandonCancelled();
    }

    localInfo.usedGpu = anyGpu;
    localInfo.usedCpu = anyCpu;
    localInfo.mixedBackend = anyGpu && anyCpu;
    if (localInfo.mixedBackend) {
        localInfo.path = passThroughPresent
            ? QStringLiteral("Mixed native/CPU tiled hierarchy compositor + Pass Through groups")
            : QStringLiteral("Mixed native/CPU tiled hierarchy compositor");
        if (localInfo.fallbackReason.isEmpty()) {
            localInfo.fallbackReason = QStringLiteral("The current viewport reused one or more CPU-resident tiles");
        }
    } else if (anyGpu) {
        localInfo.path = passThroughPresent
            ? QStringLiteral("Native WebGPU tiled hierarchy compositor + WGSL adjustments + Pass Through groups")
            : QStringLiteral("Native WebGPU tiled hierarchy compositor + WGSL adjustments");
    } else {
        localInfo.path = passThroughPresent
            ? QStringLiteral("CPU tiled reference compositor + Pass Through groups")
            : QStringLiteral("CPU tiled reference compositor");
        if (gpuEligible && anyCpu && localInfo.fallbackReason.isEmpty()) {
            localInfo.fallbackReason = QStringLiteral(
                "The current viewport reused CPU-resident tiles from an earlier native fallback");
        }
    }
    m_lastBackend = localInfo.path;
    publishInfo(localInfo);
    return result;
}

TiledCanvasEngine::BrushResult TiledCanvasEngine::stampRasterStroke(
    const QImage &sourceRaster,
    const QSize &documentSize,
    const QColorSpace &colourSpace,
    const QUuid &layerId,
    const quint64 layerRevision,
    const QVector<QLineF> &documentSegments,
    const QTransform &documentToLayer,
    const double diameter,
    const double opacity,
    const double hardness,
    const QColor &colour,
    const bool erasing,
    const bool allowGpu,
    const QUuid &documentSessionId,
    const SelectionMask::Snapshot *selectionSnapshot,
    const QTransform &layerToDocument)
{
    BrushResult result;
    if (documentSize.isEmpty() || layerId.isNull() || documentSegments.isEmpty()) {
        result.error = QStringLiteral("Stroke input is empty");
        return result;
    }

    const bool sixteenBit = sourceRaster.depth() > 32;
    const QImage::Format rasterFormat = sixteenBit
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    QImage raster = sourceRaster;
    if (raster.isNull() || raster.size() != documentSize) {
        raster = QImage(documentSize, rasterFormat);
        raster.fill(Qt::transparent);
        raster.setColorSpace(colourSpace);
    } else {
        raster = raster.convertToFormat(rasterFormat);
        raster.detach();
    }

    const bool selectionActive = selectionSnapshot && selectionSnapshot->active;
    const QSize selectionSize = selectionActive
        ? selectionSnapshot->size : documentSize;
    SelectionMask activeSelection(selectionSize);
    if (selectionActive
        && (selectionSize.isEmpty()
            || !activeSelection.restoreSnapshot(*selectionSnapshot, false))) {
        result.error = QStringLiteral("The snapshotted selection could not be restored for the raster stroke");
        return result;
    }
    const bool applySelectionInEngine = selectionActive && !sixteenBit;
    const SelectionMask *selection = applySelectionInEngine ? &activeSelection : nullptr;
    const quint64 selectionRevision = selectionActive ? selectionSnapshot->revision : 0;
    if (selectionActive && activeSelection.isEmpty()) {
        result.image = raster;
        result.selectionApplied = true;
        m_lastBackend = QStringLiteral("Selection-aware brush skipped an active empty selection");
        return result;
    }

    const int tipSize = std::clamp(qCeil(diameter), 1, 4096);
    const double radius = tipSize * 0.5;
    const double spacing = std::max(1.0, radius * 0.22);
    const QVector<QPointF> stamps = strokeStampPoints(documentSegments, documentToLayer, spacing);
    const QRect affected = stampBounds(stamps, radius, documentSize);
    if (affected.isEmpty()) {
        result.image = raster;
        return result;
    }

    bool usedGpu = false;
    const bool gpuEligible = allowGpu && m_webGpu && m_webGpu->deviceReady()
        && !sixteenBit;
    for (const QRect &tileRect : tileRectsForRegion(affected, documentSize)) {
        QVector<QPointF> tileStamps;
        const QRectF expanded = QRectF(tileRect).adjusted(-radius, -radius, radius, radius);
        for (const QPointF &stamp : stamps) {
            if (expanded.contains(stamp)) {
                tileStamps.push_back(stamp);
            }
        }
        if (tileStamps.isEmpty()) {
            continue;
        }

        const TileAddress address {layerId,
                                   tileRect.x() / TileSize,
                                   tileRect.y() / TileSize,
                                   0,
                                   TileDomain::Raster,
                                   documentSessionId};
        const QImage sourceTile = raster.copy(tileRect);
        const QImage selectionCoverage = selectionCoverageTile(selection,
                                                                layerToDocument,
                                                                tileRect);
        const quint64 revision = strokeRevision(sourceTile,
                                                tileRect.topLeft(),
                                                tileStamps,
                                                diameter,
                                                opacity,
                                                hardness,
                                                colour,
                                                erasing,
                                                layerRevision,
                                                selectionRevision,
                                                selectionCoverage);
        m_cache.beginUpdate(address, revision);
        QImage tile;
        bool tileGpu = false;
        if (gpuEligible) {
            QString error;
            tile = m_webGpu->stampBrushTile(sourceTile,
                                            tileRect.topLeft(),
                                            tileStamps,
                                            radius,
                                            hardness,
                                            opacity,
                                            colour,
                                            erasing,
                                            tileResidencyKey(address),
                                            tileContentRevision(sourceTile),
                                            coverageAsRgba(selectionCoverage),
                                            &error);
            tileGpu = !tile.isNull();
            if (!tileGpu && result.error.isEmpty()) {
                result.error = error;
            }
        }
        if (tile.isNull()) {
            tile = stampRasterTileCpu(sourceTile,
                                      tileRect.topLeft(),
                                      tileStamps,
                                      radius,
                                      hardness,
                                      opacity,
                                      colour,
                                      erasing);
            if (tile.isNull()) {
                m_cache.cancelUpdate(address, revision);
                result.error = QStringLiteral("The straight raster tile could not be painted");
                return result;
            }
            if (applySelectionInEngine) {
                tile = applyRasterSelectionCoverage(sourceTile, tile, selectionCoverage);
                if (tile.isNull()) {
                    m_cache.cancelUpdate(address, revision);
                    result.error = QStringLiteral("The raster selection coverage could not be applied");
                    return result;
                }
            }
            tileGpu = false;
        }
        if (!m_cache.publish(address,
                             revision,
                             tile,
                             tileGpu,
                             tileGpu ? tile.sizeInBytes() : 0)
            || !m_cache.markSynchronized(address, revision)) {
            m_cache.cancelUpdate(address, revision);
            result.error = QStringLiteral("A newer raster tile generation superseded this stroke");
            return result;
        }
        if (tileGpu) {
            cacheResidentTile(address, tileContentRevision(tile), tile);
            usedGpu = true;
        }
        const int bytesPerPixel = sixteenBit ? 8 : 4;
        const int rowBytes = tile.width() * bytesPerPixel;
        for (int row = 0; row < tile.height(); ++row) {
            std::memcpy(raster.scanLine(tileRect.y() + row)
                            + tileRect.x() * bytesPerPixel,
                        tile.constScanLine(row),
                        static_cast<size_t>(rowBytes));
        }
    }

    result.image = raster;
    result.affectedRect = affected;
    result.usedGpu = usedGpu;
    result.selectionApplied = applySelectionInEngine;
    m_lastBackend = usedGpu
        ? (applySelectionInEngine
               ? QStringLiteral("Native WebGPU tiled selection-aware brush")
               : QStringLiteral("Native WebGPU tiled brush"))
        : (sixteenBit ? QStringLiteral("CPU tiled 16-bit brush reference")
                      : (applySelectionInEngine
                             ? QStringLiteral("CPU tiled selection-aware brush reference")
                             : QStringLiteral("CPU tiled brush reference")));
    return result;
}

TiledCanvasEngine::BrushResult TiledCanvasEngine::stampChannelStroke(
    const QImage &sourceRaster,
    const QSize &documentSize,
    const QColorSpace &colourSpace,
    const QUuid &layerId,
    const quint64 layerRevision,
    const QVector<QLineF> &documentSegments,
    const QTransform &documentToLayer,
    const double diameter,
    const double opacity,
    const double hardness,
    const int channelIndex,
    const int channelValue,
    const QUuid &documentSessionId)
{
    Q_UNUSED(layerRevision);
    Q_UNUSED(documentSessionId);
    BrushResult result;
    if (documentSize.isEmpty() || layerId.isNull() || documentSegments.isEmpty()
        || channelIndex < 0 || channelIndex > 3) {
        result.error = QStringLiteral("Channel stroke input is invalid");
        return result;
    }

    const bool sixteenBit = sourceRaster.depth() > 32;
    QImage raster;
    if (sourceRaster.isNull() || sourceRaster.size() != documentSize) {
        raster = QImage(documentSize,
                        sixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        raster.fill(Qt::transparent);
        raster.setColorSpace(colourSpace);
    } else {
        raster = sourceRaster.convertToFormat(sixteenBit
                                                  ? QImage::Format_RGBA64
                                                  : QImage::Format_RGBA8888);
        raster.detach();
    }

    const int tipSize = std::clamp(qCeil(diameter), 1, 4096);
    const double radius = tipSize * 0.5;
    const double spacing = std::max(1.0, radius * 0.22);
    const QVector<QPointF> stamps = strokeStampPoints(documentSegments, documentToLayer, spacing);
    const QRect affected = stampBounds(stamps, radius, documentSize);
    if (affected.isEmpty()) {
        result.image = raster;
        return result;
    }

    for (const QRect &tileRect : tileRectsForRegion(affected, documentSize)) {
        QVector<QPointF> tileStamps;
        const QRectF expanded = QRectF(tileRect).adjusted(-radius, -radius, radius, radius);
        for (const QPointF &stamp : stamps) {
            if (expanded.contains(stamp)) {
                tileStamps.push_back(stamp);
            }
        }
        if (tileStamps.isEmpty()) {
            continue;
        }
        const QImage tile = stampChannelTileCpu(raster.copy(tileRect),
                                                tileRect.topLeft(),
                                                tileStamps,
                                                radius,
                                                hardness,
                                                opacity,
                                                channelIndex,
                                                channelValue);
        if (tile.isNull()) {
            result.error = QStringLiteral("The straight channel tile could not be painted");
            return result;
        }
        const int rowBytes = tile.width() * (sixteenBit ? 8 : 4);
        for (int row = 0; row < tile.height(); ++row) {
            std::memcpy(raster.scanLine(tileRect.y() + row)
                            + tileRect.x() * (sixteenBit ? 8 : 4),
                        tile.constScanLine(row),
                        static_cast<size_t>(rowBytes));
        }
    }

    result.image = raster;
    result.affectedRect = affected;
    result.usedGpu = false;
    m_lastBackend = sixteenBit
        ? QStringLiteral("CPU tiled 16-bit straight-channel brush")
        : QStringLiteral("CPU tiled straight-channel brush");
    return result;
}

TiledCanvasEngine::BrushResult TiledCanvasEngine::stampMaskStroke(
    const QImage &sourceMask,
    const QSize &documentSize,
    const QUuid &layerId,
    const quint64 layerRevision,
    const QVector<QLineF> &documentSegments,
    const QTransform &documentToLayer,
    const double diameter,
    const double opacity,
    const double hardness,
    const int greyscaleValue,
    const bool restoringCoverage,
    const bool allowGpu,
    const QUuid &documentSessionId,
    const SelectionMask::Snapshot *selectionSnapshot,
    const QTransform &layerToDocument)
{
    BrushResult result;
    if (documentSize.isEmpty() || layerId.isNull() || documentSegments.isEmpty()
        || sourceMask.isNull()) {
        result.error = QStringLiteral("Mask stroke input is empty");
        return result;
    }

    QImage mask = materialisedMask(sourceMask, documentSize);
    if (mask.isNull()) {
        result.error = QStringLiteral("Mask pixels could not be materialised");
        return result;
    }
    mask.detach();

    const bool selectionActive = selectionSnapshot && selectionSnapshot->active;
    const QSize selectionSize = selectionActive
        ? selectionSnapshot->size : documentSize;
    SelectionMask activeSelection(selectionSize);
    if (selectionActive
        && (selectionSize.isEmpty()
            || !activeSelection.restoreSnapshot(*selectionSnapshot, false))) {
        result.error = QStringLiteral("The snapshotted selection could not be restored for the mask stroke");
        return result;
    }
    const SelectionMask *selection = selectionActive ? &activeSelection : nullptr;
    const quint64 selectionRevision = selectionActive ? selectionSnapshot->revision : 0;
    if (selectionActive && activeSelection.isEmpty()) {
        result.image = mask;
        result.selectionApplied = true;
        m_lastBackend = QStringLiteral("Selection-aware mask brush skipped an active empty selection");
        return result;
    }

    const int value = restoringCoverage ? 255 : std::clamp(greyscaleValue, 0, 255);
    const QColor paintColour(value, value, value, 255);
    const int tipSize = std::clamp(qCeil(diameter), 1, 4096);
    const double radius = tipSize * 0.5;
    const double spacing = std::max(1.0, radius * 0.22);
    const QVector<QPointF> stamps = strokeStampPoints(documentSegments, documentToLayer, spacing);
    const QRect affected = stampBounds(stamps, radius, documentSize);
    if (affected.isEmpty()) {
        result.image = mask;
        return result;
    }

    bool usedGpu = false;
    const bool gpuEligible = allowGpu && m_webGpu && m_webGpu->deviceReady();
    for (const QRect &tileRect : tileRectsForRegion(affected, documentSize)) {
        QVector<QPointF> tileStamps;
        const QRectF expanded = QRectF(tileRect).adjusted(-radius, -radius, radius, radius);
        for (const QPointF &stamp : stamps) {
            if (expanded.contains(stamp)) {
                tileStamps.push_back(stamp);
            }
        }
        if (tileStamps.isEmpty()) {
            continue;
        }

        const TileAddress address {layerId,
                                   tileRect.x() / TileSize,
                                   tileRect.y() / TileSize,
                                   0,
                                   TileDomain::Mask,
                                   documentSessionId};
        const QImage sourceTile = mask.copy(tileRect);
        const QImage sourceRgba = opaqueGreyscaleRgba(sourceTile);
        const QImage selectionCoverage = selectionCoverageTile(selection,
                                                                layerToDocument,
                                                                tileRect);
        const quint64 revision = strokeRevision(sourceRgba,
                                                tileRect.topLeft(),
                                                tileStamps,
                                                diameter,
                                                opacity,
                                                hardness,
                                                paintColour,
                                                false,
                                                layerRevision,
                                                selectionRevision,
                                                selectionCoverage);
        m_cache.beginUpdate(address, revision);
        QImage tile;
        bool tileGpu = false;
        if (gpuEligible) {
            QString error;
            const QImage rgbaTile = m_webGpu->stampBrushTile(sourceRgba,
                                                             tileRect.topLeft(),
                                                             tileStamps,
                                                             radius,
                                                             hardness,
                                                             opacity,
                                                             paintColour,
                                                             false,
                                                             tileResidencyKey(address),
                                                             tileContentRevision(sourceRgba),
                                                             coverageAsRgba(selectionCoverage),
                                                             &error);
            if (!rgbaTile.isNull()) {
                tile = rgbaTile.convertToFormat(QImage::Format_Grayscale8);
                tileGpu = !tile.isNull();
            }
            if (!tileGpu && result.error.isEmpty()) {
                result.error = error;
            }
        }
        if (tile.isNull()) {
            tile = stampMaskTileCpu(sourceTile,
                                    tileRect.topLeft(),
                                    tileStamps,
                                    radius,
                                    hardness,
                                    opacity,
                                    value);
            if (selectionActive) {
                tile = applyMaskSelectionCoverage(sourceTile, tile, selectionCoverage);
                if (tile.isNull()) {
                    m_cache.cancelUpdate(address, revision);
                    result.error = QStringLiteral("The mask selection coverage could not be applied");
                    return result;
                }
            }
            tileGpu = false;
        }
        if (!m_cache.publish(address,
                             revision,
                             tile,
                             tileGpu,
                             tileGpu ? tile.sizeInBytes() : 0)
            || !m_cache.markSynchronized(address, revision)) {
            m_cache.cancelUpdate(address, revision);
            result.error = QStringLiteral("A newer mask tile generation superseded this stroke");
            return result;
        }
        if (tileGpu) {
            cacheResidentTile(address, tileContentRevision(tile), tile);
            usedGpu = true;
        }
        for (int row = 0; row < tile.height(); ++row) {
            std::memcpy(mask.scanLine(tileRect.y() + row) + tileRect.x(),
                        tile.constScanLine(row),
                        static_cast<size_t>(tile.width()));
        }
    }

    result.image = mask;
    result.affectedRect = affected;
    result.usedGpu = usedGpu;
    result.selectionApplied = selectionActive;
    m_lastBackend = usedGpu
        ? (selectionActive
               ? QStringLiteral("Native WebGPU tiled selection-aware mask brush")
               : QStringLiteral("Native WebGPU tiled mask brush"))
        : (selectionActive
               ? QStringLiteral("CPU tiled selection-aware mask brush reference")
               : QStringLiteral("CPU tiled mask brush reference"));
    return result;
}



TiledCanvasEngine::FillResult TiledCanvasEngine::applyFillCoverage(
    const QImage &sourceImage,
    const QImage &coverage,
    const QUuid &layerId,
    const quint64 layerRevision,
    const FillTarget target,
    const int componentIndex,
    const QColor &colour,
    const bool preserveTransparency,
    const bool allowGpu,
    const QUuid &documentSessionId)
{
    FillResult result;
    if (sourceImage.isNull() || coverage.isNull()
        || sourceImage.size() != coverage.size() || layerId.isNull()) {
        result.error = QStringLiteral("Fill source, coverage or layer is invalid");
        return result;
    }

    const QImage coverageMask = coverage.convertToFormat(QImage::Format_Grayscale8);
    if (coverageMask.isNull()) {
        result.error = QStringLiteral("Fill coverage could not be normalised");
        return result;
    }

    QRect affected;
    for (int y = 0; y < coverageMask.height(); ++y) {
        const uchar *row = coverageMask.constScanLine(y);
        int first = -1;
        int last = -1;
        for (int x = 0; x < coverageMask.width(); ++x) {
            if (row[x] == 0) continue;
            if (first < 0) first = x;
            last = x;
        }
        if (first >= 0) {
            const QRect rowRect(first, y, last - first + 1, 1);
            affected = affected.isEmpty() ? rowRect : affected.united(rowRect);
        }
    }
    if (affected.isEmpty()) {
        result.image = target == FillTarget::Mask
            ? sourceImage.convertToFormat(QImage::Format_Grayscale8)
            : sourceImage.convertToFormat(sourceImage.depth() > 32
                                                ? QImage::Format_RGBA64
                                                : QImage::Format_RGBA8888);
        result.image.setColorSpace(sourceImage.colorSpace());
        return result;
    }

    const bool sixteenBit = sourceImage.depth() > 32;
    const QImage::Format outputFormat = target == FillTarget::Mask
        ? QImage::Format_Grayscale8
        : (sixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    QImage output = sourceImage.convertToFormat(outputFormat);
    if (output.isNull()) {
        result.error = QStringLiteral("Fill output allocation failed");
        return result;
    }
    output.detach();

    auto copyTile = [&](const QImage &tile, const QRect &tileRect) {
        if (outputFormat == QImage::Format_Grayscale8) {
            for (int row = 0; row < tile.height(); ++row) {
                std::memcpy(output.scanLine(tileRect.y() + row) + tileRect.x(),
                            tile.constScanLine(row),
                            static_cast<size_t>(tile.width()));
            }
            return;
        }
        const int bytesPerPixel = sixteenBit ? 8 : 4;
        for (int row = 0; row < tile.height(); ++row) {
            std::memcpy(output.scanLine(tileRect.y() + row)
                            + tileRect.x() * bytesPerPixel,
                        tile.constScanLine(row),
                        static_cast<size_t>(tile.width() * bytesPerPixel));
        }
    };

    auto tileDifference = [&](const QImage &before, const QImage &after,
                              const QRect &tileRect, QRect *changedBounds) {
        int changed = 0;
        QRect bounds;
        if (target == FillTarget::Mask) {
            const QImage beforeMask = before.convertToFormat(QImage::Format_Grayscale8);
            const QImage afterMask = after.convertToFormat(QImage::Format_Grayscale8);
            for (int y = 0; y < beforeMask.height(); ++y) {
                const uchar *a = beforeMask.constScanLine(y);
                const uchar *b = afterMask.constScanLine(y);
                for (int x = 0; x < beforeMask.width(); ++x) {
                    if (a[x] == b[x]) continue;
                    ++changed;
                    const QRect point(tileRect.x() + x, tileRect.y() + y, 1, 1);
                    bounds = bounds.isEmpty() ? point : bounds.united(point);
                }
            }
        } else if (sixteenBit) {
            const QImage beforeRgba = before.convertToFormat(QImage::Format_RGBA64);
            const QImage afterRgba = after.convertToFormat(QImage::Format_RGBA64);
            for (int y = 0; y < beforeRgba.height(); ++y) {
                const auto *a = reinterpret_cast<const QRgba64 *>(beforeRgba.constScanLine(y));
                const auto *b = reinterpret_cast<const QRgba64 *>(afterRgba.constScanLine(y));
                for (int x = 0; x < beforeRgba.width(); ++x) {
                    if (a[x] == b[x]) continue;
                    ++changed;
                    const QRect point(tileRect.x() + x, tileRect.y() + y, 1, 1);
                    bounds = bounds.isEmpty() ? point : bounds.united(point);
                }
            }
        } else {
            const QImage beforeRgba = before.convertToFormat(QImage::Format_RGBA8888);
            const QImage afterRgba = after.convertToFormat(QImage::Format_RGBA8888);
            for (int y = 0; y < beforeRgba.height(); ++y) {
                const uchar *a = beforeRgba.constScanLine(y);
                const uchar *b = afterRgba.constScanLine(y);
                for (int x = 0; x < beforeRgba.width(); ++x) {
                    if (std::memcmp(a + x * 4, b + x * 4, 4) == 0) continue;
                    ++changed;
                    const QRect point(tileRect.x() + x, tileRect.y() + y, 1, 1);
                    bounds = bounds.isEmpty() ? point : bounds.united(point);
                }
            }
        }
        if (changedBounds) *changedBounds = bounds;
        return changed;
    };

    const bool gpuEligible = allowGpu && !sixteenBit && m_webGpu
        && m_webGpu->deviceReady() && m_webGpu->fillGpuApproved();
    const QVector<QRect> dirtyTiles = tileRectsForRegion(affected, sourceImage.size());
    QString gpuFailure;
    bool anyGpu = false;
    QRect changedBounds;

    const auto resetPass = [&] {
        output = sourceImage.convertToFormat(outputFormat);
        if (!output.isNull()) output.detach();
        result.changedPixelCount = 0;
        changedBounds = {};
        anyGpu = false;
    };
    const auto applyPass = [&](const bool useGpu) {
        for (const QRect &tileRect : dirtyTiles) {
            const QImage coverageTile = coverageMask.copy(tileRect);
            bool hasCoverage = false;
            for (int y = 0; y < coverageTile.height() && !hasCoverage; ++y) {
                const uchar *row = coverageTile.constScanLine(y);
                for (int x = 0; x < coverageTile.width(); ++x) {
                    if (row[x] != 0) { hasCoverage = true; break; }
                }
            }
            if (!hasCoverage) continue;

            const QImage sourceTile = output.copy(tileRect);
            QImage editedTile;
            if (useGpu) {
                QImage gpuSource = sourceTile;
                if (target == FillTarget::Mask) {
                    gpuSource = sourceTile.convertToFormat(QImage::Format_Grayscale8)
                                    .convertToFormat(QImage::Format_RGBA8888);
                }
                editedTile = m_webGpu->applyFillTile(gpuSource,
                                                      coverageTile,
                                                      target,
                                                      componentIndex,
                                                      colour,
                                                      preserveTransparency,
                                                      &gpuFailure);
                if (!editedTile.isNull()) {
                    editedTile = target == FillTarget::Mask
                        ? editedTile.convertToFormat(QImage::Format_Grayscale8)
                        : editedTile.convertToFormat(QImage::Format_RGBA8888);
                }
                if (editedTile.isNull()) {
                    if (gpuFailure.isEmpty()) {
                        gpuFailure = QStringLiteral(
                            "the native Fill tile could not be converted");
                    }
                    return false;
                }
            } else {
                const FillApplyResult cpu = applyFillCoverageCpu(sourceTile,
                                                                  coverageTile,
                                                                  target,
                                                                  componentIndex,
                                                                  colour,
                                                                  preserveTransparency);
                if (!cpu.succeeded()) {
                    result.error = cpu.error;
                    return false;
                }
                editedTile = cpu.image;
            }

            QRect tileChangedBounds;
            const int changed = tileDifference(sourceTile, editedTile, tileRect,
                                               &tileChangedBounds);
            if (changed == 0) continue;
            result.changedPixelCount += changed;
            changedBounds = changedBounds.isEmpty()
                ? tileChangedBounds : changedBounds.united(tileChangedBounds);
            copyTile(editedTile, tileRect);
            anyGpu = anyGpu || useGpu;
        }
        return true;
    };

    if (gpuEligible && !applyPass(true)) {
        // Never publish a mixture of GPU-quantised and CPU-exact tiles. Any
        // native failure discards the provisional pass and reruns the complete
        // bounded operation through the deterministic reference.
        resetPass();
        if (!applyPass(false)) {
            return result;
        }
    } else if (!gpuEligible && !applyPass(false)) {
        return result;
    }

    output.setColorSpace(sourceImage.colorSpace());
    output.setDevicePixelRatio(sourceImage.devicePixelRatio());
    output.setDotsPerMeterX(sourceImage.dotsPerMeterX());
    output.setDotsPerMeterY(sourceImage.dotsPerMeterY());
    result.image = output;
    result.affectedRect = changedBounds;
    result.usedGpu = anyGpu;
    if (result.changedPixelCount == 0) {
        result.error.clear();
    }
    m_lastBackend = anyGpu
        ? QStringLiteral("Native WebGPU tiled Fill application")
        : (!gpuFailure.isEmpty()
               ? QStringLiteral("CPU tiled Fill reference (native operation fallback: %1)")
                     .arg(gpuFailure)
               : sixteenBit
                   ? QStringLiteral("CPU tiled 16-bit Fill reference")
                   : QStringLiteral("CPU tiled Fill reference"));
    Q_UNUSED(layerRevision);
    Q_UNUSED(documentSessionId);
    return result;
}


TiledCanvasEngine::FillResult TiledCanvasEngine::applyGradient(
    const GradientApplyRequest &request,
    const QUuid &layerId,
    const quint64 layerRevision,
    const bool allowGpu,
    const QUuid &documentSessionId)
{
    FillResult result;
    if (request.sourceImage.isNull() || request.selectionCoverage.isNull()
        || request.sourceImage.size() != request.selectionCoverage.size()
        || layerId.isNull()) {
        result.error = QStringLiteral("Gradient source, selection coverage or layer is invalid");
        return result;
    }

    const QImage coverage = request.selectionCoverage.convertToFormat(QImage::Format_Grayscale8);
    if (coverage.isNull()) {
        result.error = QStringLiteral("Gradient coverage could not be normalised");
        return result;
    }
    QRect affected;
    for (int y = 0; y < coverage.height(); ++y) {
        const uchar *row = coverage.constScanLine(y);
        int first = -1;
        int last = -1;
        for (int x = 0; x < coverage.width(); ++x) {
            if (row[x] == 0) continue;
            if (first < 0) first = x;
            last = x;
        }
        if (first >= 0) {
            const QRect rowRect(first, y, last - first + 1, 1);
            affected = affected.isEmpty() ? rowRect : affected.united(rowRect);
        }
    }

    const bool sixteenBit = request.sourceImage.depth() > 32;
    const QImage::Format outputFormat = request.target == FillTarget::Mask
        ? QImage::Format_Grayscale8
        : (sixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    QImage output = request.sourceImage.convertToFormat(outputFormat);
    if (output.isNull()) {
        result.error = QStringLiteral("Gradient output allocation failed");
        return result;
    }
    output.detach();
    if (affected.isEmpty()) {
        result.image = output;
        return result;
    }

    auto copyTile = [&](const QImage &tile, const QRect &tileRect) {
        const int bytesPerPixel = outputFormat == QImage::Format_Grayscale8
            ? 1 : (sixteenBit ? 8 : 4);
        for (int row = 0; row < tile.height(); ++row) {
            std::memcpy(output.scanLine(tileRect.y() + row)
                            + tileRect.x() * bytesPerPixel,
                        tile.constScanLine(row),
                        static_cast<size_t>(tile.width() * bytesPerPixel));
        }
    };
    auto compareTile = [&](const QImage &before, const QImage &after,
                           const QRect &tileRect, QRect *bounds) {
        int changed = 0;
        QRect localBounds;
        if (request.target == FillTarget::Mask) {
            const QImage a = before.convertToFormat(QImage::Format_Grayscale8);
            const QImage b = after.convertToFormat(QImage::Format_Grayscale8);
            for (int y = 0; y < a.height(); ++y) {
                for (int x = 0; x < a.width(); ++x) {
                    if (a.constScanLine(y)[x] == b.constScanLine(y)[x]) continue;
                    ++changed;
                    const QRect point(tileRect.x() + x, tileRect.y() + y, 1, 1);
                    localBounds = localBounds.isEmpty() ? point : localBounds.united(point);
                }
            }
        } else if (sixteenBit) {
            const QImage a = before.convertToFormat(QImage::Format_RGBA64);
            const QImage b = after.convertToFormat(QImage::Format_RGBA64);
            for (int y = 0; y < a.height(); ++y) {
                const auto *ar = reinterpret_cast<const QRgba64 *>(a.constScanLine(y));
                const auto *br = reinterpret_cast<const QRgba64 *>(b.constScanLine(y));
                for (int x = 0; x < a.width(); ++x) {
                    if (ar[x] == br[x]) continue;
                    ++changed;
                    const QRect point(tileRect.x() + x, tileRect.y() + y, 1, 1);
                    localBounds = localBounds.isEmpty() ? point : localBounds.united(point);
                }
            }
        } else {
            const QImage a = before.convertToFormat(QImage::Format_RGBA8888);
            const QImage b = after.convertToFormat(QImage::Format_RGBA8888);
            for (int y = 0; y < a.height(); ++y) {
                const uchar *ar = a.constScanLine(y);
                const uchar *br = b.constScanLine(y);
                for (int x = 0; x < a.width(); ++x) {
                    if (std::memcmp(ar + x * 4, br + x * 4, 4) == 0) continue;
                    ++changed;
                    const QRect point(tileRect.x() + x, tileRect.y() + y, 1, 1);
                    localBounds = localBounds.isEmpty() ? point : localBounds.united(point);
                }
            }
        }
        if (bounds) *bounds = localBounds;
        return changed;
    };

    const QVector<QRect> dirtyTiles = tileRectsForRegion(affected, request.sourceImage.size());
    const bool gpuEligible = allowGpu && !sixteenBit && m_webGpu
        && m_webGpu->deviceReady() && m_webGpu->gradientGpuApproved();
    QString gpuFailure;
    QRect changedBounds;
    bool anyGpu = false;
    const auto resetPass = [&] {
        output = request.sourceImage.convertToFormat(outputFormat);
        if (!output.isNull()) output.detach();
        result.changedPixelCount = 0;
        changedBounds = {};
        anyGpu = false;
    };
    const auto applyPass = [&](const bool useGpu) {
        for (const QRect &tileRect : dirtyTiles) {
            const QImage coverageTile = coverage.copy(tileRect);
            bool hasCoverage = false;
            for (int y = 0; y < coverageTile.height() && !hasCoverage; ++y) {
                const uchar *row = coverageTile.constScanLine(y);
                for (int x = 0; x < coverageTile.width(); ++x) {
                    if (row[x] != 0) { hasCoverage = true; break; }
                }
            }
            if (!hasCoverage) continue;
            const QImage sourceTile = output.copy(tileRect);
            QImage editedTile;
            if (useGpu) {
                QImage gpuSource = sourceTile;
                if (request.target == FillTarget::Mask) {
                    gpuSource = sourceTile.convertToFormat(QImage::Format_Grayscale8)
                                    .convertToFormat(QImage::Format_RGBA8888);
                }
                editedTile = m_webGpu->applyGradientTile(gpuSource,
                                                          coverageTile,
                                                          tileRect.topLeft(),
                                                          request.target,
                                                          request.componentIndex,
                                                          request.start,
                                                          request.end,
                                                          request.type,
                                                          request.startColour,
                                                          request.endColour,
                                                          request.reverse,
                                                          &gpuFailure);
                if (!editedTile.isNull()) {
                    editedTile = request.target == FillTarget::Mask
                        ? editedTile.convertToFormat(QImage::Format_Grayscale8)
                        : editedTile.convertToFormat(QImage::Format_RGBA8888);
                }
                if (editedTile.isNull()) return false;
            } else {
                GradientApplyRequest tileRequest = request;
                tileRequest.sourceImage = sourceTile;
                tileRequest.selectionCoverage = coverageTile;
                tileRequest.start -= QPointF(tileRect.topLeft());
                tileRequest.end -= QPointF(tileRect.topLeft());
                const GradientApplyResult cpu = applyGradientCpu(tileRequest);
                if (!cpu.succeeded()) {
                    result.error = cpu.error;
                    return false;
                }
                editedTile = cpu.image;
            }
            QRect tileBounds;
            const int changed = compareTile(sourceTile, editedTile, tileRect, &tileBounds);
            if (changed == 0) continue;
            result.changedPixelCount += changed;
            changedBounds = changedBounds.isEmpty() ? tileBounds : changedBounds.united(tileBounds);
            copyTile(editedTile, tileRect);
            anyGpu = anyGpu || useGpu;
        }
        return true;
    };

    if (gpuEligible && !applyPass(true)) {
        resetPass();
        if (!applyPass(false)) return result;
    } else if (!gpuEligible && !applyPass(false)) {
        return result;
    }

    output.setColorSpace(request.sourceImage.colorSpace());
    output.setDevicePixelRatio(request.sourceImage.devicePixelRatio());
    output.setDotsPerMeterX(request.sourceImage.dotsPerMeterX());
    output.setDotsPerMeterY(request.sourceImage.dotsPerMeterY());
    result.image = output;
    result.affectedRect = changedBounds;
    result.usedGpu = anyGpu;
    m_lastBackend = anyGpu
        ? QStringLiteral("Native WebGPU tiled Gradient application")
        : (!gpuFailure.isEmpty()
               ? QStringLiteral("CPU tiled Gradient reference (native operation fallback: %1)").arg(gpuFailure)
               : sixteenBit
                   ? QStringLiteral("CPU tiled 16-bit Gradient reference")
                   : QStringLiteral("CPU tiled Gradient reference"));
    Q_UNUSED(layerRevision);
    Q_UNUSED(documentSessionId);
    return result;
}

TiledCanvasEngine::BrushResult TiledCanvasEngine::stampCloneStroke(
    const CloneStampRequest &request,
    const QSize &documentSize,
    const QUuid &layerId,
    const quint64 layerRevision,
    const bool allowGpu,
    const QUuid &documentSessionId,
    const SelectionMask::Snapshot *selectionSnapshot)
{
    BrushResult result;
    if (request.destination.isNull() || request.source.isNull()
        || request.targetSegments.isEmpty() || documentSize.isEmpty()
        || layerId.isNull()) {
        result.error = QStringLiteral("Clone Stamp input is empty");
        return result;
    }
    if (request.target == CloneStampTarget::ComponentChannel
        && (request.componentIndex < 0 || request.componentIndex > 3)) {
        result.error = QStringLiteral("Clone Stamp component target is invalid");
        return result;
    }

    const bool targetMask = request.target == CloneStampTarget::Mask;
    const bool sixteenBit = (!targetMask && request.destination.depth() > 32)
        || request.source.depth() > 32
        || request.source.format() == QImage::Format_Grayscale16;
    const QImage::Format destinationFormat = targetMask
        ? QImage::Format_Grayscale8
        : (request.destination.depth() > 32 ? QImage::Format_RGBA64
                                             : QImage::Format_RGBA8888);
    QImage destination = request.destination.convertToFormat(destinationFormat);
    if (destination.isNull()) {
        result.error = QStringLiteral("Clone Stamp destination could not be prepared");
        return result;
    }
    destination.detach();

    const bool selectionActive = selectionSnapshot && selectionSnapshot->active;
    const QSize selectionSize = selectionActive
        ? selectionSnapshot->size : documentSize;
    SelectionMask activeSelection(selectionSize);
    if (selectionActive
        && (selectionSize.isEmpty()
            || !activeSelection.restoreSnapshot(*selectionSnapshot, false))) {
        result.error = QStringLiteral("The snapshotted selection could not be restored for Clone Stamp");
        return result;
    }
    if (selectionActive && activeSelection.isEmpty()) {
        result.image = destination;
        result.selectionApplied = true;
        m_lastBackend = QStringLiteral("Selection-aware Clone Stamp skipped an active empty selection");
        return result;
    }

    const double radius = std::max(0.5, request.diameterPixels * 0.5);
    const double spacing = std::max(1.0, radius * 0.22);
    const QVector<QPointF> stamps = strokeStampPoints(request.targetSegments,
                                                      QTransform(),
                                                      spacing);
    const QRect affected = cloneStampBounds(stamps, radius, destination.size());
    if (affected.isEmpty()) {
        result.image = destination;
        return result;
    }

    const auto cpuFallback = [&](const QString &reason) {
        const CloneStampResult cpu = applyCloneStamp(request);
        BrushResult fallback;
        fallback.image = cpu.image;
        fallback.affectedRect = cpu.affectedRect;
        fallback.error = cpu.error;
        fallback.usedGpu = false;
        fallback.selectionApplied = false;
        m_lastBackend = reason.isEmpty()
            ? QStringLiteral("CPU tiled Clone Stamp reference")
            : QStringLiteral("CPU tiled Clone Stamp reference (native fallback: %1)")
                  .arg(reason);
        return fallback;
    };

    const bool affineTransforms = request.targetPixelToLayer.isAffine()
        && request.targetLayerToDocument.isAffine()
        && request.sourceDocumentToLayer.isAffine()
        && request.sourceLayerToPixel.isAffine();
    const bool gpuEligible = allowGpu && m_webGpu && m_webGpu->deviceReady()
        && !sixteenBit && affineTransforms;
    if (!gpuEligible) {
        QString reason;
        if (sixteenBit) {
            reason = QStringLiteral("16-bit Clone Stamp remains on the CPU reference path");
        } else if (!affineTransforms) {
            reason = QStringLiteral("projective Clone Stamp transforms require the CPU reference path");
        } else {
            reason = QStringLiteral("native WebGPU is unavailable");
        }
        return cpuFallback(reason);
    }

    const bool sourceIsGrey = request.source.format() == QImage::Format_Grayscale8
        || request.source.format() == QImage::Format_Grayscale16;
    const SelectionMask *selection = selectionActive ? &activeSelection : nullptr;
    const quint64 selectionRevision = selectionActive ? selectionSnapshot->revision : 0;
    struct PendingCloneTile {
        TileAddress address;
        quint64 revision = 0;
        QRect rect;
        QImage image;
    };
    const QVector<QRect> dirtyTileRects = tileRectsForRegion(affected,
                                                              destination.size());
    qsizetype pendingByteEstimate = 0;
    for (const QRect &tileRect : dirtyTileRects) {
        const qsizetype tileBytes = qsizetype(tileRect.width())
            * qsizetype(tileRect.height()) * 4;
        if (tileBytes > MaximumClonePendingBytes - pendingByteEstimate) {
            return cpuFallback(QStringLiteral(
                "the provisional Clone Stamp tile transaction exceeded its memory guard"));
        }
        pendingByteEstimate += tileBytes;
    }

    QVector<PendingCloneTile> pendingTiles;
    pendingTiles.reserve(dirtyTileRects.size());
    QString gpuFailure;

    for (const QRect &tileRect : dirtyTileRects) {
        QVector<QPointF> tileStamps;
        const QRectF expanded = QRectF(tileRect).adjusted(-radius, -radius,
                                                          radius, radius);
        for (const QPointF &stamp : stamps) {
            if (expanded.contains(stamp)) {
                tileStamps.push_back(stamp);
            }
        }
        if (tileStamps.isEmpty()) {
            continue;
        }

        const CloneSourcePatch sourcePatch = cloneSourcePatchForTile(request, tileRect);
        if (sourcePatch.exceededGuard) {
            gpuFailure = QStringLiteral(
                "the transformed source footprint exceeded the guarded GPU patch size");
            break;
        }
        if (sourcePatch.image.isNull()) {
            gpuFailure = QStringLiteral("the immutable source patch could not be prepared");
            break;
        }

        const TileDomain domain = targetMask ? TileDomain::Mask : TileDomain::Raster;
        const TileAddress address {layerId,
                                   tileRect.x() / TileSize,
                                   tileRect.y() / TileSize,
                                   0,
                                   domain,
                                   documentSessionId};
        const QImage destinationTile = destination.copy(tileRect);
        const QImage selectionCoverage = cloneSelectionCoverageTile(
            selection, request, tileRect);
        if (selectionCoverage.isNull()
            || selectionCoverage.size() != tileRect.size()) {
            gpuFailure = QStringLiteral(
                "the snapshotted selection tile could not be prepared");
            break;
        }
        const quint64 revision = cloneStrokeRevision(destinationTile,
                                                      sourcePatch,
                                                      tileRect,
                                                      tileStamps,
                                                      request,
                                                      layerRevision,
                                                      selectionRevision,
                                                      selectionCoverage);
        m_cache.beginUpdate(address, revision);

        QString error;
        QImage tile = m_webGpu->stampCloneTile(
            destinationTile,
            tileRect.topLeft(),
            sourcePatch.image,
            sourcePatch.origin,
            request.source.size(),
            tileStamps,
            request.targetPixelToLayer,
            request.targetLayerToDocument,
            request.sourceDocumentToLayer,
            request.sourceLayerToPixel,
            request.sourceOffsetDocument,
            radius,
            request.hardness,
            request.opacity,
            request.target,
            request.sample,
            request.componentIndex,
            sourceIsGrey,
            coverageAsRgba(selectionCoverage),
            &error);
        if (tile.isNull()) {
            m_cache.cancelUpdate(address, revision);
            gpuFailure = error.isEmpty()
                ? QStringLiteral("the Clone Stamp compute dispatch failed") : error;
            break;
        }
        if (targetMask) {
            tile = tile.convertToFormat(QImage::Format_Grayscale8);
            if (tile.isNull()) {
                m_cache.cancelUpdate(address, revision);
                gpuFailure = QStringLiteral("the GPU mask clone tile could not be converted");
                break;
            }
        }
        pendingTiles.push_back({address, revision, tileRect, tile});
    }

    if (!gpuFailure.isEmpty()) {
        for (const PendingCloneTile &pending : pendingTiles) {
            m_cache.cancelUpdate(pending.address, pending.revision);
        }
        return cpuFallback(gpuFailure);
    }

    for (const PendingCloneTile &pending : pendingTiles) {
        if (!m_cache.publish(pending.address,
                             pending.revision,
                             pending.image,
                             true,
                             pending.image.sizeInBytes())
            || !m_cache.markSynchronized(pending.address, pending.revision)) {
            // Publication is transactional at the document level. A stale
            // generation may be discovered only after an earlier tile was
            // synchronized, so cancel every outstanding update and invalidate
            // the whole surface before the CPU reference reruns the stroke.
            // This prevents a partially published Clone Stamp from surviving
            // in either the shared CPU cache or native resident-tile cache.
            for (const PendingCloneTile &remaining : pendingTiles) {
                m_cache.cancelUpdate(remaining.address, remaining.revision);
            }
            m_cache.invalidateSurface(documentSessionId, layerId);
            invalidateResidentSurface(documentSessionId, layerId);
            return cpuFallback(QStringLiteral(
                "a newer tile generation superseded the Clone Stamp transaction"));
        }
    }

    for (const PendingCloneTile &pending : pendingTiles) {
        cacheResidentTile(pending.address,
                          tileContentRevision(pending.image),
                          pending.image);
        if (targetMask) {
            for (int row = 0; row < pending.image.height(); ++row) {
                std::memcpy(destination.scanLine(pending.rect.y() + row)
                                + pending.rect.x(),
                            pending.image.constScanLine(row),
                            static_cast<size_t>(pending.image.width()));
            }
        } else {
            const int rowBytes = pending.image.width() * 4;
            for (int row = 0; row < pending.image.height(); ++row) {
                std::memcpy(destination.scanLine(pending.rect.y() + row)
                                + pending.rect.x() * 4,
                            pending.image.constScanLine(row),
                            static_cast<size_t>(rowBytes));
            }
        }
    }

    destination.setColorSpace(request.destination.colorSpace());
    result.image = destination;
    result.affectedRect = affected;
    result.usedGpu = !pendingTiles.isEmpty();
    result.selectionApplied = true;
    m_lastBackend = selectionActive
        ? QStringLiteral("Native WebGPU tiled selection-aware Clone Stamp")
        : QStringLiteral("Native WebGPU tiled Clone Stamp");
    return result;
}

QString TiledCanvasEngine::residentSurfaceKey(const QUuid &documentSessionId,
                                                const QUuid &surfaceId)
{
    return documentSessionId.toString(QUuid::WithoutBraces)
        + QLatin1Char('/') + surfaceId.toString(QUuid::WithoutBraces);
}

void TiledCanvasEngine::trackSmartResidentTiles(
    const QUuid &documentSessionId,
    const QVector<PreparedTileLayer> &prepared)
{
    if (!m_webGpu || documentSessionId.isNull() || prepared.isEmpty()) return;
    QSet<quint64> keys;
    collectPreparedResidentKeys(prepared, &keys);
    if (keys.isEmpty()) return;
    m_residentKeysBySurface[residentSurfaceKey(
        documentSessionId, smartIntermediateResidentSurfaceId())].unite(keys);
}

void TiledCanvasEngine::cacheResidentTile(const TileAddress &address,
                                          const quint64 revision,
                                          const QImage &image)
{
    if (!m_webGpu || address.surfaceId.isNull() || image.isNull()) {
        return;
    }
    const quint64 key = tileResidencyKey(address);
    m_webGpu->cacheResidentTile(key, revision, image);
    m_residentKeysBySurface[residentSurfaceKey(address.documentSessionId,
                                               address.surfaceId)].insert(key);
}

void TiledCanvasEngine::invalidateResidentSurface(const QUuid &documentSessionId,
                                                   const QUuid &surfaceId)
{
    if (!m_webGpu || surfaceId.isNull()) {
        return;
    }
    const QSet<quint64> keys = m_residentKeysBySurface.take(
        residentSurfaceKey(documentSessionId, surfaceId));
    for (const quint64 key : keys) {
        m_webGpu->invalidateResidentTile(key);
    }
}

void TiledCanvasEngine::invalidateResidentSession(const QUuid &documentSessionId)
{
    if (!m_webGpu) {
        return;
    }
    const QString prefix = documentSessionId.toString(QUuid::WithoutBraces)
        + QLatin1Char('/');
    const auto surfaces = m_residentKeysBySurface.keys();
    for (const QString &surfaceKey : surfaces) {
        if (!surfaceKey.startsWith(prefix)) {
            continue;
        }
        const QSet<quint64> keys = m_residentKeysBySurface.take(surfaceKey);
        for (const quint64 key : keys) {
            m_webGpu->invalidateResidentTile(key);
        }
    }
}

void TiledCanvasEngine::invalidateSurface(const QUuid &surfaceId)
{
    invalidateSurface(QUuid(), surfaceId);
}

void TiledCanvasEngine::invalidateSurface(const QUuid &documentSessionId,
                                          const QUuid &surfaceId)
{
    m_cache.invalidateSurface(documentSessionId, surfaceId);
    invalidateResidentSurface(documentSessionId, surfaceId);
}

void TiledCanvasEngine::invalidateSession(const QUuid &documentSessionId)
{
    m_cache.invalidateSession(documentSessionId);
    invalidateResidentSession(documentSessionId);
}

void TiledCanvasEngine::clear()
{
    if (m_webGpu) {
        const auto surfaces = m_residentKeysBySurface.keys();
        for (const QString &surfaceKey : surfaces) {
            const QSet<quint64> keys = m_residentKeysBySurface.take(surfaceKey);
            for (const quint64 key : keys) {
                m_webGpu->invalidateResidentTile(key);
            }
        }
    }
    m_residentKeysBySurface.clear();
    m_cache.clear();
}

TileCache::Stats TiledCanvasEngine::cacheStats() const
{
    return m_cache.stats();
}

TileCache::Stats TiledCanvasEngine::cacheStatsForSession(
    const QUuid &documentSessionId) const
{
    return m_cache.statsForSession(documentSessionId);
}

QString TiledCanvasEngine::lastBackendText() const
{
    return m_lastBackend;
}

quint64 TiledCanvasEngine::cancelledCompositeTiles() const
{
    return m_cancelledCompositeTiles;
}

} // namespace vfx
