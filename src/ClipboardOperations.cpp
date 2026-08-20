#include "ClipboardOperations.h"

#include "TransformSampling.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QDataStream>
#include <QCryptographicHash>
#include <QRgba64>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vfx {
namespace {

constexpr qint64 MaximumClipboardBytes = 2ll * 1024ll * 1024ll * 1024ll;
constexpr int MaximumClipboardDimension = 131072;

constexpr qsizetype MaximumClipboardProfileBytes = 16 * 1024 * 1024;

QString clipboardColourSpaceToken(const QColorSpace &colourSpace)
{
    if (colourSpace == QColorSpace(QColorSpace::SRgb)) {
        return QStringLiteral("srgb");
    }
    if (colourSpace == QColorSpace(QColorSpace::SRgbLinear)) {
        return QStringLiteral("linear-srgb");
    }
    return {};
}

QColorSpace restoredClipboardColourSpace(const QString &token,
                                         const QByteArray &profile)
{
    if (token == QStringLiteral("srgb")) {
        return QColorSpace(QColorSpace::SRgb);
    }
    if (token == QStringLiteral("linear-srgb")) {
        return QColorSpace(QColorSpace::SRgbLinear);
    }
    if (!profile.isEmpty()) {
        const QColorSpace colourSpace = QColorSpace::fromIccProfile(profile);
        if (colourSpace.isValid()) {
            return colourSpace;
        }
    }
    return {};
}

int clipboardBytesPerPixel(const QImage::Format format)
{
    switch (format) {
    case QImage::Format_Grayscale8:
        return 1;
    case QImage::Format_Grayscale16:
        return 2;
    case QImage::Format_RGBA8888:
        return 4;
    case QImage::Format_RGBA64:
        return 8;
    default:
        return 0;
    }
}

// QDataStream's built-in QImage operator serialises as PNG, which can
// normalise formats and lose exact RGBA64/hidden-RGB representation. The
// private clipboard therefore stores only active scanline bytes plus guarded
// image metadata and a SHA-256 integrity digest.
bool writeExactClipboardImage(QDataStream &stream, const QImage &image)
{
    stream << !image.isNull();
    if (image.isNull()) {
        return stream.status() == QDataStream::Ok;
    }

    const int bytesPerPixel = clipboardBytesPerPixel(image.format());
    const int rowBytes = image.width() * bytesPerPixel;
    const quint64 byteCount = rowBytes > 0
        ? static_cast<quint64>(rowBytes) * static_cast<quint64>(image.height())
        : 0;
    const QByteArray profile = image.colorSpace().iccProfile();
    if (bytesPerPixel == 0 || rowBytes <= 0 || image.bytesPerLine() < rowBytes
        || byteCount == 0 || byteCount > static_cast<quint64>(MaximumClipboardBytes)
        || profile.size() > MaximumClipboardProfileBytes) {
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < image.height(); ++y) {
        hash.addData(QByteArrayView(
            reinterpret_cast<const char *>(image.constScanLine(y)), rowBytes));
    }
    const QByteArray digest = hash.result();

    stream << static_cast<qint32>(image.width())
           << static_cast<qint32>(image.height())
           << static_cast<qint32>(image.format())
           << static_cast<qint32>(rowBytes)
           << static_cast<qint32>(image.dotsPerMeterX())
           << static_cast<qint32>(image.dotsPerMeterY())
           << image.devicePixelRatio()
           << clipboardColourSpaceToken(image.colorSpace())
           << profile
           << byteCount
           << digest;
    if (stream.status() != QDataStream::Ok) {
        return false;
    }
    for (int y = 0; y < image.height(); ++y) {
        if (stream.writeRawData(
                reinterpret_cast<const char *>(image.constScanLine(y)),
                rowBytes) != rowBytes) {
            return false;
        }
    }
    return stream.status() == QDataStream::Ok;
}

bool readExactClipboardImage(QDataStream &stream,
                             QImage *image,
                             QString *errorMessage)
{
    bool present = false;
    stream >> present;
    if (stream.status() != QDataStream::Ok) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The private clipboard ended before an image header.");
        }
        return false;
    }
    if (!present) {
        *image = {};
        return true;
    }

    qint32 width = 0;
    qint32 height = 0;
    qint32 formatValue = 0;
    qint32 serializedRowBytes = 0;
    qint32 dotsPerMeterX = 0;
    qint32 dotsPerMeterY = 0;
    double devicePixelRatio = 1.0;
    QString colourToken;
    QByteArray profile;
    quint64 byteCount = 0;
    QByteArray expectedDigest;
    stream >> width >> height >> formatValue >> serializedRowBytes
           >> dotsPerMeterX >> dotsPerMeterY >> devicePixelRatio
           >> colourToken >> profile >> byteCount >> expectedDigest;
    if (stream.status() != QDataStream::Ok
        || width < 1 || height < 1
        || width > MaximumClipboardDimension || height > MaximumClipboardDimension
        || serializedRowBytes < 1
        || byteCount == 0 || byteCount > static_cast<quint64>(MaximumClipboardBytes)
        || profile.size() > MaximumClipboardProfileBytes
        || expectedDigest.size() != 32) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The private clipboard contains an invalid image header.");
        }
        return false;
    }

    const QImage::Format format = static_cast<QImage::Format>(formatValue);
    const int bytesPerPixel = clipboardBytesPerPixel(format);
    const quint64 expectedRowBytes = bytesPerPixel > 0
        ? static_cast<quint64>(width) * static_cast<quint64>(bytesPerPixel)
        : 0;
    const quint64 expectedByteCount = expectedRowBytes * static_cast<quint64>(height);
    if (bytesPerPixel == 0
        || serializedRowBytes != static_cast<qint32>(expectedRowBytes)
        || byteCount != expectedByteCount
        || !stream.device()
        || stream.device()->bytesAvailable() < static_cast<qint64>(byteCount)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The private clipboard image layout is inconsistent.");
        }
        return false;
    }

    QImage restored(width, height, format);
    if (restored.isNull() || restored.bytesPerLine() < serializedRowBytes) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not allocate the private clipboard image.");
        }
        return false;
    }
    restored.fill(0);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < height; ++y) {
        char *row = reinterpret_cast<char *>(restored.scanLine(y));
        if (stream.readRawData(row, serializedRowBytes) != serializedRowBytes) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("The private clipboard image is incomplete.");
            }
            return false;
        }
        hash.addData(QByteArrayView(row, serializedRowBytes));
    }
    if (hash.result() != expectedDigest) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The private clipboard image failed its integrity check.");
        }
        return false;
    }

    restored.setDotsPerMeterX(std::max(0, dotsPerMeterX));
    restored.setDotsPerMeterY(std::max(0, dotsPerMeterY));
    if (std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0) {
        restored.setDevicePixelRatio(devicePixelRatio);
    }
    const QColorSpace colourSpace = restoredClipboardColourSpace(colourToken, profile);
    if ((!colourToken.isEmpty() || !profile.isEmpty()) && !colourSpace.isValid()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The private clipboard colour profile is invalid.");
        }
        return false;
    }
    if (colourSpace.isValid()) {
        restored.setColorSpace(colourSpace);
    }
    *image = std::move(restored);
    return true;
}

