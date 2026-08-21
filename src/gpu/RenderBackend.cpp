#include "gpu/RenderBackend.h"

#include "VectorRasterizer.h"

#include "ImageProcessor.h"
#include "CubeLut.h"
#include "SelectionLocalEditing.h"

#include <QDebug>
#include <QMutexLocker>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vfx {
namespace {

QString pathForRenderInfo(const TiledCanvasEngine::RenderInfo &info)
{
    const bool passThrough = info.visiblePassThroughGroups > 0;
    if (info.usedGpu && info.usedCpu) {
        return passThrough
            ? QStringLiteral("Mixed native/CPU tiled hierarchy compositor + Pass Through groups")
            : QStringLiteral("Mixed native/CPU tiled hierarchy compositor");
    }
    if (info.usedGpu) {
        return passThrough
            ? QStringLiteral("Native WebGPU tiled hierarchy compositor + WGSL adjustments + Pass Through groups")
            : QStringLiteral("Native WebGPU tiled hierarchy compositor + WGSL adjustments");
    }
    if (info.usedCpu) {
        return passThrough
            ? QStringLiteral("CPU tiled reference compositor + Pass Through groups")
            : QStringLiteral("CPU tiled reference compositor");
    }
    return info.path;
}

void mergeDisplayedRenderInfo(TiledCanvasEngine::RenderInfo *target,
                              const TiledCanvasEngine::RenderInfo &incoming)
{
    if (!target) {
        return;
    }
    target->usedGpu = target->usedGpu || incoming.usedGpu;
    target->usedCpu = target->usedCpu || incoming.usedCpu;
    target->mixedBackend = target->usedGpu && target->usedCpu;
    target->visiblePassThroughGroups = std::max(target->visiblePassThroughGroups,
                                                incoming.visiblePassThroughGroups);
    target->visibleIsolatedGroups = std::max(target->visibleIsolatedGroups,
                                             incoming.visibleIsolatedGroups);
    target->maximumGroupDepth = std::max(target->maximumGroupDepth,
                                         incoming.maximumGroupDepth);
    if (target->fallbackReason.isEmpty()) {
        target->fallbackReason = incoming.fallbackReason;
    } else if (!incoming.fallbackReason.isEmpty()
               && !target->fallbackReason.contains(incoming.fallbackReason)) {
        target->fallbackReason += QStringLiteral("; ") + incoming.fallbackReason;
    }
    target->path = pathForRenderInfo(*target);
}

int maximumPremultipliedDifference(const QImage &first, const QImage &second)
{
    if (first.isNull() || second.isNull() || first.size() != second.size()) {
        return 255;
    }
    const QImage a = first.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage b = second.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int maximum = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *aRow = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *bRow = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            maximum = std::max({maximum,
                                std::abs(qRed(aRow[x]) - qRed(bRow[x])),
                                std::abs(qGreen(aRow[x]) - qGreen(bRow[x])),
                                std::abs(qBlue(aRow[x]) - qBlue(bRow[x])),
                                std::abs(qAlpha(aRow[x]) - qAlpha(bRow[x]))});
        }
    }
    return maximum;
}


int maximumStraightDifference(const QImage &first, const QImage &second)
{
    if (first.isNull() || second.isNull() || first.size() != second.size()) {
        return 255;
    }
    const QImage a = first.convertToFormat(QImage::Format_RGBA8888);
    const QImage b = second.convertToFormat(QImage::Format_RGBA8888);
    int maximum = 0;
    for (int y = 0; y < a.height(); ++y) {
        const uchar *aRow = a.constScanLine(y);
        const uchar *bRow = b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x) {
            const int offset = x * 4;
            for (int component = 0; component < 4; ++component) {
                maximum = std::max(maximum,
                                   std::abs(static_cast<int>(aRow[offset + component])
                                            - static_cast<int>(bRow[offset + component])));
            }
        }
    }
    return maximum;
}

QImage applyStraightSelectionForParity(const QImage &beforeImage,
                                       const QImage &editedImage,
                                       const SelectionMask::Snapshot &snapshot,
                                       const QTransform &layerToDocument,
                                       const QRect &affectedRect)
{
    QImage before = beforeImage.convertToFormat(QImage::Format_RGBA8888);
    QImage edited = editedImage.convertToFormat(QImage::Format_RGBA8888);
    if (before.isNull() || edited.isNull() || before.size() != edited.size()
        || !snapshot.active) {
        return edited;
    }

    SelectionMask selection(snapshot.size);
    if (!selection.restoreSnapshot(snapshot, false)) {
        return {};
    }
    const QRect rect = affectedRect.intersected(edited.rect());
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const uchar *beforeRow = before.constScanLine(y);
        uchar *editedRow = edited.scanLine(y);
        for (int x = rect.left(); x <= rect.right(); ++x) {
            const double coverage = sampleSelectionCoverage(
                selection, layerToDocument, QPointF(x + 0.5, y + 0.5));
            if (coverage >= 1.0) {
                continue;
            }
            const int offset = x * 4;
            if (coverage <= 0.0) {
                std::memcpy(editedRow + offset, beforeRow + offset, 4);
                continue;
            }

            const double inverseCoverage = 1.0 - coverage;
            const double beforeAlpha = beforeRow[offset + 3] / 255.0;
            const double editedAlpha = editedRow[offset + 3] / 255.0;
            const double outputAlpha = editedAlpha * coverage
                + beforeAlpha * inverseCoverage;
            double output[4] {0.0, 0.0, 0.0, outputAlpha};
            if (outputAlpha > 1.0e-12) {
                for (int component = 0; component < 3; ++component) {
                    output[component] =
                        (editedRow[offset + component] / 255.0 * editedAlpha * coverage
                         + beforeRow[offset + component] / 255.0 * beforeAlpha
                               * inverseCoverage)
                        / outputAlpha;
                }
            } else {
                for (int component = 0; component < 3; ++component) {
                    output[component] = editedRow[offset + component] / 255.0 * coverage
                        + beforeRow[offset + component] / 255.0 * inverseCoverage;
                }
            }
            for (int component = 0; component < 4; ++component) {
                editedRow[offset + component] = static_cast<uchar>(std::clamp(
                    qRound(output[component] * 255.0), 0, 255));
            }
        }
    }
    edited.setColorSpace(editedImage.colorSpace().isValid()
                             ? editedImage.colorSpace() : beforeImage.colorSpace());
    return edited;
}

bool containsDeepMixedGroupHierarchy(const QVector<LayerNode> &layers,
                                     const int parentGroupDepth = 0,
                                     const bool passThroughAncestor = false,
                                     const bool isolatedAncestor = false)
{
    for (const LayerNode &layer : layers) {
        if (!layer.visible || layer.opacity <= 0.0
            || layer.type != LayerType::Group) {
            continue;
        }
        const int depth = parentGroupDepth + 1;
        const bool passThrough = passThroughAncestor
            || layer.groupCompositeMode == GroupCompositeMode::PassThrough;
        const bool isolated = isolatedAncestor
            || layer.groupCompositeMode == GroupCompositeMode::Isolated;
        // The only currently non-conforming startup case is a deeply nested
        // hierarchy that mixes both group-compositing semantics. Shallower
        // Pass Through/Isolated combinations have their own passing parity
        // cases and remain eligible for native compositing.
        if (depth >= 3 && passThrough && isolated) {
            return true;
        }
        if (containsDeepMixedGroupHierarchy(layer.children,
                                            depth,
                                            passThrough,
                                            isolated)) {
            return true;
        }
    }
    return false;
}


bool containsUnapprovedAdjustment(const QVector<LayerNode> &layers,
                                  const quint32 approvedMask,
                                  const QColorSpace &colourSpace)
{
    for (const LayerNode &layer : layers) {
        if (!layer.visible || layer.opacity <= 0.0) {
            continue;
        }
        if (layer.type == LayerType::Adjustment) {
            const quint32 bit = quint32(1)
                << static_cast<quint32>(layer.adjustmentType);
            if ((approvedMask & bit) == 0) {
                return true;
            }
            if (layer.adjustmentType == AdjustmentType::Lut) {
                const AdjustmentData data = layer.effectiveAdjustmentData();
                const auto &parameters = std::get<LutParameters>(data.parameters);
                const int width = std::max(parameters.hasCube()
                                               ? parameters.cubeSize * parameters.cubeSize
                                               : 0,
                                           parameters.hasShaper()
                                               ? parameters.shaperSize
                                               : 0);
                const int height = (parameters.hasCube() ? parameters.cubeSize : 0)
                    + (parameters.hasShaper() ? 1 : 0);
                if (parameters.hasData()
                    && (CubeLut::requiresCpuEvaluation(parameters, colourSpace)
                        || width <= 0 || width > 8192 || height <= 0 || height > 8192)) {
                    return true;
                }
            }
        }
        if (layer.type == LayerType::Group
            && containsUnapprovedAdjustment(layer.children, approvedMask, colourSpace)) {
            return true;
        }
    }
    return false;
}

bool containsAdjustmentCoverageWithoutParityApproval(
    const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (!layer.visible || layer.opacity <= 0.0) continue;

        // Startup parity approves the adjustment kernels themselves. Fractional
        // Adjustment-Layer coverage (opacity/masks) is a separate compositing
        // operation and has shown driver/platform deltas beyond the accepted
        // contract. Keep those hierarchies on the exact CPU reference until a
        // dedicated coverage parity fixture approves them.
        if (layer.type == LayerType::Adjustment
            && (std::abs(layer.opacity - 1.0) > 1.0e-12
                || (layer.maskEnabled && !layer.maskImage.isNull()))) {
            return true;
        }

        if (layer.type == LayerType::Smart) {
            for (const LiveFilter &filter : layer.liveFilters) {
                if (filter.enabled && filter.maskEnabled
                    && !filter.maskImage.isNull()) {
                    return true;
                }
            }
        }

        if (layer.type == LayerType::Group
            && containsAdjustmentCoverageWithoutParityApproval(layer.children)) {
            return true;
        }
    }
    return false;
}

} // namespace

RenderBackend &RenderBackend::instance()
{
    static RenderBackend backend;
    return backend;
}

RenderBackend::RenderBackend()
    : m_tiledCanvas(&m_webGpu)
{
}

