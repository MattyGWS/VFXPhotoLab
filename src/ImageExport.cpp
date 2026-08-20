#include "ImageExport.h"

#include "BlueNoise64.h"
#include "ColourConversion.h"
#include "TgaCodec.h"

#include <QByteArray>
#include <QFileInfo>
#include <QList>
#include <QImageWriter>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace vfx {
namespace {

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) *errorMessage = message;
}

bool cancellationRequested(const std::atomic_bool *cancelRequested)
{
    return cancelRequested
        && cancelRequested->load(std::memory_order_acquire);
}

QString normalisedSuffix(const QString &filePath)
{
    QString suffix = QFileInfo(filePath).suffix().trimmed().toLower();
    if (suffix == QStringLiteral("jpeg")) suffix = QStringLiteral("jpg");
    if (suffix == QStringLiteral("tif")) suffix = QStringLiteral("tiff");
    return suffix;
}

quint8 quantizeChannel(const quint16 value,
                       const double noise)
{
    if (value == 0) return 0;
    if (value == std::numeric_limits<quint16>::max()) return 255;
    const double scaled = static_cast<double>(value) * 255.0 / 65535.0;
    return static_cast<quint8>(std::clamp(
        static_cast<int>(std::floor(scaled + noise + 0.5)), 0, 255));
}

quint8 quantizeChannelNearest(const quint16 value)
{
    const quint32 numerator = static_cast<quint32>(value) * 255u + 32767u;
    return static_cast<quint8>(numerator / 65535u);
}

bool outputMatteColour(const QColor &matte,
                       const DocumentColourState &colourState,
                       const OutputColourSettings &outputSettings,
                       const ColourSpaceDescriptor &outputProfile,
                       QColor *result,
                       const std::atomic_bool *cancelRequested,
                       QString *errorMessage)
{
    if (!result) {
        setError(errorMessage,
                 QStringLiteral("The export matte destination is missing."));
        return false;
    }
    QColor source = matte.isValid() ? matte : QColor(Qt::white);
    source.setAlpha(255);
    if (outputProfile.isUntagged()) {
        *result = source;
        return true;
    }

    QImage swatch(1, 1, QImage::Format_RGBA64);
    if (swatch.isNull()) {
        setError(errorMessage,
                 QStringLiteral("The export matte colour could not be allocated."));
        return false;
    }
    auto *swatchPixel = reinterpret_cast<QRgba64 *>(swatch.scanLine(0));
    swatchPixel[0] = source.rgba64();
    const ColourSpaceDescriptor srgb = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    swatch.setColorSpace(srgb.toQColorSpace());
    if (!transformImageColourSpace(&swatch,
                                   colourState.ocioConfig,
                                   srgb,
                                   outputProfile,
                                   ColourTransformPurpose::WorkingToOutput,
                                   outputSettings.renderingIntent,
                                   outputSettings.blackPointCompensation,
                                   cancelRequested,
                                   errorMessage)) {
        if (!cancellationRequested(cancelRequested)
            && errorMessage && errorMessage->isEmpty()) {
            setError(errorMessage,
                     QStringLiteral("The export matte colour could not be converted into the selected output space."));
        }
        return false;
    }
    const auto *pixel = reinterpret_cast<const QRgba64 *>(
        swatch.constScanLine(0));
    *result = QColor::fromRgba64(pixel[0].red(), pixel[0].green(),
                                 pixel[0].blue(), pixel[0].alpha());
    return true;
}

bool flattenTransparency(QImage *image,
                         const QColor &matte,
                         const std::atomic_bool *cancelRequested)
{
    if (!image || image->isNull()) return false;
    QImage rgba64 = image->convertToFormat(QImage::Format_RGBA64);
    if (rgba64.isNull()) return false;
    const QRgba64 matte64 = matte.rgba64();
    rgba64.detach();
    for (int y = 0; y < rgba64.height(); ++y) {
        if (cancellationRequested(cancelRequested)) return false;
        auto *pixels = reinterpret_cast<QRgba64 *>(rgba64.scanLine(y));
        for (int x = 0; x < rgba64.width(); ++x) {
            const quint32 alpha = pixels[x].alpha();
            const quint32 inverse = 65535u - alpha;
            const auto blend = [alpha, inverse](const quint16 foreground,
                                                const quint16 background) {
                return static_cast<quint16>((static_cast<quint64>(foreground) * alpha
                                             + static_cast<quint64>(background) * inverse
                                             + 32767u) / 65535u);
            };
            pixels[x] = QRgba64::fromRgba64(
                blend(pixels[x].red(), matte64.red()),
                blend(pixels[x].green(), matte64.green()),
                blend(pixels[x].blue(), matte64.blue()),
                65535);
        }
    }
    rgba64.setColorSpace(image->colorSpace());
    rgba64.setDevicePixelRatio(image->devicePixelRatio());
    rgba64.setDotsPerMeterX(image->dotsPerMeterX());
    rgba64.setDotsPerMeterY(image->dotsPerMeterY());
    *image = std::move(rgba64);
    return true;
}