bool validImageForClipboard(const QImage &image)
{
    return !image.isNull()
        && image.width() > 0 && image.height() > 0
        && image.width() <= MaximumClipboardDimension
        && image.height() <= MaximumClipboardDimension
        && static_cast<qint64>(image.sizeInBytes()) <= MaximumClipboardBytes;
}

SelectionMask restoredSelection(const SelectionMask::Snapshot &snapshot)
{
    SelectionMask selection(snapshot.size);
    if (!snapshot.size.isEmpty()) {
        selection.restoreSnapshot(snapshot, false);
    }
    return selection;
}

quint16 mix16(const quint16 a, const quint16 b, const double t)
{
    return static_cast<quint16>(std::clamp(
        qRound(a + (static_cast<double>(b) - a) * t), 0, 65535));
}

quint8 mix8(const quint8 a, const quint8 b, const double t)
{
    return static_cast<quint8>(std::clamp(
        qRound(a + (static_cast<double>(b) - a) * t), 0, 255));
}

QRgba64 sampleRgba64(const QImage &rgba64,
                     const QSize &localExtent,
                     const QPointF &localPoint,
                     const TransformInterpolation interpolation =
                         TransformInterpolation::Bilinear)
{
    return sampleTransformRgba64(rgba64,
                                 localExtent,
                                 localPoint,
                                 interpolation);
}

quint16 sampleGrey16(const QImage &grey16,
                     const QSize &localExtent,
                     const QPointF &localPoint,
                     const bool compactConstant,
                     const TransformInterpolation interpolation =
                         TransformInterpolation::Bilinear)
{
    return sampleTransformGrey16(grey16,
                                 localExtent,
                                 localPoint,
                                 interpolation,
                                 0,
                                 compactConstant);
}

QImage grayscaleValuesFromSource(const QImage &source,
                                 const ClipboardSourceKind sourceKind,
                                 const int channelIndex)
{
    if (source.isNull()) {
        return {};
    }
    if (sourceKind == ClipboardSourceKind::Mask) {
        return source.convertToFormat(QImage::Format_Grayscale16);
    }

    const QImage rgba = source.convertToFormat(QImage::Format_RGBA64);
    QImage values(source.size(), QImage::Format_Grayscale16);
    if (rgba.isNull() || values.isNull()) {
        return {};
    }
    values.setColorSpace(source.colorSpace());
    for (int y = 0; y < rgba.height(); ++y) {
        const auto *src = reinterpret_cast<const QRgba64 *>(rgba.constScanLine(y));
        auto *dst = reinterpret_cast<quint16 *>(values.scanLine(y));
        for (int x = 0; x < rgba.width(); ++x) {
            if (channelIndex >= 0 && channelIndex <= 3) {
                switch (channelIndex) {
                case 0: dst[x] = src[x].red(); break;
                case 1: dst[x] = src[x].green(); break;
                case 2: dst[x] = src[x].blue(); break;
                case 3: dst[x] = src[x].alpha(); break;
                default: break;
                }
            } else {
                dst[x] = static_cast<quint16>((src[x].red() * 299ull
                                               + src[x].green() * 587ull
                                               + src[x].blue() * 114ull + 500ull)
                                              / 1000ull);
            }
        }
    }
    return values;
}

quint8 selectionCoverage(const SelectionMask &selection,
                         const bool selectionActive,
                         const int x,
                         const int y)
{
    return selectionActive ? selection.coverageAt(x, y) : 255;
}

QImage materialisedTargetMask(const QImage &mask, const QSize &extent)
{
    if (mask.isNull() || extent.isEmpty()) {
        return {};
    }
    if (mask.size() == QSize(1, 1)) {
        QImage expanded(extent, QImage::Format_Grayscale8);
        expanded.fill(qGray(mask.pixel(0, 0)));
        return expanded;
    }
    return mask.size() == extent
        ? mask.convertToFormat(QImage::Format_Grayscale8)
        : mask.scaled(extent, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
              .convertToFormat(QImage::Format_Grayscale8);
}

QImage compactUniformMask(QImage mask)
{
    if (mask.isNull()) {
        return {};
    }
    mask = mask.convertToFormat(QImage::Format_Grayscale8);
    const uchar first = mask.constScanLine(0)[0];
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            if (row[x] != first) {
                return mask;
            }
        }
    }
    QImage compact(1, 1, QImage::Format_Grayscale8);
    compact.fill(first);
    return compact;
}