bool RenderBackend::canvasGpuReady()
{
    GpuDiagnosticCapabilities externalCapabilities;
    bool useExternalCapabilities = false;
    {
        QMutexLocker lock(&m_stateMutex);
        if (!m_externalGpuApproved) {
            return false;
        }
        if (m_gpuInitialisationAttempted) {
            return m_webGpu.deviceReady() && m_gpuFoundationParityPassed;
        }
        m_gpuInitialisationAttempted = true;
        useExternalCapabilities = m_externalCapabilitiesAvailable;
        externalCapabilities = m_externalCapabilities;
    }

    const bool initialised = m_webGpu.initialise();
    if (useExternalCapabilities) {
        if (!initialised) {
            QMutexLocker lock(&m_stateMutex);
            m_gpuFoundationParityPassed = false;
            m_gpuCompositorParityPassed = false;
            m_gpuStandardCompositorParityPassed = false;
            m_gpuBrushParityPassed = false;
            m_gpuCloneStampParityPassed = false;
            m_externalDiagnosticStatus += QStringLiteral(
                " The GUI runtime could not create its native WebGPU device; "
                "the CPU renderer remains active.");
            return false;
        }
        m_webGpu.adoptExternalValidationState(externalCapabilities.webGpu);
        QMutexLocker lock(&m_stateMutex);
        m_gpuFoundationParityPassed = externalCapabilities.foundation;
        m_gpuCompositorParityPassed = externalCapabilities.foundation
            && externalCapabilities.compositor;
        m_gpuStandardCompositorParityPassed = externalCapabilities.foundation
            && externalCapabilities.standardCompositor;
        m_gpuBrushParityPassed = externalCapabilities.foundation
            && externalCapabilities.brush;
        m_gpuCloneStampParityPassed = externalCapabilities.foundation
            && externalCapabilities.cloneStamp;
        m_localValidationStatus = externalCapabilities.localValidationStatus;
        qInfo().noquote()
            << "[GPU runtime] Imported the isolated helper's per-feature validation record; exhaustive parity tests were not repeated in the GUI process.";
        return m_webGpu.deviceReady() && m_gpuFoundationParityPassed;
    }

    // Compatibility path for unit tests and callers that only provide the
    // legacy pass/fail status. Production GUI startup transfers the detailed
    // helper-process validation snapshot and never repeats the exhaustive
    // parity suite inside the long-lived application process.
    QString baseParityDetails;
    const bool baseParity = initialised
        && m_webGpu.runTileParitySelfTest(&baseParityDetails);
    NativeGpuCapabilities capabilities;
    QString featureParityStatus;
    if (baseParity) {
        runNativeHierarchyParitySelfTest(&capabilities,
                                         &featureParityStatus);
    }
    {
        QMutexLocker lock(&m_stateMutex);
        m_gpuFoundationParityPassed = baseParity;
        m_gpuCompositorParityPassed = baseParity && capabilities.compositor;
        m_gpuStandardCompositorParityPassed = baseParity
            && capabilities.standardCompositor;
        m_gpuBrushParityPassed = baseParity && capabilities.brush;
        m_gpuCloneStampParityPassed = baseParity && capabilities.cloneStamp;
        m_localValidationStatus = featureParityStatus;
    }
    return m_webGpu.deviceReady() && baseParity;
}

bool RenderBackend::compositorGpuReady(const QVector<LayerNode> &layers,
                                      const QColorSpace &colourSpace)
{
    if (!canvasGpuReady()) {
        return false;
    }

    // Each adjustment is validated independently. A driver-specific mismatch
    // in one operation no longer disables the compositor or unrelated WGSL
    // adjustments; only hierarchies containing that unapproved operation use
    // the exact CPU reference fallback.
    if (containsUnapprovedAdjustment(layers,
                                     m_webGpu.approvedAdjustmentMask(),
                                     colourSpace)) {
        return false;
    }
    if (containsAdjustmentCoverageWithoutParityApproval(layers)) {
        return false;
    }

    bool fullParity = false;
    bool standardParity = false;
    {
        QMutexLocker lock(&m_stateMutex);
        fullParity = m_gpuCompositorParityPassed;
        standardParity = m_gpuStandardCompositorParityPassed;
    }
    if (fullParity) {
        return true;
    }
    return standardParity && !containsDeepMixedGroupHierarchy(layers);
}

bool RenderBackend::brushGpuReady()
{
    if (!canvasGpuReady()) {
        return false;
    }
    QMutexLocker lock(&m_stateMutex);
    return m_gpuBrushParityPassed;
}

bool RenderBackend::cloneStampGpuReady()
{
    if (!canvasGpuReady()) {
        return false;
    }
    QMutexLocker lock(&m_stateMutex);
    return m_gpuCloneStampParityPassed;
}

bool RenderBackend::fillGpuReady()
{
    return canvasGpuReady() && m_webGpu.fillGpuApproved();
}

bool RenderBackend::gradientGpuReady()
{
    return canvasGpuReady() && m_webGpu.gradientGpuApproved();
}

bool RenderBackend::sessionCurrentLocked(const RenderSessionContext &sessionContext) const
{
    if (!sessionContext.isValid()) {
        return false;
    }
    const auto iterator = m_sessionRenderIdentities.constFind(sessionContext.documentSessionId);
    return iterator != m_sessionRenderIdentities.cend()
        && iterator.value().matches(sessionContext);
}

bool RenderBackend::isSessionCurrent(const RenderSessionContext &sessionContext) const
{
    QMutexLocker lock(&m_stateMutex);
    return sessionCurrentLocked(sessionContext);
}

QImage RenderBackend::renderRegion(const RenderSessionContext &sessionContext,
                                   const QImage &source,
                                   const QVector<LayerNode> &layers,
                                   const QRect &previewRegion,
                                   const QSize &documentSize,
                                   const int level,
                                   const std::atomic_bool *cancelRequested,
                                   TiledCanvasEngine::RenderInfo *renderInfo)
{
    const auto cancelledOrObsolete = [&] {
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            return true;
        }
        return !isSessionCurrent(sessionContext);
    };
    const auto publishCancellation = [&] {
        if (renderInfo) {
            renderInfo->cancelled = true;
            renderInfo->path = QStringLiteral("Cancelled obsolete document-session composite request");
        }
    };

    if (cancelledOrObsolete()) {
        publishCancellation();
        return {};
    }
    QMutexLocker renderLock(&m_renderMutex);
    if (cancelledOrObsolete()) {
        publishCancellation();
        return {};
    }
    return m_tiledCanvas.renderRegion(source,
                                      layers,
                                      previewRegion,
                                      documentSize,
                                      compositorGpuReady(layers, source.colorSpace())
                                          && (sessionContext.processingCompatibility
                                                  != ColourProcessingCompatibility::ManagedV1
                                              || !layerTreeRequiresManagedDomainTransform(layers)
                                              || m_webGpu.managedAdjustmentTransformsGpuApproved()),
                                      level,
                                      cancelRequested,
                                      renderInfo,
                                      sessionContext.documentSessionId,
                                      sessionContext.colourStateRevision,
                                      sessionContext.processingCompatibility);
}

QImage RenderBackend::renderRegion(const QImage &source,
                                   const QVector<LayerNode> &layers,
                                   const QRect &previewRegion,
                                   const QSize &documentSize,
                                   const int level,
                                   const std::atomic_bool *cancelRequested,
                                   TiledCanvasEngine::RenderInfo *renderInfo)
{
    if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
        if (renderInfo) {
            renderInfo->cancelled = true;
            renderInfo->path = QStringLiteral("Cancelled obsolete composite request");
        }
        return {};
    }
    QMutexLocker renderLock(&m_renderMutex);
    if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
        if (renderInfo) {
            renderInfo->cancelled = true;
            renderInfo->path = QStringLiteral("Cancelled obsolete composite request");
        }
        return {};
    }
    return m_tiledCanvas.renderRegion(source,
                                      layers,
                                      previewRegion,
                                      documentSize,
                                      compositorGpuReady(layers, source.colorSpace()),
                                      level,
                                      cancelRequested,
                                      renderInfo,
                                      QUuid(),
                                      0,
                                      ColourProcessingCompatibility::LegacyV1);
}

QImage RenderBackend::renderInteractiveRegion(
    const RenderSessionContext &sessionContext,
    const QImage &source,
    const QVector<LayerNode> &layers,
    const QRect &previewRegion,
    const QSize &documentSize,
    const int level,
    const std::atomic_bool *cancelRequested,
    TiledCanvasEngine::RenderInfo *renderInfo,
    const bool skipPersistentCacheFallback)
{
    const auto cancelledOrObsolete = [&] {
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            return true;
        }
        return !isSessionCurrent(sessionContext);
    };
    const auto publishCancellation = [&] {
        if (renderInfo) {
            renderInfo->cancelled = true;
            renderInfo->path = QStringLiteral(
                "Cancelled obsolete interactive composite request");
        }
    };

    if (cancelledOrObsolete()) {
        publishCancellation();
        return {};
    }
    QMutexLocker renderLock(&m_renderMutex);
    if (cancelledOrObsolete()) {
        publishCancellation();
        return {};
    }
    return m_tiledCanvas.renderInteractiveRegion(
        source,
        layers,
        previewRegion,
        documentSize,
        compositorGpuReady(layers, source.colorSpace())
            && (sessionContext.processingCompatibility
                    != ColourProcessingCompatibility::ManagedV1
                || !layerTreeRequiresManagedDomainTransform(layers)
                || m_webGpu.managedAdjustmentTransformsGpuApproved()),
        level,
        cancelRequested,
        renderInfo,
        sessionContext.documentSessionId,
        sessionContext.colourStateRevision,
        sessionContext.processingCompatibility,
        skipPersistentCacheFallback);
}

