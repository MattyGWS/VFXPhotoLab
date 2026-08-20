#include "SelectionOperations.h"

#include <QPainter>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <utility>

namespace vfx {
namespace {

using CoveragePipeline = std::function<QImage(const QImage &)>;

QImage grayscaleImage(const QImage &source)
{
    return source.format() == QImage::Format_Grayscale8
        ? source
        : source.convertToFormat(QImage::Format_Grayscale8);
}

QImage boxBlurHorizontal(const QImage &source, const int radius)
{
    const QImage input = grayscaleImage(source);
    if (input.isNull() || radius <= 0) {
        return input;
    }
    QImage output(input.size(), QImage::Format_Grayscale8);
    if (output.isNull()) {
        return {};
    }
    const int divisor = radius * 2 + 1;
    QVector<int> prefix(input.width() + 1);
    for (int y = 0; y < input.height(); ++y) {
        const uchar *src = input.constScanLine(y);
        uchar *dst = output.scanLine(y);
        prefix[0] = 0;
        for (int x = 0; x < input.width(); ++x) {
            prefix[x + 1] = prefix[x] + src[x];
        }
        for (int x = 0; x < input.width(); ++x) {
            const int left = std::max(0, x - radius);
            const int right = std::min(input.width(), x + radius + 1);
            const int sum = prefix[right] - prefix[left];
            dst[x] = static_cast<uchar>((sum + divisor / 2) / divisor);
        }
    }
    return output;
}

QImage boxBlurVertical(const QImage &source, const int radius)
{
    const QImage input = grayscaleImage(source);
    if (input.isNull() || radius <= 0) {
        return input;
    }
    QImage output(input.size(), QImage::Format_Grayscale8);
    if (output.isNull()) {
        return {};
    }
    const int divisor = radius * 2 + 1;
    QVector<int> prefix(input.height() + 1);
    for (int x = 0; x < input.width(); ++x) {
        prefix[0] = 0;
        for (int y = 0; y < input.height(); ++y) {
            prefix[y + 1] = prefix[y] + input.constScanLine(y)[x];
        }
        for (int y = 0; y < input.height(); ++y) {
            const int top = std::max(0, y - radius);
            const int bottom = std::min(input.height(), y + radius + 1);
            const int sum = prefix[bottom] - prefix[top];
            output.scanLine(y)[x] = static_cast<uchar>(
                (sum + divisor / 2) / divisor);
        }
    }
    return output;
}

int gaussianBoxRadius(const int radius)
{
    return radius <= 0
        ? 0
        : std::max(1, qRound(radius * 0.5773502691896258));
}

QImage gaussianApproximation(const QImage &source, const int radius)
{
    const int boxRadius = gaussianBoxRadius(radius);
    QImage result = grayscaleImage(source);
    if (result.isNull() || boxRadius <= 0) {
        return result;
    }
    for (int pass = 0; pass < 3; ++pass) {
        result = boxBlurHorizontal(result, boxRadius);
        result = boxBlurVertical(result, boxRadius);
        if (result.isNull()) {
            return {};
        }
    }
    return result;
}

QImage extremeHorizontal(const QImage &source,
                         const int radius,
                         const bool maximum)
{
    const QImage input = grayscaleImage(source);
    if (input.isNull() || radius <= 0) {
        return input;
    }
    QImage output(input.size(), QImage::Format_Grayscale8);
    if (output.isNull()) {
        return {};
    }
    const int window = radius * 2 + 1;
    for (int y = 0; y < input.height(); ++y) {
        const uchar *src = input.constScanLine(y);
        uchar *dst = output.scanLine(y);
        std::deque<std::pair<int, int>> queue;
        for (int paddedIndex = 0;
             paddedIndex < input.width() + radius * 2;
             ++paddedIndex) {
            const int sourceIndex = paddedIndex - radius;
            const int value = sourceIndex >= 0 && sourceIndex < input.width()
                ? src[sourceIndex]
                : 0;
            while (!queue.empty()
                   && (maximum ? queue.back().second <= value
                               : queue.back().second >= value)) {
                queue.pop_back();
            }
            queue.emplace_back(paddedIndex, value);
            const int firstValid = paddedIndex - window + 1;
            while (!queue.empty() && queue.front().first < firstValid) {
                queue.pop_front();
            }
            if (paddedIndex >= window - 1) {
                const int outputIndex = paddedIndex - window + 1;
                if (outputIndex < input.width()) {
                    dst[outputIndex] = static_cast<uchar>(queue.front().second);
                }
            }
        }
    }
    return output;
}

QImage extremeVertical(const QImage &source,
                       const int radius,
                       const bool maximum)
{
    const QImage input = grayscaleImage(source);
    if (input.isNull() || radius <= 0) {
        return input;
    }
    QImage output(input.size(), QImage::Format_Grayscale8);
    if (output.isNull()) {
        return {};
    }
    const int window = radius * 2 + 1;
    for (int x = 0; x < input.width(); ++x) {
        std::deque<std::pair<int, int>> queue;
        for (int paddedIndex = 0;
             paddedIndex < input.height() + radius * 2;
             ++paddedIndex) {
            const int sourceIndex = paddedIndex - radius;
            const int value = sourceIndex >= 0 && sourceIndex < input.height()
                ? input.constScanLine(sourceIndex)[x]
                : 0;
            while (!queue.empty()
                   && (maximum ? queue.back().second <= value
                               : queue.back().second >= value)) {
                queue.pop_back();
            }
            queue.emplace_back(paddedIndex, value);
            const int firstValid = paddedIndex - window + 1;
            while (!queue.empty() && queue.front().first < firstValid) {
                queue.pop_front();
            }
            if (paddedIndex >= window - 1) {
                const int outputIndex = paddedIndex - window + 1;
                if (outputIndex < input.height()) {
                    output.scanLine(outputIndex)[x] = static_cast<uchar>(
                        queue.front().second);
                }
            }
        }
    }
    return output;
}

QImage morphology(const QImage &source,
                  const int radius,
                  const bool maximum)
{
    if (radius <= 0) {
        return grayscaleImage(source);
    }
    return extremeVertical(extremeHorizontal(source, radius, maximum),
                           radius,
                           maximum);
}

QImage applyContrast(const QImage &source, const int contrast)
{
    const QImage input = grayscaleImage(source);
    if (input.isNull() || contrast <= 0) {
        return input;
    }
    QImage output(input.size(), QImage::Format_Grayscale8);
    if (output.isNull()) {
        return {};
    }
    const double factor = 1.0 + std::clamp(contrast, 0, 100) / 8.0;
    for (int y = 0; y < input.height(); ++y) {
        const uchar *src = input.constScanLine(y);
        uchar *dst = output.scanLine(y);
        for (int x = 0; x < input.width(); ++x) {
            dst[x] = static_cast<uchar>(std::clamp(
                qRound(127.5 + (src[x] - 127.5) * factor),
                0,
                255));
        }
    }
    return output;
}

QImage smoothCoverage(const QImage &source, const int radius)
{
    if (radius <= 0) {
        return grayscaleImage(source);
    }
    return applyContrast(gaussianApproximation(source, radius), 72);
}

int refineShiftPixels(const SelectionRefineParameters &parameters)
{
    const int edgeWidth = std::max(1,
        std::max(parameters.smoothRadius, parameters.featherRadius));
    return qRound(std::abs(std::clamp(parameters.shiftEdgePercent, -100, 100))
                  * edgeWidth / 100.0);
}

QImage applyRefinePipeline(const QImage &source,
                           const SelectionRefineParameters &parameters,
                           const int shiftPixelsOverride = -1)
{
    QImage result = grayscaleImage(source);
    if (result.isNull()) {
        return {};
    }
    if (parameters.smoothRadius > 0) {
        result = smoothCoverage(result, parameters.smoothRadius);
    }
    const int shiftPixels = shiftPixelsOverride >= 0
        ? shiftPixelsOverride
        : refineShiftPixels(parameters);
    if (shiftPixels > 0) {
        result = morphology(result,
                            shiftPixels,
                            parameters.shiftEdgePercent > 0);
    }
    if (parameters.featherRadius > 0) {
        result = gaussianApproximation(result, parameters.featherRadius);
    }
    if (parameters.contrast > 0) {
        result = applyContrast(result, parameters.contrast);
    }
    return result;
}

QRect expandedClipped(const QRect &rect, const int margin, const QSize &size)
{
    return rect.adjusted(-margin, -margin, margin, margin)
        .intersected(QRect(QPoint(0, 0), size));
}

QImage coveragePatch(const SelectionMask &selection, const QRect &desiredRect)
{
    if (desiredRect.isEmpty()) {
        return {};
    }
    QImage patch(desiredRect.size(), QImage::Format_Grayscale8);
    if (patch.isNull()) {
        return {};
    }
    patch.fill(0);
    const QRect documentBounds(QPoint(0, 0), selection.size());
    const QRect clipped = desiredRect.intersected(documentBounds);
    if (clipped.isEmpty()) {
        return patch;
    }
    const QImage source = selection.coverageImage(clipped);
    if (source.isNull()) {
        return {};
    }
    QPainter painter(&patch);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(clipped.topLeft() - desiredRect.topLeft(), source);
    return patch;
}

bool snapshotChanged(const SelectionMask::Snapshot &before,
                     const SelectionMask::Snapshot &after)
{
    return before.active != after.active
        || before.implicitCoverage != after.implicitCoverage
        || before.tiles != after.tiles;
}

bool processTiled(const SelectionMask &selection,
                  const int support,
                  const int outputExpansion,
                  const CoveragePipeline &pipeline,
                  SelectionMask::Snapshot *result)
{
    if (!result || !selection.isActive() || selection.size().isEmpty()) {
        return false;
    }
    const SelectionMask::Snapshot before = selection.snapshot();
    if (selection.isEmpty()) {
        *result = before;
        return false;
    }

    const QRect documentBounds(QPoint(0, 0), selection.size());
    const QRect outputRegion = selection.implicitCoverage() > 0
        ? documentBounds
        : expandedClipped(selection.nonZeroBounds(), outputExpansion, selection.size());
    if (outputRegion.isEmpty()) {
        *result = before;
        return false;
    }

    SelectionMask::Snapshot after;
    after.size = selection.size();
    after.active = true;
    // Preserve the current sparse polarity when the operation necessarily
    // covers the full document. Select All and mostly-selected masks remain
    // implicit-white instead of expanding into one explicit tile per block.
    after.implicitCoverage = outputRegion == documentBounds
        && selection.implicitCoverage() > 0 ? 255 : 0;
    after.revision = before.revision;

    const int firstTileX = outputRegion.left() / SelectionMask::TileSize;
    const int lastTileX = outputRegion.right() / SelectionMask::TileSize;
    const int firstTileY = outputRegion.top() / SelectionMask::TileSize;
    const int lastTileY = outputRegion.bottom() / SelectionMask::TileSize;
    bool allDocumentPixelsSelected = outputRegion == documentBounds;

    for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
        for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
            const QPoint tileIndex(tileX, tileY);
            const QSize tileSize = selection.tilePixelSize(tileIndex);
            if (tileSize.isEmpty()) {
                continue;
            }
            const QRect tileRect(tileX * SelectionMask::TileSize,
                                 tileY * SelectionMask::TileSize,
                                 tileSize.width(),
                                 tileSize.height());
            const QRect desiredPatch = tileRect.adjusted(-support,
                                                         -support,
                                                         support,
                                                         support);
            const QImage sourcePatch = coveragePatch(selection, desiredPatch);
            const QImage processedPatch = pipeline(sourcePatch);
            if (processedPatch.isNull() || processedPatch.size() != sourcePatch.size()) {
                return false;
            }

            QImage outputTile(tileSize, QImage::Format_Grayscale8);
            if (outputTile.isNull()) {
                return false;
            }
            outputTile.fill(0);
            const QRect writeRect = tileRect.intersected(outputRegion);
            bool all255 = writeRect == tileRect;
            for (int y = writeRect.top(); y <= writeRect.bottom(); ++y) {
                const uchar *src = processedPatch.constScanLine(
                    y - desiredPatch.top());
                uchar *dst = outputTile.scanLine(y - tileRect.top());
                for (int x = writeRect.left(); x <= writeRect.right(); ++x) {
                    const uchar value = src[x - desiredPatch.left()];
                    dst[x - tileRect.left()] = value;
                    all255 = all255 && value == 255;
                }
            }
            allDocumentPixelsSelected = allDocumentPixelsSelected && all255;
            QByteArray bytes(tileSize.width() * tileSize.height(), '\0');
            bool uniformImplicit = true;
            for (int y = 0; y < tileSize.height(); ++y) {
                const char *sourceLine = reinterpret_cast<const char *>(
                    outputTile.constScanLine(y));
                std::copy_n(sourceLine,
                            tileSize.width(),
                            bytes.data() + y * tileSize.width());
                for (int x = 0; x < tileSize.width(); ++x) {
                    uniformImplicit = uniformImplicit
                        && static_cast<quint8>(sourceLine[x])
                            == after.implicitCoverage;
                }
            }
            if (!uniformImplicit) {
                after.tiles.insert(SelectionMask::tileKey(tileIndex),
                                   std::move(bytes));
            }
        }
    }

