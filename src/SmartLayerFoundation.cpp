#include "SmartLayerFoundation.h"

#include <QBuffer>
#include <QByteArray>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <limits>
#include <utility>

namespace vfx {
namespace {

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) *errorMessage = message;
}

constexpr int MaximumEmbeddedCanvasExtent = 32768;
constexpr qint64 MaximumPresentationPixels = qint64(64) * 1024 * 1024;
constexpr qint64 MaximumPresentationBytes = qint64(256) * 1024 * 1024;

QString encodePresentationImage(const QImage &image, bool *ok)
{
    if (ok) *ok = false;
    if (image.isNull()) {
        if (ok) *ok = true;
        return {};
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        return {};
    }
    if (ok) *ok = true;
    return QString::fromLatin1(encoded.toBase64());
}

QImage decodePresentationImage(const QString &encoded, bool *ok)
{
    if (ok) *ok = false;
    if (encoded.isEmpty()) {
        if (ok) *ok = true;
        return {};
    }
    const QByteArray bytes = QByteArray::fromBase64(
        encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (bytes.isEmpty()) return {};
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&buffer, "PNG");
    const QSize size = reader.size();
    if (size.isValid()
        && (size.width() < 1 || size.height() < 1
            || size.width() > MaximumEmbeddedCanvasExtent
            || size.height() > MaximumEmbeddedCanvasExtent
            || qint64(size.width()) * size.height() > MaximumPresentationPixels)) {
        return {};
    }
    QImage image = reader.read();
    if (image.isNull()
        || image.width() > MaximumEmbeddedCanvasExtent
        || image.height() > MaximumEmbeddedCanvasExtent
        || qint64(image.width()) * image.height() > MaximumPresentationPixels
        || image.sizeInBytes() > MaximumPresentationBytes) {
        return {};
    }
    if (ok) *ok = true;
    return image;
}

bool visitSource(const QUuid &id,
                 const QHash<QUuid, SmartSourceDescriptor> &sources,
                 QSet<QUuid> *visiting,
                 QSet<QUuid> *visited)
{
    if (visited->contains(id)) return true;
    if (visiting->contains(id)) return false;
    const auto iterator = sources.constFind(id);
    if (iterator == sources.cend()) return false;
    visiting->insert(id);
    for (const QUuid &dependency : iterator->dependencies) {
        if (!sources.contains(dependency)
            || !visitSource(dependency, sources, visiting, visited)) {
            return false;
        }
    }
    visiting->remove(id);
    visited->insert(id);
    return true;
}

} // namespace

QString smartSourceStorageToString(const SmartSourceStorage storage)
{
    switch (storage) {
    case SmartSourceStorage::Embedded: return QStringLiteral("embedded");
    case SmartSourceStorage::Linked: return QStringLiteral("linked");
    }
    return QStringLiteral("embedded");
}

SmartSourceStorage smartSourceStorageFromString(const QString &value, bool *ok)
{
    if (ok) *ok = true;
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("embedded")) return SmartSourceStorage::Embedded;
    if (normalised == QStringLiteral("linked")) return SmartSourceStorage::Linked;
    if (ok) *ok = false;
    return SmartSourceStorage::Embedded;
}

bool SmartTransformState::isSafe() const
{
    const int value = static_cast<int>(interpolation);
    return schema == CurrentSchema
        && value >= static_cast<int>(TransformInterpolation::NearestNeighbour)
        && value <= static_cast<int>(TransformInterpolation::Lanczos3);
}

QJsonObject SmartTransformState::toJson(bool *ok) const
{
    const bool safe = isSafe();
    if (ok) *ok = safe;
    if (!safe) return {};
    QJsonObject object;
    object.insert(QStringLiteral("schema"), static_cast<int>(schema));
    object.insert(QStringLiteral("interpolation"), static_cast<int>(interpolation));
    return object;
}

SmartTransformState SmartTransformState::fromJson(const QJsonObject &object, bool *ok)
{
    SmartTransformState result;
    result.schema = static_cast<quint32>(object.value(QStringLiteral("schema")).toInt(-1));
    result.interpolation = static_cast<TransformInterpolation>(
        object.value(QStringLiteral("interpolation")).toInt(-1));
    const bool safe = result.isSafe();
    if (ok) *ok = safe;
    return safe ? result : SmartTransformState {};
}

