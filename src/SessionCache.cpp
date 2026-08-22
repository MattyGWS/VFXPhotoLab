#include "SessionCache.h"

#include "ColourResourceAudit.h"
#include "OcioIntegration.h"
#include "TransformSafety.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLockFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace vfx {
namespace {

constexpr char SnapshotMagic[] = {'V', 'F', 'X', 'P', 'L', 'S', 'C', '1'};
constexpr quint32 SnapshotFormatVersion = 28;
constexpr quint32 MaximumLayerCount = 100000;
constexpr quint32 MaximumGuideCount = 1000000;
constexpr quint32 MaximumStringBytes = 1024 * 1024;
constexpr quint32 MaximumAdjustmentBytes = 16 * 1024 * 1024;
constexpr quint32 MaximumVectorBytes = 64 * 1024 * 1024;
constexpr quint32 MaximumTextBytes = 8 * 1024 * 1024;
constexpr quint32 MaximumColourStateBytes = 64 * 1024 * 1024;
constexpr quint32 MaximumSmartSourceBytes = 1024U * 1024U * 1024U;
constexpr int MaximumLayerDepth = 128;
constexpr quint64 MaximumImageBytes = 0xfffffffeULL;
constexpr qsizetype MaximumProfileBytes = 16 * 1024 * 1024;
constexpr qsizetype ImageChunkBytes = 4 * 1024 * 1024;

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

QColorSpace documentWorkingQtSpace(const DocumentColourState &state)
{
    if (state.workingSpace.kind == ColourSpaceKind::Ocio) {
        return ocioQtWorkingSpaceProxy(state.workingSpace);
    }
    return state.workingSpace.toQColorSpace();
}

void setRasterLayerColourSpaceRecursive(QVector<LayerNode> &layers,
                                       const QColorSpace &colourSpace)
{
    for (LayerNode &layer : layers) {
        if (!layer.rasterImage.isNull()) {
            layer.rasterImage.setColorSpace(colourSpace);
        }
        setRasterLayerColourSpaceRecursive(layer.children, colourSpace);
    }
}


bool writeSizedBytes(QDataStream &stream,
                     const QByteArray &bytes,
                     const quint32 maximumSize,
                     QString *errorMessage)
{
    if (static_cast<quint64>(bytes.size()) > maximumSize) {
        setError(errorMessage, QStringLiteral("A session-cache byte field exceeds its safe limit."));
        return false;
    }
    const quint32 size = static_cast<quint32>(bytes.size());
    const int sizeForQt65 = static_cast<int>(size);
    stream << size;
    if (size > 0
        && stream.writeRawData(bytes.constData(), sizeForQt65) != sizeForQt65) {
        setError(errorMessage, QStringLiteral("Could not write a session-cache byte field."));
        return false;
    }
    return stream.status() == QDataStream::Ok;
}

bool readSizedBytes(QDataStream &stream,
                    QByteArray *bytes,
                    const quint32 maximumSize,
                    QString *errorMessage)
{
    quint32 size = 0;
    stream >> size;
    if (stream.status() != QDataStream::Ok || size > maximumSize) {
        setError(errorMessage, QStringLiteral("The session cache contains an invalid byte-field size."));
        return false;
    }
    if (QIODevice *device = stream.device(); device && !device->isSequential()
        && device->bytesAvailable() < static_cast<qint64>(size)) {
        setError(errorMessage, QStringLiteral("The session cache ends inside a byte field."));
        return false;
    }
    QByteArray restored;
    restored.resize(static_cast<qsizetype>(size));
    const int sizeForQt65 = static_cast<int>(size);
    if (size > 0
        && stream.readRawData(restored.data(), sizeForQt65) != sizeForQt65) {
        setError(errorMessage, QStringLiteral("The session cache ends inside a byte field."));
        return false;
    }
    *bytes = std::move(restored);
    return true;
}


bool writeString(QDataStream &stream,
                 const QString &value,
                 QString *errorMessage)
{
    return writeSizedBytes(stream,
                           value.toUtf8(),
                           MaximumStringBytes,
                           errorMessage);
}

bool readString(QDataStream &stream,
                QString *value,
                QString *errorMessage)
{
    QByteArray encoded;
    if (!readSizedBytes(stream, &encoded, MaximumStringBytes, errorMessage)) {
        return false;
    }
    const QString decoded = QString::fromUtf8(encoded);
    if (decoded.toUtf8() != encoded) {
        setError(errorMessage, QStringLiteral("The session cache contains invalid UTF-8 text."));
        return false;
    }
    *value = decoded;
    return true;
}

bool writeGuideVector(QDataStream &stream,
                      const QVector<double> &values,
                      QString *errorMessage)
{
    if (static_cast<quint64>(values.size()) > MaximumGuideCount) {
        setError(errorMessage, QStringLiteral("The session cache contains too many guides."));
        return false;
    }
    stream << static_cast<quint32>(values.size());
    for (const double value : values) {
        stream << value;
    }
    return stream.status() == QDataStream::Ok;
}

bool readGuideVector(QDataStream &stream,
                     QVector<double> *values,
                     QString *errorMessage)
{
    quint32 count = 0;
    stream >> count;
    if (stream.status() != QDataStream::Ok || count > MaximumGuideCount) {
        setError(errorMessage, QStringLiteral("The session cache contains an invalid guide count."));
        return false;
    }
    QVector<double> restored;
    restored.reserve(static_cast<qsizetype>(count));
    for (quint32 index = 0; index < count; ++index) {
        double value = 0.0;
        stream >> value;
        if (stream.status() != QDataStream::Ok) {
            setError(errorMessage, QStringLiteral("The session cache ends inside its guide data."));
            return false;
        }
        restored.push_back(value);
    }
    *values = std::move(restored);
    return true;
}

bool writeUuidVector(QDataStream &stream,
                     const QVector<QUuid> &values,
                     QString *errorMessage)
{
    if (static_cast<quint64>(values.size()) > MaximumLayerCount) {
        setError(errorMessage, QStringLiteral("The session cache contains too many selected layers."));
        return false;
    }
    stream << static_cast<quint32>(values.size());
    for (const QUuid &value : values) {
        stream << value;
    }
    return stream.status() == QDataStream::Ok;
}

bool readUuidVector(QDataStream &stream,
                    QVector<QUuid> *values,
                    QString *errorMessage)
{
    quint32 count = 0;
    stream >> count;
    if (stream.status() != QDataStream::Ok || count > MaximumLayerCount) {
        setError(errorMessage, QStringLiteral("The session cache contains an invalid selection count."));
        return false;
    }
    QVector<QUuid> restored;
    restored.reserve(static_cast<qsizetype>(count));
    for (quint32 index = 0; index < count; ++index) {
        QUuid value;
        stream >> value;
        if (stream.status() != QDataStream::Ok) {
            setError(errorMessage, QStringLiteral("The session cache ends inside its selection data."));
            return false;
        }
        restored.push_back(value);
    }
    *values = std::move(restored);
    return true;
}

qsizetype activeImageRowBytes(const QImage &image)
{
    if (image.isNull() || image.width() <= 0 || image.depth() <= 0
        || image.depth() % 8 != 0) {
        return 0;
    }
    const quint64 rowBytes = static_cast<quint64>(image.width())
        * static_cast<quint64>(image.depth() / 8);
    if (rowBytes > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        return 0;
    }
    return static_cast<qsizetype>(rowBytes);
}

quint32 qCompressDeclaredSize(const QByteArray &payload)
{
    if (payload.size() < 4) {
        return 0;
    }
    const auto *bytes = reinterpret_cast<const uchar *>(payload.constData());
    return (static_cast<quint32>(bytes[0]) << 24)
        | (static_cast<quint32>(bytes[1]) << 16)
        | (static_cast<quint32>(bytes[2]) << 8)
        | static_cast<quint32>(bytes[3]);
}

bool finiteTransform(const QTransform &transform)
{
    return transform.isInvertible()
        && transformMatrixIsFiniteAndBounded(transform);
}

bool validLayerType(const qint32 value)
{
    return value >= static_cast<qint32>(LayerType::BaseImage)
        && value <= static_cast<qint32>(LayerType::Smart);
}

bool validAdjustmentType(const qint32 value)
{
    return value >= static_cast<qint32>(AdjustmentType::Exposure)
        && value <= static_cast<qint32>(AdjustmentType::ChromaticAberrationCorrection);
}

bool validBlendMode(const qint32 value)
{
    return value >= static_cast<qint32>(BlendMode::Copy)
        && value <= static_cast<qint32>(BlendMode::Exclusion);
}

bool validGroupMode(const qint32 value)
{
    return value >= static_cast<qint32>(GroupCompositeMode::Isolated)
        && value <= static_cast<qint32>(GroupCompositeMode::PassThrough);
}

bool validChannelView(const qint32 value)
{
    return value >= static_cast<qint32>(ChannelView::Composite)
        && value <= static_cast<qint32>(ChannelView::Mask);
}

bool validEditTarget(const qint32 value)
{
    return value >= static_cast<qint32>(LayerEditTarget::Pixels)
        && value <= static_cast<qint32>(LayerEditTarget::Mask);
}

QString colourSpaceToken(const QColorSpace &colourSpace)
{
    if (colourSpace == QColorSpace(QColorSpace::SRgb)) {
        return QStringLiteral("srgb");
    }
    if (colourSpace == QColorSpace(QColorSpace::SRgbLinear)) {
        return QStringLiteral("linear-srgb");
    }
    return {};
}

QColorSpace restoredColourSpace(const QString &token, const QByteArray &profile)
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


bool validSessionImageFormat(const QImage::Format format)
{
    switch (format) {
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
    case QImage::Format_Grayscale8:
    case QImage::Format_Alpha8:
    case QImage::Format_RGBX64:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
    case QImage::Format_Grayscale16:
        return true;
    default:
        return false;
    }
}

bool writeImage(QDataStream &stream, const QImage &image, QString *errorMessage)
{
    stream << !image.isNull();
    if (image.isNull()) {
        return stream.status() == QDataStream::Ok;
    }
    if (!validSessionImageFormat(image.format())) {
        setError(errorMessage,
                 QStringLiteral("A session image uses an unsupported internal pixel format."));
        return false;
    }

    const qsizetype activeRowBytes = activeImageRowBytes(image);
    const quint64 byteCount = activeRowBytes > 0
        ? static_cast<quint64>(activeRowBytes) * static_cast<quint64>(image.height())
        : 0;
    if (activeRowBytes <= 0 || byteCount == 0 || byteCount > MaximumImageBytes
        || byteCount > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        setError(errorMessage, QStringLiteral("A session image exceeds the internal cache limit."));
        return false;
    }

    const QByteArray profile = image.colorSpace().iccProfile();
    if (profile.size() > MaximumProfileBytes) {
        setError(errorMessage,
                 QStringLiteral("An image colour profile exceeds the private session-cache limit."));
        return false;
    }
    const quint32 chunkCount = static_cast<quint32>(
        (byteCount + static_cast<quint64>(ImageChunkBytes) - 1)
        / static_cast<quint64>(ImageChunkBytes));

    stream << static_cast<qint32>(image.width())
           << static_cast<qint32>(image.height())
           << static_cast<qint32>(image.format())
           << static_cast<qint32>(activeRowBytes)
           << static_cast<qint32>(image.dotsPerMeterX())
           << static_cast<qint32>(image.dotsPerMeterY())
           << image.devicePixelRatio();
    if (!writeString(stream, colourSpaceToken(image.colorSpace()), errorMessage)
        || !writeSizedBytes(stream,
                            profile,
                            static_cast<quint32>(MaximumProfileBytes),
                            errorMessage)) {
        return false;
    }
    stream << byteCount << chunkCount;
    if (stream.status() != QDataStream::Ok) {
        setError(errorMessage, QStringLiteral("Could not write an image header to the session cache."));
        return false;
    }

    quint64 sourceOffset = 0;
    for (quint32 chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        const qsizetype rawSize = static_cast<qsizetype>(std::min<quint64>(
            static_cast<quint64>(ImageChunkBytes), byteCount - sourceOffset));
        QByteArray raw;
        raw.resize(rawSize);
        qsizetype chunkOffset = 0;
        while (chunkOffset < rawSize) {
            const quint64 rowIndex = sourceOffset / static_cast<quint64>(activeRowBytes);
            const qsizetype rowOffset = static_cast<qsizetype>(
                sourceOffset % static_cast<quint64>(activeRowBytes));
            const qsizetype copyBytes = std::min(
                rawSize - chunkOffset, activeRowBytes - rowOffset);
            std::memcpy(raw.data() + chunkOffset,
                        image.constScanLine(static_cast<int>(rowIndex)) + rowOffset,
                        static_cast<std::size_t>(copyBytes));
            chunkOffset += copyBytes;
            sourceOffset += static_cast<quint64>(copyBytes);
        }

        const QByteArray compressed = qCompress(raw, 1);
        const bool useCompression = !compressed.isEmpty() && compressed.size() < raw.size();
        const QByteArray payload = useCompression ? compressed : raw;
        const QByteArray digest = QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
        stream << static_cast<quint32>(rawSize)
               << useCompression;
        if (!writeSizedBytes(stream, digest, 32, errorMessage)
            || !writeSizedBytes(stream,
                                payload,
                                static_cast<quint32>(ImageChunkBytes),
                                errorMessage)) {
            return false;
        }
        if (stream.status() != QDataStream::Ok) {
            setError(errorMessage, QStringLiteral("Could not write image pixels to the session cache."));
            return false;
        }
    }
    return sourceOffset == byteCount;
}

bool readImage(QDataStream &stream, QImage *image, QString *errorMessage)
{
    bool present = false;
    stream >> present;
    if (stream.status() != QDataStream::Ok) {
        setError(errorMessage, QStringLiteral("The session cache ended before an image header."));
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
    quint64 rawByteCount = 0;
    quint32 chunkCount = 0;
    stream >> width >> height >> formatValue >> serializedRowBytes
           >> dotsPerMeterX >> dotsPerMeterY >> devicePixelRatio;
    if (!readString(stream, &colourToken, errorMessage)
        || !readSizedBytes(stream,
                           &profile,
                           static_cast<quint32>(MaximumProfileBytes),
                           errorMessage)) {
        return false;
    }
    stream >> rawByteCount >> chunkCount;

    const quint64 expectedChunkCount = rawByteCount == 0
        ? 0
        : (rawByteCount + static_cast<quint64>(ImageChunkBytes) - 1)
              / static_cast<quint64>(ImageChunkBytes);
    if (stream.status() != QDataStream::Ok
        || width < 1 || height < 1 || width > 32768 || height > 32768
        || serializedRowBytes < 1
        || rawByteCount == 0 || rawByteCount > MaximumImageBytes
        || rawByteCount > static_cast<quint64>(std::numeric_limits<qsizetype>::max())
        || chunkCount == 0 || chunkCount != expectedChunkCount) {
        setError(errorMessage, QStringLiteral("The session cache contains an invalid image header."));
        return false;
    }

    const QImage::Format format = static_cast<QImage::Format>(formatValue);
    if (!validSessionImageFormat(format)) {
        setError(errorMessage, QStringLiteral("The session cache uses an unsupported image format."));
        return false;
    }

    QImage restored(width, height, format);
    if (restored.isNull()) {
        setError(errorMessage, QStringLiteral("Could not allocate an exact session image."));
        return false;
    }
    const qsizetype activeRowBytes = activeImageRowBytes(restored);
    const quint64 expectedByteCount = activeRowBytes > 0
        ? static_cast<quint64>(activeRowBytes) * static_cast<quint64>(height)
        : 0;
    if (activeRowBytes != serializedRowBytes || expectedByteCount != rawByteCount
        || restored.bytesPerLine() < activeRowBytes) {
        setError(errorMessage, QStringLiteral("The session cache image layout is inconsistent."));
        return false;
    }
    // Pixel padding is never persisted. QImage::fill() only guarantees pixel
    // values, not row-padding bytes, so zero the complete allocation before
    // copying the active row payload back in.
    std::memset(restored.bits(),
                0,
                static_cast<std::size_t>(restored.sizeInBytes()));

    quint64 destinationOffset = 0;
    for (quint32 chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        quint32 rawSize = 0;
        bool compressed = false;
        QByteArray expectedDigest;
        QByteArray payload;
        stream >> rawSize >> compressed;
        if (!readSizedBytes(stream, &expectedDigest, 32, errorMessage)
            || !readSizedBytes(stream,
                               &payload,
                               static_cast<quint32>(ImageChunkBytes),
                               errorMessage)) {
            return false;
        }
        const quint64 remaining = rawByteCount - destinationOffset;
        const quint32 expectedRawSize = static_cast<quint32>(
            std::min<quint64>(static_cast<quint64>(ImageChunkBytes), remaining));
        if (stream.status() != QDataStream::Ok
            || rawSize != expectedRawSize
            || expectedDigest.size() != 32
            || (compressed && qCompressDeclaredSize(payload) != rawSize)) {
            setError(errorMessage, QStringLiteral("The session cache contains an invalid image chunk."));
            return false;
        }
        const QByteArray raw = compressed ? qUncompress(payload) : payload;
        if (raw.size() != static_cast<qsizetype>(rawSize)
            || QCryptographicHash::hash(raw, QCryptographicHash::Sha256) != expectedDigest) {
            setError(errorMessage, QStringLiteral("An image in the session cache is damaged."));
            return false;
        }

        qsizetype chunkOffset = 0;
        while (chunkOffset < raw.size()) {
            const quint64 rowIndex = destinationOffset / static_cast<quint64>(activeRowBytes);
            const qsizetype rowOffset = static_cast<qsizetype>(
                destinationOffset % static_cast<quint64>(activeRowBytes));
            const qsizetype copyBytes = std::min(
                raw.size() - chunkOffset, activeRowBytes - rowOffset);
            std::memcpy(restored.scanLine(static_cast<int>(rowIndex)) + rowOffset,
                        raw.constData() + chunkOffset,
                        static_cast<std::size_t>(copyBytes));
            chunkOffset += copyBytes;
            destinationOffset += static_cast<quint64>(copyBytes);
        }
    }
    if (destinationOffset != rawByteCount) {
        setError(errorMessage, QStringLiteral("The session cache image payload is incomplete."));
        return false;
    }

    restored.setDotsPerMeterX(std::max(0, dotsPerMeterX));
    restored.setDotsPerMeterY(std::max(0, dotsPerMeterY));
    if (std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0) {
        restored.setDevicePixelRatio(devicePixelRatio);
    }
    const QColorSpace colourSpace = restoredColourSpace(colourToken, profile);
    if ((!colourToken.isEmpty() || !profile.isEmpty()) && !colourSpace.isValid()) {
        setError(errorMessage, QStringLiteral("An image colour profile in the session cache is damaged."));
        return false;
    }
    if (colourSpace.isValid()) {
        restored.setColorSpace(colourSpace);
    }
    *image = std::move(restored);
    return true;
}

bool writeLayer(QDataStream &stream,
                const LayerNode &layer,
                const int depth,
                quint32 *layerCount,
                QString *errorMessage)
{
    if (depth > MaximumLayerDepth || !layerCount || ++(*layerCount) > MaximumLayerCount
        || layer.id.isNull() || !finiteTransform(layer.transform)) {
        setError(errorMessage, QStringLiteral("The layer tree cannot be stored safely in the session cache."));
        return false;
    }

    stream << layer.id
           << static_cast<qint32>(layer.type);
    if (!writeString(stream, layer.name, errorMessage)) {
        return false;
    }
    stream << layer.visible
           << layer.opacity
           << static_cast<qint32>(layer.blendMode)
           << static_cast<qint32>(layer.groupCompositeMode)
           << layer.transform.m11() << layer.transform.m12() << layer.transform.m13()
           << layer.transform.m21() << layer.transform.m22() << layer.transform.m23()
           << layer.transform.m31() << layer.transform.m32() << layer.transform.m33()
           << std::max<quint64>(1, layer.revision)
           << static_cast<qint32>(layer.adjustmentType)
           << layer.exposure << layer.contrast << layer.saturation
           << layer.blackPoint << layer.whitePoint << layer.gamma;

    bool adjustmentOk = false;
    const QByteArray adjustmentBytes = QJsonDocument(
        layer.effectiveAdjustmentData().toJson(&adjustmentOk)).toJson(QJsonDocument::Compact);
    if (!adjustmentOk
        || !writeSizedBytes(stream, adjustmentBytes, MaximumAdjustmentBytes, errorMessage)) {
        return false;
    }
    bool vectorOk = false;
    const QByteArray vectorBytes = QJsonDocument(
        layer.vectorData.toJson(&vectorOk)).toJson(QJsonDocument::Compact);
    if (!vectorOk
        || !writeSizedBytes(stream, vectorBytes, MaximumVectorBytes, errorMessage)) {
        return false;
    }
    bool textOk = false;
    const QByteArray textBytes = QJsonDocument(
        layer.textData.toJson(&textOk)).toJson(QJsonDocument::Compact);
    if (!textOk || !writeSizedBytes(stream, textBytes, MaximumTextBytes, errorMessage)) {
        return false;
    }
    const bool hasSmartReference = layer.type == LayerType::Smart;
    if (hasSmartReference
        && (!layer.smartSource.isSafe() || !layer.smartTransform.isSafe())) {
        setError(errorMessage, QStringLiteral(
            "A Smart Layer has invalid Smart Source or transform metadata."));
        return false;
    }
    if (!hasSmartReference && !layer.smartSource.isEmpty()) {
        setError(errorMessage, QStringLiteral(
            "A non-Smart layer contains Smart Source metadata."));
        return false;
    }
    stream << hasSmartReference;
    if (hasSmartReference) {
        stream << layer.smartSource.sourceId
               << std::max<quint64>(1, layer.smartSource.observedSourceRevision)
               << static_cast<quint32>(layer.smartTransform.schema)
               << static_cast<qint32>(layer.smartTransform.interpolation);
        if (layer.liveFilters.size() > LayerNode::MaximumLiveFilterCount) {
            setError(errorMessage, QStringLiteral("A Smart Layer contains too many Live Filters."));
            return false;
        }
        stream << static_cast<quint32>(layer.liveFilters.size());
        for (const LiveFilter &filter : layer.liveFilters) {
            bool filterOk = false;
            const QByteArray bytes = QJsonDocument(filter.toJson(&filterOk))
                                         .toJson(QJsonDocument::Compact);
            if (!filterOk
                || !writeSizedBytes(stream, bytes, MaximumAdjustmentBytes, errorMessage)) {
                return false;
            }
        }
    } else if (!layer.liveFilters.isEmpty()) {
        setError(errorMessage, QStringLiteral("A non-Smart layer contains Live Filter metadata."));
        return false;
    }
    if (!layer.layerEffects.isEmpty() && !layerTypeSupportsLayerEffects(layer.type)) {
        setError(errorMessage, QStringLiteral("This layer type cannot contain Layer Effects."));
        return false;
    }
    if (layer.layerEffects.size() > LayerNode::MaximumLayerEffectCount) {
        setError(errorMessage, QStringLiteral("A layer contains too many Layer Effects."));
        return false;
    }
    stream << static_cast<quint32>(layer.layerEffects.size());
    QSet<QUuid> effectIds;
    for (const LayerEffect &effect : layer.layerEffects) {
        if (effectIds.contains(effect.id)) {
            setError(errorMessage, QStringLiteral("A layer contains duplicate Layer Effect IDs."));
            return false;
        }
        effectIds.insert(effect.id);
        bool effectOk = false;
        const QByteArray bytes = QJsonDocument(effect.toJson(&effectOk))
                                     .toJson(QJsonDocument::Compact);
        if (!effectOk
            || !writeSizedBytes(stream, bytes, MaximumAdjustmentBytes, errorMessage)) {
            return false;
        }
    }
    stream << layer.rasterReferenceSize << layer.maskReferenceSize
           << layer.rasterReferenceOrigin << layer.maskReferenceOrigin;

    if (!writeImage(stream, layer.rasterImage, errorMessage)
        || !writeImage(stream, layer.maskImage, errorMessage)) {
        return false;
    }
    if (static_cast<quint64>(layer.children.size()) > MaximumLayerCount) {
        setError(errorMessage, QStringLiteral("A group contains too many child layers."));
        return false;
    }
    stream << layer.maskEnabled << layer.maskInverted
           << static_cast<quint32>(layer.children.size());
    if (stream.status() != QDataStream::Ok) {
        setError(errorMessage, QStringLiteral("Could not write layer metadata to the session cache."));
        return false;
    }
    for (const LayerNode &child : layer.children) {
        if (!writeLayer(stream, child, depth + 1, layerCount, errorMessage)) {
            return false;
        }
    }
    return true;
}

bool readLayer(QDataStream &stream,
               LayerNode *layer,
               const int depth,
               quint32 *layerCount,
               const quint32 formatVersion,
               QString *errorMessage)
{
    if (depth > MaximumLayerDepth || !layerCount || ++(*layerCount) > MaximumLayerCount) {
        setError(errorMessage, QStringLiteral("The session cache layer tree exceeds safe limits."));
        return false;
    }

    QUuid id;
    qint32 typeValue = 0;
    QString name;
    bool visible = true;
    double opacity = 1.0;
    qint32 blendValue = 0;
    qint32 groupValue = 0;
    double m11 = 1.0;
    double m12 = 0.0;
    double m13 = 0.0;
    double m21 = 0.0;
    double m22 = 1.0;
    double m23 = 0.0;
    double m31 = 0.0;
    double m32 = 0.0;
    double m33 = 1.0;
    quint64 revision = 1;
    qint32 adjustmentValue = 0;
    double exposure = 0.0;
    double contrast = 0.0;
    double saturation = 0.0;
    double blackPoint = 0.0;
    double whitePoint = 1.0;
    double gamma = 1.0;

    stream >> id >> typeValue;
    if (!readString(stream, &name, errorMessage)) {
        return false;
    }
    stream >> visible >> opacity
           >> blendValue >> groupValue
           >> m11 >> m12 >> m13 >> m21 >> m22 >> m23 >> m31 >> m32 >> m33
           >> revision >> adjustmentValue
           >> exposure >> contrast >> saturation >> blackPoint >> whitePoint >> gamma;

    AdjustmentData typedAdjustment;
    bool hasTypedAdjustment = false;
    if (formatVersion >= 7) {
        QByteArray adjustmentBytes;
        if (!readSizedBytes(stream, &adjustmentBytes, MaximumAdjustmentBytes, errorMessage)) {
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(adjustmentBytes, &parseError);
        bool adjustmentOk = false;
        typedAdjustment = AdjustmentData::fromJson(document.object(),
                                                   static_cast<AdjustmentType>(adjustmentValue),
                                                   &adjustmentOk);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || !adjustmentOk
            || typedAdjustment.type != static_cast<AdjustmentType>(adjustmentValue)) {
            setError(errorMessage, QStringLiteral("The session cache contains invalid typed adjustment parameters."));
            return false;
        }
        hasTypedAdjustment = true;
    }

    VectorLayerData vectorData;
    if (formatVersion >= 8) {
        QByteArray vectorBytes;
        if (!readSizedBytes(stream, &vectorBytes, MaximumVectorBytes, errorMessage)) {
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument vectorDocument = QJsonDocument::fromJson(vectorBytes, &parseError);
        bool vectorOk = false;
        vectorData = VectorLayerData::fromJson(vectorDocument.object(), &vectorOk);
        if (parseError.error != QJsonParseError::NoError
            || !vectorDocument.isObject() || !vectorOk) {
            setError(errorMessage, QStringLiteral("The session cache contains invalid vector-layer data."));
            return false;
        }
        if (formatVersion < 10) {
            for (const VectorShape &shape : vectorData.objects) {
                if (shape.type == VectorShapeType::Path) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-schema-10 session cache cannot contain editable paths."));
                    return false;
                }
            }
        }
        if (formatVersion < 11) {
            for (const VectorShape &shape : vectorData.objects) {
                if (shape.type == VectorShapeType::Path) {
                    bool hasCorners = shape.bezierPath.hasCornerMetadata();
                    for (const VectorBezierPath &path : shape.additionalBezierPaths) {
                        hasCorners = hasCorners || path.hasCornerMetadata();
                    }
                    if (hasCorners) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-schema-11 session cache cannot contain live vector corners."));
                        return false;
                    }
                }
            }
        }
        if (formatVersion < 12) {
            for (const VectorShape &shape : vectorData.objects) {
                if (shape.stroke.pattern == VectorStrokePattern::Dashed) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-schema-12 session cache cannot contain dashed vector strokes."));
                    return false;
                }
            }
        }
        if (formatVersion < 13) {
            for (const VectorShape &shape : vectorData.objects) {
                if (shape.type == VectorShapeType::Path
                    && !shape.additionalBezierPaths.isEmpty()) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-schema-13 session cache cannot contain compound vector paths."));
                    return false;
                }
            }
        }
        if (formatVersion < 14) {
            for (const VectorShape &shape : vectorData.objects) {
                if (shape.type == VectorShapeType::Path
                    && shape.pathFillRule == VectorPathFillRule::NonZero) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-schema-14 session cache cannot contain nonzero-winding vector paths."));
                    return false;
                }
            }
        }
        if (formatVersion < 15) {
            for (const VectorShape &shape : vectorData.objects) {
                if (shape.type == VectorShapeType::Arrow
                    || shape.stroke.startArrowhead != VectorArrowheadType::None
                    || shape.stroke.endArrowhead != VectorArrowheadType::None
                    || std::abs(shape.stroke.startArrowScale - 1.0) > 1.0e-12
                    || std::abs(shape.stroke.endArrowScale - 1.0) > 1.0e-12) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-schema-15 session cache cannot contain vector arrowheads or arrow shapes."));
                    return false;
                }
            }
        }
        if (formatVersion < 17
            && std::abs(vectorData.featherRadius) > 1.0e-12) {
            setError(errorMessage, QStringLiteral(
                "A pre-schema-17 session cache cannot contain nonzero vector Feather values."));
            return false;
        }
    }

    TextLayerData textData;
    if (formatVersion >= 9) {
        QByteArray textBytes;
        if (!readSizedBytes(stream, &textBytes, MaximumTextBytes, errorMessage)) return false;
        QJsonParseError parseError;
        const QJsonDocument textDocument = QJsonDocument::fromJson(textBytes, &parseError);
        bool textOk = false;
        textData = TextLayerData::fromJson(textDocument.object(), &textOk);
        if (parseError.error != QJsonParseError::NoError || !textDocument.isObject() || !textOk) {
            setError(errorMessage, QStringLiteral("The session cache contains invalid text-layer data."));
            return false;
        }
    }

    SmartLayerReference smartSource;
    SmartTransformState smartTransform;
    QVector<LiveFilter> liveFilters;
    if (formatVersion >= 18) {
        bool hasSmartReference = false;
        stream >> hasSmartReference;
        if (hasSmartReference) {
            stream >> smartSource.sourceId >> smartSource.observedSourceRevision;
            if (formatVersion >= 21) {
                quint32 smartTransformSchema = 0;
                qint32 smartInterpolation = -1;
                stream >> smartTransformSchema >> smartInterpolation;
                smartTransform.schema = smartTransformSchema;
                smartTransform.interpolation = static_cast<TransformInterpolation>(
                    smartInterpolation);
            }
            if (!smartSource.isSafe() || !smartTransform.isSafe()) {
                setError(errorMessage, QStringLiteral(
                    "The session cache contains invalid Smart Source or transform metadata."));
                return false;
            }
            if (formatVersion >= 22) {
                quint32 filterCount = 0;
                stream >> filterCount;
                if (stream.status() != QDataStream::Ok
                    || filterCount > static_cast<quint32>(LayerNode::MaximumLiveFilterCount)) {
                    setError(errorMessage, QStringLiteral(
                        "The session cache contains too many Live Filters."));
                    return false;
                }
                QSet<QUuid> ids;
                liveFilters.reserve(static_cast<qsizetype>(filterCount));
                for (quint32 index = 0; index < filterCount; ++index) {
                    QByteArray bytes;
                    if (!readSizedBytes(stream, &bytes, MaximumAdjustmentBytes, errorMessage)) {
                        return false;
                    }
                    QJsonParseError parseError;
                    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
                    const QJsonObject filterObject = document.object();
                    const bool hasMaskMetadata = filterObject.value(QStringLiteral("schema")).toInt(1) >= 2
                        || filterObject.contains(QStringLiteral("maskImage"))
                        || filterObject.contains(QStringLiteral("maskReferenceWidth"))
                        || filterObject.contains(QStringLiteral("maskReferenceHeight"))
                        || filterObject.contains(QStringLiteral("maskReferenceOriginX"))
                        || filterObject.contains(QStringLiteral("maskReferenceOriginY"))
                        || filterObject.contains(QStringLiteral("maskEnabled"))
                        || filterObject.contains(QStringLiteral("maskInverted"));
                    if (formatVersion < 23 && hasMaskMetadata) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-version-23 session snapshot cannot contain Live Filter mask metadata."));
                        return false;
                    }
                    bool filterOk = false;
                    LiveFilter filter = LiveFilter::fromJson(filterObject, &filterOk);
                    if (parseError.error != QJsonParseError::NoError || !document.isObject()
                        || !filterOk || ids.contains(filter.id)) {
                        setError(errorMessage, QStringLiteral(
                            "The session cache contains invalid Live Filter data."));
                        return false;
                    }
                    ids.insert(filter.id);
                    liveFilters.push_back(std::move(filter));
                }
            }
        }
        if ((static_cast<LayerType>(typeValue) == LayerType::Smart) != hasSmartReference) {
            setError(errorMessage, QStringLiteral("The session cache Smart Layer metadata is inconsistent."));
            return false;
        }
    }

    QVector<LayerEffect> layerEffects;
    if (formatVersion >= 24) {
        quint32 effectCount = 0;
        stream >> effectCount;
        if (stream.status() != QDataStream::Ok
            || effectCount > static_cast<quint32>(LayerNode::MaximumLayerEffectCount)) {
            setError(errorMessage, QStringLiteral("The session cache contains too many Layer Effects."));
            return false;
        }
        QSet<QUuid> effectIds;
        layerEffects.reserve(static_cast<qsizetype>(effectCount));
        for (quint32 index = 0; index < effectCount; ++index) {
            QByteArray bytes;
            if (!readSizedBytes(stream, &bytes, MaximumAdjustmentBytes, errorMessage)) return false;
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
            const QJsonObject effectObject = document.object();
            const bool hasRendererParameters = effectObject.value(QStringLiteral("schema")).toInt(1) >= 2
                || effectObject.contains(QStringLiteral("colour"))
                || effectObject.contains(QStringLiteral("opacity"))
                || effectObject.contains(QStringLiteral("blendMode"))
                || effectObject.contains(QStringLiteral("angle"))
                || effectObject.contains(QStringLiteral("distance"))
                || effectObject.contains(QStringLiteral("spread"))
                || effectObject.contains(QStringLiteral("size"));
            const bool hasStrokeOverlayParameters = effectObject.value(QStringLiteral("schema")).toInt(1) >= 3
                || effectObject.contains(QStringLiteral("strokePosition"))
                || effectObject.contains(QStringLiteral("gradientStops"))
                || effectObject.contains(QStringLiteral("gradientInterpolation"))
                || effectObject.contains(QStringLiteral("gradientStyle"))
                || effectObject.contains(QStringLiteral("gradientAngle"))
                || effectObject.contains(QStringLiteral("gradientScale"))
                || effectObject.contains(QStringLiteral("gradientReverse"));
            const bool hasBevelParameters = effectObject.value(QStringLiteral("schema")).toInt(1) >= 4
                || effectObject.contains(QStringLiteral("bevelStyle"))
                || effectObject.contains(QStringLiteral("bevelDirection"))
                || effectObject.contains(QStringLiteral("bevelDepth"))
                || effectObject.contains(QStringLiteral("bevelSoften"))
                || effectObject.contains(QStringLiteral("bevelAltitude"))
                || effectObject.contains(QStringLiteral("bevelHighlightColour"))
                || effectObject.contains(QStringLiteral("bevelHighlightBlendMode"))
                || effectObject.contains(QStringLiteral("bevelHighlightOpacity"))
                || effectObject.contains(QStringLiteral("bevelShadowColour"))
                || effectObject.contains(QStringLiteral("bevelShadowBlendMode"))
                || effectObject.contains(QStringLiteral("bevelShadowOpacity"));
            if (formatVersion < 25 && hasRendererParameters) {
                setError(errorMessage, QStringLiteral(
                    "A pre-version-25 session snapshot cannot contain Layer Effect renderer parameters."));
                return false;
            }
            if (formatVersion < 26 && hasStrokeOverlayParameters) {
                setError(errorMessage, QStringLiteral(
                    "A pre-version-26 session snapshot cannot contain Stroke/Overlay Layer Effect parameters."));
                return false;
            }
            if (formatVersion < 27 && hasBevelParameters) {
                setError(errorMessage, QStringLiteral(
                    "A pre-version-27 session snapshot cannot contain Bevel & Emboss Layer Effect parameters."));
                return false;
            }
            bool effectOk = false;
            LayerEffect effect = LayerEffect::fromJson(effectObject, &effectOk);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()
                || !effectOk || effectIds.contains(effect.id)) {
                setError(errorMessage, QStringLiteral("The session cache contains invalid Layer Effect data."));
                return false;
            }
            effectIds.insert(effect.id);
            layerEffects.push_back(std::move(effect));
        }
        if (!layerEffects.isEmpty()
            && !layerTypeSupportsLayerEffects(static_cast<LayerType>(typeValue))) {
            setError(errorMessage, QStringLiteral("The session cache attaches Layer Effects to an unsupported layer type."));
            return false;
        }
    }

    QSize rasterReferenceSize;
    QSize maskReferenceSize;
    QPointF rasterReferenceOrigin;
    QPointF maskReferenceOrigin;
    if (formatVersion >= 5) {
        stream >> rasterReferenceSize >> maskReferenceSize;
    }
    if (formatVersion >= 6) {
        stream >> rasterReferenceOrigin >> maskReferenceOrigin;
    }

    const QTransform transform(m11, m12, m13,
                               m21, m22, m23,
                               m31, m32, m33);
    if (stream.status() != QDataStream::Ok || id.isNull()
        || !validLayerType(typeValue)
        || (static_cast<LayerType>(typeValue) == LayerType::Vector && formatVersion < 8)
        || (static_cast<LayerType>(typeValue) == LayerType::Text && formatVersion < 9)
        || (static_cast<LayerType>(typeValue) == LayerType::Smart && formatVersion < 18)
        || !validBlendMode(blendValue)
        || !validGroupMode(groupValue) || !validAdjustmentType(adjustmentValue)
        || !std::isfinite(opacity) || !finiteTransform(transform)
        || !std::isfinite(exposure) || !std::isfinite(contrast)
        || !std::isfinite(saturation) || !std::isfinite(blackPoint)
        || !std::isfinite(whitePoint) || !std::isfinite(gamma)) {
        setError(errorMessage, QStringLiteral("The session cache contains invalid layer metadata."));
        return false;
    }

    LayerNode restored;
    restored.id = id;
    restored.type = static_cast<LayerType>(typeValue);
    restored.name = name;
    restored.visible = visible;
    restored.opacity = std::clamp(opacity, 0.0, 1.0);
    restored.blendMode = static_cast<BlendMode>(blendValue);
    restored.groupCompositeMode = static_cast<GroupCompositeMode>(groupValue);
    restored.transform = transform;
    restored.revision = std::max<quint64>(1, revision);
    restored.adjustmentType = static_cast<AdjustmentType>(adjustmentValue);
    restored.exposure = exposure;
    restored.contrast = contrast;
    restored.saturation = saturation;
    restored.blackPoint = blackPoint;
    restored.whitePoint = whitePoint;
    restored.gamma = gamma;
    restored.vectorData = vectorData;
    restored.textData = textData;
    restored.smartSource = smartSource;
    restored.smartTransform = smartTransform;
    restored.liveFilters = std::move(liveFilters);
    restored.layerEffects = std::move(layerEffects);
    if (hasTypedAdjustment) {
        restored.setAdjustmentData(typedAdjustment);
    } else {
        AdjustmentData migrated;
        migrated.reset(restored.adjustmentType);
        switch (restored.adjustmentType) {
        case AdjustmentType::Exposure:
            migrated.parameters = ExposureParameters {restored.exposure};
            break;
        case AdjustmentType::Contrast:
            migrated.parameters = ContrastParameters {restored.contrast};
            break;
        case AdjustmentType::Saturation:
            migrated.parameters = SaturationParameters {restored.saturation};
            break;
        case AdjustmentType::Levels: {
            LevelsParameters levels;
            auto &rgb = levels.channel(AdjustmentChannel::Rgb);
            rgb.inputBlack = restored.blackPoint;
            rgb.inputWhite = restored.whitePoint;
            rgb.gamma = restored.gamma;
            migrated.parameters = levels;
            break;
        }
        case AdjustmentType::Curves:
            migrated.parameters = CurvesParameters {};
            break;
        case AdjustmentType::HueSaturation:
            migrated.parameters = HueSaturationParameters {};
            break;
        case AdjustmentType::Vibrance:
            migrated.parameters = VibranceParameters {};
            break;
        case AdjustmentType::WhiteBalance:
            migrated.parameters = WhiteBalanceParameters {};
            break;
        case AdjustmentType::ColourBalance:
            migrated.parameters = ColourBalanceParameters {};
            break;
        case AdjustmentType::ChannelMixer:
            migrated.parameters = ChannelMixerParameters {};
            break;
        case AdjustmentType::BlackAndWhite:
            migrated.parameters = BlackAndWhiteParameters {};
            break;
        case AdjustmentType::GradientMap:
            migrated.parameters = GradientMapParameters {};
            break;
        case AdjustmentType::Posterise:
            migrated.parameters = PosteriseParameters {};
            break;
        case AdjustmentType::Threshold:
            migrated.parameters = ThresholdParameters {};
            break;
        case AdjustmentType::Lut:
            migrated.parameters = LutParameters {};
            break;
        case AdjustmentType::ShadowsHighlights:
            migrated.parameters = ShadowsHighlightsParameters {};
            break;
        case AdjustmentType::GaussianBlur:
            migrated.parameters = GaussianBlurParameters {};
            break;
        case AdjustmentType::BoxBlur:
            migrated.parameters = BoxBlurParameters {};
            break;
        case AdjustmentType::UnsharpMask:
            migrated.parameters = UnsharpMaskParameters {};
            break;
        case AdjustmentType::HighPass:
            migrated.parameters = HighPassParameters {};
            break;
        case AdjustmentType::Invert:
            migrated.parameters = InvertParameters {};
            break;
        case AdjustmentType::PhotoFilter:
            migrated.parameters = PhotoFilterParameters {};
            break;
        case AdjustmentType::SelectiveColour:
            migrated.parameters = SelectiveColourParameters {};
            break;
        case AdjustmentType::Vignette:
            migrated.parameters = VignetteParameters {};
            break;
        case AdjustmentType::RgbSplit:
            migrated.parameters = RgbSplitParameters {};
            break;
        case AdjustmentType::ChromaticAberrationCorrection:
            migrated.parameters = ChromaticAberrationCorrectionParameters {};
            break;
        case AdjustmentType::SurfaceBlur:
            migrated.parameters = SurfaceBlurParameters {};
            break;
        case AdjustmentType::MotionBlur:
            migrated.parameters = MotionBlurParameters {};
            break;
        case AdjustmentType::RadialBlur:
            migrated.parameters = RadialBlurParameters {};
            break;
        }
        restored.setAdjustmentData(migrated);
    }
    restored.rasterReferenceSize = rasterReferenceSize;
    restored.maskReferenceSize = maskReferenceSize;
    restored.rasterReferenceOrigin = rasterReferenceOrigin;
    restored.maskReferenceOrigin = maskReferenceOrigin;

    if (!readImage(stream, &restored.rasterImage, errorMessage)
        || !readImage(stream, &restored.maskImage, errorMessage)) {
        return false;
    }

    quint32 childCount = 0;
    stream >> restored.maskEnabled >> restored.maskInverted >> childCount;
    if (stream.status() != QDataStream::Ok || childCount > MaximumLayerCount - *layerCount) {
        setError(errorMessage, QStringLiteral("The session cache contains an invalid child-layer count."));
        return false;
    }
    restored.children.reserve(static_cast<qsizetype>(childCount));
    for (quint32 index = 0; index < childCount; ++index) {
        LayerNode child;
        if (!readLayer(stream, &child, depth + 1, layerCount,
                       formatVersion, errorMessage)) {
            return false;
        }
        restored.children.push_back(std::move(child));
    }
    *layer = std::move(restored);
    return true;
}

