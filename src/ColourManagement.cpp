#include "ColourManagement.h"

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vfx {
namespace {

constexpr qsizetype MaximumIccProfileBytes = 16 * 1024 * 1024;
constexpr qsizetype MaximumDescriptorTextBytes = 1024 * 1024;
constexpr qsizetype MaximumColourStateJsonBytes = 64 * 1024 * 1024;

bool embeddedIccMatchesFingerprint(const QByteArray &profile,
                                   const QByteArray &fingerprint)
{
    return profile.isEmpty()
        || (fingerprint.size() == 32
            && QCryptographicHash::hash(profile, QCryptographicHash::Sha256)
                == fingerprint);
}

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

void appendRawField(QCryptographicHash &hash,
                    const char *data,
                    const qsizetype length)
{
    const quint64 size = static_cast<quint64>(std::max<qsizetype>(0, length));
    char header[8];
    for (int index = 0; index < 8; ++index) {
        header[index] = static_cast<char>((size >> (index * 8)) & 0xffU);
    }
    hash.addData(QByteArrayView(header, sizeof(header)));
    if (length > 0) {
        hash.addData(QByteArrayView(data, length));
    }
}

void appendField(QCryptographicHash &hash, const QString &value)
{
    const QByteArray encoded = value.toUtf8();
    appendRawField(hash, encoded.constData(), encoded.size());
}

void appendField(QCryptographicHash &hash, const QByteArray &value)
{
    appendRawField(hash, value.constData(), value.size());
}

void appendField(QCryptographicHash &hash, const quint64 value)
{
    char bytes[8];
    for (int index = 0; index < 8; ++index) {
        bytes[index] = static_cast<char>((value >> (index * 8)) & 0xffU);
    }
    appendRawField(hash, bytes, sizeof(bytes));
}

void appendField(QCryptographicHash &hash, const bool value)
{
    const char byte = value ? 1 : 0;
    appendRawField(hash, &byte, 1);
}

QByteArray finishFingerprint(QCryptographicHash &hash)
{
    return hash.result();
}

QString colourSpaceKindToken(const ColourSpaceKind kind)
{
    switch (kind) {
    case ColourSpaceKind::Untagged: return QStringLiteral("untagged");
    case ColourSpaceKind::BuiltIn: return QStringLiteral("built-in");
    case ColourSpaceKind::EmbeddedIcc: return QStringLiteral("embedded-icc");
    case ColourSpaceKind::ExternalIcc: return QStringLiteral("external-icc");
    case ColourSpaceKind::Ocio: return QStringLiteral("ocio");
    }
    return QStringLiteral("untagged");
}

std::optional<ColourSpaceKind> colourSpaceKindFromToken(const QString &token)
{
    if (token == QStringLiteral("untagged")) return ColourSpaceKind::Untagged;
    if (token == QStringLiteral("built-in")) return ColourSpaceKind::BuiltIn;
    if (token == QStringLiteral("embedded-icc")) return ColourSpaceKind::EmbeddedIcc;
    if (token == QStringLiteral("external-icc")) return ColourSpaceKind::ExternalIcc;
    if (token == QStringLiteral("ocio")) return ColourSpaceKind::Ocio;
    return std::nullopt;
}

QString builtInToken(const BuiltInColourSpace space)
{
    switch (space) {
    case BuiltInColourSpace::None: return QStringLiteral("none");
    case BuiltInColourSpace::SRgb: return QStringLiteral("srgb");
    case BuiltInColourSpace::LinearSRgb: return QStringLiteral("linear-srgb");
    case BuiltInColourSpace::DisplayP3: return QStringLiteral("display-p3");
    case BuiltInColourSpace::AdobeRgb: return QStringLiteral("adobe-rgb");
    case BuiltInColourSpace::ProPhotoRgb: return QStringLiteral("prophoto-rgb");
    }
    return QStringLiteral("none");
}

std::optional<BuiltInColourSpace> builtInFromToken(const QString &token)
{
    if (token == QStringLiteral("none")) return BuiltInColourSpace::None;
    if (token == QStringLiteral("srgb")) return BuiltInColourSpace::SRgb;
    if (token == QStringLiteral("linear-srgb")) return BuiltInColourSpace::LinearSRgb;
    if (token == QStringLiteral("display-p3")) return BuiltInColourSpace::DisplayP3;
    if (token == QStringLiteral("adobe-rgb")) return BuiltInColourSpace::AdobeRgb;
    if (token == QStringLiteral("prophoto-rgb")) return BuiltInColourSpace::ProPhotoRgb;
    return std::nullopt;
}

QString defaultBuiltInName(const BuiltInColourSpace space)
{
    switch (space) {
    case BuiltInColourSpace::SRgb: return QStringLiteral("sRGB");
    case BuiltInColourSpace::LinearSRgb: return QStringLiteral("Linear sRGB");
    case BuiltInColourSpace::DisplayP3: return QStringLiteral("Display P3");
    case BuiltInColourSpace::AdobeRgb: return QStringLiteral("Adobe RGB");
    case BuiltInColourSpace::ProPhotoRgb: return QStringLiteral("ProPhoto RGB");
    case BuiltInColourSpace::None: break;
    }
    return QStringLiteral("Untagged");
}

QColorSpace qtBuiltInSpace(const BuiltInColourSpace space)
{
    switch (space) {
    case BuiltInColourSpace::SRgb:
        return QColorSpace(QColorSpace::SRgb);
    case BuiltInColourSpace::LinearSRgb:
        return QColorSpace(QColorSpace::SRgbLinear);
    case BuiltInColourSpace::DisplayP3:
        return QColorSpace(QColorSpace::DisplayP3);
    case BuiltInColourSpace::AdobeRgb:
        return QColorSpace(QColorSpace::AdobeRgb);
    case BuiltInColourSpace::ProPhotoRgb:
        return QColorSpace(QColorSpace::ProPhotoRgb);
    case BuiltInColourSpace::None:
        return {};
    }
    return {};
}

QString processingToken(const ColourProcessingCompatibility compatibility)
{
    return compatibility == ColourProcessingCompatibility::LegacyV1
        ? QStringLiteral("legacy-v1")
        : QStringLiteral("managed-v1");
}


QString untaggedPolicyTokenInternal(const UntaggedImagePolicy policy)
{
    switch (policy) {
    case UntaggedImagePolicy::Ask: return QStringLiteral("ask");
    case UntaggedImagePolicy::AssumeSRgb: return QStringLiteral("assume-srgb");
    case UntaggedImagePolicy::LeaveUntagged: return QStringLiteral("leave-untagged");
    }
    return QStringLiteral("assume-srgb");
}

std::optional<UntaggedImagePolicy> untaggedPolicyFromTokenInternal(const QString &token)
{
    if (token == QStringLiteral("ask")) return UntaggedImagePolicy::Ask;
    if (token == QStringLiteral("assume-srgb")) return UntaggedImagePolicy::AssumeSRgb;
    if (token == QStringLiteral("leave-untagged")) return UntaggedImagePolicy::LeaveUntagged;
    return std::nullopt;
}

QString inputProfileStatusToken(const InputProfileStatus status)
{
    switch (status) {
    case InputProfileStatus::LegacyUnknown: return QStringLiteral("legacy-unknown");
    case InputProfileStatus::Generated: return QStringLiteral("generated");
    case InputProfileStatus::EmbeddedValid: return QStringLiteral("embedded-valid");
    case InputProfileStatus::ClipboardValid: return QStringLiteral("clipboard-valid");
    case InputProfileStatus::Untagged: return QStringLiteral("untagged");
    case InputProfileStatus::InvalidOrUnsupported: return QStringLiteral("invalid-or-unsupported");
    }
    return QStringLiteral("legacy-unknown");
}

std::optional<InputProfileStatus> inputProfileStatusFromToken(const QString &token)
{
    if (token == QStringLiteral("legacy-unknown")) return InputProfileStatus::LegacyUnknown;
    if (token == QStringLiteral("generated")) return InputProfileStatus::Generated;
    if (token == QStringLiteral("embedded-valid")) return InputProfileStatus::EmbeddedValid;
    if (token == QStringLiteral("clipboard-valid")) return InputProfileStatus::ClipboardValid;
    if (token == QStringLiteral("untagged")) return InputProfileStatus::Untagged;
    if (token == QStringLiteral("invalid-or-unsupported")) return InputProfileStatus::InvalidOrUnsupported;
    return std::nullopt;
}

std::optional<ColourProcessingCompatibility> processingFromToken(const QString &token)
{
    if (token == QStringLiteral("legacy-v1")) {
        return ColourProcessingCompatibility::LegacyV1;
    }
    if (token == QStringLiteral("managed-v1")) {
        return ColourProcessingCompatibility::ManagedV1;
    }
    return std::nullopt;
}

QString displayKindToken(const DisplayTransformKind kind)
{
    switch (kind) {
    case DisplayTransformKind::Disabled: return QStringLiteral("disabled");
    case DisplayTransformKind::SystemIcc: return QStringLiteral("system-icc");
    case DisplayTransformKind::IccProfile: return QStringLiteral("icc-profile");
    case DisplayTransformKind::OcioView: return QStringLiteral("ocio-view");
    }
    return QStringLiteral("disabled");
}

std::optional<DisplayTransformKind> displayKindFromToken(const QString &token)
{
    if (token == QStringLiteral("disabled")) return DisplayTransformKind::Disabled;
    if (token == QStringLiteral("system-icc")) return DisplayTransformKind::SystemIcc;
    if (token == QStringLiteral("icc-profile")) return DisplayTransformKind::IccProfile;
    if (token == QStringLiteral("ocio-view")) return DisplayTransformKind::OcioView;
    return std::nullopt;
}

QString renderingIntentToken(const ColourRenderingIntent intent)
{
    switch (intent) {
    case ColourRenderingIntent::Perceptual: return QStringLiteral("perceptual");
    case ColourRenderingIntent::RelativeColorimetric: return QStringLiteral("relative-colorimetric");
    case ColourRenderingIntent::Saturation: return QStringLiteral("saturation");
    case ColourRenderingIntent::AbsoluteColorimetric: return QStringLiteral("absolute-colorimetric");
    }
    return QStringLiteral("relative-colorimetric");
}

std::optional<ColourRenderingIntent> renderingIntentFromToken(const QString &token)
{
    if (token == QStringLiteral("perceptual")) return ColourRenderingIntent::Perceptual;
    if (token == QStringLiteral("relative-colorimetric")) {
        return ColourRenderingIntent::RelativeColorimetric;
    }
    if (token == QStringLiteral("saturation")) return ColourRenderingIntent::Saturation;
    if (token == QStringLiteral("absolute-colorimetric")) {
        return ColourRenderingIntent::AbsoluteColorimetric;
    }
    return std::nullopt;
}

QString ocioConfigSourceToken(const OcioConfigSource source)
{
    switch (source) {
    case OcioConfigSource::None: return QStringLiteral("none");
    case OcioConfigSource::BuiltIn: return QStringLiteral("built-in");
    case OcioConfigSource::ExternalFile: return QStringLiteral("external-file");
    case OcioConfigSource::Environment: return QStringLiteral("environment");
    }
    return QStringLiteral("none");
}

std::optional<OcioConfigSource> ocioConfigSourceFromToken(const QString &token)
{
    if (token == QStringLiteral("none")) return OcioConfigSource::None;
    if (token == QStringLiteral("built-in")) return OcioConfigSource::BuiltIn;
    if (token == QStringLiteral("external-file")) return OcioConfigSource::ExternalFile;
    if (token == QStringLiteral("environment")) return OcioConfigSource::Environment;
    return std::nullopt;
}

QString transformPurposeToken(const ColourTransformPurpose purpose)
{
    switch (purpose) {
    case ColourTransformPurpose::InputToWorking: return QStringLiteral("input-to-working");
    case ColourTransformPurpose::WorkingToWorking: return QStringLiteral("working-to-working");
    case ColourTransformPurpose::WorkingToDisplay: return QStringLiteral("working-to-display");
    case ColourTransformPurpose::WorkingToProof: return QStringLiteral("working-to-proof");
    case ColourTransformPurpose::WorkingToOutput: return QStringLiteral("working-to-output");
    case ColourTransformPurpose::AdjustmentDomain: return QStringLiteral("adjustment-domain");
    }
    return QStringLiteral("adjustment-domain");
}

bool safeText(const QString &value)
{
    return value.toUtf8().size() <= MaximumDescriptorTextBytes;
}

QByteArray decodedBase64Field(const QJsonObject &object,
                              const QString &name,
                              bool *ok)
{
    const QJsonValue value = object.value(name);
    if (value.isUndefined()) {
        return {};
    }
    if (!value.isString()) {
        *ok = false;
        return {};
    }
    const QByteArray encoded = value.toString().toLatin1();
    const QByteArray decoded = QByteArray::fromBase64(encoded,
        QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.isNull() && !encoded.isEmpty()) {
        *ok = false;
    }
    return decoded;
}

quint64 revisionFromJson(const QJsonValue &value, bool *ok)
{
    if (value.isString()) {
        bool converted = false;
        const qulonglong revision = value.toString().toULongLong(&converted);
        if (converted && revision > 0) {
            return revision;
        }
    } else if (value.isDouble()) {
        const double numeric = value.toDouble();
        if (numeric >= 1.0 && numeric <= static_cast<double>(std::numeric_limits<quint64>::max())
            && std::floor(numeric) == numeric) {
            return static_cast<quint64>(numeric);
        }
    }
    *ok = false;
    return 1;
}

} // namespace

OcioConfigReference OcioConfigReference::disabled()
{
    return {};
}

bool OcioConfigReference::isConfigured() const
{
    return source != OcioConfigSource::None;
}

bool OcioConfigReference::isSafe(QString *errorMessage) const
{
    if (!safeText(identifier) || !safeText(canonicalPath)
        || !safeText(displayName) || !safeText(version)
        || !safeText(iccBridgeSpace)
        || (!fingerprint.isEmpty() && fingerprint.size() != 32)) {
        setError(errorMessage, QStringLiteral("The OCIO configuration reference exceeds its safety limits."));
        return false;
    }
    if (source == OcioConfigSource::None) {
        if (!identifier.isEmpty() || !canonicalPath.isEmpty()
            || !fingerprint.isEmpty() || !iccBridgeSpace.isEmpty()) {
            setError(errorMessage, QStringLiteral("A disabled OCIO configuration contains unexpected data."));
            return false;
        }
        return true;
    }
    if (identifier.trimmed().isEmpty() || fingerprint.size() != 32) {
        setError(errorMessage, QStringLiteral("The OCIO configuration reference is incomplete."));
        return false;
    }
    if (source == OcioConfigSource::ExternalFile
        && canonicalPath.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("The external OCIO configuration path is missing."));
        return false;
    }
    return true;
}