QImage straightTargetRgba(const QImage &image,
                          const QSize &extent,
                          const bool sixteenBit)
{
    const QImage::Format format = sixteenBit
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    if (image.isNull()) {
        QImage output(extent, format);
        output.fill(Qt::transparent);
        return output;
    }
    return image.size() == extent
        ? image.convertToFormat(format)
        : image.scaled(extent, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
              .convertToFormat(format);
}

bool inPayload(const ClipboardPayload &payload, const QPointF &documentPoint)
{
    return documentPoint.x() >= payload.documentBounds.left()
        && documentPoint.y() >= payload.documentBounds.top()
        && documentPoint.x() < payload.documentBounds.right() + 1.0
        && documentPoint.y() < payload.documentBounds.bottom() + 1.0;
}

} // namespace

bool ClipboardPayload::isValid() const
{
    if (!validImageForClipboard(image)
        || documentBounds.size() != image.size()
        || documentBounds.isEmpty()) {
        return false;
    }
    if (hasDocumentPlacement
        && (sourceDocumentSize.isEmpty()
            || !QRect(QPoint(0, 0), sourceDocumentSize).contains(documentBounds))) {
        return false;
    }
    if (imageKind == ClipboardImageKind::Rgba && !coverage.isNull()) {
        return false;
    }
    if (imageKind == ClipboardImageKind::Grayscale
        && !coverage.isNull()
        && (!validImageForClipboard(coverage) || coverage.size() != image.size())) {
        return false;
    }
    const qint64 imageBytes = static_cast<qint64>(image.sizeInBytes());
    const qint64 coverageBytes = coverage.isNull()
        ? 0 : static_cast<qint64>(coverage.sizeInBytes());
    return imageBytes >= 0 && coverageBytes >= 0
        && imageBytes <= MaximumClipboardBytes - coverageBytes;
}

QByteArray ClipboardPayload::toBytes(bool *ok) const
{
    if (ok) {
        *ok = false;
    }
    if (!isValid()) {
        return {};
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {};
    }
    QDataStream stream(&buffer);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << Magic << FormatVersion
           << static_cast<quint8>(imageKind)
           << static_cast<quint8>(sourceKind)
           << documentBounds << sourceDocumentSize
           << hasDocumentPlacement << sourceName;
    if (stream.status() != QDataStream::Ok
        || !writeExactClipboardImage(stream, image)
        || !writeExactClipboardImage(stream, coverage)
        || bytes.size() > MaximumClipboardBytes) {
        return {};
    }
    if (ok) {
        *ok = true;
    }
    return bytes;
}

std::optional<ClipboardPayload> ClipboardPayload::fromBytes(
    const QByteArray &bytes,
    QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (bytes.isEmpty() || bytes.size() > MaximumClipboardBytes) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The private clipboard payload is empty or too large.");
        }
        return std::nullopt;
    }
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QDataStream stream(&buffer);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint16 version = 0;
    quint8 imageKindValue = 0;
    quint8 sourceKindValue = 0;
    ClipboardPayload payload;
    stream >> magic >> version >> imageKindValue >> sourceKindValue
           >> payload.documentBounds >> payload.sourceDocumentSize
           >> payload.hasDocumentPlacement >> payload.sourceName;
    if (stream.status() != QDataStream::Ok
        || magic != Magic || version != FormatVersion
        || imageKindValue > static_cast<quint8>(ClipboardImageKind::Grayscale)
        || sourceKindValue > static_cast<quint8>(ClipboardSourceKind::ExternalImage)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The private clipboard payload is invalid or unsupported.");
        }
        return std::nullopt;
    }
    if (!readExactClipboardImage(stream, &payload.image, errorMessage)
        || !readExactClipboardImage(stream, &payload.coverage, errorMessage)
        || stream.status() != QDataStream::Ok
        || !stream.atEnd()) {
        if (errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("The private clipboard image data is invalid.");
        }
        return std::nullopt;
    }
    payload.imageKind = static_cast<ClipboardImageKind>(imageKindValue);
    payload.sourceKind = static_cast<ClipboardSourceKind>(sourceKindValue);
    if (!payload.isValid()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The private clipboard image data failed validation.");
        }
        return std::nullopt;
    }
    return payload;
}

QImage ClipboardPayload::interoperabilityImage() const
{
    if (!isValid()) {
        return {};
    }
    if (imageKind == ClipboardImageKind::Rgba) {
        return image.format() == QImage::Format_RGBA8888
            ? image : image.convertToFormat(QImage::Format_RGBA8888);
    }

    const QImage values = image.convertToFormat(QImage::Format_Grayscale16);
    const QImage alpha = coverage.isNull()
        ? QImage() : coverage.convertToFormat(QImage::Format_Grayscale16);
    QImage output(image.size(), QImage::Format_RGBA8888);
    output.setColorSpace(image.colorSpace());
    for (int y = 0; y < output.height(); ++y) {
        const auto *valueRow = reinterpret_cast<const quint16 *>(values.constScanLine(y));
        const quint16 *alphaRow = alpha.isNull()
            ? nullptr : reinterpret_cast<const quint16 *>(alpha.constScanLine(y));
        uchar *dst = output.scanLine(y);
        for (int x = 0; x < output.width(); ++x) {
            const uchar value = static_cast<uchar>((valueRow[x] + 128) / 257);
            dst[x * 4] = value;
            dst[x * 4 + 1] = value;
            dst[x * 4 + 2] = value;
            dst[x * 4 + 3] = alphaRow
                ? static_cast<uchar>((alphaRow[x] + 128) / 257) : 255;
        }
    }
    return output;
}