bool validateLayerTree(const QVector<LayerNode> &layers,
                       const QSize &documentSize,
                       QSet<QUuid> *ids,
                       int *baseCount,
                       int depth = 0)
{
    Q_UNUSED(documentSize);
    if (!ids || !baseCount || depth > MaximumLayerDepth) {
        return false;
    }
    for (const LayerNode &layer : layers) {
        if (layer.id.isNull() || ids->contains(layer.id) || !finiteTransform(layer.transform)
            || (layer.type == LayerType::Smart && !layer.smartTransform.isSafe())) {
            return false;
        }
        ids->insert(layer.id);
        if (layer.type == LayerType::BaseImage) {
            ++(*baseCount);
        }
        const auto validRaster = [](const QImage &image) {
            return image.isNull() || (image.width() > 0 && image.height() > 0);
        };
        const auto validMask = [](const QImage &image) {
            return image.isNull() || (image.width() > 0 && image.height() > 0);
        };
        const auto validReference = [](const QSize &size) {
            return size.isEmpty()
                || (size.width() > 0 && size.height() > 0
                    && size.width() <= 32768 && size.height() <= 32768);
        };
        const auto validOrigin = [](const QPointF &origin) {
            constexpr double MaximumCoordinate = 1073741824.0;
            return std::isfinite(origin.x()) && std::isfinite(origin.y())
                && std::abs(origin.x()) <= MaximumCoordinate
                && std::abs(origin.y()) <= MaximumCoordinate;
        };
        bool liveFiltersOk = layer.type == LayerType::Smart
            ? layer.liveFilters.size() <= LayerNode::MaximumLiveFilterCount
            : layer.liveFilters.isEmpty();
        bool layerEffectsOk = layerTypeSupportsLayerEffects(layer.type)
            ? layer.layerEffects.size() <= LayerNode::MaximumLayerEffectCount
            : layer.layerEffects.isEmpty();
        if (layerEffectsOk) {
            QSet<QUuid> effectIds;
            for (const LayerEffect &effect : layer.layerEffects) {
                if (!effect.isSafe() || effectIds.contains(effect.id)) {
                    layerEffectsOk = false;
                    break;
                }
                effectIds.insert(effect.id);
            }
        }
        if (liveFiltersOk && layer.type == LayerType::Smart) {
            QSet<QUuid> filterIds;
            for (const LiveFilter &filter : layer.liveFilters) {
                if (!filter.isSafe() || filterIds.contains(filter.id)) {
                    liveFiltersOk = false;
                    break;
                }
                filterIds.insert(filter.id);
            }
        }
        if (!validRaster(layer.rasterImage) || !validMask(layer.maskImage)
            || !validReference(layer.rasterReferenceSize)
            || !validReference(layer.maskReferenceSize)
            || !validOrigin(layer.rasterReferenceOrigin)
            || !validOrigin(layer.maskReferenceOrigin)
            || (layer.type == LayerType::Vector && !layer.vectorData.isSafe())
            || (layer.type == LayerType::Text && !layer.textData.isSafe())
            || (layer.type == LayerType::Smart && !layer.smartSource.isSafe())
            || (layer.type != LayerType::Smart && !layer.smartSource.isEmpty())
            || !liveFiltersOk
            || !layerEffectsOk
            || !validateLayerTree(layer.children, documentSize, ids, baseCount, depth + 1)) {
            return false;
        }
    }
    return true;
}