QByteArray OcioConfigReference::stableFingerprint() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    appendField(hash, QStringLiteral("VFXPhotoLab/OcioConfigReference/v1"));
    appendField(hash, static_cast<quint64>(source));
    appendField(hash, identifier);
    appendField(hash, canonicalPath);
    appendField(hash, displayName);
    appendField(hash, version);
    appendField(hash, fingerprint);
    appendField(hash, iccBridgeSpace);
    return finishFingerprint(hash);
}

qint64 OcioConfigReference::estimatedBytes() const
{
    return static_cast<qint64>(sizeof(OcioConfigReference))
        + identifier.toUtf8().size() + canonicalPath.toUtf8().size()
        + displayName.toUtf8().size() + version.toUtf8().size()
        + fingerprint.size() + iccBridgeSpace.toUtf8().size();
}

QJsonObject OcioConfigReference::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("source"), ocioConfigSourceToken(source));
    object.insert(QStringLiteral("identifier"), identifier);
    object.insert(QStringLiteral("canonicalPath"), canonicalPath);
    object.insert(QStringLiteral("displayName"), displayName);
    object.insert(QStringLiteral("version"), version);
    object.insert(QStringLiteral("fingerprint"),
                  QString::fromLatin1(fingerprint.toHex()));
    object.insert(QStringLiteral("iccBridgeSpace"), iccBridgeSpace);
    return object;
}

