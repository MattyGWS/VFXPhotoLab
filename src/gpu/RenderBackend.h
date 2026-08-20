#pragma once

#include "Adjustment.h"
#include "ColourManagement.h"
#include "gpu/TiledCanvasEngine.h"
#include "gpu/WebGpuContext.h"

#include <QColor>
#include <QColorSpace>
#include <QHash>
#include <QImage>
#include <QLineF>
#include <QMutex>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTransform>
#include <QUuid>
#include <QVector>

#include <atomic>

namespace vfx {

struct RenderSessionContext {
    QUuid documentSessionId;
    quint64 renderSerial = 0;
    quint64 colourStateRevision = 0;
    ColourProcessingCompatibility processingCompatibility =
        ColourProcessingCompatibility::LegacyV1;

    RenderSessionContext() = default;
    RenderSessionContext(const QUuid &sessionId,
                         const quint64 serial,
                         const quint64 colourRevision,
                         const ColourProcessingCompatibility compatibility =
                             ColourProcessingCompatibility::LegacyV1)
        : documentSessionId(sessionId)
        , renderSerial(serial)
        , colourStateRevision(colourRevision)
        , processingCompatibility(compatibility)
    {
    }

    bool isValid() const
    {
        return !documentSessionId.isNull() && renderSerial != 0;
    }
};

struct GpuDiagnosticCapabilities {
    bool valid = false;
    bool foundation = false;
    bool compositor = false;
    bool standardCompositor = false;
    bool brush = false;
    bool cloneStamp = false;
    WebGpuValidationState webGpu;
    QString localValidationStatus;
};

class RenderBackend final {
public:
    static RenderBackend &instance();

    QImage renderRegion(const RenderSessionContext &sessionContext,
                        const QImage &source,
                        const QVector<LayerNode> &layers,
                        const QRect &previewRegion,
                        const QSize &documentSize,
                        int level = 0,
                        const std::atomic_bool *cancelRequested = nullptr,
                        TiledCanvasEngine::RenderInfo *renderInfo = nullptr);

    QImage renderRegion(const QImage &source,
                        const QVector<LayerNode> &layers,
                        const QRect &previewRegion,
                        const QSize &documentSize,
                        int level = 0,
                        const std::atomic_bool *cancelRequested = nullptr,
                        TiledCanvasEngine::RenderInfo *renderInfo = nullptr);

    QImage renderInteractiveRegion(const RenderSessionContext &sessionContext,
                                   const QImage &source,
                                   const QVector<LayerNode> &layers,
                                   const QRect &previewRegion,
                                   const QSize &documentSize,
                                   int level = 0,
                                   const std::atomic_bool *cancelRequested = nullptr,
                                   TiledCanvasEngine::RenderInfo *renderInfo = nullptr,
                                   bool skipPersistentCacheFallback = false);

    TiledCanvasEngine::BrushResult stampRasterStroke(
        const RenderSessionContext &sessionContext,
        const QImage &sourceRaster,
        const QSize &documentSize,
        const QColorSpace &colourSpace,
        const QUuid &layerId,
        quint64 layerRevision,
        const QVector<QLineF> &documentSegments,
        const QTransform &documentToLayer,
        double diameter,
        double opacity,
        double hardness,
        const QColor &colour,
        bool erasing,
        bool allowGpu = true,
        const SelectionMask::Snapshot *selectionSnapshot = nullptr,
        const QTransform &layerToDocument = QTransform());

    TiledCanvasEngine::BrushResult stampRasterStroke(
        const QImage &sourceRaster,
        const QSize &documentSize,
        const QColorSpace &colourSpace,
        const QUuid &layerId,
        quint64 layerRevision,
        const QVector<QLineF> &documentSegments,
        const QTransform &documentToLayer,
        double diameter,
        double opacity,
        double hardness,
        const QColor &colour,
        bool erasing,
        bool allowGpu = true,
        const SelectionMask::Snapshot *selectionSnapshot = nullptr,
        const QTransform &layerToDocument = QTransform());

