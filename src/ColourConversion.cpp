#include "ColourConversion.h"

#include "Adjustment.h"
#include "OcioIntegration.h"
#include "PhotoDocument.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <functional>
#include <limits>

namespace vfx {
namespace {

constexpr qsizetype MinimumExternalIccBytes = 128;
constexpr qsizetype MaximumExternalIccBytes = 16 * 1024 * 1024;

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) *errorMessage = message;
}

bool cancellationRequested(const std::atomic_bool *cancelRequested)
{
    return cancelRequested
        && cancelRequested->load(std::memory_order_acquire);
}

bool descriptorSupportsDocumentConversion(
    const ColourSpaceDescriptor &descriptor)
{
    if (!descriptor.isValid() || descriptor.isUntagged()) return false;
    if (descriptor.kind == ColourSpaceKind::Ocio) {
        return !descriptor.ocioConfigId.trimmed().isEmpty()
            && descriptor.ocioConfigFingerprint.size() == 32
            && !descriptor.ocioSpace.trimmed().isEmpty();
    }
    return descriptor.toQColorSpace().isValid();
}

DocumentColourState targetColourState(const PhotoDocument &document,
                                      const ColourSpaceDescriptor &target,
                                      const DocumentProfileOperation operation)
{
    DocumentColourState state = document.colourState();
    const ColourSpaceDescriptor oldWorking = state.workingSpace;
    state.workingSpace = target;
    state.processingCompatibility = ColourProcessingCompatibility::ManagedV1;

    if (operation == DocumentProfileOperation::Assign
        && state.inputProfile.isUntagged()) {
        state.inputProfile = target;
    }

    if (state.output.profile.isUntagged()
        || state.output.profile.stableFingerprint()
            == oldWorking.stableFingerprint()) {
        state.output.profile = target;
    }
    const quint64 nextRevision = state.revision
        == std::numeric_limits<quint64>::max()
        ? state.revision : state.revision + 1;
    state.revision = std::max<quint64>(nextRevision, 2);
    return state;
}

bool retagImage(QImage *image, const ColourSpaceDescriptor &target)
{
    if (!image || image->isNull()) return true;
    if (target.kind == ColourSpaceKind::Ocio) {
        // DocumentColourState remains authoritative. For supported ACES and
        // Rec.709 working spaces, attach an exact matrix/TRC proxy so the
        // existing managed encoded/linear CPU adjustment contracts continue
        // to operate in the correct primaries and transfer function.
        const QColorSpace proxy = ocioQtWorkingSpaceProxy(target);
        if (!proxy.isValid()) return false;
        image->setColorSpace(proxy);
        return image->colorSpace().isValid();
    }
    const QColorSpace targetSpace = target.toQColorSpace();
    if (!targetSpace.isValid()) return false;
    image->setColorSpace(targetSpace);
    return image->colorSpace().isValid();
}

struct PreparedTransform {
    std::optional<QColorTransform> qt;
    std::shared_ptr<const OcioCpuTransform> ocio;

    bool isValid() const { return qt.has_value() || (ocio && ocio->isValid()); }
};

PreparedTransform createPreparedTransform(
    const OcioConfigReference &ocioConfig,
    const ColourSpaceDescriptor &source,
    const ColourSpaceDescriptor &target,
    const ColourTransformPurpose purpose,
    const ColourRenderingIntent renderingIntent,
    const bool blackPointCompensation,
    QString *errorMessage)
{
    PreparedTransform prepared;
    if (source.kind == ColourSpaceKind::Ocio
        || target.kind == ColourSpaceKind::Ocio) {
        prepared.ocio = createOcioCpuTransform(
            ocioConfig, source, target, errorMessage);
        return prepared;
    }

    ColourTransformRequest request;
    request.source = source;
    request.destination = target;
    request.purpose = purpose;
    request.renderingIntent = renderingIntent;
    request.blackPointCompensation = blackPointCompensation;
    prepared.qt = ColourTransformService::instance().qtTransform(request);
    if (!prepared.qt) {
        setError(errorMessage,
                 QStringLiteral("A colour transform could not be created for the selected source and destination profiles."));
    }
    return prepared;
}