std::optional<OcioConfigReference> OcioConfigReference::fromJson(
    const QJsonObject &object,
    QString *errorMessage)
{
    const auto source = ocioConfigSourceFromToken(
        object.value(QStringLiteral("source")).toString());
    if (!source) {
        setError(errorMessage, QStringLiteral("The OCIO configuration source is invalid."));
        return std::nullopt;
    }
    OcioConfigReference reference;
    reference.source = *source;
    reference.identifier = object.value(QStringLiteral("identifier")).toString();
    reference.canonicalPath = object.value(QStringLiteral("canonicalPath")).toString();
    reference.displayName = object.value(QStringLiteral("displayName")).toString();
    reference.version = object.value(QStringLiteral("version")).toString();
    reference.fingerprint = QByteArray::fromHex(
        object.value(QStringLiteral("fingerprint")).toString().toLatin1());
    reference.iccBridgeSpace = object.value(
        QStringLiteral("iccBridgeSpace")).toString();
    if (!reference.isSafe(errorMessage)) {
        return std::nullopt;
    }
    return reference;
}

ColourSpaceDescriptor ColourSpaceDescriptor::untagged()
{
    ColourSpaceDescriptor descriptor;
    descriptor.kind = ColourSpaceKind::Untagged;
    descriptor.displayName = QStringLiteral("Untagged");
    return descriptor;
}

ColourSpaceDescriptor ColourSpaceDescriptor::fromQColorSpace(
    const QColorSpace &colourSpace)
{
    if (!colourSpace.isValid()) {
        return untagged();
    }

    ColourSpaceDescriptor descriptor;
    descriptor.kind = ColourSpaceKind::BuiltIn;
    if (colourSpace == QColorSpace(QColorSpace::SRgb)) {
        descriptor.builtIn = BuiltInColourSpace::SRgb;
    } else if (colourSpace == QColorSpace(QColorSpace::SRgbLinear)) {
        descriptor.builtIn = BuiltInColourSpace::LinearSRgb;
    } else if (colourSpace == QColorSpace(QColorSpace::DisplayP3)) {
        descriptor.builtIn = BuiltInColourSpace::DisplayP3;
    } else if (colourSpace == QColorSpace(QColorSpace::AdobeRgb)) {
        descriptor.builtIn = BuiltInColourSpace::AdobeRgb;
    } else if (colourSpace == QColorSpace(QColorSpace::ProPhotoRgb)) {
        descriptor.builtIn = BuiltInColourSpace::ProPhotoRgb;
    } else {
        const QByteArray profile = colourSpace.iccProfile();
        if (profile.isEmpty()) {
            return untagged();
        }
        return embeddedIcc(profile, colourSpace.description());
    }
    descriptor.displayName = colourSpace.description().trimmed();
    if (descriptor.displayName.isEmpty()) {
        if (descriptor.kind == ColourSpaceKind::BuiltIn) {
            descriptor.displayName = defaultBuiltInName(descriptor.builtIn);
        } else {
            descriptor.displayName = QStringLiteral("Embedded ICC Profile");
        }
    }
    return descriptor;
}

ColourSpaceDescriptor ColourSpaceDescriptor::embeddedIcc(
    const QByteArray &profile,
    const QString &name)
{
    ColourSpaceDescriptor descriptor;
    descriptor.kind = ColourSpaceKind::EmbeddedIcc;
    descriptor.iccProfile = profile;
    descriptor.displayName = name.trimmed();
    if (descriptor.displayName.isEmpty()) {
        descriptor.displayName = QColorSpace::fromIccProfile(profile)
            .description().trimmed();
    }
    if (descriptor.displayName.isEmpty()) {
        descriptor.displayName = QStringLiteral("Embedded ICC Profile");
    }
    return descriptor;
}

ColourSpaceDescriptor ColourSpaceDescriptor::externalIcc(
    const QString &path,
    const QByteArray &fileFingerprint,
    const QByteArray &profile,
    const QString &name)
{
    ColourSpaceDescriptor descriptor;
    descriptor.kind = ColourSpaceKind::ExternalIcc;
    descriptor.externalPath = path.trimmed();
    descriptor.externalFingerprint = fileFingerprint;
    descriptor.iccProfile = profile;
    descriptor.displayName = name.trimmed();
    if (descriptor.displayName.isEmpty()) {
        const QColorSpace colourSpace = descriptor.toQColorSpace();
        descriptor.displayName = colourSpace.description().trimmed();
    }
    if (descriptor.displayName.isEmpty()) {
        descriptor.displayName = QStringLiteral("External ICC Profile");
    }
    return descriptor;
}

ColourSpaceDescriptor ColourSpaceDescriptor::ocio(const QString &configId,
                                                   const QByteArray &configFingerprint,
                                                   const QString &space,
                                                   const QString &name)
{
    ColourSpaceDescriptor descriptor;
    descriptor.kind = ColourSpaceKind::Ocio;
    descriptor.ocioConfigId = configId.trimmed();
    descriptor.ocioConfigFingerprint = configFingerprint;
    descriptor.ocioSpace = space.trimmed();
    descriptor.displayName = name.trimmed().isEmpty()
        ? descriptor.ocioSpace : name.trimmed();
    return descriptor;
}