    TiledCanvasEngine::BrushResult stampChannelStroke(
        const RenderSessionContext &sessionContext,
        const QImage &sourceRaster,
        const QSize &documentSize,
        const QColorSpace &colourSpace,
        const QUuid &layerId,
        quint64 layerRevision,
        const QVector<QLineF> &documentSegments,
        const QTransform &documentToLayer,
        double diameter,
        double opacity,
        double hardness,
        int channelIndex,
        int channelValue);

    TiledCanvasEngine::BrushResult stampChannelStroke(
        const QImage &sourceRaster,
        const QSize &documentSize,
        const QColorSpace &colourSpace,
        const QUuid &layerId,
        quint64 layerRevision,
        const QVector<QLineF> &documentSegments,
        const QTransform &documentToLayer,
        double diameter,
        double opacity,
        double hardness,
        int channelIndex,
        int channelValue);

    TiledCanvasEngine::BrushResult stampMaskStroke(
        const RenderSessionContext &sessionContext,
        const QImage &sourceMask,
        const QSize &documentSize,
        const QUuid &layerId,
        quint64 layerRevision,
        const QVector<QLineF> &documentSegments,
        const QTransform &documentToLayer,
        double diameter,
        double opacity,
        double hardness,
        int greyscaleValue,
        bool restoringCoverage,
        bool allowGpu = true,
        const SelectionMask::Snapshot *selectionSnapshot = nullptr,
        const QTransform &layerToDocument = QTransform());

    TiledCanvasEngine::BrushResult stampCloneStroke(
        const RenderSessionContext &sessionContext,
        const CloneStampRequest &request,
        const QSize &documentSize,
        const QUuid &layerId,
        quint64 layerRevision,
        bool allowGpu,
        const SelectionMask::Snapshot *selectionSnapshot = nullptr);

    TiledCanvasEngine::BrushResult stampCloneStroke(
        const CloneStampRequest &request,
        const QSize &documentSize,
        const QUuid &layerId,
        quint64 layerRevision,
        bool allowGpu,
        const SelectionMask::Snapshot *selectionSnapshot = nullptr);

    TiledCanvasEngine::BrushResult stampMaskStroke(
        const QImage &sourceMask,
        const QSize &documentSize,
        const QUuid &layerId,
        quint64 layerRevision,
        const QVector<QLineF> &documentSegments,
        const QTransform &documentToLayer,
        double diameter,
        double opacity,
        double hardness,
        int greyscaleValue,
        bool restoringCoverage,
        bool allowGpu = true,
        const SelectionMask::Snapshot *selectionSnapshot = nullptr,
        const QTransform &layerToDocument = QTransform());

    TiledCanvasEngine::FillResult applyFillCoverage(
        const RenderSessionContext &sessionContext,
        const QImage &sourceImage,
        const QImage &coverage,
        const QUuid &layerId,
        quint64 layerRevision,
        FillTarget target,
        int componentIndex,
        const QColor &colour,
        bool preserveTransparency,
        bool allowGpu = true);

    TiledCanvasEngine::FillResult applyFillCoverage(
        const QImage &sourceImage,
        const QImage &coverage,
        const QUuid &layerId,
        quint64 layerRevision,
        FillTarget target,
        int componentIndex,
        const QColor &colour,
        bool preserveTransparency,
        bool allowGpu = true);

    TiledCanvasEngine::FillResult applyGradient(
        const RenderSessionContext &sessionContext,
        const GradientApplyRequest &request,
        const QUuid &layerId,
        quint64 layerRevision,
        bool allowGpu = true);

    TiledCanvasEngine::FillResult applyGradient(
        const GradientApplyRequest &request,
        const QUuid &layerId,
        quint64 layerRevision,
        bool allowGpu = true);