TiledCanvasEngine::BrushResult RenderBackend::stampRasterStroke(
    const RenderSessionContext &sessionContext,
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
    const SelectionMask::Snapshot *selectionSnapshot,
    const QTransform &layerToDocument)
{
    if (!isSessionCurrent(sessionContext)) {
        TiledCanvasEngine::BrushResult result;
        result.error = QStringLiteral("The document session changed before the raster stroke could run");
        return result;
    }
    QMutexLocker renderLock(&m_renderMutex);
    if (!isSessionCurrent(sessionContext)) {
        TiledCanvasEngine::BrushResult result;
        result.error = QStringLiteral("The document session changed while the raster stroke was queued");
        return result;
    }
    return m_tiledCanvas.stampRasterStroke(sourceRaster,
                                            documentSize,
                                            colourSpace,
                                            layerId,
                                            layerRevision,
                                            documentSegments,
                                            documentToLayer,
                                            diameter,
                                            opacity,
                                            hardness,
                                            colour,
                                            erasing,
                                            allowGpu && brushGpuReady()
                                                && (!selectionSnapshot || !selectionSnapshot->active),
                                            sessionContext.documentSessionId,
                                            selectionSnapshot,
                                            layerToDocument);
}

TiledCanvasEngine::BrushResult RenderBackend::stampRasterStroke(
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
    const SelectionMask::Snapshot *selectionSnapshot,
    const QTransform &layerToDocument)
{
    QMutexLocker renderLock(&m_renderMutex);
    return m_tiledCanvas.stampRasterStroke(sourceRaster,
                                            documentSize,
                                            colourSpace,
                                            layerId,
                                            layerRevision,
                                            documentSegments,
                                            documentToLayer,
                                            diameter,
                                            opacity,
                                            hardness,
                                            colour,
                                            erasing,
                                            allowGpu && brushGpuReady()
                                                && (!selectionSnapshot || !selectionSnapshot->active),
                                            QUuid(),
                                            selectionSnapshot,
                                            layerToDocument);
}

TiledCanvasEngine::BrushResult RenderBackend::stampChannelStroke(
    const RenderSessionContext &sessionContext,
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
    const int channelValue)
{
    if (!isSessionCurrent(sessionContext)) {
        TiledCanvasEngine::BrushResult result;
        result.error = QStringLiteral("The document session changed before the channel stroke could run");
        return result;
    }
    QMutexLocker renderLock(&m_renderMutex);
    if (!isSessionCurrent(sessionContext)) {
        TiledCanvasEngine::BrushResult result;
        result.error = QStringLiteral("The document session changed while the channel stroke was queued");
        return result;
    }
    return m_tiledCanvas.stampChannelStroke(sourceRaster,
                                             documentSize,
                                             colourSpace,
                                             layerId,
                                             layerRevision,
                                             documentSegments,
                                             documentToLayer,
                                             diameter,
                                             opacity,
                                             hardness,
                                             channelIndex,
                                             channelValue,
                                             sessionContext.documentSessionId);
}

TiledCanvasEngine::BrushResult RenderBackend::stampChannelStroke(
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
    const int channelValue)
{
    QMutexLocker renderLock(&m_renderMutex);
    return m_tiledCanvas.stampChannelStroke(sourceRaster,
                                             documentSize,
                                             colourSpace,
                                             layerId,
                                             layerRevision,
                                             documentSegments,
                                             documentToLayer,
                                             diameter,
                                             opacity,
                                             hardness,
                                             channelIndex,
                                             channelValue);
}

TiledCanvasEngine::BrushResult RenderBackend::stampMaskStroke(
    const RenderSessionContext &sessionContext,
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
    const SelectionMask::Snapshot *selectionSnapshot,
    const QTransform &layerToDocument)
{
    if (!isSessionCurrent(sessionContext)) {
        TiledCanvasEngine::BrushResult result;
        result.error = QStringLiteral("The document session changed before the mask stroke could run");
        return result;
    }
    QMutexLocker renderLock(&m_renderMutex);
    if (!isSessionCurrent(sessionContext)) {
        TiledCanvasEngine::BrushResult result;
        result.error = QStringLiteral("The document session changed while the mask stroke was queued");
        return result;
    }
    return m_tiledCanvas.stampMaskStroke(sourceMask,
                                         documentSize,
                                         layerId,
                                         layerRevision,
                                         documentSegments,
                                         documentToLayer,
                                         diameter,
                                         opacity,
                                         hardness,
                                         greyscaleValue,
                                         restoringCoverage,
                                         allowGpu && brushGpuReady(),
                                         sessionContext.documentSessionId,
                                         selectionSnapshot,
                                         layerToDocument);
}

TiledCanvasEngine::BrushResult RenderBackend::stampMaskStroke(
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
    const SelectionMask::Snapshot *selectionSnapshot,
    const QTransform &layerToDocument)
{
    QMutexLocker renderLock(&m_renderMutex);
    return m_tiledCanvas.stampMaskStroke(sourceMask,
                                         documentSize,
                                         layerId,
                                         layerRevision,
                                         documentSegments,
                                         documentToLayer,
                                         diameter,
                                         opacity,
                                         hardness,
                                         greyscaleValue,
                                         restoringCoverage,
                                         allowGpu && brushGpuReady(),
                                         QUuid(),
                                         selectionSnapshot,
                                         layerToDocument);
}


TiledCanvasEngine::BrushResult RenderBackend::stampCloneStroke(
    const RenderSessionContext &sessionContext,
    const CloneStampRequest &request,
    const QSize &documentSize,
    const QUuid &layerId,
    const quint64 layerRevision,
    const bool allowGpu,
    const SelectionMask::Snapshot *selectionSnapshot)
{
    if (!isSessionCurrent(sessionContext)) {
        TiledCanvasEngine::BrushResult result;
        result.error = QStringLiteral(
            "The document session changed before the Clone Stamp stroke could run");
        return result;
    }
    QMutexLocker renderLock(&m_renderMutex);
    if (!isSessionCurrent(sessionContext)) {
        TiledCanvasEngine::BrushResult result;
        result.error = QStringLiteral(
            "The document session changed while the Clone Stamp stroke was queued");
        return result;
    }
    return m_tiledCanvas.stampCloneStroke(request,
                                           documentSize,
                                           layerId,
                                           layerRevision,
                                           allowGpu && cloneStampGpuReady(),
                                           sessionContext.documentSessionId,
                                           selectionSnapshot);
}

TiledCanvasEngine::BrushResult RenderBackend::stampCloneStroke(
    const CloneStampRequest &request,
    const QSize &documentSize,
    const QUuid &layerId,
    const quint64 layerRevision,
    const bool allowGpu,
    const SelectionMask::Snapshot *selectionSnapshot)
{
    QMutexLocker renderLock(&m_renderMutex);
    return m_tiledCanvas.stampCloneStroke(request,
                                           documentSize,
                                           layerId,
                                           layerRevision,
                                           allowGpu && cloneStampGpuReady(),
                                           QUuid(),
                                           selectionSnapshot);
}

TiledCanvasEngine::FillResult RenderBackend::applyFillCoverage(
    const RenderSessionContext &sessionContext,
    const QImage &sourceImage,
    const QImage &coverage,
    const QUuid &layerId,
    const quint64 layerRevision,
    const FillTarget target,
    const int componentIndex,
    const QColor &colour,
    const bool preserveTransparency,
    const bool allowGpu)
{
    if (!isSessionCurrent(sessionContext)) {
        TiledCanvasEngine::FillResult result;
        result.error = QStringLiteral(
            "The document session changed before the Fill operation could run");
        return result;
    }
    QMutexLocker renderLock(&m_renderMutex);
    if (!isSessionCurrent(sessionContext)) {
        TiledCanvasEngine::FillResult result;
        result.error = QStringLiteral(
            "The document session changed while the Fill operation was queued");
        return result;
    }
    return m_tiledCanvas.applyFillCoverage(sourceImage,
                                            coverage,
                                            layerId,
                                            layerRevision,
                                            target,
                                            componentIndex,
                                            colour,
                                            preserveTransparency,
                                            allowGpu && fillGpuReady(),
                                            sessionContext.documentSessionId);
}

TiledCanvasEngine::FillResult RenderBackend::applyFillCoverage(
    const QImage &sourceImage,
    const QImage &coverage,
    const QUuid &layerId,
    const quint64 layerRevision,
    const FillTarget target,
    const int componentIndex,
    const QColor &colour,
    const bool preserveTransparency,
    const bool allowGpu)
{
    QMutexLocker renderLock(&m_renderMutex);
    return m_tiledCanvas.applyFillCoverage(sourceImage,
                                            coverage,
                                            layerId,
                                            layerRevision,
                                            target,
                                            componentIndex,
                                            colour,
                                            preserveTransparency,
                                            allowGpu && fillGpuReady());
}


TiledCanvasEngine::FillResult RenderBackend::applyGradient(
    const RenderSessionContext &sessionContext,
    const GradientApplyRequest &request,
    const QUuid &layerId,
    const quint64 layerRevision,
    const bool allowGpu)
{
    if (!isSessionCurrent(sessionContext)) {
        TiledCanvasEngine::FillResult result;
        result.error = QStringLiteral(
            "The document session changed before the Gradient operation could run");
        return result;
    }
    QMutexLocker renderLock(&m_renderMutex);
    if (!isSessionCurrent(sessionContext)) {
        TiledCanvasEngine::FillResult result;
        result.error = QStringLiteral(
            "The document session changed while the Gradient operation was queued");
        return result;
    }
    return m_tiledCanvas.applyGradient(request,
                                        layerId,
                                        layerRevision,
                                        allowGpu && gradientGpuReady(),
                                        sessionContext.documentSessionId);
}

TiledCanvasEngine::FillResult RenderBackend::applyGradient(
    const GradientApplyRequest &request,
    const QUuid &layerId,
    const quint64 layerRevision,
    const bool allowGpu)
{
    QMutexLocker renderLock(&m_renderMutex);
    return m_tiledCanvas.applyGradient(request,
                                        layerId,
                                        layerRevision,
                                        allowGpu && gradientGpuReady());
}

bool RenderBackend::initialiseGpuFoundation()
{
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_gpuInitialisationAttempted) {
            return m_webGpu.deviceReady() && m_gpuFoundationParityPassed;
        }
        m_gpuInitialisationAttempted = true;
    }
    const bool initialised = m_webGpu.initialise();
    QString baseParityDetails;
    const bool baseParity = initialised
        && m_webGpu.runTileParitySelfTest(&baseParityDetails);
    NativeGpuCapabilities capabilities;
    QString featureParityStatus;
    if (baseParity) {
        runNativeHierarchyParitySelfTest(&capabilities,
                                         &featureParityStatus);
    }
    QMutexLocker lock(&m_stateMutex);
    m_gpuFoundationParityPassed = baseParity;
    m_gpuCompositorParityPassed = baseParity && capabilities.compositor;
    m_gpuStandardCompositorParityPassed = baseParity
        && capabilities.standardCompositor;
    m_gpuBrushParityPassed = baseParity && capabilities.brush;
    m_gpuCloneStampParityPassed = baseParity && capabilities.cloneStamp;
    m_localValidationStatus = featureParityStatus;
    return m_webGpu.deviceReady() && baseParity;
}


