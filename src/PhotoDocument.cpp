#include "PhotoDocument.h"

#include "ColourResourceAudit.h"
#include "ImageProcessor.h"
#include "OcioIntegration.h"
#include "SpatialFilter.h"
#include "SmartLayerTileCache.h"

#include "TgaCodec.h"
#include "ImageProfileImport.h"
#include "TransformSafety.h"

#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QSizeF>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace vfx {
namespace {

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

QString suffixForPath(const QString &filePath)
{
    return QFileInfo(filePath).suffix().trimmed().toLower();
}

constexpr double InchesPerMetre = 39.37007874015748;

int dotsPerMetreFromDpi(const double dpi)
{
    return std::max(1, qRound(std::clamp(dpi, 1.0, 9600.0) * InchesPerMetre));
}

double dpiFromDotsPerMetre(const int dotsPerMetre)
{
    return dotsPerMetre > 0 ? dotsPerMetre / InchesPerMetre : 72.0;
}

QColorSpace documentWorkingQtSpace(const DocumentColourState &state)
{
    if (state.workingSpace.kind == ColourSpaceKind::Ocio) {
        return ocioQtWorkingSpaceProxy(state.workingSpace);
    }
    return state.workingSpace.toQColorSpace();
}

bool sourceFormatIsGrayscale(const QImage::Format format)
{
    return format == QImage::Format_Grayscale8
        || format == QImage::Format_Grayscale16
        || format == QImage::Format_Mono
        || format == QImage::Format_MonoLSB;
}

constexpr int MaximumProjectVectorObjectCount = 100000;
constexpr qsizetype MaximumProjectVectorNodeCount = 4000000;
constexpr int MaximumProjectGuideCount = 65536;

thread_local QSet<QString> linkedProjectLoadStack;
thread_local int strictLinkedCycleDetectionDepth = 0;

QString canonicalProjectPath(const QString &path)
{
    if (path.trimmed().isEmpty()) return {};
    QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

QString resolvedLinkedPath(const QString &storedPath, const QString &ownerProjectPath)
{
    if (storedPath.trimmed().isEmpty()) return {};
    QFileInfo linkedInfo(storedPath);
    if (linkedInfo.isAbsolute()) return QDir::cleanPath(linkedInfo.absoluteFilePath());
    const QFileInfo ownerInfo(ownerProjectPath);
    const QString base = ownerProjectPath.trimmed().isEmpty()
        ? QDir::currentPath() : ownerInfo.absolutePath();
    return QDir(base).absoluteFilePath(storedPath);
}

QString persistedLinkedPath(const QString &absolutePath, const QString &ownerProjectPath)
{
    const QString cleanAbsolute = QDir::cleanPath(QFileInfo(absolutePath).absoluteFilePath());
    if (ownerProjectPath.trimmed().isEmpty()) return cleanAbsolute;
    return QDir(QFileInfo(ownerProjectPath).absolutePath()).relativeFilePath(cleanAbsolute);
}

QStringList linkedWarningsForRegistry(const SmartSourceRegistry &registry,
                                           const QString &ownerProjectPath)
{
    QStringList warnings;
    for (const SmartSourceDescriptor &source : registry.descriptors()) {
        if (source.storage != SmartSourceStorage::Linked) continue;
        if (source.linkedAvailable && source.linkedRuntimeWarning.isEmpty()) continue;
        const QString absolute = resolvedLinkedPath(source.linkedPath, ownerProjectPath);
        const QString name = source.name.isEmpty()
            ? QFileInfo(absolute).fileName() : source.name;
        const QString detail = source.linkedRuntimeWarning.isEmpty()
            ? QStringLiteral("The linked Smart Source is unavailable.")
            : source.linkedRuntimeWarning;
        warnings.push_back(QStringLiteral("%1: %2").arg(name, detail));
    }
    warnings.removeDuplicates();
    return warnings;
}

bool linkedDescriptorMatchesTrigger(const SmartSourceDescriptor &source,
                                    const QString &ownerProjectPath,
                                    const QUuid &documentId,
                                    const QString &projectPath)
{
    if (source.storage != SmartSourceStorage::Linked) return false;
    if (documentId.isNull() && projectPath.trimmed().isEmpty()) return true;
    if (!documentId.isNull()) {
        if (source.linkedDocumentId == documentId) return true;
        if (source.linkedResolvedDocumentIds.contains(documentId)) return true;
    }
    if (!projectPath.trimmed().isEmpty()) {
        const QString linkedCanonical = canonicalProjectPath(
            resolvedLinkedPath(source.linkedPath, ownerProjectPath));
        const QString triggerCanonical = canonicalProjectPath(projectPath);
        if (!linkedCanonical.isEmpty() && linkedCanonical == triggerCanonical) return true;
    }
    return false;
}

QByteArray fileSha256(const QString &path, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage, file.errorString());
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError) {
            setError(errorMessage, file.errorString());
            return {};
        }
        hash.addData(chunk);
    }
    return hash.result();
}

QUuid legacyProjectIdentityForPath(const QString &path)
{
    // Pre-0.14.0l projects do not yet carry a persistent UUID. Derive the
    // migration identity from the project bytes when available so moving an
    // untouched legacy .vfxphoto does not change its identity and Relink can
    // still recognise it. Once the document is saved by 0.14.0l this UUID is
    // written explicitly and becomes independent of both path and contents.
    const QByteArray contentFingerprint = fileSha256(path, nullptr);
    QByteArray identityMaterial;
    if (contentFingerprint.size() == 32) {
        identityMaterial = QByteArrayLiteral("VFXPhotoLab/legacy-project-content/")
            + contentFingerprint;
    } else {
        identityMaterial = QByteArrayLiteral("VFXPhotoLab/legacy-project-path/")
            + canonicalProjectPath(path).toUtf8();
    }
    QByteArray seed = QCryptographicHash::hash(
        identityMaterial, QCryptographicHash::Sha256).left(16);
    if (seed.size() != 16) return QUuid::createUuid();
    // Mark the deterministic value as an RFC4122 variant/version-5-style UUID.
    seed[6] = char((quint8(seed[6]) & 0x0fU) | 0x50U);
    seed[8] = char((quint8(seed[8]) & 0x3fU) | 0x80U);
    return QUuid::fromRfc4122(seed);
}

class LinkedProjectLoadGuard final {
public:
    explicit LinkedProjectLoadGuard(QString path)
        : m_path(canonicalProjectPath(path))
    {
        if (!m_path.isEmpty() && !linkedProjectLoadStack.contains(m_path)) {
            linkedProjectLoadStack.insert(m_path);
            m_entered = true;
        }
    }
    ~LinkedProjectLoadGuard()
    {
        if (m_entered) linkedProjectLoadStack.remove(m_path);
    }
    bool entered() const { return m_entered; }
    const QString &path() const { return m_path; }
private:
    QString m_path;
    bool m_entered = false;
};

class StrictLinkedCycleDetectionGuard final {
public:
    StrictLinkedCycleDetectionGuard() { ++strictLinkedCycleDetectionDepth; }
    ~StrictLinkedCycleDetectionGuard() { --strictLinkedCycleDetectionDepth; }
};

bool decodeBoundedProjectPng(const QByteArray &bytes,
                             QImage *image,
                             QString *errorMessage)
{
    constexpr int MaximumImageExtent = 32768;
    if (!image || bytes.isEmpty()) {
        setError(errorMessage, QStringLiteral("The embedded source image is damaged."));
        return false;
    }
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QStringLiteral("The embedded source image could not be opened."));
        return false;
    }
    QImageReader reader(&buffer, "PNG");
    const QSize declaredSize = reader.size();
    if (declaredSize.isValid()
        && (declaredSize.width() < 1 || declaredSize.height() < 1
            || declaredSize.width() > MaximumImageExtent
            || declaredSize.height() > MaximumImageExtent)) {
        setError(errorMessage,
                 QStringLiteral("The embedded source image exceeds the 32768-pixel safety limit."));
        return false;
    }
    QImage decoded = reader.read();
    if (decoded.isNull()) {
        setError(errorMessage, QStringLiteral("The embedded source image is damaged."));
        return false;
    }
    if (decoded.width() < 1 || decoded.height() < 1
        || decoded.width() > MaximumImageExtent
        || decoded.height() > MaximumImageExtent) {
        setError(errorMessage,
                 QStringLiteral("The embedded source image exceeds the 32768-pixel safety limit."));
        return false;
    }
    *image = std::move(decoded);
    return true;
}

bool layerJsonContainsSmartTransformMetadata(const QJsonObject &layer)
{
    if (layer.contains(QStringLiteral("smartTransform"))) return true;
    const QJsonValue childrenValue = layer.value(QStringLiteral("children"));
    if (!childrenValue.isArray()) return false;
    for (const QJsonValue &child : childrenValue.toArray()) {
        if (child.isObject()
            && layerJsonContainsSmartTransformMetadata(child.toObject())) {
            return true;
        }
    }
    return false;
}

bool layerJsonContainsLiveFilterMetadata(const QJsonObject &layer)
{
    if (layer.contains(QStringLiteral("liveFilters"))) return true;
    const QJsonValue childrenValue = layer.value(QStringLiteral("children"));
    if (!childrenValue.isArray()) return false;
    for (const QJsonValue &child : childrenValue.toArray()) {
        if (child.isObject()
            && layerJsonContainsLiveFilterMetadata(child.toObject())) {
            return true;
        }
    }
    return false;
}

bool layerJsonContainsLiveFilterMaskMetadata(const QJsonObject &layer)
{
    const QJsonValue filtersValue = layer.value(QStringLiteral("liveFilters"));
    if (filtersValue.isArray()) {
        for (const QJsonValue &value : filtersValue.toArray()) {
            if (!value.isObject()) continue;
            const QJsonObject filter = value.toObject();
            if (filter.value(QStringLiteral("schema")).toInt(1) >= 2
                || filter.contains(QStringLiteral("maskImage"))
                || filter.contains(QStringLiteral("maskReferenceWidth"))
                || filter.contains(QStringLiteral("maskReferenceHeight"))
                || filter.contains(QStringLiteral("maskReferenceOriginX"))
                || filter.contains(QStringLiteral("maskReferenceOriginY"))
                || filter.contains(QStringLiteral("maskEnabled"))
                || filter.contains(QStringLiteral("maskInverted"))) {
                return true;
            }
        }
    }
    const QJsonValue childrenValue = layer.value(QStringLiteral("children"));
    if (!childrenValue.isArray()) return false;
    for (const QJsonValue &child : childrenValue.toArray()) {
        if (child.isObject()
            && layerJsonContainsLiveFilterMaskMetadata(child.toObject())) {
            return true;
        }
    }
    return false;
}

bool layerJsonContainsLayerEffectMetadata(const QJsonObject &layer)
{
    if (layer.contains(QStringLiteral("layerEffects"))) return true;
    const QJsonValue childrenValue = layer.value(QStringLiteral("children"));
    if (!childrenValue.isArray()) return false;
    for (const QJsonValue &child : childrenValue.toArray()) {
        if (child.isObject()
            && layerJsonContainsLayerEffectMetadata(child.toObject())) {
            return true;
        }
    }
    return false;
}

bool layerJsonContainsLayerEffectParameterMetadata(const QJsonObject &layer)
{
    const QJsonValue effectsValue = layer.value(QStringLiteral("layerEffects"));
    if (effectsValue.isArray()) {
        const QStringList fields {
            QStringLiteral("colour"), QStringLiteral("opacity"),
            QStringLiteral("blendMode"), QStringLiteral("angle"),
            QStringLiteral("distance"), QStringLiteral("spread"),
            QStringLiteral("size")};
        for (const QJsonValue &value : effectsValue.toArray()) {
            if (!value.isObject()) continue;
            const QJsonObject effect = value.toObject();
            if (effect.value(QStringLiteral("schema")).toInt(1) >= 2) return true;
            for (const QString &field : fields) {
                if (effect.contains(field)) return true;
            }
        }
    }
    const QJsonValue childrenValue = layer.value(QStringLiteral("children"));
    if (!childrenValue.isArray()) return false;
    for (const QJsonValue &child : childrenValue.toArray()) {
        if (child.isObject()
            && layerJsonContainsLayerEffectParameterMetadata(child.toObject())) return true;
    }
    return false;
}

bool layerJsonContainsLayerEffectStrokeOverlayMetadata(const QJsonObject &layer)
{
    const QJsonValue effectsValue = layer.value(QStringLiteral("layerEffects"));
    if (effectsValue.isArray()) {
        const QStringList fields {
            QStringLiteral("strokePosition"), QStringLiteral("gradientStops"),
            QStringLiteral("gradientInterpolation"), QStringLiteral("gradientStyle"),
            QStringLiteral("gradientAngle"), QStringLiteral("gradientScale"),
            QStringLiteral("gradientReverse")};
        for (const QJsonValue &value : effectsValue.toArray()) {
            if (!value.isObject()) continue;
            const QJsonObject effect = value.toObject();
            if (effect.value(QStringLiteral("schema")).toInt(1) >= 3) return true;
            for (const QString &field : fields) {
                if (effect.contains(field)) return true;
            }
        }
    }
    const QJsonValue childrenValue = layer.value(QStringLiteral("children"));
    if (!childrenValue.isArray()) return false;
    for (const QJsonValue &child : childrenValue.toArray()) {
        if (child.isObject()
            && layerJsonContainsLayerEffectStrokeOverlayMetadata(child.toObject())) return true;
    }
    return false;
}

bool layerJsonContainsLayerEffectBevelMetadata(const QJsonObject &layer)
{
    const QJsonValue effectsValue = layer.value(QStringLiteral("layerEffects"));
    if (effectsValue.isArray()) {
        const QStringList fields {
            QStringLiteral("bevelStyle"), QStringLiteral("bevelDirection"),
            QStringLiteral("bevelDepth"), QStringLiteral("bevelSoften"),
            QStringLiteral("bevelAltitude"), QStringLiteral("bevelHighlightColour"),
            QStringLiteral("bevelHighlightBlendMode"), QStringLiteral("bevelHighlightOpacity"),
            QStringLiteral("bevelShadowColour"), QStringLiteral("bevelShadowBlendMode"),
            QStringLiteral("bevelShadowOpacity")};
        for (const QJsonValue &value : effectsValue.toArray()) {
            if (!value.isObject()) continue;
            const QJsonObject effect = value.toObject();
            if (effect.value(QStringLiteral("schema")).toInt(1) >= 4) return true;
            for (const QString &field : fields) {
                if (effect.contains(field)) return true;
            }
        }
    }
    const QJsonValue childrenValue = layer.value(QStringLiteral("children"));
    if (!childrenValue.isArray()) return false;
    for (const QJsonValue &child : childrenValue.toArray()) {
        if (child.isObject()
            && layerJsonContainsLayerEffectBevelMetadata(child.toObject())) return true;
    }
    return false;
}

bool projectLayerJsonIsWithinSafetyLimits(const QJsonArray &roots,
                                          QString *errorMessage)
{
    struct PendingLayer {
        QJsonObject object;
        int depth = 0;
    };
    QVector<PendingLayer> pending;
    pending.reserve(std::min<qsizetype>(roots.size(), LayerNode::MaximumTreeLayerCount));
    for (const QJsonValue &value : roots) {
        if (!value.isObject()) {
            setError(errorMessage, QStringLiteral("The project contains an invalid layer entry."));
            return false;
        }
        pending.push_back({value.toObject(), 0});
    }

    int layerCount = 0;
    qint64 vectorObjectCount = 0;
    qsizetype vectorNodeCount = 0;
    while (!pending.isEmpty()) {
        const PendingLayer entry = pending.takeLast();
        if (entry.depth < 0 || entry.depth >= LayerNode::MaximumTreeDepth
            || ++layerCount > LayerNode::MaximumTreeLayerCount) {
            setError(errorMessage,
                     QStringLiteral("The project layer hierarchy exceeds the safety limit."));
            return false;
        }

        const QJsonValue effectsValue = entry.object.value(QStringLiteral("layerEffects"));
        if (!effectsValue.isUndefined()
            && (!effectsValue.isArray()
                || effectsValue.toArray().size() > LayerNode::MaximumLayerEffectCount)) {
            setError(errorMessage,
                     QStringLiteral("The project contains excessive or invalid Layer Effect data."));
            return false;
        }

        const QString kind = entry.object.value(QStringLiteral("kind")).toString();
        if (kind.compare(QStringLiteral("group"), Qt::CaseInsensitive) == 0) {
            const QJsonValue childrenValue = entry.object.value(QStringLiteral("children"));
            if (!childrenValue.isUndefined() && !childrenValue.isArray()) {
                setError(errorMessage, QStringLiteral("The project contains damaged group data."));
                return false;
            }
            const QJsonArray children = childrenValue.toArray();
            if (children.size() > LayerNode::MaximumTreeLayerCount - layerCount) {
                setError(errorMessage,
                         QStringLiteral("The project layer hierarchy exceeds the safety limit."));
                return false;
            }
            for (const QJsonValue &child : children) {
                if (!child.isObject()) {
                    setError(errorMessage, QStringLiteral("The project contains an invalid layer entry."));
                    return false;
                }
                pending.push_back({child.toObject(), entry.depth + 1});
            }
        }

        if (kind.compare(QStringLiteral("vector"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QJsonValue vectorValue = entry.object.value(QStringLiteral("vector"));
        if (!vectorValue.isObject()) {
            setError(errorMessage, QStringLiteral("The project contains damaged vector data."));
            return false;
        }
        const QJsonValue objectsValue = vectorValue.toObject().value(QStringLiteral("objects"));
        if (!objectsValue.isArray()) {
            setError(errorMessage, QStringLiteral("The project contains damaged vector data."));
            return false;
        }
        const QJsonArray objects = objectsValue.toArray();
        vectorObjectCount += objects.size();
        if (vectorObjectCount > MaximumProjectVectorObjectCount) {
            setError(errorMessage,
                     QStringLiteral("The project contains excessive vector object data."));
            return false;
        }
        for (const QJsonValue &shapeValue : objects) {
            if (!shapeValue.isObject()) {
                setError(errorMessage, QStringLiteral("The project contains damaged vector data."));
                return false;
            }
            const QJsonObject shape = shapeValue.toObject();
            if (shape.value(QStringLiteral("kind")).toString().compare(
                    QStringLiteral("path"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            const QJsonValue pathValue = shape.value(QStringLiteral("path"));
            if (!pathValue.isObject()
                || !pathValue.toObject().value(QStringLiteral("nodes")).isArray()) {
                setError(errorMessage, QStringLiteral("The project contains damaged vector path data."));
                return false;
            }
            const qsizetype primaryNodes = pathValue.toObject()
                .value(QStringLiteral("nodes")).toArray().size();
            if (primaryNodes > MaximumProjectVectorNodeCount - vectorNodeCount) {
                setError(errorMessage,
                         QStringLiteral("The project contains excessive editable vector nodes."));
                return false;
            }
            vectorNodeCount += primaryNodes;
            const QJsonValue additionalValue = shape.value(QStringLiteral("additionalPaths"));
            if (!additionalValue.isUndefined() && !additionalValue.isArray()) {
                setError(errorMessage, QStringLiteral("The project contains damaged compound path data."));
                return false;
            }
            for (const QJsonValue &additionalPathValue : additionalValue.toArray()) {
                if (!additionalPathValue.isObject()
                    || !additionalPathValue.toObject().value(QStringLiteral("nodes")).isArray()) {
                    setError(errorMessage, QStringLiteral("The project contains damaged compound path data."));
                    return false;
                }
                const qsizetype nodes = additionalPathValue.toObject()
                    .value(QStringLiteral("nodes")).toArray().size();
                if (nodes > MaximumProjectVectorNodeCount - vectorNodeCount) {
                    setError(errorMessage,
                             QStringLiteral("The project contains excessive editable vector nodes."));
                    return false;
                }
                vectorNodeCount += nodes;
            }
        }
    }
    return true;
}

QString colourModelToString(const DocumentColourModel model)
{
    return model == DocumentColourModel::Grayscale
        ? QStringLiteral("grayscale")
        : QStringLiteral("rgb");
}

DocumentColourModel colourModelFromString(const QString &value)
{
    return value.compare(QStringLiteral("grayscale"), Qt::CaseInsensitive) == 0
        ? DocumentColourModel::Grayscale
        : DocumentColourModel::Rgb;
}

QString colourSpaceToString(const QColorSpace &colourSpace)
{
    if (colourSpace == QColorSpace(QColorSpace::SRgbLinear)) {
        return QStringLiteral("linear-srgb");
    }
    if (colourSpace == QColorSpace(QColorSpace::SRgb)) {
        return QStringLiteral("srgb");
    }
    if (colourSpace == QColorSpace(QColorSpace::DisplayP3)) {
        return QStringLiteral("display-p3");
    }
    if (colourSpace == QColorSpace(QColorSpace::AdobeRgb)) {
        return QStringLiteral("adobe-rgb");
    }
    if (colourSpace == QColorSpace(QColorSpace::ProPhotoRgb)) {
        return QStringLiteral("prophoto-rgb");
    }
    return {};
}

QColorSpace colourSpaceFromString(const QString &value,
                                  const QColorSpace &fallback)
{
    if (value.compare(QStringLiteral("linear-srgb"), Qt::CaseInsensitive) == 0) {
        return QColorSpace(QColorSpace::SRgbLinear);
    }
    if (value.compare(QStringLiteral("srgb"), Qt::CaseInsensitive) == 0) {
        return QColorSpace(QColorSpace::SRgb);
    }
    if (value.compare(QStringLiteral("display-p3"), Qt::CaseInsensitive) == 0) {
        return QColorSpace(QColorSpace::DisplayP3);
    }
    if (value.compare(QStringLiteral("adobe-rgb"), Qt::CaseInsensitive) == 0) {
        return QColorSpace(QColorSpace::AdobeRgb);
    }
    if (value.compare(QStringLiteral("prophoto-rgb"), Qt::CaseInsensitive) == 0) {
        return QColorSpace(QColorSpace::ProPhotoRgb);
    }
    return fallback;
}

LayerNode *findLayerRecursive(QVector<LayerNode> &layers, const QUuid &id)
{
    for (LayerNode &layer : layers) {
        if (layer.id == id) {
            return &layer;
        }
        if (LayerNode *child = findLayerRecursive(layer.children, id)) {
            return child;
        }
    }
    return nullptr;
}

const LayerNode *findLayerRecursive(const QVector<LayerNode> &layers, const QUuid &id)
{
    for (const LayerNode &layer : layers) {
        if (layer.id == id) {
            return &layer;
        }
        if (const LayerNode *child = findLayerRecursive(layer.children, id)) {
            return child;
        }
    }
    return nullptr;
}

bool findWorldTransformRecursive(const QVector<LayerNode> &layers,
                                 const QUuid &id,
                                 const QTransform &parentWorld,
                                 QTransform *worldResult,
                                 QTransform *parentResult = nullptr)
{
    for (const LayerNode &layer : layers) {
        // QTransform uses row-vector ordering: local * parent maps local layer
        // coordinates through the parent hierarchy into document space.
        const QTransform world = layer.transform * parentWorld;
        if (layer.id == id) {
            if (worldResult) {
                *worldResult = world;
            }
            if (parentResult) {
                *parentResult = parentWorld;
            }
            return true;
        }
        if (findWorldTransformRecursive(layer.children,
                                        id,
                                        world,
                                        worldResult,
                                        parentResult)) {
            return true;
        }
    }
    return false;
}

QVector<LayerNode> *findSiblingsRecursive(QVector<LayerNode> &layers,
                                          const QUuid &id,
                                          int *index)
{
    for (int layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
        if (layers.at(layerIndex).id == id) {
            if (index) {
                *index = layerIndex;
            }
            return &layers;
        }
        if (QVector<LayerNode> *siblings = findSiblingsRecursive(
                layers[layerIndex].children, id, index)) {
            return siblings;
        }
    }
    return nullptr;
}

struct LayerLocation {
    QVector<LayerNode> *siblings = nullptr;
    QUuid parentId;
    int index = -1;
};

bool findLayerLocationRecursive(QVector<LayerNode> &layers,
                                const QUuid &id,
                                const QUuid &parentId,
                                LayerLocation *location)
{
    for (int index = 0; index < layers.size(); ++index) {
        LayerNode &layer = layers[index];
        if (layer.id == id) {
            if (location) {
                location->siblings = &layers;
                location->parentId = parentId;
                location->index = index;
            }
            return true;
        }
        if (findLayerLocationRecursive(layer.children, id, layer.id, location)) {
            return true;
        }
    }
    return false;
}

bool findLayerPlacementRecursive(const QVector<LayerNode> &layers,
                                 const QUuid &id,
                                 const QUuid &parentId,
                                 QUuid *foundParentId,
                                 int *foundIndex)
{
    for (int index = 0; index < layers.size(); ++index) {
        const LayerNode &layer = layers.at(index);
        if (layer.id == id) {
            if (foundParentId) {
                *foundParentId = parentId;
            }
            if (foundIndex) {
                *foundIndex = index;
            }
            return true;
        }
        if (findLayerPlacementRecursive(layer.children,
                                        id,
                                        layer.id,
                                        foundParentId,
                                        foundIndex)) {
            return true;
        }
    }
    return false;
}

bool containsLayerRecursive(const LayerNode &layer, const QUuid &id)
{
    if (layer.id == id) {
        return true;
    }
    for (const LayerNode &child : layer.children) {
        if (containsLayerRecursive(child, id)) {
            return true;
        }
    }
    return false;
}

void collectSelectedRootOrder(const QVector<LayerNode> &layers,
                              const QSet<QUuid> &wanted,
                              const bool selectedAncestor,
                              QVector<QUuid> *ordered)
{
    for (const LayerNode &layer : layers) {
        const bool selected = wanted.contains(layer.id);
        if (selected && !selectedAncestor) {
            ordered->push_back(layer.id);
        }
        collectSelectedRootOrder(layer.children,
                                 wanted,
                                 selectedAncestor || selected,
                                 ordered);
    }
}

bool removeLayerById(QVector<LayerNode> &layers, const QUuid &id, LayerNode *removed)
{
    for (int index = 0; index < layers.size(); ++index) {
        if (layers.at(index).id == id) {
            if (removed) {
                *removed = std::move(layers[index]);
            }
            layers.removeAt(index);
            return true;
        }
        if (removeLayerById(layers[index].children, id, removed)) {
            return true;
        }
    }
    return false;
}

QVector<LayerNode> *childrenForParent(QVector<LayerNode> &layers, const QUuid &parentId)
{
    if (parentId.isNull()) {
        return &layers;
    }
    LayerNode *parent = findLayerRecursive(layers, parentId);
    return parent && parent->type == LayerType::Group ? &parent->children : nullptr;
}

const QVector<LayerNode> *childrenForParent(const QVector<LayerNode> &layers, const QUuid &parentId)
{
    if (parentId.isNull()) {
        return &layers;
    }
    const LayerNode *parent = findLayerRecursive(layers, parentId);
    return parent && parent->type == LayerType::Group ? &parent->children : nullptr;
}

int countLayersRecursive(const QVector<LayerNode> &layers)
{
    int count = 0;
    for (const LayerNode &layer : layers) {
        ++count;
        count += countLayersRecursive(layer.children);
    }
    return count;
}

int countAdjustmentsRecursive(const QVector<LayerNode> &layers)
{
    int count = 0;
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Adjustment) {
            ++count;
        }
        count += countAdjustmentsRecursive(layer.children);
    }
    return count;
}

int countBaseLayersRecursive(const QVector<LayerNode> &layers)
{
    int count = 0;
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::BaseImage) {
            ++count;
        }
        count += countBaseLayersRecursive(layer.children);
    }
    return count;
}

QUuid findLegacyBaseIdRecursive(const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::BaseImage) {
            return layer.id;
        }
        const QUuid nested = findLegacyBaseIdRecursive(layer.children);
        if (!nested.isNull()) {
            return nested;
        }
    }
    return {};
}

QUuid findBottomRasterIdRecursive(const QVector<LayerNode> &layers)
{
    for (auto it = layers.crbegin(); it != layers.crend(); ++it) {
        const LayerNode &layer = *it;
        if (layer.type == LayerType::Raster) {
            return layer.id;
        }
        const QUuid nested = findBottomRasterIdRecursive(layer.children);
        if (!nested.isNull()) {
            return nested;
        }
    }
    return {};
}

QUuid findBottomLayerIdRecursive(const QVector<LayerNode> &layers)
{
    for (auto it = layers.crbegin(); it != layers.crend(); ++it) {
        const LayerNode &layer = *it;
        const QUuid nested = findBottomLayerIdRecursive(layer.children);
        if (!nested.isNull()) {
            return nested;
        }
        return layer.id;
    }
    return {};
}

QUuid findSourceSharedRasterIdRecursive(const QVector<LayerNode> &layers,
                                        const QImage &source)
{
    if (source.isNull()) {
        return {};
    }
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Raster
            && !layer.rasterImage.isNull()
            && layer.rasterImage.cacheKey() == source.cacheKey()
            && layer.rasterImage.size() == source.size()
            && layer.rasterImage.format() == source.format()) {
            return layer.id;
        }
        const QUuid nested = findSourceSharedRasterIdRecursive(layer.children, source);
        if (!nested.isNull()) {
            return nested;
        }
    }
    return {};
}

bool stripSourceSharedRasterPayload(QJsonArray *layers,
                                    const QUuid &sourceRasterId)
{
    if (!layers || sourceRasterId.isNull()) {
        return false;
    }
    const QString sourceId = sourceRasterId.toString(QUuid::WithoutBraces);
    for (qsizetype index = 0; index < layers->size(); ++index) {
        QJsonObject layer = layers->at(index).toObject();
        if (layer.value(QStringLiteral("id")).toString() == sourceId) {
            layer.remove(QStringLiteral("rasterEncoding"));
            layer.remove(QStringLiteral("rasterData"));
            (*layers)[index] = layer;
            return true;
        }
        QJsonArray children = layer.value(QStringLiteral("children")).toArray();
        if (stripSourceSharedRasterPayload(&children, sourceRasterId)) {
            layer.insert(QStringLiteral("children"), children);
            (*layers)[index] = layer;
            return true;
        }
    }
    return false;
}

