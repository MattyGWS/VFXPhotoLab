#pragma once

#include "SelectionLocalEditing.h"
#include "SelectionMask.h"

#include <QImage>
#include <QRectF>
#include <QTransform>

namespace vfx {

// Build a target-local image containing only the active selection's coverage.
// Raster RGB remains straight and is retained beneath zero alpha. Masks retain
// their stored greyscale values while coverage outside the selection is black.
QImage selectedOnlyImageThroughSelection(
    const QImage &sourceImage,
    const QSize &targetExtent,
    const SelectionMask::Snapshot &selectionSnapshot,
    const QTransform &targetToDocument,
    SelectionEditKind kind,
    int channelIndex = -1);

// Transform one document selection through an arbitrary document-space affine
// matrix. The output remains an active 8-bit sparse selection, including when
// the transformed result falls entirely outside the document.
SelectionMask::Snapshot transformedSelectionSnapshot(
    const SelectionMask::Snapshot &sourceSnapshot,
    const QTransform &documentTransform,
    const QSize &documentSize);

bool transformIsEffectivelyIdentity(const QTransform &transform,
                                    double epsilon = 1.0e-7);

} // namespace vfx