bool convertImage(QImage *image,
                  const PreparedTransform &transform,
                  const ColourSpaceDescriptor &target,
                  const std::atomic_bool *cancelRequested,
                  QString *errorMessage)
{
    if (!image || image->isNull()) return true;
    if (cancellationRequested(cancelRequested)) return false;

    if (transform.ocio) {
        if (!applyOcioCpuTransform(image, *transform.ocio,
                                   cancelRequested, errorMessage)) {
            return false;
        }
        return retagImage(image, target);
    }
    if (!transform.qt) {
        setError(errorMessage,
                 QStringLiteral("The document colour transform is unavailable."));
        return false;
    }

    const QImage::Format originalFormat = image->format();
    QImage converted = *image;
    converted.applyColorTransform(*transform.qt);
    const QColorSpace targetSpace = target.toQColorSpace();
    converted.setColorSpace(targetSpace);
    if (converted.isNull() || !converted.colorSpace().isValid()) {
        setError(errorMessage,
                 QStringLiteral("Qt could not convert one of the document's raster images to the selected profile."));
        return false;
    }
    if (converted.format() != originalFormat) {
        converted = converted.convertToFormat(originalFormat);
        if (converted.isNull()) {
            setError(errorMessage,
                     QStringLiteral("The converted raster image could not be restored to its original pixel format."));
            return false;
        }
        converted.setColorSpace(targetSpace);
    }
    *image = std::move(converted);
    return true;
}

QColor convertSemanticColour(const QColor &colour,
                             const PreparedTransform &transform,
                             bool *ok)
{
    if (ok) *ok = false;
    if (!colour.isValid()) {
        if (ok) *ok = true;
        return colour;
    }
    if (transform.ocio) {
        return mapOcioSemanticColour(colour, *transform.ocio, ok);
    }
    if (!transform.qt) return {};
    QColor converted = transform.qt->map(colour);
    converted.setAlphaF(colour.alphaF());
    if (ok) *ok = converted.isValid();
    return converted;
}

bool transformLayerTree(QVector<LayerNode> *layers,
                        const PreparedTransform *transform,
                        const ColourSpaceDescriptor &target,
                        const bool convertPixels,
                        int *rasterCount,
                        int *semanticColourCount,
                        const std::atomic_bool *cancelRequested,
                        QString *errorMessage)
{
    if (!layers) {
        setError(errorMessage,
                 QStringLiteral("The document layer tree is unavailable."));
        return false;
    }
    if (convertPixels && (!transform || !transform->isValid())) {
        setError(errorMessage,
                 QStringLiteral("The document colour transform is unavailable."));
        return false;
    }

    std::function<bool(QVector<LayerNode> &)> visit;
    visit = [&](QVector<LayerNode> &nodes) {
        for (LayerNode &layer : nodes) {
            if (cancellationRequested(cancelRequested)) return false;

            if (!layer.rasterImage.isNull()) {
                const bool accepted = convertPixels
                    ? convertImage(&layer.rasterImage, *transform, target,
                                   cancelRequested, errorMessage)
                    : retagImage(&layer.rasterImage, target);
                if (!accepted) {
                    if (!cancellationRequested(cancelRequested)
                        && errorMessage && errorMessage->isEmpty()) {
                        setError(errorMessage,
                                 QStringLiteral("A raster layer could not be prepared for the selected profile."));
                    }
                    return false;
                }
                ++(*rasterCount);
                ++layer.revision;
            }

            if (convertPixels && layer.type == LayerType::Vector) {
                for (VectorShape &shape : layer.vectorData.objects) {
                    bool changed = false;
                    if (shape.fill.colour.isValid()) {
                        bool ok = false;
                        const QColor converted = convertSemanticColour(
                            shape.fill.colour, *transform, &ok);
                        if (!ok) {
                            setError(errorMessage,
                                     QStringLiteral("A vector fill colour could not be converted."));
                            return false;
                        }
                        shape.fill.colour = converted;
                        ++(*semanticColourCount);
                        changed = true;
                    }
                    if (shape.stroke.colour.isValid()) {
                        bool ok = false;
                        const QColor converted = convertSemanticColour(
                            shape.stroke.colour, *transform, &ok);
                        if (!ok) {
                            setError(errorMessage,
                                     QStringLiteral("A vector stroke colour could not be converted."));
                            return false;
                        }
                        shape.stroke.colour = converted;
                        ++(*semanticColourCount);
                        changed = true;
                    }
                    if (changed) ++shape.revision;
                }
                layer.vectorData.normalise();
                ++layer.revision;
            }

            if (convertPixels && layer.type == LayerType::Text
                && layer.textData.colour.isValid()) {
                bool ok = false;
                const QColor converted = convertSemanticColour(
                    layer.textData.colour, *transform, &ok);
                if (!ok) {
                    setError(errorMessage,
                             QStringLiteral("A text-layer colour could not be converted."));
                    return false;
                }
                layer.textData.colour = converted;
                ++layer.textData.revision;
                ++layer.revision;
                ++(*semanticColourCount);
            }

            if (convertPixels && layer.type == LayerType::Adjustment
                && layer.adjustmentType == AdjustmentType::GradientMap) {
                AdjustmentData data = layer.effectiveAdjustmentData();
                if (auto *gradient = std::get_if<GradientMapParameters>(
                        &data.parameters)) {
                    for (GradientStop &stop : gradient->stops) {
                        bool ok = false;
                        const QColor converted = convertSemanticColour(
                            stop.colour, *transform, &ok);
                        if (!ok) {
                            setError(errorMessage,
                                     QStringLiteral("A Gradient Map stop could not be converted."));
                            return false;
                        }
                        stop.colour = converted;
                        ++(*semanticColourCount);
                    }
                    layer.setAdjustmentData(data);
                }
            }

            // Masks and selections are scalar coverage and are never colour
            // transformed. Child layers share the document working space.
            if (!visit(layer.children)) return false;
        }
        return true;
    };
    return visit(*layers);
}