bool SmartLayerReference::isSafe() const
{
    return schema == CurrentSchema && !sourceId.isNull() && observedSourceRevision >= 1;
}

QJsonObject SmartLayerReference::toJson(bool *ok) const
{
    const bool safe = isSafe();
    if (ok) *ok = safe;
    if (!safe) return {};
    QJsonObject object;
    object.insert(QStringLiteral("schema"), static_cast<int>(schema));
    object.insert(QStringLiteral("sourceId"), sourceId.toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("observedRevision"),
                  QString::number(observedSourceRevision));
    return object;
}

SmartLayerReference SmartLayerReference::fromJson(const QJsonObject &object, bool *ok)
{
    SmartLayerReference result;
    bool revisionOk = false;
    result.schema = static_cast<quint32>(object.value(QStringLiteral("schema")).toInt(-1));
    result.sourceId = QUuid(object.value(QStringLiteral("sourceId")).toString());
    result.observedSourceRevision = object.value(QStringLiteral("observedRevision"))
        .toString().toULongLong(&revisionOk);
    const bool safe = revisionOk && result.isSafe();
    if (ok) *ok = safe;
    return safe ? result : SmartLayerReference {};
}

bool SmartSourceDescriptor::isSafe(QString *errorMessage) const
{
    if (schema != CurrentSchema || id.isNull() || revision < 1) {
        setError(errorMessage, QStringLiteral("The Smart Source identity is invalid."));
        return false;
    }
    if (name.size() > 4096 || linkedPath.size() > 32768 || linkedRuntimeWarning.size() > 32768) {
        setError(errorMessage, QStringLiteral("The Smart Source text metadata exceeds safety limits."));
        return false;
    }
    if (storage == SmartSourceStorage::Linked) {
        if (linkedPath.trimmed().isEmpty()) {
            setError(errorMessage, QStringLiteral("A linked Smart Source must retain its source path."));
            return false;
        }
        if (!linkedContentFingerprint.isEmpty() && linkedContentFingerprint.size() != 32) {
            setError(errorMessage, QStringLiteral("The linked Smart Source fingerprint is invalid."));
            return false;
        }
        if (!embeddedDocument.isEmpty()) {
            setError(errorMessage, QStringLiteral("A linked Smart Source cannot carry authoritative embedded contents."));
            return false;
        }
    } else if (!linkedPath.trimmed().isEmpty() || !linkedDocumentId.isNull()
               || !linkedContentFingerprint.isEmpty()
               || !linkedResolvedDocumentIds.isEmpty()) {
        setError(errorMessage, QStringLiteral("Embedded Smart Sources cannot carry linked-source metadata."));
        return false;
    }
    if (linkedResolvedDocumentIds.size() > MaximumDependencies) {
        setError(errorMessage, QStringLiteral("The resolved linked-document dependency closure exceeds safety limits."));
        return false;
    }
    QSet<QUuid> resolvedLinkedIds;
    for (const QUuid &resolvedId : linkedResolvedDocumentIds) {
        if (resolvedId.isNull() || resolvedLinkedIds.contains(resolvedId)) {
            setError(errorMessage, QStringLiteral("The resolved linked-document dependency closure is invalid."));
            return false;
        }
        resolvedLinkedIds.insert(resolvedId);
    }
    if (dependencies.size() > MaximumDependencies) {
        setError(errorMessage, QStringLiteral("The Smart Source has too many dependencies."));
        return false;
    }
    QSet<QUuid> unique;
    for (const QUuid &dependency : dependencies) {
        if (dependency.isNull() || dependency == id || unique.contains(dependency)) {
            setError(errorMessage, QStringLiteral("The Smart Source dependency list is invalid."));
            return false;
        }
        unique.insert(dependency);
    }

    if (!embeddedDocument.isEmpty()) {
        const int embeddedSchema = embeddedDocument.value(QStringLiteral("schema")).toInt(-1);
        const QJsonValue canvasValue = embeddedDocument.value(QStringLiteral("canvas"));
        if ((embeddedSchema < 1
             || embeddedSchema > static_cast<int>(EmbeddedDocumentSchema))
            || !canvasValue.isObject()) {
            setError(errorMessage, QStringLiteral("The embedded Smart Source document header is invalid."));
            return false;
        }
        const QJsonObject canvas = canvasValue.toObject();
        const int width = canvas.value(QStringLiteral("width")).toInt(-1);
        const int height = canvas.value(QStringLiteral("height")).toInt(-1);
        if (width < 1 || height < 1
            || width > MaximumEmbeddedCanvasExtent
            || height > MaximumEmbeddedCanvasExtent) {
            setError(errorMessage, QStringLiteral("The embedded Smart Source canvas exceeds safety limits."));
            return false;
        }
        if (!embeddedDocument.value(QStringLiteral("layers")).isArray()
            || !embeddedDocument.value(QStringLiteral("colourManagement")).isObject()) {
            setError(errorMessage, QStringLiteral("The embedded Smart Source document payload is incomplete."));
            return false;
        }
    }

    if (!presentationImage.isNull()) {
        if (presentationBounds.isEmpty()
            || presentationBounds.size() != presentationImage.size()
            || presentationImage.width() > MaximumEmbeddedCanvasExtent
            || presentationImage.height() > MaximumEmbeddedCanvasExtent
            || qint64(presentationImage.width()) * presentationImage.height()
                > MaximumPresentationPixels
            || presentationImage.sizeInBytes() > MaximumPresentationBytes
            || presentationRevision < 1 || presentationRevision > revision) {
            setError(errorMessage, QStringLiteral("The Smart Source presentation cache is invalid."));
            return false;
        }
    } else if (!presentationBounds.isEmpty() || presentationRevision != 0) {
        setError(errorMessage, QStringLiteral("The Smart Source presentation cache metadata is inconsistent."));
        return false;
    }
    return true;
}