bool materialiseSourceSharedRasterRecursive(QVector<LayerNode> &layers,
                                            const QUuid &sourceRasterId,
                                            const QImage &source)
{
    for (LayerNode &layer : layers) {
        if (layer.id == sourceRasterId) {
            if (layer.type != LayerType::Raster || !layer.rasterImage.isNull()) {
                return false;
            }
            layer.rasterImage = source;
            return true;
        }
        if (materialiseSourceSharedRasterRecursive(layer.children,
                                                   sourceRasterId,
                                                   source)) {
            return true;
        }
    }
    return false;
}

void promoteLegacyBaseLayersRecursive(QVector<LayerNode> &layers,
                                      const QImage &source)
{
    for (LayerNode &layer : layers) {
        if (layer.type == LayerType::BaseImage) {
            if (layer.rasterImage.isNull()) {
                // QImage uses implicit sharing, so this is an exact, cheap
                // promotion until either copy is edited. Straight RGBA and
                // hidden RGB beneath zero alpha are preserved byte-for-byte.
                layer.rasterImage = source;
            }
            layer.type = LayerType::Raster;
            if (layer.rasterReferenceSize.isEmpty()) {
                layer.rasterReferenceSize = source.size();
            }
        }
        promoteLegacyBaseLayersRecursive(layer.children, source);
    }
}

bool containsVectorLayerRecursive(const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Vector
            || containsVectorLayerRecursive(layer.children)) {
            return true;
        }
    }
    return false;
}

bool containsTextLayerRecursive(const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Text || containsTextLayerRecursive(layer.children)) return true;
    }
    return false;
}

bool containsBezierPathRecursive(const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Vector) {
            for (const VectorShape &shape : layer.vectorData.objects) {
                if (shape.type == VectorShapeType::Path) return true;
            }
        }
        if (containsBezierPathRecursive(layer.children)) return true;
    }
    return false;
}

bool containsLiveVectorCornersRecursive(const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Vector) {
            for (const VectorShape &shape : layer.vectorData.objects) {
                if (shape.type == VectorShapeType::Path) {
                    if (shape.bezierPath.hasCornerMetadata()) return true;
                    for (const VectorBezierPath &path : shape.additionalBezierPaths) {
                        if (path.hasCornerMetadata()) return true;
                    }
                }
            }
        }
        if (containsLiveVectorCornersRecursive(layer.children)) return true;
    }
    return false;
}

bool containsDashedVectorStrokesRecursive(const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Vector) {
            for (const VectorShape &shape : layer.vectorData.objects) {
                if (shape.stroke.pattern == VectorStrokePattern::Dashed) return true;
            }
        }
        if (containsDashedVectorStrokesRecursive(layer.children)) return true;
    }
    return false;
}

bool containsCompoundVectorPathsRecursive(const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Vector) {
            for (const VectorShape &shape : layer.vectorData.objects) {
                if (shape.type == VectorShapeType::Path
                    && !shape.additionalBezierPaths.isEmpty()) {
                    return true;
                }
            }
        }
        if (containsCompoundVectorPathsRecursive(layer.children)) return true;
    }
    return false;
}

bool containsNonZeroVectorPathFillRecursive(const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Vector) {
            for (const VectorShape &shape : layer.vectorData.objects) {
                if (shape.type == VectorShapeType::Path
                    && shape.pathFillRule == VectorPathFillRule::NonZero) {
                    return true;
                }
            }
        }
        if (containsNonZeroVectorPathFillRecursive(layer.children)) return true;
    }
    return false;
}

bool containsVectorArrowMetadataRecursive(const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Vector) {
            for (const VectorShape &shape : layer.vectorData.objects) {
                if (shape.type == VectorShapeType::Arrow
                    || shape.stroke.startArrowhead != VectorArrowheadType::None
                    || shape.stroke.endArrowhead != VectorArrowheadType::None
                    || std::abs(shape.stroke.startArrowScale - 1.0) > 1.0e-12
                    || std::abs(shape.stroke.endArrowScale - 1.0) > 1.0e-12) {
                    return true;
                }
            }
        }
        if (containsVectorArrowMetadataRecursive(layer.children)) return true;
    }
    return false;
}

bool containsNonZeroVectorFeatherRecursive(const QVector<LayerNode> &layers)
{
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Vector
            && std::abs(layer.vectorData.featherRadius) > 1.0e-12) {
            return true;
        }
        if (containsNonZeroVectorFeatherRecursive(layer.children)) return true;
    }
    return false;
}

bool idsAreUniqueRecursive(const QVector<LayerNode> &layers, QSet<QUuid> &seen)
{
    for (const LayerNode &layer : layers) {
        if (layer.id.isNull() || seen.contains(layer.id)) {
            return false;
        }
        seen.insert(layer.id);
        if (!idsAreUniqueRecursive(layer.children, seen)) {
            return false;
        }
    }
    return true;
}

struct LayerTreeSafetyBudget {
    int layerCount = 0;
    qint64 vectorObjectCount = 0;
    qsizetype vectorNodeCount = 0;
};

bool layerMetadataIsSafeRecursiveImpl(const QVector<LayerNode> &layers,
                                      const int depth,
                                      LayerTreeSafetyBudget *budget)
{
    constexpr double MaximumCoordinate = 1.0e9;
    constexpr int MaximumExtent = 32768;
    if (layers.isEmpty()) return true;
    if (!budget || depth < 0 || depth >= LayerNode::MaximumTreeDepth) return false;
    const auto safeOrigin = [MaximumCoordinate](const QPointF &origin) {
        return std::isfinite(origin.x()) && std::isfinite(origin.y())
            && std::abs(origin.x()) <= MaximumCoordinate
            && std::abs(origin.y()) <= MaximumCoordinate;
    };
    const auto safeExtent = [MaximumExtent](const QSize &size) {
        if (size.width() <= 0 || size.height() <= 0) {
            return size.width() <= 0 && size.height() <= 0;
        }
        return size.width() <= MaximumExtent
            && size.height() <= MaximumExtent;
    };
    const auto safeImage = [&safeExtent](const QImage &image) {
        return image.isNull() || safeExtent(image.size());
    };
    const auto liveFiltersSafe = [](const LayerNode &layer) {
        if (layer.type != LayerType::Smart) return layer.liveFilters.isEmpty();
        if (layer.liveFilters.size() > LayerNode::MaximumLiveFilterCount) return false;
        QSet<QUuid> ids;
        for (const LiveFilter &filter : layer.liveFilters) {
            if (!filter.isSafe() || ids.contains(filter.id)) return false;
            ids.insert(filter.id);
        }
        return true;
    };
    const auto layerEffectsSafe = [](const LayerNode &layer) {
        if (!layerTypeSupportsLayerEffects(layer.type)) return layer.layerEffects.isEmpty();
        if (layer.layerEffects.size() > LayerNode::MaximumLayerEffectCount) return false;
        QSet<QUuid> ids;
        for (const LayerEffect &effect : layer.layerEffects) {
            if (!effect.isSafe() || ids.contains(effect.id)) return false;
            ids.insert(effect.id);
        }
        return true;
    };

    for (const LayerNode &layer : layers) {
        if (++budget->layerCount > LayerNode::MaximumTreeLayerCount) return false;
        if (layer.type == LayerType::Vector) {
            budget->vectorObjectCount += layer.vectorData.objects.size();
            if (budget->vectorObjectCount > MaximumProjectVectorObjectCount) return false;
            for (const VectorShape &shape : layer.vectorData.objects) {
                if (shape.type != VectorShapeType::Path) continue;
                qsizetype objectNodes = shape.bezierPath.nodes.size();
                for (const VectorBezierPath &path : shape.additionalBezierPaths) {
                    if (path.nodes.size() > MaximumProjectVectorNodeCount - objectNodes) {
                        return false;
                    }
                    objectNodes += path.nodes.size();
                }
                if (objectNodes > MaximumProjectVectorNodeCount - budget->vectorNodeCount) {
                    return false;
                }
                budget->vectorNodeCount += objectNodes;
            }
        }
        if (!layer.transform.isInvertible()
            || !transformMatrixIsFiniteAndBounded(layer.transform)
            || !std::isfinite(layer.opacity)
            || layer.opacity < 0.0 || layer.opacity > 1.0
            || (layer.type == LayerType::Adjustment
                && (!std::isfinite(layer.exposure)
                    || !std::isfinite(layer.contrast)
                    || !std::isfinite(layer.saturation)
                    || !std::isfinite(layer.blackPoint)
                    || !std::isfinite(layer.whitePoint)
                    || !std::isfinite(layer.gamma)))
            || !safeOrigin(layer.rasterReferenceOrigin)
            || !safeOrigin(layer.maskReferenceOrigin)
            || !safeExtent(layer.rasterReferenceSize)
            || !safeExtent(layer.maskReferenceSize)
            || !safeImage(layer.rasterImage)
            || !safeImage(layer.maskImage)
            || (layer.type == LayerType::Vector && !layer.vectorData.isSafe())
            || (layer.type == LayerType::Text && !layer.textData.isSafe())
            || (layer.type == LayerType::Smart
                && (!layer.smartSource.isSafe() || !layer.smartTransform.isSafe()))
            || (layer.type != LayerType::Smart && !layer.smartSource.isEmpty())
            || !liveFiltersSafe(layer)
            || !layerEffectsSafe(layer)
            || !layerMetadataIsSafeRecursiveImpl(layer.children, depth + 1, budget)) {
            return false;
        }
    }
    return true;
}

bool layerMetadataIsSafeRecursive(const QVector<LayerNode> &layers)
{
    LayerTreeSafetyBudget budget;
    return layerMetadataIsSafeRecursiveImpl(layers, 0, &budget);
}

bool smartLayerReferencesResolveRecursive(const QVector<LayerNode> &layers,
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
        if (!smartLayerReferencesResolveRecursive(layer.children, sources)) {
            return false;
        }
    }
    return true;
}


void synchronizeSmartSourceRevisionRecursive(QVector<LayerNode> &layers,
                                               const QUuid &sourceId,
                                               const quint64 revision)
{
    for (LayerNode &layer : layers) {
        if (layer.type == LayerType::Smart && layer.smartSource.sourceId == sourceId) {
            if (layer.smartSource.observedSourceRevision != revision) {
                layer.smartSource.observedSourceRevision = revision;
                if (layer.revision != std::numeric_limits<quint64>::max()) ++layer.revision;
            }
        }
        synchronizeSmartSourceRevisionRecursive(layer.children, sourceId, revision);
    }
}

bool colourManageSmartPresentation(const QImage &presentation,
                                  const QColorSpace &targetSpace,
                                  QImage *managed,
                                  QString *errorMessage)
{
    if (!managed || presentation.isNull()) {
        setError(errorMessage, QStringLiteral(
            "The Smart Source presentation is unavailable for colour-managed binding."));
        return false;
    }
    const QColorSpace sourceSpace = presentation.colorSpace();
    if (sourceSpace == targetSpace
        || (!sourceSpace.isValid() && !targetSpace.isValid())) {
        *managed = presentation;
        return true;
    }
    if (!sourceSpace.isValid() || !targetSpace.isValid()) {
        setError(errorMessage, QStringLiteral(
            "A Smart Source working space cannot be composed into an untagged containing document without an explicit colour interpretation."));
        return false;
    }

    ColourTransformRequest request;
    request.source = ColourSpaceDescriptor::fromQColorSpace(sourceSpace);
    request.destination = ColourSpaceDescriptor::fromQColorSpace(targetSpace);
    request.purpose = ColourTransformPurpose::WorkingToWorking;
    const std::optional<QColorTransform> transform =
        ColourTransformService::instance().qtTransform(request);
    if (!transform.has_value()) {
        setError(errorMessage, QStringLiteral(
            "A Smart Source working-space transform could not be created for its containing document."));
        return false;
    }

    const QImage::Format originalFormat = presentation.format();
    QImage converted = presentation;
    converted.applyColorTransform(*transform);
    if (converted.isNull()) {
        setError(errorMessage, QStringLiteral(
            "The Smart Source presentation could not be converted into the containing document working space."));
        return false;
    }
    if (converted.format() != originalFormat) {
        converted = converted.convertToFormat(originalFormat);
        if (converted.isNull()) {
            setError(errorMessage, QStringLiteral(
                "The colour-managed Smart Source presentation could not preserve its pixel precision."));
            return false;
        }
    }
    converted.setColorSpace(targetSpace);
    *managed = std::move(converted);
    return true;
}

bool bindSmartPresentationsRecursiveImpl(
    QVector<LayerNode> &layers,
    const SmartSourceRegistry &sources,
    const QColorSpace &targetSpace,
    QHash<QUuid, QImage> *managedCache,
    QString *errorMessage)
{
    if (!managedCache) return false;
    for (LayerNode &layer : layers) {
        if (layer.type == LayerType::Smart) {
            const SmartSourceDescriptor *source = sources.find(layer.smartSource.sourceId);
            if (source && source->hasCurrentPresentation()
                && layer.smartSource.observedSourceRevision == source->revision) {
                auto cached = managedCache->constFind(source->id);
                if (cached == managedCache->cend()) {
                    QImage managed;
                    if (!colourManageSmartPresentation(source->presentationImage,
                                                      targetSpace,
                                                      &managed,
                                                      errorMessage)) {
                        return false;
                    }
                    managedCache->insert(source->id, std::move(managed));
                    cached = managedCache->constFind(source->id);
                }
                layer.smartPresentationImage = cached.value();
                layer.smartPresentationReferenceSize = source->presentationBounds.size();
                layer.smartPresentationReferenceOrigin = source->presentationBounds.topLeft();
            } else {
                layer.smartPresentationImage = {};
                layer.smartPresentationReferenceSize = {};
                layer.smartPresentationReferenceOrigin = {};
            }
        } else {
            layer.smartPresentationImage = {};
            layer.smartPresentationReferenceSize = {};
            layer.smartPresentationReferenceOrigin = {};
        }
        if (!bindSmartPresentationsRecursiveImpl(layer.children, sources, targetSpace,
                                                 managedCache, errorMessage)) {
            return false;
        }
    }
    return true;
}

bool bindSmartPresentationsRecursive(QVector<LayerNode> &layers,
                                     const SmartSourceRegistry &sources,
                                     const QColorSpace &targetSpace,
                                     QString *errorMessage = nullptr)
{
    QHash<QUuid, QImage> managedCache;
    return bindSmartPresentationsRecursiveImpl(layers, sources, targetSpace,
                                               &managedCache, errorMessage);
}

void collectSmartSourceDependenciesRecursive(const QVector<LayerNode> &layers,
                                             QSet<QUuid> *dependencies)
{
    if (!dependencies) return;
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Smart && !layer.smartSource.sourceId.isNull()) {
            dependencies->insert(layer.smartSource.sourceId);
        }
        collectSmartSourceDependenciesRecursive(layer.children, dependencies);
    }
}

void setRasterLayerColourSpaceRecursive(QVector<LayerNode> &layers,
                                       const QColorSpace &colourSpace);

QImage straightRgbaImage(const QImage &image);

void materialiseEmbeddedBasePixelsRecursive(QVector<LayerNode> &layers,
                                            const QImage &source,
                                            const QSize &documentSize)
{
    for (LayerNode &layer : layers) {
        if (layer.type == LayerType::BaseImage && layer.rasterImage.isNull()) {
            layer.rasterImage = source;
            layer.rasterReferenceSize = documentSize;
            layer.rasterReferenceOrigin = {};
        }
        materialiseEmbeddedBasePixelsRecursive(layer.children, source, documentSize);
    }
}

