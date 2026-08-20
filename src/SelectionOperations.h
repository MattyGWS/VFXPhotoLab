#pragma once

#include "SelectionMask.h"

#include <QImage>

namespace vfx {

struct SelectionRefineParameters {
    int smoothRadius = 0;
    int featherRadius = 0;
    int contrast = 0;
    int shiftEdgePercent = 0;
};

// Local selection edge processing. Full-resolution work is evaluated in
// independent 256x256 output tiles with only a bounded halo, so large
// documents never require an image-sized temporary allocation.
class SelectionOperations final {
public:
    static bool feather(const SelectionMask &selection,
                        int radius,
                        SelectionMask::Snapshot *result);
    static bool expand(const SelectionMask &selection,
                       int radius,
                       SelectionMask::Snapshot *result);
    static bool contract(const SelectionMask &selection,
                         int radius,
                         SelectionMask::Snapshot *result);
    static bool smooth(const SelectionMask &selection,
                       int radius,
                       SelectionMask::Snapshot *result);
    static bool refine(const SelectionMask &selection,
                       const SelectionRefineParameters &parameters,
                       SelectionMask::Snapshot *result);

    // Preview helper. pixelScale converts document-pixel radii to the supplied
    // preview image. The returned coverage remains 8-bit and does not mutate
    // the document selection.
    static QImage refineCoverageImage(const QImage &coverage,
                                      const SelectionRefineParameters &parameters,
                                      double pixelScale = 1.0);
};

} // namespace vfx