qint64 SmartSourceDescriptor::estimatedBytes() const
{
    qint64 total = static_cast<qint64>(sizeof(SmartSourceDescriptor));
    total += static_cast<qint64>(name.capacity()) * static_cast<qint64>(sizeof(QChar));
    total += static_cast<qint64>(linkedPath.capacity()) * static_cast<qint64>(sizeof(QChar));
    total += linkedContentFingerprint.capacity();
    total += static_cast<qint64>(linkedRuntimeWarning.capacity()) * static_cast<qint64>(sizeof(QChar));
    total += static_cast<qint64>(linkedResolvedDocumentIds.capacity())
        * static_cast<qint64>(sizeof(QUuid));
    total += static_cast<qint64>(dependencies.capacity()) * static_cast<qint64>(sizeof(QUuid));
    total += embeddedDocument.isEmpty()
        ? 0
        : static_cast<qint64>(QJsonDocument(embeddedDocument)
                                  .toJson(QJsonDocument::Compact).size());
    if (!presentationImage.isNull()) {
        total += presentationImage.sizeInBytes();
    }
    return total;
}

bool SmartSourceDescriptor::operator==(const SmartSourceDescriptor &other) const
{
    const auto imagesMatch = [](const QImage &left, const QImage &right) {
        if (left.isNull() || right.isNull()) return left.isNull() == right.isNull();
        return left.cacheKey() == right.cacheKey()
            && left.size() == right.size()
            && left.format() == right.format()
            && left.devicePixelRatio() == right.devicePixelRatio();
    };
    return schema == other.schema
        && id == other.id
        && revision == other.revision
        && storage == other.storage
        && name == other.name
        && linkedPath == other.linkedPath
        && linkedDocumentId == other.linkedDocumentId
        && linkedContentFingerprint == other.linkedContentFingerprint
        && dependencies == other.dependencies
        && embeddedDocument == other.embeddedDocument
        && imagesMatch(presentationImage, other.presentationImage)
        && presentationBounds == other.presentationBounds
        && presentationRevision == other.presentationRevision;
}