QRect nonZeroRgbaBounds(const QImage &image)
{
    if (image.isNull()) return {};
    const QImage rgba = image.convertToFormat(
        image.depth() > 32 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    int left = rgba.width();
    int top = rgba.height();
    int right = -1;
    int bottom = -1;
    if (rgba.depth() > 32) {
        for (int y = 0; y < rgba.height(); ++y) {
            const auto *row = reinterpret_cast<const QRgba64 *>(rgba.constScanLine(y));
            for (int x = 0; x < rgba.width(); ++x) {
                if (row[x].red() == 0 && row[x].green() == 0
                    && row[x].blue() == 0 && row[x].alpha() == 0) {
                    continue;
                }
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    } else {
        for (int y = 0; y < rgba.height(); ++y) {
            const uchar *row = rgba.constScanLine(y);
            for (int x = 0; x < rgba.width(); ++x) {
                const int offset = x * 4;
                if (row[offset] == 0 && row[offset + 1] == 0
                    && row[offset + 2] == 0 && row[offset + 3] == 0) {
                    continue;
                }
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    return right >= left && bottom >= top
        ? QRect(QPoint(left, top), QPoint(right, bottom))
        : QRect();
}

QRectF rasterStorageBoundsRecursive(const QImage &source,
                                    const QVector<LayerNode> &layers,
                                    const QSize &documentSize,
                                    const QTransform &parentTransform = QTransform())
{
    QRectF bounds;
    for (const LayerNode &layer : layers) {
        if (!layer.visible || layer.opacity <= 0.0) continue;
        const QTransform worldTransform = layer.transform * parentTransform;
        QRectF layerBounds;
        if (layer.type == LayerType::BaseImage || layer.type == LayerType::Raster) {
            const QImage &pixels = layer.type == LayerType::BaseImage && layer.rasterImage.isNull()
                ? source : layer.rasterImage;
            if (!pixels.isNull()) {
                const QSize referenceSize = layer.rasterReferenceSize.isEmpty()
                    ? documentSize : layer.rasterReferenceSize;
                if (!referenceSize.isEmpty()) {
                    layerBounds = worldTransform.mapRect(
                        QRectF(layer.rasterReferenceOrigin, QSizeF(referenceSize)));
                }
            }
        } else if (layer.type == LayerType::Smart && !layer.smartPresentationImage.isNull()) {
            const QSize referenceSize = layer.smartPresentationReferenceSize.isEmpty()
                ? layer.smartPresentationImage.size() : layer.smartPresentationReferenceSize;
            if (!referenceSize.isEmpty()) {
                layerBounds = worldTransform.mapRect(
                    QRectF(layer.smartPresentationReferenceOrigin, QSizeF(referenceSize)));
                const QSize liveRadius = liveFilterStackSpatialRadius2D(layer.liveFilters);
                if (!liveRadius.isEmpty()) {
                    layerBounds.adjust(-liveRadius.width(), -liveRadius.height(),
                                       liveRadius.width(), liveRadius.height());
                }
            }
        } else if (layer.type == LayerType::Group) {
            layerBounds = rasterStorageBoundsRecursive(source, layer.children, documentSize,
                                                       worldTransform);
        }
        if (!layerBounds.isEmpty()) {
            bounds = bounds.isEmpty() ? layerBounds : bounds.united(layerBounds);
        }
    }
    return bounds;
}

QJsonObject encodeEmbeddedSmartDocument(const QVector<LayerNode> &layers,
                                        const QSize &canvasSize,
                                        const double resolutionX,
                                        const double resolutionY,
                                        const DocumentColourModel colourModel,
                                        const int bitDepth,
                                        const DocumentColourState &colourState,
                                        bool *ok)
{
    if (ok) *ok = false;
    if (canvasSize.isEmpty() || canvasSize.width() > 32768
        || canvasSize.height() > 32768
        || (bitDepth != 8 && bitDepth != 16)) {
        return {};
    }
    QJsonArray layerArray;
    for (const LayerNode &layer : layers) {
        bool layerOk = false;
        const QJsonObject encoded = layer.toJson(&layerOk);
        if (!layerOk) return {};
        layerArray.append(encoded);
    }
    QJsonObject canvas;
    canvas.insert(QStringLiteral("width"), canvasSize.width());
    canvas.insert(QStringLiteral("height"), canvasSize.height());
    QJsonObject object;
    object.insert(QStringLiteral("schema"),
                  static_cast<int>(SmartSourceDescriptor::EmbeddedDocumentSchema));
    object.insert(QStringLiteral("canvas"), canvas);
    object.insert(QStringLiteral("resolutionX"), resolutionX);
    object.insert(QStringLiteral("resolutionY"), resolutionY);
    object.insert(QStringLiteral("colourModel"), colourModelToString(colourModel));
    object.insert(QStringLiteral("bitDepth"), bitDepth);
    object.insert(QStringLiteral("colourManagement"), colourState.toJson());
    object.insert(QStringLiteral("layers"), layerArray);
    if (ok) *ok = true;
    return object;
}

bool decodeEmbeddedSmartDocument(const SmartSourceDescriptor &source,
                                 const SmartSourceRegistry &registry,
                                 QVector<LayerNode> *layers,
                                 QSize *canvasSize,
                                 DocumentColourState *colourState,
                                 double *resolutionX,
                                 double *resolutionY,
                                 DocumentColourModel *colourModel,
                                 int *bitDepth,
                                 QString *errorMessage)
{
    if (!layers || !source.hasEmbeddedDocument()) {
        setError(errorMessage, QStringLiteral("The Smart Source has no embedded document contents."));
        return false;
    }
    const QJsonObject object = source.embeddedDocument;
    const int embeddedSchema = object.value(QStringLiteral("schema")).toInt(-1);
    if ((embeddedSchema < 1
         || embeddedSchema > static_cast<int>(SmartSourceDescriptor::EmbeddedDocumentSchema))
        || !object.value(QStringLiteral("canvas")).isObject()
        || !object.value(QStringLiteral("layers")).isArray()
        || !object.value(QStringLiteral("colourManagement")).isObject()) {
        setError(errorMessage, QStringLiteral("The embedded Smart Source document is invalid."));
        return false;
    }
    const QJsonObject canvas = object.value(QStringLiteral("canvas")).toObject();
    const QSize decodedSize(canvas.value(QStringLiteral("width")).toInt(-1),
                            canvas.value(QStringLiteral("height")).toInt(-1));
    const double decodedResolutionX = object.value(QStringLiteral("resolutionX")).toDouble(-1.0);
    const double decodedResolutionY = object.value(QStringLiteral("resolutionY")).toDouble(-1.0);
    const QString decodedColourModel = object.value(QStringLiteral("colourModel")).toString();
    const int decodedBitDepth = embeddedSchema >= 2
        ? object.value(QStringLiteral("bitDepth")).toInt(-1)
        : (source.presentationImage.depth() > 32 ? 16 : 8);
    if (decodedSize.isEmpty() || decodedSize.width() > 32768
        || decodedSize.height() > 32768
        || !std::isfinite(decodedResolutionX) || !std::isfinite(decodedResolutionY)
        || decodedResolutionX < 1.0 || decodedResolutionX > 9600.0
        || decodedResolutionY < 1.0 || decodedResolutionY > 9600.0
        || (decodedBitDepth != 8 && decodedBitDepth != 16)
        || (decodedColourModel != QStringLiteral("rgb")
            && decodedColourModel != QStringLiteral("grayscale"))) {
        setError(errorMessage, QStringLiteral("The embedded Smart Source document metadata is invalid."));
        return false;
    }
    QString colourError;
    const auto decodedColour = DocumentColourState::fromJson(
        object.value(QStringLiteral("colourManagement")).toObject(), &colourError);
    if (!decodedColour) {
        setError(errorMessage, colourError.isEmpty()
            ? QStringLiteral("The embedded Smart Source colour state is invalid.")
            : colourError);
        return false;
    }
    const QJsonArray array = object.value(QStringLiteral("layers")).toArray();
    if (embeddedSchema < 3) {
        for (const QJsonValue &value : array) {
            if (value.isObject()
                && layerJsonContainsSmartTransformMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-schema-3 embedded Smart Source cannot contain Smart transform metadata."));
                return false;
            }
        }
    }
    if (embeddedSchema < 4) {
        for (const QJsonValue &value : array) {
            if (value.isObject()
                && layerJsonContainsLiveFilterMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-schema-4 embedded Smart Source cannot contain Live Filter metadata."));
                return false;
            }
        }
    }
    if (embeddedSchema < 5) {
        for (const QJsonValue &value : array) {
            if (value.isObject()
                && layerJsonContainsLiveFilterMaskMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-schema-5 embedded Smart Source cannot contain Live Filter mask metadata."));
                return false;
            }
        }
    }
    if (embeddedSchema < 6) {
        for (const QJsonValue &value : array) {
            if (value.isObject()
                && layerJsonContainsLayerEffectMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-schema-6 embedded Smart Source cannot contain Layer Effect metadata."));
                return false;
            }
        }
    }
    if (embeddedSchema < 7) {
        for (const QJsonValue &value : array) {
            if (value.isObject()
                && layerJsonContainsLayerEffectParameterMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-schema-7 embedded Smart Source cannot contain Layer Effect renderer parameters."));
                return false;
            }
        }
    }
    if (embeddedSchema < 8) {
        for (const QJsonValue &value : array) {
            if (value.isObject()
                && layerJsonContainsLayerEffectStrokeOverlayMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-schema-8 embedded Smart Source cannot contain Stroke/Overlay Layer Effect parameters."));
                return false;
            }
        }
    }
    if (embeddedSchema < 9) {
        for (const QJsonValue &value : array) {
            if (value.isObject()
                && layerJsonContainsLayerEffectBevelMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-schema-9 embedded Smart Source cannot contain Bevel & Emboss Layer Effect parameters."));
                return false;
            }
        }
    }
    if (array.size() > LayerNode::MaximumTreeLayerCount) {
        setError(errorMessage, QStringLiteral("The embedded Smart Source layer tree exceeds safety limits."));
        return false;
    }
    QVector<LayerNode> decoded;
    decoded.reserve(array.size());
    QStringList warnings;
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            setError(errorMessage, QStringLiteral("The embedded Smart Source contains invalid layer data."));
            return false;
        }
        bool layerOk = false;
        LayerNode layer = LayerNode::fromJson(value.toObject(), &layerOk, &warnings);
        if (!layerOk) {
            setError(errorMessage, QStringLiteral("The embedded Smart Source contains damaged layer data."));
            return false;
        }
        decoded.push_back(std::move(layer));
    }
    if (!layerMetadataIsSafeRecursive(decoded)) {
        setError(errorMessage, QStringLiteral("The embedded Smart Source layer tree exceeds safety limits."));
        return false;
    }
    QSet<QUuid> ids;
    if (!idsAreUniqueRecursive(decoded, ids)) {
        setError(errorMessage, QStringLiteral("The embedded Smart Source contains duplicate layer IDs."));
        return false;
    }
    if (!smartLayerReferencesResolveRecursive(decoded, registry)) {
        setError(errorMessage, QStringLiteral("The embedded Smart Source contains an unresolved nested Smart Layer."));
        return false;
    }
    QSet<QUuid> dependencies;
    collectSmartSourceDependenciesRecursive(decoded, &dependencies);
    QSet<QUuid> declared;
    for (const QUuid &id : source.dependencies) declared.insert(id);
    if (dependencies != declared) {
        setError(errorMessage, QStringLiteral("The embedded Smart Source dependency metadata does not match its contents."));
        return false;
    }
    const QColorSpace decodedWorkingSpace = documentWorkingQtSpace(*decodedColour);
    setRasterLayerColourSpaceRecursive(decoded, decodedWorkingSpace);
    if (!bindSmartPresentationsRecursive(decoded, registry, decodedWorkingSpace, errorMessage)) {
        return false;
    }
    *layers = std::move(decoded);
    if (canvasSize) *canvasSize = decodedSize;
    if (colourState) *colourState = *decodedColour;
    if (resolutionX) *resolutionX = decodedResolutionX;
    if (resolutionY) *resolutionY = decodedResolutionY;
    if (colourModel) {
        *colourModel = decodedColourModel == QStringLiteral("grayscale")
            ? DocumentColourModel::Grayscale : DocumentColourModel::Rgb;
    }
    if (bitDepth) *bitDepth = decodedBitDepth;
    return true;
}

bool validateEmbeddedSmartSources(const SmartSourceRegistry &registry,
                                  QString *errorMessage)
{
    for (const SmartSourceDescriptor &source : registry.descriptors()) {
        if (!source.hasEmbeddedDocument()) continue;
        if (source.storage != SmartSourceStorage::Embedded) {
            setError(errorMessage, QStringLiteral(
                "A linked Smart Source cannot carry authoritative embedded contents."));
            return false;
        }
        QVector<LayerNode> decoded;
        if (!decodeEmbeddedSmartDocument(source, registry, &decoded, nullptr, nullptr,
                                         nullptr, nullptr, nullptr, nullptr, errorMessage)) {
            return false;
        }
        if (!source.hasCurrentPresentation()) {
            setError(errorMessage, QStringLiteral(
                "An embedded Smart Source has no current presentation cache."));
            return false;
        }
    }
    return true;
}

bool refreshEmbeddedSmartSourcePresentation(SmartSourceDescriptor *source,
                                            const SmartSourceRegistry &registry,
                                            QString *errorMessage)
{
    if (!source || source->storage != SmartSourceStorage::Embedded
        || !source->hasEmbeddedDocument()) {
        setError(errorMessage, QStringLiteral(
            "Only an embedded Smart Source can build an embedded presentation."));
        return false;
    }

    QVector<LayerNode> layers;
    QSize canvasSize;
    DocumentColourState colourState;
    double resolutionX = 72.0;
    double resolutionY = 72.0;
    DocumentColourModel colourModel = DocumentColourModel::Rgb;
    int bitDepth = 8;
    if (!decodeEmbeddedSmartDocument(*source, registry, &layers, &canvasSize,
                                     &colourState, &resolutionX, &resolutionY,
                                     &colourModel, &bitDepth, errorMessage)) {
        return false;
    }

    // A dependent source revision can advance while this embedded payload
    // still records the last revision it observed. Bring those lightweight
    // references forward before binding presentations and re-serialising the
    // source document. This changes cache identity, not editable pixel data.
    for (const QUuid &dependency : source->dependencies) {
        const SmartSourceDescriptor *nested = registry.find(dependency);
        if (!nested) {
            setError(errorMessage, QStringLiteral(
                "An embedded Smart Source dependency could not be resolved."));
            return false;
        }
        synchronizeSmartSourceRevisionRecursive(layers, dependency, nested->revision);
    }
    const QColorSpace workingSpace = documentWorkingQtSpace(colourState);
    if (!bindSmartPresentationsRecursive(layers, registry, workingSpace, errorMessage)) {
        return false;
    }

    bool payloadOk = false;
    source->embeddedDocument = encodeEmbeddedSmartDocument(
        layers, canvasSize, resolutionX, resolutionY, colourModel, bitDepth,
        colourState, &payloadOk);
    if (!payloadOk) {
        setError(errorMessage, QStringLiteral(
            "Could not refresh the embedded Smart Source document metadata."));
        return false;
    }

    QImage transparentSource(canvasSize,
                             bitDepth > 8 ? QImage::Format_RGBA64
                                          : QImage::Format_RGBA8888);
    if (transparentSource.isNull()) {
        setError(errorMessage, QStringLiteral(
            "Could not allocate the embedded Smart Source canvas."));
        return false;
    }
    transparentSource.fill(Qt::transparent);
    transparentSource.setColorSpace(workingSpace);

    QVector<QUuid> rootIds;
    rootIds.reserve(layers.size());
    for (const LayerNode &layer : layers) rootIds.push_back(layer.id);
    QRectF boundsF = ImageProcessor::contentBounds(
        transparentSource, layers, rootIds, canvasSize);
    const QRectF storageBounds = rasterStorageBoundsRecursive(
        transparentSource, layers, canvasSize);
    if (!storageBounds.isEmpty()) {
        boundsF = boundsF.isEmpty() ? storageBounds : boundsF.united(storageBounds);
    }
    const QSize radius = maximumSpatialAdjustmentRadius2D(layers);
    if (!boundsF.isEmpty()) {
        const int safety = SpatialFilterContract::DefaultSafetyPadding;
        boundsF.adjust(-(radius.width() + safety), -(radius.height() + safety),
                       radius.width() + safety, radius.height() + safety);
    }

    QRect renderBounds = boundsF.isEmpty()
        ? QRect(0, 0, 1, 1) : boundsF.toAlignedRect();
    const qint64 presentationPixels = qint64(renderBounds.width()) * renderBounds.height();
    const qint64 presentationBytes = presentationPixels
        * (bitDepth > 8 ? qint64(8) : qint64(4));
    if (renderBounds.width() < 1 || renderBounds.height() < 1
        || renderBounds.width() > 32768 || renderBounds.height() > 32768
        || presentationPixels > qint64(64) * 1024 * 1024
        || presentationBytes > qint64(256) * 1024 * 1024) {
        setError(errorMessage, QStringLiteral(
            "The edited Smart Layer presentation is too large to materialise safely."));
        return false;
    }

    QImage presentation = ImageProcessor::renderUnclippedRegionPreservingHiddenRgb(
        transparentSource, layers, renderBounds, canvasSize, nullptr,
        colourState.processingCompatibility);
    if (presentation.isNull()) {
        setError(errorMessage, QStringLiteral(
            "Could not render the edited Smart Layer presentation."));
        return false;
    }
    presentation = straightRgbaImage(presentation);
    presentation.setColorSpace(workingSpace);

    const QRect meaningful = nonZeroRgbaBounds(presentation);
    if (!meaningful.isEmpty()) {
        presentation = presentation.copy(meaningful);
        renderBounds = QRect(renderBounds.topLeft() + meaningful.topLeft(),
                             meaningful.size());
    } else {
        presentation = QImage(QSize(1, 1), bitDepth > 8
            ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        if (presentation.isNull()) {
            setError(errorMessage, QStringLiteral(
                "Could not allocate an empty Smart Layer presentation."));
            return false;
        }
        presentation.fill(Qt::transparent);
        presentation.setColorSpace(workingSpace);
        renderBounds = QRect(0, 0, 1, 1);
    }

    source->presentationImage = std::move(presentation);
    source->presentationBounds = renderBounds;
    source->presentationRevision = source->revision;
    return true;
}

bool refreshAffectedEmbeddedSmartPresentations(
    SmartSourceRegistry *registry,
    const QSet<QUuid> &affected,
    QString *errorMessage)
{
    if (!registry) return false;
    QSet<QUuid> pending;
    for (const QUuid &id : affected) {
        const SmartSourceDescriptor *source = registry->find(id);
        if (source && source->storage == SmartSourceStorage::Embedded
            && source->hasEmbeddedDocument()) {
            pending.insert(id);
        }
    }

    while (!pending.isEmpty()) {
        bool progressed = false;
        const auto candidates = pending.values();
        for (const QUuid &id : candidates) {
            const SmartSourceDescriptor *current = registry->find(id);
            if (!current) return false;
            bool dependencyPending = false;
            for (const QUuid &dependency : current->dependencies) {
                if (pending.contains(dependency)) {
                    dependencyPending = true;
                    break;
                }
            }
            if (dependencyPending) continue;

            SmartSourceDescriptor refreshed = *current;
            if (!refreshEmbeddedSmartSourcePresentation(&refreshed, *registry,
                                                        errorMessage)
                || !registry->replace(refreshed, errorMessage)) {
                return false;
            }
            pending.remove(id);
            progressed = true;
        }
        if (!progressed) {
            setError(errorMessage, QStringLiteral(
                "The Smart Source dependency graph could not be refreshed without a cycle."));
            return false;
        }
    }
    return true;
}

bool rootRequiresBackdrop(const LayerNode &layer)
{
    return layer.type == LayerType::Adjustment
        || layer.blendMode != BlendMode::Copy
        || (layer.type == LayerType::Group
            && layer.groupCompositeMode == GroupCompositeMode::PassThrough);
}

bool nestedSmartPresentationsAreCurrent(const QVector<LayerNode> &layers,
                                        const SmartSourceRegistry &sources,
                                        QString *errorMessage)
{
    for (const LayerNode &layer : layers) {
        if (layer.type == LayerType::Smart) {
            const SmartSourceDescriptor *source = sources.find(layer.smartSource.sourceId);
            if (!source || !source->hasCurrentPresentation()
                || layer.smartSource.observedSourceRevision != source->revision) {
                setError(errorMessage, QStringLiteral(
                    "The selection contains a Smart Layer whose source presentation is not current."));
                return false;
            }
        }
        if (!nestedSmartPresentationsAreCurrent(layer.children, sources, errorMessage)) {
            return false;
        }
    }
    return true;
}

LayerNode duplicateLayerRecursive(const LayerNode &source,
                                  QHash<QUuid, QUuid> *idMap)
{
    LayerNode duplicate = source;
    const QUuid newId = QUuid::createUuid();
    if (idMap) {
        idMap->insert(source.id, newId);
    }
    duplicate.id = newId;
    duplicate.revision = std::max<quint64>(1, source.revision);
    if (duplicate.type == LayerType::Text) {
        duplicate.textData.revision = std::max<quint64>(1, duplicate.textData.revision + 1);
    }
    if (duplicate.type == LayerType::Vector) {
        for (VectorShape &shape : duplicate.vectorData.objects) {
            shape.id = QUuid::createUuid();
            for (VectorPathNode &node : shape.bezierPath.nodes) {
                node.id = QUuid::createUuid();
            }
            for (VectorBezierPath &path : shape.additionalBezierPaths) {
                for (VectorPathNode &node : path.nodes) {
                    node.id = QUuid::createUuid();
                }
            }
            ++shape.revision;
        }
        duplicate.vectorData.normalise();
    }
    duplicate.children.clear();
    duplicate.children.reserve(source.children.size());
    for (const LayerNode &child : source.children) {
        duplicate.children.push_back(duplicateLayerRecursive(child, idMap));
    }
    return duplicate;
}

QString duplicateLayerName(const QString &sourceName,
                           const QVector<LayerNode> &siblings)
{
    const QString base = sourceName.trimmed().isEmpty()
        ? QStringLiteral("Layer")
        : sourceName.trimmed();
    QSet<QString> existing;
    for (const LayerNode &sibling : siblings) {
        existing.insert(sibling.name.trimmed().toCaseFolded());
    }

    QString candidate = QStringLiteral("%1 copy").arg(base);
    int suffix = 2;
    while (existing.contains(candidate.toCaseFolded())) {
        candidate = QStringLiteral("%1 copy %2").arg(base).arg(suffix++);
    }
    return candidate;
}

bool subtreeIdsAreUniqueAndAbsent(const LayerNode &layer,
                                  const QVector<LayerNode> &existingLayers,
                                  QSet<QUuid> *subtreeIds)
{
    if (layer.id.isNull() || subtreeIds->contains(layer.id)
        || findLayerRecursive(existingLayers, layer.id)) {
        return false;
    }
    subtreeIds->insert(layer.id);
    for (const LayerNode &child : layer.children) {
        if (!subtreeIdsAreUniqueAndAbsent(child, existingLayers, subtreeIds)) {
            return false;
        }
    }
    return true;
}


QImage straightRgbaImage(const QImage &image)
{
    if (image.isNull()) {
        return {};
    }
    const QImage::Format targetFormat = image.depth() > 32
        ? QImage::Format_RGBA64
        : QImage::Format_RGBA8888;
    QImage converted = image.format() == targetFormat
        ? image
        : image.convertToFormat(targetFormat);
    converted.setColorSpace(image.colorSpace());
    return converted;
}

struct LinkedTargetSnapshot {
    QUuid documentId;
    QByteArray fingerprint;
    QString displayName;
    QStringList dependencyWarnings;
    // Runtime-only identity closure for selective open-document propagation.
    // Includes this target document and every resolved nested linked document.
    QVector<QUuid> resolvedDocumentIds;
    QImage presentation;
    QRect presentationBounds;
};

bool renderLinkedTargetPresentation(const PhotoDocument &linked,
                                    LinkedTargetSnapshot *snapshot,
                                    QString *errorMessage)
{
    if (!snapshot) return false;
    QImage presentation = ImageProcessor::renderPreservingHiddenRgb(
        linked.sourceImage(), linked.layers(), nullptr, linked.sourceImage().size(),
        linked.colourState().processingCompatibility);
    if (presentation.isNull()) {
        setError(errorMessage, QStringLiteral(
            "The linked .vfxphoto source could not be rendered safely."));
        return false;
    }
    presentation = straightRgbaImage(presentation);
    presentation.setColorSpace(linked.sourceImage().colorSpace());
    snapshot->presentation = std::move(presentation);
    snapshot->presentationBounds = QRect(QPoint(0, 0), linked.sourceImage().size());
    return true;
}

bool loadLinkedTargetSnapshot(const QString &filePath,
                              LinkedTargetSnapshot *snapshot,
                              PhotoDocument *loadedDocument,
                              QString *errorMessage,
                              const bool renderPresentation = true)
{
    if (!snapshot) return false;
    const QString canonical = canonicalProjectPath(filePath);
    if (canonical.isEmpty()
        || QFileInfo(canonical).suffix().compare(QStringLiteral("vfxphoto"),
                                                Qt::CaseInsensitive) != 0) {
        setError(errorMessage, QStringLiteral(
            "Linked Smart Layers require an external .vfxphoto project source."));
        return false;
    }
    if (!QFileInfo::exists(canonical)) {
        setError(errorMessage, QStringLiteral("The linked .vfxphoto source is missing."));
        return false;
    }
    if (linkedProjectLoadStack.contains(canonical)) {
        setError(errorMessage, QStringLiteral(
            "Circular linked Smart Layer dependency detected while opening %1.")
                .arg(QFileInfo(canonical).fileName()));
        return false;
    }

    PhotoDocument linked;
    QString loadError;
    if (!linked.loadProject(canonical, &loadError)) {
        setError(errorMessage, loadError.isEmpty()
            ? QStringLiteral("The linked .vfxphoto source could not be opened.")
            : loadError);
        return false;
    }
    QString fingerprintError;
    const QByteArray fileFingerprint = fileSha256(canonical, &fingerprintError);
    if (fileFingerprint.size() != 32) {
        setError(errorMessage, fingerprintError.isEmpty()
            ? QStringLiteral("The linked .vfxphoto source could not be fingerprinted.")
            : fingerprintError);
        return false;
    }
    // The authoritative project bytes identify direct edits, while resolved
    // nested-link fingerprints make transitive changes observable without
    // forcing the intermediate .vfxphoto file itself to be rewritten.
    QCryptographicHash resolvedFingerprint(QCryptographicHash::Sha256);
    resolvedFingerprint.addData(QByteArrayLiteral("VFXPhotoLab/linked-source/v1\0"));
    resolvedFingerprint.addData(fileFingerprint);
    QVector<SmartSourceDescriptor> nestedSources = linked.smartSources().descriptors();
    std::sort(nestedSources.begin(), nestedSources.end(),
              [](const SmartSourceDescriptor &left, const SmartSourceDescriptor &right) {
                  return left.id.toString(QUuid::WithoutBraces)
                      < right.id.toString(QUuid::WithoutBraces);
              });
    QSet<QUuid> resolvedDocumentIds;
    if (!linked.documentIdentity().isNull()) {
        resolvedDocumentIds.insert(linked.documentIdentity());
    }
    for (const SmartSourceDescriptor &nested : nestedSources) {
        if (nested.storage != SmartSourceStorage::Linked) continue;
        resolvedFingerprint.addData(nested.id.toRfc4122());
        resolvedFingerprint.addData(nested.linkedDocumentId.toRfc4122());
        resolvedFingerprint.addData(nested.linkedContentFingerprint);
        if (!nested.linkedDocumentId.isNull()) {
            resolvedDocumentIds.insert(nested.linkedDocumentId);
        }
        for (const QUuid &resolvedId : nested.linkedResolvedDocumentIds) {
            if (!resolvedId.isNull()) resolvedDocumentIds.insert(resolvedId);
        }
    }
    if (resolvedDocumentIds.size() > SmartSourceDescriptor::MaximumDependencies) {
        setError(errorMessage, QStringLiteral(
            "The linked .vfxphoto dependency identity closure exceeds safety limits."));
        return false;
    }
    const QByteArray fingerprint = resolvedFingerprint.result();

    snapshot->documentId = linked.documentIdentity();
    snapshot->fingerprint = fingerprint;
    snapshot->displayName = QFileInfo(canonical).completeBaseName();
    snapshot->dependencyWarnings = linked.linkedSourceWarnings();
    snapshot->resolvedDocumentIds = resolvedDocumentIds.values();
    std::sort(snapshot->resolvedDocumentIds.begin(), snapshot->resolvedDocumentIds.end(),
              [](const QUuid &left, const QUuid &right) {
                  return left.toString(QUuid::WithoutBraces)
                      < right.toString(QUuid::WithoutBraces);
              });
    snapshot->presentationBounds = QRect(QPoint(0, 0), linked.sourceImage().size());
    if (renderPresentation
        && !renderLinkedTargetPresentation(linked, snapshot, errorMessage)) {
        return false;
    }
    if (loadedDocument) *loadedDocument = std::move(linked);
    return true;
}

QString linkedDependencyWarning(const LinkedTargetSnapshot &snapshot)
{
    if (snapshot.dependencyWarnings.isEmpty()) return {};
    QStringList displayed = snapshot.dependencyWarnings;
    displayed.removeDuplicates();
    constexpr int MaximumNestedWarnings = 6;
    if (displayed.size() > MaximumNestedWarnings) {
        const int remaining = displayed.size() - MaximumNestedWarnings;
        displayed = displayed.mid(0, MaximumNestedWarnings);
        displayed.push_back(QStringLiteral("…and %1 more nested linked-source issue(s)")
                                .arg(remaining));
    }
    return QStringLiteral("The linked document has nested linked-source issue(s): %1")
        .arg(displayed.join(QStringLiteral(" | "))).left(16000);
}

void remapSmartSourceReferencesRecursive(
    QVector<LayerNode> &layers, const QHash<QUuid, QUuid> &sourceIdMap)
{
    for (LayerNode &layer : layers) {
        if (layer.type == LayerType::Smart) {
            const auto mapped = sourceIdMap.constFind(layer.smartSource.sourceId);
            if (mapped != sourceIdMap.cend()) {
                layer.smartSource.sourceId = mapped.value();
            }
        }
        remapSmartSourceReferencesRecursive(layer.children, sourceIdMap);
    }
}

QImage alphaSafeScaledRgba(const QImage &image, const QSize &size)
{
    if (image.isNull() || size.isEmpty()) {
        return {};
    }
    if (image.size() == size) {
        return straightRgbaImage(image);
    }

    const bool sixteenBit = image.depth() > 32;
    if (sixteenBit) {
        QImage rgb = image.convertToFormat(QImage::Format_RGBA64);
        QImage alpha(image.size(), QImage::Format_Grayscale16);
        for (int y = 0; y < image.height(); ++y) {
            auto *rgbRow = reinterpret_cast<QRgba64 *>(rgb.scanLine(y));
            auto *alphaRow = reinterpret_cast<quint16 *>(alpha.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const QRgba64 pixel = rgbRow[x];
                alphaRow[x] = pixel.alpha();
                rgbRow[x] = QRgba64::fromRgba64(pixel.red(),
                                                pixel.green(),
                                                pixel.blue(),
                                                65535);
            }
        }
        rgb = rgb.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                  .convertToFormat(QImage::Format_RGBA64);
        alpha = alpha.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_Grayscale16);
        if (rgb.isNull() || alpha.isNull()) {
            return {};
        }
        for (int y = 0; y < size.height(); ++y) {
            auto *rgbRow = reinterpret_cast<QRgba64 *>(rgb.scanLine(y));
            const auto *alphaRow = reinterpret_cast<const quint16 *>(alpha.constScanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                const QRgba64 pixel = rgbRow[x];
                rgbRow[x] = QRgba64::fromRgba64(pixel.red(),
                                                pixel.green(),
                                                pixel.blue(),
                                                alphaRow[x]);
            }
        }
        rgb.setColorSpace(image.colorSpace());
        return rgb;
    }

    QImage rgb = image.convertToFormat(QImage::Format_RGBA8888);
    QImage alpha(image.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < image.height(); ++y) {
        uchar *rgbRow = rgb.scanLine(y);
        uchar *alphaRow = alpha.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            alphaRow[x] = rgbRow[x * 4 + 3];
            rgbRow[x * 4 + 3] = 255;
        }
    }
    rgb = rgb.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
              .convertToFormat(QImage::Format_RGBA8888);
    alpha = alpha.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_Grayscale8);
    if (rgb.isNull() || alpha.isNull()) {
        return {};
    }
    for (int y = 0; y < size.height(); ++y) {
        uchar *rgbRow = rgb.scanLine(y);
        const uchar *alphaRow = alpha.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            rgbRow[x * 4 + 3] = alphaRow[x];
        }
    }
    rgb.setColorSpace(image.colorSpace());
    return rgb;
}

void normaliseRasterLayersRecursive(QVector<LayerNode> &layers,
                                    const QSize &documentSize,
                                    QStringList *warnings)
{
    for (LayerNode &layer : layers) {
        if ((layer.type == LayerType::Raster || layer.type == LayerType::BaseImage)
            && !layer.rasterImage.isNull()) {
            QImage pixels = layer.rasterImage;
            if (layer.rasterReferenceSize.isEmpty()) {
                // Version 1-6 projects before 0.7.0b treated every stored raster
                // as spanning the whole document. Preserve that contract while
                // recording the explicit reference extent used by crops.
                if (pixels.size() != documentSize) {
                    pixels = alphaSafeScaledRgba(pixels, documentSize);
                    if (pixels.isNull()) {
                        layer.rasterImage = {};
                        if (warnings) {
                            warnings->push_back(
                                QStringLiteral("The raster pixels on layer ‘%1’ had invalid dimensions and were discarded.")
                                    .arg(layer.name));
                        }
                        normaliseRasterLayersRecursive(layer.children, documentSize, warnings);
                        continue;
                    }
                    if (warnings) {
                        warnings->push_back(
                            QStringLiteral("The raster pixels on layer ‘%1’ had unexpected dimensions and were resized to the document.")
                                .arg(layer.name));
                    }
                }
                layer.rasterReferenceSize = documentSize;
            }
            layer.rasterImage = straightRgbaImage(pixels);
        } else if ((layer.type == LayerType::Raster || layer.type == LayerType::BaseImage)
                   && layer.rasterReferenceSize.isEmpty()) {
            layer.rasterReferenceSize = documentSize;
        }
        normaliseRasterLayersRecursive(layer.children, documentSize, warnings);
    }
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

void normaliseMasksRecursive(QVector<LayerNode> &layers,
                             const QSize &documentSize,
                             QStringList *warnings)
{
    for (LayerNode &layer : layers) {
        const bool legacyMaskExtent = layer.maskReferenceSize.isEmpty();
        if (!layer.maskImage.isNull()) {
            if (legacyMaskExtent) {
                layer.maskReferenceSize = documentSize;
            }
            if (layer.maskImage.format() != QImage::Format_Grayscale8) {
                const bool expectedIndexedMask =
                    layer.maskImage.format() == QImage::Format_Indexed8;
                const QImage converted = layer.maskImage.convertToFormat(QImage::Format_Grayscale8);
                if (converted.isNull()) {
                    layer.maskImage = {};
                    layer.maskEnabled = true;
                    layer.maskInverted = false;
                    if (warnings) {
                        warnings->push_back(
                            QStringLiteral("The mask on layer ‘%1’ could not be converted and was discarded.")
                                .arg(layer.name));
                    }
                } else {
                    layer.maskImage = converted;
                    if (warnings && !expectedIndexedMask) {
                        warnings->push_back(
                            QStringLiteral("The mask on layer ‘%1’ used an unexpected pixel format and was converted.")
                                .arg(layer.name));
                    }
                }
            }

            if (legacyMaskExtent && !layer.maskImage.isNull()
                && layer.maskImage.size() != QSize(1, 1)
                && layer.maskImage.size() != documentSize) {
                const QImage resized = layer.maskImage.scaled(
                    documentSize,
                    Qt::IgnoreAspectRatio,
                    Qt::SmoothTransformation);
                if (resized.isNull()) {
                    layer.maskImage = {};
                    layer.maskEnabled = true;
                    layer.maskInverted = false;
                    if (warnings) {
                        warnings->push_back(
                            QStringLiteral("The mask on layer ‘%1’ had invalid dimensions and was discarded.")
                                .arg(layer.name));
                    }
                } else {
                    layer.maskImage = resized.convertToFormat(QImage::Format_Grayscale8);
                    if (warnings) {
                        warnings->push_back(
                            QStringLiteral("The mask on layer ‘%1’ had unexpected dimensions and was resized to the document.")
                                .arg(layer.name));
                    }
                }
            }
        }

        if (layer.maskImage.isNull()) {
            layer.maskEnabled = true;
            layer.maskInverted = false;
        }
        normaliseMasksRecursive(layer.children, documentSize, warnings);
    }
}

LayerNode legacyAdjustmentFromJson(const QJsonObject &object, bool *ok)
{
    bool typeOk = false;
    LayerNode layer;
    layer.type = LayerType::Adjustment;
    layer.adjustmentType = adjustmentTypeFromString(
        object.value(QStringLiteral("type")).toString(), &typeOk);
    if (!typeOk) {
        if (ok) {
            *ok = false;
        }
        return layer;
    }

    const QUuid parsedId(object.value(QStringLiteral("id")).toString());
    if (!parsedId.isNull()) {
        layer.id = parsedId;
    }
    layer.name = object.value(QStringLiteral("name")).toString(
        defaultAdjustmentName(layer.adjustmentType));
    layer.visible = object.value(QStringLiteral("enabled")).toBool(true);
    layer.exposure = object.value(QStringLiteral("exposure")).toDouble(0.0);
    layer.contrast = object.value(QStringLiteral("contrast")).toDouble(0.0);
    layer.saturation = object.value(QStringLiteral("saturation")).toDouble(0.0);
    layer.blackPoint = object.value(QStringLiteral("blackPoint")).toDouble(0.0);
    layer.whitePoint = object.value(QStringLiteral("whitePoint")).toDouble(1.0);
    layer.gamma = object.value(QStringLiteral("gamma")).toDouble(1.0);
    AdjustmentData migrated;
    migrated.reset(layer.adjustmentType);
    switch (layer.adjustmentType) {
    case AdjustmentType::Exposure:
        migrated.parameters = ExposureParameters {layer.exposure};
        break;
    case AdjustmentType::Contrast:
        migrated.parameters = ContrastParameters {layer.contrast};
        break;
    case AdjustmentType::Saturation:
        migrated.parameters = SaturationParameters {layer.saturation};
        break;
    case AdjustmentType::Levels: {
        LevelsParameters levels;
        LevelsChannelParameters &rgb = levels.channel(AdjustmentChannel::Rgb);
        rgb.inputBlack = layer.blackPoint;
        rgb.inputWhite = layer.whitePoint;
        rgb.gamma = layer.gamma;
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
    layer.setAdjustmentData(migrated);
    if (ok) {
        *ok = true;
    }
    return layer;
}

} // namespace

bool PhotoDocument::hasImage() const
{
    return !m_sourceImage.isNull();
}

bool PhotoDocument::isModified() const
{
    return m_modified;
}

void PhotoDocument::setModified(const bool modified)
{
    m_modified = modified;
}

const QImage &PhotoDocument::sourceImage() const
{
    return m_sourceImage;
}

const QImage &PhotoDocument::previewSource() const
{
    return m_previewSource;
}

const QString &PhotoDocument::sourcePath() const
{
    return m_sourcePath;
}

const QString &PhotoDocument::projectPath() const
{
    return m_projectPath;
}

const QUuid &PhotoDocument::documentIdentity() const
{
    return m_documentIdentity;
}

const QStringList &PhotoDocument::loadWarnings() const
{
    return m_loadWarnings;
}

const QStringList &PhotoDocument::colourResourceWarnings() const
{
    return m_colourResourceWarnings;
}

const QStringList &PhotoDocument::linkedSourceWarnings() const
{
    return m_linkedSourceWarnings;
}

QString PhotoDocument::displayName() const
{
    if (!m_projectPath.isEmpty()) {
        return QFileInfo(m_projectPath).fileName();
    }
    if (!m_sourcePath.isEmpty()) {
        return QFileInfo(m_sourcePath).fileName();
    }
    return m_documentName.isEmpty() ? QStringLiteral("Untitled Photo") : m_documentName;
}

const QString &PhotoDocument::documentName() const
{
    return m_documentName;
}

DocumentColourModel PhotoDocument::colourModel() const
{
    return m_colourModel;
}

bool PhotoDocument::isBlankDocument() const
{
    return m_blankDocument;
}

double PhotoDocument::resolutionX() const
{
    return m_resolutionX;
}

double PhotoDocument::resolutionY() const
{
    return m_resolutionY;
}

QString PhotoDocument::colourProfileName() const
{
    const QString managedName = m_colourState.workingSpace.displayName.trimmed();
    if (!managedName.isEmpty()) {
        if (m_colourState.untaggedPolicyApplied
            && m_colourState.untaggedPolicy == UntaggedImagePolicy::AssumeSRgb
            && m_colourState.workingSpace.stableFingerprint()
                == m_colourState.inputProfile.stableFingerprint()) {
            return QStringLiteral("%1 (assumed)").arg(managedName);
        }
        return managedName;
    }
    const QColorSpace colourSpace = m_sourceImage.colorSpace();
    if (colourSpace == QColorSpace(QColorSpace::SRgbLinear)) {
        return QStringLiteral("Linear sRGB");
    }
    if (colourSpace == QColorSpace(QColorSpace::SRgb)) {
        return QStringLiteral("sRGB");
    }
    const QString description = colourSpace.description().trimmed();
    return description.isEmpty() ? QStringLiteral("Unmanaged") : description;
}

const DocumentColourState &PhotoDocument::colourState() const
{
    return m_colourState;
}

quint64 PhotoDocument::colourStateRevision() const
{
    return m_colourState.revision;
}

QByteArray PhotoDocument::colourStateFingerprint() const
{
    return m_colourState.stableFingerprint();
}

qint64 PhotoDocument::estimatedResidentBytes() const
{
    QSet<qint64> imageKeys;
    qint64 total = 0;
    const auto addImage = [&](const QImage &image) {
        if (image.isNull() || imageKeys.contains(image.cacheKey())) {
            return;
        }
        imageKeys.insert(image.cacheKey());
        total += static_cast<qint64>(image.sizeInBytes());
    };
    std::function<void(const QVector<LayerNode> &)> collectLayers;
    collectLayers = [&](const QVector<LayerNode> &layers) {
        for (const LayerNode &layer : layers) {
            addImage(layer.rasterImage);
            addImage(layer.maskImage);
            addImage(layer.smartPresentationImage);
            if (layer.type == LayerType::Vector) {
                total += layer.vectorData.estimatedBytes();
            }
            if (layer.type == LayerType::Smart) {
                total += liveFilterStackEstimatedBytes(layer.liveFilters);
            }
            total += layerEffectStackEstimatedBytes(layer.layerEffects);
            collectLayers(layer.children);
        }
    };
    addImage(m_sourceImage);
    addImage(m_previewSource);
    collectLayers(m_layers);
    total += m_selectionMask.estimatedResidentBytes();
    total += m_colourState.estimatedBytes();
    total += m_smartSources.estimatedBytes();
    return total;
}

const QVector<LayerNode> &PhotoDocument::layers() const
{
    return m_layers;
}

const SmartSourceRegistry &PhotoDocument::smartSources() const
{
    return m_smartSources;
}

bool PhotoDocument::synchronizeSmartLayerPresentations(QString *errorMessage)
{
    return bindSmartPresentationsRecursive(m_layers, m_smartSources,
                                           m_sourceImage.colorSpace(),
                                           errorMessage);
}

bool PhotoDocument::embeddedSmartSourceLayers(const QUuid &sourceId,
                                               QVector<LayerNode> *layers,
                                               QSize *canvasSize,
                                               DocumentColourState *colourState,
                                               QString *errorMessage) const
{
    if (!layers || sourceId.isNull()) {
        setError(errorMessage, QStringLiteral("A valid Smart Source identity is required."));
        return false;
    }
    const SmartSourceDescriptor *source = m_smartSources.find(sourceId);
    if (!source) {
        setError(errorMessage, QStringLiteral("The Smart Source identity does not exist."));
        return false;
    }
    return decodeEmbeddedSmartDocument(*source, m_smartSources, layers, canvasSize,
                                       colourState, nullptr, nullptr, nullptr,
                                       nullptr, errorMessage);
}

bool PhotoDocument::dependsOnLinkedDocument(const QUuid &documentId,
                                            const QString &projectPath) const
{
    if (documentId.isNull() && projectPath.trimmed().isEmpty()) return false;
    for (const SmartSourceDescriptor &source : m_smartSources.descriptors()) {
        if (linkedDescriptorMatchesTrigger(source, m_projectPath,
                                           documentId, projectPath)) {
            return true;
        }
    }
    return false;
}

QString PhotoDocument::resolvedLinkedSmartSourcePath(const QUuid &smartLayerId) const
{
    const LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart) return {};
    const SmartSourceDescriptor *source = m_smartSources.find(layer->smartSource.sourceId);
    if (!source || source->storage != SmartSourceStorage::Linked) return {};
    return resolvedLinkedPath(source->linkedPath, m_projectPath);
}

QUuid PhotoDocument::createLinkedSmartLayer(const QString &projectPath,
                                             const QUuid &selection,
                                             QString *errorMessage)
{
    if (!hasImage()) {
        setError(errorMessage, QStringLiteral("There is no document to receive a linked Smart Layer."));
        return {};
    }
    const QString absolute = canonicalProjectPath(projectPath);
    if (absolute.isEmpty() || !absolute.endsWith(QStringLiteral(".vfxphoto"), Qt::CaseInsensitive)) {
        setError(errorMessage, QStringLiteral("Linked Smart Layers require a .vfxphoto source."));
        return {};
    }
    LinkedProjectLoadGuard ownerGuard(m_projectPath);
    StrictLinkedCycleDetectionGuard strictCycleGuard;
    LinkedTargetSnapshot target;
    QString linkError;
    if (!loadLinkedTargetSnapshot(absolute, &target, nullptr, &linkError)) {
        setError(errorMessage, linkError);
        return {};
    }
    if (!m_documentIdentity.isNull() && target.documentId == m_documentIdentity) {
        setError(errorMessage, QStringLiteral("A document cannot link to itself as a Smart Layer source."));
        return {};
    }

    SmartSourceDescriptor source;
    source.storage = SmartSourceStorage::Linked;
    source.name = target.displayName.isEmpty() ? QStringLiteral("Linked Smart Layer") : target.displayName;
    source.linkedPath = persistedLinkedPath(absolute, m_projectPath);
    source.linkedDocumentId = target.documentId;
    source.linkedContentFingerprint = target.fingerprint;
    source.linkedAvailable = true;
    source.linkedRuntimeWarning = linkedDependencyWarning(target);
    source.linkedResolvedDocumentIds = target.resolvedDocumentIds;
    source.presentationImage = target.presentation;
    source.presentationBounds = target.presentationBounds;
    source.presentationRevision = source.revision;

    SmartSourceRegistry preparedSources = m_smartSources;
    if (!preparedSources.insert(source, errorMessage)) return {};

    LayerNode smart;
    smart.type = LayerType::Smart;
    smart.name = source.name;
    smart.smartSource.sourceId = source.id;
    smart.smartSource.observedSourceRevision = source.revision;
    smart.smartPresentationImage = source.presentationImage;
    smart.smartPresentationReferenceSize = source.presentationBounds.size();
    smart.smartPresentationReferenceOrigin = source.presentationBounds.topLeft();
    const QUuid layerId = smart.id;

    QVector<LayerNode> preparedLayers = m_layers;
    const auto insertPrepared = [&](QVector<LayerNode> *roots) -> bool {
        if (!selection.isNull()) {
            LayerNode *selected = findLayerRecursive(*roots, selection);
            if (selected && selected->type == LayerType::Group) {
                selected->children.prepend(smart);
                return true;
            }
            int index = -1;
            if (QVector<LayerNode> *siblings = findSiblingsRecursive(*roots, selection, &index)) {
                siblings->insert(index, smart);
                return true;
            }
        }
        roots->prepend(smart);
        return true;
    };
    insertPrepared(&preparedLayers);
    if (!smartLayerReferencesResolveRecursive(preparedLayers, preparedSources)
        || !bindSmartPresentationsRecursive(preparedLayers, preparedSources,
                                            m_sourceImage.colorSpace(), errorMessage)) {
        return {};
    }
    m_smartSources = std::move(preparedSources);
    m_layers = std::move(preparedLayers);
    m_linkedSourceWarnings = linkedWarningsForRegistry(m_smartSources, m_projectPath);
    m_modified = true;
    return layerId;
}

bool PhotoDocument::refreshLinkedSmartSources(QHash<QUuid, quint64> *changedRevisions,
                                               QStringList *warnings,
                                               QString *errorMessage,
                                               const QUuid &triggerDocumentId,
                                               const QString &triggerProjectPath)
{
    if (changedRevisions) changedRevisions->clear();
    SmartSourceRegistry preparedSources = m_smartSources;
    QHash<QUuid, quint64> aggregateChanged;
    LinkedProjectLoadGuard ownerGuard(m_projectPath);

    const QVector<SmartSourceDescriptor> snapshot = preparedSources.descriptors();
    for (const SmartSourceDescriptor &saved : snapshot) {
        if (saved.storage != SmartSourceStorage::Linked) continue;
        if (!linkedDescriptorMatchesTrigger(saved, m_projectPath,
                                            triggerDocumentId, triggerProjectPath)) {
            continue;
        }
        const QString absolute = resolvedLinkedPath(saved.linkedPath, m_projectPath);
        LinkedTargetSnapshot target;
        PhotoDocument linkedDocument;
        QString linkError;
        if (!loadLinkedTargetSnapshot(absolute, &target, &linkedDocument, &linkError, false)) {
            if (strictLinkedCycleDetectionDepth > 0
                && linkError.contains(QStringLiteral("Circular linked Smart Layer dependency"))) {
                setError(errorMessage, linkError);
                return false;
            }
            SmartSourceDescriptor unavailable = *preparedSources.find(saved.id);
            unavailable.linkedAvailable = false;
            unavailable.linkedRuntimeWarning = linkError.isEmpty()
                ? QStringLiteral("The linked Smart Source is unavailable.") : linkError;
            if (!preparedSources.replace(unavailable, errorMessage)) return false;
            continue;
        }
        const SmartSourceDescriptor *currentPtr = preparedSources.find(saved.id);
        if (!currentPtr) return false;
        if (!currentPtr->linkedDocumentId.isNull()
            && currentPtr->linkedDocumentId != target.documentId) {
            SmartSourceDescriptor mismatch = *currentPtr;
            mismatch.linkedAvailable = false;
            mismatch.linkedRuntimeWarning = QStringLiteral(
                "The file at the linked path has a different persistent document identity. Use Relink for the same source or Replace Source intentionally.");
            if (!preparedSources.replace(mismatch, errorMessage)) return false;
            continue;
        }

        const bool contentChanged = currentPtr->linkedContentFingerprint != target.fingerprint
            || currentPtr->linkedDocumentId.isNull()
            || !currentPtr->hasCurrentPresentation();
        QSet<QUuid> affected;
        if (contentChanged) {
            if (!renderLinkedTargetPresentation(linkedDocument, &target, errorMessage)) {
                return false;
            }
            QHash<QUuid, quint64> changed;
            if (!preparedSources.bumpRevisionCascade(saved.id, &changed)) {
                setError(errorMessage, QStringLiteral(
                    "The linked Smart Source revision graph could not be advanced safely."));
                return false;
            }
            for (auto it = changed.cbegin(); it != changed.cend(); ++it) {
                aggregateChanged.insert(it.key(), it.value());
                affected.insert(it.key());
            }
        }

        SmartSourceDescriptor refreshed = *preparedSources.find(saved.id);
        refreshed.linkedDocumentId = target.documentId;
        refreshed.linkedContentFingerprint = target.fingerprint;
        refreshed.linkedAvailable = true;
        refreshed.linkedRuntimeWarning = linkedDependencyWarning(target);
        refreshed.linkedResolvedDocumentIds = target.resolvedDocumentIds;
        if (contentChanged) {
            refreshed.presentationImage = target.presentation;
            refreshed.presentationBounds = target.presentationBounds;
            refreshed.presentationRevision = refreshed.revision;
        }
        if (!preparedSources.replace(refreshed, errorMessage)) return false;
        if (!affected.isEmpty()
            && !refreshAffectedEmbeddedSmartPresentations(&preparedSources, affected,
                                                          errorMessage)) {
            return false;
        }
    }

    QVector<LayerNode> preparedLayers = m_layers;
    for (auto it = aggregateChanged.cbegin(); it != aggregateChanged.cend(); ++it) {
        synchronizeSmartSourceRevisionRecursive(preparedLayers, it.key(), it.value());
    }
    if (!bindSmartPresentationsRecursive(preparedLayers, preparedSources,
                                         m_sourceImage.colorSpace(), errorMessage)) {
        return false;
    }
    const QStringList resolvedWarnings = linkedWarningsForRegistry(
        preparedSources, m_projectPath);
    m_smartSources = std::move(preparedSources);
    m_layers = std::move(preparedLayers);
    if (!aggregateChanged.isEmpty()) m_modified = true;
    m_linkedSourceWarnings = resolvedWarnings;
    if (warnings) *warnings = resolvedWarnings;
    if (changedRevisions) *changedRevisions = aggregateChanged;
    return true;
}

bool PhotoDocument::replaceLinkedSmartSource(const QUuid &smartLayerId,
                                              const QString &projectPath,
                                              QString *errorMessage)
{
    LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart) {
        setError(errorMessage, QStringLiteral("Select one linked Smart Layer first."));
        return false;
    }
    const SmartSourceDescriptor *existing = m_smartSources.find(layer->smartSource.sourceId);
    if (!existing || existing->storage != SmartSourceStorage::Linked) {
        setError(errorMessage, QStringLiteral("The selected Smart Layer is not externally linked."));
        return false;
    }
    const QString absolute = canonicalProjectPath(projectPath);
    LinkedProjectLoadGuard ownerGuard(m_projectPath);
    StrictLinkedCycleDetectionGuard strictCycleGuard;
    LinkedTargetSnapshot target;
    if (!loadLinkedTargetSnapshot(absolute, &target, nullptr, errorMessage)) return false;
    if (target.documentId == m_documentIdentity) {
        setError(errorMessage, QStringLiteral("A document cannot use itself as a linked Smart Source."));
        return false;
    }

    SmartSourceRegistry preparedSources = m_smartSources;
    QHash<QUuid, quint64> changed;
    if (!preparedSources.bumpRevisionCascade(existing->id, &changed)) return false;
    SmartSourceDescriptor replacement = *preparedSources.find(existing->id);
    replacement.name = target.displayName.isEmpty() ? replacement.name : target.displayName;
    replacement.linkedPath = persistedLinkedPath(absolute, m_projectPath);
    replacement.linkedDocumentId = target.documentId;
    replacement.linkedContentFingerprint = target.fingerprint;
    replacement.linkedAvailable = true;
    replacement.linkedRuntimeWarning = linkedDependencyWarning(target);
    replacement.linkedResolvedDocumentIds = target.resolvedDocumentIds;
    replacement.presentationImage = target.presentation;
    replacement.presentationBounds = target.presentationBounds;
    replacement.presentationRevision = replacement.revision;
    if (!preparedSources.replace(replacement, errorMessage)) return false;
    QSet<QUuid> affected;
    for (auto it = changed.cbegin(); it != changed.cend(); ++it) affected.insert(it.key());
    if (!refreshAffectedEmbeddedSmartPresentations(&preparedSources, affected, errorMessage)) return false;

    QVector<LayerNode> preparedLayers = m_layers;
    for (auto it = changed.cbegin(); it != changed.cend(); ++it) {
        synchronizeSmartSourceRevisionRecursive(preparedLayers, it.key(), it.value());
    }
    if (!bindSmartPresentationsRecursive(preparedLayers, preparedSources,
                                         m_sourceImage.colorSpace(), errorMessage)) return false;
    m_smartSources = std::move(preparedSources);
    m_layers = std::move(preparedLayers);
    m_linkedSourceWarnings = linkedWarningsForRegistry(m_smartSources, m_projectPath);
    m_modified = true;
    return true;
}

bool PhotoDocument::relinkLinkedSmartSource(const QUuid &smartLayerId,
                                             const QString &projectPath,
                                             QString *errorMessage)
{
    const LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart) {
        setError(errorMessage, QStringLiteral("Select one linked Smart Layer first."));
        return false;
    }
    const SmartSourceDescriptor *existing = m_smartSources.find(layer->smartSource.sourceId);
    if (!existing || existing->storage != SmartSourceStorage::Linked) {
        setError(errorMessage, QStringLiteral("The selected Smart Layer is not externally linked."));
        return false;
    }
    const QUuid expectedIdentity = existing->linkedDocumentId;
    const QByteArray previousFingerprint = existing->linkedContentFingerprint;
    const QString absolute = canonicalProjectPath(projectPath);
    LinkedProjectLoadGuard ownerGuard(m_projectPath);
    StrictLinkedCycleDetectionGuard strictCycleGuard;
    LinkedTargetSnapshot target;
    if (!loadLinkedTargetSnapshot(absolute, &target, nullptr, errorMessage)) return false;
    if (!expectedIdentity.isNull() && target.documentId != expectedIdentity) {
        setError(errorMessage, QStringLiteral(
            "Relink refused the selected file because its persistent document identity does not match the missing/moved source. Use Replace Source to intentionally choose different contents."));
        return false;
    }
    if (target.documentId == m_documentIdentity) {
        setError(errorMessage, QStringLiteral("A document cannot relink a Smart Source to itself."));
        return false;
    }

    SmartSourceRegistry preparedSources = m_smartSources;
    QHash<QUuid, quint64> changed;
    const bool sourceChanged = previousFingerprint != target.fingerprint
        || !existing->hasCurrentPresentation();
    if (sourceChanged && !preparedSources.bumpRevisionCascade(existing->id, &changed)) {
        return false;
    }
    SmartSourceDescriptor relinked = *preparedSources.find(existing->id);
    relinked.linkedPath = persistedLinkedPath(absolute, m_projectPath);
    relinked.linkedDocumentId = target.documentId;
    relinked.linkedContentFingerprint = target.fingerprint;
    relinked.linkedAvailable = true;
    relinked.linkedRuntimeWarning = linkedDependencyWarning(target);
    relinked.linkedResolvedDocumentIds = target.resolvedDocumentIds;
    relinked.presentationImage = target.presentation;
    relinked.presentationBounds = target.presentationBounds;
    relinked.presentationRevision = relinked.revision;
    if (!preparedSources.replace(relinked, errorMessage)) return false;
    QSet<QUuid> affected;
    for (auto it = changed.cbegin(); it != changed.cend(); ++it) affected.insert(it.key());
    if (!affected.isEmpty()
        && !refreshAffectedEmbeddedSmartPresentations(&preparedSources, affected, errorMessage)) return false;

    QVector<LayerNode> preparedLayers = m_layers;
    for (auto it = changed.cbegin(); it != changed.cend(); ++it) {
        synchronizeSmartSourceRevisionRecursive(preparedLayers, it.key(), it.value());
    }
    if (!bindSmartPresentationsRecursive(preparedLayers, preparedSources,
                                         m_sourceImage.colorSpace(), errorMessage)) return false;
    m_smartSources = std::move(preparedSources);
    m_layers = std::move(preparedLayers);
    m_linkedSourceWarnings = linkedWarningsForRegistry(m_smartSources, m_projectPath);
    m_modified = true;
    return true;
}