bool descriptorEmbedsIcc(const ColourSpaceDescriptor &descriptor)
{
    return descriptor.kind != ColourSpaceKind::Ocio
        && !descriptor.isUntagged()
        && descriptor.toQColorSpace().isValid();
}

} // namespace

bool PreparedImageExport::isValid() const
{
    return !image.isNull() && capabilities.valid
        && (bitDepth == ImageExportBitDepth::Eight
            || bitDepth == ImageExportBitDepth::Sixteen);
}

ImageExportCapabilities imageExportCapabilitiesForPath(const QString &filePath)
{
    const QString suffix = normalisedSuffix(filePath);
    ImageExportCapabilities result;
    result.suffix = suffix;
    if (suffix == QStringLiteral("png")) {
        result.displayName = QStringLiteral("PNG");
        result.valid = true;
        result.supportsAlpha = true;
        result.supportsSixteenBit = true;
        result.supportsIccProfile = true;
    } else if (suffix == QStringLiteral("jpg")) {
        result.displayName = QStringLiteral("JPEG");
        result.valid = true;
        result.supportsAlpha = false;
        result.supportsIccProfile = true;
        result.supportsQuality = true;
    } else if (suffix == QStringLiteral("tga")) {
        result.displayName = QStringLiteral("TGA");
        result.valid = true;
        result.supportsAlpha = true;
    } else if (suffix == QStringLiteral("tiff")) {
        result.displayName = QStringLiteral("TIFF");
        result.valid = true;
        result.supportsAlpha = true;
        result.supportsSixteenBit = true;
        result.supportsIccProfile = true;
    } else if (suffix == QStringLiteral("webp")) {
        result.displayName = QStringLiteral("WebP");
        result.valid = true;
        result.supportsAlpha = true;
        // Qt's image plugin availability is runtime-dependent and does not
        // expose a reliable per-plugin ICC-write capability query. Do not
        // promise embedded metadata here; converted pixels remain valid and
        // the UI reports that this export is untagged.
        result.supportsQuality = true;
    } else if (suffix == QStringLiteral("bmp")) {
        result.displayName = QStringLiteral("BMP");
        result.valid = true;
        result.supportsAlpha = false;
    }
    return result;
}

bool imageExportWriterAvailable(
    const ImageExportCapabilities &capabilities)
{
    if (!capabilities.valid) return false;
    if (capabilities.suffix == QStringLiteral("tga")) return true;
    const QList<QByteArray> formats = QImageWriter::supportedImageFormats();
    const auto has = [&](const QByteArray &format) {
        return formats.contains(format);
    };
    if (capabilities.suffix == QStringLiteral("jpg")) {
        return has(QByteArrayLiteral("jpg"))
            || has(QByteArrayLiteral("jpeg"));
    }
    if (capabilities.suffix == QStringLiteral("tiff")) {
        return has(QByteArrayLiteral("tiff"))
            || has(QByteArrayLiteral("tif"));
    }
    return has(capabilities.suffix.toLatin1());
}

QString imageExportBitDepthName(const ImageExportBitDepth bitDepth)
{
    return bitDepth == ImageExportBitDepth::Sixteen
        ? QStringLiteral("16-bit integer")
        : QStringLiteral("8-bit integer");
}

QString imageExportDitherName(const ImageExportDither dither)
{
    return dither == ImageExportDither::BlueNoise64
        ? QStringLiteral("64 × 64 blue-noise")
        : QStringLiteral("Disabled");
}

QString imageExportAlphaModeName(const ImageExportAlphaMode mode)
{
    return mode == ImageExportAlphaMode::FlattenToMatte
        ? QStringLiteral("Flatten to matte")
        : QStringLiteral("Preserve when supported");
}

