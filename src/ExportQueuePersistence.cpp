#include "ExportQueuePersistence.h"

#include "ExportQueueCore.h"
#include "OcioIntegration.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace vfx {
namespace {

QColorSpace documentWorkingQtSpace(const DocumentColourState &state)
{
    if (state.workingSpace.kind == ColourSpaceKind::Ocio) {
        return ocioQtWorkingSpaceProxy(state.workingSpace);
    }
    return state.workingSpace.toQColorSpace();
}

constexpr qint64 MaximumRecoveryFileBytes = 1024ll * 1024ll * 1024ll;
constexpr qint64 MaximumRecoveryImageBytes = 768ll * 1024ll * 1024ll;
constexpr int MaximumJsonStringLength = 4096;
constexpr int MaximumRecoveryLayerCount = LayerNode::MaximumTreeLayerCount;

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) *errorMessage = message;
}

bool jsonString(const QJsonObject &object,
                const QString &key,
                QString *value)
{
    const QJsonValue field = object.value(key);
    if (!value || !field.isString()) return false;
    *value = field.toString();
    return true;
}

bool jsonInteger(const QJsonObject &object,
                 const QString &key,
                 const int minimum,
                 const int maximum,
                 int *value)
{
    const QJsonValue field = object.value(key);
    if (!value || !field.isDouble()) return false;
    const double decoded = field.toDouble();
    if (!std::isfinite(decoded) || std::floor(decoded) != decoded
        || decoded < minimum || decoded > maximum) {
        return false;
    }
    *value = static_cast<int>(decoded);
    return true;
}

bool jsonFiniteNumber(const QJsonObject &object,
                      const QString &key,
                      double *value)
{
    const QJsonValue field = object.value(key);
    if (!value || !field.isDouble()) return false;
    const double decoded = field.toDouble();
    if (!std::isfinite(decoded)) return false;
    *value = decoded;
    return true;
}

struct JsonLayerArray {
    QJsonArray layers;
    int depth = 0;
};

bool jsonLayerTreeIsBounded(const QJsonArray &roots)
{
    QVector<JsonLayerArray> pending;
    pending.push_back({roots, 0});
    int layerCount = 0;
    while (!pending.isEmpty()) {
        const JsonLayerArray current = pending.takeLast();
        if (current.depth < 0 || current.depth >= LayerNode::MaximumTreeDepth) {
            return false;
        }
        for (const QJsonValue &value : current.layers) {
            if (!value.isObject() || ++layerCount > MaximumRecoveryLayerCount) {
                return false;
            }
            const QJsonValue children = value.toObject().value(
                QStringLiteral("children"));
            if (children.isUndefined()) continue;
            if (!children.isArray()) return false;
            if (!children.toArray().isEmpty()) {
                pending.push_back({children.toArray(), current.depth + 1});
            }
        }
    }
    return true;
}

QString collisionPolicyToken(const ProductionExportCollisionPolicy policy)
{
    switch (policy) {
    case ProductionExportCollisionPolicy::AskBeforeStart: return QStringLiteral("ask");
    case ProductionExportCollisionPolicy::Overwrite: return QStringLiteral("overwrite");
    case ProductionExportCollisionPolicy::SkipExisting: return QStringLiteral("skip");
    case ProductionExportCollisionPolicy::AutoRename: return QStringLiteral("auto-rename");
    }
    return {};
}

bool collisionPolicyFromToken(const QString &token,
                              ProductionExportCollisionPolicy *policy)
{
    if (!policy) return false;
    if (token == QStringLiteral("ask")) {
        *policy = ProductionExportCollisionPolicy::AskBeforeStart;
    } else if (token == QStringLiteral("overwrite")) {
        *policy = ProductionExportCollisionPolicy::Overwrite;
    } else if (token == QStringLiteral("skip")) {
        *policy = ProductionExportCollisionPolicy::SkipExisting;
    } else if (token == QStringLiteral("auto-rename")) {
        *policy = ProductionExportCollisionPolicy::AutoRename;
    } else {
        return false;
    }
    return true;
}

QString resizeModeToken(const ProductionExportResizeMode mode)
{
    switch (mode) {
    case ProductionExportResizeMode::OriginalSize: return QStringLiteral("original");
    case ProductionExportResizeMode::ExactPixels: return QStringLiteral("exact");
    case ProductionExportResizeMode::LongEdge: return QStringLiteral("long-edge");
    case ProductionExportResizeMode::Percentage: return QStringLiteral("percentage");
    }
    return {};
}