bool PhotoDocument::embedLinkedSmartSource(const QUuid &smartLayerId,
                                            QString *errorMessage)
{
    const LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart) {
        setError(errorMessage, QStringLiteral("Select one linked Smart Layer first."));
        return false;
    }
    const SmartSourceDescriptor *existing = m_smartSources.find(layer->smartSource.sourceId);
    if (!existing || existing->storage != SmartSourceStorage::Linked) {
        setError(errorMessage, QStringLiteral("The selected Smart Layer is not externally linked."));
        return false;
    }
    const QString absolute = resolvedLinkedPath(existing->linkedPath, m_projectPath);
    LinkedProjectLoadGuard ownerGuard(m_projectPath);
    StrictLinkedCycleDetectionGuard strictCycleGuard;
    LinkedTargetSnapshot target;
    PhotoDocument linkedDocument;
    if (!loadLinkedTargetSnapshot(absolute, &target, &linkedDocument, errorMessage)) return false;
    if (!existing->linkedDocumentId.isNull()
        && existing->linkedDocumentId != target.documentId) {
        setError(errorMessage, QStringLiteral(
            "Embed refused a different file at the linked path. Relink or Replace Source explicitly first."));
        return false;
    }

    SmartSourceRegistry preparedSources = m_smartSources;
    QHash<QUuid, QUuid> sourceIdMap;
    for (const SmartSourceDescriptor &nested : linkedDocument.m_smartSources.descriptors()) {
        QUuid mapped = nested.id;
        while (preparedSources.contains(mapped) || mapped == existing->id
               || sourceIdMap.values().contains(mapped)) {
            mapped = QUuid::createUuid();
        }
        sourceIdMap.insert(nested.id, mapped);
    }

    QVector<SmartSourceDescriptor> pending;
    pending.reserve(linkedDocument.m_smartSources.size());
    for (const SmartSourceDescriptor &nested : linkedDocument.m_smartSources.descriptors()) {
        SmartSourceDescriptor imported = nested;
        imported.id = sourceIdMap.value(nested.id, nested.id);
        for (QUuid &dependency : imported.dependencies) {
            dependency = sourceIdMap.value(dependency, dependency);
        }
        if (imported.storage == SmartSourceStorage::Linked) {
            const QString nestedAbsolute = resolvedLinkedPath(
                nested.linkedPath, linkedDocument.m_projectPath);
            imported.linkedPath = persistedLinkedPath(nestedAbsolute, m_projectPath);
        }
        if (nested.hasEmbeddedDocument()) {
            QVector<LayerNode> nestedLayers;
            QSize nestedSize;
            DocumentColourState nestedColour;
            double nestedResX = 72.0;
            double nestedResY = 72.0;
            DocumentColourModel nestedModel = DocumentColourModel::Rgb;
            int nestedDepth = 8;
            if (!decodeEmbeddedSmartDocument(nested, linkedDocument.m_smartSources,
                                             &nestedLayers, &nestedSize, &nestedColour,
                                             &nestedResX, &nestedResY, &nestedModel,
                                             &nestedDepth, errorMessage)) {
                return false;
            }
            remapSmartSourceReferencesRecursive(nestedLayers, sourceIdMap);
            bool encodedOk = false;
            imported.embeddedDocument = encodeEmbeddedSmartDocument(
                nestedLayers, nestedSize, nestedResX, nestedResY, nestedModel,
                nestedDepth, nestedColour, &encodedOk);
            if (!encodedOk) return false;
        }
        pending.push_back(std::move(imported));
    }

    while (!pending.isEmpty()) {
        bool progressed = false;
        for (qsizetype index = pending.size(); index-- > 0;) {
            const SmartSourceDescriptor &candidate = pending.at(index);
            bool ready = true;
            for (const QUuid &dependency : candidate.dependencies) {
                if (!preparedSources.contains(dependency)) { ready = false; break; }
            }
            if (!ready) continue;
            if (!preparedSources.insert(candidate, errorMessage)) return false;
            pending.removeAt(index);
            progressed = true;
        }
        if (!progressed) {
            setError(errorMessage, QStringLiteral(
                "The linked document contains a Smart Source graph that cannot be embedded without a cycle."));
            return false;
        }
    }

    QVector<LayerNode> rootLayers = linkedDocument.m_layers;
    materialiseEmbeddedBasePixelsRecursive(rootLayers, linkedDocument.m_sourceImage,
                                           linkedDocument.m_sourceImage.size());
    remapSmartSourceReferencesRecursive(rootLayers, sourceIdMap);
    if (!bindSmartPresentationsRecursive(rootLayers, preparedSources,
                                         linkedDocument.m_sourceImage.colorSpace(), errorMessage)) {
        return false;
    }
    QSet<QUuid> rootDependencySet;
    collectSmartSourceDependenciesRecursive(rootLayers, &rootDependencySet);
    QVector<QUuid> rootDependencies = rootDependencySet.values();
    std::sort(rootDependencies.begin(), rootDependencies.end(), [](const QUuid &a, const QUuid &b) {
        return a.toString(QUuid::WithoutBraces) < b.toString(QUuid::WithoutBraces);
    });

    QHash<QUuid, quint64> changed;
    if (!preparedSources.bumpRevisionCascade(existing->id, &changed)) return false;
    SmartSourceDescriptor embedded = *preparedSources.find(existing->id);
    embedded.storage = SmartSourceStorage::Embedded;
    embedded.linkedPath.clear();
    embedded.linkedDocumentId = {};
    embedded.linkedContentFingerprint.clear();
    embedded.linkedAvailable = false;
    embedded.linkedRuntimeWarning.clear();
    embedded.linkedResolvedDocumentIds.clear();
    embedded.dependencies = rootDependencies;
    bool payloadOk = false;
    embedded.embeddedDocument = encodeEmbeddedSmartDocument(
        rootLayers, linkedDocument.m_sourceImage.size(), linkedDocument.m_resolutionX,
        linkedDocument.m_resolutionY, linkedDocument.m_colourModel,
        linkedDocument.m_sourceImage.depth() > 32 ? 16 : 8,
        linkedDocument.m_colourState, &payloadOk);
    if (!payloadOk) {
        setError(errorMessage, QStringLiteral("The linked source could not be embedded safely."));
        return false;
    }
    embedded.presentationImage = target.presentation;
    embedded.presentationBounds = target.presentationBounds;
    embedded.presentationRevision = embedded.revision;
    if (!preparedSources.replace(embedded, errorMessage)) return false;

    QSet<QUuid> affected;
    for (auto it = changed.cbegin(); it != changed.cend(); ++it) {
        // Keep the newly embedded wrapper's presentation byte-for-byte aligned
        // with the linked document's visible canvas at the moment of Embed. Its
        // authoritative embedded contents are already stored above; only outer
        // embedded dependants need rebuilding against this new source revision.
        if (it.key() != existing->id) affected.insert(it.key());
    }
    if (!refreshAffectedEmbeddedSmartPresentations(&preparedSources, affected, errorMessage)) return false;
    QVector<LayerNode> preparedLayers = m_layers;
    for (auto it = changed.cbegin(); it != changed.cend(); ++it) {
        synchronizeSmartSourceRevisionRecursive(preparedLayers, it.key(), it.value());
    }
    if (!bindSmartPresentationsRecursive(preparedLayers, preparedSources,
                                         m_sourceImage.colorSpace(), errorMessage)) return false;
    m_smartSources = std::move(preparedSources);
    m_layers = std::move(preparedLayers);
    m_linkedSourceWarnings = linkedWarningsForRegistry(m_smartSources, m_projectPath);
    m_modified = true;
    return true;
}

bool PhotoDocument::createEditableSmartSourceDocument(
    const QUuid &sourceId,
    PhotoDocument *editableDocument,
    QHash<QUuid, quint64> *baselineSourceRevisions,
    QString *errorMessage) const
{
    if (!editableDocument || sourceId.isNull()) {
        setError(errorMessage, QStringLiteral(
            "A valid embedded Smart Source is required for Edit Contents."));
        return false;
    }
    const SmartSourceDescriptor *source = m_smartSources.find(sourceId);
    if (!source || source->storage != SmartSourceStorage::Embedded
        || !source->hasEmbeddedDocument()) {
        setError(errorMessage, QStringLiteral(
            "This Smart Layer does not contain an editable embedded source document."));
        return false;
    }

    QVector<LayerNode> layers;
    QSize canvasSize;
    DocumentColourState colourState;
    double resolutionX = 72.0;
    double resolutionY = 72.0;
    DocumentColourModel colourModel = DocumentColourModel::Rgb;
    int bitDepth = 8;
    if (!decodeEmbeddedSmartDocument(*source, m_smartSources, &layers, &canvasSize,
                                     &colourState, &resolutionX, &resolutionY,
                                     &colourModel, &bitDepth, errorMessage)) {
        return false;
    }

    QImage transparentSource(canvasSize,
                             bitDepth > 8 ? QImage::Format_RGBA64
                                          : QImage::Format_RGBA8888);
    if (transparentSource.isNull()) {
        setError(errorMessage, QStringLiteral(
            "Could not allocate the Smart Layer source document canvas."));
        return false;
    }
    transparentSource.fill(Qt::transparent);
    transparentSource.setColorSpace(documentWorkingQtSpace(colourState));
    transparentSource.setDotsPerMeterX(dotsPerMetreFromDpi(resolutionX));
    transparentSource.setDotsPerMeterY(dotsPerMetreFromDpi(resolutionY));

    PhotoDocument prepared;
    prepared.m_sourceImage = std::move(transparentSource);
    prepared.m_sourcePath.clear();
    prepared.m_projectPath.clear();
    prepared.m_documentName = source->name.trimmed().isEmpty()
        ? QStringLiteral("Smart Layer Contents")
        : source->name + QStringLiteral(" — Smart Contents");
    prepared.m_colourModel = colourModel;
    prepared.m_blankDocument = false;
    prepared.m_resolutionX = resolutionX;
    prepared.m_resolutionY = resolutionY;
    prepared.m_colourState = colourState;
    prepared.m_layers = std::move(layers);
    prepared.m_smartSources = m_smartSources;
    // Embedded contents editors have no public project path of their own. Make
    // linked descriptors absolute while editing so nested linked Smart Layers
    // keep resolving against the owning project rather than the process CWD.
    for (const SmartSourceDescriptor &descriptor : prepared.m_smartSources.descriptors()) {
        if (descriptor.storage != SmartSourceStorage::Linked) continue;
        SmartSourceDescriptor rebased = descriptor;
        rebased.linkedPath = resolvedLinkedPath(descriptor.linkedPath, m_projectPath);
        QString rebaseError;
        if (!prepared.m_smartSources.replace(rebased, &rebaseError)) {
            setError(errorMessage, rebaseError);
            return false;
        }
    }
    prepared.m_selectionMask.reset(canvasSize);
    prepared.m_horizontalGuides.clear();
    prepared.m_verticalGuides.clear();
    prepared.m_loadWarnings.clear();
    prepared.m_colourResourceWarnings =
        auditDocumentColourResources(prepared.m_colourState).messages();
    if (!prepared.synchronizeSmartLayerPresentations(errorMessage)) {
        return false;
    }
    prepared.rebuildPreviewSource();
    prepared.m_modified = false;

    if (baselineSourceRevisions) {
        baselineSourceRevisions->clear();
        for (const SmartSourceDescriptor &descriptor : m_smartSources.descriptors()) {
            baselineSourceRevisions->insert(descriptor.id, descriptor.revision);
        }
    }
    *editableDocument = std::move(prepared);
    return true;
}

