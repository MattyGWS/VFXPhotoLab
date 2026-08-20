#pragma once

#include "SelectionMask.h"

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QTransform>

#include <atomic>
#include <memory>

namespace vfx {

struct PatchToolRequest {
    QImage destination;
    QImage source;
    SelectionMask::Snapshot selection;

    // Destination coverage is the document selection translated by this
    // amount. Source sampling is independently offset from each destination
    // document point. Source mode uses destinationOffset=0 and sourceOffset=drag;
    // Destination mode uses destinationOffset=drag and sourceOffset=-drag.
    QPointF destinationOffsetDocument;
    QPointF sourceOffsetDocument;

    QTransform targetPixelToLayer;
    QTransform targetLayerToDocument;
    QTransform sourceDocumentToLayer;
    QTransform sourceLayerToPixel;

    double opacity = 1.0;
    std::shared_ptr<std::atomic_bool> cancelRequested;
};

struct PatchToolResult {
    QImage image;
    QRect affectedRect;
    bool cancelled = false;
    QString error;
};

// Selection-shaped Poisson seamless cloning. The selected source structure is
// copied through a harmonic destination-minus-source correction field, using
// the selection's own feathered coverage as final blend weight. Destination
// Alpha is preserved exactly while hidden straight RGB remains editable.
PatchToolResult applyPatchTool(const PatchToolRequest &request);

} // namespace vfx