bool writeSelectionMask(QDataStream &stream,
                        const SelectionMask &selection,
                        QString *errorMessage)
{
    const SelectionMask::Snapshot snapshot = selection.snapshot();
    stream << snapshot.active
           << snapshot.implicitCoverage
           << snapshot.revision;
    const QVector<QPoint> indices = selection.explicitTileIndices();
    stream << static_cast<quint32>(indices.size());
    for (const QPoint &tileIndex : indices) {
        const QSize dimensions = selection.tilePixelSize(tileIndex);
        const QByteArray raw = selection.actualTileBytes(tileIndex);
        if (dimensions.isEmpty()
            || raw.size() != dimensions.width() * dimensions.height()) {
            setError(errorMessage, QStringLiteral("A selection tile has invalid dimensions."));
            return false;
        }
        const QByteArray compressed = qCompress(raw, 1);
        const bool useCompression = !compressed.isEmpty() && compressed.size() < raw.size();
        const QByteArray payload = useCompression ? compressed : raw;
        stream << static_cast<qint32>(tileIndex.x())
               << static_cast<qint32>(tileIndex.y())
               << static_cast<qint32>(dimensions.width())
               << static_cast<qint32>(dimensions.height())
               << useCompression;
        if (!writeSizedBytes(stream,
                             QCryptographicHash::hash(raw, QCryptographicHash::Sha256),
                             32,
                             errorMessage)
            || !writeSizedBytes(stream,
                                payload,
                                static_cast<quint32>(SelectionMask::TileSize
                                                     * SelectionMask::TileSize + 64),
                                errorMessage)) {
            return false;
        }
    }
    return stream.status() == QDataStream::Ok;
}