bool PhotoDocument::commitEditableSmartSourceDocument(
    const QUuid &sourceId,
    const PhotoDocument &editedDocument,
    const QHash<QUuid, quint64> &baselineSourceRevisions,
    QHash<QUuid, quint64> *changedRevisions,
    QString *errorMessage)
{
    if (sourceId.isNull() || !editedDocument.hasImage()) {
        setError(errorMessage, QStringLiteral(
            "The Smart Layer source document is not available to save."));
        return false;
    }
    const SmartSourceDescriptor *ownerSource = m_smartSources.find(sourceId);
    if (!ownerSource || ownerSource->storage != SmartSourceStorage::Embedded
        || !ownerSource->hasEmbeddedDocument()) {
        setError(errorMessage, QStringLiteral(
            "The owning embedded Smart Source no longer exists."));
        return false;
    }
    const auto baselineTarget = baselineSourceRevisions.constFind(sourceId);
    if (baselineTarget == baselineSourceRevisions.cend()
        || ownerSource->revision != baselineTarget.value()) {
        setError(errorMessage, QStringLiteral(
            "The Smart Source changed in its owner while this contents document was open. Close and reopen Edit Contents before saving."));
        return false;
    }
    if (!layerMetadataIsSafeRecursive(editedDocument.m_layers)) {
        setError(errorMessage, QStringLiteral(
            "The edited Smart Source layer tree exceeds safety limits."));
        return false;
    }
    QSet<QUuid> editorLayerIds;
    if (!idsAreUniqueRecursive(editedDocument.m_layers, editorLayerIds)
        || !editedDocument.m_smartSources.validate()
        || !smartLayerReferencesResolveRecursive(editedDocument.m_layers,
                                                 editedDocument.m_smartSources)) {
        setError(errorMessage, QStringLiteral(
            "The edited Smart Source contains an inconsistent layer or source graph."));
        return false;
    }

    SmartSourceRegistry preparedSources = m_smartSources;
    QSet<QUuid> affected;

    for (const SmartSourceDescriptor &editorSource
         : editedDocument.m_smartSources.descriptors()) {
        if (editorSource.id == sourceId) continue;
        SmartSourceDescriptor editedSource = editorSource;
        if (editedSource.storage == SmartSourceStorage::Linked) {
            const QString absolute = resolvedLinkedPath(editedSource.linkedPath,
                                                        editedDocument.m_projectPath);
            editedSource.linkedPath = persistedLinkedPath(absolute, m_projectPath);
        }
        const auto baseline = baselineSourceRevisions.constFind(editedSource.id);
        if (baseline == baselineSourceRevisions.cend()) {
            if (preparedSources.contains(editedSource.id)) {
                setError(errorMessage, QStringLiteral(
                    "A newly created nested Smart Source conflicts with an existing source identity."));
                return false;
            }
            QString nestedError;
            if (!preparedSources.insert(editedSource, &nestedError)) {
                setError(errorMessage, nestedError);
                return false;
            }
            affected.insert(editedSource.id);
            continue;
        }
        if (editedSource.revision == baseline.value()) continue;

        const SmartSourceDescriptor *currentOwner = preparedSources.find(editedSource.id);
        if (!currentOwner || currentOwner->revision != baseline.value()) {
            setError(errorMessage, QStringLiteral(
                "A nested Smart Source changed elsewhere while this contents document was open."));
            return false;
        }
        QString nestedError;
        if (!preparedSources.adoptEditedDescriptor(
                editedSource, baseline.value(), &nestedError)) {
            setError(errorMessage, nestedError);
            return false;
        }
        QHash<QUuid, quint64> nestedChanged;
        if (!preparedSources.bumpRevisionCascade(editedSource.id, &nestedChanged)) {
            setError(errorMessage, QStringLiteral(
                "The nested Smart Source revision graph could not be advanced safely."));
            return false;
        }
        QSet<QUuid> nestedAffected;
        for (auto it = nestedChanged.cbegin(); it != nestedChanged.cend(); ++it) {
            nestedAffected.insert(it.key());
        }
        if (!refreshAffectedEmbeddedSmartPresentations(
                &preparedSources, nestedAffected, errorMessage)) {
            return false;
        }
        affected.unite(nestedAffected);
    }

    QVector<LayerNode> sourceLayers = editedDocument.m_layers;
    materialiseEmbeddedBasePixelsRecursive(sourceLayers,
                                           editedDocument.m_sourceImage,
                                           editedDocument.m_sourceImage.size());
    if (!bindSmartPresentationsRecursive(sourceLayers, preparedSources,
                                         editedDocument.m_sourceImage.colorSpace(),
                                         errorMessage)) {
        return false;
    }

    QSet<QUuid> dependencySet;
    collectSmartSourceDependenciesRecursive(sourceLayers, &dependencySet);
    QVector<QUuid> dependencies;
    dependencies.reserve(dependencySet.size());
    for (const QUuid &dependency : dependencySet) dependencies.push_back(dependency);
    std::sort(dependencies.begin(), dependencies.end(), [](const QUuid &a, const QUuid &b) {
        return a.toString(QUuid::WithoutBraces) < b.toString(QUuid::WithoutBraces);
    });

    const SmartSourceDescriptor *currentTarget = preparedSources.find(sourceId);
    if (!currentTarget) {
        setError(errorMessage, QStringLiteral("The owning Smart Source disappeared during save."));
        return false;
    }
    SmartSourceDescriptor replacement = *currentTarget;
    replacement.storage = SmartSourceStorage::Embedded;
    replacement.linkedPath.clear();
    replacement.dependencies = dependencies;
    bool payloadOk = false;
    replacement.embeddedDocument = encodeEmbeddedSmartDocument(
        sourceLayers,
        editedDocument.m_sourceImage.size(),
        editedDocument.m_resolutionX,
        editedDocument.m_resolutionY,
        editedDocument.m_colourModel,
        editedDocument.m_sourceImage.depth() > 32 ? 16 : 8,
        editedDocument.m_colourState,
        &payloadOk);
    replacement.presentationImage = {};
    replacement.presentationBounds = {};
    replacement.presentationRevision = 0;
    if (!payloadOk || replacement.embeddedDocument.isEmpty()) {
        setError(errorMessage, QStringLiteral(
            "Could not encode the edited Smart Source document."));
        return false;
    }

    QString sourceError;
    if (!preparedSources.replace(replacement, &sourceError)) {
        setError(errorMessage, sourceError);
        return false;
    }
    QHash<QUuid, quint64> targetChanged;
    if (!preparedSources.bumpRevisionCascade(sourceId, &targetChanged)) {
        setError(errorMessage, QStringLiteral(
            "The Smart Source dependency graph could not advance revisions safely."));
        return false;
    }
    QSet<QUuid> targetAffected;
    for (auto it = targetChanged.cbegin(); it != targetChanged.cend(); ++it) {
        targetAffected.insert(it.key());
    }
    if (!refreshAffectedEmbeddedSmartPresentations(
            &preparedSources, targetAffected, errorMessage)) {
        return false;
    }
    affected.unite(targetAffected);
    if (!preparedSources.validate(&sourceError)
        || !smartLayerReferencesResolveRecursive(m_layers, preparedSources)
        || !validateEmbeddedSmartSources(preparedSources, &sourceError)) {
        setError(errorMessage, sourceError.isEmpty()
            ? QStringLiteral("The edited Smart Source could not be validated after saving.")
            : sourceError);
        return false;
    }

    QVector<LayerNode> preparedOwnerLayers = m_layers;
    QHash<QUuid, quint64> committed;
    for (const QUuid &id : affected) {
        const SmartSourceDescriptor *descriptor = preparedSources.find(id);
        if (!descriptor) continue;
        committed.insert(id, descriptor->revision);
        synchronizeSmartSourceRevisionRecursive(
            preparedOwnerLayers, id, descriptor->revision);
    }
    if (!bindSmartPresentationsRecursive(preparedOwnerLayers, preparedSources,
                                         m_sourceImage.colorSpace(), errorMessage)) {
        return false;
    }

    m_smartSources = std::move(preparedSources);
    m_layers = std::move(preparedOwnerLayers);
    // Runtime Smart caches are content/revision addressed. Do not flush this
    // source branch merely because its authoritative revision advanced: exact
    // unchanged source footprints can remain reusable across Edit Contents
    // commits, while changed footprints naturally receive different keys.
    m_modified = true;
    if (changedRevisions) *changedRevisions = std::move(committed);
    return true;
}

bool PhotoDocument::adoptSmartSourceRegistrySnapshot(
    const SmartSourceRegistry &registry,
    QString *errorMessage,
    const QString &registryOwnerProjectPath)
{
    QString sourceError;
    if (!registry.validate(&sourceError)
        || !smartLayerReferencesResolveRecursive(m_layers, registry)
        || !validateEmbeddedSmartSources(registry, &sourceError)) {
        setError(errorMessage, sourceError.isEmpty()
            ? QStringLiteral("The Smart Source registry snapshot is invalid.")
            : sourceError);
        return false;
    }
    SmartSourceRegistry prepared = registry;
    // A Smart-contents editor intentionally has no project path. When its owner
    // rebases the shared registry after Save, translate owner-relative links
    // back into this document's path basis (absolute for the editor) so nested
    // linked Smart Layers keep resolving correctly across repeated saves.
    if (!registryOwnerProjectPath.trimmed().isEmpty()) {
        for (const SmartSourceDescriptor &descriptor : prepared.descriptors()) {
            if (descriptor.storage != SmartSourceStorage::Linked) continue;
            SmartSourceDescriptor rebased = descriptor;
            const QString absolute = resolvedLinkedPath(descriptor.linkedPath,
                                                        registryOwnerProjectPath);
            rebased.linkedPath = persistedLinkedPath(absolute, m_projectPath);
            if (!prepared.replace(rebased, &sourceError)) {
                setError(errorMessage, sourceError);
                return false;
            }
        }
    }
    QVector<LayerNode> preparedLayers = m_layers;
    for (const SmartSourceDescriptor &descriptor : prepared.descriptors()) {
        synchronizeSmartSourceRevisionRecursive(preparedLayers,
                                                descriptor.id,
                                                descriptor.revision);
    }
    if (!bindSmartPresentationsRecursive(preparedLayers, prepared,
                                         m_sourceImage.colorSpace(), errorMessage)) {
        return false;
    }
    QSet<QUuid> removedSources;
    for (const SmartSourceDescriptor &descriptor : m_smartSources.descriptors()) {
        if (!prepared.contains(descriptor.id)) {
            removedSources.insert(descriptor.id);
        }
    }
    m_smartSources = std::move(prepared);
    m_layers = std::move(preparedLayers);
    m_linkedSourceWarnings = linkedWarningsForRegistry(m_smartSources, m_projectPath);
    // Changed/reinstated sources are safely version/content addressed. Purge
    // only identities that no longer exist so Undo/Redo can still reuse exact
    // unchanged intermediates without retaining deleted-source residency.
    SmartLayerTileCache::instance().invalidateSources(removedSources);
    return true;
}

bool PhotoDocument::canConvertLayersToSmart(const QVector<QUuid> &ids,
                                             QString *errorMessage) const
{
    if (!hasImage()) {
        setError(errorMessage, QStringLiteral("There is no open document to convert."));
        return false;
    }
    QSet<QUuid> wanted;
    for (const QUuid &id : ids) {
        if (!id.isNull() && containsLayer(id)) wanted.insert(id);
    }
    if (wanted.isEmpty()) {
        setError(errorMessage, QStringLiteral("Select at least one layer to convert."));
        return false;
    }

    QVector<QUuid> ordered;
    collectSelectedRootOrder(m_layers, wanted, false, &ordered);
    if (ordered.isEmpty()) {
        setError(errorMessage, QStringLiteral("The selected layers could not be resolved."));
        return false;
    }

    QUuid commonParent;
    int firstIndex = -1;
    QVector<int> indices;
    indices.reserve(ordered.size());
    for (int i = 0; i < ordered.size(); ++i) {
        QUuid parentId;
        int index = -1;
        if (!layerPlacement(ordered.at(i), &parentId, &index)) {
            setError(errorMessage, QStringLiteral("The selected layer placement is invalid."));
            return false;
        }
        if (i == 0) {
            commonParent = parentId;
            firstIndex = index;
        } else if (parentId != commonParent) {
            setError(errorMessage, QStringLiteral(
                "Convert to Smart Layer requires selected root layers to share the same parent."));
            return false;
        }
        indices.push_back(index);
    }
    std::sort(indices.begin(), indices.end());
    for (int i = 1; i < indices.size(); ++i) {
        if (indices.at(i) != indices.at(i - 1) + 1) {
            setError(errorMessage, QStringLiteral(
                "Convert to Smart Layer requires a contiguous layer selection so unselected siblings are not reordered."));
            return false;
        }
    }

    const QVector<LayerNode> *siblings = childrenForParent(m_layers, commonParent);
    if (!siblings || firstIndex < 0) {
        setError(errorMessage, QStringLiteral("The selected layer parent is invalid."));
        return false;
    }

    bool requiresBackdrop = false;
    QVector<LayerNode> roots;
    roots.reserve(ordered.size());
    for (const QUuid &id : ordered) {
        const LayerNode *layer = findLayerRecursive(m_layers, id);
        if (!layer) return false;
        roots.push_back(*layer);
        requiresBackdrop = requiresBackdrop || rootRequiresBackdrop(*layer);
    }
    if (!nestedSmartPresentationsAreCurrent(roots, m_smartSources, errorMessage)) {
        return false;
    }

    if (requiresBackdrop) {
        if (indices.constLast() != siblings->size() - 1) {
            setError(errorMessage, QStringLiteral(
                "This selection contains backdrop-dependent blending, an Adjustment Layer, or a Pass Through group. Include every sibling below it so conversion can preserve the exact appearance."));
            return false;
        }
        if (!commonParent.isNull()) {
            const LayerNode *parent = findLayerRecursive(m_layers, commonParent);
            if (!parent || parent->type != LayerType::Group
                || parent->groupCompositeMode != GroupCompositeMode::Isolated) {
                setError(errorMessage, QStringLiteral(
                    "Backdrop-dependent layers inside a Pass Through context cannot yet be converted without changing appearance."));
                return false;
            }
        }
    }
    return true;
}

QUuid PhotoDocument::convertLayersToEmbeddedSmart(const QVector<QUuid> &ids,
                                                   QString *errorMessage)
{
    if (!canConvertLayersToSmart(ids, errorMessage)) return {};

    QSet<QUuid> wanted;
    for (const QUuid &id : ids) {
        if (!id.isNull() && containsLayer(id)) wanted.insert(id);
    }
    QVector<QUuid> ordered;
    collectSelectedRootOrder(m_layers, wanted, false, &ordered);
    if (ordered.isEmpty()) return {};

    QUuid parentId;
    int insertionIndex = -1;
    if (!layerPlacement(ordered.constFirst(), &parentId, &insertionIndex)) {
        setError(errorMessage, QStringLiteral("The selected layer placement is invalid."));
        return {};
    }

    QVector<LayerNode> preparedLayers = m_layers;
    SmartSourceRegistry preparedSources = m_smartSources;
    QVector<LayerNode> embeddedLayers;
    embeddedLayers.reserve(ordered.size());
    for (const QUuid &id : ordered) {
        LayerNode removed;
        if (!removeLayerById(preparedLayers, id, &removed)) {
            setError(errorMessage, QStringLiteral("A selected layer could not be detached safely."));
            return {};
        }
        embeddedLayers.push_back(std::move(removed));
    }
    materialiseEmbeddedBasePixelsRecursive(embeddedLayers, m_sourceImage, m_sourceImage.size());
    if (!bindSmartPresentationsRecursive(embeddedLayers, preparedSources,
                                         m_sourceImage.colorSpace(), errorMessage)) {
        return {};
    }

    QSet<QUuid> dependencySet;
    collectSmartSourceDependenciesRecursive(embeddedLayers, &dependencySet);
    QVector<QUuid> dependencies;
    dependencies.reserve(dependencySet.size());
    for (const QUuid &dependency : dependencySet) dependencies.push_back(dependency);
    std::sort(dependencies.begin(), dependencies.end(), [](const QUuid &a, const QUuid &b) {
        return a.toString(QUuid::WithoutBraces) < b.toString(QUuid::WithoutBraces);
    });

    SmartSourceDescriptor source;
    source.storage = SmartSourceStorage::Embedded;
    source.name = ordered.size() == 1
        ? (embeddedLayers.constFirst().name.trimmed().isEmpty()
               ? QStringLiteral("Smart Layer")
               : embeddedLayers.constFirst().name.trimmed())
        : QStringLiteral("Smart Layer (%1 layers)").arg(ordered.size());
    source.dependencies = dependencies;

    bool payloadOk = false;
    source.embeddedDocument = encodeEmbeddedSmartDocument(
        embeddedLayers, m_sourceImage.size(), m_resolutionX, m_resolutionY,
        m_colourModel, m_sourceImage.depth() > 32 ? 16 : 8,
        m_colourState, &payloadOk);
    if (!payloadOk || source.embeddedDocument.isEmpty()) {
        setError(errorMessage, QStringLiteral("Could not encode the embedded Smart Layer contents."));
        return {};
    }

    QVector<QUuid> embeddedRootIds;
    embeddedRootIds.reserve(embeddedLayers.size());
    for (const LayerNode &layer : embeddedLayers) embeddedRootIds.push_back(layer.id);
    QRectF boundsF = ImageProcessor::contentBounds(
        m_sourceImage, embeddedLayers, embeddedRootIds, m_sourceImage.size());
    const QRectF storageBounds = rasterStorageBoundsRecursive(
        m_sourceImage, embeddedLayers, m_sourceImage.size());
    if (!storageBounds.isEmpty()) {
        boundsF = boundsF.isEmpty() ? storageBounds : boundsF.united(storageBounds);
    }
    const QSize radius = maximumSpatialAdjustmentRadius2D(embeddedLayers);
    if (!boundsF.isEmpty()) {
        const int safety = SpatialFilterContract::DefaultSafetyPadding;
        boundsF.adjust(-(radius.width() + safety), -(radius.height() + safety),
                       radius.width() + safety, radius.height() + safety);
    }

    QRect renderBounds;
    if (boundsF.isEmpty()) {
        renderBounds = QRect(0, 0, 1, 1);
    } else {
        renderBounds = boundsF.toAlignedRect();
        const qint64 presentationPixels = qint64(renderBounds.width()) * renderBounds.height();
        const qint64 presentationBytes = presentationPixels
            * (m_sourceImage.depth() > 32 ? qint64(8) : qint64(4));
        if (renderBounds.width() < 1 || renderBounds.height() < 1
            || renderBounds.width() > 32768 || renderBounds.height() > 32768
            || presentationPixels > qint64(64) * 1024 * 1024
            || presentationBytes > qint64(256) * 1024 * 1024) {
            setError(errorMessage, QStringLiteral(
                "The selected Smart Layer presentation is too large to materialise safely."));
            return {};
        }
    }

    QImage presentation = ImageProcessor::renderUnclippedRegionPreservingHiddenRgb(
        m_sourceImage, embeddedLayers, renderBounds, m_sourceImage.size(), nullptr,
        m_colourState.processingCompatibility);
    if (presentation.isNull()) {
        setError(errorMessage, QStringLiteral("Could not render the embedded Smart Layer presentation."));
        return {};
    }
    presentation = straightRgbaImage(presentation);
    presentation.setColorSpace(m_sourceImage.colorSpace());

    const QRect meaningful = nonZeroRgbaBounds(presentation);
    if (!meaningful.isEmpty()) {
        presentation = presentation.copy(meaningful);
        renderBounds = QRect(renderBounds.topLeft() + meaningful.topLeft(), meaningful.size());
    } else {
        presentation = QImage(QSize(1, 1), m_sourceImage.depth() > 32
            ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        if (presentation.isNull()) {
            setError(errorMessage, QStringLiteral("Could not allocate an empty Smart Layer presentation."));
            return {};
        }
        presentation.fill(Qt::transparent);
        presentation.setColorSpace(m_sourceImage.colorSpace());
        renderBounds = QRect(0, 0, 1, 1);
    }
    source.presentationImage = std::move(presentation);
    source.presentationBounds = renderBounds;
    source.presentationRevision = source.revision;

    QString sourceError;
    if (!preparedSources.insert(source, &sourceError)) {
        setError(errorMessage, sourceError.isEmpty()
            ? QStringLiteral("Could not register the embedded Smart Source.")
            : sourceError);
        return {};
    }

    LayerNode smart;
    smart.type = LayerType::Smart;
    smart.name = source.name;
    smart.smartSource.sourceId = source.id;
    smart.smartSource.observedSourceRevision = source.revision;
    smart.smartPresentationImage = source.presentationImage;
    smart.smartPresentationReferenceSize = source.presentationBounds.size();
    smart.smartPresentationReferenceOrigin = source.presentationBounds.topLeft();
    const QUuid smartLayerId = smart.id;

    QVector<LayerNode> *destination = childrenForParent(preparedLayers, parentId);
    if (!destination) {
        setError(errorMessage, QStringLiteral("The destination layer parent is no longer available."));
        return {};
    }
    destination->insert(std::clamp(insertionIndex, 0, static_cast<int>(destination->size())),
                        std::move(smart));

    if (!layerMetadataIsSafeRecursive(preparedLayers)) {
        setError(errorMessage, QStringLiteral("The converted Smart Layer tree exceeds safety limits."));
        return {};
    }
    QSet<QUuid> allIds;
    if (!idsAreUniqueRecursive(preparedLayers, allIds)
        || !smartLayerReferencesResolveRecursive(preparedLayers, preparedSources)) {
        setError(errorMessage, QStringLiteral("The converted Smart Layer structure is inconsistent."));
        return {};
    }
    if (!validateEmbeddedSmartSources(preparedSources, &sourceError)) {
        setError(errorMessage, sourceError);
        return {};
    }

    if (!bindSmartPresentationsRecursive(preparedLayers, preparedSources,
                                         m_sourceImage.colorSpace(), errorMessage)) {
        return {};
    }
    m_layers = std::move(preparedLayers);
    m_smartSources = std::move(preparedSources);
    m_modified = true;
    return smartLayerId;
}

bool PhotoDocument::registerSmartSource(const SmartSourceDescriptor &descriptor,
                                        QString *errorMessage)
{
    SmartSourceRegistry prepared = m_smartSources;
    if (!prepared.insert(descriptor, errorMessage)) return false;
    QString embeddedError;
    if (!validateEmbeddedSmartSources(prepared, &embeddedError)) {
        setError(errorMessage, embeddedError);
        return false;
    }
    QVector<LayerNode> preparedLayers = m_layers;
    if (!bindSmartPresentationsRecursive(preparedLayers, prepared,
                                         m_sourceImage.colorSpace(), errorMessage)) {
        return false;
    }
    m_smartSources = std::move(prepared);
    m_layers = std::move(preparedLayers);
    m_modified = true;
    return true;
}

bool PhotoDocument::replaceSmartSource(const SmartSourceDescriptor &descriptor,
                                       QString *errorMessage)
{
    const SmartSourceDescriptor *existing = m_smartSources.find(descriptor.id);
    if (!existing) {
        setError(errorMessage, QStringLiteral("The Smart Source identity does not exist."));
        return false;
    }
    const bool presentationChanged = existing->storage != descriptor.storage
        || existing->linkedPath != descriptor.linkedPath
        || existing->dependencies != descriptor.dependencies
        || existing->embeddedDocument != descriptor.embeddedDocument;
    SmartSourceRegistry prepared = m_smartSources;
    if (!prepared.replace(descriptor, errorMessage)) return false;
    QHash<QUuid, quint64> changedRevisions;
    if (presentationChanged
        && !prepared.bumpRevisionCascade(descriptor.id, &changedRevisions)) {
        setError(errorMessage, QStringLiteral(
            "The Smart Source revision graph cannot be advanced safely."));
        return false;
    }
    if (!smartLayerReferencesResolveRecursive(m_layers, prepared)) {
        setError(errorMessage, QStringLiteral(
            "The Smart Source update would invalidate an existing Smart Layer reference."));
        return false;
    }
    QVector<LayerNode> preparedLayers = m_layers;
    for (auto iterator = changedRevisions.cbegin();
         iterator != changedRevisions.cend(); ++iterator) {
        synchronizeSmartSourceRevisionRecursive(
            preparedLayers, iterator.key(), iterator.value());
    }
    if (!bindSmartPresentationsRecursive(preparedLayers, prepared,
                                         m_sourceImage.colorSpace(), errorMessage)) {
        return false;
    }
    m_smartSources = std::move(prepared);
    m_layers = std::move(preparedLayers);
    m_modified = true;
    return true;
}

bool PhotoDocument::removeSmartSource(const QUuid &id, QString *errorMessage)
{
    if (!m_smartSources.contains(id)) return false;
    SmartSourceRegistry prepared = m_smartSources;
    if (!prepared.remove(id)) {
        setError(errorMessage, QStringLiteral(
            "The Smart Source is still required by another Smart Source."));
        return false;
    }
    QString graphError;
    if (!prepared.validate(&graphError)
        || !smartLayerReferencesResolveRecursive(m_layers, prepared)) {
        setError(errorMessage, graphError.isEmpty()
            ? QStringLiteral("The Smart Source is still referenced by a Smart Layer.")
            : graphError);
        return false;
    }
    QVector<LayerNode> preparedLayers = m_layers;
    if (!bindSmartPresentationsRecursive(preparedLayers, prepared,
                                         m_sourceImage.colorSpace(), errorMessage)) {
        return false;
    }
    m_smartSources = std::move(prepared);
    m_layers = std::move(preparedLayers);
    SmartLayerTileCache::instance().invalidateSource(id);
    m_modified = true;
    return true;
}

bool PhotoDocument::bumpSmartSourceRevision(const QUuid &id, quint64 *newRevision)
{
    SmartSourceRegistry preparedSources = m_smartSources;
    QHash<QUuid, quint64> changedRevisions;
    if (!preparedSources.bumpRevisionCascade(id, &changedRevisions)) return false;
    QVector<LayerNode> preparedLayers = m_layers;
    for (auto iterator = changedRevisions.cbegin();
         iterator != changedRevisions.cend(); ++iterator) {
        synchronizeSmartSourceRevisionRecursive(
            preparedLayers, iterator.key(), iterator.value());
    }
    QString bindError;
    if (!bindSmartPresentationsRecursive(preparedLayers, preparedSources,
                                         m_sourceImage.colorSpace(), &bindError)) {
        return false;
    }
    m_smartSources = std::move(preparedSources);
    m_layers = std::move(preparedLayers);
    if (newRevision) *newRevision = changedRevisions.value(id, 0);
    m_modified = true;
    return true;
}

QUuid PhotoDocument::addLiveFilter(const QUuid &smartLayerId,
                                     const AdjustmentType type,
                                     QString *errorMessage)
{
    LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart) {
        setError(errorMessage, QStringLiteral("Live Filters can only be added to a Smart Layer."));
        return {};
    }
    if (!adjustmentTypeSupportsLiveFilter(type)) {
        setError(errorMessage, QStringLiteral("This operator cannot be used as a Live Filter."));
        return {};
    }
    if (layer->liveFilters.size() >= LayerNode::MaximumLiveFilterCount) {
        setError(errorMessage, QStringLiteral("The Smart Layer has reached the Live Filter limit."));
        return {};
    }
    LiveFilter filter;
    filter.adjustment.reset(type);
    filter.normalise();
    const QUuid id = filter.id;
    layer->liveFilters.push_back(std::move(filter));
    ++layer->revision;
    m_modified = true;
    return id;
}

bool PhotoDocument::removeLiveFilter(const QUuid &smartLayerId, const QUuid &filterId)
{
    LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart || filterId.isNull()) return false;
    for (qsizetype index = 0; index < layer->liveFilters.size(); ++index) {
        if (layer->liveFilters.at(index).id != filterId) continue;
        layer->liveFilters.removeAt(index);
        ++layer->revision;
        m_modified = true;
        return true;
    }
    return false;
}

bool PhotoDocument::setLiveFilterEnabled(const QUuid &smartLayerId,
                                         const QUuid &filterId,
                                         const bool enabled)
{
    LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart || filterId.isNull()) return false;
    for (LiveFilter &filter : layer->liveFilters) {
        if (filter.id != filterId) continue;
        if (filter.enabled == enabled) return true;
        filter.enabled = enabled;
        ++filter.revision;
        ++layer->revision;
        m_modified = true;
        return true;
    }
    return false;
}

bool PhotoDocument::updateLiveFilter(const QUuid &smartLayerId,
                                     const QUuid &filterId,
                                     const AdjustmentData &adjustment)
{
    if (!adjustmentTypeSupportsLiveFilter(adjustment.type)) return false;
    LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart || filterId.isNull()) return false;
    for (LiveFilter &filter : layer->liveFilters) {
        if (filter.id != filterId) continue;
        AdjustmentData safe = adjustment;
        safe.normalise();
        if (filter.adjustment == safe) return true;
        filter.adjustment = std::move(safe);
        ++filter.revision;
        ++layer->revision;
        m_modified = true;
        return true;
    }
    return false;
}

bool PhotoDocument::moveLiveFilter(const QUuid &smartLayerId,
                                   const QUuid &filterId,
                                   int destinationIndex)
{
    LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart || filterId.isNull()
        || layer->liveFilters.isEmpty()) return false;
    qsizetype sourceIndex = -1;
    for (qsizetype index = 0; index < layer->liveFilters.size(); ++index) {
        if (layer->liveFilters.at(index).id == filterId) {
            sourceIndex = index;
            break;
        }
    }
    if (sourceIndex < 0) return false;
    const int maximumIndex = static_cast<int>(layer->liveFilters.size()) - 1;
    destinationIndex = std::clamp(destinationIndex, 0, maximumIndex);
    if (sourceIndex == destinationIndex) return true;
    LiveFilter filter = layer->liveFilters.takeAt(sourceIndex);
    layer->liveFilters.insert(destinationIndex, std::move(filter));
    ++layer->revision;
    m_modified = true;
    return true;
}


bool PhotoDocument::addLiveFilterMask(const QUuid &smartLayerId, const QUuid &filterId)
{
    LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart || filterId.isNull()) return false;
    for (LiveFilter &filter : layer->liveFilters) {
        if (filter.id != filterId) continue;
        if (filter.hasMask()) return false;
        filter.maskImage = QImage(1, 1, QImage::Format_Grayscale8);
        if (filter.maskImage.isNull()) return false;
        filter.maskImage.fill(255);
        filter.maskReferenceSize = layer->smartPresentationReferenceSize.isValid()
                && !layer->smartPresentationReferenceSize.isEmpty()
            ? layer->smartPresentationReferenceSize : m_sourceImage.size();
        filter.maskReferenceOrigin = layer->smartPresentationReferenceOrigin;
        filter.maskEnabled = true;
        filter.maskInverted = false;
        filter.normalise();
        ++filter.revision;
        ++layer->revision;
        m_modified = true;
        return true;
    }
    return false;
}

bool PhotoDocument::removeLiveFilterMask(const QUuid &smartLayerId, const QUuid &filterId)
{
    LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart || filterId.isNull()) return false;
    for (LiveFilter &filter : layer->liveFilters) {
        if (filter.id != filterId || !filter.hasMask()) continue;
        filter.maskImage = {};
        filter.maskReferenceSize = {};
        filter.maskReferenceOrigin = {};
        filter.maskEnabled = true;
        filter.maskInverted = false;
        ++filter.revision;
        ++layer->revision;
        m_modified = true;
        return true;
    }
    return false;
}