void RenderBackend::shutdownGpuFoundation()
{
    // Tear down while Qt and the worker/runtime infrastructure are still
    // alive. The isolated helper previously left wgpu-native destruction to
    // function-static teardown, which can occur after supporting runtime state
    // has already begun to disappear.
    QMutexLocker renderLock(&m_renderMutex);
    m_tiledCanvas.clear();
    VectorRasterizer::clearCache();
    m_webGpu.shutdown();

    QMutexLocker stateLock(&m_stateMutex);
    m_gpuInitialisationAttempted = false;
    m_gpuFoundationParityPassed = false;
    m_gpuCompositorParityPassed = false;
    m_gpuStandardCompositorParityPassed = false;
    m_gpuBrushParityPassed = false;
    m_gpuCloneStampParityPassed = false;
    m_localValidationStatus.clear();
}

bool RenderBackend::runNativeHierarchyParitySelfTest(
    NativeGpuCapabilities *capabilities,
    QString *details)
{
    if (capabilities) {
        *capabilities = {};
    }
    if (details) {
        details->clear();
    }
    if (!m_webGpu.deviceReady()) {
        if (details) {
            *details = QStringLiteral(" Native tiled hierarchy, mask, Clone Stamp and Pass Through validation could not start.");
        }
        return false;
    }

    QImage source(66, 66, QImage::Format_RGBA8888);
    QImage rasterImage(source.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x,
                                 y,
                                 QColor((31 + x * 3 + y * 5) & 255,
                                        (73 + x * 7 + y * 2) & 255,
                                        (19 + x * 11 + y * 3) & 255,
                                        255));
            rasterImage.setPixelColor(x,
                                      y,
                                      QColor((211 + x * 2 + y) & 255,
                                             (43 + x * 5 + y * 7) & 255,
                                             (127 + x * 3 + y * 9) & 255,
                                             255));
        }
    }

    QImage compactWhite(1, 1, QImage::Format_Grayscale8);
    compactWhite.fill(255);

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode maskedRaster;
    maskedRaster.type = LayerType::Raster;
    maskedRaster.rasterImage = rasterImage;
    maskedRaster.maskImage = compactWhite;
    maskedRaster.maskInverted = true;

    LayerNode disabledRaster = maskedRaster;
    disabledRaster.id = QUuid::createUuid();
    disabledRaster.maskEnabled = false;

    LayerNode maskedAdjustment;
    maskedAdjustment.type = LayerType::Adjustment;
    maskedAdjustment.adjustmentType = AdjustmentType::Exposure;
    maskedAdjustment.exposure = 1.0;
    maskedAdjustment.maskImage = compactWhite;
    maskedAdjustment.maskInverted = true;

    LayerNode groupChild;
    groupChild.type = LayerType::Raster;
    groupChild.rasterImage = rasterImage;
    LayerNode maskedGroup;
    maskedGroup.type = LayerType::Group;
    maskedGroup.children = {groupChild};
    maskedGroup.maskImage = compactWhite;
    maskedGroup.maskInverted = true;

    QImage passThroughMask(source.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < passThroughMask.height(); ++y) {
        uchar *row = passThroughMask.scanLine(y);
        for (int x = 0; x < passThroughMask.width(); ++x) {
            row[x] = static_cast<uchar>((31 + x * 5 + y * 3) & 255);
        }
    }

    LayerNode passExposure;
    passExposure.type = LayerType::Adjustment;
    passExposure.adjustmentType = AdjustmentType::Exposure;
    passExposure.exposure = 0.85;

    LayerNode passRaster;
    passRaster.type = LayerType::Raster;
    passRaster.rasterImage = rasterImage;
    passRaster.opacity = 0.41;
    passRaster.blendMode = BlendMode::Screen;

    LayerNode passThroughGroup;
    passThroughGroup.type = LayerType::Group;
    passThroughGroup.groupCompositeMode = GroupCompositeMode::PassThrough;
    passThroughGroup.children = {passExposure, passRaster};
    passThroughGroup.opacity = 0.63;
    passThroughGroup.maskImage = passThroughMask;
    passThroughGroup.transform = QTransform::fromTranslate(2.0, -1.0);
    passThroughGroup.transform.scale(0.96, 1.04);

    LayerNode invertedPassThrough = passThroughGroup;
    invertedPassThrough.id = QUuid::createUuid();
    invertedPassThrough.maskInverted = true;

    LayerNode disabledPassThrough = passThroughGroup;
    disabledPassThrough.id = QUuid::createUuid();
    disabledPassThrough.maskEnabled = false;

    LayerNode innerPassThrough;
    innerPassThrough.type = LayerType::Group;
    innerPassThrough.groupCompositeMode = GroupCompositeMode::PassThrough;
    innerPassThrough.children = {passExposure};

    LayerNode outerPassThrough;
    outerPassThrough.type = LayerType::Group;
    outerPassThrough.groupCompositeMode = GroupCompositeMode::PassThrough;
    outerPassThrough.children = {innerPassThrough};
    outerPassThrough.opacity = 0.72;

    LayerNode isolatedInner = innerPassThrough;
    isolatedInner.id = QUuid::createUuid();
    isolatedInner.groupCompositeMode = GroupCompositeMode::Isolated;
    LayerNode passThroughWithIsolatedChild = outerPassThrough;
    passThroughWithIsolatedChild.id = QUuid::createUuid();
    passThroughWithIsolatedChild.children = {isolatedInner};

    LayerNode isolatedOuter = outerPassThrough;
    isolatedOuter.id = QUuid::createUuid();
    isolatedOuter.groupCompositeMode = GroupCompositeMode::Isolated;

    LayerNode deepExposure = passExposure;
    deepExposure.id = QUuid::createUuid();
    deepExposure.exposure = 0.55;
    LayerNode deepInnerPass;
    deepInnerPass.type = LayerType::Group;
    deepInnerPass.groupCompositeMode = GroupCompositeMode::PassThrough;
    deepInnerPass.children = {deepExposure};
    deepInnerPass.opacity = 0.81;
    deepInnerPass.maskImage = passThroughMask;
    deepInnerPass.maskInverted = true;

    LayerNode deepRaster = passRaster;
    deepRaster.id = QUuid::createUuid();
    deepRaster.opacity = 0.52;
    LayerNode deepIsolated;
    deepIsolated.type = LayerType::Group;
    deepIsolated.groupCompositeMode = GroupCompositeMode::Isolated;
    deepIsolated.children = {deepInnerPass, deepRaster};
    deepIsolated.opacity = 0.74;
    deepIsolated.maskImage = passThroughMask;

    LayerNode deepContrast;
    deepContrast.type = LayerType::Adjustment;
    deepContrast.adjustmentType = AdjustmentType::Contrast;
    deepContrast.contrast = 31.0;
    LayerNode deepMiddlePass;
    deepMiddlePass.type = LayerType::Group;
    deepMiddlePass.groupCompositeMode = GroupCompositeMode::PassThrough;
    deepMiddlePass.children = {deepContrast, deepIsolated};
    deepMiddlePass.opacity = 0.67;
    deepMiddlePass.transform = QTransform::fromTranslate(-1.0, 2.0);

    LayerNode deepOuterPass;
    deepOuterPass.type = LayerType::Group;
    deepOuterPass.groupCompositeMode = GroupCompositeMode::PassThrough;
    deepOuterPass.children = {deepMiddlePass};
    deepOuterPass.opacity = 0.88;
    deepOuterPass.maskImage = passThroughMask;
    deepOuterPass.transform = QTransform::fromTranslate(1.0, -2.0);

    struct NamedHierarchyCase {
        QString name;
        QVector<LayerNode> layers;
    };
    const QVector<NamedHierarchyCase> cases {
        {QStringLiteral("inverted raster mask"), {maskedRaster, base}},
        {QStringLiteral("disabled raster mask"), {disabledRaster, base}},
        {QStringLiteral("inverted adjustment mask"), {maskedAdjustment, base}},
        {QStringLiteral("inverted group mask"), {maskedGroup, base}},
        {QStringLiteral("Pass Through group"), {passThroughGroup, base}},
        {QStringLiteral("inverted Pass Through mask"), {invertedPassThrough, base}},
        {QStringLiteral("disabled Pass Through mask"), {disabledPassThrough, base}},
        {QStringLiteral("nested Pass Through"), {outerPassThrough, base}},
        {QStringLiteral("Pass Through with isolated child"), {passThroughWithIsolatedChild, base}},
        {QStringLiteral("isolated outer group"), {isolatedOuter, base}},
        {QStringLiteral("deep mixed hierarchy"), {deepOuterPass, base}}
    };

    enum class CapabilityGroup {
        Compositor,
        Brush,
        CloneStamp
    };
    struct ParityMeasurement {
        QString name;
        int difference = 255;
        int tolerance = 0;
        CapabilityGroup group = CapabilityGroup::Compositor;
    };
    QVector<ParityMeasurement> measurements;
    int maximumDifference = 0;
    bool allWithinTolerance = true;
    const auto recordDifference = [&](const QString &name,
                                      const int difference,
                                      const int tolerance,
                                      const CapabilityGroup group) {
        measurements.push_back({name, difference, tolerance, group});
        maximumDifference = std::max(maximumDifference, difference);
        allWithinTolerance = allWithinTolerance && difference <= tolerance;
        qInfo().noquote() << QStringLiteral(
            "[GPU diagnostic] Native parity: %1 max difference %2 (limit %3).")
                                .arg(name)
                                .arg(difference)
                                .arg(tolerance);
    };
    const auto publishMeasuredCapabilities = [&] {
        if (!capabilities) {
            return;
        }
        bool compositorPassed = true;
        bool brushPassed = true;
        bool cloneStampPassed = true;
        bool compositorSeen = false;
        bool brushSeen = false;
        bool cloneStampSeen = false;
        QStringList compositorFailures;
        for (const ParityMeasurement &measurement : measurements) {
            const bool passed = measurement.difference <= measurement.tolerance;
            switch (measurement.group) {
            case CapabilityGroup::Compositor:
                compositorSeen = true;
                compositorPassed = compositorPassed && passed;
                if (!passed) {
                    compositorFailures.push_back(measurement.name);
                }
                break;
            case CapabilityGroup::Brush:
                brushSeen = true;
                brushPassed = brushPassed && passed;
                break;
            case CapabilityGroup::CloneStamp:
                cloneStampSeen = true;
                cloneStampPassed = cloneStampPassed && passed;
                break;
            }
        }
        compositorPassed = compositorSeen && compositorPassed;
        capabilities->compositor = compositorPassed;
        capabilities->standardCompositor = compositorSeen
            && (compositorPassed
                || (compositorFailures.size() == 1
                    && compositorFailures.front()
                        == QStringLiteral("deep mixed hierarchy")));
        capabilities->brush = brushSeen && brushPassed;
        capabilities->cloneStamp = cloneStampSeen && cloneStampPassed;
    };

    for (const NamedHierarchyCase &testCase : cases) {
        const QImage expected = ImageProcessor::renderRegion(source,
                                                              testCase.layers,
                                                              source.rect(),
                                                              source.size());
        const QImage actual = m_tiledCanvas.renderRegion(source,
                                                          testCase.layers,
                                                          source.rect(),
                                                          source.size(),
                                                          true,
                                                          0);
        if (expected.isNull() || actual.isNull()
            || !m_tiledCanvas.lastBackendText().startsWith(QStringLiteral("Native WebGPU"))) {
            m_tiledCanvas.clear();
            if (details) {
                *details = QStringLiteral(
                    " Native tiled hierarchy case '%1' failed to execute on WebGPU; the affected feature uses its CPU reference path.")
                               .arg(testCase.name);
            }
            recordDifference(testCase.name, 255, 2,
                             CapabilityGroup::Compositor);
            publishMeasuredCapabilities();
            return false;
        }
        recordDifference(testCase.name,
                         maximumPremultipliedDifference(expected, actual),
                         2,
                         CapabilityGroup::Compositor);
    }

    // Pass Through mixes premultiplied before/after RGBA values. Exercise a
    // varying-alpha parent and child so hidden colour and coverage cannot pass
    // validation merely because the normal diagnostic source is opaque.
    QImage alphaSource(source.size(), QImage::Format_RGBA8888);
    QImage alphaRaster(source.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < alphaSource.height(); ++y) {
        for (int x = 0; x < alphaSource.width(); ++x) {
            alphaSource.setPixelColor(x,
                                      y,
                                      QColor((17 + x * 7 + y * 3) & 255,
                                             (61 + x * 2 + y * 11) & 255,
                                             (109 + x * 5 + y * 13) & 255,
                                             (x * 9 + y * 5) & 255));
            alphaRaster.setPixelColor(x,
                                      y,
                                      QColor((223 + x + y * 3) & 255,
                                             (47 + x * 5 + y) & 255,
                                             (137 + x * 3 + y * 7) & 255,
                                             (43 + x * 7 + y * 11) & 255));
        }
    }
    LayerNode alphaBase;
    alphaBase.type = LayerType::BaseImage;
    LayerNode alphaChild;
    alphaChild.type = LayerType::Raster;
    alphaChild.rasterImage = alphaRaster;
    alphaChild.opacity = 0.58;
    alphaChild.blendMode = BlendMode::Copy;
    LayerNode alphaPassThrough;
    alphaPassThrough.type = LayerType::Group;
    alphaPassThrough.groupCompositeMode = GroupCompositeMode::PassThrough;
    alphaPassThrough.children = {passExposure, alphaChild};
    alphaPassThrough.opacity = 0.47;
    alphaPassThrough.maskImage = passThroughMask;
    const QVector<LayerNode> alphaLayers {alphaPassThrough, alphaBase};
    const QImage expectedAlpha = ImageProcessor::renderRegion(alphaSource,
                                                               alphaLayers,
                                                               alphaSource.rect(),
                                                               alphaSource.size());
    const QImage actualAlpha = m_tiledCanvas.renderRegion(alphaSource,
                                                           alphaLayers,
                                                           alphaSource.rect(),
                                                           alphaSource.size(),
                                                           true,
                                                           0);
    if (expectedAlpha.isNull() || actualAlpha.isNull()
        || !m_tiledCanvas.lastBackendText().startsWith(QStringLiteral("Native WebGPU"))) {
        m_tiledCanvas.clear();
        if (details) {
            *details = QStringLiteral(
                " Native tiled alpha Pass Through validation failed to execute on WebGPU; the affected feature uses its CPU reference path.");
        }
        recordDifference(QStringLiteral("varying-alpha Pass Through"),
                         255, 2, CapabilityGroup::Compositor);
        publishMeasuredCapabilities();
        return false;
    }
    // Straight RGB is undefined as alpha approaches zero: two pixels can
    // differ by dozens of straight-channel values while contributing only one
    // premultiplied code value to the displayed result. The renderer contract
    // and CPU working canvas are premultiplied, so validate the representation
    // that is actually composited rather than hidden low-alpha colour.
    recordDifference(QStringLiteral("varying-alpha Pass Through"),
                     maximumPremultipliedDifference(expectedAlpha, actualAlpha),
                     2,
                     CapabilityGroup::Compositor);


    // Exercise four 256x256 composite tiles at once, with mask transitions and
    // semi-transparent raster content crossing both x=255/256 and y=255/256.
    // This guards the final layer/mask milestone against tile-edge seams that
    // a single small diagnostic tile cannot expose.
    QImage boundarySource(258, 258, QImage::Format_RGBA8888);
    QImage boundaryRaster(boundarySource.size(), QImage::Format_RGBA8888);
    QImage boundaryMask(boundarySource.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < boundarySource.height(); ++y) {
        uchar *maskRow = boundaryMask.scanLine(y);
        for (int x = 0; x < boundarySource.width(); ++x) {
            boundarySource.setPixelColor(
                x,
                y,
                QColor((19 + x * 5 + y * 3) & 255,
                       (83 + x * 2 + y * 7) & 255,
                       (37 + x * 11 + y) & 255,
                       96 + ((x * 3 + y * 5) % 160)));
            boundaryRaster.setPixelColor(
                x,
                y,
                QColor((227 + x * 3 + y) & 255,
                       (29 + x + y * 9) & 255,
                       (151 + x * 7 + y * 2) & 255,
                       48 + ((x * 5 + y * 7) % 208)));
            const int edgeBand = (x >= 254 && x <= 257) || (y >= 254 && y <= 257)
                ? ((x + y) & 1 ? 37 : 219)
                : (13 + x * 3 + y * 5) & 255;
            maskRow[x] = static_cast<uchar>(edgeBand);
        }
    }
    LayerNode boundaryBase;
    boundaryBase.type = LayerType::BaseImage;
    LayerNode boundaryChild;
    boundaryChild.type = LayerType::Raster;
    boundaryChild.rasterImage = boundaryRaster;
    boundaryChild.maskImage = boundaryMask;
    boundaryChild.opacity = 0.66;
    boundaryChild.blendMode = BlendMode::Overlay;
    LayerNode boundaryLevels;
    boundaryLevels.type = LayerType::Adjustment;
    boundaryLevels.adjustmentType = AdjustmentType::Levels;
    boundaryLevels.blackPoint = 0.04;
    boundaryLevels.whitePoint = 0.95;
    boundaryLevels.gamma = 1.16;
    boundaryLevels.maskImage = boundaryMask;
    boundaryLevels.maskInverted = true;
    LayerNode boundaryPass;
    boundaryPass.type = LayerType::Group;
    boundaryPass.groupCompositeMode = GroupCompositeMode::PassThrough;
    boundaryPass.children = {boundaryLevels, boundaryChild};
    boundaryPass.opacity = 0.71;
    boundaryPass.maskImage = boundaryMask;
    const QVector<LayerNode> boundaryLayers {boundaryPass, boundaryBase};
    const QImage expectedBoundary = ImageProcessor::renderRegion(
        boundarySource,
        boundaryLayers,
        boundarySource.rect(),
        boundarySource.size());
    const QImage actualBoundary = m_tiledCanvas.renderRegion(
        boundarySource,
        boundaryLayers,
        boundarySource.rect(),
        boundarySource.size(),
        true,
        0);
    if (expectedBoundary.isNull() || actualBoundary.isNull()
        || !m_tiledCanvas.lastBackendText().startsWith(QStringLiteral("Native WebGPU"))) {
        m_tiledCanvas.clear();
        if (details) {
            *details = QStringLiteral(
                " Native tiled cross-boundary layer/mask validation failed to execute on WebGPU; the affected feature uses its CPU reference path.");
        }
        recordDifference(QStringLiteral("cross-boundary hierarchy and masks"),
                         255, 2, CapabilityGroup::Compositor);
        publishMeasuredCapabilities();
        return false;
    }
    recordDifference(QStringLiteral("cross-boundary hierarchy and masks"),
                     maximumPremultipliedDifference(expectedBoundary, actualBoundary),
                     2,
                     CapabilityGroup::Compositor);

    const QVector<QLineF> maskStroke {
        QLineF(QPointF(-12.0, 18.0), QPointF(34.0, 33.0)),
        QLineF(QPointF(34.0, 33.0), QPointF(78.0, 48.0))
    };
    TiledCanvasEngine cpuMaskEngine;
    const TiledCanvasEngine::BrushResult expectedMask = cpuMaskEngine.stampMaskStroke(
        compactWhite,
        source.size(),
        QUuid::createUuid(),
        1,
        maskStroke,
        QTransform(),
        19.0,
        0.73,
        0.61,
        37,
        false,
        false);
    const TiledCanvasEngine::BrushResult actualMask = m_tiledCanvas.stampMaskStroke(
        compactWhite,
        source.size(),
        QUuid::createUuid(),
        1,
        maskStroke,
        QTransform(),
        19.0,
        0.73,
        0.61,
        37,
        false,
        true);
    if (expectedMask.image.isNull() || actualMask.image.isNull() || !actualMask.usedGpu) {
        m_tiledCanvas.clear();
        if (details) {
            *details = QStringLiteral(
                " Native tiled mask-brush validation failed to execute on WebGPU; the affected feature uses its CPU reference path.");
        }
        recordDifference(QStringLiteral("mask brush"), 255, 2,
                         CapabilityGroup::Brush);
        publishMeasuredCapabilities();
        return false;
    }
    int maskBrushDifference = 0;
    for (int y = 0; y < actualMask.image.height(); ++y) {
        const uchar *gpu = actualMask.image.constScanLine(y);
        const uchar *cpu = expectedMask.image.constScanLine(y);
        for (int x = 0; x < actualMask.image.width(); ++x) {
            maskBrushDifference = std::max(maskBrushDifference,
                                           std::abs(static_cast<int>(gpu[x])
                                                    - static_cast<int>(cpu[x])));
        }
    }
    recordDifference(QStringLiteral("mask brush"), maskBrushDifference, 2,
                     CapabilityGroup::Brush);

    // Clone Stamp has a stricter straight-RGBA contract than the compositor:
    // hidden RGB underneath zero alpha is editable data and must survive GPU
    // sampling, soft dabs and selection clipping. Exercise four destination
    // tiles, fractional source coordinates and both x/y tile boundaries.
    QImage cloneDestination(258, 258, QImage::Format_RGBA8888);
    QImage cloneSource(cloneDestination.size(), QImage::Format_RGBA8888);
    QImage cloneCoverage(cloneDestination.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < cloneDestination.height(); ++y) {
        uchar *destinationRow = cloneDestination.scanLine(y);
        uchar *sourceRow = cloneSource.scanLine(y);
        uchar *coverageRow = cloneCoverage.scanLine(y);
        for (int x = 0; x < cloneDestination.width(); ++x) {
            const int offset = x * 4;
            destinationRow[offset] = static_cast<uchar>((17 + x * 3 + y * 7) & 255);
            destinationRow[offset + 1] = static_cast<uchar>((131 + x * 11 + y) & 255);
            destinationRow[offset + 2] = static_cast<uchar>((53 + x + y * 5) & 255);
            destinationRow[offset + 3] = static_cast<uchar>(
                ((x + y) % 29 == 0) ? 0 : 31 + ((x * 5 + y * 3) % 225));
            sourceRow[offset] = static_cast<uchar>((229 + x * 7 + y * 2) & 255);
            sourceRow[offset + 1] = static_cast<uchar>((41 + x * 2 + y * 13) & 255);
            sourceRow[offset + 2] = static_cast<uchar>((167 + x * 5 + y * 3) & 255);
            sourceRow[offset + 3] = static_cast<uchar>(
                ((x * 3 + y) % 23 == 0) ? 0 : 19 + ((x * 7 + y * 11) % 237));
            coverageRow[x] = static_cast<uchar>(
                ((x >= 254 && x <= 257) || (y >= 254 && y <= 257))
                    ? (((x + y) & 1) ? 43 : 217)
                    : ((7 + x * 5 + y * 9) & 255));
        }
    }
    SelectionMask cloneSelection(cloneDestination.size());
    if (!cloneSelection.setCoverageImage(cloneDestination.rect(), cloneCoverage)) {
        m_tiledCanvas.clear();
        if (details) {
            *details = QStringLiteral(
                " Native tiled Clone Stamp selection validation could not prepare coverage; Clone Stamp uses its CPU reference path.");
        }
        recordDifference(QStringLiteral("straight-RGBA Clone Stamp"), 255, 3,
                         CapabilityGroup::CloneStamp);
        recordDifference(QStringLiteral("low-opacity overlapping Clone Stamp"), 255, 3,
                         CapabilityGroup::CloneStamp);
        recordDifference(QStringLiteral("scalar mask Clone Stamp"), 255, 3,
                         CapabilityGroup::CloneStamp);
        publishMeasuredCapabilities();
        return false;
    }
    const SelectionMask::Snapshot cloneSelectionSnapshot = cloneSelection.snapshot();

    CloneStampRequest cloneRequest;
    cloneRequest.destination = cloneDestination;
    cloneRequest.source = cloneSource;
    cloneRequest.targetSegments = {
        QLineF(QPointF(224.25, 235.75), QPointF(267.5, 260.25)),
        QLineF(QPointF(267.5, 260.25), QPointF(237.0, 271.5))
    };
    cloneRequest.targetLayerToDocument = QTransform::fromTranslate(2.25, -1.5);
    cloneRequest.sourceDocumentToLayer = QTransform::fromTranslate(-0.75, 1.125);
    cloneRequest.sourceOffsetDocument = QPointF(-121.625, -116.375);
    cloneRequest.diameterPixels = 43.0;
    cloneRequest.opacity = 0.71;
    cloneRequest.hardness = 0.46;
    cloneRequest.target = CloneStampTarget::RasterPixels;
    cloneRequest.sample = CloneStampSample::Rgba;

    const CloneStampResult cpuClone = applyCloneStamp(cloneRequest);
    const QImage expectedClone = applyStraightSelectionForParity(
        cloneDestination,
        cpuClone.image,
        cloneSelectionSnapshot,
        cloneRequest.targetLayerToDocument,
        cpuClone.affectedRect);
    const TiledCanvasEngine::BrushResult actualClone = m_tiledCanvas.stampCloneStroke(
        cloneRequest,
        cloneDestination.size(),
        QUuid::createUuid(),
        1,
        true,
        QUuid(),
        &cloneSelectionSnapshot);
    if (!cpuClone.error.isEmpty() || expectedClone.isNull()
        || actualClone.image.isNull() || !actualClone.usedGpu
        || !actualClone.selectionApplied) {
        m_tiledCanvas.clear();
        if (details) {
            *details = QStringLiteral(
                " Native tiled straight-RGBA Clone Stamp validation failed to execute on WebGPU; the affected feature uses its CPU reference path.");
        }
        recordDifference(QStringLiteral("straight-RGBA Clone Stamp"), 255, 3,
                         CapabilityGroup::CloneStamp);
        publishMeasuredCapabilities();
        return false;
    }
    recordDifference(QStringLiteral("straight-RGBA Clone Stamp"),
                     maximumStraightDifference(expectedClone, actualClone.image),
                     3,
                     CapabilityGroup::CloneStamp);

    // Very soft, low-opacity overlapping dabs previously quantised each RGB
    // update independently and could form coloured contour bands. Keep this
    // case in startup parity so both CPU and GPU accumulate the whole stroke in
    // floating point before the single RGBA8 write.
    CloneStampRequest softCloneRequest = cloneRequest;
    softCloneRequest.targetSegments = {
        QLineF(QPointF(206.5, 221.25), QPointF(276.75, 254.5)),
        QLineF(QPointF(276.75, 254.5), QPointF(246.25, 274.0))
    };
    softCloneRequest.diameterPixels = 57.0;
    softCloneRequest.opacity = 0.05;
    softCloneRequest.hardness = 0.0;
    const CloneStampResult cpuSoftClone = applyCloneStamp(softCloneRequest);
    const QImage expectedSoftClone = applyStraightSelectionForParity(
        cloneDestination,
        cpuSoftClone.image,
        cloneSelectionSnapshot,
        softCloneRequest.targetLayerToDocument,
        cpuSoftClone.affectedRect);
    const TiledCanvasEngine::BrushResult actualSoftClone =
        m_tiledCanvas.stampCloneStroke(
            softCloneRequest,
            cloneDestination.size(),
            QUuid::createUuid(),
            2,
            true,
            QUuid(),
            &cloneSelectionSnapshot);
    if (!cpuSoftClone.error.isEmpty() || expectedSoftClone.isNull()
        || actualSoftClone.image.isNull() || !actualSoftClone.usedGpu
        || !actualSoftClone.selectionApplied) {
        m_tiledCanvas.clear();
        if (details) {
            *details = QStringLiteral(
                " Native tiled low-opacity Clone Stamp validation failed to execute on WebGPU; the affected feature uses its CPU reference path.");
        }
        recordDifference(QStringLiteral("low-opacity overlapping Clone Stamp"), 255, 3,
                         CapabilityGroup::CloneStamp);
        publishMeasuredCapabilities();
        return false;
    }
    recordDifference(QStringLiteral("low-opacity overlapping Clone Stamp"),
                     maximumStraightDifference(expectedSoftClone,
                                               actualSoftClone.image),
                     3,
                     CapabilityGroup::CloneStamp);

    // Also exercise the scalar/mask branch and greyscale readback path rather
    // than relying on raster-only shader validation.
    QImage cloneMaskDestination(66, 66, QImage::Format_Grayscale8);
    for (int y = 0; y < cloneMaskDestination.height(); ++y) {
        uchar *row = cloneMaskDestination.scanLine(y);
        for (int x = 0; x < cloneMaskDestination.width(); ++x) {
            row[x] = static_cast<uchar>((13 + x * 7 + y * 5) & 255);
        }
    }
    CloneStampRequest maskCloneRequest;
    maskCloneRequest.destination = cloneMaskDestination;
    maskCloneRequest.source = cloneSource.copy(QRect(0, 0, 66, 66));
    maskCloneRequest.targetSegments = {
        QLineF(QPointF(7.0, 12.5), QPointF(61.0, 55.25))
    };
    maskCloneRequest.sourceOffsetDocument = QPointF(-3.375, 4.625);
    maskCloneRequest.diameterPixels = 21.0;
    maskCloneRequest.opacity = 0.64;
    maskCloneRequest.hardness = 0.52;
    maskCloneRequest.target = CloneStampTarget::Mask;
    maskCloneRequest.sample = CloneStampSample::Luminance;
    const CloneStampResult cpuMaskClone = applyCloneStamp(maskCloneRequest);
    const TiledCanvasEngine::BrushResult actualMaskClone = m_tiledCanvas.stampCloneStroke(
        maskCloneRequest,
        cloneMaskDestination.size(),
        QUuid::createUuid(),
        1,
        true);
    if (!cpuMaskClone.error.isEmpty() || cpuMaskClone.image.isNull()
        || actualMaskClone.image.isNull() || !actualMaskClone.usedGpu) {
        m_tiledCanvas.clear();
        if (details) {
            *details = QStringLiteral(
                " Native tiled mask Clone Stamp validation failed to execute on WebGPU; the affected feature uses its CPU reference path.");
        }
        recordDifference(QStringLiteral("scalar mask Clone Stamp"), 255, 3,
                         CapabilityGroup::CloneStamp);
        publishMeasuredCapabilities();
        return false;
    }
    const QImage expectedMaskClone = cpuMaskClone.image.convertToFormat(
        QImage::Format_Grayscale8);
    const QImage gpuMaskClone = actualMaskClone.image.convertToFormat(
        QImage::Format_Grayscale8);
    int maskCloneDifference = 0;
    for (int y = 0; y < gpuMaskClone.height(); ++y) {
        const uchar *gpu = gpuMaskClone.constScanLine(y);
        const uchar *cpu = expectedMaskClone.constScanLine(y);
        for (int x = 0; x < gpuMaskClone.width(); ++x) {
            maskCloneDifference = std::max(maskCloneDifference,
                                           std::abs(static_cast<int>(gpu[x])
                                                    - static_cast<int>(cpu[x])));
        }
    }
    recordDifference(QStringLiteral("scalar mask Clone Stamp"),
                     maskCloneDifference,
                     3,
                     CapabilityGroup::CloneStamp);
    m_tiledCanvas.clear();

    QStringList resultSummaries;
    QStringList failures;
    resultSummaries.reserve(measurements.size());
    bool compositorPassed = true;
    bool brushPassed = true;
    bool cloneStampPassed = true;
    bool compositorSeen = false;
    bool brushSeen = false;
    QStringList compositorFailures;
    bool cloneStampSeen = false;
    for (const ParityMeasurement &measurement : measurements) {
        const bool passed = measurement.difference <= measurement.tolerance;
        resultSummaries.push_back(QStringLiteral("%1 %2/%3")
                                      .arg(measurement.name)
                                      .arg(measurement.difference)
                                      .arg(measurement.tolerance));
        if (!passed) {
            failures.push_back(QStringLiteral("%1 (%2 > %3)")
                                   .arg(measurement.name)
                                   .arg(measurement.difference)
                                   .arg(measurement.tolerance));
        }
        switch (measurement.group) {
        case CapabilityGroup::Compositor:
            compositorSeen = true;
            compositorPassed = compositorPassed && passed;
            if (!passed) {
                compositorFailures.push_back(measurement.name);
            }
            break;
        case CapabilityGroup::Brush:
            brushSeen = true;
            brushPassed = brushPassed && passed;
            break;
        case CapabilityGroup::CloneStamp:
            cloneStampSeen = true;
            cloneStampPassed = cloneStampPassed && passed;
            break;
        }
    }
    compositorPassed = compositorSeen && compositorPassed;
    const bool standardCompositorPassed = compositorSeen
        && (compositorPassed
            || (compositorFailures.size() == 1
                && compositorFailures.front()
                    == QStringLiteral("deep mixed hierarchy")));
    brushPassed = brushSeen && brushPassed;
    cloneStampPassed = cloneStampSeen && cloneStampPassed;
    publishMeasuredCapabilities();

    if (details) {
        const QString compositorStatus = compositorPassed
            ? QStringLiteral("enabled")
            : (standardCompositorPassed
                   ? QStringLiteral("enabled for validated hierarchies; deep mixed nesting uses CPU fallback")
                   : QStringLiteral("CPU fallback"));
        const QString capabilitySummary = QStringLiteral(
            " GPU capabilities: compositor %1; raster/mask brush %2; Clone Stamp %3.")
                                              .arg(compositorStatus,
                                                   brushPassed
                                                       ? QStringLiteral("enabled")
                                                       : QStringLiteral("CPU fallback"),
                                                   cloneStampPassed
                                                       ? QStringLiteral("enabled")
                                                       : QStringLiteral("CPU fallback"));
        if (allWithinTolerance) {
            *details = capabilitySummary + QStringLiteral(
                " Native feature parity passed (maximum difference %1; case delta/limit: %2).")
                           .arg(maximumDifference)
                           .arg(resultSummaries.join(QStringLiteral(", ")));
        } else {
            *details = capabilitySummary + QStringLiteral(
                " Selective parity fallback was activated for %1. Other validated GPU capabilities remain available. Case delta/limit: %2.")
                           .arg(failures.join(QStringLiteral(", ")),
                                resultSummaries.join(QStringLiteral(", ")));
        }
    }
    return true;
}