bool readSelectionMask(QDataStream &stream,
                       const QSize &documentSize,
                       SelectionMask *selection,
                       QString *errorMessage)
{
    if (!selection || documentSize.isEmpty()) {
        setError(errorMessage, QStringLiteral("The session selection has no document size."));
        return false;
    }
    bool active = false;
    quint8 implicitCoverage = 0;
    quint64 revision = 1;
    quint32 tileCount = 0;
    stream >> active >> implicitCoverage >> revision >> tileCount;
    const qint64 maximumTilesX =
        (static_cast<qint64>(documentSize.width()) + SelectionMask::TileSize - 1)
        / SelectionMask::TileSize;
    const qint64 maximumTilesY =
        (static_cast<qint64>(documentSize.height()) + SelectionMask::TileSize - 1)
        / SelectionMask::TileSize;
    const quint64 maximumTiles = static_cast<quint64>(maximumTilesX * maximumTilesY);
    if (stream.status() != QDataStream::Ok
        || static_cast<quint64>(tileCount) > maximumTiles
        || (!active && (implicitCoverage != 0 || tileCount != 0))) {
        setError(errorMessage, QStringLiteral("The session selection has inconsistent metadata or an invalid tile count."));
        return false;
    }

    SelectionMask::Snapshot snapshot;
    snapshot.size = documentSize;
    snapshot.active = active;
    snapshot.implicitCoverage = active ? implicitCoverage : 0;
    snapshot.revision = std::max<quint64>(1, revision);
    for (quint32 index = 0; index < tileCount; ++index) {
        qint32 tileX = -1;
        qint32 tileY = -1;
        qint32 width = 0;
        qint32 height = 0;
        bool compressed = false;
        QByteArray expectedDigest;
        QByteArray payload;
        stream >> tileX >> tileY >> width >> height >> compressed;
        if (!readSizedBytes(stream, &expectedDigest, 32, errorMessage)
            || !readSizedBytes(stream,
                               &payload,
                               static_cast<quint32>(SelectionMask::TileSize
                                                    * SelectionMask::TileSize + 64),
                               errorMessage)) {
            return false;
        }
        const qint64 maximumTilesX =
            (static_cast<qint64>(documentSize.width()) + SelectionMask::TileSize - 1)
            / SelectionMask::TileSize;
        const qint64 maximumTilesY =
            (static_cast<qint64>(documentSize.height()) + SelectionMask::TileSize - 1)
            / SelectionMask::TileSize;
        if (stream.status() != QDataStream::Ok || tileX < 0 || tileY < 0
            || tileX >= maximumTilesX || tileY >= maximumTilesY) {
            setError(errorMessage, QStringLiteral("The session selection contains an invalid tile index."));
            return false;
        }
        const QPoint tileIndex(tileX, tileY);
        const int expectedWidth = std::min(
            SelectionMask::TileSize,
            documentSize.width() - tileX * SelectionMask::TileSize);
        const int expectedHeight = std::min(
            SelectionMask::TileSize,
            documentSize.height() - tileY * SelectionMask::TileSize);
        const int expectedRawSize = expectedWidth * expectedHeight;
        if (expectedWidth <= 0 || expectedHeight <= 0
            || width != expectedWidth || height != expectedHeight
            || expectedDigest.size() != 32
            || (compressed && qCompressDeclaredSize(payload)
                                  != static_cast<quint32>(expectedRawSize))) {
            setError(errorMessage, QStringLiteral("The session selection contains an invalid tile header."));
            return false;
        }
        const QByteArray raw = compressed ? qUncompress(payload) : payload;
        if (raw.size() != expectedRawSize
            || QCryptographicHash::hash(raw, QCryptographicHash::Sha256)
                != expectedDigest) {
            setError(errorMessage, QStringLiteral("A selection tile in the session cache is damaged."));
            return false;
        }
        const quint64 key = SelectionMask::tileKey(tileIndex);
        if (snapshot.tiles.contains(key)) {
            setError(errorMessage, QStringLiteral("The session selection contains duplicate tiles."));
            return false;
        }
        const bool uniformImplicit = std::all_of(
            raw.cbegin(), raw.cend(), [implicitCoverage = snapshot.implicitCoverage](const char byte) {
                return static_cast<quint8>(byte) == implicitCoverage;
            });
        if (snapshot.active && !uniformImplicit) {
            snapshot.tiles.insert(key, raw);
        }
    }

    SelectionMask restored(documentSize);
    if (!restored.restoreSnapshot(snapshot, false)) {
        setError(errorMessage, QStringLiteral("The session selection is inconsistent."));
        return false;
    }
    *selection = std::move(restored);
    return true;
}