    if (allDocumentPixelsSelected) {
        after.implicitCoverage = 255;
        after.tiles.clear();
        after.nonZeroBounds = documentBounds;
    }
    SelectionMask candidate(selection.size());
    if (!candidate.restoreSnapshot(after, false)) {
        return false;
    }
    after = candidate.snapshot();
    after.revision = before.revision;
    *result = after;
    return snapshotChanged(before, after);
}

SelectionRefineParameters scaledParameters(const SelectionRefineParameters &parameters,
                                           const double pixelScale)
{
    SelectionRefineParameters scaled = parameters;
    const double scale = std::max(0.0, pixelScale);
    scaled.smoothRadius = qRound(parameters.smoothRadius * scale);
    scaled.featherRadius = qRound(parameters.featherRadius * scale);
    // Preview shift distance is supplied explicitly after scaling the
    // full-resolution result; keep the sign here to retain inward/outward
    // morphology direction.
    return scaled;
}

} // namespace

bool SelectionOperations::feather(const SelectionMask &selection,
                                  const int radius,
                                  SelectionMask::Snapshot *result)
{
    const int safeRadius = std::clamp(radius, 0, 2048);
    if (!result) {
        return false;
    }
    if (safeRadius == 0) {
        *result = selection.snapshot();
        return false;
    }
    const int support = gaussianBoxRadius(safeRadius) * 3;
    return processTiled(selection,
                        support,
                        support,
                        [safeRadius](const QImage &source) {
                            return gaussianApproximation(source, safeRadius);
                        },
                        result);
}