ClipboardPayload extractClipboardPayload(const QImage &sourceImage,
                                         const QSize &localExtent,
                                         const QTransform &localToDocument,
                                         const QSize &documentSize,
                                         const SelectionMask::Snapshot &selectionSnapshot,
                                         const ClipboardSourceKind sourceKind,
                                         const int channelIndex,
                                         const QString &sourceName)
{
    ClipboardPayload payload;
    payload.sourceKind = sourceKind;
    payload.sourceDocumentSize = documentSize;
    payload.hasDocumentPlacement = true;
    payload.sourceName = sourceName;
    if (sourceImage.isNull() || localExtent.isEmpty() || documentSize.isEmpty()) {
        return payload;
    }

    bool invertible = false;
    const QTransform documentToLocal = localToDocument.inverted(&invertible);
    if (!invertible) {
        return payload;
    }
    const SelectionMask selection = restoredSelection(selectionSnapshot);
    const bool selectionActive = selectionSnapshot.active;
    if (selectionActive && selection.isEmpty()) {
        return payload;
    }
    const QRect documentRect(QPoint(0, 0), documentSize);
    QRect bounds = selectionActive
        ? selection.nonZeroBounds().intersected(documentRect)
        : localToDocument.mapRect(QRectF(QPointF(0, 0), QSizeF(localExtent)))
              .toAlignedRect().intersected(documentRect);
    if (bounds.isEmpty()) {
        return payload;
    }
    payload.documentBounds = bounds;

    const bool rgbaPayload = sourceKind == ClipboardSourceKind::RasterPixels
        || sourceKind == ClipboardSourceKind::Composite
        || sourceKind == ClipboardSourceKind::ExternalImage;
    const bool completeSelectionCoverage = !selectionActive || selection.isFull();
    if (rgbaPayload
        && completeSelectionCoverage
        && localToDocument.isIdentity()
        && localExtent == documentSize
        && sourceImage.size() == documentSize
        && bounds == documentRect) {
        // Select All on an untransformed full-document raster is the dominant
        // photo workflow. Preserve Qt's implicit sharing instead of resampling
        // every pixel through a transform and selection lookup. Conversion is
        // likewise shallow when the source already uses our straight format.
        payload.imageKind = ClipboardImageKind::Rgba;
        payload.image = sourceImage.convertToFormat(
            sourceImage.depth() > 32 ? QImage::Format_RGBA64
                                     : QImage::Format_RGBA8888);
        return payload;
    }
    if (rgbaPayload) {
        payload.imageKind = ClipboardImageKind::Rgba;
        const bool sixteenBit = sourceImage.depth() > 32;
        const QImage source = sourceImage.convertToFormat(QImage::Format_RGBA64);
        QImage output(bounds.size(), sixteenBit
                          ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        output.setColorSpace(sourceImage.colorSpace());
        for (int y = 0; y < bounds.height(); ++y) {
            const int documentY = bounds.y() + y;
            for (int x = 0; x < bounds.width(); ++x) {
                const int documentX = bounds.x() + x;
                const QPointF localPoint = documentToLocal.map(
                    QPointF(documentX + 0.5, documentY + 0.5));
                QRgba64 pixel = sampleRgba64(source, localExtent, localPoint);
                const quint8 coverage = selectionCoverage(selection,
                                                           selectionActive,
                                                           documentX,
                                                           documentY);
                pixel = QRgba64::fromRgba64(
                    pixel.red(), pixel.green(), pixel.blue(),
                    static_cast<quint16>((pixel.alpha() * coverage + 127) / 255));
                if (sixteenBit) {
                    reinterpret_cast<QRgba64 *>(output.scanLine(y))[x] = pixel;
                } else {
                    uchar *dst = output.scanLine(y) + x * 4;
                    dst[0] = static_cast<uchar>((pixel.red() + 128) / 257);
                    dst[1] = static_cast<uchar>((pixel.green() + 128) / 257);
                    dst[2] = static_cast<uchar>((pixel.blue() + 128) / 257);
                    dst[3] = static_cast<uchar>((pixel.alpha() + 128) / 257);
                }
            }
        }
        payload.image = output;
        return payload;
    }

    payload.imageKind = ClipboardImageKind::Grayscale;
    const bool sixteenBit = sourceImage.depth() > 32
        && sourceKind != ClipboardSourceKind::Mask;
    const QImage values = grayscaleValuesFromSource(sourceImage, sourceKind, channelIndex);
    if (values.isNull()) {
        return {};
    }
    QImage output(bounds.size(), sixteenBit
                      ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8);
    QImage coverage(bounds.size(), QImage::Format_Grayscale8);
    output.setColorSpace(sourceImage.colorSpace());
    const bool compactConstant = sourceKind == ClipboardSourceKind::Mask
        && sourceImage.size() == QSize(1, 1);
    for (int y = 0; y < bounds.height(); ++y) {
        const int documentY = bounds.y() + y;
        for (int x = 0; x < bounds.width(); ++x) {
            const int documentX = bounds.x() + x;
            const QPointF localPoint = documentToLocal.map(
                QPointF(documentX + 0.5, documentY + 0.5));
            const bool localInside = localPoint.x() >= 0.0 && localPoint.y() >= 0.0
                && localPoint.x() < localExtent.width()
                && localPoint.y() < localExtent.height();
            const quint16 value = sampleGrey16(values,
                                               localExtent,
                                               localPoint,
                                               compactConstant);
            const quint8 applicationCoverage = localInside
                ? selectionCoverage(selection, selectionActive, documentX, documentY)
                : 0;
            coverage.scanLine(y)[x] = applicationCoverage;
            if (sixteenBit) {
                reinterpret_cast<quint16 *>(output.scanLine(y))[x] = value;
            } else {
                output.scanLine(y)[x] = static_cast<uchar>((value + 128) / 257);
            }
        }
    }
    payload.image = output;
    payload.coverage = coverage;
    return payload;
}

QImage clipboardPayloadAsRaster(const ClipboardPayload &payload,
                                const QColorSpace &targetColourSpace,
                                const bool targetSixteenBit,
                                const bool targetGrayscale)
{
    if (!payload.isValid()) {
        return {};
    }
    const QImage::Format format = targetSixteenBit
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    if (payload.imageKind == ClipboardImageKind::Rgba) {
        QImage output = payload.image;
        if (output.colorSpace().isValid() && targetColourSpace.isValid()
            && output.colorSpace() != targetColourSpace) {
            output.convertToColorSpace(targetColourSpace);
        }
        output = output.convertToFormat(format);
        if (targetColourSpace.isValid()
            && output.colorSpace() != targetColourSpace) {
            output.setColorSpace(targetColourSpace);
        }
        if (targetGrayscale) {
            if (targetSixteenBit) {
                for (int y = 0; y < output.height(); ++y) {
                    auto *row = reinterpret_cast<QRgba64 *>(output.scanLine(y));
                    for (int x = 0; x < output.width(); ++x) {
                        const quint16 value = static_cast<quint16>((row[x].red() * 299ull
                                                                   + row[x].green() * 587ull
                                                                   + row[x].blue() * 114ull + 500ull)
                                                                  / 1000ull);
                        row[x] = QRgba64::fromRgba64(value, value, value, row[x].alpha());
                    }
                }
            } else {
                for (int y = 0; y < output.height(); ++y) {
                    uchar *row = output.scanLine(y);
                    for (int x = 0; x < output.width(); ++x) {
                        uchar *pixel = row + x * 4;
                        const uchar value = static_cast<uchar>((pixel[0] * 299
                                                                + pixel[1] * 587
                                                                + pixel[2] * 114 + 500)
                                                               / 1000);
                        pixel[0] = value;
                        pixel[1] = value;
                        pixel[2] = value;
                    }
                }
            }
        }
        return output;
    }

    const QImage values = payload.image.convertToFormat(QImage::Format_Grayscale16);
    const QImage coverage = payload.coverage.isNull()
        ? QImage() : payload.coverage.convertToFormat(QImage::Format_Grayscale16);
    QImage output(payload.image.size(), format);
    output.setColorSpace(targetColourSpace);
    for (int y = 0; y < output.height(); ++y) {
        const auto *valueRow = reinterpret_cast<const quint16 *>(values.constScanLine(y));
        const quint16 *coverageRow = coverage.isNull()
            ? nullptr : reinterpret_cast<const quint16 *>(coverage.constScanLine(y));
        for (int x = 0; x < output.width(); ++x) {
            const quint16 value = valueRow[x];
            const quint16 alpha = coverageRow ? coverageRow[x] : 65535;
            if (targetSixteenBit) {
                reinterpret_cast<QRgba64 *>(output.scanLine(y))[x]
                    = QRgba64::fromRgba64(value, value, value, alpha);
            } else {
                uchar *dst = output.scanLine(y) + x * 4;
                dst[0] = static_cast<uchar>((value + 128) / 257);
                dst[1] = dst[0];
                dst[2] = dst[0];
                dst[3] = static_cast<uchar>((alpha + 128) / 257);
            }
        }
    }
    return output;
}

QImage clipboardPayloadAsNewDocumentRaster(const ClipboardPayload &payload,
                                           QString *errorMessage)
{
    const QColorSpace compatibilitySpace = payload.image.colorSpace().isValid()
        ? payload.image.colorSpace()
        : QColorSpace(QColorSpace::SRgb);
    return clipboardPayloadAsNewDocumentRaster(payload,
                                               compatibilitySpace,
                                               errorMessage);
}

QImage clipboardPayloadAsNewDocumentRaster(const ClipboardPayload &payload,
                                           const QColorSpace &effectiveColourSpace,
                                           QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (!payload.isValid()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The clipboard image data is invalid.");
        }
        return {};
    }
    constexpr int MaximumDocumentDimension = 32768;
    if (payload.image.width() < 1 || payload.image.height() < 1
        || payload.image.width() > MaximumDocumentDimension
        || payload.image.height() > MaximumDocumentDimension) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Clipboard image dimensions must be between 1 and 32768 pixels.");
        }
        return {};
    }

    const bool sixteenBit = payload.image.format() == QImage::Format_RGBA64
        || payload.image.format() == QImage::Format_Grayscale16
        || payload.image.depth() > 32;
    QImage output = clipboardPayloadAsRaster(payload,
                                             effectiveColourSpace,
                                             sixteenBit,
                                             false);
    if (output.isNull()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The clipboard image could not be prepared as a new document.");
        }
        return {};
    }
    output.setColorSpace(effectiveColourSpace);
    output.setDotsPerMeterX(std::max(0, payload.image.dotsPerMeterX()));
    output.setDotsPerMeterY(std::max(0, payload.image.dotsPerMeterY()));
    // A new document is pixel-addressed; clipboard device-pixel ratios must
    // not make the same stored raster appear to have different dimensions.
    output.setDevicePixelRatio(1.0);
    return output;
}