bool ColourSpaceDescriptor::isValid() const
{
    if (!safeText(displayName) || !safeText(externalPath)
        || !safeText(ocioConfigId) || !safeText(ocioSpace)
        || iccProfile.size() > MaximumIccProfileBytes
        || externalFingerprint.size() > 1024
        || (!ocioConfigFingerprint.isEmpty() && ocioConfigFingerprint.size() != 32)) {
        return false;
    }
    switch (kind) {
    case ColourSpaceKind::Untagged:
        return builtIn == BuiltInColourSpace::None && iccProfile.isEmpty()
            && externalPath.isEmpty() && externalFingerprint.isEmpty()
            && ocioConfigId.isEmpty() && ocioConfigFingerprint.isEmpty()
            && ocioSpace.isEmpty();
    case ColourSpaceKind::BuiltIn:
        return builtIn != BuiltInColourSpace::None
            && qtBuiltInSpace(builtIn).isValid()
            && iccProfile.isEmpty()
            && externalPath.isEmpty() && externalFingerprint.isEmpty()
            && ocioConfigId.isEmpty() && ocioConfigFingerprint.isEmpty()
            && ocioSpace.isEmpty();
    case ColourSpaceKind::EmbeddedIcc:
        return builtIn == BuiltInColourSpace::None
            && !iccProfile.isEmpty()
            && QColorSpace::fromIccProfile(iccProfile).isValid()
            && externalPath.isEmpty() && externalFingerprint.isEmpty()
            && ocioConfigId.isEmpty() && ocioConfigFingerprint.isEmpty()
            && ocioSpace.isEmpty();
    case ColourSpaceKind::ExternalIcc:
        return builtIn == BuiltInColourSpace::None
            && !externalPath.trimmed().isEmpty()
            && externalFingerprint.size() == 32
            && (iccProfile.isEmpty() || QColorSpace::fromIccProfile(iccProfile).isValid())
            && embeddedIccMatchesFingerprint(iccProfile, externalFingerprint)
            && ocioConfigId.isEmpty() && ocioConfigFingerprint.isEmpty()
            && ocioSpace.isEmpty();
    case ColourSpaceKind::Ocio:
        return builtIn == BuiltInColourSpace::None
            && iccProfile.isEmpty()
            && externalPath.isEmpty() && externalFingerprint.isEmpty()
            && !ocioConfigId.trimmed().isEmpty()
            && !ocioConfigFingerprint.isEmpty()
            && !ocioSpace.trimmed().isEmpty();
    }
    return false;
}

bool ColourSpaceDescriptor::isUntagged() const
{
    return kind == ColourSpaceKind::Untagged;
}

QColorSpace ColourSpaceDescriptor::toQColorSpace() const
{
    if (kind == ColourSpaceKind::BuiltIn) {
        return qtBuiltInSpace(builtIn);
    }
    if ((kind == ColourSpaceKind::EmbeddedIcc || kind == ColourSpaceKind::ExternalIcc)
        && !iccProfile.isEmpty()) {
        return QColorSpace::fromIccProfile(iccProfile);
    }
    return {};
}

QByteArray ColourSpaceDescriptor::stableFingerprint() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    appendField(hash, QStringLiteral("VFXPhotoLab/ColourSpaceDescriptor/v1"));
    appendField(hash, static_cast<quint64>(kind));
    appendField(hash, static_cast<quint64>(builtIn));
    appendField(hash, iccProfile);
    appendField(hash, externalPath);
    appendField(hash, externalFingerprint);
    appendField(hash, ocioConfigId);
    appendField(hash, ocioConfigFingerprint);
    appendField(hash, ocioSpace);
    return finishFingerprint(hash);
}

qint64 ColourSpaceDescriptor::estimatedBytes() const
{
    return static_cast<qint64>(sizeof(ColourSpaceDescriptor))
        + displayName.size() * 2LL + iccProfile.size()
        + externalPath.size() * 2LL + externalFingerprint.size()
        + ocioConfigId.size() * 2LL + ocioConfigFingerprint.size()
        + ocioSpace.size() * 2LL;
}

QJsonObject ColourSpaceDescriptor::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), colourSpaceKindToken(kind));
    object.insert(QStringLiteral("name"), displayName);
    if (kind == ColourSpaceKind::BuiltIn) {
        object.insert(QStringLiteral("builtIn"), builtInToken(builtIn));
    }
    if (!iccProfile.isEmpty()) {
        object.insert(QStringLiteral("iccProfile"),
                      QString::fromLatin1(iccProfile.toBase64()));
    }
    if (!externalPath.isEmpty()) {
        object.insert(QStringLiteral("externalPath"), externalPath);
    }
    if (!externalFingerprint.isEmpty()) {
        object.insert(QStringLiteral("externalFingerprint"),
                      QString::fromLatin1(externalFingerprint.toHex()));
    }
    if (!ocioConfigId.isEmpty()) {
        object.insert(QStringLiteral("ocioConfigId"), ocioConfigId);
    }
    if (!ocioConfigFingerprint.isEmpty()) {
        object.insert(QStringLiteral("ocioConfigFingerprint"),
                      QString::fromLatin1(ocioConfigFingerprint.toHex()));
    }
    if (!ocioSpace.isEmpty()) {
        object.insert(QStringLiteral("ocioSpace"), ocioSpace);
    }
    return object;
}

std::optional<ColourSpaceDescriptor> ColourSpaceDescriptor::fromJson(
    const QJsonObject &object,
    QString *errorMessage)
{
    const auto kind = colourSpaceKindFromToken(object.value(QStringLiteral("kind")).toString());
    if (!kind) {
        setError(errorMessage, QStringLiteral("The colour-space descriptor has an unknown kind."));
        return std::nullopt;
    }

    ColourSpaceDescriptor descriptor;
    descriptor.kind = *kind;
    descriptor.displayName = object.value(QStringLiteral("name")).toString();
    if (descriptor.kind == ColourSpaceKind::BuiltIn) {
        const auto builtIn = builtInFromToken(
            object.value(QStringLiteral("builtIn")).toString());
        if (!builtIn || *builtIn == BuiltInColourSpace::None) {
            setError(errorMessage, QStringLiteral("The colour-space descriptor has an unknown built-in space."));
            return std::nullopt;
        }
        descriptor.builtIn = *builtIn;
    }

    bool bytesOk = true;
    descriptor.iccProfile = decodedBase64Field(object, QStringLiteral("iccProfile"), &bytesOk);
    descriptor.externalPath = object.value(QStringLiteral("externalPath")).toString();
    descriptor.externalFingerprint = QByteArray::fromHex(
        object.value(QStringLiteral("externalFingerprint")).toString().toLatin1());
    descriptor.ocioConfigId = object.value(QStringLiteral("ocioConfigId")).toString();
    descriptor.ocioConfigFingerprint = QByteArray::fromHex(
        object.value(QStringLiteral("ocioConfigFingerprint")).toString().toLatin1());
    descriptor.ocioSpace = object.value(QStringLiteral("ocioSpace")).toString();
    if (!bytesOk || !descriptor.isValid()) {
        setError(errorMessage, QStringLiteral("The colour-space descriptor is invalid or exceeds its safety limits."));
        return std::nullopt;
    }
    if (descriptor.displayName.trimmed().isEmpty()) {
        if (descriptor.kind == ColourSpaceKind::BuiltIn) {
            descriptor.displayName = defaultBuiltInName(descriptor.builtIn);
        } else if (descriptor.kind == ColourSpaceKind::Ocio) {
            descriptor.displayName = descriptor.ocioSpace;
        } else if (descriptor.kind == ColourSpaceKind::Untagged) {
            descriptor.displayName = QStringLiteral("Untagged");
        } else {
            descriptor.displayName = descriptor.toQColorSpace().description().trimmed();
            if (descriptor.displayName.isEmpty()) {
                descriptor.displayName = descriptor.kind == ColourSpaceKind::ExternalIcc
                    ? QStringLiteral("External ICC Profile")
                    : QStringLiteral("Embedded ICC Profile");
            }
        }
    }
    return descriptor;
}