bool smartLayerReferencesResolve(const QVector<LayerNode> &layers,
                                 const SmartSourceRegistry &sources)
{
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Smart) {
            const SmartSourceDescriptor *source = sources.find(layer.smartSource.sourceId);
            if (!source || layer.smartSource.observedSourceRevision > source->revision) {
                return false;
            }
        } else if (!layer.smartSource.isEmpty()) {
            return false;
        }
        if (!smartLayerReferencesResolve(layer.children, sources)) return false;
    }
    return true;
}

qint64 fileSizeOrZero(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() ? std::max<qint64>(0, info.size()) : 0;
}

} // namespace

class SessionSnapshotCodec final {
public:
    static bool write(const QString &filePath,
                      const DocumentSession &session,
                      QString *errorMessage)
    {
        if (!session.document().hasImage()) {
            setError(errorMessage, QStringLiteral("There is no resident document to snapshot."));
            return false;
        }

        QSaveFile file(filePath);
        if (!file.open(QIODevice::WriteOnly)) {
            setError(errorMessage, file.errorString());
            return false;
        }
        QDataStream stream(&file);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.setVersion(QDataStream::Qt_6_0);
        if (stream.writeRawData(SnapshotMagic, sizeof(SnapshotMagic)) != sizeof(SnapshotMagic)) {
            setError(errorMessage, file.errorString());
            file.cancelWriting();
            return false;
        }
        stream << SnapshotFormatVersion;

        const PhotoDocument &document = session.document();
        QString smartSourceError;
        if (!document.m_smartSources.validate(&smartSourceError)
            || !smartLayerReferencesResolve(document.m_layers,
                                            document.m_smartSources)) {
            setError(errorMessage, smartSourceError.isEmpty()
                ? QStringLiteral("The document Smart Layer references cannot be cached safely.")
                : smartSourceError);
            file.cancelWriting();
            return false;
        }
        for (const SmartSourceDescriptor &source : document.m_smartSources.descriptors()) {
            if (!source.hasEmbeddedDocument()) continue;
            QVector<LayerNode> embeddedLayers;
            if (!document.embeddedSmartSourceLayers(source.id, &embeddedLayers, nullptr,
                                                    nullptr, &smartSourceError)
                || !source.hasCurrentPresentation()) {
                setError(errorMessage, smartSourceError.isEmpty()
                    ? QStringLiteral("The embedded Smart Source cannot be cached safely.")
                    : smartSourceError);
                file.cancelWriting();
                return false;
            }
        }
        if (!writeString(stream, document.m_sourcePath, errorMessage)
            || !writeString(stream, document.m_projectPath, errorMessage)
            || !writeString(stream, document.m_documentName, errorMessage)) {
            file.cancelWriting();
            return false;
        }
        stream << document.m_documentIdentity;
        stream << static_cast<qint32>(document.m_colourModel)
               << document.m_blankDocument
               << document.m_resolutionX
               << document.m_resolutionY
               << document.m_modified;
        if (!writeImage(stream, document.m_sourceImage, errorMessage)) {
            file.cancelWriting();
            return false;
        }
        QString colourStateError;
        if (!document.m_colourState.isSafe(&colourStateError)) {
            setError(errorMessage,
                     QStringLiteral("The document colour state cannot be cached: %1")
                         .arg(colourStateError));
            file.cancelWriting();
            return false;
        }
        const QByteArray colourStateBytes = QJsonDocument(
            document.m_colourState.toJson()).toJson(QJsonDocument::Compact);
        if (!writeSizedBytes(stream,
                             colourStateBytes,
                             MaximumColourStateBytes,
                             errorMessage)) {
            file.cancelWriting();
            return false;
        }

        if (static_cast<quint64>(document.m_layers.size()) > MaximumLayerCount) {
            setError(errorMessage, QStringLiteral("The document has too many root layers to cache."));
            file.cancelWriting();
            return false;
        }
        stream << static_cast<quint32>(document.m_layers.size());
        quint32 layerCount = 0;
        for (const LayerNode &layer : document.m_layers) {
            if (!writeLayer(stream, layer, 0, &layerCount, errorMessage)) {
                file.cancelWriting();
                return false;
            }
        }
        bool smartSourcesOk = false;
        const QJsonArray smartSourcesArray = document.m_smartSources.toJson(&smartSourcesOk);
        const QByteArray smartSourceBytes = QJsonDocument(smartSourcesArray)
            .toJson(QJsonDocument::Compact);
        if (!smartSourcesOk
            || !writeSizedBytes(stream, smartSourceBytes, MaximumSmartSourceBytes, errorMessage)) {
            file.cancelWriting();
            return false;
        }
        if (!writeGuideVector(stream, document.m_horizontalGuides, errorMessage)
            || !writeGuideVector(stream, document.m_verticalGuides, errorMessage)
            || !writeSelectionMask(stream, document.m_selectionMask, errorMessage)) {
            file.cancelWriting();
            return false;
        }

        stream << session.m_baselineRequiresSave
               << session.m_editTargetLayerId
               << static_cast<qint32>(session.m_editTarget)
               << static_cast<qint32>(session.m_channelView);
        if (!writeUuidVector(stream, session.m_selectedLayerIds, errorMessage)) {
            file.cancelWriting();
            return false;
        }
        stream << session.m_viewState.zoom
               << session.m_viewState.scrollPosition
               << session.m_viewState.fitToView
               << session.m_viewState.valid
               << session.m_selectionEdgesVisible
               << session.m_cropState.initialised
               << session.m_cropState.frame
               << static_cast<qint32>(session.m_cropState.mode)
               << session.m_cropState.ratioWidth
               << session.m_cropState.ratioHeight
               << session.m_cropState.originalRatio
               << session.m_cropState.fixedSize
               << static_cast<qint32>(session.m_cropState.overlay)
               << session.m_cropState.overlayOrientation
               << session.m_cropState.dimOpacity
               << session.m_cropState.snappingEnabled
               << session.m_cropState.deleteCroppedPixels
               << session.m_cropState.straightenSampling
               << session.m_cropState.straightenAngle;

        const bool sourceEditor = session.m_smartSourceEditBinding.isValid();
        stream << sourceEditor;
        if (sourceEditor) {
            const auto &binding = session.m_smartSourceEditBinding;
            stream << binding.ownerSessionId << binding.sourceId;
            if (!writeString(stream, binding.sourceName, errorMessage)) {
                file.cancelWriting();
                return false;
            }
            QVector<QUuid> baselineIds;
            baselineIds.reserve(binding.baselineSourceRevisions.size());
            for (auto it = binding.baselineSourceRevisions.cbegin();
                 it != binding.baselineSourceRevisions.cend(); ++it) {
                baselineIds.push_back(it.key());
            }
            std::sort(baselineIds.begin(), baselineIds.end(), [](const QUuid &a, const QUuid &b) {
                return a.toString(QUuid::WithoutBraces) < b.toString(QUuid::WithoutBraces);
            });
            if (baselineIds.size() > SmartSourceRegistry::MaximumSources) {
                setError(errorMessage, QStringLiteral(
                    "The Smart Source editor baseline is too large to cache safely."));
                file.cancelWriting();
                return false;
            }
            stream << static_cast<quint32>(baselineIds.size());
            for (const QUuid &id : baselineIds) {
                stream << id << binding.baselineSourceRevisions.value(id);
            }
        }

        if (stream.status() != QDataStream::Ok || !file.commit()) {
            setError(errorMessage,
                     file.errorString().isEmpty()
                         ? QStringLiteral("Could not commit the session snapshot.")
                         : file.errorString());
            return false;
        }
        return true;
    }

