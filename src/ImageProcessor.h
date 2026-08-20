#pragma once

#include "Adjustment.h"
#include "ColourManagement.h"

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSet>
#include <QSize>
#include <QTransform>
#include <QVector>
#include <QUuid>

#include <atomic>

namespace vfx {


struct LayerEffectRenderPass {
    QImage image;
    BlendMode blendMode = BlendMode::Copy;
    double opacity = 1.0;
    bool behindSource = true;
    QUuid effectId;
};

struct LayerEffectInputRegion {
    // Post-content/Smart-transform/Live-Filter pixels before the owning layer
    // mask, opacity and blend mode. Layer Effect renderers generate appearance
    // relative to these pixels rather than mutating them.
    QImage content;
    // Effective alpha coverage after the owning layer mask is evaluated. This
    // is kept separate so shadows/glows can derive from the visible silhouette
    // without baking the mask into authoritative layer pixels.
    QImage coverage;

    bool isValid() const
    {
        return !content.isNull() && !coverage.isNull()
            && content.size() == coverage.size();
    }
};

class ImageProcessor final {
public:
    static QImage render(const QImage &source,
                         const QVector<LayerNode> &layers,
                         const std::atomic_bool *cancelRequested = nullptr,
                         const QSize &documentSize = {},
                         ColourProcessingCompatibility processingCompatibility =
                             ColourProcessingCompatibility::LegacyV1);

    // Evaluate only a preview-space rectangle. This is the CPU fallback for
    // the tile scheduler and the exact contract used by the WGSL backend.
    static QImage renderRegion(const QImage &source,
                               const QVector<LayerNode> &layers,
                               const QRect &previewRegion,
                               const QSize &documentSize = {},
                               const std::atomic_bool *cancelRequested = nullptr,
                               ColourProcessingCompatibility processingCompatibility =
                                   ColourProcessingCompatibility::LegacyV1);

    // Exact Smart-instance mask transform used by both CPU composition and the
    // native hierarchy preparation path. The result is a tile-local grayscale
    // coverage image and participates in the bounded Smart intermediate cache.
    static QImage renderSmartMaskRegion(
        const QImage &mask,
        const QUuid &cacheIdentity,
        const QSize &referenceSize,
        const QPointF &referenceOrigin,
        const QSize &previewSize,
        const QTransform &worldTransform,
        const QSize &documentSize,
        const QRect &region,
        bool enabled,
        bool inverted,
        const SmartTransformState &smartTransform,
        const std::atomic_bool *cancelRequested = nullptr);

    // Transform previews may need selected pixels whose document-space
    // bounds lie partly or wholly beyond the canvas. Unlike renderRegion(),
    // this CPU reference path deliberately does not intersect previewRegion
    // with source.rect(); the returned image is exactly previewRegion.size().
    // A spatial-filter halo is still evaluated before cropping the requested
    // result so adjustment/group semantics remain exact.
    static QImage renderUnclippedRegion(
        const QImage &source,
        const QVector<LayerNode> &layers,
        const QRect &previewRegion,
        const QSize &documentSize = {},
        const std::atomic_bool *cancelRequested = nullptr,
        ColourProcessingCompatibility processingCompatibility =
            ColourProcessingCompatibility::LegacyV1);

    // Smart Source presentation generation uses the same unclipped geometry
    // contract but must also retain hidden RGB at zero alpha. This is a derived
    // cache only; authoritative embedded layer pixels remain untouched.
    static QImage renderUnclippedRegionPreservingHiddenRgb(
        const QImage &source,
        const QVector<LayerNode> &layers,
        const QRect &previewRegion,
        const QSize &documentSize = {},
        const std::atomic_bool *cancelRequested = nullptr,
        ColourProcessingCompatibility processingCompatibility =
            ColourProcessingCompatibility::LegacyV1);

    // Capture the exact composite entering one adjustment at its position in
    // the hierarchy. Pass Through groups begin with the parent accumulator;
    // Isolated groups begin with a transparent local accumulator. The result
    // is straight RGBA so histogram analysis is not darkened by premultiplication.
    static QImage renderAdjustmentInput(const QImage &source,
                                        const QVector<LayerNode> &layers,
                                        const QUuid &adjustmentLayerId,
                                        const QSize &documentSize = {},
                                        const std::atomic_bool *cancelRequested = nullptr,
                                        ColourProcessingCompatibility processingCompatibility =
                                            ColourProcessingCompatibility::LegacyV1);