bool PhotoDocument::setLiveFilterMaskEnabled(const QUuid &smartLayerId,
                                             const QUuid &filterId,
                                             const bool enabled)
{
    LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart || filterId.isNull()) return false;
    for (LiveFilter &filter : layer->liveFilters) {
        if (filter.id != filterId || !filter.hasMask()) continue;
        if (filter.maskEnabled == enabled) return true;
        filter.maskEnabled = enabled;
        ++filter.revision;
        ++layer->revision;
        m_modified = true;
        return true;
    }
    return false;
}

bool PhotoDocument::setLiveFilterMaskInverted(const QUuid &smartLayerId,
                                              const QUuid &filterId,
                                              const bool inverted)
{
    LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart || filterId.isNull()) return false;
    for (LiveFilter &filter : layer->liveFilters) {
        if (filter.id != filterId || !filter.hasMask()) continue;
        if (filter.maskInverted == inverted) return true;
        filter.maskInverted = inverted;
        ++filter.revision;
        ++layer->revision;
        m_modified = true;
        return true;
    }
    return false;
}

bool PhotoDocument::updateLiveFilterMask(const QUuid &smartLayerId,
                                         const QUuid &filterId,
                                         const QImage &mask,
                                         const QSize &referenceSize,
                                         const QPointF &referenceOrigin)
{
    if (mask.isNull() || !referenceSize.isValid() || referenceSize.isEmpty()
        || referenceSize.width() > 32768 || referenceSize.height() > 32768
        || !std::isfinite(referenceOrigin.x()) || !std::isfinite(referenceOrigin.y())
        || std::abs(referenceOrigin.x()) > 1.0e9 || std::abs(referenceOrigin.y()) > 1.0e9) {
        return false;
    }
    QImage safeMask = mask.convertToFormat(QImage::Format_Grayscale8);
    if (safeMask.isNull() || safeMask.width() > 32768 || safeMask.height() > 32768) return false;
    LayerNode *layer = findLayerRecursive(m_layers, smartLayerId);
    if (!layer || layer->type != LayerType::Smart || filterId.isNull()) return false;
    for (LiveFilter &filter : layer->liveFilters) {
        if (filter.id != filterId) continue;
        LiveFilter candidate = filter;
        candidate.maskImage = std::move(safeMask);
        candidate.maskReferenceSize = referenceSize;
        candidate.maskReferenceOrigin = referenceOrigin;
        candidate.maskEnabled = true;
        candidate.maskInverted = false;
        candidate.normalise();
        if (!candidate.isSafe()) return false;
        candidate.revision = std::max<quint64>(1, filter.revision + 1);
        filter = std::move(candidate);
        ++layer->revision;
        m_modified = true;
        return true;
    }
    return false;
}


QUuid PhotoDocument::addLayerEffect(const QUuid &layerId,
                                    const LayerEffectType type,
                                    QString *errorMessage)
{
    LayerNode *layer = findLayerRecursive(m_layers, layerId);
    if (!layer || !layerTypeSupportsLayerEffects(layer->type)) {
        setError(errorMessage, QStringLiteral(
            "Layer Effects are supported on Raster, legacy Base Image, Vector, Text and Smart Layers."));
        return {};
    }
    if (layer->layerEffects.size() >= LayerNode::MaximumLayerEffectCount) {
        setError(errorMessage, QStringLiteral("The layer has reached the Layer Effect limit."));
        return {};
    }
    LayerEffect effect;
    effect.type = type;
    switch (type) {
    case LayerEffectType::DropShadow:
        effect.colour = QColor(0, 0, 0);
        effect.effectOpacity = 0.75;
        effect.effectBlendMode = BlendMode::Multiply;
        effect.angleDegrees = 135.0;
        effect.distance = 10.0;
        effect.spread = 0.0;
        effect.size = 10.0;
        break;
    case LayerEffectType::InnerShadow:
        effect.colour = QColor(0, 0, 0);
        effect.effectOpacity = 0.75;
        effect.effectBlendMode = BlendMode::Multiply;
        effect.angleDegrees = 135.0;
        effect.distance = 5.0;
        effect.spread = 0.0;
        effect.size = 5.0;
        break;
    case LayerEffectType::OuterGlow:
    case LayerEffectType::InnerGlow:
        effect.colour = QColor(255, 246, 190);
        effect.effectOpacity = 0.75;
        effect.effectBlendMode = BlendMode::Screen;
        effect.angleDegrees = 0.0;
        effect.distance = 0.0;
        effect.spread = 0.0;
        effect.size = 10.0;
        break;
    case LayerEffectType::Stroke:
        effect.colour = QColor(0, 0, 0);
        effect.effectOpacity = 1.0;
        effect.effectBlendMode = BlendMode::Copy;
        effect.distance = 0.0;
        effect.spread = 0.0;
        effect.size = 3.0;
        effect.strokePosition = LayerEffectStrokePosition::Outside;
        break;
    case LayerEffectType::ColourOverlay:
        effect.colour = QColor(255, 0, 0);
        effect.effectOpacity = 1.0;
        effect.effectBlendMode = BlendMode::Copy;
        effect.distance = 0.0;
        effect.spread = 0.0;
        effect.size = 0.0;
        break;
    case LayerEffectType::GradientOverlay:
        effect.effectOpacity = 1.0;
        effect.effectBlendMode = BlendMode::Copy;
        effect.distance = 0.0;
        effect.spread = 0.0;
        effect.size = 0.0;
        effect.gradientStops = {{0.0, Qt::black}, {1.0, Qt::white}};
        effect.gradientInterpolation = GradientInterpolation::Linear;
        effect.gradientStyle = LayerEffectGradientStyle::Linear;
        effect.gradientAngleDegrees = 90.0;
        effect.gradientScale = 100.0;
        effect.gradientReverse = false;
        break;
    case LayerEffectType::BevelEmboss:
        effect.effectOpacity = 1.0;
        effect.effectBlendMode = BlendMode::Copy;
        effect.angleDegrees = 135.0;
        effect.distance = 0.0;
        effect.spread = 0.0;
        effect.size = 8.0;
        effect.bevelStyle = LayerEffectBevelStyle::InnerBevel;
        effect.bevelDirection = LayerEffectBevelDirection::Up;
        effect.bevelDepth = 100.0;
        effect.bevelSoften = 0.0;
        effect.bevelAltitudeDegrees = 30.0;
        effect.bevelHighlightColour = QColor(255, 255, 255);
        effect.bevelHighlightBlendMode = BlendMode::Screen;
        effect.bevelHighlightOpacity = 0.75;
        effect.bevelShadowColour = QColor(0, 0, 0);
        effect.bevelShadowBlendMode = BlendMode::Multiply;
        effect.bevelShadowOpacity = 0.75;
        break;
    }
    effect.enabled = layerEffectTypeHasRenderer(type);
    effect.normalise();
    if (!effect.isSafe()) {
        setError(errorMessage, QStringLiteral("Could not create the Layer Effect definition."));
        return {};
    }
    const QUuid id = effect.id;
    layer->layerEffects.push_back(std::move(effect));
    ++layer->revision;
    m_modified = true;
    return id;
}

bool PhotoDocument::removeLayerEffect(const QUuid &layerId, const QUuid &effectId)
{
    LayerNode *layer = findLayerRecursive(m_layers, layerId);
    if (!layer || effectId.isNull()) return false;
    for (qsizetype index = 0; index < layer->layerEffects.size(); ++index) {
        if (layer->layerEffects.at(index).id != effectId) continue;
        layer->layerEffects.removeAt(index);
        ++layer->revision;
        m_modified = true;
        return true;
    }
    return false;
}

bool PhotoDocument::setLayerEffectEnabled(const QUuid &layerId,
                                          const QUuid &effectId,
                                          const bool enabled,
                                          QString *errorMessage)
{
    LayerNode *layer = findLayerRecursive(m_layers, layerId);
    if (!layer || effectId.isNull()) return false;
    for (LayerEffect &effect : layer->layerEffects) {
        if (effect.id != effectId) continue;
        if (enabled && !layerEffectTypeHasRenderer(effect.type)) {
            setError(errorMessage,
                     QStringLiteral("%1 rendering is not implemented yet; its exact renderer arrives in %2.")
                         .arg(layerEffectTypeDisplayName(effect.type),
                              layerEffectImplementationRevision(effect.type)));
            return false;
        }
        if (effect.enabled == enabled) return true;
        effect.enabled = enabled;
        ++effect.revision;
        ++layer->revision;
        m_modified = true;
        return true;
    }
    return false;
}

bool PhotoDocument::updateLayerEffect(const QUuid &layerId,
                                      const QUuid &effectId,
                                      const LayerEffect &updated,
                                      QString *errorMessage)
{
    LayerNode *layer = findLayerRecursive(m_layers, layerId);
    if (!layer || effectId.isNull() || !layerTypeSupportsLayerEffects(layer->type)) return false;
    for (LayerEffect &effect : layer->layerEffects) {
        if (effect.id != effectId) continue;
        if (updated.id != effectId || updated.type != effect.type) {
            setError(errorMessage, QStringLiteral("A Layer Effect update cannot change its identity or type."));
            return false;
        }
        LayerEffect replacement = updated;
        replacement.revision = std::max(effect.revision + 1, replacement.revision);
        replacement.normalise();
        if (!replacement.isSafe()) {
            setError(errorMessage, QStringLiteral("The Layer Effect parameters are invalid."));
            return false;
        }
        if (replacement == effect) return true;
        effect = std::move(replacement);
        ++layer->revision;
        m_modified = true;
        return true;
    }
    return false;
}

bool PhotoDocument::moveLayerEffect(const QUuid &layerId,
                                    const QUuid &effectId,
                                    int destinationIndex)
{
    LayerNode *layer = findLayerRecursive(m_layers, layerId);
    if (!layer || effectId.isNull() || layer->layerEffects.isEmpty()) return false;
    qsizetype sourceIndex = -1;
    for (qsizetype index = 0; index < layer->layerEffects.size(); ++index) {
        if (layer->layerEffects.at(index).id == effectId) {
            sourceIndex = index;
            break;
        }
    }
    if (sourceIndex < 0) return false;
    destinationIndex = std::clamp(destinationIndex, 0,
                                  static_cast<int>(layer->layerEffects.size()) - 1);
    if (sourceIndex == destinationIndex) return true;
    LayerEffect effect = layer->layerEffects.takeAt(sourceIndex);
    layer->layerEffects.insert(destinationIndex, std::move(effect));
    ++layer->revision;
    m_modified = true;
    return true;
}

int PhotoDocument::layerCount() const
{
    return countLayersRecursive(m_layers);
}

int PhotoDocument::adjustmentCount() const
{
    return countAdjustmentsRecursive(m_layers);
}

QUuid PhotoDocument::baseLayerId() const
{
    // Retain the historical API name as a compatibility/default-selection
    // helper. Live 0.7 documents contain ordinary Raster layers only.
    const QUuid legacy = findLegacyBaseIdRecursive(m_layers);
    if (!legacy.isNull()) {
        return legacy;
    }
    const QUuid raster = findBottomRasterIdRecursive(m_layers);
    return raster.isNull() ? findBottomLayerIdRecursive(m_layers) : raster;
}

LayerNode PhotoDocument::layerById(const QUuid &id) const
{
    if (const LayerNode *layer = findLayerRecursive(m_layers, id)) {
        return *layer;
    }
    return {};
}

bool PhotoDocument::containsLayer(const QUuid &id) const
{
    return !id.isNull() && findLayerRecursive(m_layers, id) != nullptr;
}

QPointF PhotoDocument::layerWorldOffset(const QUuid &id) const
{
    return layerWorldTransform(id).map(QPointF());
}

QTransform PhotoDocument::layerWorldTransform(const QUuid &id) const
{
    QTransform result;
    findWorldTransformRecursive(m_layers, id, QTransform(), &result);
    return result;
}

QTransform PhotoDocument::layerParentWorldTransform(const QUuid &id) const
{
    QTransform world;
    QTransform parent;
    findWorldTransformRecursive(m_layers, id, QTransform(), &world, &parent);
    return parent;
}

const SelectionMask &PhotoDocument::selectionMask() const
{
    return m_selectionMask;
}

SelectionMask &PhotoDocument::selectionMask()
{
    return m_selectionMask;
}

const QVector<double> &PhotoDocument::horizontalGuides() const
{
    return m_horizontalGuides;
}

const QVector<double> &PhotoDocument::verticalGuides() const
{
    return m_verticalGuides;
}

void PhotoDocument::setGuides(const QVector<double> &horizontal,
                              const QVector<double> &vertical)
{
    if (m_horizontalGuides == horizontal && m_verticalGuides == vertical) {
        return;
    }
    m_horizontalGuides = horizontal;
    m_verticalGuides = vertical;
    m_modified = true;
}

void PhotoDocument::clear()
{
    m_sourceImage = {};
    m_previewSource = {};
    m_sourcePath.clear();
    m_projectPath.clear();
    m_documentIdentity = QUuid::createUuid();
    m_documentName.clear();
    m_colourModel = DocumentColourModel::Rgb;
    m_blankDocument = false;
    m_resolutionX = 72.0;
    m_resolutionY = 72.0;
    m_colourState = DocumentColourState::managedForImage(QColorSpace());
    m_layers.clear();
    m_smartSources = {};
    m_selectionMask.reset(QSize());
    m_horizontalGuides.clear();
    m_verticalGuides.clear();
    m_loadWarnings.clear();
    m_colourResourceWarnings.clear();
    m_linkedSourceWarnings.clear();
    m_modified = false;
}

bool PhotoDocument::createNewDocument(const NewDocumentSettings &settings,
                                      QString *errorMessage)
{
    const int width = settings.pixelSize.width();
    const int height = settings.pixelSize.height();
    if (width < 1 || height < 1 || width > 32768 || height > 32768) {
        setError(errorMessage,
                 QStringLiteral("Document dimensions must be between 1 and 32768 pixels."));
        return false;
    }
    if (settings.bitDepth != 8 && settings.bitDepth != 16) {
        setError(errorMessage, QStringLiteral("Only 8-bit and 16-bit integer documents are supported."));
        return false;
    }

    QImage image(settings.pixelSize,
                 settings.bitDepth == 16 ? QImage::Format_RGBA64
                                         : QImage::Format_RGBA8888);
    if (image.isNull()) {
        setError(errorMessage,
                 QStringLiteral("Could not allocate the requested document. Try smaller dimensions or a lower bit depth."));
        return false;
    }

    QColor fill = settings.backgroundColour.isValid()
        ? settings.backgroundColour
        : QColor(Qt::white);
    if (settings.colourModel == DocumentColourModel::Grayscale) {
        const int grey = qGray(fill.rgb());
        fill.setRgb(grey, grey, grey, fill.alpha());
    }
    image.fill(fill);
    image.setColorSpace(settings.colourSpace.isValid()
                            ? settings.colourSpace
                            : QColorSpace(QColorSpace::SRgb));
    image.setDotsPerMeterX(dotsPerMetreFromDpi(settings.resolutionX));
    image.setDotsPerMeterY(dotsPerMetreFromDpi(settings.resolutionY));

    m_sourceImage = std::move(image);
    m_previewSource = {};
    m_sourcePath.clear();
    m_projectPath.clear();
    m_documentIdentity = QUuid::createUuid();
    m_documentName = settings.name.trimmed();
    if (m_documentName.isEmpty()) {
        m_documentName = QStringLiteral("Untitled Photo");
    }
    m_colourModel = settings.colourModel;
    m_blankDocument = true;
    m_resolutionX = std::clamp(settings.resolutionX, 1.0, 9600.0);
    m_resolutionY = std::clamp(settings.resolutionY, 1.0, 9600.0);
    m_colourState = DocumentColourState::managedForImage(m_sourceImage.colorSpace());
    m_loadWarnings.clear();
    m_colourResourceWarnings.clear();
    m_linkedSourceWarnings.clear();
    m_layers.clear();
    m_selectionMask.reset(m_sourceImage.size());
    m_horizontalGuides.clear();
    m_verticalGuides.clear();
    rebuildPreviewSource();
    createInitialRasterLayer();
    m_modified = true;
    return true;
}

void PhotoDocument::setSourceImage(const QImage &image, const QString &sourcePath)
{
    ImageColourImportInfo colourInfo;
    if (!sourcePath.isEmpty()) {
        colourInfo.sourceStatus = image.colorSpace().isValid()
            ? InputProfileStatus::EmbeddedValid
            : InputProfileStatus::Untagged;
        colourInfo.embeddedProfileAdvertised = image.colorSpace().isValid();
    } else {
        colourInfo.sourceStatus = InputProfileStatus::Generated;
    }
    colourInfo.originalProfileFingerprint = colourProfileContentFingerprint(
        image.colorSpace());
    setSourceImage(image, sourcePath, colourInfo);
}

void PhotoDocument::setSourceImage(const QImage &image,
                                   const QString &sourcePath,
                                   const ImageColourImportInfo &colourInfo)
{
    m_colourModel = sourceFormatIsGrayscale(image.format())
        ? DocumentColourModel::Grayscale
        : DocumentColourModel::Rgb;
    m_sourceImage = straightRgbaImage(image);
    m_sourcePath = sourcePath;
    m_projectPath.clear();
    m_documentIdentity = QUuid::createUuid();
    m_documentName.clear();
    m_blankDocument = false;
    m_resolutionX = dpiFromDotsPerMetre(image.dotsPerMeterX());
    m_resolutionY = dpiFromDotsPerMetre(image.dotsPerMeterY());
    m_colourState = DocumentColourState::managedForImportedImage(
        m_sourceImage.colorSpace(), colourInfo);
    m_loadWarnings = colourInfo.warnings;
    m_colourResourceWarnings.clear();
    m_linkedSourceWarnings.clear();
    m_layers.clear();
    m_selectionMask.reset(m_sourceImage.size());
    m_horizontalGuides.clear();
    m_verticalGuides.clear();
    rebuildPreviewSource();
    createInitialRasterLayer();
    m_modified = true;
}

bool PhotoDocument::replaceCanvasImage(const QImage &image)
{
    if (image.isNull() || image.width() < 1 || image.height() < 1
        || image.width() > 32768 || image.height() > 32768) {
        return false;
    }
    m_sourceImage = straightRgbaImage(image);
    if (m_sourceImage.isNull()) {
        return false;
    }
    const QColorSpace workingColourSpace = documentWorkingQtSpace(m_colourState);
    m_sourceImage.setColorSpace(workingColourSpace);
    m_sourceImage.setDotsPerMeterX(dotsPerMetreFromDpi(m_resolutionX));
    m_sourceImage.setDotsPerMeterY(dotsPerMetreFromDpi(m_resolutionY));
    if (m_selectionMask.size() != m_sourceImage.size()) {
        m_selectionMask.reset(m_sourceImage.size());
    }
    rebuildPreviewSource();
    m_modified = true;
    return true;
}

bool PhotoDocument::replaceStructuralState(
    const QImage &canvasImage,
    const QVector<LayerNode> &layers,
    const SelectionMask::Snapshot &selection,
    const QVector<double> &horizontalGuides,
    const QVector<double> &verticalGuides,
    QString *errorMessage)
{
    return replaceStructuralState(canvasImage,
                                  layers,
                                  selection,
                                  horizontalGuides,
                                  verticalGuides,
                                  m_resolutionX,
                                  m_resolutionY,
                                  errorMessage);
}

bool PhotoDocument::replaceStructuralState(
    const QImage &canvasImage,
    const QVector<LayerNode> &layers,
    const SelectionMask::Snapshot &selection,
    const QVector<double> &horizontalGuides,
    const QVector<double> &verticalGuides,
    const double resolutionX,
    const double resolutionY,
    QString *errorMessage)
{
    return replaceStructuralState(canvasImage,
                                  layers,
                                  selection,
                                  horizontalGuides,
                                  verticalGuides,
                                  resolutionX,
                                  resolutionY,
                                  m_colourState,
                                  errorMessage);
}

bool PhotoDocument::replaceStructuralState(
    const QImage &canvasImage,
    const QVector<LayerNode> &layers,
    const SelectionMask::Snapshot &selection,
    const QVector<double> &horizontalGuides,
    const QVector<double> &verticalGuides,
    const double resolutionX,
    const double resolutionY,
    const DocumentColourState &colourState,
    QString *errorMessage)
{
    return replaceStructuralState(canvasImage, layers, m_smartSources, selection,
                                  horizontalGuides, verticalGuides, resolutionX,
                                  resolutionY, colourState, errorMessage);
}

bool PhotoDocument::replaceStructuralState(
    const QImage &canvasImage,
    const QVector<LayerNode> &layers,
    const SmartSourceRegistry &smartSources,
    const SelectionMask::Snapshot &selection,
    const QVector<double> &horizontalGuides,
    const QVector<double> &verticalGuides,
    const double resolutionX,
    const double resolutionY,
    const DocumentColourState &colourState,
    QString *errorMessage)
{
    if (canvasImage.isNull() || canvasImage.width() < 1
        || canvasImage.height() < 1 || canvasImage.width() > 32768
        || canvasImage.height() > 32768) {
        setError(errorMessage,
                 QStringLiteral("The replacement canvas dimensions are invalid."));
        return false;
    }
    if (!std::isfinite(resolutionX) || !std::isfinite(resolutionY)
        || resolutionX < 1.0 || resolutionX > 9600.0
        || resolutionY < 1.0 || resolutionY > 9600.0) {
        setError(errorMessage,
                 QStringLiteral("The replacement image resolution is invalid."));
        return false;
    }

    const bool colourStateChanged = !colourState.semanticallyEquals(m_colourState);
    DocumentColourState preparedColourState = colourState;
    QString colourStateError;
    if (!preparedColourState.isSafe(&colourStateError)) {
        setError(errorMessage,
                 colourStateError.isEmpty()
                     ? QStringLiteral("The replacement colour state is invalid.")
                     : colourStateError);
        return false;
    }
    // Every successful application, including Undo/Redo, receives a fresh
    // monotonic cache revision while retaining the requested semantic state.
    const quint64 nextColourRevision = m_colourState.revision
            == std::numeric_limits<quint64>::max()
        ? m_colourState.revision
        : m_colourState.revision + 1;
    preparedColourState.revision = std::max(
        nextColourRevision, preparedColourState.revision);

    // Prepare every allocation-bearing replacement before changing any live
    // member. Moving the completed Qt containers below is non-allocating, so a
    // failed conversion, preview build or container copy cannot strand a
    // document with only half of its structural state replaced.
    QImage preparedCanvas = straightRgbaImage(canvasImage);
    if (preparedCanvas.isNull()) {
        setError(errorMessage,
                 QStringLiteral("Could not prepare the replacement canvas."));
        return false;
    }
    const QColorSpace preparedWorkingSpace = documentWorkingQtSpace(preparedColourState);
    preparedCanvas.setColorSpace(preparedWorkingSpace);
    preparedCanvas.setDotsPerMeterX(dotsPerMetreFromDpi(resolutionX));
    preparedCanvas.setDotsPerMeterY(dotsPerMetreFromDpi(resolutionY));

    QImage preparedPreview;
    if (std::max(preparedCanvas.width(), preparedCanvas.height())
        <= PreviewMaxDimension) {
        preparedPreview = preparedCanvas;
    } else {
        const QSize previewSize = preparedCanvas.size().scaled(
            PreviewMaxDimension, PreviewMaxDimension, Qt::KeepAspectRatio);
        preparedPreview = alphaSafeScaledRgba(preparedCanvas, previewSize);
        if (preparedPreview.isNull()) {
            setError(errorMessage,
                     QStringLiteral("Could not prepare the replacement preview."));
            return false;
        }
        preparedPreview.setColorSpace(preparedCanvas.colorSpace());
        preparedPreview.setDotsPerMeterX(dotsPerMetreFromDpi(resolutionX));
        preparedPreview.setDotsPerMeterY(dotsPerMetreFromDpi(resolutionY));
    }

    QVector<LayerNode> preparedLayers = layers;
    if (!layerMetadataIsSafeRecursive(preparedLayers)) {
        setError(errorMessage,
                 QStringLiteral("The replacement layer tree exceeds safe hierarchy, vector-complexity, transform or storage limits."));
        return false;
    }
    QSet<QUuid> ids;
    if (!idsAreUniqueRecursive(preparedLayers, ids)) {
        setError(errorMessage,
                 QStringLiteral("The replacement layer tree contains duplicate or invalid layer IDs."));
        return false;
    }
    SmartSourceRegistry preparedSmartSources = smartSources;
    QString smartSourceError;
    if (!preparedSmartSources.validate(&smartSourceError)
        || !smartLayerReferencesResolveRecursive(preparedLayers, preparedSmartSources)
        || !validateEmbeddedSmartSources(preparedSmartSources, &smartSourceError)) {
        setError(errorMessage, smartSourceError.isEmpty()
            ? QStringLiteral("The replacement Smart Source graph or Smart Layer references are invalid.")
            : smartSourceError);
        return false;
    }
    setRasterLayerColourSpaceRecursive(preparedLayers, preparedWorkingSpace);
    if (!bindSmartPresentationsRecursive(preparedLayers, preparedSmartSources,
                                         preparedWorkingSpace, errorMessage)) {
        return false;
    }

    SelectionMask preparedSelection(preparedCanvas.size());
    if (selection.size != preparedCanvas.size()
        || !preparedSelection.restoreSnapshot(selection, false)) {
        setError(errorMessage,
                 QStringLiteral("The replacement selection does not match the canvas."));
        return false;
    }

    const auto guidesAreValid = [](const QVector<double> &guides,
                                   const double maximum) {
        return std::all_of(guides.cbegin(), guides.cend(),
                           [maximum](const double guide) {
                               return std::isfinite(guide)
                                   && guide >= 0.0 && guide <= maximum;
                           });
    };
    if (horizontalGuides.size() > MaximumProjectGuideCount
        || verticalGuides.size() > MaximumProjectGuideCount
        || !guidesAreValid(horizontalGuides, preparedCanvas.height())
        || !guidesAreValid(verticalGuides, preparedCanvas.width())) {
        setError(errorMessage,
                 QStringLiteral("The replacement guide coordinates are invalid."));
        return false;
    }
    QVector<double> preparedHorizontalGuides = horizontalGuides;
    QVector<double> preparedVerticalGuides = verticalGuides;

    // All validation and potentially allocating preparation is complete.
    m_sourceImage = std::move(preparedCanvas);
    m_previewSource = std::move(preparedPreview);
    m_layers = std::move(preparedLayers);
    m_smartSources = std::move(preparedSmartSources);
    m_linkedSourceWarnings = linkedWarningsForRegistry(m_smartSources, m_projectPath);
    m_selectionMask = std::move(preparedSelection);
    m_horizontalGuides = std::move(preparedHorizontalGuides);
    m_verticalGuides = std::move(preparedVerticalGuides);
    m_resolutionX = resolutionX;
    m_resolutionY = resolutionY;
    m_colourState = std::move(preparedColourState);
    if (colourStateChanged) {
        m_colourResourceWarnings =
            auditDocumentColourResources(m_colourState).messages();
    }
    m_modified = true;
    return true;
}

bool PhotoDocument::replacePresentationColourState(
    const DocumentColourState &colourState,
    QString *errorMessage)
{
    DocumentColourState prepared = m_colourState;
    prepared.presentationColourManagementEnabled =
        colourState.presentationColourManagementEnabled;
    prepared.displayTransform = colourState.displayTransform;
    prepared.proofing = colourState.proofing;

    if (prepared.semanticallyEquals(m_colourState)) {
        return true;
    }

    QString safetyError;
    if (!prepared.isSafe(&safetyError)) {
        setError(errorMessage,
                 safetyError.isEmpty()
                     ? QStringLiteral("The presentation colour state is invalid.")
                     : safetyError);
        return false;
    }

    // Display/proof settings are not part of the authoritative processing
    // identity. Keep the revision stable so tiled rendering, histograms and
    // thumbnails remain valid; ImageCanvas owns their derived display copies.
    prepared.revision = m_colourState.revision;
    m_colourState = std::move(prepared);
    m_colourResourceWarnings =
        auditDocumentColourResources(m_colourState).messages();
    m_modified = true;
    return true;
}

bool PhotoDocument::replaceOutputColourSettings(
    const OutputColourSettings &settings,
    QString *errorMessage)
{
    if (m_colourState.output == settings) {
        return true;
    }

    DocumentColourState prepared = m_colourState;
    prepared.output = settings;

    QString safetyError;
    if (!prepared.isSafe(&safetyError)) {
        setError(errorMessage,
                 safetyError.isEmpty()
                     ? QStringLiteral("The output colour settings are invalid.")
                     : safetyError);
        return false;
    }

    // Export defaults are metadata only. Keep the processing revision stable
    // so compositor, histogram and presentation caches remain reusable.
    prepared.revision = m_colourState.revision;
    m_colourState = std::move(prepared);
    m_colourResourceWarnings =
        auditDocumentColourResources(m_colourState).messages();
    m_modified = true;
    return true;
}

void PhotoDocument::createInitialRasterLayer()
{
    if (!hasImage()) {
        return;
    }
    LayerNode base;
    base.type = LayerType::Raster;
    base.rasterImage = m_sourceImage;
    base.rasterReferenceSize = m_sourceImage.size();
    const QString sourceName = QFileInfo(m_sourcePath).fileName();
    if (m_blankDocument) {
        base.name = QStringLiteral("Background");
    } else {
        base.name = sourceName.isEmpty()
            ? QStringLiteral("Base Image")
            : QStringLiteral("Base Image — %1").arg(sourceName);
    }
    m_layers.push_back(base);
}

void PhotoDocument::promoteLegacyBaseLayers()
{
    promoteLegacyBaseLayersRecursive(m_layers, m_sourceImage);
}

QUuid PhotoDocument::insertLayer(LayerNode layer, const QUuid &selection)
{
    const QUuid insertedId = layer.id;
    if (!selection.isNull()) {
        if (LayerNode *selected = findLayerRecursive(m_layers, selection);
            selected && selected->type == LayerType::Group) {
            selected->children.prepend(std::move(layer));
            m_modified = true;
            return insertedId;
        }

        int index = -1;
        if (QVector<LayerNode> *siblings = findSiblingsRecursive(m_layers, selection, &index)) {
            siblings->insert(index, std::move(layer));
            m_modified = true;
            return insertedId;
        }
    }

    m_layers.prepend(std::move(layer));
    m_modified = true;
    return insertedId;
}

