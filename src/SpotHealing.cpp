#include "SpotHealing.h"

#include "HealingBrush.h"

#include <QRgba64>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace vfx {
namespace {

constexpr double Pi = 3.141592653589793238462643383279502884;

struct Sample {
    std::array<double, 4> rgba {0.0, 0.0, 0.0, 0.0};
    bool valid = false;
};

Sample sampleImage(const QImage &image, const QPointF &position, const bool sixteenBit)
{
    Sample sample;
    if (image.isNull() || image.width() <= 0 || image.height() <= 0
        || !std::isfinite(position.x()) || !std::isfinite(position.y())
        || position.x() < 0.0 || position.y() < 0.0
        || position.x() > image.width() - 1.0
        || position.y() > image.height() - 1.0) {
        return sample;
    }

    const int x0 = std::clamp(static_cast<int>(std::floor(position.x())),
                              0, image.width() - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(position.y())),
                              0, image.height() - 1);
    const int x1 = std::min(x0 + 1, image.width() - 1);
    const int y1 = std::min(y0 + 1, image.height() - 1);
    const double tx = std::clamp(position.x() - x0, 0.0, 1.0);
    const double ty = std::clamp(position.y() - y0, 0.0, 1.0);

    const auto read = [&](const int x, const int y) {
        std::array<double, 4> value {};
        if (sixteenBit) {
            const auto *row = reinterpret_cast<const QRgba64 *>(image.constScanLine(y));
            const QRgba64 pixel = row[x];
            value = {pixel.red() / 65535.0,
                     pixel.green() / 65535.0,
                     pixel.blue() / 65535.0,
                     pixel.alpha() / 65535.0};
        } else {
            const uchar *pixel = image.constScanLine(y) + x * 4;
            value = {pixel[0] / 255.0,
                     pixel[1] / 255.0,
                     pixel[2] / 255.0,
                     pixel[3] / 255.0};
        }
        return value;
    };

    const std::array<double, 4> pixels[4] {
        read(x0, y0), read(x1, y0), read(x0, y1), read(x1, y1)
    };
    const double weights[4] {
        (1.0 - tx) * (1.0 - ty),
        tx * (1.0 - ty),
        (1.0 - tx) * ty,
        tx * ty
    };

    double alpha = 0.0;
    for (int index = 0; index < 4; ++index) {
        alpha += pixels[index][3] * weights[index];
    }
    sample.rgba[3] = std::clamp(alpha, 0.0, 1.0);
    if (alpha > 1.0e-12) {
        for (int component = 0; component < 3; ++component) {
            double associated = 0.0;
            for (int index = 0; index < 4; ++index) {
                associated += pixels[index][component]
                    * pixels[index][3] * weights[index];
            }
            sample.rgba[component] = std::clamp(associated / alpha, 0.0, 1.0);
        }
    } else {
        for (int component = 0; component < 3; ++component) {
            for (int index = 0; index < 4; ++index) {
                sample.rgba[component] += pixels[index][component] * weights[index];
            }
        }
    }
    sample.valid = true;
    return sample;
}

double luminance(const std::array<double, 4> &rgba)
{
    return rgba[0] * 0.2126 + rgba[1] * 0.7152 + rgba[2] * 0.0722;
}

QVector<QPointF> buildStamps(const QVector<QLineF> &segments, const double radius)
{
    QVector<QPointF> stamps;
    const double spacing = std::max(1.0, radius * 0.32);
    for (const QLineF &segment : segments) {
        const double length = segment.length();
        if (length <= 1.0e-6) {
            stamps.push_back(segment.p2());
            continue;
        }
        const int steps = std::max(1, static_cast<int>(std::ceil(length / spacing)));
        stamps.reserve(stamps.size() + steps);
        for (int step = 1; step <= steps; ++step) {
            const double amount = step / static_cast<double>(steps);
            stamps.push_back(segment.p1() + (segment.p2() - segment.p1()) * amount);
        }
    }
    return stamps;
}

QPointF mapTargetPixelToDocument(const SpotHealingRequest &request,
                                 const QPointF &targetPixel)
{
    return request.targetLayerToDocument.map(
        request.targetPixelToLayer.map(targetPixel));
}

QPointF mapDocumentToSourcePixel(const SpotHealingRequest &request,
                                 const QPointF &documentPoint)
{
    return request.sourceLayerToPixel.map(
        request.sourceDocumentToLayer.map(documentPoint)) - QPointF(0.5, 0.5);
}