bool resizeModeFromToken(const QString &token, ProductionExportResizeMode *mode)
{
    if (!mode) return false;
    if (token == QStringLiteral("original")) {
        *mode = ProductionExportResizeMode::OriginalSize;
    } else if (token == QStringLiteral("exact")) {
        *mode = ProductionExportResizeMode::ExactPixels;
    } else if (token == QStringLiteral("long-edge")) {
        *mode = ProductionExportResizeMode::LongEdge;
    } else if (token == QStringLiteral("percentage")) {
        *mode = ProductionExportResizeMode::Percentage;
    } else {
        return false;
    }
    return true;
}

QString resampleToken(const ImageResampleMethod method)
{
    switch (method) {
    case ImageResampleMethod::NearestNeighbour: return QStringLiteral("nearest");
    case ImageResampleMethod::Bilinear: return QStringLiteral("bilinear");
    case ImageResampleMethod::Bicubic: return QStringLiteral("bicubic");
    case ImageResampleMethod::Lanczos3: return QStringLiteral("lanczos3");
    case ImageResampleMethod::Area: return QStringLiteral("area");
    }
    return {};
}

bool resampleFromToken(const QString &token, ImageResampleMethod *method)
{
    if (!method) return false;
    if (token == QStringLiteral("nearest")) {
        *method = ImageResampleMethod::NearestNeighbour;
    } else if (token == QStringLiteral("bilinear")) {
        *method = ImageResampleMethod::Bilinear;
    } else if (token == QStringLiteral("bicubic")) {
        *method = ImageResampleMethod::Bicubic;
    } else if (token == QStringLiteral("lanczos3")) {
        *method = ImageResampleMethod::Lanczos3;
    } else if (token == QStringLiteral("area")) {
        *method = ImageResampleMethod::Area;
    } else {
        return false;
    }
    return true;
}