    bool initialiseGpuFoundation();
    void shutdownGpuFoundation();
    bool webGpuFoundationAvailable() const;
    bool webGpuTileParityPassed() const;
    bool webGpuCompositorParityPassed() const;
    bool webGpuFullCompositorParityPassed() const;
    bool webGpuBrushParityPassed() const;
    bool webGpuCloneStampParityPassed() const;
    bool webGpuFillParityPassed() const;
    bool webGpuDisplayTransformParityPassed() const;
    bool webGpuManagedAdjustmentTransformParityPassed() const;
    bool webGpuAdjustmentApproved(AdjustmentType type) const;
    QImage roundTripDiagnosticTile(const QImage &source, QString *error = nullptr);
    QImage resampleImageTiled(const QImage &source,
                              const QSize &destinationSize,
                              ImageResampleMethod method,
                              const std::atomic_bool *cancelRequested = nullptr,
                              QString *error = nullptr);
    QImage transformPreviewComposite(const QImage &background,
                                     const QImage &foreground,
                                     const QTransform &previewTransform,
                                     QString *error = nullptr);
    QImage applyDisplayColourTransform(
        const QImage &source,
        const DisplayColourTransform &transform,
        const std::atomic_bool *cancelRequested = nullptr,
        QString *error = nullptr);

    GpuDiagnosticCapabilities diagnosticCapabilities() const;
    void setExternalDiagnosticResult(
        const QString &status,
        const GpuDiagnosticCapabilities &capabilities);
    void setExternalDiagnosticStatus(const QString &status, bool approved = false);

    void activateSession(const RenderSessionContext &sessionContext);
    bool isSessionCurrent(const RenderSessionContext &sessionContext) const;
    void invalidateSurface(const RenderSessionContext &sessionContext,
                           const QUuid &surfaceId);
    void resetSessionState(const RenderSessionContext &sessionContext);
    void releaseSession(const QUuid &documentSessionId);
    void setDisplayedRenderInfo(const RenderSessionContext &sessionContext,
                                const TiledCanvasEngine::RenderInfo &info,
                                quint64 generation,
                                int level);

    // Compatibility entry points for backend self-tests and unit tests that do
    // not belong to a live document session. Production document work should
    // always use the explicit RenderSessionContext overloads above.
    void invalidateSurface(const QUuid &surfaceId);
    void setDisplayedRenderInfo(const TiledCanvasEngine::RenderInfo &info,
                                quint64 generation,
                                int level);
    void resetDocumentState();
    QString statusText() const;

private:
    struct SessionRenderIdentity {
        quint64 renderSerial = 0;
        quint64 colourStateRevision = 0;
        ColourProcessingCompatibility processingCompatibility =
            ColourProcessingCompatibility::LegacyV1;

        bool matches(const RenderSessionContext &context) const
        {
            return renderSerial == context.renderSerial
                && colourStateRevision == context.colourStateRevision
                && processingCompatibility == context.processingCompatibility;
        }
    };

    struct DisplayedRenderState {
        TiledCanvasEngine::RenderInfo info;
        quint64 generation = 0;
        int level = -1;
        bool hasInfo = false;
    };

    struct NativeGpuCapabilities {
        bool compositor = false;
        bool standardCompositor = false;
        bool brush = false;
        bool cloneStamp = false;
    };

    RenderBackend();
    bool canvasGpuReady();
    bool compositorGpuReady(const QVector<LayerNode> &layers,
                            const QColorSpace &colourSpace);
    bool brushGpuReady();
    bool cloneStampGpuReady();
    bool fillGpuReady();
    bool gradientGpuReady();
    bool sessionCurrentLocked(const RenderSessionContext &sessionContext) const;
    bool runNativeHierarchyParitySelfTest(NativeGpuCapabilities *capabilities,
                                          QString *details = nullptr);

    mutable QMutex m_stateMutex;
    mutable QMutex m_renderMutex;
    WebGpuContext m_webGpu;
    TiledCanvasEngine m_tiledCanvas;
    bool m_gpuInitialisationAttempted = false;
    bool m_gpuFoundationParityPassed = false;
    bool m_gpuCompositorParityPassed = false;
    bool m_gpuStandardCompositorParityPassed = false;
    bool m_gpuBrushParityPassed = false;
    bool m_gpuCloneStampParityPassed = false;
    bool m_externalGpuApproved = false;
    bool m_externalCapabilitiesAvailable = false;
    GpuDiagnosticCapabilities m_externalCapabilities;
    QString m_externalDiagnosticStatus;
    QString m_localValidationStatus;
    QHash<QUuid, SessionRenderIdentity> m_sessionRenderIdentities;
    QHash<QUuid, DisplayedRenderState> m_displayedRenderStates;
    QUuid m_activeDocumentSessionId;
};

} // namespace vfx