QImage clipboardPayloadAsDocumentRaster(const ClipboardPayload &payload,
                                        const QColorSpace &targetColourSpace,
                                        const bool targetSixteenBit,
                                        const bool targetGrayscale,
                                        const QSize &documentSize)
{
    if (!payload.isValid() || documentSize.isEmpty()) {
        return {};
    }
    const QImage cropped = clipboardPayloadAsRaster(payload,
                                                    targetColourSpace,
                                                    targetSixteenBit,
                                                    targetGrayscale);
    if (cropped.isNull()) {
        return {};
    }
    const QImage::Format format = targetSixteenBit
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
    const QRect fullDocumentRect(QPoint(0, 0), documentSize);
    if (payload.documentBounds == fullDocumentRect
        && cropped.size() == documentSize
        && cropped.format() == format) {
        // Full-image Paste can hand the implicitly shared straight raster
        // directly to the new layer. Avoid allocating and copying another
        // 35–70 MB document-sized buffer before history is built.
        return cropped;
    }
    QImage output(documentSize, format);
    output.fill(Qt::transparent);
    output.setColorSpace(targetColourSpace);

    const QRect destination = payload.documentBounds.intersected(
        QRect(QPoint(0, 0), documentSize));
    if (destination.isEmpty()) {
        return output;
    }
    const QPoint sourceOffset = destination.topLeft() - payload.documentBounds.topLeft();
    const int bytesPerPixel = targetSixteenBit ? 8 : 4;
    const int rowBytes = destination.width() * bytesPerPixel;
    for (int y = 0; y < destination.height(); ++y) {
        const uchar *source = cropped.constScanLine(sourceOffset.y() + y)
            + sourceOffset.x() * bytesPerPixel;
        uchar *target = output.scanLine(destination.y() + y)
            + destination.x() * bytesPerPixel;
        std::memcpy(target, source, static_cast<size_t>(rowBytes));
    }
    return output;
}

