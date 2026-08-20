#pragma once

#include "Adjustment.h"
#include "CubeLut.h"
#include "DisplayColourManagement.h"
#include "CloneStamp.h"
#include "FillOperations.h"
#include "GradientOperations.h"
#include "ManagedAdjustmentGpuLut.h"
#include "ImageSizeOperations.h"

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QTransform>
#include <QVector>
#include <QtGlobal>

#include <atomic>
#include <memory>

namespace vfx {

struct WebGpuValidationState {
    bool foundation = false;
    bool fill = false;
    bool gradient = false;
    bool vectorFeather = false;
    bool displayTransform = false;
    bool managedAdjustmentTransforms = false;
    quint32 approvedAdjustmentMask = 0;
};

struct PreparedTileLayer {
    enum class Kind {
        Image,
        Group,
        Adjustment
    };

    Kind kind = Kind::Image;
    QImage image;
    QImage mask;
    // Optional immutable tile residency supplied by the Smart Layer cache.
    // When the key/revision is present and still resident, the hierarchy
    // compositor borrows that native texture instead of uploading the same
    // Smart intermediate tile after every lower-layer composite invalidation.
    quint64 residentTileKey = 0;
    quint64 residentTileRevision = 0;
    double opacity = 1.0;
    BlendMode blendMode = BlendMode::Copy;
    GroupCompositeMode groupCompositeMode = GroupCompositeMode::Isolated;
    QVector<PreparedTileLayer> children;

    AdjustmentType adjustmentType = AdjustmentType::Exposure;
    AdjustmentProcessingDomain processingDomain =
        AdjustmentProcessingDomain::EncodedWorking;
    std::shared_ptr<const ManagedAdjustmentGpuLutData> managedDomainLut;
    double exposure = 0.0;
    double contrast = 0.0;
    double saturation = 0.0;
    double blackPoint = 0.0;
    double whitePoint = 1.0;
    double gamma = 1.0;
    ExposureParameters exposureParameters;
    ContrastParameters contrastParameters;
    SaturationParameters saturationParameters;
    LevelsParameters levels;
    CurvesParameters curves;
    HueSaturationParameters hueSaturationParameters;
    VibranceParameters vibranceParameters;
    WhiteBalanceParameters whiteBalanceParameters;
    ColourBalanceParameters colourBalanceParameters;
    ChannelMixerParameters channelMixerParameters;
    BlackAndWhiteParameters blackAndWhiteParameters;
    GradientMapParameters gradientMapParameters;
    PosteriseParameters posteriseParameters;
    ThresholdParameters thresholdParameters;
    InvertParameters invertParameters;
    PhotoFilterParameters photoFilterParameters;
    SelectiveColourParameters selectiveColourParameters;
    LutParameters lutParameters;
    ShadowsHighlightsParameters shadowsHighlightsParameters;
    LutGpuTextureData lutLookup;
    LutDocumentTransfer lutDocumentTransfer = LutDocumentTransfer::EncodedSrgb;
    QImage tonalLookup;

    bool isGroup() const { return kind == Kind::Group; }
    bool isAdjustment() const { return kind == Kind::Adjustment; }
};

struct VectorFeatherGpuTileData;

class WebGpuContext final {
public:
    WebGpuContext();
    ~WebGpuContext();

    WebGpuContext(const WebGpuContext &) = delete;
    WebGpuContext &operator=(const WebGpuContext &) = delete;

    bool compiledIn() const;
    bool initialise();
    void shutdown();
    bool isInitialised() const;
    bool deviceReady() const;

    QImage roundTripTile(const QImage &source, QString *error = nullptr);

    // Bounded one-off resize path. Destination work is split into 256-pixel
    // tiles and each tile uploads only the source patch (plus bilinear halo)
    // needed by its global pixel-centre mapping. This keeps temporary VRAM
    // bounded and makes adjacent output tiles sample the same source pixels.
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
        const DisplayGpuLutData &lut,
        const std::atomic_bool *cancelRequested = nullptr,
        QString *error = nullptr);

    QImage stampBrushTile(const QImage &source,
                          const QPoint &tileOrigin,
                          const QVector<QPointF> &stampPoints,
                          double radius,
                          double hardness,
                          double opacity,
                          const QColor &colour,
                          bool erasing,
                          quint64 residencyKey,
                          quint64 sourceRevision,
                          const QImage &selectionCoverage,
                          QString *error = nullptr);

    QImage applyFillTile(const QImage &source,
                         const QImage &coverage,
                         FillTarget target,
                         int componentIndex,
                         const QColor &colour,
                         bool preserveTransparency,
                         QString *error = nullptr);

    QImage applyGradientTile(const QImage &source,
                             const QImage &coverage,
                             const QPoint &tileOrigin,
                             FillTarget target,
                             int componentIndex,
                             const QPointF &start,
                             const QPointF &end,
                             RasterGradientType type,
                             const QColor &startColour,
                             const QColor &endColour,
                             bool reverse,
                             QString *error = nullptr);

    QImage stampCloneTile(const QImage &destination,
                          const QPoint &destinationTileOrigin,
                          const QImage &sourcePatch,
                          const QPoint &sourcePatchOrigin,
                          const QSize &sourceImageSize,
                          const QVector<QPointF> &stampPoints,
                          const QTransform &targetPixelToLayer,
                          const QTransform &targetLayerToDocument,
                          const QTransform &sourceDocumentToLayer,
                          const QTransform &sourceLayerToPixel,
                          const QPointF &sourceOffsetDocument,
                          double radius,
                          double hardness,
                          double opacity,
                          CloneStampTarget target,
                          CloneStampSample sample,
                          int componentIndex,
                          bool sourceIsGrey,
                          const QImage &selectionCoverage,
                          QString *error = nullptr);

    QImage compositeTile(const QImage &base,
                         const QImage &layer,
                         const QImage &mask,
                         double opacity,
                         BlendMode blendMode,
                         QString *error = nullptr);

    // Applies only the combined vector coverage Feather on the native GPU.
    // Semantic geometry and exact nearest authored-colour propagation are
    // prepared separately so WGSL never blurs fill/stroke RGB together.
    QImage featherVectorCoverageTile(const VectorFeatherGpuTileData &prepared,
                                     QString *error = nullptr);

    QImage compositeHierarchyTile(const QSize &size,
                                  const QColorSpace &colourSpace,
                                  const QVector<PreparedTileLayer> &layers,
                                  QString *error = nullptr);

    void cacheResidentTile(quint64 residencyKey,
                           quint64 revision,
                           const QImage &image);
    void invalidateResidentTile(quint64 residencyKey);
    qsizetype residentVramBytes() const;
    int residentTileCount() const;

    bool runTileParitySelfTest(QString *details = nullptr);
    WebGpuValidationState validationState() const;
    void adoptExternalValidationState(const WebGpuValidationState &state);
    bool fillGpuApproved() const;
    bool gradientGpuApproved() const;
    bool vectorFeatherGpuApproved() const;
    bool displayTransformGpuApproved() const;
    bool managedAdjustmentTransformsGpuApproved() const;
    bool adjustmentGpuApproved(AdjustmentType type) const;
    quint32 approvedAdjustmentMask() const;
    QString statusText() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vfx