bool RenderBackend::webGpuFoundationAvailable() const
{
    return m_webGpu.deviceReady();
}

bool RenderBackend::webGpuTileParityPassed() const
{
    QMutexLocker lock(&m_stateMutex);
    return m_gpuFoundationParityPassed;
}

bool RenderBackend::webGpuCompositorParityPassed() const
{
    QMutexLocker lock(&m_stateMutex);
    return m_gpuCompositorParityPassed
        || m_gpuStandardCompositorParityPassed;
}

bool RenderBackend::webGpuFullCompositorParityPassed() const
{
    QMutexLocker lock(&m_stateMutex);
    return m_gpuCompositorParityPassed;
}

bool RenderBackend::webGpuBrushParityPassed() const
{
    QMutexLocker lock(&m_stateMutex);
    return m_gpuBrushParityPassed;
}

bool RenderBackend::webGpuCloneStampParityPassed() const
{
    QMutexLocker lock(&m_stateMutex);
    return m_gpuCloneStampParityPassed;
}

bool RenderBackend::webGpuFillParityPassed() const
{
    QMutexLocker lock(&m_stateMutex);
    return m_gpuFoundationParityPassed && m_webGpu.fillGpuApproved();
}

bool RenderBackend::webGpuDisplayTransformParityPassed() const
{
    QMutexLocker lock(&m_stateMutex);
    return m_externalGpuApproved
        && m_gpuFoundationParityPassed
        && m_webGpu.deviceReady()
        && m_webGpu.displayTransformGpuApproved();
}