QJsonObject imageToJson(const QImage &source, QString *errorMessage)
{
    if (source.isNull() || source.size().isEmpty()) {
        setError(errorMessage, QStringLiteral("The recovery image is invalid."));
        return {};
    }
    const QImage image = source.convertToFormat(
        source.depth() > 32 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (image.isNull() || image.sizeInBytes() < 1
        || image.sizeInBytes() > MaximumRecoveryImageBytes) {
        setError(errorMessage, QStringLiteral(
            "The recovery image could not be normalised within its safety limit."));
        return {};
    }
    QByteArray raw;
    raw.resize(static_cast<qsizetype>(image.sizeInBytes()));
    std::memcpy(raw.data(), image.constBits(), static_cast<size_t>(image.sizeInBytes()));
    const QByteArray compressed = qCompress(raw, 1);
    if (compressed.isEmpty() && !raw.isEmpty()) {
        setError(errorMessage, QStringLiteral("The recovery image could not be compressed."));
        return {};
    }
    QJsonObject object;
    object.insert(QStringLiteral("encoding"), QStringLiteral("qimage-zlib-base64-v1"));
    object.insert(QStringLiteral("width"), image.width());
    object.insert(QStringLiteral("height"), image.height());
    object.insert(QStringLiteral("format"), static_cast<int>(image.format()));
    object.insert(QStringLiteral("bytesPerLine"), image.bytesPerLine());
    object.insert(QStringLiteral("data"), QString::fromLatin1(compressed.toBase64()));
    return object;
}

bool imageFromJson(const QJsonObject &object,
                   const DocumentColourState &colourState,
                   QImage *image,
                   QString *errorMessage)
{
    QString encoding;
    int width = 0;
    int height = 0;
    int formatValue = -1;
    int storedBytesPerLine = 0;
    if (!image
        || !jsonString(object, QStringLiteral("encoding"), &encoding)
        || encoding != QStringLiteral("qimage-zlib-base64-v1")
        || !jsonInteger(object, QStringLiteral("width"), 1, 32768, &width)
        || !jsonInteger(object, QStringLiteral("height"), 1, 32768, &height)
        || !jsonInteger(object, QStringLiteral("format"), 0, 255, &formatValue)
        || !jsonInteger(object, QStringLiteral("bytesPerLine"), 1,
                        32768 * 8, &storedBytesPerLine)
        || !object.value(QStringLiteral("data")).isString()) {
        setError(errorMessage, QStringLiteral("The recovery image metadata is invalid."));
        return false;
    }
    const qint64 bytesPerPixel =
        formatValue == static_cast<int>(QImage::Format_RGBA64) ? 8 : 4;
    const qint64 expectedBytes = static_cast<qint64>(width)
        * static_cast<qint64>(height) * bytesPerPixel;
    if ((formatValue != static_cast<int>(QImage::Format_RGBA8888)
            && formatValue != static_cast<int>(QImage::Format_RGBA64))
        || expectedBytes < 1 || expectedBytes > MaximumRecoveryImageBytes
        || storedBytesPerLine != static_cast<qint64>(width) * bytesPerPixel) {
        setError(errorMessage, QStringLiteral("The recovery image metadata is invalid."));
        return false;
    }
    QImage restored(width, height, static_cast<QImage::Format>(formatValue));
    if (restored.isNull() || restored.bytesPerLine() != storedBytesPerLine) {
        setError(errorMessage, QStringLiteral("The recovery image layout is incompatible."));
        return false;
    }
    const QByteArray encoded = object.value(QStringLiteral("data")).toString().toLatin1();
    const QByteArray compressed = QByteArray::fromBase64(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (compressed.size() < 4) {
        setError(errorMessage, QStringLiteral("The recovery image payload is damaged."));
        return false;
    }
    const quint32 advertisedBytes = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar *>(compressed.constData()));
    if (advertisedBytes != static_cast<quint64>(expectedBytes)) {
        setError(errorMessage, QStringLiteral(
            "The recovery image compression header is inconsistent."));
        return false;
    }
    const QByteArray raw = qUncompress(compressed);
    if (raw.size() != static_cast<qsizetype>(restored.sizeInBytes())) {
        setError(errorMessage, QStringLiteral("The recovery image payload is damaged."));
        return false;
    }
    std::memcpy(restored.bits(), raw.constData(), static_cast<size_t>(raw.size()));
    restored.setColorSpace(documentWorkingQtSpace(colourState));
    *image = std::move(restored);
    return true;
}

QJsonObject resizeToJson(const ProductionExportResize &resize)
{
    QJsonObject object;
    object.insert(QStringLiteral("mode"), resizeModeToken(resize.mode));
    object.insert(QStringLiteral("width"), resize.width);
    object.insert(QStringLiteral("height"), resize.height);
    object.insert(QStringLiteral("longEdge"), resize.longEdge);
    object.insert(QStringLiteral("percentage"), resize.percentage);
    object.insert(QStringLiteral("preserveAspect"), resize.preserveAspect);
    object.insert(QStringLiteral("method"), resampleToken(resize.method));
    return object;
}

bool resizeFromJson(const QJsonObject &object,
                    ProductionExportResize *resize,
                    QString *errorMessage)
{
    QString modeToken;
    QString methodToken;
    if (!resize
        || !jsonString(object, QStringLiteral("mode"), &modeToken)
        || !jsonString(object, QStringLiteral("method"), &methodToken)
        || !resizeModeFromToken(modeToken, &resize->mode)
        || !resampleFromToken(methodToken, &resize->method)
        || !jsonInteger(object, QStringLiteral("width"), 0, 1000000,
                        &resize->width)
        || !jsonInteger(object, QStringLiteral("height"), 0, 1000000,
                        &resize->height)
        || !jsonInteger(object, QStringLiteral("longEdge"), 0, 1000000,
                        &resize->longEdge)
        || !jsonFiniteNumber(object, QStringLiteral("percentage"),
                             &resize->percentage)
        || !object.value(QStringLiteral("preserveAspect")).isBool()) {
        setError(errorMessage, QStringLiteral("A recovered output has invalid resize settings."));
        return false;
    }
    resize->preserveAspect = object.value(QStringLiteral("preserveAspect")).toBool();
    return true;
}

QJsonObject planToJson(const ProductionExportPlan &plan, QString *errorMessage)
{
    const QString collisionToken = collisionPolicyToken(plan.collisionPolicy);
    if (plan.outputDirectory.trimmed().isEmpty()
        || plan.outputDirectory.size() > 32768
        || plan.documentName.size() > MaximumJsonStringLength
        || plan.workingSpaceName.size() > MaximumJsonStringLength
        || plan.documentSize.isEmpty()
        || plan.documentSize.width() > 32768
        || plan.documentSize.height() > 32768
        || !plan.timestampUtc.isValid()
        || collisionToken.isEmpty()
        || plan.outputs.isEmpty()
        || plan.outputs.size() > 32) {
        setError(errorMessage, QStringLiteral(
            "The queued production-export plan cannot be represented safely for recovery."));
        return {};
    }
    QJsonArray outputs;
    for (const ProductionExportOutput &output : plan.outputs) {
        // Disabled draft rows are intentionally not part of a recoverable
        // execution description. They were never resolved for the queue and
        // may contain incomplete editor state that must not block recovery of
        // valid enabled outputs.
        if (!output.enabled) continue;
        QString profileError;
        if (output.id.isEmpty() || output.id.size() > 128
            || output.profileId.isEmpty() || output.profileId.size() > 256
            || output.profileName.trimmed().isEmpty()
            || output.profileName.size() > MaximumJsonStringLength
            || output.namingTemplate.size() > MaximumJsonStringLength
            || !output.resize.isValid(plan.documentSize, &profileError)) {
            setError(errorMessage, profileError.isEmpty()
                ? QStringLiteral("An enabled queued output cannot be represented safely for recovery.")
                : profileError);
            return {};
        }
        const QJsonObject profile = output.profile.toJson(&profileError);
        if (!profileError.isEmpty()) {
            setError(errorMessage, profileError);
            return {};
        }
        QJsonObject value;
        value.insert(QStringLiteral("id"), output.id);
        value.insert(QStringLiteral("enabled"), output.enabled);
        value.insert(QStringLiteral("profileId"), output.profileId);
        value.insert(QStringLiteral("profileName"), output.profileName);
        value.insert(QStringLiteral("profile"), profile);
        value.insert(QStringLiteral("namingTemplate"), output.namingTemplate);
        value.insert(QStringLiteral("resize"), resizeToJson(output.resize));
        outputs.append(value);
    }
    if (outputs.isEmpty()) {
        setError(errorMessage, QStringLiteral(
            "The recoverable export plan has no enabled outputs."));
        return {};
    }
    QJsonObject object;
    object.insert(QStringLiteral("outputDirectory"), plan.outputDirectory);
    object.insert(QStringLiteral("documentName"), plan.documentName);
    object.insert(QStringLiteral("documentWidth"), plan.documentSize.width());
    object.insert(QStringLiteral("documentHeight"), plan.documentSize.height());
    object.insert(QStringLiteral("workingSpaceName"), plan.workingSpaceName);
    object.insert(QStringLiteral("timestampUtc"),
                  plan.timestampUtc.toUTC().toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("collisionPolicy"), collisionToken);
    object.insert(QStringLiteral("outputs"), outputs);
    return object;
}

bool planFromJson(const QJsonObject &object,
                  ProductionExportPlan *plan,
                  QString *errorMessage)
{
    QString collisionToken;
    int documentWidth = 0;
    int documentHeight = 0;
    if (!plan
        || !jsonString(object, QStringLiteral("collisionPolicy"),
                       &collisionToken)
        || !collisionPolicyFromToken(collisionToken, &plan->collisionPolicy)
        || !jsonString(object, QStringLiteral("outputDirectory"),
                       &plan->outputDirectory)
        || !jsonString(object, QStringLiteral("documentName"),
                       &plan->documentName)
        || !jsonString(object, QStringLiteral("workingSpaceName"),
                       &plan->workingSpaceName)
        || !jsonInteger(object, QStringLiteral("documentWidth"), 1, 32768,
                        &documentWidth)
        || !jsonInteger(object, QStringLiteral("documentHeight"), 1, 32768,
                        &documentHeight)
        || !object.value(QStringLiteral("timestampUtc")).isString()) {
        setError(errorMessage, QStringLiteral(
            "The recovered export plan metadata is invalid."));
        return false;
    }
    plan->documentSize = QSize(documentWidth, documentHeight);
    if (plan->outputDirectory.size() > 32768
        || plan->documentName.size() > MaximumJsonStringLength
        || plan->workingSpaceName.size() > MaximumJsonStringLength) {
        setError(errorMessage, QStringLiteral(
            "The recovered export plan contains oversized text fields."));
        return false;
    }
    plan->timestampUtc = QDateTime::fromString(
        object.value(QStringLiteral("timestampUtc")).toString(), Qt::ISODateWithMs).toUTC();
    const QJsonValue outputsValue = object.value(QStringLiteral("outputs"));
    if (!outputsValue.isArray() || outputsValue.toArray().isEmpty()
        || outputsValue.toArray().size() > 32
        || !plan->documentSize.isValid() || plan->documentSize.isEmpty()
        || !plan->timestampUtc.isValid()) {
        setError(errorMessage, QStringLiteral("The recovered export plan is incomplete."));
        return false;
    }
    plan->outputs.clear();
    for (const QJsonValue &value : outputsValue.toArray()) {
        if (!value.isObject()) {
            setError(errorMessage, QStringLiteral("The recovered export output is invalid."));
            return false;
        }
        const QJsonObject outputObject = value.toObject();
        ProductionExportOutput output;
        if (!jsonString(outputObject, QStringLiteral("id"), &output.id)
            || !outputObject.value(QStringLiteral("enabled")).isBool()
            || !jsonString(outputObject, QStringLiteral("profileId"),
                           &output.profileId)
            || !jsonString(outputObject, QStringLiteral("profileName"),
                           &output.profileName)
            || !jsonString(outputObject, QStringLiteral("namingTemplate"),
                           &output.namingTemplate)) {
            setError(errorMessage, QStringLiteral(
                "A recovered output contains malformed metadata."));
            return false;
        }
        output.enabled = outputObject.value(QStringLiteral("enabled")).toBool();
        if (!output.enabled) {
            setError(errorMessage, QStringLiteral(
                "Recovered execution descriptions may contain only enabled outputs."));
            return false;
        }
        if (output.id.size() > 128 || output.profileId.size() > 256
            || output.profileName.size() > MaximumJsonStringLength
            || output.namingTemplate.size() > MaximumJsonStringLength
            || !outputObject.value(QStringLiteral("profile")).isObject()
            || !outputObject.value(QStringLiteral("resize")).isObject()
            || !ExportProfileData::fromJson(
                outputObject.value(QStringLiteral("profile")).toObject(),
                &output.profile, errorMessage)
            || !resizeFromJson(outputObject.value(QStringLiteral("resize")).toObject(),
                               &output.resize, errorMessage)) {
            if (errorMessage && errorMessage->isEmpty()) {
                *errorMessage = QStringLiteral("The recovered output settings are invalid.");
            }
            return false;
        }
        plan->outputs.push_back(std::move(output));
    }
    return true;
}

bool layerTreeIsSafeForRecovery(const QVector<LayerNode> &roots)
{
    struct PendingLayerArray {
        const QVector<LayerNode> *layers = nullptr;
        int depth = 0;
    };
    QVector<PendingLayerArray> pending;
    pending.push_back({&roots, 0});
    QSet<QUuid> identifiers;
    identifiers.reserve(std::min(
        roots.size(), static_cast<qsizetype>(MaximumRecoveryLayerCount)));
    int count = 0;
    while (!pending.isEmpty()) {
        const PendingLayerArray entry = pending.takeLast();
        if (!entry.layers || entry.depth < 0
            || entry.depth >= LayerNode::MaximumTreeDepth) {
            return false;
        }
        for (const LayerNode &layer : *entry.layers) {
            if (++count > MaximumRecoveryLayerCount || layer.id.isNull()
                || identifiers.contains(layer.id)) {
                return false;
            }
            identifiers.insert(layer.id);
            if (!layer.children.isEmpty()) {
                pending.push_back({&layer.children, entry.depth + 1});
            }
        }
    }
    return true;
}

QJsonObject requestToJson(const ExportQueueEnqueueRequest &request,
                          QString *errorMessage)
{
    if (request.source.format() != QImage::Format_RGBA8888
        && request.source.format() != QImage::Format_RGBA64) {
        setError(errorMessage, QStringLiteral(
            "The queued recovery snapshot must use straight RGBA8 or RGBA16 pixels."));
        return {};
    }
    QString resolvedError;
    if (!validateResolvedProductionExportOutputs(
            request.plan, request.colourState, request.outputs, &resolvedError)) {
        setError(errorMessage, resolvedError.isEmpty()
            ? QStringLiteral("The queued resolved outputs do not match their recoverable production plan.")
            : resolvedError);
        return {};
    }
    if (request.title.size() > MaximumJsonStringLength
        || request.documentName.size() > MaximumJsonStringLength
        || request.processingCompatibility
            != request.colourState.processingCompatibility) {
        setError(errorMessage, QStringLiteral(
            "The queued job labels or processing contract cannot be represented safely for recovery."));
        return {};
    }
    const QJsonObject source = imageToJson(request.source, errorMessage);
    if (source.isEmpty()) return {};
    QJsonArray layers;
    if (!layerTreeIsSafeForRecovery(request.layers)) {
        setError(errorMessage, QStringLiteral(
            "The queued layer tree has duplicate identifiers or exceeds its safety limit."));
        return {};
    }
    for (const LayerNode &layer : request.layers) {
        bool ok = false;
        const QJsonObject layerObject = layer.toJson(&ok);
        if (!ok) {
            setError(errorMessage, QStringLiteral("A queued layer could not be serialised safely."));
            return {};
        }
        layers.append(layerObject);
    }
    const QJsonObject plan = planToJson(request.plan, errorMessage);
    if (plan.isEmpty()) return {};
    QJsonObject object;
    object.insert(QStringLiteral("title"), request.title);
    object.insert(QStringLiteral("documentName"), request.documentName);
    object.insert(QStringLiteral("source"), source);
    object.insert(QStringLiteral("layers"), layers);
    object.insert(QStringLiteral("colourState"), request.colourState.toJson());
    object.insert(QStringLiteral("processingCompatibility"),
                  static_cast<int>(request.processingCompatibility));
    object.insert(QStringLiteral("plan"), plan);
    return object;
}

bool requestFromJson(const QJsonObject &object,
                     ExportQueueEnqueueRequest *request,
                     QString *errorMessage)
{
    if (!request || !object.value(QStringLiteral("source")).isObject()
        || !object.value(QStringLiteral("layers")).isArray()
        || !object.value(QStringLiteral("colourState")).isObject()
        || !object.value(QStringLiteral("plan")).isObject()) {
        setError(errorMessage, QStringLiteral("The recovery job description is incomplete."));
        return false;
    }
    const auto colourState = DocumentColourState::fromJson(
        object.value(QStringLiteral("colourState")).toObject(), errorMessage);
    if (!colourState) return false;
    if (!jsonString(object, QStringLiteral("title"), &request->title)
        || !jsonString(object, QStringLiteral("documentName"),
                       &request->documentName)) {
        setError(errorMessage, QStringLiteral(
            "The recovery job labels are malformed."));
        return false;
    }
    if (request->title.size() > MaximumJsonStringLength
        || request->documentName.size() > MaximumJsonStringLength) {
        setError(errorMessage, QStringLiteral(
            "The recovered job title or document name exceeds its safety limit."));
        return false;
    }
    request->colourState = *colourState;
    int compatibilityValue = -1;
    if (!jsonInteger(object, QStringLiteral("processingCompatibility"),
                     0, 255, &compatibilityValue)
        || compatibilityValue
        != static_cast<int>(request->colourState.processingCompatibility)) {
        setError(errorMessage, QStringLiteral(
            "The recovered processing-compatibility contract does not match its colour state."));
        return false;
    }
    request->processingCompatibility = request->colourState.processingCompatibility;
    if (!imageFromJson(object.value(QStringLiteral("source")).toObject(),
                       request->colourState, &request->source, errorMessage)) {
        return false;
    }
    request->layers.clear();
    const QJsonArray layers = object.value(QStringLiteral("layers")).toArray();
    if (!jsonLayerTreeIsBounded(layers)) {
        setError(errorMessage, QStringLiteral(
            "The recovered layer tree exceeds its hierarchy or count safety limit."));
        return false;
    }
    QStringList layerWarnings;
    for (const QJsonValue &value : layers) {
        if (!value.isObject()) {
            setError(errorMessage, QStringLiteral("The recovered layer tree is damaged."));
            return false;
        }
        bool ok = false;
        LayerNode layer = LayerNode::fromJson(value.toObject(), &ok, &layerWarnings);
        if (!ok) {
            setError(errorMessage, QStringLiteral("A recovered layer is damaged."));
            return false;
        }
        request->layers.push_back(std::move(layer));
    }
    if (!layerWarnings.isEmpty()) {
        setError(errorMessage, QStringLiteral(
            "A recovered layer required compatibility repair and was rejected: %1")
                .arg(layerWarnings.constFirst()));
        return false;
    }
    if (!layerTreeIsSafeForRecovery(request->layers)) {
        setError(errorMessage, QStringLiteral(
            "The recovered layer tree has duplicate identifiers or exceeds its safety limit."));
        return false;
    }
    if (!planFromJson(object.value(QStringLiteral("plan")).toObject(),
                      &request->plan, errorMessage)
        || request->plan.documentSize != request->source.size()) {
        if (errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("The recovered plan does not match its source image.");
        }
        return false;
    }
    request->outputs.clear();
    return true;
}

QString invalidPathFor(const QString &path)
{
    QString candidate = path + QStringLiteral(".invalid");
    int suffix = 2;
    while (QFileInfo::exists(candidate) && suffix < 1000) {
        candidate = path + QStringLiteral(".invalid-%1").arg(suffix++);
    }
    return candidate;
}

} // namespace