QJsonObject SmartSourceDescriptor::toJson(bool *ok) const
{
    QString error;
    const bool safe = isSafe(&error);
    if (ok) *ok = safe;
    if (!safe) return {};
    QJsonObject object;
    object.insert(QStringLiteral("schema"), static_cast<int>(schema));
    object.insert(QStringLiteral("id"), id.toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("revision"), QString::number(revision));
    object.insert(QStringLiteral("storage"), smartSourceStorageToString(storage));
    object.insert(QStringLiteral("name"), name);
    if (storage == SmartSourceStorage::Linked) {
        object.insert(QStringLiteral("linkedPath"), linkedPath);
        if (!linkedDocumentId.isNull()) {
            object.insert(QStringLiteral("linkedDocumentId"),
                          linkedDocumentId.toString(QUuid::WithoutBraces));
        }
        if (!linkedContentFingerprint.isEmpty()) {
            object.insert(QStringLiteral("linkedFingerprint"),
                          QString::fromLatin1(linkedContentFingerprint.toHex()));
        }
    }
    QJsonArray dependencyArray;
    for (const QUuid &dependency : dependencies) {
        dependencyArray.append(dependency.toString(QUuid::WithoutBraces));
    }
    object.insert(QStringLiteral("dependencies"), dependencyArray);
    if (!embeddedDocument.isEmpty()) {
        object.insert(QStringLiteral("embeddedDocument"), embeddedDocument);
    }
    if (!presentationImage.isNull()) {
        bool imageOk = false;
        const QString encoded = encodePresentationImage(presentationImage, &imageOk);
        if (!imageOk) {
            if (ok) *ok = false;
            return {};
        }
        QJsonObject cache;
        cache.insert(QStringLiteral("revision"), QString::number(presentationRevision));
        cache.insert(QStringLiteral("x"), presentationBounds.x());
        cache.insert(QStringLiteral("y"), presentationBounds.y());
        cache.insert(QStringLiteral("width"), presentationBounds.width());
        cache.insert(QStringLiteral("height"), presentationBounds.height());
        cache.insert(QStringLiteral("encoding"), QStringLiteral("png-base64"));
        cache.insert(QStringLiteral("data"), encoded);
        object.insert(QStringLiteral("presentationCache"), cache);
    }
    return object;
}

