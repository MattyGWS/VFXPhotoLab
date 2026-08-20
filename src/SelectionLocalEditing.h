#pragma once

#include "SelectionMask.h"

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QTransform>

namespace vfx {

enum class SelectionEditKind {
    RasterPixels,
    Mask,
    GreyChannel,
    ComponentChannel
};

// Sample document-space selection coverage at the centre of one layer-local
// pixel. Affine transforms use bilinear interpolation so feathered edges stay
// stable while translated, scaled or rotated layer targets are edited.
double sampleSelectionCoverage(const SelectionMask &selection,
                               const QTransform &layerToDocument,
                               const QPointF &layerPixelCentre);

// Apply a snapshotted selection to the ordinary result of a Brush/Eraser or
// direct-channel operation. Only requestedRect is touched. Raster pixels are
// mixed in premultiplied arithmetic and returned as straight RGBA, retaining
// source RGB whenever the selected result reaches zero alpha.
bool clipEditedImageToSelection(QImage *editedImage,
                                const QImage &sourceImage,
                                const QRect &requestedRect,
                                const SelectionMask::Snapshot &selectionSnapshot,
                                const QTransform &layerToDocument,
                                SelectionEditKind kind,
                                int channelIndex = -1);

// Clear selected coverage on one editable target. RasterPixels reduces only
// alpha, GreyChannel reduces RGB, ComponentChannel reduces one component, and
// Mask reduces stored one-byte coverage. Returns false for a valid no-op.
bool clearImageThroughSelection(QImage *resultImage,
                                const QImage &sourceImage,
                                const SelectionMask::Snapshot &selectionSnapshot,
                                const QSize &targetReferenceSize,
                                const QTransform &layerToDocument,
                                SelectionEditKind kind,
                                int channelIndex,
                                QRect *affectedRect = nullptr);

// Convert document-space selection coverage into a normal layer-local mask.
// Full/empty and uniform results remain compact 1x1 masks.
QImage selectionAsLayerMask(const SelectionMask &selection,
                            const QTransform &layerToDocument,
                            const QSize &documentSize);

} // namespace vfx