QByteArray DisplayTransformDescriptor::stableFingerprint() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    appendField(hash, QStringLiteral("VFXPhotoLab/DisplayTransformDescriptor/v1"));
    appendField(hash, static_cast<quint64>(kind));
    appendField(hash, profile.stableFingerprint());
    appendField(hash, ocioConfigId);
    appendField(hash, ocioConfigFingerprint);
    appendField(hash, ocioDisplay);
    appendField(hash, ocioView);
    appendField(hash, ocioLook);
    return finishFingerprint(hash);
}

qint64 DisplayTransformDescriptor::estimatedBytes() const
{
    return static_cast<qint64>(sizeof(DisplayTransformDescriptor))
        + profile.estimatedBytes() + ocioConfigId.size() * 2LL
        + ocioConfigFingerprint.size() + ocioDisplay.size() * 2LL
        + ocioView.size() * 2LL + ocioLook.size() * 2LL;
}

QJsonObject DisplayTransformDescriptor::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("kind"), displayKindToken(kind));
    object.insert(QStringLiteral("profile"), profile.toJson());
    object.insert(QStringLiteral("ocioConfigId"), ocioConfigId);
    object.insert(QStringLiteral("ocioConfigFingerprint"),
                  QString::fromLatin1(ocioConfigFingerprint.toHex()));
    object.insert(QStringLiteral("ocioDisplay"), ocioDisplay);
    object.insert(QStringLiteral("ocioView"), ocioView);
    object.insert(QStringLiteral("ocioLook"), ocioLook);
    return object;
}

std::optional<DisplayTransformDescriptor> DisplayTransformDescriptor::fromJson(
    const QJsonObject &object,
    QString *errorMessage)
{
    const auto kind = displayKindFromToken(object.value(QStringLiteral("kind")).toString());
    if (!kind || !object.value(QStringLiteral("profile")).isObject()) {
        setError(errorMessage, QStringLiteral("The display-transform descriptor is invalid."));
        return std::nullopt;
    }
    const auto profile = ColourSpaceDescriptor::fromJson(
        object.value(QStringLiteral("profile")).toObject(), errorMessage);
    if (!profile) {
        return std::nullopt;
    }
    DisplayTransformDescriptor descriptor;
    descriptor.kind = *kind;
    descriptor.profile = *profile;
    descriptor.ocioConfigId = object.value(QStringLiteral("ocioConfigId")).toString();
    descriptor.ocioConfigFingerprint = QByteArray::fromHex(
        object.value(QStringLiteral("ocioConfigFingerprint")).toString().toLatin1());
    descriptor.ocioDisplay = object.value(QStringLiteral("ocioDisplay")).toString();
    descriptor.ocioView = object.value(QStringLiteral("ocioView")).toString();
    descriptor.ocioLook = object.value(QStringLiteral("ocioLook")).toString();
    if (!safeText(descriptor.ocioConfigId) || !safeText(descriptor.ocioDisplay)
        || !safeText(descriptor.ocioView) || !safeText(descriptor.ocioLook)
        || (!descriptor.ocioConfigFingerprint.isEmpty()
            && descriptor.ocioConfigFingerprint.size() != 32)) {
        setError(errorMessage, QStringLiteral("The display-transform descriptor exceeds its safety limits."));
        return std::nullopt;
    }
    if (descriptor.kind == DisplayTransformKind::IccProfile
        && descriptor.profile.toQColorSpace().isValid() == false) {
        setError(errorMessage, QStringLiteral("The display ICC profile is invalid."));
        return std::nullopt;
    }
    if (descriptor.kind == DisplayTransformKind::OcioView
        && (descriptor.ocioConfigId.trimmed().isEmpty()
            || descriptor.ocioConfigFingerprint.isEmpty()
            || descriptor.ocioDisplay.trimmed().isEmpty()
            || descriptor.ocioView.trimmed().isEmpty())) {
        setError(errorMessage, QStringLiteral("The OCIO display/view selection is incomplete."));
        return std::nullopt;
    }
    return descriptor;
}

QByteArray ProofingColourSettings::stableFingerprint() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    appendField(hash, QStringLiteral("VFXPhotoLab/ProofingColourSettings/v1"));
    appendField(hash, enabled);
    appendField(hash, profile.stableFingerprint());
    appendField(hash, static_cast<quint64>(renderingIntent));
    appendField(hash, blackPointCompensation);
    appendField(hash, gamutWarning);
    return finishFingerprint(hash);
}

qint64 ProofingColourSettings::estimatedBytes() const
{
    return static_cast<qint64>(sizeof(ProofingColourSettings)) + profile.estimatedBytes();
}

QJsonObject ProofingColourSettings::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("enabled"), enabled);
    object.insert(QStringLiteral("profile"), profile.toJson());
    object.insert(QStringLiteral("renderingIntent"), renderingIntentToken(renderingIntent));
    object.insert(QStringLiteral("blackPointCompensation"), blackPointCompensation);
    object.insert(QStringLiteral("gamutWarning"), gamutWarning);
    return object;
}

std::optional<ProofingColourSettings> ProofingColourSettings::fromJson(
    const QJsonObject &object,
    QString *errorMessage)
{
    if (!object.value(QStringLiteral("profile")).isObject()) {
        setError(errorMessage, QStringLiteral("The proofing settings have no profile descriptor."));
        return std::nullopt;
    }
    const auto profile = ColourSpaceDescriptor::fromJson(
        object.value(QStringLiteral("profile")).toObject(), errorMessage);
    const auto intent = renderingIntentFromToken(
        object.value(QStringLiteral("renderingIntent")).toString());
    if (!profile || !intent) {
        if (!intent) {
            setError(errorMessage, QStringLiteral("The proofing settings have an unknown rendering intent."));
        }
        return std::nullopt;
    }
    ProofingColourSettings settings;
    settings.enabled = object.value(QStringLiteral("enabled")).toBool(false);
    settings.profile = *profile;
    settings.renderingIntent = *intent;
    settings.blackPointCompensation = object.value(
        QStringLiteral("blackPointCompensation")).toBool(true);
    settings.gamutWarning = object.value(QStringLiteral("gamutWarning")).toBool(false);
    return settings;
}

QByteArray OutputColourSettings::stableFingerprint() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    appendField(hash, QStringLiteral("VFXPhotoLab/OutputColourSettings/v1"));
    appendField(hash, profile.stableFingerprint());
    appendField(hash, static_cast<quint64>(renderingIntent));
    appendField(hash, blackPointCompensation);
    appendField(hash, embedProfile);
    return finishFingerprint(hash);
}

qint64 OutputColourSettings::estimatedBytes() const
{
    return static_cast<qint64>(sizeof(OutputColourSettings)) + profile.estimatedBytes();
}

QJsonObject OutputColourSettings::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("profile"), profile.toJson());
    object.insert(QStringLiteral("renderingIntent"), renderingIntentToken(renderingIntent));
    object.insert(QStringLiteral("blackPointCompensation"), blackPointCompensation);
    object.insert(QStringLiteral("embedProfile"), embedProfile);
    return object;
}