    static bool read(const QString &filePath,
                     DocumentSession *session,
                     QString *errorMessage)
    {
        if (!session) {
            setError(errorMessage, QStringLiteral("No document session was supplied for restore."));
            return false;
        }
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            setError(errorMessage, file.errorString());
            return false;
        }
        QDataStream stream(&file);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.setVersion(QDataStream::Qt_6_0);
        char magic[sizeof(SnapshotMagic)] = {};
        if (stream.readRawData(magic, sizeof(magic)) != sizeof(magic)
            || std::memcmp(magic, SnapshotMagic, sizeof(magic)) != 0) {
            setError(errorMessage, QStringLiteral("This is not a VFX Photo Lab session snapshot."));
            return false;
        }
        quint32 formatVersion = 0;
        stream >> formatVersion;
        if (formatVersion < 2 || formatVersion > SnapshotFormatVersion) {
            setError(errorMessage, QStringLiteral("Unsupported session snapshot version: %1.")
                                       .arg(formatVersion));
            return false;
        }

        PhotoDocument restored;
        qint32 colourModelValue = 0;
        if (!readString(stream, &restored.m_sourcePath, errorMessage)
            || !readString(stream, &restored.m_projectPath, errorMessage)
            || !readString(stream, &restored.m_documentName, errorMessage)) {
            return false;
        }
        if (formatVersion >= 28) {
            stream >> restored.m_documentIdentity;
            if (restored.m_documentIdentity.isNull()) {
                setError(errorMessage, QStringLiteral(
                    "The session snapshot contains an invalid document identity."));
                return false;
            }
        } else {
            if (restored.m_projectPath.isEmpty()) {
                restored.m_documentIdentity = QUuid::createUuid();
            } else {
                QFileInfo identityInfo(restored.m_projectPath);
                QString identityPath = identityInfo.canonicalFilePath();
                if (identityPath.isEmpty()) identityPath = identityInfo.absoluteFilePath();
                QByteArray identityMaterial;
                QFile identityFile(identityPath);
                if (identityFile.open(QIODevice::ReadOnly)) {
                    QCryptographicHash contentHash(QCryptographicHash::Sha256);
                    while (!identityFile.atEnd()) {
                        const QByteArray chunk = identityFile.read(1024 * 1024);
                        if (chunk.isEmpty() && identityFile.error() != QFile::NoError) {
                            identityMaterial.clear();
                            break;
                        }
                        contentHash.addData(chunk);
                    }
                    if (identityFile.error() == QFile::NoError) {
                        identityMaterial = QByteArrayLiteral(
                            "VFXPhotoLab/legacy-project-content/")
                            + contentHash.result();
                    }
                }
                if (identityMaterial.isEmpty()) {
                    identityMaterial = QByteArrayLiteral(
                        "VFXPhotoLab/legacy-project-path/")
                        + identityPath.toUtf8();
                }
                QByteArray seed = QCryptographicHash::hash(
                    identityMaterial, QCryptographicHash::Sha256).left(16);
                if (seed.size() == 16) {
                    seed[6] = char((quint8(seed[6]) & 0x0fU) | 0x50U);
                    seed[8] = char((quint8(seed[8]) & 0x3fU) | 0x80U);
                    restored.m_documentIdentity = QUuid::fromRfc4122(seed);
                }
                if (restored.m_documentIdentity.isNull())
                    restored.m_documentIdentity = QUuid::createUuid();
            }
        }
        stream >> colourModelValue
               >> restored.m_blankDocument
               >> restored.m_resolutionX
               >> restored.m_resolutionY
               >> restored.m_modified;
        if (colourModelValue < static_cast<qint32>(DocumentColourModel::Rgb)
            || colourModelValue > static_cast<qint32>(DocumentColourModel::Grayscale)
            || !std::isfinite(restored.m_resolutionX)
            || !std::isfinite(restored.m_resolutionY)) {
            setError(errorMessage, QStringLiteral("The session snapshot contains invalid document metadata."));
            return false;
        }
        restored.m_colourModel = static_cast<DocumentColourModel>(colourModelValue);
        restored.m_resolutionX = std::clamp(restored.m_resolutionX, 1.0, 9600.0);
        restored.m_resolutionY = std::clamp(restored.m_resolutionY, 1.0, 9600.0);
        if (!readImage(stream, &restored.m_sourceImage, errorMessage)
            || restored.m_sourceImage.isNull()) {
            if (errorMessage && errorMessage->isEmpty()) {
                setError(errorMessage, QStringLiteral("The session snapshot has no source image."));
            }
            return false;
        }
        if (formatVersion >= 16) {
            QByteArray colourStateBytes;
            if (!readSizedBytes(stream,
                                &colourStateBytes,
                                MaximumColourStateBytes,
                                errorMessage)) {
                return false;
            }
            QJsonParseError colourStateParseError;
            const QJsonDocument colourStateJson = QJsonDocument::fromJson(
                colourStateBytes, &colourStateParseError);
            if (!colourStateJson.isObject()) {
                setError(errorMessage,
                         QStringLiteral("The session snapshot colour state is invalid JSON: %1")
                             .arg(colourStateParseError.errorString()));
                return false;
            }
            QString colourStateError;
            const auto colourState = DocumentColourState::fromJson(
                colourStateJson.object(), &colourStateError);
            if (!colourState) {
                setError(errorMessage,
                         QStringLiteral("The session snapshot colour state is invalid: %1")
                             .arg(colourStateError));
                return false;
            }
            restored.m_colourState = *colourState;
            restored.m_sourceImage.setColorSpace(
                documentWorkingQtSpace(restored.m_colourState));
        } else {
            restored.m_colourState = DocumentColourState::legacyForImage(
                restored.m_sourceImage.colorSpace());
        }