    // Full-document input to one per-Smart-instance Live Filter. This mirrors
    // renderAdjustmentInput() for histogram/on-image analysis without turning
    // the filter into a structural Adjustment Layer. Only the owning Smart
    // source transform and filters preceding liveFilterId are evaluated; the
    // Smart layer mask/opacity/blend and the target/downstream filters are not.
    // Mask-aware coverage-generation contract used by Layer Effect renderers.
    // Effects themselves are not rendered here; this returns the exact
    // pre-effect content and mask-aware coverage for one eligible owner layer.
    static LayerEffectInputRegion renderLayerEffectInputRegion(
        const QImage &source,
        const QVector<LayerNode> &layers,
        const QUuid &layerId,
        const QRect &previewRegion,
        const QSize &documentSize = {},
        const std::atomic_bool *cancelRequested = nullptr,
        ColourProcessingCompatibility processingCompatibility =
            ColourProcessingCompatibility::LegacyV1);

    // Render only the generated Layer Effect appearance for one layer and
    // output region. The owning content itself is not included. Behind/front
    // passes retain their independent effect blend modes so the CPU compositor
    // and native WebGPU hierarchy can share identical generated pixels.
    static QVector<LayerEffectRenderPass> renderLayerEffectPasses(
        const QImage &source,
        const LayerNode &layer,
        const QRect &previewRegion,
        const QSize &documentSize,
        const QTransform &worldTransform,
        const std::atomic_bool *cancelRequested = nullptr,
        ColourProcessingCompatibility processingCompatibility =
            ColourProcessingCompatibility::LegacyV1);

    // Runtime-derived fx intermediates are never persisted. Cold residency can
    // release them explicitly instead of waiting for global LRU pressure.
    static void invalidateLayerEffectCaches(const QSet<QUuid> &layerIds);

    static QImage renderLiveFilterInput(const QImage &source,
                                        const QVector<LayerNode> &layers,
                                        const QUuid &smartLayerId,
                                        const QUuid &liveFilterId,
                                        const QSize &documentSize = {},
                                        const std::atomic_bool *cancelRequested = nullptr,
                                        ColourProcessingCompatibility processingCompatibility =
                                            ColourProcessingCompatibility::LegacyV1);

    // Render the visible RGB result with pixel alpha treated as fully opaque.
    // This reference keeps hidden colour available when the authoritative
    // composite has zero alpha. Masks, layer visibility and adjustments remain
    // active; only pixel alpha is decoupled from RGB.
    static QImage renderRgbReference(const QImage &source,
                                     const QVector<LayerNode> &layers,
                                     const std::atomic_bool *cancelRequested = nullptr,
                                     const QSize &documentSize = {},
                                     ColourProcessingCompatibility processingCompatibility =
                                         ColourProcessingCompatibility::LegacyV1);
    static QImage renderRegionRgbReference(const QImage &source,
                                           const QVector<LayerNode> &layers,
                                           const QRect &previewRegion,
                                           const QSize &documentSize = {},
                                           const std::atomic_bool *cancelRequested = nullptr,
                                           ColourProcessingCompatibility processingCompatibility =
                                               ColourProcessingCompatibility::LegacyV1);

    // Render one pixel-bearing layer's stored colour values in document space.
    // Visibility, opacity, blend mode, masks and sibling layers are deliberately
    // ignored because the Channels panel edits the selected layer itself, not
    // the visible composite. Transparent pixels retain their hidden RGB.
    static QImage renderLayerRegionChannelReference(
        const QImage &source,
        const QVector<LayerNode> &layers,
        const QUuid &layerId,
        const QRect &previewRegion,
        const QSize &documentSize = {},
        bool forceOpaquePixelAlpha = true,
        const std::atomic_bool *cancelRequested = nullptr,
        ColourProcessingCompatibility processingCompatibility =
            ColourProcessingCompatibility::LegacyV1);

    // Full-resolution export helper. The normal composite supplies alpha; RGB
    // at fully transparent pixels comes from the independent RGB reference.
    static QImage renderPreservingHiddenRgb(const QImage &source,
                                            const QVector<LayerNode> &layers,
                                            const std::atomic_bool *cancelRequested = nullptr,
                                            const QSize &documentSize = {},
                                            ColourProcessingCompatibility processingCompatibility =
                                                ColourProcessingCompatibility::LegacyV1);

    // Region variant used by large cancellable composite analyses. The output
    // is straight RGBA8 or RGBA64, with hidden RGB restored only where the
    // authoritative visible-composite alpha is zero.
    static QImage renderRegionPreservingHiddenRgb(
        const QImage &source,
        const QVector<LayerNode> &layers,
        const QRect &previewRegion,
        const QSize &documentSize = {},
        const std::atomic_bool *cancelRequested = nullptr,
        ColourProcessingCompatibility processingCompatibility =
            ColourProcessingCompatibility::LegacyV1);

    // Bounds are returned in full-resolution document coordinates and include
    // affine transforms, child groups, alpha and masks.
    static QRectF contentBounds(const QImage &source,
                                const QVector<LayerNode> &layers,
                                const QVector<QUuid> &layerIds,
                                const QSize &documentSize = {});
};

} // namespace vfx
