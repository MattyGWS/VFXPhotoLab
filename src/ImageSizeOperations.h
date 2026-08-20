#pragma once

#include "PhotoDocument.h"

#include <QtGlobal>

#include <atomic>
#include <functional>

namespace vfx {

enum class ImageResampleMethod : quint8 {
    NearestNeighbour,
    Bilinear,
    Bicubic,
    Lanczos3,
    Area
};

using ImageResampleAccelerator = std::function<QImage(
    const QImage &source,
    const QSize &destinationSize,
    ImageResampleMethod method,
    const std::atomic_bool *cancelRequested,
    QString *errorMessage)>;

struct ImageSizeRequest {
    QSize pixelSize;
    ImageResampleMethod method = ImageResampleMethod::Bicubic;
    bool resamplePixels = true;
    // Values <= 0 preserve the document's existing metadata.
    double resolutionX = 0.0;
    double resolutionY = 0.0;

    // Optional all-payload output budget used by the integration preflight.
    // Zero leaves the operation bounded only by the per-image persistent
    // snapshot limit. The UI supplies a budget derived from the shared
    // document-residency target and the active document's current footprint.
    quint64 maximumPreparedBytes = 0;

    // Optional native accelerator. A null result falls back to the exact CPU
    // reference unless cancellation was requested. The accelerator currently
    // accepts Nearest Neighbour and Bilinear RGBA8/Grayscale8 payloads only.
    // It must never mutate the source and must preserve straight components.
    ImageResampleAccelerator accelerator;
};

struct ImageSizeResult {
    QImage canvasImage;
    QVector<LayerNode> layers;
    SelectionMask::Snapshot selection;
    QVector<double> horizontalGuides;
    QVector<double> verticalGuides;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double resolutionX = 72.0;
    double resolutionY = 72.0;
    bool pixelsResampled = true;
    int gpuPayloads = 0;
    int cpuPayloads = 0;
    quint64 estimatedPreparedBytes = 0;
    QString firstGpuFallbackReason;
};

// Exact CPU reference functions used both by the production fallback and native
// GPU parity tests. Straight RGBA channels are filtered independently so hidden
// RGB beneath zero Alpha remains data.
QImage resampleStraightRgbaCpuReference(
    const QImage &source,
    const QSize &destinationSize,
    ImageResampleMethod method,
    const std::atomic_bool *cancelRequested = nullptr);

QImage resampleGrayscaleCpuReference(
    const QImage &source,
    const QSize &destinationSize,
    ImageResampleMethod method,
    const std::atomic_bool *cancelRequested = nullptr);

// Resample the complete editable document while preserving the layer tree, or
// perform a metadata-only resolution change when resamplePixels is false.
// Raster pixels, masks and selection coverage are filtered independently in
// straight component space, so RGB beneath zero Alpha is never discarded.
bool buildImageSizeResult(const PhotoDocument &document,
                          const ImageSizeRequest &request,
                          ImageSizeResult *result,
                          const std::atomic_bool *cancelRequested = nullptr,
                          QString *errorMessage = nullptr);

} // namespace vfx