bool validateImageExportRequest(const ImageExportRequest &request,
                                const DocumentColourState &colourState,
                                QString *errorMessage)
{
    const ImageExportCapabilities capabilities =
        imageExportCapabilitiesForPath(request.filePath);
    if (!capabilities.valid) {
        setError(errorMessage,
                 QStringLiteral("The selected file extension is not a supported export format."));
        return false;
    }
    if (request.bitDepth != ImageExportBitDepth::Eight
        && request.bitDepth != ImageExportBitDepth::Sixteen) {
        setError(errorMessage,
                 QStringLiteral("The requested export bit depth is invalid."));
        return false;
    }
    if (request.dither != ImageExportDither::None
        && request.dither != ImageExportDither::BlueNoise64) {
        setError(errorMessage,
                 QStringLiteral("The requested export dither mode is invalid."));
        return false;
    }
    if (request.alphaMode != ImageExportAlphaMode::PreserveWhenSupported
        && request.alphaMode != ImageExportAlphaMode::FlattenToMatte) {
        setError(errorMessage,
                 QStringLiteral("The requested export Alpha mode is invalid."));
        return false;
    }
    if (!request.matteColour.isValid()) {
        setError(errorMessage,
                 QStringLiteral("The requested export matte colour is invalid."));
        return false;
    }
    if (request.bitDepth == ImageExportBitDepth::Sixteen
        && !capabilities.supportsSixteenBit) {
        setError(errorMessage,
                 QStringLiteral("%1 export does not support 16-bit integer channels.")
                     .arg(capabilities.displayName));
        return false;
    }
    if (request.bitDepth == ImageExportBitDepth::Eight
        && !capabilities.supportsEightBit) {
        setError(errorMessage,
                 QStringLiteral("%1 export does not support 8-bit integer channels.")
                     .arg(capabilities.displayName));
        return false;
    }
    if (!colourState.workingSpace.isValid()) {
        setError(errorMessage,
                 QStringLiteral("The document working-space descriptor is invalid."));
        return false;
    }
    if (request.convertToOutputProfile) {
        if (!request.output.profile.isValid()
            || request.output.profile.isUntagged()) {
            setError(errorMessage,
                     QStringLiteral("Choose a valid ICC or OCIO output profile before exporting."));
            return false;
        }
        if (request.output.profile.kind == ColourSpaceKind::Ocio
            && (!colourState.ocioConfig.isConfigured()
                || request.output.profile.ocioConfigId
                    != colourState.ocioConfig.identifier
                || request.output.profile.ocioConfigFingerprint
                    != colourState.ocioConfig.fingerprint)) {
            setError(errorMessage,
                     QStringLiteral("The selected OCIO output space does not belong to the document's active fingerprint-matched configuration."));
            return false;
        }
        if (colourState.workingSpace.isUntagged()) {
            setError(errorMessage,
                     QStringLiteral("The document is untagged. Assign its working profile before converting to an output profile."));
            return false;
        }
    }
    return true;
}

