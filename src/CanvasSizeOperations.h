#pragma once

#include "PhotoDocument.h"

#include <QColor>
#include <QRect>

#include <atomic>

namespace vfx {

enum class CanvasAnchor {
    TopLeft,
    Top,
    TopRight,
    Left,
    Centre,
    Right,
    BottomLeft,
    Bottom,
    BottomRight
};

enum class CanvasFillMode {
    Transparent,
    Foreground,
    Background,
    Custom
};

enum class CanvasFitMode {
    RevealAll,
    Selection,
    SelectedLayers
};

struct CanvasSizeRequest {
    QSize pixelSize;
    CanvasAnchor anchor = CanvasAnchor::Centre;
    CanvasFillMode fillMode = CanvasFillMode::Transparent;
    QColor fillColour = QColor(Qt::transparent);
    bool deleteOutsideCanvas = false;
};

struct CanvasSizeResult {
    QImage canvasImage;
    QVector<LayerNode> layers;
    SelectionMask::Snapshot selection;
    QVector<double> horizontalGuides;
    QVector<double> verticalGuides;
    QRect previousCanvasRect;
    bool extensionLayerCreated = false;
    bool destructiveClippingApplied = false;
};

struct CanvasFitRequest {
    CanvasFitMode mode = CanvasFitMode::RevealAll;
    QVector<QUuid> layerIds;
};

struct CanvasFitResult {
    CanvasSizeResult canvas;
    QRect documentRect;
    bool noChange = false;
    QString noChangeMessage;
};

// Returns the new canvas rectangle expressed in the old document coordinate
// system. The selected anchor remains stationary. For centred odd differences,
// the unmatched pixel is assigned to the right or bottom side.
QRect canvasSizeDocumentRect(const QSize &oldSize,
                             const QSize &newSize,
                             CanvasAnchor anchor);

// Prepare a document-bounds change for an exact rectangle expressed in the
// current document coordinate system. The operation never resamples layers.
// This is the common transaction used by Canvas Size, Reveal All and Fit.
bool buildCanvasBoundsResult(const PhotoDocument &document,
                             const QRect &newCanvasInOldCoordinates,
                             CanvasFillMode fillMode,
                             const QColor &fillColour,
                             bool deleteOutsideCanvas,
                             CanvasSizeResult *result,
                             const std::atomic_bool *cancelRequested = nullptr,
                             QString *errorMessage = nullptr);

bool buildCanvasSizeResult(const PhotoDocument &document,
                           const CanvasSizeRequest &request,
                           CanvasSizeResult *result,
                           const std::atomic_bool *cancelRequested = nullptr,
                           QString *errorMessage = nullptr);

// Resolve and prepare Reveal All, Fit Canvas to Selection, or Fit Canvas to
// Selected Layers. Reveal All includes hidden straight-RGBA storage (including
// hidden RGB beneath zero alpha) but never shrinks. Selected-layer fitting uses
// finite transformed layer/mask bounds and intentionally includes hidden layers
// that the user selected explicitly.
bool buildCanvasFitResult(const PhotoDocument &document,
                          const CanvasFitRequest &request,
                          CanvasFitResult *result,
                          const std::atomic_bool *cancelRequested = nullptr,
                          QString *errorMessage = nullptr);

} // namespace vfx
