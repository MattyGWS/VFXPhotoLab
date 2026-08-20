#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <atomic>
#include <cstddef>
#include <functional>

namespace vfx {

using SpatialRowProcessor = std::function<void(
    int width, int height, const std::function<void(int)> &processRow)>;

// Shared spatial-processing contract introduced in 0.13.0a. Filters describe
// their document-space footprint once; the CPU renderer, tiled compositor and
// future live-filter cache derive identical preview halos and invalidation.
enum class SpatialEdgeMode : quint8 {
    Clamp = 0,
    Mirror = 1,
    Wrap = 2,
    Transparent = 3
};

enum class SpatialAlphaMode : quint8 {
    // Filter all four straight components independently. Hidden RGB remains
    // meaningful even when the filtered alpha is zero.
    StraightRgba = 0,
    // Filter straight RGB while retaining the source alpha exactly. This is
    // the contract used by Shadows/Highlights and colour-only spatial effects.
    PreserveSourceAlpha = 1,
    // Filter premultiplied colour and alpha, then unpremultiply. Where the
    // result has zero alpha, retain the independently filtered hidden RGB.
    CoverageAwareRgba = 2
};

enum class SpatialPreviewQuality : quint8 {
    Interactive = 0,
    Final = 1
};

struct SpatialFilterContract {
    static constexpr quint32 CurrentVersion = 1;
    static constexpr int DefaultMaximumRadius = 4096;
    static constexpr int DefaultSafetyPadding = 2;

    quint32 version = CurrentVersion;
    QSize documentRadius;
    SpatialEdgeMode edgeMode = SpatialEdgeMode::Clamp;
    SpatialAlphaMode alphaMode = SpatialAlphaMode::PreserveSourceAlpha;
    SpatialPreviewQuality quality = SpatialPreviewQuality::Final;
    int safetyPadding = DefaultSafetyPadding;
    int maximumRadius = DefaultMaximumRadius;
    bool deterministic = true;

    void normalise();
    bool isIdentity() const;
    quint64 fingerprint() const;
    bool operator==(const SpatialFilterContract &) const = default;
};

// POD layout mirrored by the future WGSL live-filter kernels. Keeping this
// stable now prevents each filter from inventing its own tile-origin, halo and
// edge-mode uniform packing later.
struct alignas(16) SpatialFilterGpuContract {
    quint32 sourceWidth = 0;
    quint32 sourceHeight = 0;
    qint32 outputOriginX = 0;
    qint32 outputOriginY = 0;

    quint32 outputWidth = 0;
    quint32 outputHeight = 0;
    quint32 radiusX = 0;
    quint32 radiusY = 0;

    quint32 edgeMode = 0;
    quint32 alphaMode = 0;
    quint32 quality = 0;
    quint32 flags = 0;

    qint32 samplingOriginX = 0;
    qint32 samplingOriginY = 0;
    quint32 samplingWidth = 0;
    quint32 samplingHeight = 0;
};
static_assert(sizeof(SpatialFilterGpuContract) == 64);
static_assert(alignof(SpatialFilterGpuContract) == 16);
static_assert(offsetof(SpatialFilterGpuContract, sourceWidth) == 0);
static_assert(offsetof(SpatialFilterGpuContract, outputOriginX) == 8);
static_assert(offsetof(SpatialFilterGpuContract, outputWidth) == 16);
static_assert(offsetof(SpatialFilterGpuContract, edgeMode) == 32);
static_assert(offsetof(SpatialFilterGpuContract, samplingOriginX) == 48);

struct SpatialFilterTilePlan {
    QRect outputRect;
    // Sampling coordinates are allowed to extend beyond the image. The edge
    // mode maps those coordinates when an explicit halo image is requested.
    QRect samplingRect;
    // In-bounds source regions that can affect this output. Mirror uses the
    // complete source conservatively when a reflected halo crosses an edge.
    QVector<QRect> dependencyRegions;
    QRect dependencyBounds;
    QSize scaledRadius;
    QPoint cropOffset;
    double scaleX = 1.0;
    double scaleY = 1.0;
    quint64 cacheFingerprint = 0;
    bool valid = false;
    QString failureReason;