bool prepareProfileOperation(const PhotoDocument &document,
                             const ColourSpaceDescriptor &target,
                             const DocumentProfileOperation operation,
                             PreparedColourProfileResult *result,
                             const std::atomic_bool *cancelRequested,
                             QString *errorMessage)
{
    if (!result) {
        setError(errorMessage,
                 QStringLiteral("The colour-conversion result destination is missing."));
        return false;
    }
    *result = {};
    result->operation = operation;

    if (!document.hasImage()) {
        setError(errorMessage,
                 QStringLiteral("There is no open document to change."));
        return false;
    }
    if (!descriptorSupportsDocumentConversion(target)) {
        setError(errorMessage,
                 QStringLiteral("The selected destination is not a supported ICC or OCIO working colour space."));
        return false;
    }
    if (target.kind == ColourSpaceKind::Ocio) {
        const OcioConfigReference &config = document.colourState().ocioConfig;
        if (!config.isConfigured()
            || target.ocioConfigId != config.identifier
            || target.ocioConfigFingerprint != config.fingerprint) {
            setError(errorMessage,
                     QStringLiteral("The selected OCIO colour space does not belong to the document's active configuration."));
            return false;
        }
    }

    const ColourSpaceDescriptor sourceDescriptor =
        document.colourState().workingSpace;
    if (operation == DocumentProfileOperation::Convert
        && sourceDescriptor.isUntagged()) {
        setError(errorMessage,
                 QStringLiteral("The document is untagged. Assign a source profile before converting it to another profile."));
        return false;
    }

    PreparedTransform transform;
    if (operation == DocumentProfileOperation::Convert) {
        transform = createPreparedTransform(
            document.colourState().ocioConfig,
            sourceDescriptor,
            target,
            ColourTransformPurpose::WorkingToWorking,
            ColourRenderingIntent::RelativeColorimetric,
            true,
            errorMessage);
        if (!transform.isValid()) return false;
    }

    result->canvasImage = document.sourceImage();
    result->layers = document.layers();
    result->colourState = targetColourState(document, target, operation);

    if (cancellationRequested(cancelRequested)) {
        result->cancelled = true;
        return false;
    }

    const bool convertPixels = operation == DocumentProfileOperation::Convert;
    const bool canvasAccepted = convertPixels
        ? convertImage(&result->canvasImage, transform, target,
                       cancelRequested, errorMessage)
        : retagImage(&result->canvasImage, target);
    if (!canvasAccepted) {
        result->cancelled = cancellationRequested(cancelRequested);
        return false;
    }
    ++result->processedRasterImages;

    if (!transformLayerTree(&result->layers,
                            convertPixels ? &transform : nullptr,
                            target,
                            convertPixels,
                            &result->processedRasterImages,
                            &result->processedSemanticColours,
                            cancelRequested,
                            errorMessage)) {
        result->cancelled = cancellationRequested(cancelRequested);
        return false;
    }

    if (!result->colourState.isSafe(errorMessage)) return false;
    return true;
}

} // namespace

bool PreparedColourProfileResult::isValid() const
{
    return !canvasImage.isNull() && colourState.isSafe(nullptr);
}

