#pragma once
#include "Adjustment.h"
#include <QColorSpace>
#include <QImage>
#include <QRect>
#include <QTransform>
#include <atomic>
namespace vfx {
class TextRasterizer {
public:
    static QImage renderLayerRegion(const LayerNode &layer, const QSize &sourceSize,
        const QRect &sourceRegion, const QSize &documentSize, const QTransform &worldTransform,
        QImage::Format format, const QColorSpace &colourSpace, bool forceOpaquePixelAlpha,
        bool grayscaleDocument, const std::atomic_bool *cancelRequested = nullptr);
    static QRectF contentBounds(const LayerNode &layer, const QTransform &worldTransform);
};
}