QString ExportQueuePersistence::storageDirectory()
{
    const QString overrideRoot = qEnvironmentVariable(
        "VFXPHOTOLAB_EXPORT_QUEUE_RECOVERY_ROOT").trimmed();
    const QString root = overrideRoot.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        : overrideRoot;
    return QDir(root).filePath(QStringLiteral("export-queue/recovery"));
}

QString ExportQueuePersistence::jobPath(const QString &jobId)
{
    if (!validExportQueueJobIdentifier(jobId)) return {};
    return QDir(storageDirectory()).filePath(jobId + QStringLiteral(".vfxqueue.json"));
}

bool ExportQueuePersistence::writeJob(const QString &jobId,
                                      const QDateTime &createdUtc,
                                      const ExportQueueEnqueueRequest &request,
                                      QString *errorMessage)
{
    if (!validExportQueueJobIdentifier(jobId) || !createdUtc.isValid()) {
        setError(errorMessage, QStringLiteral("The recovery job identifier is invalid."));
        return false;
    }
    QString requestError;
    const QJsonObject requestObject = requestToJson(request, &requestError);
    if (requestObject.isEmpty()) {
        setError(errorMessage, requestError.isEmpty()
            ? QStringLiteral("The queue snapshot could not be serialised.") : requestError);
        return false;
    }
    QJsonObject root;
    root.insert(QStringLiteral("kind"), QStringLiteral("vfx-photo-lab-export-queue-job"));
    root.insert(QStringLiteral("schema"), SchemaVersion);
    root.insert(QStringLiteral("id"), jobId);
    root.insert(QStringLiteral("createdUtc"),
                createdUtc.toUTC().toString(Qt::ISODateWithMs));
    root.insert(QStringLiteral("request"), requestObject);
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (json.isEmpty() || json.size() > MaximumRecoveryFileBytes) {
        setError(errorMessage, QStringLiteral("The recoverable export job exceeds its storage limit."));
        return false;
    }
    QDir directory;
    if (!directory.mkpath(storageDirectory())) {
        setError(errorMessage, QStringLiteral("The export recovery directory could not be created."));
        return false;
    }
    QStorageInfo storage(storageDirectory());
    storage.refresh();
    constexpr qint64 MinimumFreeSpaceAfterRecoveryWrite = 16ll * 1024ll * 1024ll;
    if (storage.isValid() && storage.isReady()
        && storage.bytesAvailable() >= 0
        && storage.bytesAvailable()
            < static_cast<qint64>(json.size()) + MinimumFreeSpaceAfterRecoveryWrite) {
        setError(errorMessage, QStringLiteral(
            "There is not enough free space to store the recoverable export snapshot safely."));
        return false;
    }
    QSaveFile file(jobPath(jobId));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()
        || !file.commit()) {
        setError(errorMessage, QStringLiteral("The recoverable export job could not be written atomically."));
        return false;
    }
    return true;
}