bool transformImageColourSpace(
    QImage *image,
    const OcioConfigReference &ocioConfig,
    const ColourSpaceDescriptor &source,
    const ColourSpaceDescriptor &destination,
    const ColourTransformPurpose purpose,
    const ColourRenderingIntent renderingIntent,
    const bool blackPointCompensation,
    const std::atomic_bool *cancelRequested,
    QString *errorMessage)
{
    if (!image || image->isNull()) {
        setError(errorMessage, QStringLiteral("There is no image to colour convert."));
        return false;
    }
    if (!source.isValid() || !destination.isValid()) {
        setError(errorMessage,
                 QStringLiteral("The source or destination colour-space descriptor is invalid."));
        return false;
    }
    if (cancellationRequested(cancelRequested)) return false;

    if (source.stableFingerprint() == destination.stableFingerprint()) {
        if (destination.isUntagged()) {
            image->setColorSpace(QColorSpace());
            return true;
        }
        if (!retagImage(image, destination)) {
            setError(errorMessage,
                     QStringLiteral("The image could not be tagged with the requested destination profile."));
            return false;
        }
        return true;
    }
    if (source.isUntagged() || destination.isUntagged()) {
        setError(errorMessage,
                 QStringLiteral("An untagged colour space cannot be converted without first assigning its interpretation."));
        return false;
    }

    const PreparedTransform transform = createPreparedTransform(
        ocioConfig, source, destination, purpose, renderingIntent,
        blackPointCompensation, errorMessage);
    if (!transform.isValid()) return false;
    if (purpose == ColourTransformPurpose::WorkingToOutput
        && destination.kind == ColourSpaceKind::Ocio
        && transform.ocio) {
        // An OCIO export destination has no ICC payload requirement. Apply the
        // processor and leave the encoded image untagged when the destination
        // has no exact Qt matrix/TRC proxy. Document Convert to Profile still
        // uses convertImage() and therefore retains its stricter working-space
        // proxy requirement.
        return applyOcioCpuTransform(image, *transform.ocio,
                                     cancelRequested, errorMessage);
    }
    return convertImage(image, transform, destination,
                        cancelRequested, errorMessage);
}

QVector<ColourSpaceDescriptor> commonWorkingColourSpaces()
{
    const QColorSpace::NamedColorSpace namedSpaces[] = {
        QColorSpace::SRgb,
        QColorSpace::SRgbLinear,
        QColorSpace::DisplayP3,
        QColorSpace::AdobeRgb,
        QColorSpace::ProPhotoRgb,
    };
    QVector<ColourSpaceDescriptor> spaces;
    spaces.reserve(5);
    for (const QColorSpace::NamedColorSpace namedSpace : namedSpaces) {
        const ColourSpaceDescriptor descriptor =
            ColourSpaceDescriptor::fromQColorSpace(QColorSpace(namedSpace));
        if (descriptor.isValid() && descriptor.toQColorSpace().isValid()) {
            spaces.push_back(descriptor);
        }
    }
    return spaces;
}

bool loadExternalIccProfile(const QString &filePath,
                            ColourSpaceDescriptor *descriptor,
                            QString *errorMessage)
{
    if (!descriptor) {
        setError(errorMessage,
                 QStringLiteral("The profile destination is missing."));
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage,
                 QStringLiteral("Could not open the selected ICC profile: %1")
                     .arg(file.errorString()));
        return false;
    }
    if (file.size() < MinimumExternalIccBytes || file.size() > MaximumExternalIccBytes) {
        setError(errorMessage,
                 QStringLiteral("The selected ICC profile is smaller than an ICC header or exceeds the 16 MiB safety limit."));
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() != file.size()) {
        setError(errorMessage,
                 QStringLiteral("The selected ICC profile could not be read completely."));
        return false;
    }
    const QColorSpace colourSpace = QColorSpace::fromIccProfile(bytes);
    if (!colourSpace.isValid()) {
        setError(errorMessage,
                 QStringLiteral("The selected file is not a supported ICC profile."));
        return false;
    }
    if (bytes.size() < 20 || bytes.mid(16, 4) != QByteArray("RGB ", 4)) {
        setError(errorMessage,
                 QStringLiteral("The selected ICC profile is not an RGB working-space profile."));
        return false;
    }

    const QByteArray fingerprint = QCryptographicHash::hash(
        bytes, QCryptographicHash::Sha256);
    *descriptor = ColourSpaceDescriptor::externalIcc(
        QFileInfo(filePath).absoluteFilePath(), fingerprint, bytes,
        colourSpace.description());
    if (!descriptor->isValid()) {
        setError(errorMessage,
                 QStringLiteral("The selected ICC profile could not be represented safely."));
        return false;
    }
    return true;
}

bool prepareAssignedDocumentProfile(const PhotoDocument &document,
                                    const ColourSpaceDescriptor &target,
                                    PreparedColourProfileResult *result,
                                    QString *errorMessage)
{
    return prepareProfileOperation(document, target,
                                   DocumentProfileOperation::Assign,
                                   result, nullptr, errorMessage);
}

bool prepareConvertedDocumentProfile(const PhotoDocument &document,
                                      const ColourSpaceDescriptor &target,
                                      PreparedColourProfileResult *result,
                                      const std::atomic_bool *cancelRequested,
                                      QString *errorMessage)
{
    return prepareProfileOperation(document, target,
                                   DocumentProfileOperation::Convert,
                                   result, cancelRequested, errorMessage);
}

} // namespace vfx