QUuid PhotoDocument::addAdjustment(const AdjustmentType type, const QUuid &selection)
{
    LayerNode layer;
    layer.type = LayerType::Adjustment;
    layer.resetAdjustmentParameters(type);
    layer.name = defaultAdjustmentName(type);
    return insertLayer(std::move(layer), selection);
}

QUuid PhotoDocument::addRasterLayer(const QUuid &selection)
{
    if (!hasImage()) {
        return {};
    }
    LayerNode layer;
    layer.type = LayerType::Raster;
    layer.name = QStringLiteral("Raster Layer");
    // A null raster payload is the compact representation of a completely
    // transparent layer. Resolve its editable local extent after insertion so
    // a layer created inside a hierarchy translated by Crop still covers the
    // visible canvas rather than the old document coordinates.
    const QUuid id = insertLayer(std::move(layer), selection);
    LayerNode *inserted = findLayerRecursive(m_layers, id);
    if (!inserted) {
        return {};
    }
    bool invertible = false;
    const QTransform documentToLayer = layerWorldTransform(id).inverted(&invertible);
    const QRect localCanvas = invertible
        ? documentToLayer.mapRect(QRectF(QPointF(0.0, 0.0),
                                         QSizeF(m_sourceImage.size())))
              .normalized().toAlignedRect()
        : QRect(QPoint(0, 0), m_sourceImage.size());
    const bool safeLocalCanvas = !localCanvas.isEmpty()
        && localCanvas.width() <= 32768 && localCanvas.height() <= 32768
        && std::abs(static_cast<double>(localCanvas.x())) <= 1073741824.0
        && std::abs(static_cast<double>(localCanvas.y())) <= 1073741824.0;
    inserted->rasterReferenceSize = safeLocalCanvas
        ? localCanvas.size() : m_sourceImage.size();
    inserted->rasterReferenceOrigin = safeLocalCanvas
        ? QPointF(localCanvas.topLeft()) : QPointF();
    return id;
}

QUuid PhotoDocument::addVectorShape(const VectorShapeType type,
                                      const QRectF &documentBounds,
                                      const QColor &fillColour,
                                      const QUuid &selection,
                                      const double cornerRadius,
                                      const QLineF &documentLine)
{
    if (!hasImage()) return {};
    const QRectF normalised = documentBounds.normalized();
    const bool lineGeometry = type == VectorShapeType::Line && !documentLine.isNull()
        && std::isfinite(documentLine.p1().x()) && std::isfinite(documentLine.p1().y())
        && std::isfinite(documentLine.p2().x()) && std::isfinite(documentLine.p2().y())
        && documentLine.length() > 0.0;
    if ((!lineGeometry && (normalised.width() <= 0.0 || normalised.height() <= 0.0))
        || !std::isfinite(normalised.x()) || !std::isfinite(normalised.y())
        || !std::isfinite(normalised.width()) || !std::isfinite(normalised.height())) {
        return {};
    }

    const bool wasModified = m_modified;
    LayerNode layer;
    layer.type = LayerType::Vector;
    layer.name = vectorShapeTypeDisplayName(type);
    const QUuid id = insertLayer(std::move(layer), selection);
    LayerNode *inserted = findLayerRecursive(m_layers, id);
    if (!inserted) return {};

    bool invertible = false;
    const QTransform documentToLocal = layerWorldTransform(id).inverted(&invertible);
    const QRectF localBounds = invertible
        ? documentToLocal.mapRect(normalised).normalized()
        : normalised;
    VectorShape shape;
    shape.type = type;
    shape.bounds = localBounds;
    if (type == VectorShapeType::Line && !documentLine.isNull()) {
        shape.lineStart = documentToLocal.map(documentLine.p1());
        shape.lineEnd = documentToLocal.map(documentLine.p2());
    } else {
        shape.lineStart = localBounds.topLeft();
        shape.lineEnd = localBounds.bottomRight();
    }
    shape.cornerRadii.setAll(cornerRadius);
    shape.cornerRadiiLinked = true;
    shape.fill.colour = fillColour.isValid() ? fillColour : QColor(Qt::black);
    if (type == VectorShapeType::Line) {
        shape.fill.enabled = false;
        shape.stroke.enabled = true;
        shape.stroke.colour = shape.fill.colour;
        shape.stroke.width = 2.0;
        shape.stroke.cap = VectorStrokeCap::Round;
    }
    shape.normalise();
    if (shape.type == VectorShapeType::RoundedRectangle) {
        bool orthogonal = false;
        const QSizeF documentSize = shape.orthogonalWorldSize(
            layerWorldTransform(id), &orthogonal);
        const QSizeF available = orthogonal ? documentSize : normalised.size();
        const double maximumRadius = std::max(0.0,
            std::min(available.width(), available.height()) * 0.5);
        shape.cornerRadii.setAll(std::min(shape.cornerRadii.topLeft,
                                           maximumRadius));
    }
    inserted->vectorData.objects = {shape};
    inserted->vectorData.normalise();
    if (!inserted->vectorData.isSafe()) {
        removeLayer(id);
        m_modified = wasModified;
        return {};
    }
    ++inserted->revision;
    m_modified = true;
    return id;
}

QUuid PhotoDocument::addVectorPath(const VectorBezierPath &documentPath,
                                     const QColor &strokeColour,
                                     const QUuid &selection)
{
    if (!hasImage() || !documentPath.isSafe()) return {};
    const bool wasModified = m_modified;
    LayerNode layer;
    layer.type = LayerType::Vector;
    layer.name = QStringLiteral("Path");
    const QUuid id = insertLayer(std::move(layer), selection);
    LayerNode *inserted = findLayerRecursive(m_layers, id);
    if (!inserted) return {};

    bool invertible = false;
    const QTransform documentToLocal = layerWorldTransform(id).inverted(&invertible);
    if (!invertible) {
        removeLayer(id);
        m_modified = wasModified;
        return {};
    }
    VectorShape shape;
    shape.type = VectorShapeType::Path;
    shape.bezierPath = documentPath;
    for (VectorPathNode &node : shape.bezierPath.nodes) {
        node.anchor = documentToLocal.map(node.anchor);
        node.inHandle = documentToLocal.map(node.inHandle);
        node.outHandle = documentToLocal.map(node.outHandle);
    }
    shape.fill.enabled = shape.bezierPath.closed;
    shape.fill.colour = strokeColour.isValid() ? strokeColour : QColor(Qt::black);
    shape.stroke.enabled = true;
    shape.stroke.colour = shape.fill.colour;
    shape.stroke.width = 2.0;
    shape.stroke.cap = VectorStrokeCap::Round;
    shape.stroke.join = VectorStrokeJoin::Round;
    shape.normalise();
    inserted->vectorData.objects = {shape};
    inserted->vectorData.normalise();
    if (!inserted->vectorData.isSafe()) {
        removeLayer(id);
        m_modified = wasModified;
        return {};
    }
    ++inserted->revision;
    m_modified = true;
    return id;
}

QUuid PhotoDocument::addTextLayer(const TextLayerData &data, const QUuid &selection)
{
    if (!hasImage() || !data.isSafe()) return {};
    LayerNode layer; layer.type = LayerType::Text; layer.name = QStringLiteral("Text"); layer.textData = data;
    const QUuid id = insertLayer(std::move(layer), selection);
    if (LayerNode *inserted = findLayerRecursive(m_layers, id)) { ++inserted->revision; m_modified = true; }
    return id;
}

QUuid PhotoDocument::insertTextLayerCopy(const LayerNode &source,
                                         const QTransform &desiredWorldTransform,
                                         const QUuid &selection)
{
    if (!hasImage() || source.type != LayerType::Text || !source.textData.isSafe()
        || !desiredWorldTransform.isInvertible()) return {};
    LayerNode copy = duplicateLayerRecursive(source, nullptr); copy.type = LayerType::Text;
    copy.children.clear(); copy.rasterImage = {}; copy.transform.reset();
    const QUuid id = insertLayer(std::move(copy), selection);
    LayerNode *inserted = findLayerRecursive(m_layers, id); if (!inserted) return {};
    const QTransform parent = layerParentWorldTransform(id); bool ok=false;
    const QTransform inv = parent.inverted(&ok); const QTransform local = desiredWorldTransform * inv;
    if (!ok || !local.isInvertible()) { removeLayer(id); return {}; }
    inserted->transform = local; ++inserted->revision; m_modified = true; return id;
}

QUuid PhotoDocument::insertVectorLayerCopy(
    const LayerNode &source,
    const QTransform &desiredWorldTransform,
    const QUuid &selection)
{
    if (!hasImage() || source.type != LayerType::Vector
        || !source.vectorData.isSafe() || !source.children.isEmpty()
        || !desiredWorldTransform.isInvertible()
        || !transformMatrixIsFiniteAndBounded(desiredWorldTransform)) {
        return {};
    }

    const bool wasModified = m_modified;
    LayerNode copy = duplicateLayerRecursive(source, nullptr);
    copy.type = LayerType::Vector;
    copy.children.clear();
    copy.rasterImage = {};
    copy.rasterReferenceSize = {};
    copy.rasterReferenceOrigin = {};
    copy.transform.reset();
    const QUuid id = insertLayer(std::move(copy), selection);
    LayerNode *inserted = findLayerRecursive(m_layers, id);
    if (!inserted) {
        m_modified = wasModified;
        return {};
    }

    const QTransform parentWorld = layerParentWorldTransform(id);
    bool invertible = false;
    const QTransform parentInverse = parentWorld.inverted(&invertible);
    const QTransform localTransform = desiredWorldTransform * parentInverse;
    if (!invertible || !localTransform.isInvertible()
        || !transformMatrixIsFiniteAndBounded(localTransform)) {
        removeLayer(id);
        m_modified = wasModified;
        return {};
    }
    inserted->transform = localTransform;
    inserted->revision = std::max<quint64>(1, inserted->revision + 1);
    if (!inserted->vectorData.isSafe()
        || !layerMetadataIsSafeRecursive({*inserted})) {
        removeLayer(id);
        m_modified = wasModified;
        return {};
    }
    m_modified = true;
    return id;
}

QUuid PhotoDocument::addGroup(const QUuid &selection)
{
    LayerNode layer;
    layer.type = LayerType::Group;
    layer.name = QStringLiteral("Group");
    return insertLayer(std::move(layer), selection);
}


QUuid PhotoDocument::groupLayers(const QVector<QUuid> &ids, const QString &name)
{
    if (ids.isEmpty()) {
        return addGroup();
    }

    QSet<QUuid> wanted;
    for (const QUuid &id : ids) {
        if (!id.isNull() && containsLayer(id)) {
            wanted.insert(id);
        }
    }
    if (wanted.isEmpty()) {
        return {};
    }

    QVector<QUuid> ordered;
    collectSelectedRootOrder(m_layers, wanted, false, &ordered);
    if (ordered.isEmpty()) {
        return {};
    }

    QHash<QUuid, QTransform> originalWorldTransforms;
    for (const QUuid &id : ordered) {
        QTransform worldTransform;
        if (!findWorldTransformRecursive(m_layers, id, QTransform(), &worldTransform)) {
            return {};
        }
        originalWorldTransforms.insert(id, worldTransform);
    }

    LayerLocation firstLocation;
    if (!findLayerLocationRecursive(m_layers, ordered.constFirst(), {}, &firstLocation)
        || !firstLocation.siblings) {
        return {};
    }

    const QUuid commonParent = firstLocation.parentId;
    bool sameParent = true;
    int insertionIndex = firstLocation.index;
    for (const QUuid &id : ordered) {
        LayerLocation location;
        if (!findLayerLocationRecursive(m_layers, id, {}, &location)) {
            return {};
        }
        sameParent = sameParent && location.parentId == commonParent;
        if (location.parentId == commonParent) {
            insertionIndex = std::min(insertionIndex, location.index);
        }
    }

    // Layers from unrelated branches are grouped at the root. This avoids
    // silently changing one branch's local coordinate system into another's.
    const QUuid destinationParent = sameParent ? commonParent : QUuid();
    QTransform destinationParentWorldTransform;
    if (!destinationParent.isNull()
        && !findWorldTransformRecursive(m_layers,
                                        destinationParent,
                                        QTransform(),
                                        &destinationParentWorldTransform)) {
        return {};
    }
    bool parentInvertible = false;
    const QTransform destinationParentInverse =
        destinationParentWorldTransform.inverted(&parentInvertible);
    if (!parentInvertible) {
        return {};
    }
    if (!sameParent) {
        insertionIndex = 0;
        for (int index = 0; index < m_layers.size(); ++index) {
            if (wanted.contains(m_layers.at(index).id)) {
                insertionIndex = index;
                break;
            }
        }
    }

    QVector<LayerNode> moved;
    moved.reserve(ordered.size());
    for (const QUuid &id : ordered) {
        LayerNode node;
        if (removeLayerById(m_layers, id, &node)) {
            node.transform = originalWorldTransforms.value(id) * destinationParentInverse;
            moved.push_back(std::move(node));
        }
    }
    if (moved.isEmpty()) {
        return {};
    }

    LayerNode group;
    group.type = LayerType::Group;
    group.name = name.trimmed().isEmpty() ? QStringLiteral("Group") : name.trimmed();
    group.children = std::move(moved);
    const QUuid groupId = group.id;

    QVector<LayerNode> *destination = childrenForParent(m_layers, destinationParent);
    if (!destination) {
        destination = &m_layers;
        insertionIndex = 0;
    }
    insertionIndex = std::clamp(insertionIndex, 0, static_cast<int>(destination->size()));
    destination->insert(insertionIndex, std::move(group));
    m_modified = true;
    return groupId;
}

QVector<QUuid> PhotoDocument::duplicateLayers(const QVector<QUuid> &ids,
                                              QHash<QUuid, QUuid> *idMap)
{
    if (ids.isEmpty()) {
        return {};
    }

    QSet<QUuid> wanted;
    for (const QUuid &id : ids) {
        const LayerNode *candidate = findLayerRecursive(m_layers, id);
        if (!id.isNull() && candidate) {
            wanted.insert(id);
        }
    }
    if (wanted.isEmpty()) {
        return {};
    }

    QVector<QUuid> ordered;
    collectSelectedRootOrder(m_layers, wanted, false, &ordered);
    if (ordered.isEmpty()) {
        return {};
    }

    QHash<QUuid, QUuid> generatedMap;
    QVector<QUuid> duplicateRootIds;
    duplicateRootIds.reserve(ordered.size());

    // Each duplicate is inserted immediately above its original. Re-locating
    // the original for every insertion keeps indices correct when several
    // selected roots share the same parent.
    for (const QUuid &sourceId : ordered) {
        LayerLocation location;
        if (!findLayerLocationRecursive(m_layers, sourceId, {}, &location)
            || !location.siblings || location.index < 0) {
            continue;
        }

        const LayerNode source = location.siblings->at(location.index);
        LayerNode duplicate = duplicateLayerRecursive(source, &generatedMap);
        duplicate.name = duplicateLayerName(source.name, *location.siblings);
        const QUuid duplicateId = duplicate.id;
        location.siblings->insert(location.index, std::move(duplicate));
        duplicateRootIds.push_back(duplicateId);
    }

    if (duplicateRootIds.isEmpty()) {
        return {};
    }
    if (idMap) {
        *idMap = std::move(generatedMap);
    }
    m_modified = true;
    return duplicateRootIds;
}

bool PhotoDocument::moveLayers(const QVector<QUuid> &ids,
                               const QUuid &destinationParentId,
                               int destinationIndex)
{
    if (ids.isEmpty()) {
        return false;
    }

    QSet<QUuid> wanted;
    for (const QUuid &id : ids) {
        if (!id.isNull() && containsLayer(id)) {
            wanted.insert(id);
        }
    }
    if (wanted.isEmpty()) {
        return false;
    }

    QVector<QUuid> ordered;
    collectSelectedRootOrder(m_layers, wanted, false, &ordered);
    if (ordered.isEmpty()) {
        return false;
    }

    QHash<QUuid, QTransform> originalWorldTransforms;
    for (const QUuid &id : ordered) {
        QTransform worldTransform;
        if (!findWorldTransformRecursive(m_layers, id, QTransform(), &worldTransform)) {
            return false;
        }
        originalWorldTransforms.insert(id, worldTransform);
    }

    QTransform destinationParentWorldTransform;
    if (!destinationParentId.isNull()
        && !findWorldTransformRecursive(m_layers,
                                        destinationParentId,
                                        QTransform(),
                                        &destinationParentWorldTransform)) {
        return false;
    }
    bool parentInvertible = false;
    const QTransform destinationParentInverse =
        destinationParentWorldTransform.inverted(&parentInvertible);
    if (!parentInvertible) {
        return false;
    }

    if (!destinationParentId.isNull()) {
        const LayerNode *destinationParent = findLayerRecursive(m_layers, destinationParentId);
        if (!destinationParent || destinationParent->type != LayerType::Group) {
            return false;
        }
        for (const QUuid &id : ordered) {
            const LayerNode *source = findLayerRecursive(m_layers, id);
            if (source && containsLayerRecursive(*source, destinationParentId)) {
                return false;
            }
        }
    }

    // Account for selected siblings that are removed before the requested
    // insertion point. The tree view reports the index in the pre-move model.
    for (const QUuid &id : ordered) {
        LayerLocation location;
        if (findLayerLocationRecursive(m_layers, id, {}, &location)
            && location.parentId == destinationParentId
            && location.index < destinationIndex) {
            --destinationIndex;
        }
    }

    QVector<LayerNode> moved;
    moved.reserve(ordered.size());
    for (const QUuid &id : ordered) {
        LayerNode node;
        if (removeLayerById(m_layers, id, &node)) {
            node.transform = originalWorldTransforms.value(id) * destinationParentInverse;
            moved.push_back(std::move(node));
        }
    }
    if (moved.isEmpty()) {
        return false;
    }

    QVector<LayerNode> *destination = childrenForParent(m_layers, destinationParentId);
    if (!destination) {
        return false;
    }
    destinationIndex = std::clamp(destinationIndex, 0, static_cast<int>(destination->size()));
    for (LayerNode &node : moved) {
        destination->insert(destinationIndex++, std::move(node));
    }
    m_modified = true;
    return true;
}

bool PhotoDocument::addMask(const QUuid &id)
{
    if (!hasImage()) {
        return false;
    }
    LayerNode *layer = findLayerRecursive(m_layers, id);
    if (!layer || layer->hasMask()) {
        return false;
    }
    // A single white pixel represents full coverage over a stable local
    // reference rectangle. Raster masks share the raster's storage extent;
    // group/adjustment masks cover the current canvas expressed in that
    // layer's local coordinates. This keeps newly added masks aligned after a
    // non-destructive crop has translated the layer hierarchy.
    layer->maskImage = QImage(1, 1, QImage::Format_Grayscale8);
    layer->maskImage.fill(255);
    if ((layer->type == LayerType::Raster || layer->type == LayerType::BaseImage)
        && layer->rasterReferenceSize.isValid()
        && !layer->rasterReferenceSize.isEmpty()) {
        layer->maskReferenceSize = layer->rasterReferenceSize;
        layer->maskReferenceOrigin = layer->rasterReferenceOrigin;
    } else {
        bool invertible = false;
        const QTransform documentToLayer = layerWorldTransform(id).inverted(&invertible);
        const QRect localCanvas = invertible
            ? documentToLayer.mapRect(QRectF(QPointF(0.0, 0.0),
                                             QSizeF(m_sourceImage.size())))
                  .normalized().toAlignedRect()
            : QRect(QPoint(0, 0), m_sourceImage.size());
        const bool safeLocalCanvas = !localCanvas.isEmpty()
            && localCanvas.width() <= 32768 && localCanvas.height() <= 32768
            && std::abs(static_cast<double>(localCanvas.x())) <= 1073741824.0
            && std::abs(static_cast<double>(localCanvas.y())) <= 1073741824.0;
        layer->maskReferenceSize = safeLocalCanvas
            ? localCanvas.size() : m_sourceImage.size();
        layer->maskReferenceOrigin = safeLocalCanvas
            ? QPointF(localCanvas.topLeft()) : QPointF();
    }
    layer->maskEnabled = true;
    layer->maskInverted = false;
    ++layer->revision;
    m_modified = true;
    return true;
}

bool PhotoDocument::removeMask(const QUuid &id)
{
    LayerNode *layer = findLayerRecursive(m_layers, id);
    if (!layer || !layer->hasMask()) {
        return false;
    }
    layer->maskImage = {};
    layer->maskReferenceSize = {};
    layer->maskReferenceOrigin = {};
    layer->maskEnabled = true;
    layer->maskInverted = false;
    ++layer->revision;
    m_modified = true;
    return true;
}

bool PhotoDocument::setMaskEnabled(const QUuid &id, const bool enabled)
{
    LayerNode *layer = findLayerRecursive(m_layers, id);
    if (!layer || !layer->hasMask() || layer->maskEnabled == enabled) {
        return false;
    }
    layer->maskEnabled = enabled;
    ++layer->revision;
    m_modified = true;
    return true;
}

bool PhotoDocument::setMaskInverted(const QUuid &id, const bool inverted)
{
    LayerNode *layer = findLayerRecursive(m_layers, id);
    if (!layer || !layer->hasMask() || layer->maskInverted == inverted) {
        return false;
    }
    layer->maskInverted = inverted;
    ++layer->revision;
    m_modified = true;
    return true;
}

bool PhotoDocument::updateLayer(const QUuid &id,
                                const std::function<void(LayerNode &)> &update)
{
    LayerNode *layer = findLayerRecursive(m_layers, id);
    if (!layer) {
        return false;
    }
    const LayerNode before = *layer;
    update(*layer);
    layer->opacity = std::clamp(layer->opacity, 0.0, 1.0);
    if (layer->type != LayerType::Group) {
        layer->groupCompositeMode = GroupCompositeMode::Isolated;
    }
    if (!layer->hasMask()) {
        layer->maskEnabled = true;
        layer->maskInverted = false;
    }
    if (!layerMetadataIsSafeRecursive({*layer})
        || !smartLayerReferencesResolveRecursive({*layer}, m_smartSources)) {
        *layer = before;
        return false;
    }
    if (!(*layer == before)) {
        layer->revision = std::max(before.revision + 1, layer->revision);
        m_modified = true;
    }
    return true;
}

bool PhotoDocument::updateLayerInteractive(
    const QUuid &id,
    const std::function<void(LayerNode &)> &update)
{
    LayerNode *layer = findLayerRecursive(m_layers, id);
    if (!layer) {
        return false;
    }
    update(*layer);
    layer->opacity = std::clamp(layer->opacity, 0.0, 1.0);
    if (layer->type != LayerType::Group) {
        layer->groupCompositeMode = GroupCompositeMode::Isolated;
    }
    if (!layer->hasMask()) {
        layer->maskEnabled = true;
        layer->maskInverted = false;
    }
    layer->revision = std::max<quint64>(1, layer->revision + 1);
    m_modified = true;
    return true;
}

bool PhotoDocument::replaceLayer(const QUuid &id, const LayerNode &layer)
{
    LayerNode *target = findLayerRecursive(m_layers, id);
    if (!target) {
        return false;
    }
    LayerNode replacement = layer;
    replacement.id = id;
    if (replacement.type != LayerType::Group) {
        replacement.groupCompositeMode = GroupCompositeMode::Isolated;
    }
    if (!replacement.hasMask()) {
        replacement.maskEnabled = true;
        replacement.maskInverted = false;
    }
    replacement.revision = std::max(target->revision + 1, replacement.revision);
    if (!layerMetadataIsSafeRecursive({replacement})
        || !smartLayerReferencesResolveRecursive({replacement}, m_smartSources)) {
        return false;
    }
    *target = std::move(replacement);
    m_modified = true;
    return true;
}

bool PhotoDocument::layerPlacement(const QUuid &id, QUuid *parentId, int *index) const
{
    if (id.isNull()) {
        return false;
    }
    return findLayerPlacementRecursive(m_layers, id, {}, parentId, index);
}

bool PhotoDocument::insertLayerAt(const LayerNode &layer,
                                  const QUuid &parentId,
                                  const int index)
{
    QSet<QUuid> subtreeIds;
    if (!hasImage()
        || !subtreeIdsAreUniqueAndAbsent(layer, m_layers, &subtreeIds)) {
        return false;
    }
    LayerNode inserted = layer;
    inserted.revision = std::max<quint64>(1, inserted.revision);
    if (!layerMetadataIsSafeRecursive({inserted})
        || !smartLayerReferencesResolveRecursive({inserted}, m_smartSources)) {
        return false;
    }
    QVector<LayerNode> preparedLayers = m_layers;
    QVector<LayerNode> *destination = childrenForParent(preparedLayers, parentId);
    if (!destination) return false;
    destination->insert(std::clamp(index, 0, static_cast<int>(destination->size())),
                        std::move(inserted));
    QString bindError;
    if (!bindSmartPresentationsRecursive(preparedLayers, m_smartSources,
                                         m_sourceImage.colorSpace(), &bindError)) {
        return false;
    }
    m_layers = std::move(preparedLayers);
    m_modified = true;
    return true;
}

bool PhotoDocument::removeLayer(const QUuid &id)
{
    if (id.isNull()) {
        return false;
    }
    const LayerNode *target = findLayerRecursive(m_layers, id);
    if (!target) {
        return false;
    }
    int index = -1;
    QVector<LayerNode> *siblings = findSiblingsRecursive(m_layers, id, &index);
    if (!siblings || index < 0) {
        return false;
    }
    siblings->removeAt(index);
    m_modified = true;
    return true;
}

bool PhotoDocument::moveLayerUp(const QUuid &id)
{
    int index = -1;
    QVector<LayerNode> *siblings = findSiblingsRecursive(m_layers, id, &index);
    if (!siblings || index <= 0) {
        return false;
    }
    siblings->swapItemsAt(index, index - 1);
    m_modified = true;
    return true;
}

bool PhotoDocument::moveLayerDown(const QUuid &id)
{
    int index = -1;
    QVector<LayerNode> *siblings = findSiblingsRecursive(m_layers, id, &index);
    if (!siblings || index < 0 || index >= siblings->size() - 1) {
        return false;
    }
    siblings->swapItemsAt(index, index + 1);
    m_modified = true;
    return true;
}

bool PhotoDocument::replaceLayerTree(const QVector<LayerNode> &layers)
{
    if (!layerMetadataIsSafeRecursive(layers)
        || !smartLayerReferencesResolveRecursive(layers, m_smartSources)) {
        return false;
    }
    QSet<QUuid> ids;
    if (!idsAreUniqueRecursive(layers, ids)) return false;
    QVector<LayerNode> preparedLayers = layers;
    QString bindError;
    if (!bindSmartPresentationsRecursive(preparedLayers, m_smartSources,
                                         m_sourceImage.colorSpace(), &bindError)) {
        return false;
    }
    m_layers = std::move(preparedLayers);
    m_modified = true;
    return true;
}