SmartSourceDescriptor SmartSourceDescriptor::fromJson(const QJsonObject &object,
                                                       bool *ok,
                                                       QString *errorMessage)
{
    SmartSourceDescriptor result;
    const quint32 encodedSchema = static_cast<quint32>(
        object.value(QStringLiteral("schema")).toInt(-1));
    if (encodedSchema < 1 || encodedSchema > CurrentSchema) {
        if (ok) *ok = false;
        setError(errorMessage, QStringLiteral("The Smart Source schema is not supported."));
        return {};
    }
    result.schema = CurrentSchema;
    result.id = QUuid(object.value(QStringLiteral("id")).toString());
    bool revisionOk = false;
    result.revision = object.value(QStringLiteral("revision")).toString().toULongLong(&revisionOk);
    bool storageOk = false;
    result.storage = smartSourceStorageFromString(
        object.value(QStringLiteral("storage")).toString(), &storageOk);
    result.name = object.value(QStringLiteral("name")).toString();
    result.linkedPath = object.value(QStringLiteral("linkedPath")).toString();
    if (encodedSchema >= 3 && result.storage == SmartSourceStorage::Linked) {
        result.linkedDocumentId = QUuid(
            object.value(QStringLiteral("linkedDocumentId")).toString());
        const QString fingerprintHex =
            object.value(QStringLiteral("linkedFingerprint")).toString();
        if (!fingerprintHex.isEmpty()) {
            result.linkedContentFingerprint = QByteArray::fromHex(fingerprintHex.toLatin1());
            if (result.linkedContentFingerprint.size() != 32
                || QString::fromLatin1(result.linkedContentFingerprint.toHex())
                    != fingerprintHex.toLower()) {
                if (ok) *ok = false;
                setError(errorMessage, QStringLiteral("The linked Smart Source fingerprint is invalid."));
                return {};
            }
        }
    }
    const QJsonValue dependenciesValue = object.value(QStringLiteral("dependencies"));
    if (!dependenciesValue.isArray()) {
        if (ok) *ok = false;
        setError(errorMessage, QStringLiteral("The Smart Source dependency list is invalid."));
        return {};
    }
    const QJsonArray dependencies = dependenciesValue.toArray();
    if (dependencies.size() > MaximumDependencies) {
        if (ok) *ok = false;
        setError(errorMessage, QStringLiteral("The Smart Source dependency list exceeds safety limits."));
        return {};
    }
    result.dependencies.reserve(dependencies.size());
    for (const QJsonValue &value : dependencies) {
        if (!value.isString()) {
            if (ok) *ok = false;
            setError(errorMessage, QStringLiteral("The Smart Source contains an invalid dependency ID."));
            return {};
        }
        result.dependencies.push_back(QUuid(value.toString()));
    }

    if (encodedSchema >= 2) {
        const QJsonValue embeddedValue = object.value(QStringLiteral("embeddedDocument"));
        if (!embeddedValue.isUndefined() && !embeddedValue.isObject()) {
            if (ok) *ok = false;
            setError(errorMessage, QStringLiteral("The embedded Smart Source document is invalid."));
            return {};
        }
        result.embeddedDocument = embeddedValue.toObject();

        const QJsonValue cacheValue = object.value(QStringLiteral("presentationCache"));
        if (!cacheValue.isUndefined()) {
            if (!cacheValue.isObject()) {
                if (ok) *ok = false;
                setError(errorMessage, QStringLiteral("The Smart Source presentation cache is invalid."));
                return {};
            }
            const QJsonObject cache = cacheValue.toObject();
            if (cache.value(QStringLiteral("encoding")).toString()
                != QStringLiteral("png-base64")) {
                if (ok) *ok = false;
                setError(errorMessage, QStringLiteral("The Smart Source presentation encoding is invalid."));
                return {};
            }
            bool cacheRevisionOk = false;
            result.presentationRevision = cache.value(QStringLiteral("revision"))
                .toString().toULongLong(&cacheRevisionOk);
            result.presentationBounds = QRect(
                cache.value(QStringLiteral("x")).toInt(),
                cache.value(QStringLiteral("y")).toInt(),
                cache.value(QStringLiteral("width")).toInt(),
                cache.value(QStringLiteral("height")).toInt());
            bool imageOk = false;
            result.presentationImage = decodePresentationImage(
                cache.value(QStringLiteral("data")).toString(), &imageOk);
            if (!cacheRevisionOk || !imageOk || result.presentationImage.isNull()) {
                if (ok) *ok = false;
                setError(errorMessage, QStringLiteral("The Smart Source presentation cache is damaged."));
                return {};
            }
        }
    }
    result.linkedAvailable = false;
    result.linkedRuntimeWarning.clear();
    result.linkedResolvedDocumentIds.clear();
    QString safetyError;
    const bool safe = revisionOk && storageOk && result.isSafe(&safetyError);
    if (ok) *ok = safe;
    if (!safe) {
        setError(errorMessage, safetyError.isEmpty()
            ? QStringLiteral("The Smart Source descriptor is invalid.") : safetyError);
        return {};
    }
    return result;
}

qint64 SmartSourceRegistry::estimatedBytes() const
{
    qint64 total = static_cast<qint64>(sizeof(SmartSourceRegistry));
    for (auto iterator = m_sources.cbegin(); iterator != m_sources.cend(); ++iterator) {
        total += iterator.value().estimatedBytes();
        total += static_cast<qint64>(sizeof(QUuid));
    }
    return total;
}

QVector<SmartSourceDescriptor> SmartSourceRegistry::descriptors() const
{
    QVector<SmartSourceDescriptor> result;
    result.reserve(m_sources.size());
    for (auto iterator = m_sources.cbegin(); iterator != m_sources.cend(); ++iterator) {
        result.push_back(iterator.value());
    }
    std::sort(result.begin(), result.end(), [](const SmartSourceDescriptor &a,
                                               const SmartSourceDescriptor &b) {
        return a.id.toString(QUuid::WithoutBraces) < b.id.toString(QUuid::WithoutBraces);
    });
    return result;
}

const SmartSourceDescriptor *SmartSourceRegistry::find(const QUuid &id) const
{
    const auto iterator = m_sources.constFind(id);
    return iterator == m_sources.cend() ? nullptr : &iterator.value();
}