        quint32 rootLayerCount = 0;
        stream >> rootLayerCount;
        if (stream.status() != QDataStream::Ok || rootLayerCount > MaximumLayerCount) {
            setError(errorMessage, QStringLiteral("The session snapshot contains an invalid layer count."));
            return false;
        }
        quint32 totalLayerCount = 0;
        restored.m_layers.reserve(static_cast<qsizetype>(rootLayerCount));
        for (quint32 index = 0; index < rootLayerCount; ++index) {
            LayerNode layer;
            if (!readLayer(stream, &layer, 0, &totalLayerCount,
                           formatVersion, errorMessage)) {
                return false;
            }
            restored.m_layers.push_back(std::move(layer));
        }
        if (formatVersion >= 18) {
            QByteArray smartSourceBytes;
            if (!readSizedBytes(stream, &smartSourceBytes, MaximumSmartSourceBytes, errorMessage)) {
                return false;
            }
            QJsonParseError smartSourceParseError;
            const QJsonDocument smartSourceJson = QJsonDocument::fromJson(
                smartSourceBytes, &smartSourceParseError);
            if (!smartSourceJson.isArray()) {
                setError(errorMessage, QStringLiteral("The session snapshot Smart Source registry is invalid JSON: %1")
                    .arg(smartSourceParseError.errorString()));
                return false;
            }
            if (formatVersion < 28) {
                for (const QJsonValue &sourceValue : smartSourceJson.array()) {
                    if (!sourceValue.isObject()) continue;
                    const QJsonObject sourceObject = sourceValue.toObject();
                    if (sourceObject.value(QStringLiteral("schema")).toInt(1) >= 3
                        || sourceObject.contains(QStringLiteral("linkedDocumentId"))
                        || sourceObject.contains(QStringLiteral("linkedFingerprint"))) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-version-28 session snapshot cannot contain linked Smart Source identity metadata."));
                        return false;
                    }
                }
            }
            bool smartSourcesOk = false;
            restored.m_smartSources = SmartSourceRegistry::fromJson(
                smartSourceJson.array(), &smartSourcesOk, &restored.m_loadWarnings);
            if (!smartSourcesOk) {
                setError(errorMessage, QStringLiteral("The session snapshot contains an invalid Smart Source graph."));
                return false;
            }
            if (formatVersion < 19) {
                for (const SmartSourceDescriptor &source : restored.m_smartSources.descriptors()) {
                    if (source.hasEmbeddedDocument() || !source.presentationImage.isNull()) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-version-19 session snapshot cannot contain embedded Smart Source contents."));
                        return false;
                    }
                }
            }
            if (formatVersion < 20) {
                for (const SmartSourceDescriptor &source : restored.m_smartSources.descriptors()) {
                    if (source.hasEmbeddedDocument()
                        && source.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 2) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-version-20 session snapshot cannot contain embedded Smart Source precision metadata."));
                        return false;
                    }
                }
            }
            if (formatVersion < 21) {
                for (const SmartSourceDescriptor &source : restored.m_smartSources.descriptors()) {
                    if (source.hasEmbeddedDocument()
                        && source.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 3) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-version-21 session snapshot cannot contain embedded Smart transform metadata."));
                        return false;
                    }
                }
            }
            if (formatVersion < 22) {
                for (const SmartSourceDescriptor &source : restored.m_smartSources.descriptors()) {
                    if (source.hasEmbeddedDocument()
                        && source.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 4) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-version-22 session snapshot cannot contain embedded Live Filter metadata."));
                        return false;
                    }
                }
            }
            if (formatVersion < 23) {
                for (const SmartSourceDescriptor &source : restored.m_smartSources.descriptors()) {
                    if (source.hasEmbeddedDocument()
                        && source.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 5) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-version-23 session snapshot cannot contain embedded Live Filter mask metadata."));
                        return false;
                    }
                }
            }
            if (formatVersion < 24) {
                for (const SmartSourceDescriptor &source : restored.m_smartSources.descriptors()) {
                    if (source.hasEmbeddedDocument()
                        && source.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 6) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-version-24 session snapshot cannot contain embedded Layer Effect metadata."));
                        return false;
                    }
                }
            }
            if (formatVersion < 25) {
                for (const SmartSourceDescriptor &source : restored.m_smartSources.descriptors()) {
                    if (source.hasEmbeddedDocument()
                        && source.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 7) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-version-25 session snapshot cannot contain embedded Layer Effect renderer parameters."));
                        return false;
                    }
                }
            }
            if (formatVersion < 26) {
                for (const SmartSourceDescriptor &source : restored.m_smartSources.descriptors()) {
                    if (source.hasEmbeddedDocument()
                        && source.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 8) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-version-26 session snapshot cannot contain embedded Stroke/Overlay Layer Effect parameters."));
                        return false;
                    }
                }
            }
            if (formatVersion < 27) {
                for (const SmartSourceDescriptor &source : restored.m_smartSources.descriptors()) {
                    if (source.hasEmbeddedDocument()
                        && source.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 9) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-version-27 session snapshot cannot contain embedded Bevel & Emboss Layer Effect parameters."));
                        return false;
                    }
                }
            }
            if (formatVersion < 28) {
                for (const SmartSourceDescriptor &source : restored.m_smartSources.descriptors()) {
                    if (source.hasEmbeddedDocument()
                        && source.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 10) {
                        setError(errorMessage, QStringLiteral(
                            "A pre-version-28 session snapshot cannot contain linked-source-capable embedded Smart metadata."));
                        return false;
                    }
                }
            }
        }
        setRasterLayerColourSpaceRecursive(
            restored.m_layers, restored.m_sourceImage.colorSpace());
        if (formatVersion >= 28) {
            QHash<QUuid, quint64> linkedChanges;
            QStringList linkedWarnings;
            QString linkedError;
            if (!restored.refreshLinkedSmartSources(&linkedChanges, &linkedWarnings, &linkedError)) {
                setError(errorMessage, linkedError.isEmpty()
                    ? QStringLiteral("The session snapshot linked Smart Sources could not be restored safely.")
                    : linkedError);
                return false;
            }
        }
        if (!readGuideVector(stream, &restored.m_horizontalGuides, errorMessage)
            || !readGuideVector(stream, &restored.m_verticalGuides, errorMessage)) {
            return false;
        }
        if (formatVersion >= 4) {
            if (!readSelectionMask(stream,
                                   restored.m_sourceImage.size(),
                                   &restored.m_selectionMask,
                                   errorMessage)) {
                return false;
            }
        } else {
            restored.m_selectionMask.reset(restored.m_sourceImage.size());
        }

        bool baselineRequiresSave = false;
        QUuid editTargetLayerId;
        qint32 editTargetValue = 0;
        qint32 channelViewValue = 0;
        QVector<QUuid> selectedLayerIds;
        double zoom = 1.0;
        QPoint scrollPosition;
        bool fitToView = false;
        bool viewValid = false;
        bool selectionEdgesVisible = true;
        CropSessionState cropState;
        qint32 cropModeValue = static_cast<qint32>(CropMode::Free);
        qint32 cropOverlayValue = static_cast<qint32>(CropOverlay::RuleOfThirds);
        stream >> baselineRequiresSave
               >> editTargetLayerId
               >> editTargetValue
               >> channelViewValue;
        if (!readUuidVector(stream, &selectedLayerIds, errorMessage)) {
            return false;
        }
        stream >> zoom
               >> scrollPosition;
        if (formatVersion >= 3) {
            stream >> fitToView;
        }
        stream >> viewValid;
        if (formatVersion >= 4) {
            stream >> selectionEdgesVisible;
        }
        if (formatVersion >= 5) {
            stream >> cropState.initialised
                   >> cropState.frame
                   >> cropModeValue
                   >> cropState.ratioWidth
                   >> cropState.ratioHeight
                   >> cropState.originalRatio
                   >> cropState.fixedSize
                   >> cropOverlayValue
                   >> cropState.overlayOrientation
                   >> cropState.dimOpacity
                   >> cropState.snappingEnabled
                   >> cropState.deleteCroppedPixels
                   >> cropState.straightenSampling
                   >> cropState.straightenAngle;
        }
        DocumentSession::SmartSourceEditBinding sourceEditBinding;
        if (formatVersion >= 20) {
            bool sourceEditor = false;
            stream >> sourceEditor;
            if (sourceEditor) {
                stream >> sourceEditBinding.ownerSessionId >> sourceEditBinding.sourceId;
                if (!readString(stream, &sourceEditBinding.sourceName, errorMessage)) return false;
                quint32 baselineCount = 0;
                stream >> baselineCount;
                if (stream.status() != QDataStream::Ok
                    || baselineCount > static_cast<quint32>(SmartSourceRegistry::MaximumSources)) {
                    setError(errorMessage, QStringLiteral(
                        "The session snapshot contains an invalid Smart Source editor baseline."));
                    return false;
                }
                for (quint32 i = 0; i < baselineCount; ++i) {
                    QUuid id;
                    quint64 revision = 0;
                    stream >> id >> revision;
                    if (stream.status() != QDataStream::Ok || id.isNull() || revision < 1
                        || sourceEditBinding.baselineSourceRevisions.contains(id)) {
                        setError(errorMessage, QStringLiteral(
                            "The session snapshot contains an invalid Smart Source editor baseline."));
                        return false;
                    }
                    sourceEditBinding.baselineSourceRevisions.insert(id, revision);
                }
                if (!sourceEditBinding.isValid()
                    || !sourceEditBinding.baselineSourceRevisions.contains(sourceEditBinding.sourceId)) {
                    setError(errorMessage, QStringLiteral(
                        "The session snapshot contains an incomplete Smart Source editor binding."));
                    return false;
                }
            }
        }
        if (stream.status() != QDataStream::Ok || !stream.atEnd()
            || !validEditTarget(editTargetValue) || !validChannelView(channelViewValue)
            || !std::isfinite(zoom) || zoom <= 0.0
            || cropModeValue < static_cast<qint32>(CropMode::Free)
            || cropModeValue > static_cast<qint32>(CropMode::FixedSize)
            || cropOverlayValue < static_cast<qint32>(CropOverlay::None)
            || cropOverlayValue > static_cast<qint32>(CropOverlay::GoldenSpiral)
            || !std::isfinite(cropState.ratioWidth)
            || !std::isfinite(cropState.ratioHeight)
            || !std::isfinite(cropState.dimOpacity)
            || !std::isfinite(cropState.straightenAngle)) {
            setError(errorMessage, QStringLiteral("The session snapshot contains invalid editor state."));
            return false;
        }

        QSet<QUuid> ids;
        int baseCount = 0;
        if (!validateLayerTree(restored.m_layers,
                               restored.m_sourceImage.size(),
                               &ids,
                               &baseCount)
            || baseCount > 1
            || !restored.m_smartSources.validate()
            || !smartLayerReferencesResolve(restored.m_layers, restored.m_smartSources)) {
            setError(errorMessage, QStringLiteral("The session snapshot layer tree is inconsistent."));
            return false;
        }
        QString embeddedSourceError;
        for (const SmartSourceDescriptor &source : restored.m_smartSources.descriptors()) {
            if (!source.hasEmbeddedDocument()) continue;
            QVector<LayerNode> embeddedLayers;
            if (!restored.embeddedSmartSourceLayers(source.id, &embeddedLayers, nullptr,
                                                    nullptr, &embeddedSourceError)
                || !source.hasCurrentPresentation()) {
                setError(errorMessage, embeddedSourceError.isEmpty()
                    ? QStringLiteral("The session snapshot contains invalid embedded Smart Source contents.")
                    : embeddedSourceError);
                return false;
            }
        }
        QString smartPresentationError;
        if (!restored.synchronizeSmartLayerPresentations(&smartPresentationError)) {
            setError(errorMessage, smartPresentationError.isEmpty()
                ? QStringLiteral("The session snapshot Smart Layer presentation could not be restored safely.")
                : smartPresentationError);
            return false;
        }
        restored.promoteLegacyBaseLayers();
        if (!restored.synchronizeSmartLayerPresentations(&smartPresentationError)) {
            setError(errorMessage, smartPresentationError.isEmpty()
                ? QStringLiteral("The restored legacy layer tree could not bind Smart Layer presentations safely.")
                : smartPresentationError);
            return false;
        }
        restored.m_horizontalGuides.erase(
            std::remove_if(restored.m_horizontalGuides.begin(),
                           restored.m_horizontalGuides.end(),
                           [&](const double value) {
                               return !std::isfinite(value) || value < 0.0
                                   || value > restored.m_sourceImage.height();
                           }),
            restored.m_horizontalGuides.end());
        restored.m_verticalGuides.erase(
            std::remove_if(restored.m_verticalGuides.begin(),
                           restored.m_verticalGuides.end(),
                           [&](const double value) {
                               return !std::isfinite(value) || value < 0.0
                                   || value > restored.m_sourceImage.width();
                           }),
            restored.m_verticalGuides.end());
        restored.m_loadWarnings.clear();
        restored.m_colourResourceWarnings =
            auditDocumentColourResources(restored.m_colourState).messages();
        restored.rebuildPreviewSource();

        const bool preserveHistory = session->m_residency == SessionResidency::Cold;
        session->m_applyingUndoRedo = true;
        if (!preserveHistory) {
            session->m_undoStack->clear();
            session->m_undoStack->setClean();
            *session->m_rasterHistoryStats = {};
            *session->m_maskHistoryStats = {};
            *session->m_channelHistoryStats = {};
            *session->m_structuralHistoryStats = {};
            *session->m_selectionHistoryStats = {};
        }
        session->m_document = std::move(restored);
        if (preserveHistory) {
            const bool priorBaseline = session->m_historyDiscardedForColdStorage
                ? session->m_baselineRequiresSave
                : baselineRequiresSave;
            session->m_baselineRequiresSave = priorBaseline
                || session->m_document.isModified();
        } else {
            session->m_baselineRequiresSave = baselineRequiresSave
                || session->m_document.isModified();
        }
        session->m_editTargetLayerId = ids.contains(editTargetLayerId)
            ? editTargetLayerId : session->m_document.baseLayerId();
        session->m_editTarget = static_cast<LayerEditTarget>(editTargetValue);
        session->m_channelView = static_cast<ChannelView>(channelViewValue);
        session->m_selectedLayerIds.clear();
        for (const QUuid &id : selectedLayerIds) {
            if (ids.contains(id) && !session->m_selectedLayerIds.contains(id)) {
                session->m_selectedLayerIds.push_back(id);
            }
        }
        session->m_viewState.zoom = zoom;
        session->m_viewState.scrollPosition = scrollPosition;
        session->m_viewState.fitToView = fitToView;
        session->m_viewState.valid = viewValid;
        session->m_selectionEdgesVisible = selectionEdgesVisible;
        cropState.mode = static_cast<CropMode>(cropModeValue);
        cropState.overlay = static_cast<CropOverlay>(cropOverlayValue);
        cropState.dimOpacity = std::clamp(cropState.dimOpacity, 0.0, 0.95);
        cropState.ratioWidth = std::max(1.0, cropState.ratioWidth);
        cropState.ratioHeight = std::max(1.0, cropState.ratioHeight);
        if (cropState.fixedSize.isEmpty()) {
            cropState.fixedSize = session->m_document.sourceImage().size();
        }
        if (cropState.frame.isEmpty()) {
            cropState.initialised = false;
        }
        session->m_cropState = cropState;
        session->m_smartSourceEditBinding = std::move(sourceEditBinding);
        session->m_propertyUndoActive = false;
        session->m_propertyUndoText.clear();
        session->m_propertyUndoBefore = {};
        session->m_propertyUndoSelection.clear();
        session->m_thumbnailCache.clear();
        session->m_applyingUndoRedo = false;
        session->refreshSummary();
        return true;
    }
};

