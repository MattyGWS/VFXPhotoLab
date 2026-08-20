#include "TrimOperations.h"

#include "ImageProcessor.h"

#include <QRgba64>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vfx {
namespace {

constexpr int MaximumAnalysisStripHeight = 256;
constexpr qint64 TargetAnalysisSurfaceBytes = 16ll * 1024ll * 1024ll;

int analysisStripHeight(const QSize &documentSize,
                        const bool highPrecision)
{
    const qint64 bytesPerPixel = highPrecision ? 8 : 4;
    const qint64 rowBytes = std::max<qint64>(
        1, static_cast<qint64>(documentSize.width()) * bytesPerPixel);
    return static_cast<int>(std::clamp<qint64>(
        TargetAnalysisSurfaceBytes / rowBytes,
        1,
        MaximumAnalysisStripHeight));
}

bool cancelled(const std::atomic_bool *cancelRequested)
{
    return cancelRequested
        && cancelRequested->load(std::memory_order_acquire);
}

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

QPoint cornerPoint(const QSize &size, const TrimSampleCorner corner)
{
    switch (corner) {
    case TrimSampleCorner::TopLeft:
        return QPoint(0, 0);
    case TrimSampleCorner::TopRight:
        return QPoint(std::max(0, size.width() - 1), 0);
    case TrimSampleCorner::BottomLeft:
        return QPoint(0, std::max(0, size.height() - 1));
    case TrimSampleCorner::BottomRight:
        return QPoint(std::max(0, size.width() - 1),
                      std::max(0, size.height() - 1));
    }
    return QPoint(0, 0);
}

struct StraightSample {
    quint16 red = 0;
    quint16 green = 0;
    quint16 blue = 0;
    quint16 alpha = 0;
};

StraightSample sampleAt(const QImage &image, const int x, const int y)
{
    if (image.format() == QImage::Format_RGBA64) {
        const auto *row = reinterpret_cast<const QRgba64 *>(
            image.constScanLine(y));
        return {row[x].red(), row[x].green(), row[x].blue(), row[x].alpha()};
    }
    const uchar *pixel = image.constScanLine(y) + x * 4;
    return {static_cast<quint16>(pixel[0] * 257u),
            static_cast<quint16>(pixel[1] * 257u),
            static_cast<quint16>(pixel[2] * 257u),
            static_cast<quint16>(pixel[3] * 257u)};
}


quint16 compositeAlphaAt(const QImage &image, const int x, const int y)
{
    if (image.depth() > 32) {
        const auto *row = reinterpret_cast<const QRgba64 *>(
            image.constScanLine(y));
        return row[x].alpha();
    }
    const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
    return static_cast<quint16>(qAlpha(row[x]) * 257u);
}

QColor sampleColour(const StraightSample &sample)
{
    return QColor::fromRgba64(sample.red,
                              sample.green,
                              sample.blue,
                              sample.alpha);
}

bool channelWithinTolerance(const quint16 left,
                            const quint16 right,
                            const quint16 tolerance)
{
    const int difference = std::abs(static_cast<int>(left)
                                    - static_cast<int>(right));
    return difference <= static_cast<int>(tolerance);
}

bool cornerSamplesMatch(const StraightSample &sample,
                        const StraightSample &reference,
                        const quint16 tolerance)
{
    // Hidden RGB is deliberately irrelevant when both visible samples are
    // exactly transparent.
    if (sample.alpha == 0 && reference.alpha == 0) {
        return true;
    }
    return channelWithinTolerance(sample.red, reference.red, tolerance)
        && channelWithinTolerance(sample.green, reference.green, tolerance)
        && channelWithinTolerance(sample.blue, reference.blue, tolerance)
        && channelWithinTolerance(sample.alpha, reference.alpha, tolerance);
}

bool scanCompositeBounds(const PhotoDocument &document,
                         const AutomaticTrimRequest &request,
                         QRect *contentBounds,
                         QColor *sampledColour,
                         bool *sampledColourValid,
                         const std::atomic_bool *cancelRequested,
                         QString *errorMessage)
{
    if (!contentBounds || !sampledColour || !sampledColourValid) {
        setError(errorMessage, QStringLiteral("The trim analysis target is invalid."));
        return false;
    }

    const QImage source = document.sourceImage();
    const QSize documentSize = source.size();
    if (source.isNull() || documentSize.isEmpty()) {
        setError(errorMessage, QStringLiteral("There is no visible document composite to trim."));
        return false;
    }

    StraightSample reference;
    if (request.mode == AutomaticTrimMode::CornerColour) {
        const QPoint point = cornerPoint(documentSize, request.sampleCorner);
        const QImage corner = ImageProcessor::renderRegionPreservingHiddenRgb(
            source,
            document.layers(),
            QRect(point, QSize(1, 1)),
            documentSize,
            cancelRequested,
            document.colourState().processingCompatibility);
        if (cancelled(cancelRequested)) {
            setError(errorMessage, QStringLiteral("Trim analysis cancelled."));
            return false;
        }
        if (corner.isNull() || corner.size() != QSize(1, 1)) {
            setError(errorMessage, QStringLiteral("The selected corner could not be sampled."));
            return false;
        }
        reference = sampleAt(corner, 0, 0);
        *sampledColour = sampleColour(reference);
        *sampledColourValid = true;
    }

    int left = documentSize.width();
    int top = documentSize.height();
    int right = -1;
    int bottom = -1;
    const quint16 tolerance = static_cast<quint16>(
        std::clamp(request.tolerance, 0, 255) * 257u);

    const int stripStep = analysisStripHeight(
        documentSize,
        request.mode == AutomaticTrimMode::CornerColour
            || source.depth() > 32);
    for (int stripTop = 0; stripTop < documentSize.height();
         stripTop += stripStep) {
        if (cancelled(cancelRequested)) {
            setError(errorMessage, QStringLiteral("Trim analysis cancelled."));
            return false;
        }
        const int stripHeight = std::min(stripStep,
                                         documentSize.height() - stripTop);
        const QRect stripRect(0,
                              stripTop,
                              documentSize.width(),
                              stripHeight);
        const QImage strip = request.mode
                == AutomaticTrimMode::TransparentPixels
            ? ImageProcessor::renderRegion(source,
                                           document.layers(),
                                           stripRect,
                                           documentSize,
                                           cancelRequested,
                                           document.colourState().processingCompatibility)
            : ImageProcessor::renderRegionPreservingHiddenRgb(
                  source,
                  document.layers(),
                  stripRect,
                  documentSize,
                  cancelRequested,
                  document.colourState().processingCompatibility);
        if (cancelled(cancelRequested)) {
            setError(errorMessage, QStringLiteral("Trim analysis cancelled."));
            return false;
        }
        if (strip.isNull() || strip.size() != stripRect.size()) {
            setError(errorMessage,
                     QStringLiteral("The visible composite could not be analysed safely."));
            return false;
        }

        for (int localY = 0; localY < strip.height(); ++localY) {
            if ((localY & 31) == 0 && cancelled(cancelRequested)) {
                setError(errorMessage, QStringLiteral("Trim analysis cancelled."));
                return false;
            }
            const int documentY = stripTop + localY;
            for (int x = 0; x < strip.width(); ++x) {
                const bool content = request.mode
                        == AutomaticTrimMode::TransparentPixels
                    ? compositeAlphaAt(strip, x, localY) != 0
                    : !cornerSamplesMatch(sampleAt(strip, x, localY),
                                          reference,
                                          tolerance);
                if (!content) {
                    continue;
                }
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, documentY);
                bottom = std::max(bottom, documentY);
            }
        }
    }