bool SmartSourceRegistry::insert(const SmartSourceDescriptor &descriptor,
                                 QString *errorMessage)
{
    if (m_sources.size() >= MaximumSources || m_sources.contains(descriptor.id)) {
        setError(errorMessage, QStringLiteral("The Smart Source identity already exists or the registry is full."));
        return false;
    }
    QString safetyError;
    if (!descriptor.isSafe(&safetyError)) {
        setError(errorMessage, safetyError);
        return false;
    }
    m_sources.insert(descriptor.id, descriptor);
    if (!validate(&safetyError)) {
        m_sources.remove(descriptor.id);
        setError(errorMessage, safetyError);
        return false;
    }
    return true;
}

bool SmartSourceRegistry::replace(const SmartSourceDescriptor &descriptor,
                                  QString *errorMessage)
{
    const auto existing = m_sources.constFind(descriptor.id);
    if (existing == m_sources.cend()) {
        setError(errorMessage, QStringLiteral("The Smart Source identity does not exist."));
        return false;
    }
    QString safetyError;
    if (!descriptor.isSafe(&safetyError)) {
        setError(errorMessage, safetyError);
        return false;
    }
    const SmartSourceDescriptor before = existing.value();
    // Revisions are owned by the registry mutation path. Keeping descriptor
    // replacement revision-neutral prevents callers from silently moving a
    // source backwards or skipping dependency invalidation. Actual source
    // edits use bumpRevisionCascade().
    if (descriptor.revision != before.revision) {
        setError(errorMessage, QStringLiteral(
            "Smart Source descriptor replacement cannot change its revision; use the revision invalidation path."));
        return false;
    }
    m_sources[descriptor.id] = descriptor;
    if (!validate(&safetyError)) {
        m_sources[descriptor.id] = before;
        setError(errorMessage, safetyError);
        return false;
    }
    return true;
}

bool SmartSourceRegistry::adoptEditedDescriptor(
    const SmartSourceDescriptor &descriptor,
    const quint64 expectedRevision,
    QString *errorMessage)
{
    const auto existing = m_sources.constFind(descriptor.id);
    if (existing == m_sources.cend()) {
        setError(errorMessage, QStringLiteral("The Smart Source identity does not exist."));
        return false;
    }
    if (existing->revision != expectedRevision) {
        setError(errorMessage, QStringLiteral(
            "The Smart Source changed in its owner while the source document was open."));
        return false;
    }

    SmartSourceDescriptor adopted = descriptor;
    adopted.revision = existing->revision;
    if (!adopted.presentationImage.isNull()) {
        adopted.presentationRevision = adopted.revision;
    } else {
        adopted.presentationBounds = {};
        adopted.presentationRevision = 0;
    }
    QString safetyError;
    if (!adopted.isSafe(&safetyError)) {
        setError(errorMessage, safetyError);
        return false;
    }

    const SmartSourceDescriptor before = existing.value();
    m_sources[descriptor.id] = std::move(adopted);
    if (!validate(&safetyError)) {
        m_sources[descriptor.id] = before;
        setError(errorMessage, safetyError);
        return false;
    }
    return true;
}

bool SmartSourceRegistry::remove(const QUuid &id)
{
    for (auto iterator = m_sources.cbegin(); iterator != m_sources.cend(); ++iterator) {
        if (iterator.key() != id && iterator->dependencies.contains(id)) {
            return false;
        }
    }
    return m_sources.remove(id) > 0;
}

bool SmartSourceRegistry::bumpRevision(const QUuid &id, quint64 *newRevision)
{
    auto iterator = m_sources.find(id);
    if (iterator == m_sources.end()) return false;
    if (iterator->revision == std::numeric_limits<quint64>::max()) return false;
    ++iterator->revision;
    if (newRevision) *newRevision = iterator->revision;
    return true;
}