QImage quantizeExportToEightBit(
    const QImage &rgba64,
    const ImageExportDither dither,
    const quint32 seed,
    const std::atomic_bool *cancelRequested)
{
    if (rgba64.isNull()) return {};
    QImage source = rgba64.format() == QImage::Format_RGBA64
        ? rgba64 : rgba64.convertToFormat(QImage::Format_RGBA64);
    if (source.isNull()) return {};

    QImage output(source.size(), QImage::Format_RGBA8888);
    if (output.isNull()) return {};
    const quint32 widthPhase = static_cast<quint32>(source.width()) * 37u;
    const quint32 heightPhase = static_cast<quint32>(source.height()) * 53u;
    const int phaseX = static_cast<int>((seed ^ widthPhase)
                                        & (BlueNoiseTileSize - 1));
    const int phaseY = static_cast<int>(((seed >> 8) ^ heightPhase)
                                       & (BlueNoiseTileSize - 1));
    for (int y = 0; y < source.height(); ++y) {
        if (cancellationRequested(cancelRequested)) return {};
        const auto *src = reinterpret_cast<const QRgba64 *>(source.constScanLine(y));
        uchar *dst = output.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            double noise = 0.0;
            if (dither == ImageExportDither::BlueNoise64) {
                const int tileX = (x + phaseX) & (BlueNoiseTileSize - 1);
                const int tileY = (y + phaseY) & (BlueNoiseTileSize - 1);
                const quint16 rank = BlueNoiseRanks64[
                    static_cast<std::size_t>(tileY * BlueNoiseTileSize + tileX)];
                noise = (static_cast<double>(rank) + 0.5)
                    / static_cast<double>(BlueNoiseRanks64.size()) - 0.5;
            }
            const int offset = x * 4;
            if (dither == ImageExportDither::BlueNoise64) {
                // A shared threshold preserves neutral RGB relationships and
                // avoids introducing coloured noise into grey gradients.
                dst[offset + 0] = quantizeChannel(src[x].red(), noise);
                dst[offset + 1] = quantizeChannel(src[x].green(), noise);
                dst[offset + 2] = quantizeChannel(src[x].blue(), noise);
            } else {
                dst[offset + 0] = quantizeChannelNearest(src[x].red());
                dst[offset + 1] = quantizeChannelNearest(src[x].green());
                dst[offset + 2] = quantizeChannelNearest(src[x].blue());
            }
            // Alpha is coverage, not colour. Quantise it deterministically
            // without noise so masks and transparency edges remain stable.
            dst[offset + 3] = quantizeChannelNearest(src[x].alpha());
        }
    }
    output.setColorSpace(source.colorSpace());
    output.setDevicePixelRatio(source.devicePixelRatio());
    output.setDotsPerMeterX(source.dotsPerMeterX());
    output.setDotsPerMeterY(source.dotsPerMeterY());
    return output;
}