std::optional<OutputColourSettings> OutputColourSettings::fromJson(
    const QJsonObject &object,
    QString *errorMessage)
{
    if (!object.value(QStringLiteral("profile")).isObject()) {
        setError(errorMessage, QStringLiteral("The output settings have no profile descriptor."));
        return std::nullopt;
    }
    const auto profile = ColourSpaceDescriptor::fromJson(
        object.value(QStringLiteral("profile")).toObject(), errorMessage);
    const auto intent = renderingIntentFromToken(
        object.value(QStringLiteral("renderingIntent")).toString());
    if (!profile || !intent) {
        if (!intent) {
            setError(errorMessage, QStringLiteral("The output settings have an unknown rendering intent."));
        }
        return std::nullopt;
    }
    OutputColourSettings settings;
    settings.profile = *profile;
    settings.renderingIntent = *intent;
    settings.blackPointCompensation = object.value(
        QStringLiteral("blackPointCompensation")).toBool(true);
    settings.embedProfile = object.value(QStringLiteral("embedProfile")).toBool(true);
    return settings;
}

bool ImageColourImportInfo::hasUsableInputProfile() const
{
    return sourceStatus == InputProfileStatus::EmbeddedValid
        || sourceStatus == InputProfileStatus::ClipboardValid
        || sourceStatus == InputProfileStatus::Generated;
}

bool ImageColourImportInfo::requiresUntaggedPolicy() const
{
    return sourceStatus == InputProfileStatus::Untagged
        || sourceStatus == InputProfileStatus::InvalidOrUnsupported;
}

DocumentColourState DocumentColourState::managedForImage(const QColorSpace &colourSpace)
{
    DocumentColourState state;
    state.ocioConfig = OcioConfigReference::disabled();
    state.presentationColourManagementEnabled = true;
    state.inputProfile = ColourSpaceDescriptor::fromQColorSpace(colourSpace);
    state.workingSpace = state.inputProfile;
    state.displayTransform.kind = DisplayTransformKind::SystemIcc;
    state.displayTransform.profile = ColourSpaceDescriptor::untagged();
    state.proofing.profile = ColourSpaceDescriptor::untagged();
    state.output.profile = state.workingSpace;
    state.processingCompatibility = ColourProcessingCompatibility::ManagedV1;
    state.inputProfileStatus = InputProfileStatus::Generated;
    state.untaggedPolicy = UntaggedImagePolicy::AssumeSRgb;
    state.untaggedPolicyApplied = false;
    state.originalInputProfileFingerprint = colourProfileContentFingerprint(colourSpace);
    state.revision = 1;
    return state;
}

DocumentColourState DocumentColourState::managedForImportedImage(
    const QColorSpace &effectiveColourSpace,
    const ImageColourImportInfo &importInfo)
{
    DocumentColourState state = managedForImage(effectiveColourSpace);
    if (!importInfo.originalIccProfile.isEmpty()
        && QColorSpace::fromIccProfile(importInfo.originalIccProfile).isValid()) {
        state.inputProfile = ColourSpaceDescriptor::embeddedIcc(
            importInfo.originalIccProfile,
            QColorSpace::fromIccProfile(importInfo.originalIccProfile).description());
    }
    state.inputProfileStatus = importInfo.sourceStatus;
    state.untaggedPolicy = importInfo.appliedPolicy;
    state.untaggedPolicyApplied = importInfo.policyWasApplied;
    state.originalInputProfileFingerprint = importInfo.originalProfileFingerprint;
    if (state.originalInputProfileFingerprint.isEmpty()
        && effectiveColourSpace.isValid()) {
        state.originalInputProfileFingerprint = colourProfileContentFingerprint(
            effectiveColourSpace);
    }
    return state;
}

DocumentColourState DocumentColourState::legacyForImage(const QColorSpace &colourSpace)
{
    DocumentColourState state = managedForImage(colourSpace);
    state.processingCompatibility = ColourProcessingCompatibility::LegacyV1;
    state.presentationColourManagementEnabled = false;
    state.displayTransform.kind = DisplayTransformKind::Disabled;
    state.inputProfileStatus = InputProfileStatus::LegacyUnknown;
    state.untaggedPolicyApplied = false;
    return state;
}

bool DocumentColourState::isSafe(QString *errorMessage) const
{
    if (!ocioConfig.isSafe(errorMessage)
        || !inputProfile.isValid() || !workingSpace.isValid()
        || !displayTransform.profile.isValid()
        || !proofing.profile.isValid() || !output.profile.isValid()
        || (!originalInputProfileFingerprint.isEmpty()
            && originalInputProfileFingerprint.size() != 32)
        || revision == 0) {
        setError(errorMessage, QStringLiteral("The document colour state contains an invalid colour-space descriptor."));
        return false;
    }
    if (displayTransform.kind == DisplayTransformKind::IccProfile
        && !displayTransform.profile.toQColorSpace().isValid()) {
        setError(errorMessage, QStringLiteral("The document colour state contains an invalid display profile."));
        return false;
    }
    if (displayTransform.kind == DisplayTransformKind::OcioView
        && (displayTransform.ocioConfigId.trimmed().isEmpty()
            || displayTransform.ocioConfigFingerprint.size() != 32
            || displayTransform.ocioDisplay.trimmed().isEmpty()
            || displayTransform.ocioView.trimmed().isEmpty())) {
        setError(errorMessage, QStringLiteral("The document colour state contains an incomplete OCIO display selection."));
        return false;
    }

    const auto descriptorMatchesOcioConfig = [&](const ColourSpaceDescriptor &descriptor) {
        return descriptor.kind != ColourSpaceKind::Ocio
            || (ocioConfig.isConfigured()
                && descriptor.ocioConfigId == ocioConfig.identifier
                && descriptor.ocioConfigFingerprint == ocioConfig.fingerprint);
    };
    if (!descriptorMatchesOcioConfig(inputProfile)
        || !descriptorMatchesOcioConfig(workingSpace)
        || !descriptorMatchesOcioConfig(proofing.profile)
        || !descriptorMatchesOcioConfig(output.profile)) {
        setError(errorMessage, QStringLiteral("An OCIO colour-space descriptor belongs to a different configuration than the document."));
        return false;
    }
    if (displayTransform.kind == DisplayTransformKind::OcioView
        && (!ocioConfig.isConfigured()
            || displayTransform.ocioConfigId != ocioConfig.identifier
            || displayTransform.ocioConfigFingerprint != ocioConfig.fingerprint)) {
        setError(errorMessage, QStringLiteral("The OCIO display/view selection belongs to a different configuration than the document."));
        return false;
    }
    if (proofing.enabled && proofing.profile.isUntagged()) {
        setError(errorMessage, QStringLiteral("Soft proofing is enabled without a proof profile."));
        return false;
    }
    const QByteArray encoded = QJsonDocument(toJson()).toJson(QJsonDocument::Compact);
    if (encoded.size() > MaximumColourStateJsonBytes) {
        setError(errorMessage, QStringLiteral("The document colour state exceeds its storage safety limit."));
        return false;
    }
    return true;
}

namespace {

QByteArray legacyDocumentColourStateFingerprintV1(
    const DocumentColourState &state)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    appendField(hash, QStringLiteral("VFXPhotoLab/DocumentColourState/v1"));
    appendField(hash, state.inputProfile.stableFingerprint());
    appendField(hash, state.workingSpace.stableFingerprint());
    appendField(hash, state.displayTransform.stableFingerprint());
    appendField(hash, state.proofing.stableFingerprint());
    appendField(hash, state.output.stableFingerprint());
    appendField(hash, static_cast<quint64>(state.processingCompatibility));
    return finishFingerprint(hash);
}