Sample sourceAtTargetPixel(const SpotHealingRequest &request,
                           const QImage &source,
                           const bool sourceSixteenBit,
                           const QPointF &targetPixel,
                           const QPointF &offsetDocument)
{
    return sampleImage(source,
                       mapDocumentToSourcePixel(
                           request,
                           mapTargetPixelToDocument(request, targetPixel)
                               + offsetDocument),
                       sourceSixteenBit);
}

Sample destinationAtTargetPixel(const QImage &destination,
                                const bool destinationSixteenBit,
                                const QPointF &targetPixel)
{
    return sampleImage(destination,
                       targetPixel - QPointF(0.5, 0.5),
                       destinationSixteenBit);
}

struct BoundaryObservation {
    QPointF targetPixel;
    Sample destination;
    double gradientX = 0.0;
    double gradientY = 0.0;
};

} // namespace

SpotHealingResult applySpotHealing(const SpotHealingRequest &request)
{
    SpotHealingResult result;
    const auto cancellationRequested = [&request] {
        return request.cancelRequested
            && request.cancelRequested->load(std::memory_order_acquire);
    };
    const auto cancelledResult = [&result]() -> SpotHealingResult {
        result.cancelled = true;
        result.image = {};
        result.affectedRect = {};
        result.error.clear();
        return result;
    };
    if (cancellationRequested()) {
        return cancelledResult();
    }
    if (request.destination.isNull()) {
        result.error = QStringLiteral("Spot Healing destination is empty.");
        return result;
    }
    if (request.source.isNull()) {
        result.error = QStringLiteral("Spot Healing source is empty.");
        return result;
    }
    if (request.targetSegments.isEmpty()) {
        result.image = request.destination;
        return result;
    }

    const bool destinationSixteenBit = request.destination.depth() > 32;
    const bool sourceSixteenBit = request.source.depth() > 32;
    const QImage destination = request.destination.convertToFormat(
        destinationSixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    const QImage source = request.source.convertToFormat(
        sourceSixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (destination.isNull() || source.isNull()) {
        result.error = QStringLiteral("Spot Healing could not prepare its images.");
        return result;
    }

    const double radius = std::max(0.5, request.diameterPixels * 0.5);
    const QVector<QPointF> stamps = buildStamps(request.targetSegments, radius);
    if (stamps.isEmpty()) {
        result.image = request.destination;
        return result;
    }

    QRectF targetBounds;
    for (const QPointF &stamp : stamps) {
        const QRectF stampBounds(stamp.x() - radius,
                                 stamp.y() - radius,
                                 radius * 2.0,
                                 radius * 2.0);
        targetBounds = targetBounds.isNull() ? stampBounds : targetBounds.united(stampBounds);
    }

    // Sample the outside of the complete gesture rather than its blemished
    // interior. This makes candidate selection insensitive to the very defect
    // that the tool is supposed to remove.
    QVector<BoundaryObservation> boundary;
    const int maximumBoundarySamples = 96;
    const int stampStride = std::max(1, static_cast<int>(stamps.size() / 24));
    constexpr int anglesPerStamp = 16;
    for (int stampIndex = 0;
         stampIndex < stamps.size() && boundary.size() < maximumBoundarySamples;
         stampIndex += stampStride) {
        if (cancellationRequested()) {
            return cancelledResult();
        }
        const QPointF centre = stamps[stampIndex];
        for (int angleIndex = 0;
             angleIndex < anglesPerStamp && boundary.size() < maximumBoundarySamples;
             ++angleIndex) {
            const double angle = angleIndex * (2.0 * Pi / anglesPerStamp);
            const QPointF point = centre + QPointF(std::cos(angle), std::sin(angle))
                * (radius + 1.5);

            bool insideAnotherStamp = false;
            // Check the complete gesture here, not only the boundary-sampling
            // stride. Otherwise a long curved stroke could accidentally score
            // pixels that still lie inside one of its omitted intermediate dabs.
            for (int otherIndex = 0; otherIndex < stamps.size(); ++otherIndex) {
                if ((otherIndex & 511) == 0 && cancellationRequested()) {
                    return cancelledResult();
                }
                if (QLineF(point, stamps[otherIndex]).length() < radius * 0.94) {
                    insideAnotherStamp = true;
                    break;
                }
            }
            if (insideAnotherStamp) {
                continue;
            }

            const Sample centreSample = destinationAtTargetPixel(
                destination, destinationSixteenBit, point);
            const Sample left = destinationAtTargetPixel(
                destination, destinationSixteenBit, point + QPointF(-1.0, 0.0));
            const Sample right = destinationAtTargetPixel(
                destination, destinationSixteenBit, point + QPointF(1.0, 0.0));
            const Sample up = destinationAtTargetPixel(
                destination, destinationSixteenBit, point + QPointF(0.0, -1.0));
            const Sample down = destinationAtTargetPixel(
                destination, destinationSixteenBit, point + QPointF(0.0, 1.0));
            if (!centreSample.valid || !left.valid || !right.valid || !up.valid || !down.valid) {
                continue;
            }
            BoundaryObservation observation;
            observation.targetPixel = point;
            observation.destination = centreSample;
            observation.gradientX = (luminance(right.rgba) - luminance(left.rgba)) * 0.5;
            observation.gradientY = (luminance(down.rgba) - luminance(up.rgba)) * 0.5;
            boundary.push_back(observation);
        }
    }

    if (boundary.size() < 8) {
        result.error = QStringLiteral(
            "Spot Healing could not inspect enough surrounding pixels near the image edge.");
        return result;
    }

    const QPointF documentOrigin = mapTargetPixelToDocument(request, QPointF(0.0, 0.0));
    const auto localVectorToDocument = [&](const QPointF &localVector) {
        return mapTargetPixelToDocument(request, localVector) - documentOrigin;
    };

    const double span = std::max(targetBounds.width(), targetBounds.height());
    const double firstRadius = std::max(request.diameterPixels * 1.15,
                                        span * 0.55 + request.diameterPixels * 0.65);
    const double maximumRadius = std::max(firstRadius + request.diameterPixels * 1.5,
                                          span + request.diameterPixels * 6.0);
    const double ringStep = std::max(6.0, request.diameterPixels * 0.58);
    const int directions = 20;
    const int maximumCandidates = std::clamp(request.maximumCandidates, 16, 512);

    double bestScore = std::numeric_limits<double>::infinity();
    QPointF bestOffsetDocument;
    bool foundCandidate = false;
    int ringIndex = 0;

    for (double searchRadius = firstRadius;
         searchRadius <= maximumRadius + 1.0e-6
             && result.candidatesEvaluated < maximumCandidates;
         searchRadius += ringStep, ++ringIndex) {
        if (cancellationRequested()) {
            return cancelledResult();
        }
        const bool stagger = (ringIndex & 1) != 0;
        for (int direction = 0;
             direction < directions && result.candidatesEvaluated < maximumCandidates;
             ++direction) {
            const double angle = (direction + (stagger ? 0.5 : 0.0))
                * (2.0 * Pi / directions);
            const QPointF localOffset(std::cos(angle) * searchRadius,
                                      std::sin(angle) * searchRadius);

            // The sampled patch is the complete target gesture translated by
            // localOffset. Reject every candidate whose footprint overlaps the
            // blemish footprint; this prevents the tool from cloning the defect
            // back into itself.
            if (targetBounds.adjusted(-1.0, -1.0, 1.0, 1.0)
                    .intersects(targetBounds.translated(localOffset))) {
                continue;
            }

            const QPointF offsetDocument = localVectorToDocument(localOffset);
            QVector<std::array<double, 3>> differences;
            differences.reserve(boundary.size());
            QVector<std::array<double, 2>> destinationGradients;
            destinationGradients.reserve(boundary.size());
            QVector<std::array<double, 2>> sourceGradients;
            sourceGradients.reserve(boundary.size());
            QVector<double> destinationAlphas;
            destinationAlphas.reserve(boundary.size());
            QVector<double> sourceAlphas;
            sourceAlphas.reserve(boundary.size());

            int valid = 0;
            int observationIndex = 0;
            for (const BoundaryObservation &observation : boundary) {
                if ((observationIndex++ & 31) == 0 && cancellationRequested()) {
                    return cancelledResult();
                }
                const Sample sampled = sourceAtTargetPixel(request,
                                                           source,
                                                           sourceSixteenBit,
                                                           observation.targetPixel,
                                                           offsetDocument);
                const Sample left = sourceAtTargetPixel(request,
                                                        source,
                                                        sourceSixteenBit,
                                                        observation.targetPixel
                                                            + QPointF(-1.0, 0.0),
                                                        offsetDocument);
                const Sample right = sourceAtTargetPixel(request,
                                                         source,
                                                         sourceSixteenBit,
                                                         observation.targetPixel
                                                             + QPointF(1.0, 0.0),
                                                         offsetDocument);
                const Sample up = sourceAtTargetPixel(request,
                                                      source,
                                                      sourceSixteenBit,
                                                      observation.targetPixel
                                                          + QPointF(0.0, -1.0),
                                                      offsetDocument);
                const Sample down = sourceAtTargetPixel(request,
                                                        source,
                                                        sourceSixteenBit,
                                                        observation.targetPixel
                                                            + QPointF(0.0, 1.0),
                                                        offsetDocument);
                if (!sampled.valid || !left.valid || !right.valid
                    || !up.valid || !down.valid) {
                    continue;
                }
                std::array<double, 3> difference {};
                for (int component = 0; component < 3; ++component) {
                    difference[component] = observation.destination.rgba[component]
                        - sampled.rgba[component];
                }
                differences.push_back(difference);
                destinationGradients.push_back({observation.gradientX,
                                                observation.gradientY});
                sourceGradients.push_back({
                    (luminance(right.rgba) - luminance(left.rgba)) * 0.5,
                    (luminance(down.rgba) - luminance(up.rgba)) * 0.5
                });
                destinationAlphas.push_back(observation.destination.rgba[3]);
                sourceAlphas.push_back(sampled.rgba[3]);
                ++valid;
            }

            if (valid < std::max(8, static_cast<int>(boundary.size() * 0.82))) {
                continue;
            }
            ++result.candidatesEvaluated;

            std::array<double, 3> meanDifference {0.0, 0.0, 0.0};
            for (const auto &difference : differences) {
                for (int component = 0; component < 3; ++component) {
                    meanDifference[component] += difference[component];
                }
            }
            for (double &value : meanDifference) {
                value /= differences.size();
            }

            double colourResidual = 0.0;
            double gradientResidual = 0.0;
            double alphaResidual = 0.0;
            double sourceGradientEnergy = 0.0;
            double destinationGradientEnergy = 0.0;
            for (int index = 0; index < differences.size(); ++index) {
                if ((index & 63) == 0 && cancellationRequested()) {
                    return cancelledResult();
                }
                for (int component = 0; component < 3; ++component) {
                    const double residual = differences[index][component]
                        - meanDifference[component];
                    colourResidual += residual * residual;
                }
                const double destinationGx = destinationGradients[index][0];
                const double destinationGy = destinationGradients[index][1];
                const double sourceGx = sourceGradients[index][0];
                const double sourceGy = sourceGradients[index][1];
                const double dx = destinationGx - sourceGx;
                const double dy = destinationGy - sourceGy;
                gradientResidual += dx * dx + dy * dy;
                sourceGradientEnergy += sourceGx * sourceGx + sourceGy * sourceGy;
                destinationGradientEnergy += destinationGx * destinationGx
                    + destinationGy * destinationGy;
                const double alphaDelta = destinationAlphas[index]
                    - sourceAlphas[index];
                alphaResidual += alphaDelta * alphaDelta;
            }

            const double inverse = 1.0 / differences.size();
            colourResidual *= inverse;
            gradientResidual *= inverse;
            alphaResidual *= inverse;
            sourceGradientEnergy *= inverse;
            destinationGradientEnergy *= inverse;
            const double textureEnergyResidual = std::abs(
                std::sqrt(sourceGradientEnergy + 1.0e-12)
                    - std::sqrt(destinationGradientEnergy + 1.0e-12));
            const double distancePenalty = searchRadius / std::max(1.0, maximumRadius);
            const double score = colourResidual * 0.75
                + gradientResidual * 4.5
                + textureEnergyResidual * 0.8
                + alphaResidual * 1.8
                + distancePenalty * 0.035;

            if (score < bestScore) {
                bestScore = score;
                bestOffsetDocument = offsetDocument;
                foundCandidate = true;
            }
        }
    }

    if (!foundCandidate) {
        result.error = QStringLiteral(
            "Spot Healing could not find a valid nearby source patch. Try a smaller brush or move farther from the image edge.");
        return result;
    }

    HealingBrushRequest healing;
    healing.destination = request.destination;
    healing.source = request.source;
    healing.targetSegments = request.targetSegments;
    healing.targetPixelToLayer = request.targetPixelToLayer;
    healing.targetLayerToDocument = request.targetLayerToDocument;
    healing.sourceDocumentToLayer = request.sourceDocumentToLayer;
    healing.sourceLayerToPixel = request.sourceLayerToPixel;
    healing.sourceOffsetDocument = bestOffsetDocument;
    healing.diameterPixels = request.diameterPixels;
    healing.opacity = request.opacity;
    healing.hardness = request.hardness;
    healing.cancelRequested = request.cancelRequested;

    const HealingBrushResult healed = applyHealingBrush(healing);
    if (healed.cancelled) {
        return cancelledResult();
    }
    result.image = healed.image;
    result.affectedRect = healed.affectedRect;
    result.sourceOffsetDocument = bestOffsetDocument;
    result.error = healed.error;
    return result;
}

} // namespace vfx