bool RenderBackend::webGpuManagedAdjustmentTransformParityPassed() const
{
    QMutexLocker lock(&m_stateMutex);
    return m_externalGpuApproved
        && m_gpuFoundationParityPassed
        && m_webGpu.deviceReady()
        && m_webGpu.managedAdjustmentTransformsGpuApproved();
}

bool RenderBackend::webGpuAdjustmentApproved(const AdjustmentType type) const
{
    QMutexLocker lock(&m_stateMutex);
    return m_externalGpuApproved
        && m_gpuFoundationParityPassed
        && m_webGpu.deviceReady()
        && m_webGpu.adjustmentGpuApproved(type);
}

void RenderBackend::activateSession(const RenderSessionContext &sessionContext)
{
    if (!sessionContext.isValid()) {
        return;
    }
    QMutexLocker lock(&m_stateMutex);
    m_sessionRenderIdentities.insert(
        sessionContext.documentSessionId,
        SessionRenderIdentity {sessionContext.renderSerial,
                               sessionContext.colourStateRevision,
                               sessionContext.processingCompatibility});
    m_activeDocumentSessionId = sessionContext.documentSessionId;
}

void RenderBackend::invalidateSurface(const RenderSessionContext &sessionContext,
                                      const QUuid &surfaceId)
{
    if (!sessionContext.isValid() || surfaceId.isNull()
        || !isSessionCurrent(sessionContext)) {
        return;
    }
    QMutexLocker renderLock(&m_renderMutex);
    if (!isSessionCurrent(sessionContext)) {
        return;
    }
    m_tiledCanvas.invalidateSurface(sessionContext.documentSessionId, surfaceId);
}