QByteArray legacyDocumentColourStateFingerprintV2(
    const DocumentColourState &state)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    appendField(hash, QStringLiteral("VFXPhotoLab/DocumentColourState/v2"));
    appendField(hash, state.inputProfile.stableFingerprint());
    appendField(hash, state.workingSpace.stableFingerprint());
    appendField(hash, state.displayTransform.stableFingerprint());
    appendField(hash, state.proofing.stableFingerprint());
    appendField(hash, state.output.stableFingerprint());
    appendField(hash, static_cast<quint64>(state.processingCompatibility));
    appendField(hash, static_cast<quint64>(state.inputProfileStatus));
    appendField(hash, static_cast<quint64>(state.untaggedPolicy));
    appendField(hash, state.untaggedPolicyApplied);
    appendField(hash, state.originalInputProfileFingerprint);
    return finishFingerprint(hash);
}

QByteArray legacyDocumentColourStateFingerprintV3(
    const DocumentColourState &state)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    appendField(hash, QStringLiteral("VFXPhotoLab/DocumentColourState/v3"));
    appendField(hash, state.ocioConfig.stableFingerprint());
    appendField(hash, state.inputProfile.stableFingerprint());
    appendField(hash, state.workingSpace.stableFingerprint());
    appendField(hash, state.displayTransform.stableFingerprint());
    appendField(hash, state.proofing.stableFingerprint());
    appendField(hash, state.output.stableFingerprint());
    appendField(hash, static_cast<quint64>(state.processingCompatibility));
    appendField(hash, static_cast<quint64>(state.inputProfileStatus));
    appendField(hash, static_cast<quint64>(state.untaggedPolicy));
    appendField(hash, state.untaggedPolicyApplied);
    appendField(hash, state.originalInputProfileFingerprint);
    return finishFingerprint(hash);
}

} // namespace

QByteArray DocumentColourState::stableFingerprint() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    appendField(hash, QStringLiteral("VFXPhotoLab/DocumentColourState/v4"));
    appendField(hash, ocioConfig.stableFingerprint());
    appendField(hash, presentationColourManagementEnabled);
    appendField(hash, inputProfile.stableFingerprint());
    appendField(hash, workingSpace.stableFingerprint());
    appendField(hash, displayTransform.stableFingerprint());
    appendField(hash, proofing.stableFingerprint());
    appendField(hash, output.stableFingerprint());
    appendField(hash, static_cast<quint64>(processingCompatibility));
    appendField(hash, static_cast<quint64>(inputProfileStatus));
    appendField(hash, static_cast<quint64>(untaggedPolicy));
    appendField(hash, untaggedPolicyApplied);
    appendField(hash, originalInputProfileFingerprint);
    // Revision is deliberately excluded: it invalidates caches when semantic
    // state is re-applied, while the fingerprint identifies the semantic state.
    return finishFingerprint(hash);
}

qint64 DocumentColourState::estimatedBytes() const
{
    return static_cast<qint64>(sizeof(DocumentColourState))
        + ocioConfig.estimatedBytes() + inputProfile.estimatedBytes() + workingSpace.estimatedBytes()
        + displayTransform.estimatedBytes() + proofing.estimatedBytes()
        + output.estimatedBytes() + originalInputProfileFingerprint.size();
}

QJsonObject DocumentColourState::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("schema"), JsonSchemaVersion);
    object.insert(QStringLiteral("revision"), QString::number(revision));
    object.insert(QStringLiteral("processingCompatibility"),
                  processingToken(processingCompatibility));
    object.insert(QStringLiteral("inputProfileStatus"),
                  inputProfileStatusToken(inputProfileStatus));
    object.insert(QStringLiteral("untaggedPolicy"),
                  untaggedPolicyTokenInternal(untaggedPolicy));
    object.insert(QStringLiteral("untaggedPolicyApplied"), untaggedPolicyApplied);
    object.insert(QStringLiteral("originalInputProfileFingerprint"),
                  QString::fromLatin1(originalInputProfileFingerprint.toHex()));
    object.insert(QStringLiteral("ocioConfig"), ocioConfig.toJson());
    object.insert(QStringLiteral("presentationColourManagementEnabled"),
                  presentationColourManagementEnabled);
    object.insert(QStringLiteral("inputProfile"), inputProfile.toJson());
    object.insert(QStringLiteral("workingSpace"), workingSpace.toJson());
    object.insert(QStringLiteral("displayTransform"), displayTransform.toJson());
    object.insert(QStringLiteral("proofing"), proofing.toJson());
    object.insert(QStringLiteral("output"), output.toJson());
    object.insert(QStringLiteral("fingerprint"),
                  QString::fromLatin1(stableFingerprint().toHex()));
    return object;
}

std::optional<DocumentColourState> DocumentColourState::fromJson(
    const QJsonObject &object,
    QString *errorMessage)
{
    const QByteArray jsonBytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (jsonBytes.size() > MaximumColourStateJsonBytes) {
        setError(errorMessage, QStringLiteral("The document colour state exceeds its safety limit."));
        return std::nullopt;
    }
    const int schema = object.value(QStringLiteral("schema")).toInt(-1);
    if (schema != 1 && schema != 2 && schema != 3 && schema != JsonSchemaVersion) {
        setError(errorMessage, QStringLiteral("The document colour state uses an unsupported schema."));
        return std::nullopt;
    }
    const auto compatibility = processingFromToken(
        object.value(QStringLiteral("processingCompatibility")).toString());
    if (!compatibility
        || !object.value(QStringLiteral("inputProfile")).isObject()
        || !object.value(QStringLiteral("workingSpace")).isObject()
        || !object.value(QStringLiteral("displayTransform")).isObject()
        || !object.value(QStringLiteral("proofing")).isObject()
        || !object.value(QStringLiteral("output")).isObject()) {
        setError(errorMessage, QStringLiteral("The document colour state is incomplete."));
        return std::nullopt;
    }

    const auto input = ColourSpaceDescriptor::fromJson(
        object.value(QStringLiteral("inputProfile")).toObject(), errorMessage);
    const auto working = ColourSpaceDescriptor::fromJson(
        object.value(QStringLiteral("workingSpace")).toObject(), errorMessage);
    const auto display = DisplayTransformDescriptor::fromJson(
        object.value(QStringLiteral("displayTransform")).toObject(), errorMessage);
    const auto proofing = ProofingColourSettings::fromJson(
        object.value(QStringLiteral("proofing")).toObject(), errorMessage);
    const auto output = OutputColourSettings::fromJson(
        object.value(QStringLiteral("output")).toObject(), errorMessage);
    if (!input || !working || !display || !proofing || !output) {
        return std::nullopt;
    }

    bool revisionOk = true;
    DocumentColourState state;
    if (schema >= 3) {
        if (!object.value(QStringLiteral("ocioConfig")).isObject()) {
            setError(errorMessage, QStringLiteral("The document OCIO configuration reference is missing."));
            return std::nullopt;
        }
        const auto ocio = OcioConfigReference::fromJson(
            object.value(QStringLiteral("ocioConfig")).toObject(), errorMessage);
        if (!ocio) return std::nullopt;
        state.ocioConfig = *ocio;
    } else {
        state.ocioConfig = OcioConfigReference::disabled();
    }
    state.presentationColourManagementEnabled = schema >= 4
        ? object.value(QStringLiteral("presentationColourManagementEnabled")).toBool(false)
        : false;
    state.inputProfile = *input;
    state.workingSpace = *working;
    state.displayTransform = *display;
    state.proofing = *proofing;
    state.output = *output;
    state.processingCompatibility = *compatibility;
    if (schema >= 2) {
        const auto status = inputProfileStatusFromToken(
            object.value(QStringLiteral("inputProfileStatus")).toString());
        const auto policy = untaggedPolicyFromTokenInternal(
            object.value(QStringLiteral("untaggedPolicy")).toString());
        if (!status || !policy) {
            setError(errorMessage, QStringLiteral("The document input-profile metadata is invalid."));
            return std::nullopt;
        }
        state.inputProfileStatus = *status;
        state.untaggedPolicy = *policy;
        state.untaggedPolicyApplied = object.value(
            QStringLiteral("untaggedPolicyApplied")).toBool(false);
        state.originalInputProfileFingerprint = QByteArray::fromHex(
            object.value(QStringLiteral("originalInputProfileFingerprint"))
                .toString().toLatin1());
    } else {
        state.inputProfileStatus = InputProfileStatus::LegacyUnknown;
        state.untaggedPolicy = UntaggedImagePolicy::AssumeSRgb;
        state.untaggedPolicyApplied = false;
        state.originalInputProfileFingerprint = colourProfileContentFingerprint(
            state.inputProfile.toQColorSpace());
    }
    state.revision = revisionFromJson(object.value(QStringLiteral("revision")), &revisionOk);
    if (!revisionOk || !state.isSafe(errorMessage)) {
        if (!revisionOk) {
            setError(errorMessage, QStringLiteral("The document colour-state revision is invalid."));
        }
        return std::nullopt;
    }

    const QString savedFingerprint = object.value(QStringLiteral("fingerprint")).toString();
    const QByteArray expectedFingerprint = schema >= 4
        ? state.stableFingerprint()
        : (schema == 3
            ? legacyDocumentColourStateFingerprintV3(state)
            : (schema == 2
                ? legacyDocumentColourStateFingerprintV2(state)
                : legacyDocumentColourStateFingerprintV1(state)));
    if (!savedFingerprint.isEmpty()
        && savedFingerprint.toLatin1().compare(expectedFingerprint.toHex(),
                                               Qt::CaseInsensitive) != 0) {
        setError(errorMessage, QStringLiteral("The document colour state failed its integrity check."));
        return std::nullopt;
    }
    return state;
}

