#pragma once

#include "Adjustment.h"

#include <QColorSpace>
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QTransform>
#include <QString>

#include <atomic>
#include <cmath>

namespace vfx {


struct VectorFeatherGpuTileData {
    QImage coverage;
    QImage colourCarrier;
    QRect sourceRect;
    QRect outputRect;
    double radiusX = 0.0;
    double radiusY = 0.0;

    bool isValid() const
    {
        return !coverage.isNull()
            && !colourCarrier.isNull()
            && coverage.format() == QImage::Format_RGBA8888
            && colourCarrier.format() == QImage::Format_RGBA8888
            && coverage.size() == sourceRect.size()
            && colourCarrier.size() == outputRect.size()
            && std::isfinite(radiusX)
            && std::isfinite(radiusY)
            && radiusX > 0.0
            && radiusY > 0.0;
    }
};

class VectorRasterizer final {
public:
    static QImage renderLayerRegion(const LayerNode &layer,
                                    const QSize &previewSize,
                                    const QRect &previewRegion,
                                    const QSize &documentSize,
                                    const QTransform &worldTransform,
                                    QImage::Format format,
                                    const QColorSpace &colourSpace,
                                    bool forceOpaquePixelAlpha = false,
                                    bool grayscaleDocument = false,
                                    const std::atomic_bool *cancelRequested = nullptr);

    static QRectF contentBounds(const LayerNode &layer,
                                const QTransform &worldTransform = QTransform());

    // Prepares the semantic coverage and exact authored-colour carrier consumed
    // by the native tiled GPU feather kernel. Geometry remains editable and is
    // still rasterised through the accepted QPainter path; only the coverage
    // convolution is delegated to WGSL.
    static bool prepareGpuFeatherTile(const LayerNode &layer,
                                      const QSize &previewSize,
                                      const QRect &previewRegion,
                                      const QSize &documentSize,
                                      const QTransform &worldTransform,
                                      const QColorSpace &colourSpace,
                                      bool forceOpaquePixelAlpha,
                                      bool grayscaleDocument,
                                      VectorFeatherGpuTileData *prepared,
                                      QString *errorMessage = nullptr,
                                      const std::atomic_bool *cancelRequested = nullptr);

    static void clearCache();
    static qsizetype cacheBytes();
    static qsizetype cacheEntryCount();
};

} // namespace vfx