void RenderBackend::invalidateSurface(const QUuid &surfaceId)
{
    if (surfaceId.isNull()) {
        return;
    }
    QMutexLocker renderLock(&m_renderMutex);
    m_tiledCanvas.invalidateSurface(surfaceId);
}

void RenderBackend::resetSessionState(const RenderSessionContext &sessionContext)
{
    if (!sessionContext.isValid()) {
        return;
    }

    // Serialize replacement against all tile work. Publish the new serial while
    // holding the render gate, before evicting the old namespace, so a request
    // that was queued with the previous serial cannot slip through between the
    // cache reset and the identity update. Render paths use the same
    // render-then-state ordering after entering this gate.
    QMutexLocker renderLock(&m_renderMutex);
    {
        QMutexLocker stateLock(&m_stateMutex);
        m_sessionRenderIdentities.insert(
            sessionContext.documentSessionId,
            SessionRenderIdentity {sessionContext.renderSerial,
                                   sessionContext.colourStateRevision,
                                   sessionContext.processingCompatibility});
        m_displayedRenderStates.remove(sessionContext.documentSessionId);
        m_activeDocumentSessionId = sessionContext.documentSessionId;
    }
    m_tiledCanvas.invalidateSession(sessionContext.documentSessionId);
}

void RenderBackend::releaseSession(const QUuid &documentSessionId)
{
    if (documentSessionId.isNull()) {
        return;
    }

    // Retire the identity before releasing the render gate. Any queued request
    // rechecks its context after acquiring the gate and is rejected rather than
    // repopulating a session that has already been closed.
    QMutexLocker renderLock(&m_renderMutex);
    {
        QMutexLocker stateLock(&m_stateMutex);
        m_sessionRenderIdentities.remove(documentSessionId);
        m_displayedRenderStates.remove(documentSessionId);
        if (m_activeDocumentSessionId == documentSessionId) {
            m_activeDocumentSessionId = {};
        }
    }
    m_tiledCanvas.invalidateSession(documentSessionId);
}

QImage RenderBackend::roundTripDiagnosticTile(const QImage &source, QString *error)
{
    QMutexLocker lock(&m_stateMutex);
    if (!m_gpuInitialisationAttempted) {
        if (error) {
            *error = QStringLiteral("The native WebGPU foundation has not been initialised in this process");
        }
        return {};
    }
    lock.unlock();
    return m_webGpu.roundTripTile(source, error);
}