bool DocumentColourState::semanticallyEquals(const DocumentColourState &other) const
{
    return stableFingerprint() == other.stableFingerprint();
}

QByteArray ColourTransformRequest::stableFingerprint() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    appendField(hash, QStringLiteral("VFXPhotoLab/ColourTransformRequest/v1"));
    appendField(hash, source.stableFingerprint());
    appendField(hash, destination.stableFingerprint());
    appendField(hash, transformPurposeToken(purpose));
    appendField(hash, static_cast<quint64>(renderingIntent));
    appendField(hash, blackPointCompensation);
    return finishFingerprint(hash);
}

ColourTransformService &ColourTransformService::instance()
{
    static ColourTransformService service;
    return service;
}

std::optional<QColorTransform> ColourTransformService::qtTransform(
    const ColourTransformRequest &request)
{
    const QColorSpace sourceSpace = request.source.toQColorSpace();
    const QColorSpace destinationSpace = request.destination.toQColorSpace();
    if (!sourceSpace.isValid() || !destinationSpace.isValid()) {
        QMutexLocker locker(&m_mutex);
        ++m_misses;
        return std::nullopt;
    }

    const QByteArray key = request.stableFingerprint();
    {
        QMutexLocker locker(&m_mutex);
        const auto found = m_qtTransforms.constFind(key);
        if (found != m_qtTransforms.cend()) {
            ++m_hits;
            return found.value();
        }
    }

    const QColorTransform transform = sourceSpace.transformationToColorSpace(destinationSpace);
    {
        QMutexLocker locker(&m_mutex);
        const auto found = m_qtTransforms.constFind(key);
        if (found != m_qtTransforms.cend()) {
            ++m_hits;
            return found.value();
        }
        m_qtTransforms.insert(key, transform);
        ++m_misses;
    }
    return transform;
}

ColourTransformService::CacheStats ColourTransformService::cacheStats() const
{
    QMutexLocker locker(&m_mutex);
    return {m_qtTransforms.size(), m_hits, m_misses};
}

void ColourTransformService::clear()
{
    QMutexLocker locker(&m_mutex);
    m_qtTransforms.clear();
    m_hits = 0;
    m_misses = 0;
}

QString ocioConfigSourceName(const OcioConfigSource source)
{
    switch (source) {
    case OcioConfigSource::None: return QStringLiteral("Disabled");
    case OcioConfigSource::BuiltIn: return QStringLiteral("Built-in");
    case OcioConfigSource::ExternalFile: return QStringLiteral("External file");
    case OcioConfigSource::Environment: return QStringLiteral("Environment");
    }
    return QStringLiteral("Disabled");
}

QString colourProcessingCompatibilityName(
    const ColourProcessingCompatibility compatibility)
{
    return compatibility == ColourProcessingCompatibility::LegacyV1
        ? QStringLiteral("Legacy V1")
        : QStringLiteral("Managed V1");
}

QString displayTransformKindName(const DisplayTransformKind kind)
{
    switch (kind) {
    case DisplayTransformKind::Disabled: return QStringLiteral("Disabled");
    case DisplayTransformKind::SystemIcc: return QStringLiteral("System ICC");
    case DisplayTransformKind::IccProfile: return QStringLiteral("ICC Profile");
    case DisplayTransformKind::OcioView: return QStringLiteral("OCIO Display/View");
    }
    return QStringLiteral("Disabled");
}


QString untaggedImagePolicyName(const UntaggedImagePolicy policy)
{
    switch (policy) {
    case UntaggedImagePolicy::Ask: return QStringLiteral("Ask every time");
    case UntaggedImagePolicy::AssumeSRgb: return QStringLiteral("Assume sRGB");
    case UntaggedImagePolicy::LeaveUntagged: return QStringLiteral("Leave untagged");
    }
    return QStringLiteral("Assume sRGB");
}

QString untaggedImagePolicyToken(const UntaggedImagePolicy policy)
{
    return untaggedPolicyTokenInternal(policy);
}

std::optional<UntaggedImagePolicy> untaggedImagePolicyFromToken(const QString &token)
{
    return untaggedPolicyFromTokenInternal(token);
}

QString inputProfileStatusName(const InputProfileStatus status)
{
    switch (status) {
    case InputProfileStatus::LegacyUnknown: return QStringLiteral("Legacy / unknown origin");
    case InputProfileStatus::Generated: return QStringLiteral("Application-generated");
    case InputProfileStatus::EmbeddedValid: return QStringLiteral("Valid embedded profile");
    case InputProfileStatus::ClipboardValid: return QStringLiteral("Valid clipboard profile");
    case InputProfileStatus::Untagged: return QStringLiteral("No embedded profile");
    case InputProfileStatus::InvalidOrUnsupported: return QStringLiteral("Invalid or unsupported embedded profile");
    }
    return QStringLiteral("Legacy / unknown origin");
}

QByteArray colourProfileContentFingerprint(const QColorSpace &colourSpace)
{
    if (!colourSpace.isValid()) {
        return {};
    }
    const QByteArray profile = colourSpace.iccProfile();
    if (!profile.isEmpty()) {
        return QCryptographicHash::hash(profile, QCryptographicHash::Sha256);
    }
    return ColourSpaceDescriptor::fromQColorSpace(colourSpace).stableFingerprint();
}

} // namespace vfx
