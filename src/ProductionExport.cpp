#include "ProductionExport.h"

#include "ExportNamingTemplate.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <utility>

namespace vfx {
namespace {

constexpr int MaximumProductionOutputs = 32;
constexpr int MaximumOutputExtent = 32768;

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) *errorMessage = message;
}

bool validResampleMethod(const ImageResampleMethod method)
{
    switch (method) {
    case ImageResampleMethod::NearestNeighbour:
    case ImageResampleMethod::Bilinear:
    case ImageResampleMethod::Bicubic:
    case ImageResampleMethod::Lanczos3:
    case ImageResampleMethod::Area:
        return true;
    }
    return false;
}

bool validCollisionPolicy(const ProductionExportCollisionPolicy policy)
{
    switch (policy) {
    case ProductionExportCollisionPolicy::AskBeforeStart:
    case ProductionExportCollisionPolicy::Overwrite:
    case ProductionExportCollisionPolicy::SkipExisting:
    case ProductionExportCollisionPolicy::AutoRename:
        return true;
    }
    return false;
}

bool validOutputIdentifier(const QString &identifier)
{
    if (identifier.isEmpty() || identifier.size() > 128
        || identifier != identifier.trimmed()) {
        return false;
    }
    for (const QChar character : identifier) {
        const ushort value = character.unicode();
        const bool asciiAlphaNumeric =
            (value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z')
            || (value >= '0' && value <= '9');
        if (!asciiAlphaNumeric && character != QLatin1Char('-')
            && character != QLatin1Char('_') && character != QLatin1Char('.')
            && character != QLatin1Char(':')) {
            return false;
        }
    }
    return true;
}

bool validPresetReferenceIdentifier(const QString &identifier)
{
    if (identifier.isEmpty() || identifier.size() > 160
        || identifier != identifier.trimmed()) {
        return false;
    }
    for (const QChar character : identifier) {
        const ushort value = character.unicode();
        const bool lowerAsciiAlphaNumeric =
            (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9');
        if (!lowerAsciiAlphaNumeric && character != QLatin1Char('-')
            && character != QLatin1Char('_') && character != QLatin1Char('.')
            && character != QLatin1Char(':')) {
            return false;
        }
    }
    return true;
}

int scaledExtent(const int extent, const double scale)
{
    return std::clamp(qRound(static_cast<double>(extent) * scale),
                      1, MaximumOutputExtent);
}

QString pathKey(const QString &path)
{
    QString key = QFileInfo(path).absoluteFilePath();
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}

QString uniqueAutoRenamePath(const QString &path,
                             const QSet<QString> &reserved)
{
    const QFileInfo info(path);
    const QString directory = info.absolutePath();
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    for (int copy = 2; copy <= 9999; ++copy) {
        const QString name = suffix.isEmpty()
            ? QStringLiteral("%1-%2").arg(base).arg(copy)
            : QStringLiteral("%1-%2.%3").arg(base).arg(copy).arg(suffix);
        const QString candidate = QDir(directory).filePath(name);
        if (!QFileInfo::exists(candidate)
            && !reserved.contains(pathKey(candidate))) {
            return candidate;
        }
    }
    return {};
}

bool isAutoRenameSiblingPath(const QString &requestedPath,
                             const QString &candidatePath)
{
    const QFileInfo requested(requestedPath);
    const QFileInfo candidate(candidatePath);
    if (pathKey(requested.absolutePath()) != pathKey(candidate.absolutePath())
        || requested.suffix().compare(candidate.suffix(), Qt::CaseInsensitive) != 0) {
        return false;
    }
    const QString prefix = requested.completeBaseName() + QLatin1Char('-');
    const QString candidateBase = candidate.completeBaseName();
    if (!candidateBase.startsWith(prefix)) return false;
    bool ok = false;
    const int copy = candidateBase.mid(prefix.size()).toInt(&ok);
    return ok && copy >= 2 && copy <= 9999
        && candidateBase == prefix + QString::number(copy);
}

} // namespace

QSize ProductionExportResize::resolvedSize(const QSize &sourceSize,
                                           QString *errorMessage) const
{
    if (errorMessage) errorMessage->clear();
    if (sourceSize.isEmpty()) {
        setError(errorMessage, QStringLiteral("The source document size is invalid."));
        return {};
    }
    switch (mode) {
    case ProductionExportResizeMode::OriginalSize:
        return sourceSize;
    case ProductionExportResizeMode::ExactPixels: {
        if (width < 1 || height < 1 || width > MaximumOutputExtent
            || height > MaximumOutputExtent) {
            setError(errorMessage,
                     QStringLiteral("Exact output dimensions must be between 1 and %1 pixels.")
                         .arg(MaximumOutputExtent));
            return {};
        }
        if (!preserveAspect) return QSize(width, height);
        const double scale = std::min(
            static_cast<double>(width) / sourceSize.width(),
            static_cast<double>(height) / sourceSize.height());
        if (!std::isfinite(scale) || scale <= 0.0) {
            setError(errorMessage, QStringLiteral("The exact resize scale is invalid."));
            return {};
        }
        return QSize(scaledExtent(sourceSize.width(), scale),
                     scaledExtent(sourceSize.height(), scale));
    }
    case ProductionExportResizeMode::LongEdge: {
        if (longEdge < 1 || longEdge > MaximumOutputExtent) {
            setError(errorMessage,
                     QStringLiteral("The long edge must be between 1 and %1 pixels.")
                         .arg(MaximumOutputExtent));
            return {};
        }
        const int sourceLongEdge = std::max(sourceSize.width(), sourceSize.height());
        const double scale = static_cast<double>(longEdge) / sourceLongEdge;
        return QSize(scaledExtent(sourceSize.width(), scale),
                     scaledExtent(sourceSize.height(), scale));
    }
    case ProductionExportResizeMode::Percentage: {
        if (!std::isfinite(percentage) || percentage < 0.1
            || percentage > 3200.0) {
            setError(errorMessage,
                     QStringLiteral("The resize percentage must be between 0.1% and 3200%."));
            return {};
        }
        const double scale = percentage / 100.0;
        const double rawWidth = static_cast<double>(sourceSize.width()) * scale;
        const double rawHeight = static_cast<double>(sourceSize.height()) * scale;
        if (!std::isfinite(rawWidth) || !std::isfinite(rawHeight)
            || rawWidth > MaximumOutputExtent
            || rawHeight > MaximumOutputExtent) {
            setError(errorMessage,
                     QStringLiteral("The percentage resize exceeds the safe output extent."));
            return {};
        }
        return QSize(std::max(1, qRound(rawWidth)),
                     std::max(1, qRound(rawHeight)));
    }
    }
    setError(errorMessage, QStringLiteral("The resize mode is invalid."));
    return {};
}

bool ProductionExportResize::isValid(const QSize &sourceSize,
                                     QString *errorMessage) const
{
    return !resolvedSize(sourceSize, errorMessage).isEmpty();
}

QString productionExportResizeModeName(const ProductionExportResizeMode mode)
{
    switch (mode) {
    case ProductionExportResizeMode::OriginalSize: return QStringLiteral("Original size");
    case ProductionExportResizeMode::ExactPixels: return QStringLiteral("Exact pixels");
    case ProductionExportResizeMode::LongEdge: return QStringLiteral("Long edge");
    case ProductionExportResizeMode::Percentage: return QStringLiteral("Percentage");
    }
    return QStringLiteral("Unknown");
}

QString productionExportCollisionPolicyName(
    const ProductionExportCollisionPolicy policy)
{
    switch (policy) {
    case ProductionExportCollisionPolicy::AskBeforeStart:
        return QStringLiteral("Ask before replacing");
    case ProductionExportCollisionPolicy::Overwrite:
        return QStringLiteral("Replace existing files");
    case ProductionExportCollisionPolicy::SkipExisting:
        return QStringLiteral("Skip existing files");
    case ProductionExportCollisionPolicy::AutoRename:
        return QStringLiteral("Auto-rename new files");
    }
    return QStringLiteral("Unknown");
}

QString imageResampleMethodName(const ImageResampleMethod method)
{
    switch (method) {
    case ImageResampleMethod::NearestNeighbour: return QStringLiteral("Nearest neighbour");
    case ImageResampleMethod::Bilinear: return QStringLiteral("Bilinear");
    case ImageResampleMethod::Bicubic: return QStringLiteral("Bicubic");
    case ImageResampleMethod::Lanczos3: return QStringLiteral("Lanczos 3");
    case ImageResampleMethod::Area: return QStringLiteral("Area");
    }
    return QStringLiteral("Unknown");
}

QString uniqueProductionExportOutputPath(
    const QString &requestedPath,
    const QStringList &reservedPaths)
{
    if (requestedPath.trimmed().isEmpty()) return {};
    QSet<QString> reserved;
    reserved.reserve(reservedPaths.size());
    for (const QString &path : reservedPaths) {
        if (!path.trimmed().isEmpty()) reserved.insert(pathKey(path));
    }
    return uniqueAutoRenamePath(QFileInfo(requestedPath).absoluteFilePath(),
                                reserved);
}

bool resolveProductionExportPlan(
    const ProductionExportPlan &plan,
    const DocumentColourState &colourState,
    QVector<ResolvedProductionExportOutput> *resolvedOutputs,
    QStringList *warnings,
    QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (warnings) warnings->clear();
    if (!resolvedOutputs) {
        setError(errorMessage,
                 QStringLiteral("The production-export result destination is missing."));
        return false;
    }
    resolvedOutputs->clear();
    QVector<ResolvedProductionExportOutput> preparedOutputs;
    const QFileInfo directoryInfo(plan.outputDirectory);
    if (plan.outputDirectory.trimmed().isEmpty()
        || !directoryInfo.exists() || !directoryInfo.isDir()
        || !directoryInfo.isWritable()) {
        setError(errorMessage,
                 QStringLiteral("Choose an existing writable output directory."));
        return false;
    }
    if (plan.documentSize.isEmpty()) {
        setError(errorMessage, QStringLiteral("The production-export document size is invalid."));
        return false;
    }
    if (!plan.timestampUtc.isValid()) {
        setError(errorMessage,
                 QStringLiteral("The production-export naming timestamp is invalid."));
        return false;
    }
    if (!validCollisionPolicy(plan.collisionPolicy)) {
        setError(errorMessage,
                 QStringLiteral("The production-export collision policy is invalid."));
        return false;
    }
    if (plan.outputs.size() > MaximumProductionOutputs) {
        setError(errorMessage,
                 QStringLiteral("A production export may contain at most %1 outputs.")
                     .arg(MaximumProductionOutputs));
        return false;
    }
    preparedOutputs.reserve(plan.outputs.size());
    int enabledCount = 0;
    for (const ProductionExportOutput &output : plan.outputs) {
        if (output.enabled) ++enabledCount;
    }
    if (enabledCount < 1) {
        setError(errorMessage, QStringLiteral("Enable at least one production output."));
        return false;
    }

    QSet<QString> reservedPaths;
    QSet<QString> outputIds;
    for (const ProductionExportOutput &output : plan.outputs) {
        if (!validOutputIdentifier(output.id)
            || outputIds.contains(output.id)) {
            setError(errorMessage,
                     QStringLiteral("Every production output must have a unique stable identifier."));
            return false;
        }
        outputIds.insert(output.id);
        if (!output.enabled) continue;
        if (!validPresetReferenceIdentifier(output.profileId)
            || output.profileName.trimmed().isEmpty()
            || output.profileName != output.profileName.trimmed()
            || output.profileName.size() > PresetStore::MaximumNameLength) {
            setError(errorMessage,
                     QStringLiteral("Every enabled production output must reference a valid stable profile identifier and bounded profile name."));
            return false;
        }
        if (!validResampleMethod(output.resize.method)) {
            setError(errorMessage,
                     QStringLiteral("Output ‘%1’ uses an invalid resampling method.")
                         .arg(output.profileName));
            return false;
        }
        QString profileError;
        if (!output.profile.isValid(&profileError)) {
            setError(errorMessage,
                     QStringLiteral("Output ‘%1’ has invalid profile settings: %2")
                         .arg(output.profileName, profileError));
            return false;
        }
        const QSize outputSize = output.resize.resolvedSize(
            plan.documentSize, &profileError);
        if (outputSize.isEmpty()) {
            setError(errorMessage,
                     QStringLiteral("Output ‘%1’ has invalid resize settings: %2")
                         .arg(output.profileName, profileError));
            return false;
        }
        const quint64 pixelCount = static_cast<quint64>(outputSize.width())
            * static_cast<quint64>(outputSize.height());
        constexpr quint64 MaximumSurfaceBytes = 0xfffffffeULL;
        if (pixelCount > MaximumSurfaceBytes / 8u) {
            setError(errorMessage,
                     QStringLiteral("Output ‘%1’ is too large for a safe 16-bit export surface.")
                         .arg(output.profileName));
            return false;
        }
        if (output.profile.formatSuffix == QStringLiteral("tga")
            && (outputSize.width() > 65535 || outputSize.height() > 65535)) {
            setError(errorMessage,
                     QStringLiteral("Output ‘%1’ exceeds the classic TGA 65,535-pixel dimension limit.")
                         .arg(output.profileName));
            return false;
        }
        ExportNamingContext naming;
        naming.documentName = plan.documentName;
        naming.profileName = output.profileName;
        naming.formatSuffix = output.profile.formatSuffix;
        naming.bitDepth = static_cast<int>(output.profile.bitDepth);
        naming.imageSize = outputSize;
        naming.workingSpaceName = plan.workingSpaceName;
        naming.outputSpaceName = output.profile.convertToOutputProfile
            ? output.profile.output.profile.displayName
            : plan.workingSpaceName;
        naming.timestampUtc = plan.timestampUtc;
        const QString nameTemplate = output.namingTemplate.trimmed().isEmpty()
            ? output.profile.namingTemplate : output.namingTemplate;
        const QString stem = ExportNamingTemplate::resolve(
            nameTemplate, naming, &profileError);
        if (stem.isEmpty()) {
            setError(errorMessage,
                     QStringLiteral("Output ‘%1’ has an invalid filename template: %2")
                         .arg(output.profileName, profileError));
            return false;
        }
        QString path = QDir(plan.outputDirectory).filePath(
            QStringLiteral("%1.%2").arg(stem, output.profile.formatSuffix));
        path = QFileInfo(path).absoluteFilePath();
        if (reservedPaths.contains(pathKey(path))) {
            if (plan.collisionPolicy == ProductionExportCollisionPolicy::AutoRename) {
                path = uniqueAutoRenamePath(path, reservedPaths);
                if (path.isEmpty()) {
                    setError(errorMessage,
                             QStringLiteral("Could not create a unique filename for output ‘%1’.")
                                 .arg(output.profileName));
                    return false;
                }
            } else {
                setError(errorMessage,
                         QStringLiteral("Two enabled outputs resolve to the same file: %1")
                             .arg(path));
                return false;
            }
        }
        const bool exists = QFileInfo::exists(path);
        bool skip = false;
        if (exists) {
            if (plan.collisionPolicy == ProductionExportCollisionPolicy::SkipExisting) {
                skip = true;
            } else if (plan.collisionPolicy == ProductionExportCollisionPolicy::AutoRename) {
                path = uniqueAutoRenamePath(path, reservedPaths);
                if (path.isEmpty()) {
                    setError(errorMessage,
                             QStringLiteral("Could not create a unique filename for output ‘%1’.")
                                 .arg(output.profileName));
                    return false;
                }
            } else if (warnings) {
                warnings->push_back(
                    QStringLiteral("Existing file: %1").arg(path));
            }
        }

        ImageExportRequest request;
        request.filePath = path;
        request.quality = output.profile.quality;
        request.bitDepth = output.profile.bitDepth;
        request.dither = output.profile.dither;
        request.alphaMode = output.profile.alphaMode;
        request.convertToOutputProfile = output.profile.convertToOutputProfile;
        request.output = output.profile.output;
        request.matteColour = output.profile.matteColour;
        if (!validateImageExportRequest(request, colourState, &profileError)) {
            setError(errorMessage,
                     QStringLiteral("Output ‘%1’ is not compatible with this document: %2")
                         .arg(output.profileName, profileError));
            return false;
        }
        const ImageExportCapabilities capabilities =
            imageExportCapabilitiesForPath(path);
        if (!imageExportWriterAvailable(capabilities)) {
            setError(errorMessage,
                     QStringLiteral("The image writer required for output ‘%1’ (%2) is unavailable.")
                         .arg(output.profileName, capabilities.displayName));
            return false;
        }

        ResolvedProductionExportOutput resolved;
        resolved.id = output.id;
        resolved.profileId = output.profileId;
        resolved.profileName = output.profileName;
        resolved.outputSize = outputSize;
        resolved.resampleMethod = output.resize.method;
        resolved.resizeRequired = outputSize != plan.documentSize;
        resolved.existedAtPreflight = QFileInfo::exists(path);
        resolved.skipExisting = skip;
        resolved.request = request;
        preparedOutputs.push_back(std::move(resolved));
        reservedPaths.insert(pathKey(path));
    }
    *resolvedOutputs = std::move(preparedOutputs);
    return true;
}

bool validateResolvedProductionExportOutputs(
    const ProductionExportPlan &plan,
    const DocumentColourState &colourState,
    const QVector<ResolvedProductionExportOutput> &resolvedOutputs,
    QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();

    const QFileInfo outputDirectoryInfo(plan.outputDirectory);
    if (plan.outputDirectory.trimmed().isEmpty()
        || !outputDirectoryInfo.exists() || !outputDirectoryInfo.isDir()
        || !outputDirectoryInfo.isWritable() || plan.documentSize.isEmpty()
        || !plan.timestampUtc.isValid()
        || !validCollisionPolicy(plan.collisionPolicy)
        || plan.outputs.isEmpty()
        || plan.outputs.size() > MaximumProductionOutputs) {
        setError(errorMessage, QStringLiteral(
            "The production plan metadata or destination is no longer valid for queueing."));
        return false;
    }

    QHash<QString, const ProductionExportOutput *> enabledById;
    enabledById.reserve(plan.outputs.size());
    for (const ProductionExportOutput &output : plan.outputs) {
        if (!output.enabled) continue;
        QString profileError;
        if (!validOutputIdentifier(output.id) || enabledById.contains(output.id)
            || !validPresetReferenceIdentifier(output.profileId)
            || output.profileName.trimmed().isEmpty()
            || output.profileName != output.profileName.trimmed()
            || output.profileName.size() > PresetStore::MaximumNameLength
            || !validResampleMethod(output.resize.method)
            || !output.profile.isValid(&profileError)) {
            setError(errorMessage, QStringLiteral(
                "The production plan contains an invalid enabled output, profile reference, or duplicate identifier."));
            return false;
        }
        enabledById.insert(output.id, &output);
    }
    if (enabledById.isEmpty() || resolvedOutputs.size() != enabledById.size()) {
        setError(errorMessage, QStringLiteral(
            "The resolved queue payload does not contain exactly one entry for every enabled production output."));
        return false;
    }

    const QString outputDirectoryKey = pathKey(
        QFileInfo(plan.outputDirectory).absoluteFilePath());
    QSet<QString> seenIds;
    QSet<QString> seenPaths;
    seenIds.reserve(resolvedOutputs.size());
    seenPaths.reserve(resolvedOutputs.size());

    for (const ResolvedProductionExportOutput &resolved : resolvedOutputs) {
        const auto configuredIt = enabledById.constFind(resolved.id);
        if (configuredIt == enabledById.cend() || seenIds.contains(resolved.id)) {
            setError(errorMessage, QStringLiteral(
                "The resolved queue payload contains an unknown or duplicate output identifier."));
            return false;
        }
        seenIds.insert(resolved.id);
        const ProductionExportOutput &configured = *configuredIt.value();

        QString resizeError;
        const QSize expectedSize = configured.resize.resolvedSize(
            plan.documentSize, &resizeError);
        const quint64 expectedPixels = static_cast<quint64>(expectedSize.width())
            * static_cast<quint64>(expectedSize.height());
        constexpr quint64 MaximumSurfaceBytes = 0xfffffffeULL;
        if (expectedSize.isEmpty()
            || expectedPixels > MaximumSurfaceBytes / 8u
            || (configured.profile.formatSuffix == QStringLiteral("tga")
                && (expectedSize.width() > 65535
                    || expectedSize.height() > 65535))
            || resolved.profileId != configured.profileId
            || resolved.profileName != configured.profileName
            || resolved.outputSize != expectedSize
            || resolved.resampleMethod != configured.resize.method
            || resolved.resizeRequired != (expectedSize != plan.documentSize)) {
            setError(errorMessage, QStringLiteral(
                "The resolved queue payload no longer matches the production plan for output ‘%1’." )
                    .arg(configured.profileName));
            return false;
        }

        ExportNamingContext naming;
        naming.documentName = plan.documentName;
        naming.profileName = configured.profileName;
        naming.formatSuffix = configured.profile.formatSuffix;
        naming.bitDepth = static_cast<int>(configured.profile.bitDepth);
        naming.imageSize = expectedSize;
        naming.workingSpaceName = plan.workingSpaceName;
        naming.outputSpaceName = configured.profile.convertToOutputProfile
            ? configured.profile.output.profile.displayName
            : plan.workingSpaceName;
        naming.timestampUtc = plan.timestampUtc;
        const QString nameTemplate = configured.namingTemplate.trimmed().isEmpty()
            ? configured.profile.namingTemplate : configured.namingTemplate;
        QString namingError;
        const QString expectedStem = ExportNamingTemplate::resolve(
            nameTemplate, naming, &namingError);
        if (expectedStem.isEmpty()) {
            setError(errorMessage, QStringLiteral(
                "The filename template for output ‘%1’ is no longer valid: %2")
                    .arg(configured.profileName, namingError));
            return false;
        }
        const QString expectedPath = QFileInfo(QDir(plan.outputDirectory).filePath(
            QStringLiteral("%1.%2").arg(expectedStem, configured.profile.formatSuffix)))
            .absoluteFilePath();

        const ImageExportRequest &request = resolved.request;
        const QFileInfo pathInfo(request.filePath);
        const QString path = pathInfo.absoluteFilePath();
        const QString parentKey = pathKey(pathInfo.absolutePath());
        const QString fileKey = pathKey(path);
        const bool namingMatches = pathKey(path) == pathKey(expectedPath)
            || (plan.collisionPolicy
                    == ProductionExportCollisionPolicy::AutoRename
                && isAutoRenameSiblingPath(expectedPath, path));
        if (request.filePath.trimmed().isEmpty() || !pathInfo.isAbsolute()
            || parentKey != outputDirectoryKey || seenPaths.contains(fileKey)
            || pathInfo.suffix().toLower() != configured.profile.formatSuffix
            || !namingMatches) {
            setError(errorMessage, QStringLiteral(
                "The resolved path for output ‘%1’ is outside the selected directory, duplicated, or inconsistent with its filename template." )
                    .arg(configured.profileName));
            return false;
        }
        seenPaths.insert(fileKey);

        ImageExportRequest expectedRequest;
        expectedRequest.filePath = request.filePath;
        expectedRequest.quality = configured.profile.quality;
        expectedRequest.bitDepth = configured.profile.bitDepth;
        expectedRequest.dither = configured.profile.dither;
        expectedRequest.alphaMode = configured.profile.alphaMode;
        expectedRequest.convertToOutputProfile =
            configured.profile.convertToOutputProfile;
        expectedRequest.output = configured.profile.output;
        expectedRequest.matteColour = configured.profile.matteColour;
        if (request.quality != expectedRequest.quality
            || request.bitDepth != expectedRequest.bitDepth
            || request.dither != expectedRequest.dither
            || request.alphaMode != expectedRequest.alphaMode
            || request.convertToOutputProfile
                != expectedRequest.convertToOutputProfile
            || request.output != expectedRequest.output
            || request.matteColour != expectedRequest.matteColour
            || request.ditherSeed != expectedRequest.ditherSeed
            || (resolved.skipExisting
                && plan.collisionPolicy
                    != ProductionExportCollisionPolicy::SkipExisting)
            || (resolved.skipExisting && !resolved.existedAtPreflight)) {
            setError(errorMessage, QStringLiteral(
                "The resolved encoding settings for output ‘%1’ no longer match its captured export profile." )
                    .arg(configured.profileName));
            return false;
        }

        QString requestError;
        if (!validateImageExportRequest(request, colourState, &requestError)) {
            setError(errorMessage, QStringLiteral(
                "The resolved queue payload for output ‘%1’ is invalid: %2")
                    .arg(configured.profileName, requestError));
            return false;
        }
        const ImageExportCapabilities capabilities =
            imageExportCapabilitiesForPath(request.filePath);
        if (!imageExportWriterAvailable(capabilities)) {
            setError(errorMessage, QStringLiteral(
                "The image writer required by output ‘%1’ is unavailable." )
                    .arg(configured.profileName));
            return false;
        }
    }
    return true;
}

} // namespace vfx