QImage RenderBackend::resampleImageTiled(
    const QImage &source,
    const QSize &destinationSize,
    const ImageResampleMethod method,
    const std::atomic_bool *cancelRequested,
    QString *error)
{
    if (error) {
        error->clear();
    }
    if (cancelRequested
        && cancelRequested->load(std::memory_order_acquire)) {
        if (error) *error = QStringLiteral("Image resize cancelled");
        return {};
    }
    if (source.depth() > 32) {
        if (error) {
            *error = QStringLiteral(
                "Native tiled GPU resize currently supports 8-bit payloads; "
                "using the exact 16-bit CPU reference");
        }
        return {};
    }

    // Share the same render gate as canvas/brush work. Image Size is modal, so
    // serialising this bounded one-off operation prevents temporary resize
    // textures from competing with resident preview tiles or shutdown.
    QMutexLocker renderLock(&m_renderMutex);
    if (!canvasGpuReady()) {
        if (error) {
            *error = QStringLiteral(
                "The validated native WebGPU resize path is unavailable; "
                "using the CPU reference");
        }
        return {};
    }
    return m_webGpu.resampleImageTiled(source,
                                       destinationSize,
                                       method,
                                       cancelRequested,
                                       error);
}


QImage RenderBackend::transformPreviewComposite(
    const QImage &background,
    const QImage &foreground,
    const QTransform &previewTransform,
    QString *error)
{
    if (error) {
        error->clear();
    }
    if (background.isNull() || foreground.isNull()
        || background.size() != foreground.size()
        || background.depth() > 32 || foreground.depth() > 32) {
        if (error) {
            *error = QStringLiteral(
                "Native transform preview requires matching 8-bit surfaces");
        }
        return {};
    }
    QMutexLocker renderLock(&m_renderMutex);
    if (!canvasGpuReady()) {
        if (error) {
            *error = QStringLiteral(
                "The validated native WebGPU transform preview is unavailable");
        }
        return {};
    }
    return m_webGpu.transformPreviewComposite(background,
                                               foreground,
                                               previewTransform,
                                               error);
}


QImage RenderBackend::applyDisplayColourTransform(
    const QImage &source,
    const DisplayColourTransform &transform,
    const std::atomic_bool *cancelRequested,
    QString *error)
{
    if (error) error->clear();
    if (source.isNull() || transform.isIdentity()) {
        return source.copy();
    }
    if (cancelRequested
        && cancelRequested->load(std::memory_order_acquire)) {
        if (error) *error = QStringLiteral("Display transform cancelled");
        return {};
    }

    QMutexLocker renderLock(&m_renderMutex);
    if (!canvasGpuReady() || !m_webGpu.displayTransformGpuApproved()) {
        if (error) *error = QStringLiteral(
            "The validated WGSL display-transform path is unavailable");
        return {};
    }
    QString lutError;
    const auto lut = transform.gpuLutData(&lutError);
    if (!lut) {
        if (error) {
            *error = lutError.isEmpty()
                ? QStringLiteral("The display transform could not be baked for GPU execution")
                : lutError;
        }
        return {};
    }
    return m_webGpu.applyDisplayColourTransform(
        source, *lut, cancelRequested, error);
}

GpuDiagnosticCapabilities RenderBackend::diagnosticCapabilities() const
{
    GpuDiagnosticCapabilities capabilities;
    {
        QMutexLocker lock(&m_stateMutex);
        capabilities.valid = m_gpuInitialisationAttempted;
        capabilities.foundation = m_gpuFoundationParityPassed;
        capabilities.compositor = m_gpuCompositorParityPassed;
        capabilities.standardCompositor = m_gpuStandardCompositorParityPassed;
        capabilities.brush = m_gpuBrushParityPassed;
        capabilities.cloneStamp = m_gpuCloneStampParityPassed;
        capabilities.localValidationStatus = m_localValidationStatus;
    }
    capabilities.webGpu = m_webGpu.validationState();
    return capabilities;
}

void RenderBackend::setExternalDiagnosticResult(
    const QString &status,
    const GpuDiagnosticCapabilities &capabilities)
{
    QMutexLocker lock(&m_stateMutex);
    m_externalDiagnosticStatus = status;
    m_externalCapabilities = capabilities;
    m_externalCapabilitiesAvailable = capabilities.valid;
    m_externalGpuApproved = capabilities.valid && capabilities.foundation;
}

void RenderBackend::setExternalDiagnosticStatus(const QString &status, const bool approved)
{
    QMutexLocker lock(&m_stateMutex);
    m_externalDiagnosticStatus = status;
    m_externalGpuApproved = approved;
    m_externalCapabilitiesAvailable = false;
    m_externalCapabilities = {};
}

void RenderBackend::resetDocumentState()
{
    QMutexLocker renderLock(&m_renderMutex);
    {
        QMutexLocker stateLock(&m_stateMutex);
        m_sessionRenderIdentities.clear();
        m_displayedRenderStates.clear();
        m_activeDocumentSessionId = {};
    }
    m_tiledCanvas.clear();
    VectorRasterizer::clearCache();
}

void RenderBackend::setDisplayedRenderInfo(const RenderSessionContext &sessionContext,
                                           const TiledCanvasEngine::RenderInfo &info,
                                           const quint64 generation,
                                           const int level)
{
    if (!sessionContext.isValid() || info.cancelled || info.path.isEmpty()) {
        return;
    }
    TiledCanvasEngine::RenderInfo normalised = info;
    normalised.mixedBackend = normalised.usedGpu && normalised.usedCpu;
    normalised.path = pathForRenderInfo(normalised);

    QMutexLocker lock(&m_stateMutex);
    if (!sessionCurrentLocked(sessionContext)
        || m_activeDocumentSessionId != sessionContext.documentSessionId) {
        return;
    }
    DisplayedRenderState &state = m_displayedRenderStates[sessionContext.documentSessionId];
    if (state.hasInfo && generation < state.generation) {
        return;
    }
    if (state.hasInfo && generation == state.generation) {
        if (state.level >= 0 && level > state.level) {
            return;
        }
        if (state.level < 0 || level < state.level) {
            state.info = normalised;
            state.level = level;
        } else {
            mergeDisplayedRenderInfo(&state.info, normalised);
        }
        return;
    }

    state.info = normalised;
    state.generation = generation;
    state.level = level;
    state.hasInfo = true;
}

void RenderBackend::setDisplayedRenderInfo(const TiledCanvasEngine::RenderInfo &info,
                                           const quint64 generation,
                                           const int level)
{
    if (info.cancelled || info.path.isEmpty()) {
        return;
    }
    TiledCanvasEngine::RenderInfo normalised = info;
    normalised.mixedBackend = normalised.usedGpu && normalised.usedCpu;
    normalised.path = pathForRenderInfo(normalised);

    QMutexLocker lock(&m_stateMutex);
    DisplayedRenderState &state = m_displayedRenderStates[QUuid()];
    m_activeDocumentSessionId = {};
    if (state.hasInfo && generation < state.generation) {
        return;
    }
    if (state.hasInfo && generation == state.generation) {
        if (state.level >= 0 && level > state.level) {
            return;
        }
        if (state.level < 0 || level < state.level) {
            state.info = normalised;
            state.level = level;
        } else {
            mergeDisplayedRenderInfo(&state.info, normalised);
        }
        return;
    }
    state.info = normalised;
    state.generation = generation;
    state.level = level;
    state.hasInfo = true;
}

QString RenderBackend::statusText() const
{
    QString externalStatus;
    QString localValidationStatus;
    bool attempted = false;
    QUuid activeSessionId;
    DisplayedRenderState displayedState;
    {
        QMutexLocker lock(&m_stateMutex);
        externalStatus = m_externalDiagnosticStatus;
        localValidationStatus = m_localValidationStatus;
        attempted = m_gpuInitialisationAttempted;
        activeSessionId = m_activeDocumentSessionId;
        const auto iterator = m_displayedRenderStates.constFind(activeSessionId);
        if (iterator != m_displayedRenderStates.cend()) {
            displayedState = iterator.value();
        }
    }
    QMutexLocker renderLock(&m_renderMutex);
    const TileCache::Stats stats = m_tiledCanvas.cacheStats();
    const TileCache::Stats activeStats = m_tiledCanvas.cacheStatsForSession(activeSessionId);
    QString displayedText;
    if (displayedState.hasInfo) {
        displayedText = QStringLiteral(
            " Current displayed document: %1. Visible groups: %2 Pass Through, %3 Isolated; maximum nesting depth %4; published generation %5 at level %6.")
                            .arg(displayedState.info.path)
                            .arg(displayedState.info.visiblePassThroughGroups)
                            .arg(displayedState.info.visibleIsolatedGroups)
                            .arg(displayedState.info.maximumGroupDepth)
                            .arg(displayedState.generation)
                            .arg(displayedState.level);
        if (!displayedState.info.fallbackReason.isEmpty()) {
            displayedText += QStringLiteral(" Fallback detail: %1.")
                                 .arg(displayedState.info.fallbackReason);
        }
    } else {
        displayedText = QStringLiteral(
            " Current displayed document path has not been published yet.");
    }
    const QString cacheText = QStringLiteral(
        " Tiles: %1 resident globally (%2 active-session), %3 dirty; RAM %4 MiB globally (%5 MiB active-session); VRAM %6 MiB (%7 native textures); evictions %8; stale cache publications rejected %9; obsolete composite tiles cancelled %10. Last operation path: %11.")
                                  .arg(stats.residentTiles)
                                  .arg(activeStats.residentTiles)
                                  .arg(stats.dirtyTiles)
                                  .arg(stats.ramBytes / (1024.0 * 1024.0), 0, 'f', 1)
                                  .arg(activeStats.ramBytes / (1024.0 * 1024.0), 0, 'f', 1)
                                  .arg(m_webGpu.residentVramBytes() / (1024.0 * 1024.0), 0, 'f', 1)
                                  .arg(m_webGpu.residentTileCount())
                                  .arg(stats.evictions)
                                  .arg(stats.rejectedStalePublications)
                                  .arg(m_tiledCanvas.cancelledCompositeTiles())
                                  .arg(m_tiledCanvas.lastBackendText());
    if (!externalStatus.isEmpty()) {
        return externalStatus + displayedText + cacheText;
    }
    if (!attempted) {
        return QStringLiteral(
            "Native WebGPU diagnostic is pending in an isolated helper process. "
            "The tiled CPU reference renderer remains available.") + displayedText + cacheText;
    }
    return m_webGpu.statusText() + localValidationStatus + displayedText + cacheText;
}

} // namespace vfx