bool SmartSourceRegistry::bumpRevisionCascade(
    const QUuid &id,
    QHash<QUuid, quint64> *changedRevisions)
{
    if (!m_sources.contains(id)) return false;

    QSet<QUuid> affected {id};
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto iterator = m_sources.cbegin(); iterator != m_sources.cend(); ++iterator) {
            if (affected.contains(iterator.key())) continue;
            for (const QUuid &dependency : iterator->dependencies) {
                if (affected.contains(dependency)) {
                    affected.insert(iterator.key());
                    changed = true;
                    break;
                }
            }
        }
    }

    for (const QUuid &affectedId : affected) {
        const auto iterator = m_sources.constFind(affectedId);
        if (iterator == m_sources.cend()
            || iterator->revision == std::numeric_limits<quint64>::max()) {
            return false;
        }
    }

    QHash<QUuid, quint64> revisions;
    revisions.reserve(affected.size());
    for (const QUuid &affectedId : affected) {
        auto iterator = m_sources.find(affectedId);
        ++iterator->revision;
        revisions.insert(affectedId, iterator->revision);
    }
    if (changedRevisions) *changedRevisions = std::move(revisions);
    return true;
}

bool SmartSourceRegistry::validate(QString *errorMessage) const
{
    if (m_sources.size() > MaximumSources) {
        setError(errorMessage, QStringLiteral("The Smart Source registry exceeds the safety limit."));
        return false;
    }
    for (auto iterator = m_sources.cbegin(); iterator != m_sources.cend(); ++iterator) {
        QString descriptorError;
        if (iterator.key() != iterator->id || !iterator->isSafe(&descriptorError)) {
            setError(errorMessage, descriptorError.isEmpty()
                ? QStringLiteral("The Smart Source registry contains inconsistent identity metadata.")
                : descriptorError);
            return false;
        }
        for (const QUuid &dependency : iterator->dependencies) {
            if (!m_sources.contains(dependency)) {
                setError(errorMessage, QStringLiteral("A Smart Source dependency cannot be resolved."));
                return false;
            }
        }
    }
    QSet<QUuid> visiting;
    QSet<QUuid> visited;
    for (auto iterator = m_sources.cbegin(); iterator != m_sources.cend(); ++iterator) {
        if (!visitSource(iterator.key(), m_sources, &visiting, &visited)) {
            setError(errorMessage, QStringLiteral("The Smart Source dependency graph contains a circular reference."));
            return false;
        }
    }
    return true;
}

bool SmartSourceRegistry::wouldIntroduceCycle(const QUuid &sourceId,
                                              const QVector<QUuid> &dependencies) const
{
    const auto iterator = m_sources.constFind(sourceId);
    if (iterator == m_sources.cend()) return true;
    SmartSourceRegistry candidate = *this;
    SmartSourceDescriptor descriptor = iterator.value();
    descriptor.dependencies = dependencies;
    candidate.m_sources[sourceId] = descriptor;
    QString error;
    return !candidate.validate(&error);
}

QJsonArray SmartSourceRegistry::toJson(bool *ok) const
{
    QString error;
    if (!validate(&error)) {
        if (ok) *ok = false;
        return {};
    }
    QJsonArray array;
    for (const SmartSourceDescriptor &descriptor : descriptors()) {
        bool descriptorOk = false;
        const QJsonObject object = descriptor.toJson(&descriptorOk);
        if (!descriptorOk) {
            if (ok) *ok = false;
            return {};
        }
        array.append(object);
    }
    if (ok) *ok = true;
    return array;
}

SmartSourceRegistry SmartSourceRegistry::fromJson(const QJsonArray &array,
                                                   bool *ok,
                                                   QStringList *warnings)
{
    SmartSourceRegistry result;
    if (array.size() > MaximumSources) {
        if (ok) *ok = false;
        return {};
    }
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            if (ok) *ok = false;
            return {};
        }
        bool descriptorOk = false;
        QString error;
        const SmartSourceDescriptor descriptor = SmartSourceDescriptor::fromJson(
            value.toObject(), &descriptorOk, &error);
        if (!descriptorOk || result.m_sources.contains(descriptor.id)) {
            if (warnings && !error.isEmpty()) warnings->push_back(error);
            if (ok) *ok = false;
            return {};
        }
        result.m_sources.insert(descriptor.id, descriptor);
    }
    QString validationError;
    const bool valid = result.validate(&validationError);
    if (!valid && warnings && !validationError.isEmpty()) {
        warnings->push_back(validationError);
    }
    if (ok) *ok = valid;
    return valid ? result : SmartSourceRegistry {};
}

} // namespace vfx