bool prepareImageExport(const QImage &renderedWorkingImage,
                        const DocumentColourState &colourState,
                        const ImageExportRequest &request,
                        PreparedImageExport *prepared,
                        const std::atomic_bool *cancelRequested,
                        QString *errorMessage)
{
    if (!prepared) {
        setError(errorMessage,
                 QStringLiteral("The export result destination is missing."));
        return false;
    }
    *prepared = {};
    if (renderedWorkingImage.isNull()) {
        setError(errorMessage, QStringLiteral("There is no rendered image to export."));
        return false;
    }
    if (!validateImageExportRequest(request, colourState, errorMessage)) {
        return false;
    }
    if (cancellationRequested(cancelRequested)) return false;

    prepared->capabilities = imageExportCapabilitiesForPath(request.filePath);
    prepared->sourceProfile = colourState.workingSpace;
    prepared->outputProfile = request.convertToOutputProfile
        ? request.output.profile : colourState.workingSpace;
    prepared->bitDepth = request.bitDepth;
    prepared->convertedToOutputProfile = request.convertToOutputProfile
        && prepared->sourceProfile.stableFingerprint()
            != prepared->outputProfile.stableFingerprint();

    // Always enter the output transform with a straight 16-bit surface. This
    // avoids silently throwing away precision before ICC/OCIO conversion and
    // gives 8-bit exports a high-precision source for final dithering.
    QImage working = renderedWorkingImage.convertToFormat(QImage::Format_RGBA64);
    if (working.isNull()) {
        setError(errorMessage,
                 QStringLiteral("The full-resolution render could not be promoted to the export working precision."));
        return false;
    }
    working.setColorSpace(renderedWorkingImage.colorSpace());
    working.setDevicePixelRatio(renderedWorkingImage.devicePixelRatio());
    working.setDotsPerMeterX(renderedWorkingImage.dotsPerMeterX());
    working.setDotsPerMeterY(renderedWorkingImage.dotsPerMeterY());

    if (!transformImageColourSpace(&working,
                                   colourState.ocioConfig,
                                   colourState.workingSpace,
                                   prepared->outputProfile,
                                   ColourTransformPurpose::WorkingToOutput,
                                   request.output.renderingIntent,
                                   request.output.blackPointCompensation,
                                   cancelRequested,
                                   errorMessage)) {
        return false;
    }
    if (cancellationRequested(cancelRequested)) return false;

    const bool flattenAlpha = request.alphaMode == ImageExportAlphaMode::FlattenToMatte
        || !prepared->capabilities.supportsAlpha;
    if (flattenAlpha) {
        QColor matte;
        if (!outputMatteColour(request.matteColour,
                               colourState,
                               request.output,
                               prepared->outputProfile,
                               &matte,
                               cancelRequested,
                               errorMessage)) {
            return false;
        }
        if (!flattenTransparency(&working, matte, cancelRequested)) {
            if (!cancellationRequested(cancelRequested)) {
                setError(errorMessage,
                         QStringLiteral("Transparency could not be flattened for the selected export format."));
            }
            return false;
        }
        prepared->flattenedTransparency = true;
    }

    if (request.bitDepth == ImageExportBitDepth::Eight) {
        working = quantizeExportToEightBit(working, request.dither,
                                           request.ditherSeed,
                                           cancelRequested);
        if (working.isNull()) {
            setError(errorMessage,
                     QStringLiteral("The image could not be quantised to 8-bit export precision."));
            return false;
        }
        prepared->dithered = request.dither == ImageExportDither::BlueNoise64;
        if (!prepared->capabilities.supportsAlpha) {
            working = working.convertToFormat(QImage::Format_RGB888);
        }
    } else {
        if (!prepared->capabilities.supportsAlpha) {
            working = working.convertToFormat(QImage::Format_RGBX64);
        } else if (working.format() != QImage::Format_RGBA64) {
            working = working.convertToFormat(QImage::Format_RGBA64);
        }
    }
    if (working.isNull()) {
        setError(errorMessage,
                 QStringLiteral("The image could not be converted to the requested export pixel format."));
        return false;
    }

    const bool canEmbedRequestedProfile = request.output.embedProfile
        && prepared->capabilities.supportsIccProfile
        && descriptorEmbedsIcc(prepared->outputProfile);
    if (canEmbedRequestedProfile) {
        working.setColorSpace(prepared->outputProfile.toQColorSpace());
        prepared->profileEmbedded = working.colorSpace().isValid();
        if (!prepared->profileEmbedded) {
            prepared->warnings.push_back(
                QStringLiteral("The selected ICC profile could not be attached to the encoded image; the output will be saved untagged."));
        }
    } else {
        working.setColorSpace(QColorSpace());
        if (request.output.embedProfile) {
            if (prepared->outputProfile.kind == ColourSpaceKind::Ocio) {
                prepared->warnings.push_back(
                    QStringLiteral("OCIO colour spaces have no embeddable ICC payload; the converted pixels will be saved untagged."));
            } else if (prepared->outputProfile.isUntagged()) {
                prepared->warnings.push_back(
                    QStringLiteral("The exported RGB values are untagged, so there is no ICC profile available to embed."));
            } else if (!prepared->capabilities.supportsIccProfile) {
                prepared->warnings.push_back(
                    QStringLiteral("%1 does not carry an ICC profile in this export path; the output will be saved untagged.")
                        .arg(prepared->capabilities.displayName));
            }
        }
    }
    prepared->image = std::move(working);
    return true;
}

bool writePreparedImageExport(const QString &filePath,
                              const PreparedImageExport &prepared,
                              const int quality,
                              QString *errorMessage)
{
    if (!prepared.isValid()) {
        setError(errorMessage,
                 QStringLiteral("The prepared export image is incomplete."));
        return false;
    }
    const ImageExportCapabilities actual = imageExportCapabilitiesForPath(filePath);
    if (!actual.valid || actual.suffix != prepared.capabilities.suffix) {
        setError(errorMessage,
                 QStringLiteral("The destination extension no longer matches the prepared export format."));
        return false;
    }
    if (!imageExportWriterAvailable(actual)) {
        setError(errorMessage,
                 QStringLiteral("The Qt image plugin required to write %1 is unavailable on this system.")
                     .arg(actual.displayName));
        return false;
    }
    if (actual.suffix == QStringLiteral("tga")) {
        return TgaCodec::write(filePath, prepared.image, errorMessage);
    }

    QSaveFile file(filePath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorMessage, file.errorString());
        return false;
    }
    {
        QImageWriter writer(&file, actual.suffix.toLatin1());
        if (actual.supportsQuality) {
            writer.setQuality(std::clamp(quality, 0, 100));
        }
        if (!writer.write(prepared.image)) {
            setError(errorMessage, writer.errorString());
            file.cancelWriting();
            return false;
        }
    }
    if (!file.commit()) {
        setError(errorMessage, file.errorString());
        return false;
    }
    return true;
}

} // namespace vfx