SessionCacheStore::SessionCacheStore(QString rootPath)
{
    if (rootPath.trimmed().isEmpty()) {
        rootPath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (rootPath.trimmed().isEmpty()) {
            rootPath = QDir::tempPath() + QStringLiteral("/VFXPhotoLab");
        }
        rootPath += QStringLiteral("/document-sessions-v1");
    }
    m_rootPath = QDir::cleanPath(rootPath);
    QDir root;
    if (!root.mkpath(m_rootPath)) {
        return;
    }

    cleanupStaleRuns();
    const QString runName = QStringLiteral("run-%1-%2")
        .arg(QCoreApplication::applicationPid())
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_runPath = QDir(m_rootPath).filePath(runName);
    if (!root.mkpath(m_runPath)) {
        m_runPath.clear();
        return;
    }
    m_runLock = std::make_unique<QLockFile>(
        QDir(m_runPath).filePath(QStringLiteral("session.lock")));
    // Process/PID ownership remains authoritative; age alone must never let a
    // second VFX Photo Lab instance delete a long-running live workspace.
    m_runLock->setStaleLockTime(0);
    if (!m_runLock->tryLock(0)) {
        m_runLock.reset();
        QDir(m_runPath).removeRecursively();
        m_runPath.clear();
        return;
    }
    m_available = true;
}

SessionCacheStore::~SessionCacheStore()
{
    if (m_runLock) {
        m_runLock->unlock();
        m_runLock.reset();
    }
    if (!m_runPath.isEmpty()) {
        QDir(m_runPath).removeRecursively();
    }
}

bool SessionCacheStore::isAvailable() const
{
    return m_available;
}

const QString &SessionCacheStore::rootPath() const
{
    return m_rootPath;
}

const QString &SessionCacheStore::runPath() const
{
    return m_runPath;
}

QString SessionCacheStore::snapshotPath(const QUuid &sessionId) const
{
    if (!m_available || sessionId.isNull()) {
        return {};
    }
    return QDir(m_runPath).filePath(
        sessionId.toString(QUuid::WithoutBraces) + QStringLiteral(".vfxsession"));
}

bool SessionCacheStore::writeSnapshot(const DocumentSession &session,
                                      QString *writtenPath,
                                      qint64 *writtenBytes,
                                      QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (writtenPath) {
        writtenPath->clear();
    }
    if (writtenBytes) {
        *writtenBytes = 0;
    }
    if (!m_available) {
        setError(errorMessage, QStringLiteral("The private session cache directory is unavailable."));
        return false;
    }
    const QString path = snapshotPath(session.id());
    if (!SessionSnapshotCodec::write(path, session, errorMessage)) {
        return false;
    }
    if (writtenPath) {
        *writtenPath = path;
    }
    if (writtenBytes) {
        *writtenBytes = fileSizeOrZero(path);
    }
    return true;
}

bool SessionCacheStore::restoreSnapshot(const QString &filePath,
                                        DocumentSession *session,
                                        QString *errorMessage) const
{
    if (errorMessage) {
        errorMessage->clear();
    }
    return SessionSnapshotCodec::read(filePath, session, errorMessage);
}

bool SessionCacheStore::removeSnapshot(const QString &filePath,
                                       QString *errorMessage) const
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (filePath.isEmpty() || !QFileInfo::exists(filePath)) {
        return true;
    }
    if (!QFile::remove(filePath)) {
        setError(errorMessage, QStringLiteral("Could not remove the private session snapshot."));
        return false;
    }
    return true;
}

int SessionCacheStore::cleanupStaleRuns(const int maximumAgeDays) const
{
    if (m_rootPath.isEmpty()) {
        return 0;
    }
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-std::max(1, maximumAgeDays));
    QDir root(m_rootPath);
    int removed = 0;
    const QFileInfoList entries = root.entryInfoList(
        QStringList{QStringLiteral("run-*")},
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Time);
    for (const QFileInfo &entry : entries) {
        if (entry.absoluteFilePath() == m_runPath || entry.lastModified().toUTC() > cutoff) {
            continue;
        }
        QLockFile lock(QDir(entry.absoluteFilePath()).filePath(
            QStringLiteral("session.lock")));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) {
            // A live process still owns this run, even if it has been open for
            // longer than the cleanup age.
            continue;
        }
        lock.unlock();
        if (QDir(entry.absoluteFilePath()).removeRecursively()) {
            ++removed;
        }
    }
    return removed;
}

DocumentResidencyManager::DocumentResidencyManager()
    : DocumentResidencyManager(Limits{}, {})
{
}

DocumentResidencyManager::DocumentResidencyManager(Limits limits, QString cacheRoot)
    : m_limits(std::move(limits))
    , m_cacheStore(std::move(cacheRoot))
{
    setLimits(m_limits);
}

DocumentResidencyManager::~DocumentResidencyManager() = default;

void DocumentResidencyManager::registerSession(DocumentSession *session)
{
    if (!session || m_entries.contains(session)) {
        return;
    }
    m_entries.insert(session, Entry{++m_accessCounter});
    session->refreshSummary();
}

void DocumentResidencyManager::unregisterSession(DocumentSession *session,
                                                 const bool removeBackingSnapshot)
{
    if (!session) {
        return;
    }
    if (removeBackingSnapshot) {
        discardBackingSnapshot(session);
    }
    m_entries.remove(session);
    if (m_activeSession == session) {
        m_activeSession = nullptr;
    }
}

bool DocumentResidencyManager::activateSession(DocumentSession *session,
                                               QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (!session) {
        setError(errorMessage, QStringLiteral("No document session was selected."));
        return false;
    }
    registerSession(session);
    if (session->m_residency == SessionResidency::Cold
        && !session->restoreFromDisk(m_cacheStore, errorMessage)) {
        return false;
    }
    if (m_activeSession && m_activeSession != session
        && m_activeSession->m_residency == SessionResidency::Hot) {
        m_activeSession->refreshSummary();
        m_activeSession->m_residency = SessionResidency::Warm;
    }
    m_activeSession = session;
    session->m_residency = SessionResidency::Hot;
    touchSession(session);
    // A failed eviction must never block document activation. The workspace may
    // temporarily exceed its target budget and can report the cache-write error,
    // but the requested document remains usable.
    enforceLimits(errorMessage);
    return true;
}

void DocumentResidencyManager::touchSession(DocumentSession *session)
{
    auto iterator = m_entries.find(session);
    if (iterator != m_entries.end()) {
        iterator->accessOrder = ++m_accessCounter;
    }
}

bool DocumentResidencyManager::enforceLimits(QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    while (warmSessionCount() > m_limits.warmSessionCount
           || residentBytes() > m_limits.residentDocumentBytes) {
        DocumentSession *candidate = oldestWarmSession();
        if (!candidate) {
            break;
        }
        if (!candidate->evictToDisk(m_cacheStore, errorMessage)) {
            // Keep the session warm and mark it recently used so one failed
            // write cannot cause an infinite eviction loop.
            touchSession(candidate);
            return false;
        }
        if (m_onColdEviction) {
            m_onColdEviction(candidate->id());
        }
    }

    // Cold storage normally retains Undo/Redo. Purge the oldest inactive
    // checkpoint only when the active decoded pixels can fit within the hard
    // target and retained history is the remaining reason it is exceeded. A
    // single oversized active document must not destroy unrelated histories
    // when no amount of history trimming could satisfy the target.
    if (residentPixelBytes() <= m_limits.residentDocumentBytes) {
        while (residentBytes() > m_limits.residentDocumentBytes) {
            DocumentSession *candidate = oldestColdSessionWithHistory();
            if (!candidate) {
                break;
            }
            candidate->discardUndoHistoryForMemoryPressure();
            touchSession(candidate);
        }
    }
    const bool withinLimits = residentBytes() <= m_limits.residentDocumentBytes
        && warmSessionCount() <= m_limits.warmSessionCount;
    if (!withinLimits && errorMessage && errorMessage->isEmpty()) {
        setError(errorMessage,
                 QStringLiteral("The active document or unavailable private cache keeps the workspace above its memory target."));
    }
    return withinLimits;
}

void DocumentResidencyManager::discardBackingSnapshot(DocumentSession *session)
{
    if (!session) {
        return;
    }
    session->discardBackingSnapshot(m_cacheStore);
}

void DocumentResidencyManager::setLimits(const Limits &limits)
{
    m_limits.residentDocumentBytes = std::max<qint64>(64LL * 1024LL * 1024LL,
                                                      limits.residentDocumentBytes);
    m_limits.warmSessionCount = std::max(0, limits.warmSessionCount);
}

const DocumentResidencyManager::Limits &DocumentResidencyManager::limits() const
{
    return m_limits;
}

DocumentResidencyManager::Stats DocumentResidencyManager::stats() const
{
    Stats result;
    result.registeredSessions = static_cast<int>(m_entries.size());
    for (auto iterator = m_entries.cbegin(); iterator != m_entries.cend(); ++iterator) {
        const DocumentSession *session = iterator.key();
        if (!session) {
            continue;
        }
        switch (session->residency()) {
        case SessionResidency::Hot: ++result.hotSessions; break;
        case SessionResidency::Warm: ++result.warmSessions; break;
        case SessionResidency::Cold: ++result.coldSessions; break;
        }
        result.residentDocumentBytes += session->estimatedResidentBytes();
        result.historyBytes += session->estimatedHistoryBytes();
        result.backingBytes += session->backingSnapshotBytes();
        if (session->historyWasDiscardedForColdStorage()) {
            ++result.historiesDiscardedForColdStorage;
        }
    }
    return result;
}

const SessionCacheStore &DocumentResidencyManager::cacheStore() const
{
    return m_cacheStore;
}

SessionCacheStore &DocumentResidencyManager::cacheStore()
{
    return m_cacheStore;
}

void DocumentResidencyManager::setColdEvictionCallback(
    std::function<void(const QUuid &)> callback)
{
    m_onColdEviction = std::move(callback);
}

DocumentSession *DocumentResidencyManager::oldestWarmSession() const
{
    DocumentSession *candidate = nullptr;
    quint64 oldestOrder = std::numeric_limits<quint64>::max();
    for (auto iterator = m_entries.cbegin(); iterator != m_entries.cend(); ++iterator) {
        DocumentSession *session = iterator.key();
        if (!session || session == m_activeSession
            || session->residency() != SessionResidency::Warm) {
            continue;
        }
        if (iterator->accessOrder < oldestOrder) {
            oldestOrder = iterator->accessOrder;
            candidate = session;
        }
    }
    return candidate;
}

DocumentSession *DocumentResidencyManager::oldestColdSessionWithHistory() const
{
    DocumentSession *candidate = nullptr;
    quint64 oldestOrder = std::numeric_limits<quint64>::max();
    for (auto iterator = m_entries.cbegin(); iterator != m_entries.cend(); ++iterator) {
        DocumentSession *session = iterator.key();
        if (!session || session == m_activeSession
            || session->residency() != SessionResidency::Cold
            || session->estimatedHistoryBytes() <= 0) {
            continue;
        }
        if (iterator->accessOrder < oldestOrder) {
            oldestOrder = iterator->accessOrder;
            candidate = session;
        }
    }
    return candidate;
}

int DocumentResidencyManager::warmSessionCount() const
{
    int result = 0;
    for (DocumentSession *session : m_entries.keys()) {
        if (session && session->residency() == SessionResidency::Warm) {
            ++result;
        }
    }
    return result;
}

qint64 DocumentResidencyManager::residentPixelBytes() const
{
    qint64 result = 0;
    for (DocumentSession *session : m_entries.keys()) {
        if (session) {
            result += session->estimatedResidentBytes();
        }
    }
    return result;
}

qint64 DocumentResidencyManager::residentBytes() const
{
    qint64 result = residentPixelBytes();
    for (DocumentSession *session : m_entries.keys()) {
        if (session) {
            result += session->estimatedHistoryBytes();
        }
    }
    return result;
}

} // namespace vfx