bool SelectionOperations::expand(const SelectionMask &selection,
                                 const int radius,
                                 SelectionMask::Snapshot *result)
{
    const int safeRadius = std::clamp(radius, 0, 2048);
    if (!result) {
        return false;
    }
    if (safeRadius == 0) {
        *result = selection.snapshot();
        return false;
    }
    return processTiled(selection,
                        safeRadius,
                        safeRadius,
                        [safeRadius](const QImage &source) {
                            return morphology(source, safeRadius, true);
                        },
                        result);
}

bool SelectionOperations::contract(const SelectionMask &selection,
                                   const int radius,
                                   SelectionMask::Snapshot *result)
{
    const int safeRadius = std::clamp(radius, 0, 2048);
    if (!result) {
        return false;
    }
    if (safeRadius == 0) {
        *result = selection.snapshot();
        return false;
    }
    return processTiled(selection,
                        safeRadius,
                        0,
                        [safeRadius](const QImage &source) {
                            return morphology(source, safeRadius, false);
                        },
                        result);
}

bool SelectionOperations::smooth(const SelectionMask &selection,
                                 const int radius,
                                 SelectionMask::Snapshot *result)
{
    const int safeRadius = std::clamp(radius, 0, 2048);
    if (!result) {
        return false;
    }
    if (safeRadius == 0) {
        *result = selection.snapshot();
        return false;
    }
    const int support = gaussianBoxRadius(safeRadius) * 3;
    return processTiled(selection,
                        support,
                        support,
                        [safeRadius](const QImage &source) {
                            return smoothCoverage(source, safeRadius);
                        },
                        result);
}