    SpatialFilterGpuContract gpuContract(const SpatialFilterContract &contract,
                                         const QSize &sourceExtent) const;
};

class SpatialFilterFoundation final {
public:
    static constexpr qsizetype MaximumHaloBytes = qsizetype(512) * 1024 * 1024;
    static constexpr qsizetype MaximumReferenceWorkingBytes =
        qsizetype(768) * 1024 * 1024;

    static QString edgeModeName(SpatialEdgeMode mode);
    static QString alphaModeName(SpatialAlphaMode mode);

    static QSize scaledRadius(const QSize &documentRadius,
                              const QSize &previewExtent,
                              const QSize &documentExtent,
                              int maximumRadius = SpatialFilterContract::DefaultMaximumRadius);

    static SpatialFilterTilePlan plan(const QRect &requestedOutput,
                                      const QSize &previewExtent,
                                      const QSize &documentExtent,
                                      SpatialFilterContract contract);

    static QVector<QRect> dependencyRegions(const QRect &outputRect,
                                            const QSize &extent,
                                            const QSize &radius,
                                            SpatialEdgeMode edgeMode);

    // Inverse dependency mapping used by dirty-region invalidation. Wrap can
    // yield regions on the opposite edge; Mirror deliberately returns the full
    // extent when reflection makes the inverse mapping ambiguous.
    static QVector<QRect> affectedOutputRegions(const QRect &dirtyInput,
                                                const QSize &extent,
                                                const QSize &radius,
                                                SpatialEdgeMode edgeMode);

    static int mappedCoordinate(int coordinate,
                                int extent,
                                SpatialEdgeMode edgeMode,
                                bool *inside = nullptr);

    // Materialise a straight-RGBA halo for kernels that cannot sample directly
    // from neighbouring resident tiles. Exact 8/16-bit component values and
    // hidden RGB are retained. Transparent edges synthesize zero RGBA.
    static QImage extractHalo(const QImage &source,
                              const SpatialFilterTilePlan &plan,
                              SpatialEdgeMode edgeMode,
                              const std::atomic_bool *cancelRequested = nullptr);

    // Reusable scalar-plane fixture matching the existing 13-tap separable
    // Shadows/Highlights adaptation kernel. This is shared by the CPU reference
    // and parity tests while the established WGSL kernel remains authoritative
    // for approved 8-bit GPU compositing.
    static bool blurSparseThirteenTap(QVector<float> *plane,
                                      int width,
                                      int height,
                                      QSize radius,
                                      SpatialEdgeMode edgeMode,
                                      bool quantiseHorizontalToEightBit,
                                      const std::atomic_bool *cancelRequested = nullptr,
                                      const SpatialRowProcessor &rowProcessor = {});

    // Deterministic exact CPU reference for validating halos, edge modes,
    // Alpha contracts, cancellation and bit-depth behaviour. The public Box
    // Blur adjustment and later spatial filters share this implementation.
    static QImage boxBlurReference(const QImage &source,
                                   QSize radius,
                                   SpatialEdgeMode edgeMode,
                                   SpatialAlphaMode alphaMode,
                                   const std::atomic_bool *cancelRequested = nullptr,
                                   const SpatialRowProcessor &rowProcessor = {});

    // Deterministic three-box Gaussian approximation. The supplied radius is
    // the complete sampling support: the three pass radii always sum to it, so
    // tiled dependency planning remains exact and large radii stay O(pixels).
    static QImage gaussianBlurReference(const QImage &source,
                                        QSize supportRadius,
                                        SpatialEdgeMode edgeMode,
                                        SpatialAlphaMode alphaMode,
                                        const std::atomic_bool *cancelRequested = nullptr,
                                        const SpatialRowProcessor &rowProcessor = {});
};

} // namespace vfx