bool PhotoDocument::saveProject(const QString &filePath, QString *errorMessage)
{
    if (!hasImage()) {
        setError(errorMessage, QStringLiteral("There is no image to save."));
        return false;
    }

    QStringList linkedWarnings;
    QString linkedRefreshError;
    if (!refreshLinkedSmartSources(nullptr, &linkedWarnings, &linkedRefreshError)) {
        setError(errorMessage, linkedRefreshError.isEmpty()
            ? QStringLiteral("Linked Smart Sources could not be refreshed safely before saving.")
            : linkedRefreshError);
        return false;
    }

    QByteArray encodedImage;
    QBuffer imageBuffer(&encodedImage);
    if (!imageBuffer.open(QIODevice::WriteOnly) || !m_sourceImage.save(&imageBuffer, "PNG")) {
        setError(errorMessage, QStringLiteral("Could not encode the source image for the project."));
        return false;
    }

    QJsonObject sourceObject;
    sourceObject.insert(QStringLiteral("originalPath"), m_sourcePath);
    sourceObject.insert(QStringLiteral("fileName"), QFileInfo(m_sourcePath).fileName());
    sourceObject.insert(QStringLiteral("encoding"), QStringLiteral("png-base64"));
    sourceObject.insert(QStringLiteral("data"), QString::fromLatin1(encodedImage.toBase64()));

    if (!layerMetadataIsSafeRecursive(m_layers)) {
        setError(errorMessage,
                 QStringLiteral("The layer tree exceeds safe hierarchy, vector-complexity, transform or storage limits."));
        return false;
    }
    QSet<QUuid> saveIds;
    if (!idsAreUniqueRecursive(m_layers, saveIds)) {
        setError(errorMessage,
                 QStringLiteral("The layer tree contains duplicate or invalid layer IDs."));
        return false;
    }
    QString smartSourceError;
    if (!m_smartSources.validate(&smartSourceError)
        || !smartLayerReferencesResolveRecursive(m_layers, m_smartSources)) {
        setError(errorMessage, smartSourceError.isEmpty()
            ? QStringLiteral("The Smart Source graph or Smart Layer references are invalid.")
            : smartSourceError);
        return false;
    }

    // Rebase relative links for Save As without changing what they point to.
    // Work on a copy so a failed atomic save leaves the live document untouched.
    SmartSourceRegistry persistedSources = m_smartSources;
    const QString targetProjectPath = QDir::cleanPath(QFileInfo(filePath).absoluteFilePath());
    for (const SmartSourceDescriptor &descriptor : persistedSources.descriptors()) {
        if (descriptor.storage != SmartSourceStorage::Linked) continue;
        const QString absolute = resolvedLinkedPath(descriptor.linkedPath, m_projectPath);
        if (canonicalProjectPath(absolute) == canonicalProjectPath(targetProjectPath)) {
            setError(errorMessage, QStringLiteral(
                "The project cannot be saved over one of its linked Smart Source files."));
            return false;
        }
        SmartSourceDescriptor rebased = descriptor;
        rebased.linkedPath = persistedLinkedPath(absolute, targetProjectPath);
        if (!persistedSources.replace(rebased, &smartSourceError)) {
            setError(errorMessage, smartSourceError);
            return false;
        }
    }

    QJsonArray layerArray;
    for (const LayerNode &layer : m_layers) {
        bool layerOk = false;
        const QJsonObject layerObject = layer.toJson(&layerOk);
        if (!layerOk) {
            setError(errorMessage,
                     QStringLiteral("Could not encode one or more layer images, transforms or storage fields for the project."));
            return false;
        }
        layerArray.append(layerObject);
    }

    // The untouched initial Raster layer implicitly shares the exact source
    // QImage. Keep that optimisation in the public project too: the public format
    // already embeds the source payload, so recording its UUID avoids writing
    // the same large PNG a second time. Once the raster detaches through an
    // edit it is serialised normally, exactly like any other raster layer.
    const QUuid sourceRasterId = findSourceSharedRasterIdRecursive(m_layers,
                                                                   m_sourceImage);
    if (!sourceRasterId.isNull()
        && !stripSourceSharedRasterPayload(&layerArray, sourceRasterId)) {
        setError(errorMessage,
                 QStringLiteral("Could not identify the source-backed raster layer while saving."));
        return false;
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("VFXPhotoLabDocument"));
    root.insert(QStringLiteral("version"), ProjectFormatVersion);
    root.insert(QStringLiteral("applicationVersion"), QString::fromLatin1(VFXPHOTOLAB_VERSION));
    root.insert(QStringLiteral("documentIdentity"),
                m_documentIdentity.toString(QUuid::WithoutBraces));
    root.insert(QStringLiteral("editableRasterBase"), true);
    if (!sourceRasterId.isNull()) {
        root.insert(QStringLiteral("sourceRasterLayerId"),
                    sourceRasterId.toString(QUuid::WithoutBraces));
    }
    root.insert(QStringLiteral("source"), sourceObject);
    root.insert(QStringLiteral("layerTree"), layerArray);
    QString embeddedSourceError;
    if (!validateEmbeddedSmartSources(persistedSources, &embeddedSourceError)) {
        setError(errorMessage, embeddedSourceError.isEmpty()
            ? QStringLiteral("The embedded Smart Source state cannot be saved.")
            : embeddedSourceError);
        return false;
    }
    bool smartSourcesOk = false;
    const QJsonArray smartSources = persistedSources.toJson(&smartSourcesOk);
    if (!smartSourcesOk) {
        setError(errorMessage, QStringLiteral("Could not encode the Smart Source registry."));
        return false;
    }
    root.insert(QStringLiteral("smartSources"), smartSources);

    QJsonObject documentSettingsObject;
    documentSettingsObject.insert(QStringLiteral("name"), m_documentName);
    documentSettingsObject.insert(QStringLiteral("colourModel"),
                                  colourModelToString(m_colourModel));
    documentSettingsObject.insert(QStringLiteral("colourSpace"),
                                  colourSpaceToString(m_sourceImage.colorSpace()));
    documentSettingsObject.insert(QStringLiteral("blankDocument"), m_blankDocument);
    documentSettingsObject.insert(QStringLiteral("resolutionX"), m_resolutionX);
    documentSettingsObject.insert(QStringLiteral("resolutionY"), m_resolutionY);
    root.insert(QStringLiteral("documentSettings"), documentSettingsObject);

    QString colourStateError;
    if (!m_colourState.isSafe(&colourStateError)) {
        setError(errorMessage,
                 QStringLiteral("The document colour state cannot be saved: %1")
                     .arg(colourStateError));
        return false;
    }
    root.insert(QStringLiteral("colourManagement"), m_colourState.toJson());

    if (m_horizontalGuides.size() > MaximumProjectGuideCount
        || m_verticalGuides.size() > MaximumProjectGuideCount) {
        setError(errorMessage, QStringLiteral("The document contains excessive guide data."));
        return false;
    }
    QJsonArray horizontalGuideArray;
    for (const double position : m_horizontalGuides) {
        horizontalGuideArray.append(position);
    }
    QJsonArray verticalGuideArray;
    for (const double position : m_verticalGuides) {
        verticalGuideArray.append(position);
    }
    QJsonObject guidesObject;
    guidesObject.insert(QStringLiteral("horizontal"), horizontalGuideArray);
    guidesObject.insert(QStringLiteral("vertical"), verticalGuideArray);
    root.insert(QStringLiteral("guides"), guidesObject);

    if (m_selectionMask.isActive()) {
        bool selectionOk = false;
        const QJsonObject selectionObject = m_selectionMask.toJson(&selectionOk);
        if (!selectionOk) {
            setError(errorMessage, QStringLiteral("Could not encode the document selection."));
            return false;
        }
        root.insert(QStringLiteral("selection"), selectionObject);
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorMessage, file.errorString());
        return false;
    }

    const QByteArray projectBytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (file.write(projectBytes) != projectBytes.size()) {
        setError(errorMessage, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(errorMessage, file.errorString());
        return false;
    }

    m_projectPath = filePath;
    m_smartSources = std::move(persistedSources);
    m_loadWarnings.clear();
    m_colourResourceWarnings = auditDocumentColourResources(m_colourState).messages();
    m_linkedSourceWarnings = linkedWarningsForRegistry(m_smartSources, m_projectPath);
    m_modified = false;
    return true;
}

bool PhotoDocument::loadProject(const QString &filePath, QString *errorMessage)
{
    m_loadWarnings.clear();
    m_colourResourceWarnings.clear();
    m_linkedSourceWarnings.clear();
    LinkedProjectLoadGuard loadGuard(filePath);
    if (!loadGuard.entered()) {
        setError(errorMessage, QStringLiteral(
            "Circular linked Smart Layer dependency detected while opening %1.")
                .arg(QFileInfo(filePath).fileName()));
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage, file.errorString());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (json.isNull() || !json.isObject()) {
        setError(errorMessage,
                 QStringLiteral("The project is not valid JSON: %1").arg(parseError.errorString()));
        return false;
    }

    const QJsonObject root = json.object();
    if (root.value(QStringLiteral("format")).toString()
        != QStringLiteral("VFXPhotoLabDocument")) {
        setError(errorMessage, QStringLiteral("This is not a VFX Photo Lab project."));
        return false;
    }

    const int version = root.value(QStringLiteral("version")).toInt(-1);
    if (version < 1 || version > ProjectFormatVersion) {
        setError(errorMessage,
                 QStringLiteral("Unsupported VFX Photo Lab project version: %1").arg(version));
        return false;
    }
    QUuid restoredDocumentIdentity;
    if (version >= 27) {
        restoredDocumentIdentity = QUuid(root.value(QStringLiteral("documentIdentity")).toString());
        if (restoredDocumentIdentity.isNull()) {
            setError(errorMessage, QStringLiteral(
                "The project has no valid persistent document identity."));
            return false;
        }
    } else {
        restoredDocumentIdentity = legacyProjectIdentityForPath(filePath);
    }

    const QJsonObject sourceObject = root.value(QStringLiteral("source")).toObject();
    if (sourceObject.value(QStringLiteral("encoding")).toString()
        != QStringLiteral("png-base64")) {
        setError(errorMessage, QStringLiteral("The project uses an unsupported image encoding."));
        return false;
    }

    const QJsonValue sourceDataValue = sourceObject.value(QStringLiteral("data"));
    if (!sourceDataValue.isString()) {
        setError(errorMessage, QStringLiteral("The project contains invalid embedded image data."));
        return false;
    }
    const QByteArray encodedImageBytes = sourceDataValue.toString().toLatin1();
    const QByteArray imageBytes = QByteArray::fromBase64(
        encodedImageBytes, QByteArray::AbortOnBase64DecodingErrors);
    if (imageBytes.isNull() && !encodedImageBytes.isEmpty()) {
        setError(errorMessage,
                 QStringLiteral("The project source image contains invalid base64 data."));
        return false;
    }
    QImage source;
    if (!decodeBoundedProjectPng(imageBytes, &source, errorMessage)) {
        return false;
    }

    const QUuid sourceRasterLayerId(
        root.value(QStringLiteral("sourceRasterLayerId")).toString());
    if (root.contains(QStringLiteral("sourceRasterLayerId"))
        && sourceRasterLayerId.isNull()) {
        setError(errorMessage, QStringLiteral("The project contains an invalid source-raster layer ID."));
        return false;
    }

    QVector<LayerNode> parsedLayers;
    if (version == 1) {
        const QJsonValue legacyLayersValue = root.value(QStringLiteral("layers"));
        if (!legacyLayersValue.isArray()) {
            setError(errorMessage, QStringLiteral("The project contains invalid legacy layer data."));
            return false;
        }
        const QJsonArray legacyLayers = legacyLayersValue.toArray();
        if (legacyLayers.size() + 1 > LayerNode::MaximumTreeLayerCount) {
            setError(errorMessage, QStringLiteral("The project layer hierarchy exceeds the safety limit."));
            return false;
        }
        parsedLayers.reserve(legacyLayers.size() + 1);
        for (int index = legacyLayers.size() - 1; index >= 0; --index) {
            const QJsonValue value = legacyLayers.at(index);
            if (!value.isObject()) {
                setError(errorMessage, QStringLiteral("The project contains an invalid layer entry."));
                return false;
            }
            bool layerOk = false;
            LayerNode layer = legacyAdjustmentFromJson(value.toObject(), &layerOk);
            if (!layerOk) {
                setError(errorMessage, QStringLiteral("The project contains an unknown adjustment type."));
                return false;
            }
            parsedLayers.push_back(std::move(layer));
        }
        LayerNode base;
        base.type = LayerType::BaseImage;
        const QString sourceName = sourceObject.value(QStringLiteral("fileName")).toString();
        base.name = sourceName.isEmpty()
            ? QStringLiteral("Base Image")
            : QStringLiteral("Base Image — %1").arg(sourceName);
        parsedLayers.push_back(std::move(base));
    } else {
        const QJsonValue layerTreeValue = root.value(QStringLiteral("layerTree"));
        if (!layerTreeValue.isArray()) {
            setError(errorMessage, QStringLiteral("The project contains invalid layer-tree data."));
            return false;
        }
        const QJsonArray layerArray = layerTreeValue.toArray();
        if (!projectLayerJsonIsWithinSafetyLimits(layerArray, errorMessage)) {
            return false;
        }
        parsedLayers.reserve(layerArray.size());
        for (const QJsonValue &value : layerArray) {
            if (!value.isObject()) {
                setError(errorMessage, QStringLiteral("The project contains an invalid layer entry."));
                return false;
            }
            if (version < 20
                && layerJsonContainsSmartTransformMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-version-20 project cannot contain persistent Smart transform sampling metadata."));
                return false;
            }
            if (version < 21
                && layerJsonContainsLiveFilterMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-version-21 project cannot contain persistent Live Filter metadata."));
                return false;
            }
            if (version < 22
                && layerJsonContainsLiveFilterMaskMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-version-22 project cannot contain persistent Live Filter mask metadata."));
                return false;
            }
            if (version < 23
                && layerJsonContainsLayerEffectMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-version-23 project cannot contain persistent Layer Effect metadata."));
                return false;
            }
            if (version < 24
                && layerJsonContainsLayerEffectParameterMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-version-24 project cannot contain Layer Effect renderer parameters."));
                return false;
            }
            if (version < 25
                && layerJsonContainsLayerEffectStrokeOverlayMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-version-25 project cannot contain Stroke/Overlay Layer Effect parameters."));
                return false;
            }
            if (version < 26
                && layerJsonContainsLayerEffectBevelMetadata(value.toObject())) {
                setError(errorMessage, QStringLiteral(
                    "A pre-version-26 project cannot contain Bevel & Emboss Layer Effect parameters."));
                return false;
            }
            bool layerOk = false;
            LayerNode layer = LayerNode::fromJson(value.toObject(), &layerOk, &m_loadWarnings);
            if (!layerOk) {
                setError(errorMessage, QStringLiteral("The project contains damaged layer data."));
                return false;
            }
            parsedLayers.push_back(std::move(layer));
        }
        const int legacyBaseCount = countBaseLayersRecursive(parsedLayers);
        if (legacyBaseCount > 1) {
            setError(errorMessage, QStringLiteral("The project contains more than one legacy base image layer."));
            return false;
        }
        if (legacyBaseCount == 0
            && !root.value(QStringLiteral("editableRasterBase")).toBool(false)) {
            setError(errorMessage, QStringLiteral("The project has no base image or editable raster-base marker."));
            return false;
        }
    }

    if (version < 7 && containsVectorLayerRecursive(parsedLayers)) {
        setError(errorMessage, QStringLiteral("A pre-version-7 project cannot contain vector layers."));
        return false;
    }
    if (version < 8 && containsTextLayerRecursive(parsedLayers)) {
        setError(errorMessage, QStringLiteral("A pre-version-8 project cannot contain text layers."));
        return false;
    }
    if (version < 9 && containsBezierPathRecursive(parsedLayers)) {
        setError(errorMessage, QStringLiteral("A pre-version-9 project cannot contain editable Bezier paths."));
        return false;
    }
    if (version < 10 && containsLiveVectorCornersRecursive(parsedLayers)) {
        setError(errorMessage, QStringLiteral(
            "A pre-version-10 project cannot contain live vector corners."));
        return false;
    }
    if (version < 11 && containsDashedVectorStrokesRecursive(parsedLayers)) {
        setError(errorMessage, QStringLiteral(
            "A pre-version-11 project cannot contain dashed vector strokes."));
        return false;
    }
    if (version < 12 && containsCompoundVectorPathsRecursive(parsedLayers)) {
        setError(errorMessage, QStringLiteral(
            "A pre-version-12 project cannot contain compound vector paths."));
        return false;
    }

    if (version < 13 && containsNonZeroVectorPathFillRecursive(parsedLayers)) {
        setError(errorMessage, QStringLiteral(
            "A pre-version-13 project cannot contain nonzero-winding vector paths."));
        return false;
    }

    if (version < 14 && containsVectorArrowMetadataRecursive(parsedLayers)) {
        setError(errorMessage, QStringLiteral(
            "A pre-version-14 project cannot contain vector arrowheads or arrow shapes."));
        return false;
    }

    if (version < 16 && containsNonZeroVectorFeatherRecursive(parsedLayers)) {
        setError(errorMessage, QStringLiteral(
            "A pre-version-16 project cannot contain nonzero vector Feather values."));
        return false;
    }

    if (!layerMetadataIsSafeRecursive(parsedLayers)) {
        setError(errorMessage,
                 QStringLiteral("The project layer tree exceeds safe hierarchy, vector-complexity, transform or storage limits."));
        return false;
    }
    QSet<QUuid> ids;
    if (!idsAreUniqueRecursive(parsedLayers, ids)) {
        setError(errorMessage, QStringLiteral("The project contains duplicate or invalid layer IDs."));
        return false;
    }

    SmartSourceRegistry restoredSmartSources;
    if (version >= 17) {
        const QJsonValue smartSourcesValue = root.value(QStringLiteral("smartSources"));
        if (!smartSourcesValue.isArray()) {
            setError(errorMessage, QStringLiteral("The project contains invalid Smart Source registry data."));
            return false;
        }
        if (version < 27) {
            for (const QJsonValue &sourceValue : smartSourcesValue.toArray()) {
                if (!sourceValue.isObject()) continue;
                const QJsonObject sourceObject = sourceValue.toObject();
                if (sourceObject.value(QStringLiteral("schema")).toInt(1) >= 3
                    || sourceObject.contains(QStringLiteral("linkedDocumentId"))
                    || sourceObject.contains(QStringLiteral("linkedFingerprint"))) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-version-27 project cannot contain linked Smart Source identity metadata."));
                    return false;
                }
            }
        }
        bool smartSourcesOk = false;
        restoredSmartSources = SmartSourceRegistry::fromJson(
            smartSourcesValue.toArray(), &smartSourcesOk, &m_loadWarnings);
        if (!smartSourcesOk
            || !smartLayerReferencesResolveRecursive(parsedLayers, restoredSmartSources)) {
            setError(errorMessage, QStringLiteral("The project Smart Source graph or Smart Layer references are invalid."));
            return false;
        }
        if (version < 18) {
            for (const SmartSourceDescriptor &sourceDescriptor : restoredSmartSources.descriptors()) {
                if (sourceDescriptor.hasEmbeddedDocument()
                    || !sourceDescriptor.presentationImage.isNull()) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-version-18 project cannot contain embedded Smart Source contents."));
                    return false;
                }
            }
        }
        if (version < 19) {
            for (const SmartSourceDescriptor &sourceDescriptor : restoredSmartSources.descriptors()) {
                if (sourceDescriptor.hasEmbeddedDocument()
                    && sourceDescriptor.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 2) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-version-19 project cannot contain embedded Smart Source precision metadata."));
                    return false;
                }
            }
        }
        if (version < 20) {
            for (const SmartSourceDescriptor &sourceDescriptor : restoredSmartSources.descriptors()) {
                if (sourceDescriptor.hasEmbeddedDocument()
                    && sourceDescriptor.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 3) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-version-20 project cannot contain embedded Smart transform metadata."));
                    return false;
                }
            }
        }
        if (version < 21) {
            for (const SmartSourceDescriptor &sourceDescriptor : restoredSmartSources.descriptors()) {
                if (sourceDescriptor.hasEmbeddedDocument()
                    && sourceDescriptor.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 4) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-version-21 project cannot contain embedded Live Filter metadata."));
                    return false;
                }
            }
        }
        if (version < 22) {
            for (const SmartSourceDescriptor &sourceDescriptor : restoredSmartSources.descriptors()) {
                if (sourceDescriptor.hasEmbeddedDocument()
                    && sourceDescriptor.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 5) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-version-22 project cannot contain embedded Live Filter mask metadata."));
                    return false;
                }
            }
        }
        if (version < 23) {
            for (const SmartSourceDescriptor &sourceDescriptor : restoredSmartSources.descriptors()) {
                if (sourceDescriptor.hasEmbeddedDocument()
                    && sourceDescriptor.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 6) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-version-23 project cannot contain embedded Layer Effect metadata."));
                    return false;
                }
            }
        }
        if (version < 24) {
            for (const SmartSourceDescriptor &sourceDescriptor : restoredSmartSources.descriptors()) {
                if (sourceDescriptor.hasEmbeddedDocument()
                    && sourceDescriptor.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 7) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-version-24 project cannot contain embedded Layer Effect renderer parameters."));
                    return false;
                }
            }
        }
        if (version < 25) {
            for (const SmartSourceDescriptor &sourceDescriptor : restoredSmartSources.descriptors()) {
                if (sourceDescriptor.hasEmbeddedDocument()
                    && sourceDescriptor.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 8) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-version-25 project cannot contain embedded Stroke/Overlay Layer Effect parameters."));
                    return false;
                }
            }
        }
        if (version < 26) {
            for (const SmartSourceDescriptor &sourceDescriptor : restoredSmartSources.descriptors()) {
                if (sourceDescriptor.hasEmbeddedDocument()
                    && sourceDescriptor.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 9) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-version-26 project cannot contain embedded Bevel & Emboss Layer Effect parameters."));
                    return false;
                }
            }
        }
        if (version < 27) {
            for (const SmartSourceDescriptor &sourceDescriptor : restoredSmartSources.descriptors()) {
                if (sourceDescriptor.hasEmbeddedDocument()
                    && sourceDescriptor.embeddedDocument.value(QStringLiteral("schema")).toInt(1) >= 10) {
                    setError(errorMessage, QStringLiteral(
                        "A pre-version-27 project cannot contain linked-source-capable embedded Smart metadata."));
                    return false;
                }
            }
        }
        QString embeddedSourceError;
        if (!validateEmbeddedSmartSources(restoredSmartSources, &embeddedSourceError)) {
            setError(errorMessage, embeddedSourceError.isEmpty()
                ? QStringLiteral("The project contains invalid embedded Smart Source contents.")
                : embeddedSourceError);
            return false;
        }
    } else if (!smartLayerReferencesResolveRecursive(parsedLayers, restoredSmartSources)) {
        setError(errorMessage, QStringLiteral("This older project cannot contain Smart Layers."));
        return false;
    }

    normaliseRasterLayersRecursive(parsedLayers, source.size(), &m_loadWarnings);
    normaliseMasksRecursive(parsedLayers, source.size(), &m_loadWarnings);

    DocumentColourState restoredColourState;
    if (version >= 15) {
        const QJsonValue colourStateValue = root.value(QStringLiteral("colourManagement"));
        if (!colourStateValue.isObject()) {
            setError(errorMessage,
                     QStringLiteral("The project has no valid colour-management state."));
            return false;
        }
        QString colourStateError;
        const auto parsedColourState = DocumentColourState::fromJson(
            colourStateValue.toObject(), &colourStateError);
        if (!parsedColourState) {
            setError(errorMessage,
                     QStringLiteral("The project colour-management state is invalid: %1")
                         .arg(colourStateError));
            return false;
        }
        restoredColourState = *parsedColourState;
    } else {
        // Version 14 and older projects retain their established component
        // interpretation and adjustment mathematics. The migration records
        // that contract explicitly without converting or retagging pixels.
        restoredColourState = DocumentColourState::legacyForImage(source.colorSpace());
    }

    const QJsonValue guidesValue = root.value(QStringLiteral("guides"));
    if (!guidesValue.isUndefined() && !guidesValue.isObject()) {
        setError(errorMessage, QStringLiteral("The project contains invalid guide data."));
        return false;
    }
    const QJsonObject guidesObject = guidesValue.toObject();
    const QJsonValue horizontalGuidesValue = guidesObject.value(QStringLiteral("horizontal"));
    const QJsonValue verticalGuidesValue = guidesObject.value(QStringLiteral("vertical"));
    if ((!horizontalGuidesValue.isUndefined() && !horizontalGuidesValue.isArray())
        || (!verticalGuidesValue.isUndefined() && !verticalGuidesValue.isArray())) {
        setError(errorMessage, QStringLiteral("The project contains invalid guide data."));
        return false;
    }
    const QJsonArray horizontalGuideValues = horizontalGuidesValue.toArray();
    const QJsonArray verticalGuideValues = verticalGuidesValue.toArray();
    if (horizontalGuideValues.size() > MaximumProjectGuideCount
        || verticalGuideValues.size() > MaximumProjectGuideCount) {
        setError(errorMessage, QStringLiteral("The project contains excessive guide data."));
        return false;
    }
    auto parseGuides = [](const QJsonArray &array, const double maximum) {
        QVector<double> result;
        result.reserve(array.size());
        for (const QJsonValue &value : array) {
            const double position = value.toDouble(-1.0);
            if (std::isfinite(position) && position >= 0.0 && position <= maximum) {
                result.push_back(position);
            }
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    };

    const QJsonObject documentSettingsObject =
        root.value(QStringLiteral("documentSettings")).toObject();
    m_documentName = documentSettingsObject.value(QStringLiteral("name")).toString();
    m_colourModel = colourModelFromString(
        documentSettingsObject.value(QStringLiteral("colourModel")).toString());
    if (documentSettingsObject.isEmpty()) {
        m_colourModel = sourceFormatIsGrayscale(source.format())
            ? DocumentColourModel::Grayscale
            : DocumentColourModel::Rgb;
    }
    const QColorSpace documentColourSpace = colourSpaceFromString(
        documentSettingsObject.value(QStringLiteral("colourSpace")).toString(),
        source.colorSpace());
    if (version < 15) {
        restoredColourState = DocumentColourState::legacyForImage(documentColourSpace);
    }
    m_blankDocument = documentSettingsObject.value(QStringLiteral("blankDocument")).toBool(false);
    m_resolutionX = std::clamp(
        documentSettingsObject.value(QStringLiteral("resolutionX"))
            .toDouble(dpiFromDotsPerMetre(source.dotsPerMeterX())),
        1.0,
        9600.0);
    m_resolutionY = std::clamp(
        documentSettingsObject.value(QStringLiteral("resolutionY"))
            .toDouble(dpiFromDotsPerMetre(source.dotsPerMeterY())),
        1.0,
        9600.0);

    m_colourState = restoredColourState;
    m_sourceImage = straightRgbaImage(source);
    const QColorSpace managedWorkingSpace = documentWorkingQtSpace(m_colourState);
    m_sourceImage.setColorSpace(version >= 15 ? managedWorkingSpace : documentColourSpace);
    m_sourceImage.setDotsPerMeterX(dotsPerMetreFromDpi(m_resolutionX));
    m_sourceImage.setDotsPerMeterY(dotsPerMetreFromDpi(m_resolutionY));
    m_sourcePath = sourceObject.value(QStringLiteral("originalPath")).toString();
    m_projectPath = filePath;
    m_documentIdentity = restoredDocumentIdentity;
    m_layers = std::move(parsedLayers);
    m_smartSources = std::move(restoredSmartSources);
    setRasterLayerColourSpaceRecursive(m_layers, m_sourceImage.colorSpace());
    QHash<QUuid, quint64> linkedChanges;
    QString linkedRefreshError;
    if (!refreshLinkedSmartSources(&linkedChanges, &m_linkedSourceWarnings,
                                   &linkedRefreshError)) {
        setError(errorMessage, linkedRefreshError.isEmpty()
            ? QStringLiteral("The project linked Smart Sources could not be resolved safely.")
            : linkedRefreshError);
        clear();
        return false;
    }
    QString smartPresentationError;
    if (!synchronizeSmartLayerPresentations(&smartPresentationError)) {
        setError(errorMessage, smartPresentationError.isEmpty()
            ? QStringLiteral("The project Smart Layer presentation could not be colour-managed safely.")
            : smartPresentationError);
        clear();
        return false;
    }
    if (!sourceRasterLayerId.isNull()
        && !materialiseSourceSharedRasterRecursive(m_layers,
                                                   sourceRasterLayerId,
                                                   m_sourceImage)) {
        setError(errorMessage,
                 QStringLiteral("The project source-raster reference is inconsistent with its layer tree."));
        clear();
        return false;
    }
    promoteLegacyBaseLayers();
    m_horizontalGuides = parseGuides(horizontalGuideValues, source.height());
    m_verticalGuides = parseGuides(verticalGuideValues, source.width());

    bool selectionRepaired = false;
    if (root.contains(QStringLiteral("selection"))) {
        bool selectionOk = root.value(QStringLiteral("selection")).isObject();
        QString selectionWarning;
        if (selectionOk) {
            m_selectionMask = SelectionMask::fromJson(
                root.value(QStringLiteral("selection")).toObject(),
                source.size(),
                &selectionOk,
                &selectionWarning);
        } else {
            selectionWarning = QStringLiteral("The saved selection entry was invalid and was discarded.");
        }
        if (!selectionOk) {
            m_selectionMask.reset(source.size());
            selectionRepaired = true;
            m_loadWarnings.push_back(selectionWarning.isEmpty()
                ? QStringLiteral("The saved selection was damaged and was discarded.")
                : selectionWarning);
        }
    } else {
        m_selectionMask.reset(source.size());
    }
    rebuildPreviewSource();
    m_colourResourceWarnings = auditDocumentColourResources(m_colourState).messages();
    m_modified = selectionRepaired || !linkedChanges.isEmpty();
    return true;
}

ImageFileReadResult PhotoDocument::readImageFileDetailed(
    const QString &filePath,
    QString *errorMessage)
{
    ImageFileReadResult result;
    if (suffixForPath(filePath) == QStringLiteral("tga")) {
        result.image = TgaCodec::read(filePath, errorMessage);
        if (!result.image.isNull()) {
            result.colourInfo = inspectImageColourProfile(filePath, &result.image);
        }
        return result;
    }

    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    result.image = reader.read();
    if (result.image.isNull()) {
        setError(errorMessage, reader.errorString());
        return result;
    }
    result.colourInfo = inspectImageColourProfile(filePath, &result.image);
    return result;
}

QImage PhotoDocument::readImageFile(const QString &filePath, QString *errorMessage)
{
    return readImageFileDetailed(filePath, errorMessage).image;
}

bool PhotoDocument::writeImageFile(const QString &filePath,
                                   const QImage &image,
                                   const int quality,
                                   QString *errorMessage)
{
    if (image.isNull()) {
        setError(errorMessage, QStringLiteral("There is no image to save."));
        return false;
    }
    const QString suffix = suffixForPath(filePath);
    if (suffix == QStringLiteral("tga")) {
        return TgaCodec::write(filePath, image, errorMessage);
    }
    if (suffix.isEmpty()) {
        setError(errorMessage, QStringLiteral("The output file has no image extension."));
        return false;
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorMessage, file.errorString());
        return false;
    }
    QImageWriter writer(&file, suffix.toLatin1());
    writer.setQuality(std::clamp(quality, 0, 100));
    if (!writer.write(image)) {
        setError(errorMessage, writer.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(errorMessage, file.errorString());
        return false;
    }
    return true;
}

void PhotoDocument::rebuildPreviewSource()
{
    if (m_sourceImage.isNull()) {
        m_previewSource = {};
        return;
    }

    if (std::max(m_sourceImage.width(), m_sourceImage.height()) <= PreviewMaxDimension) {
        m_previewSource = m_sourceImage;
        return;
    }

    const QSize previewSize = m_sourceImage.size().scaled(PreviewMaxDimension,
                                                          PreviewMaxDimension,
                                                          Qt::KeepAspectRatio);
    m_previewSource = alphaSafeScaledRgba(m_sourceImage, previewSize);
    m_previewSource.setColorSpace(m_sourceImage.colorSpace());
}

} // namespace vfx