bool SelectionOperations::refine(const SelectionMask &selection,
                                 const SelectionRefineParameters &parameters,
                                 SelectionMask::Snapshot *result)
{
    if (!result) {
        return false;
    }
    const SelectionMask::Snapshot before = selection.snapshot();
    *result = before;
    if (!selection.isActive() || selection.isEmpty()) {
        return false;
    }

    SelectionRefineParameters safe = parameters;
    safe.smoothRadius = std::clamp(safe.smoothRadius, 0, 2048);
    safe.featherRadius = std::clamp(safe.featherRadius, 0, 2048);
    safe.contrast = std::clamp(safe.contrast, 0, 100);
    safe.shiftEdgePercent = std::clamp(safe.shiftEdgePercent, -100, 100);
    if (safe.smoothRadius == 0 && safe.featherRadius == 0
        && safe.contrast == 0 && safe.shiftEdgePercent == 0) {
        return false;
    }

    // Evaluate refinement as a series of complete sparse selection stages.
    // This keeps the largest per-tile halo bounded by one operation instead
    // of summing Smooth + Shift + Feather into a potentially huge patch.
    SelectionMask working(selection.size());
    if (!working.restoreSnapshot(before, false)) {
        return false;
    }
    auto restoreStage = [&working](const SelectionMask::Snapshot &stage) {
        return !stage.size.isEmpty() && working.restoreSnapshot(stage, false);
    };

    SelectionMask::Snapshot stage;
    if (safe.smoothRadius > 0) {
        smooth(working, safe.smoothRadius, &stage);
        if (!restoreStage(stage)) {
            return false;
        }
    }

    const int shift = refineShiftPixels(safe);
    if (shift > 0) {
        stage = {};
        if (safe.shiftEdgePercent > 0) {
            expand(working, shift, &stage);
        } else {
            contract(working, shift, &stage);
        }
        if (!restoreStage(stage)) {
            return false;
        }
    }

    if (safe.featherRadius > 0) {
        stage = {};
        feather(working, safe.featherRadius, &stage);
        if (!restoreStage(stage)) {
            return false;
        }
    }

    if (safe.contrast > 0) {
        stage = {};
        processTiled(working,
                     0,
                     0,
                     [safe](const QImage &source) {
                         return applyContrast(source, safe.contrast);
                     },
                     &stage);
        if (!restoreStage(stage)) {
            return false;
        }
    }

    SelectionMask::Snapshot after = working.snapshot();
    after.revision = before.revision;
    *result = after;
    return snapshotChanged(before, after);
}

QImage SelectionOperations::refineCoverageImage(
    const QImage &coverage,
    const SelectionRefineParameters &parameters,
    const double pixelScale)
{
    const double scale = std::max(0.0, pixelScale);
    const int previewShift = qRound(refineShiftPixels(parameters) * scale);
    return applyRefinePipeline(coverage,
                               scaledParameters(parameters, scale),
                               previewShift);
}

} // namespace vfx