bool ExportQueuePersistence::removeJob(const QString &jobId,
                                       QString *errorMessage)
{
    const QString path = jobPath(jobId);
    if (path.isEmpty()) {
        setError(errorMessage, QStringLiteral("The recovery job identifier is invalid."));
        return false;
    }
    if (!QFileInfo::exists(path)) return true;
    if (!QFile::remove(path)) {
        setError(errorMessage, QStringLiteral("The recoverable export job could not be removed."));
        return false;
    }
    return true;
}

QVector<RecoverableExportQueueJob> ExportQueuePersistence::loadJobs(
    QStringList *warnings)
{
    QVector<RecoverableExportQueueJob> jobs;
    QDir directory(storageDirectory());
    if (!directory.exists()) return jobs;
    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral("*.vfxqueue.json")}, QDir::Files,
        QDir::Time | QDir::Reversed);
    constexpr int MaximumRecoveryFilesScanned = 128;
    const int scanCount = static_cast<int>(std::min<qsizetype>(
        files.size(), MaximumRecoveryFilesScanned));
    int inspectedCount = 0;
    for (int fileIndex = 0; fileIndex < scanCount; ++fileIndex) {
        ++inspectedCount;
        const QFileInfo &info = files.at(fileIndex);
        QString failure;
        if (info.isSymLink()) {
            failure = QStringLiteral("symbolic links are unsupported");
        } else if (info.size() < 1 || info.size() > MaximumRecoveryFileBytes) {
            failure = QStringLiteral("file size is invalid");
        }
        QJsonDocument document;
        if (failure.isEmpty()) {
            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) {
                failure = QStringLiteral("file could not be opened");
            } else {
                QJsonParseError parseError;
                document = QJsonDocument::fromJson(file.readAll(), &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                    failure = QStringLiteral("JSON is malformed");
                }
            }
        }
        RecoverableExportQueueJob recovered;
        if (failure.isEmpty()) {
            const QJsonObject root = document.object();
            QString kind;
            QString createdToken;
            int schema = -1;
            if (!jsonString(root, QStringLiteral("kind"), &kind)
                || !jsonInteger(root, QStringLiteral("schema"), 0, 1000,
                                &schema)
                || !jsonString(root, QStringLiteral("id"), &recovered.id)
                || !jsonString(root, QStringLiteral("createdUtc"),
                               &createdToken)) {
                failure = QStringLiteral("envelope is malformed");
            }
            recovered.createdUtc = QDateTime::fromString(
                createdToken, Qt::ISODateWithMs).toUTC();
            if (failure.isEmpty()
                && (kind != QStringLiteral("vfx-photo-lab-export-queue-job")
                    || schema != SchemaVersion
                    || !validExportQueueJobIdentifier(recovered.id)
                    || recovered.id + QStringLiteral(".vfxqueue.json")
                        != info.fileName()
                    || !recovered.createdUtc.isValid()
                    || !root.value(QStringLiteral("request")).isObject())) {
                failure = QStringLiteral("envelope is invalid");
            }
            if (failure.isEmpty()
                && !requestFromJson(
                    root.value(QStringLiteral("request")).toObject(),
                    &recovered.request, &failure)) {
                if (failure.isEmpty()) failure = QStringLiteral("payload is invalid");
            }
        }
        if (!failure.isEmpty()) {
            const QString quarantined = invalidPathFor(info.absoluteFilePath());
            const bool quarantinedOk = QFile::rename(
                info.absoluteFilePath(), quarantined);
            if (warnings) {
                warnings->push_back(quarantinedOk
                    ? QStringLiteral("%1 — %2; the file was quarantined.")
                          .arg(info.fileName(), failure)
                    : QStringLiteral("%1 — %2; quarantine failed and the file was left in place.")
                          .arg(info.fileName(), failure));
            }
            continue;
        }
        jobs.push_back(std::move(recovered));
        if (jobs.size() >= MaximumRecoverableJobs) break;
    }
    if (warnings && files.size() > inspectedCount) {
        warnings->push_back(QStringLiteral(
            "%1 additional recovery file(s) were retained for a later launch because startup recovery is bounded to %2 records and %3 restored jobs.")
                .arg(files.size() - inspectedCount)
                .arg(MaximumRecoveryFilesScanned)
                .arg(MaximumRecoverableJobs));
    }
    std::sort(jobs.begin(), jobs.end(),
              [](const RecoverableExportQueueJob &left,
                 const RecoverableExportQueueJob &right) {
        return left.createdUtc < right.createdUtc;
    });
    return jobs;
}

} // namespace vfx