ClipboardPasteResult pasteClipboardIntoTarget(
    const ClipboardPayload &payload,
    const QImage &targetImage,
    const QSize &targetExtent,
    const QTransform &targetToDocument,
    const QSize &documentSize,
    const ClipboardPasteTarget target,
    const SelectionMask::Snapshot &destinationSelectionSnapshot,
    const bool preferSixteenBit,
    const QTransform &payloadDocumentTransform,
    const TransformInterpolation interpolation)
{
    ClipboardPasteResult result;
    if (!payload.isValid() || targetExtent.isEmpty() || documentSize.isEmpty()) {
        return result;
    }
    bool invertible = false;
    const QTransform documentToTarget = targetToDocument.inverted(&invertible);
    if (!invertible) {
        return result;
    }
    bool payloadTransformInvertible = false;
    const QTransform documentToPayload = payloadDocumentTransform.inverted(
        &payloadTransformInvertible);
    if (!payloadTransformInvertible) {
        return result;
    }
    const SelectionMask destinationSelection = restoredSelection(destinationSelectionSnapshot);
    if (destinationSelectionSnapshot.active && destinationSelection.isEmpty()) {
        result.image = targetImage;
        result.succeeded = true;
        return result;
    }
    const QImage payloadRgba64 = payload.imageKind == ClipboardImageKind::Rgba
        ? payload.image.convertToFormat(QImage::Format_RGBA64) : QImage();
    const QImage payloadGrey16 = payload.imageKind == ClipboardImageKind::Grayscale
        ? payload.image.convertToFormat(QImage::Format_Grayscale16) : QImage();
    const QImage payloadCoverage16 = payload.imageKind == ClipboardImageKind::Grayscale
        && !payload.coverage.isNull()
        ? payload.coverage.convertToFormat(QImage::Format_Grayscale16) : QImage();
    if ((payload.imageKind == ClipboardImageKind::Rgba && payloadRgba64.isNull())
        || (payload.imageKind == ClipboardImageKind::Grayscale
            && payloadGrey16.isNull())
        || (payload.imageKind == ClipboardImageKind::Grayscale
            && !payload.coverage.isNull() && payloadCoverage16.isNull())) {
        return result;
    }

    const QRect localRect(QPoint(0, 0), targetExtent);
    const QRectF transformedPayloadBounds = payloadDocumentTransform.mapRect(
        QRectF(payload.documentBounds));
    const QRect candidate = documentToTarget.mapRect(transformedPayloadBounds)
                                .toAlignedRect().adjusted(-2, -2, 2, 2)
                                .intersected(localRect);
    if (candidate.isEmpty()) {
        result.image = targetImage;
        result.succeeded = true;
        return result;
    }

    const bool maskTarget = target == ClipboardPasteTarget::Mask;
    const bool targetSixteenBit = !maskTarget
        && (targetImage.depth() > 32 || preferSixteenBit);
    QImage output = maskTarget
        ? materialisedTargetMask(targetImage, targetExtent)
        : straightTargetRgba(targetImage, targetExtent, targetSixteenBit);
    if (output.isNull()) {
        return result;
    }
    output.setColorSpace(targetImage.colorSpace());
    result.image = output;
    result.succeeded = true;

    int left = candidate.right() + 1;
    int top = candidate.bottom() + 1;
    int right = candidate.left() - 1;
    int bottom = candidate.top() - 1;
    for (int y = candidate.top(); y <= candidate.bottom(); ++y) {
        for (int x = candidate.left(); x <= candidate.right(); ++x) {
            const QPointF documentPoint = targetToDocument.map(QPointF(x + 0.5, y + 0.5));
            const QPointF payloadDocumentPoint = documentToPayload.map(documentPoint);
            if (!inPayload(payload, payloadDocumentPoint)) {
                continue;
            }
            const double sourceX = payloadDocumentPoint.x() - payload.documentBounds.x() - 0.5;
            const double sourceY = payloadDocumentPoint.y() - payload.documentBounds.y() - 0.5;
            const QPointF sourcePoint(sourceX + 0.5, sourceY + 0.5);
            const quint16 payloadCoverage = payload.imageKind == ClipboardImageKind::Rgba
                ? sampleRgba64(payloadRgba64, payload.image.size(), sourcePoint, interpolation).alpha()
                : payloadCoverage16.isNull()
                    ? 65535
                    : sampleGrey16(payloadCoverage16,
                                   payload.coverage.size(),
                                   sourcePoint,
                                   payload.coverage.size() == QSize(1, 1),
                                   interpolation);
            double application = payloadCoverage / 65535.0;
            if (destinationSelectionSnapshot.active) {
                const int docX = static_cast<int>(std::floor(documentPoint.x()));
                const int docY = static_cast<int>(std::floor(documentPoint.y()));
                application *= destinationSelection.coverageAt(docX, docY) / 255.0;
            }
            if (application <= 0.0) {
                continue;
            }
            const quint16 value = payload.imageKind == ClipboardImageKind::Grayscale
                ? sampleGrey16(payloadGrey16,
                               payload.image.size(),
                               sourcePoint,
                               payload.image.size() == QSize(1, 1),
                               interpolation)
                : [&] {
                    const QRgba64 pixel = sampleRgba64(payloadRgba64,
                                                       payload.image.size(),
                                                       sourcePoint,
                                                       interpolation);
                    return static_cast<quint16>((pixel.red() * 299ull
                                                + pixel.green() * 587ull
                                                + pixel.blue() * 114ull + 500ull)
                                               / 1000ull);
                }();
            bool changed = false;
            if (maskTarget) {
                uchar *row = output.scanLine(y);
                const quint8 before = row[x];
                const quint8 after = mix8(before,
                                          static_cast<quint8>((value + 128) / 257),
                                          application);
                if (before != after) {
                    row[x] = after;
                    changed = true;
                }
            } else if (targetSixteenBit) {
                auto *row = reinterpret_cast<QRgba64 *>(output.scanLine(y));
                const QRgba64 before = row[x];
                quint16 r = before.red();
                quint16 g = before.green();
                quint16 b = before.blue();
                quint16 a = before.alpha();
                switch (target) {
                case ClipboardPasteTarget::GreyChannel:
                    r = mix16(r, value, application);
                    g = mix16(g, value, application);
                    b = mix16(b, value, application);
                    break;
                case ClipboardPasteTarget::RedChannel: r = mix16(r, value, application); break;
                case ClipboardPasteTarget::GreenChannel: g = mix16(g, value, application); break;
                case ClipboardPasteTarget::BlueChannel: b = mix16(b, value, application); break;
                case ClipboardPasteTarget::AlphaChannel: a = mix16(a, value, application); break;
                case ClipboardPasteTarget::Mask: break;
                }
                const QRgba64 after = QRgba64::fromRgba64(r, g, b, a);
                if (before != after) {
                    row[x] = after;
                    changed = true;
                }
            } else {
                uchar *pixel = output.scanLine(y) + x * 4;
                const uchar targetValue = static_cast<uchar>((value + 128) / 257);
                switch (target) {
                case ClipboardPasteTarget::GreyChannel:
                    for (int component = 0; component < 3; ++component) {
                        const uchar before = pixel[component];
                        const uchar after = mix8(before, targetValue, application);
                        pixel[component] = after;
                        changed = changed || before != after;
                    }
                    break;
                case ClipboardPasteTarget::RedChannel: {
                    const uchar before = pixel[0];
                    pixel[0] = mix8(before, targetValue, application);
                    changed = before != pixel[0];
                    break;
                }
                case ClipboardPasteTarget::GreenChannel: {
                    const uchar before = pixel[1];
                    pixel[1] = mix8(before, targetValue, application);
                    changed = before != pixel[1];
                    break;
                }
                case ClipboardPasteTarget::BlueChannel: {
                    const uchar before = pixel[2];
                    pixel[2] = mix8(before, targetValue, application);
                    changed = before != pixel[2];
                    break;
                }
                case ClipboardPasteTarget::AlphaChannel: {
                    const uchar before = pixel[3];
                    pixel[3] = mix8(before, targetValue, application);
                    changed = before != pixel[3];
                    break;
                }
                case ClipboardPasteTarget::Mask:
                    break;
                }
            }
            if (changed) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    }

    if (right < left || bottom < top) {
        result.image = output;
        return result;
    }
    if (maskTarget) {
        output = compactUniformMask(output);
    }
    result.image = output;
    result.affectedRect = QRect(QPoint(left, top), QPoint(right, bottom));
    result.changed = true;
    return result;
}


ClipboardPasteResult pasteClipboardIntoRasterTarget(
    const ClipboardPayload &payload,
    const QImage &targetImage,
    const QSize &targetExtent,
    const QTransform &targetToDocument,
    const QSize &documentSize,
    const bool preferSixteenBit,
    const QTransform &payloadDocumentTransform,
    const TransformInterpolation interpolation)
{
    ClipboardPasteResult result;
    if (!payload.isValid() || payload.imageKind != ClipboardImageKind::Rgba
        || targetExtent.isEmpty() || documentSize.isEmpty()) {
        return result;
    }
    bool targetInvertible = false;
    const QTransform documentToTarget = targetToDocument.inverted(&targetInvertible);
    bool payloadInvertible = false;
    const QTransform documentToPayload = payloadDocumentTransform.inverted(
        &payloadInvertible);
    if (!targetInvertible || !payloadInvertible) {
        return result;
    }

    QImage payloadImage = payload.image;
    if (payloadImage.colorSpace().isValid() && targetImage.colorSpace().isValid()
        && payloadImage.colorSpace() != targetImage.colorSpace()) {
        payloadImage.convertToColorSpace(targetImage.colorSpace());
    }
    const QImage payloadRgba64 = payloadImage.convertToFormat(QImage::Format_RGBA64);
    const bool targetSixteenBit = targetImage.depth() > 32 || preferSixteenBit;
    QImage output = straightTargetRgba(targetImage, targetExtent, targetSixteenBit);
    if (payloadRgba64.isNull() || output.isNull()) {
        return result;
    }
    output.setColorSpace(targetImage.colorSpace().isValid()
                             ? targetImage.colorSpace()
                             : payloadImage.colorSpace());
    result.image = output;
    result.succeeded = true;

    const QRect localRect(QPoint(0, 0), targetExtent);
    const QRectF transformedPayloadBounds = payloadDocumentTransform.mapRect(
        QRectF(payload.documentBounds));
    const QRect candidate = documentToTarget.mapRect(transformedPayloadBounds)
                                .toAlignedRect().adjusted(-2, -2, 2, 2)
                                .intersected(localRect);
    if (candidate.isEmpty()) {
        return result;
    }

    int left = candidate.right() + 1;
    int top = candidate.bottom() + 1;
    int right = candidate.left() - 1;
    int bottom = candidate.top() - 1;
    for (int y = candidate.top(); y <= candidate.bottom(); ++y) {
        for (int x = candidate.left(); x <= candidate.right(); ++x) {
            const QPointF documentPoint = targetToDocument.map(
                QPointF(x + 0.5, y + 0.5));
            const QPointF payloadDocumentPoint = documentToPayload.map(documentPoint);
            if (!inPayload(payload, payloadDocumentPoint)) {
                continue;
            }
            const QPointF sourcePoint(
                payloadDocumentPoint.x() - payload.documentBounds.x(),
                payloadDocumentPoint.y() - payload.documentBounds.y());
            const QRgba64 source = sampleRgba64(payloadRgba64,
                                                payload.image.size(),
                                                sourcePoint,
                                                interpolation);
            const double sourceAlpha = source.alpha() / 65535.0;
            bool changed = false;
            if (targetSixteenBit) {
                auto *row = reinterpret_cast<QRgba64 *>(output.scanLine(y));
                const QRgba64 destination = row[x];
                const double destinationAlpha = destination.alpha() / 65535.0;
                const double outputAlpha = sourceAlpha
                    + destinationAlpha * (1.0 - sourceAlpha);
                quint16 red = destination.red();
                quint16 green = destination.green();
                quint16 blue = destination.blue();
                if (outputAlpha > 1.0e-12) {
                    const auto composite = [&](quint16 src, quint16 dst) {
                        const double premultiplied = src / 65535.0 * sourceAlpha
                            + dst / 65535.0 * destinationAlpha
                                * (1.0 - sourceAlpha);
                        return static_cast<quint16>(std::clamp(
                            qRound(premultiplied / outputAlpha * 65535.0),
                            0,
                            65535));
                    };
                    red = composite(source.red(), destination.red());
                    green = composite(source.green(), destination.green());
                    blue = composite(source.blue(), destination.blue());
                } else if (source.red() != 0 || source.green() != 0
                           || source.blue() != 0) {
                    red = source.red();
                    green = source.green();
                    blue = source.blue();
                }
                const QRgba64 after = QRgba64::fromRgba64(
                    red, green, blue,
                    static_cast<quint16>(std::clamp(
                        qRound(outputAlpha * 65535.0), 0, 65535)));
                if (after != destination) {
                    row[x] = after;
                    changed = true;
                }
            } else {
                uchar *pixel = output.scanLine(y) + x * 4;
                const uchar before[4] {pixel[0], pixel[1], pixel[2], pixel[3]};
                const double destinationAlpha = before[3] / 255.0;
                const double outputAlpha = sourceAlpha
                    + destinationAlpha * (1.0 - sourceAlpha);
                const uchar sourceChannels[3] {
                    static_cast<uchar>((source.red() + 128) / 257),
                    static_cast<uchar>((source.green() + 128) / 257),
                    static_cast<uchar>((source.blue() + 128) / 257),
                };
                if (outputAlpha > 1.0e-12) {
                    for (int channel = 0; channel < 3; ++channel) {
                        const double premultiplied = sourceChannels[channel] / 255.0
                                * sourceAlpha
                            + before[channel] / 255.0 * destinationAlpha
                                * (1.0 - sourceAlpha);
                        pixel[channel] = static_cast<uchar>(std::clamp(
                            qRound(premultiplied / outputAlpha * 255.0),
                            0,
                            255));
                    }
                } else if (sourceChannels[0] != 0 || sourceChannels[1] != 0
                           || sourceChannels[2] != 0) {
                    pixel[0] = sourceChannels[0];
                    pixel[1] = sourceChannels[1];
                    pixel[2] = sourceChannels[2];
                }
                pixel[3] = static_cast<uchar>(std::clamp(
                    qRound(outputAlpha * 255.0), 0, 255));
                changed = pixel[0] != before[0] || pixel[1] != before[1]
                    || pixel[2] != before[2] || pixel[3] != before[3];
            }
            if (changed) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < left || bottom < top) {
        result.image = output;
        return result;
    }
    result.image = output;
    result.affectedRect = QRect(QPoint(left, top), QPoint(right, bottom));
    result.changed = true;
    return result;
}

} // namespace vfx