    *contentBounds = right >= left && bottom >= top
        ? QRect(QPoint(left, top), QPoint(right, bottom))
        : QRect();
    return true;
}

} // namespace

bool buildAutomaticTrimResult(const PhotoDocument &document,
                              const AutomaticTrimRequest &request,
                              AutomaticTrimResult *result,
                              const std::atomic_bool *cancelRequested,
                              QString *errorMessage)
{
    if (!result || !document.hasImage()) {
        setError(errorMessage, QStringLiteral("There is no document canvas to trim."));
        return false;
    }
    *result = {};
    if (cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Trim analysis cancelled."));
        return false;
    }
    if (request.mode == AutomaticTrimMode::CornerColour
        && !request.trimTop && !request.trimBottom
        && !request.trimLeft && !request.trimRight) {
        result->noChange = true;
        result->noChangeMessage = QStringLiteral(
            "Select at least one canvas side to trim.");
        return true;
    }

    QRect contentBounds;
    if (!scanCompositeBounds(document,
                             request,
                             &contentBounds,
                             &result->sampledColour,
                             &result->sampledColourValid,
                             cancelRequested,
                             errorMessage)) {
        return false;
    }
    if (cancelled(cancelRequested)) {
        setError(errorMessage, QStringLiteral("Trim analysis cancelled."));
        return false;
    }

    const QRect currentCanvas(QPoint(), document.sourceImage().size());
    if (contentBounds.isEmpty()) {
        result->noChange = true;
        result->noChangeMessage = request.mode
                == AutomaticTrimMode::TransparentPixels
            ? QStringLiteral("The visible composite is entirely transparent; the canvas was left unchanged.")
            : QStringLiteral("The visible composite entirely matches the sampled corner colour; the canvas was left unchanged.");
        return true;
    }

    QRect target = currentCanvas;
    if (request.mode == AutomaticTrimMode::TransparentPixels) {
        target = contentBounds;
    } else {
        if (request.trimLeft) {
            target.setLeft(contentBounds.left());
        }
        if (request.trimRight) {
            target.setRight(contentBounds.right());
        }
        if (request.trimTop) {
            target.setTop(contentBounds.top());
        }
        if (request.trimBottom) {
            target.setBottom(contentBounds.bottom());
        }
    }

    result->documentRect = target;
    if (target == currentCanvas) {
        result->noChange = true;
        result->noChangeMessage = request.mode
                == AutomaticTrimMode::TransparentPixels
            ? QStringLiteral("No transparent border pixels can be trimmed.")
            : QStringLiteral("No selected border sides match the sampled corner colour.");
        return true;
    }
    if (target.isEmpty() || target.width() < 1 || target.height() < 1) {
        result->noChange = true;
        result->noChangeMessage = QStringLiteral(
            "The trim result would be empty; the canvas was left unchanged.");
        return true;
    }

    if (!buildCanvasBoundsResult(document,
                                 target,
                                 CanvasFillMode::Transparent,
                                 QColor(Qt::transparent),
                                 request.deleteOutsideCanvas,
                                 &result->canvas,
                                 cancelRequested,
                                 errorMessage)) {
        return false;
    }
    return true;
}

} // namespace vfx
