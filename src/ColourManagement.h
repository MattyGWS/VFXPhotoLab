#pragma once

#include <QByteArray>
#include <QColorSpace>
#include <QColorTransform>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>

namespace vfx {

enum class ColourSpaceKind : quint8 {
    Untagged,
    BuiltIn,
    EmbeddedIcc,
    ExternalIcc,
    Ocio
};

enum class BuiltInColourSpace : quint8 {
    None,
    SRgb,
    LinearSRgb,
    DisplayP3,
    AdobeRgb,
    ProPhotoRgb
};

enum class ColourProcessingCompatibility : quint8 {
    LegacyV1,
    ManagedV1
};

enum class UntaggedImagePolicy : quint8 {
    Ask,
    AssumeSRgb,
    LeaveUntagged
};

enum class InputProfileStatus : quint8 {
    LegacyUnknown,
    Generated,
    EmbeddedValid,
    ClipboardValid,
    Untagged,
    InvalidOrUnsupported
};

enum class OcioConfigSource : quint8 {
    None,
    BuiltIn,
    ExternalFile,
    Environment
};

enum class DisplayTransformKind : quint8 {
    Disabled,
    SystemIcc,
    IccProfile,
    OcioView
};

enum class ColourRenderingIntent : quint8 {
    Perceptual,
    RelativeColorimetric,
    Saturation,
    AbsoluteColorimetric
};

enum class ColourTransformPurpose : quint8 {
    InputToWorking,
    WorkingToWorking,
    WorkingToDisplay,
    WorkingToProof,
    WorkingToOutput,
    AdjustmentDomain
};


struct OcioConfigReference {
    OcioConfigSource source = OcioConfigSource::None;
    QString identifier;
    QString canonicalPath;
    QString displayName;
    QString version;
    QByteArray fingerprint;
    QString iccBridgeSpace;

    static OcioConfigReference disabled();
    bool isConfigured() const;
    bool isSafe(QString *errorMessage = nullptr) const;
    QByteArray stableFingerprint() const;
    qint64 estimatedBytes() const;
    QJsonObject toJson() const;
    static std::optional<OcioConfigReference> fromJson(
        const QJsonObject &object,
        QString *errorMessage = nullptr);

    bool operator==(const OcioConfigReference &other) const = default;
};

struct ColourSpaceDescriptor {
    ColourSpaceKind kind = ColourSpaceKind::Untagged;
    BuiltInColourSpace builtIn = BuiltInColourSpace::None;
    QString displayName;
    QByteArray iccProfile;
    QString externalPath;
    QByteArray externalFingerprint;
    QString ocioConfigId;
    QByteArray ocioConfigFingerprint;
    QString ocioSpace;

    static ColourSpaceDescriptor untagged();
    static ColourSpaceDescriptor fromQColorSpace(const QColorSpace &colourSpace);
    static ColourSpaceDescriptor embeddedIcc(const QByteArray &iccProfile,
                                              const QString &displayName = {});
    static ColourSpaceDescriptor externalIcc(const QString &path,
                                             const QByteArray &fileFingerprint,
                                             const QByteArray &iccProfile = {},
                                             const QString &displayName = {});
    static ColourSpaceDescriptor ocio(const QString &configId,
                                      const QByteArray &configFingerprint,
                                      const QString &space,
                                      const QString &displayName = {});

    bool isValid() const;
    bool isUntagged() const;
    QColorSpace toQColorSpace() const;
    QByteArray stableFingerprint() const;
    qint64 estimatedBytes() const;
    QJsonObject toJson() const;
    static std::optional<ColourSpaceDescriptor> fromJson(const QJsonObject &object,
                                                         QString *errorMessage = nullptr);

    bool operator==(const ColourSpaceDescriptor &other) const = default;
};

struct DisplayTransformDescriptor {
    DisplayTransformKind kind = DisplayTransformKind::Disabled;
    ColourSpaceDescriptor profile;
    QString ocioConfigId;
    QByteArray ocioConfigFingerprint;
    QString ocioDisplay;
    QString ocioView;
    QString ocioLook;

    QByteArray stableFingerprint() const;
    qint64 estimatedBytes() const;
    QJsonObject toJson() const;
    static std::optional<DisplayTransformDescriptor> fromJson(const QJsonObject &object,
                                                              QString *errorMessage = nullptr);

    bool operator==(const DisplayTransformDescriptor &other) const = default;
};

struct ProofingColourSettings {
    bool enabled = false;
    ColourSpaceDescriptor profile;
    ColourRenderingIntent renderingIntent = ColourRenderingIntent::RelativeColorimetric;
    bool blackPointCompensation = true;
    bool gamutWarning = false;

    QByteArray stableFingerprint() const;
    qint64 estimatedBytes() const;
    QJsonObject toJson() const;
    static std::optional<ProofingColourSettings> fromJson(const QJsonObject &object,
                                                          QString *errorMessage = nullptr);

    bool operator==(const ProofingColourSettings &other) const = default;
};

struct OutputColourSettings {
    ColourSpaceDescriptor profile;
    ColourRenderingIntent renderingIntent = ColourRenderingIntent::RelativeColorimetric;
    bool blackPointCompensation = true;
    bool embedProfile = true;

    QByteArray stableFingerprint() const;
    qint64 estimatedBytes() const;
    QJsonObject toJson() const;
    static std::optional<OutputColourSettings> fromJson(const QJsonObject &object,
                                                        QString *errorMessage = nullptr);

    bool operator==(const OutputColourSettings &other) const = default;
};

struct ImageColourImportInfo {
    InputProfileStatus sourceStatus = InputProfileStatus::Generated;
    UntaggedImagePolicy appliedPolicy = UntaggedImagePolicy::AssumeSRgb;
    bool policyWasApplied = false;
    bool embeddedProfileAdvertised = false;
    QByteArray originalProfileFingerprint;
    QByteArray originalIccProfile;
    QStringList warnings;

    bool hasUsableInputProfile() const;
    bool requiresUntaggedPolicy() const;
};

struct DocumentColourState {
    static constexpr int JsonSchemaVersion = 4;

    OcioConfigReference ocioConfig;
    // Schema 4 compatibility gate. Existing schema-3 projects keep their
    // 0.11.0e presentation until the user explicitly enables display management.
    bool presentationColourManagementEnabled = false;
    ColourSpaceDescriptor inputProfile;
    ColourSpaceDescriptor workingSpace;
    DisplayTransformDescriptor displayTransform;
    ProofingColourSettings proofing;
    OutputColourSettings output;
    ColourProcessingCompatibility processingCompatibility =
        ColourProcessingCompatibility::ManagedV1;
    InputProfileStatus inputProfileStatus = InputProfileStatus::Generated;
    UntaggedImagePolicy untaggedPolicy = UntaggedImagePolicy::AssumeSRgb;
    bool untaggedPolicyApplied = false;
    QByteArray originalInputProfileFingerprint;
    quint64 revision = 1;

    static DocumentColourState managedForImage(const QColorSpace &colourSpace);
    static DocumentColourState managedForImportedImage(
        const QColorSpace &effectiveColourSpace,
        const ImageColourImportInfo &importInfo);
    static DocumentColourState legacyForImage(const QColorSpace &colourSpace);

    bool isSafe(QString *errorMessage = nullptr) const;
    QByteArray stableFingerprint() const;
    qint64 estimatedBytes() const;
    QJsonObject toJson() const;
    static std::optional<DocumentColourState> fromJson(const QJsonObject &object,
                                                       QString *errorMessage = nullptr);

    bool semanticallyEquals(const DocumentColourState &other) const;
    bool operator==(const DocumentColourState &other) const = default;
};

struct ColourTransformRequest {
    ColourSpaceDescriptor source;
    ColourSpaceDescriptor destination;
    ColourTransformPurpose purpose = ColourTransformPurpose::AdjustmentDomain;
    ColourRenderingIntent renderingIntent = ColourRenderingIntent::RelativeColorimetric;
    bool blackPointCompensation = true;

    QByteArray stableFingerprint() const;
};

class ColourTransformService final {
public:
    struct CacheStats {
        qsizetype entries = 0;
        quint64 hits = 0;
        quint64 misses = 0;
    };

    static ColourTransformService &instance();

    std::optional<QColorTransform> qtTransform(const ColourTransformRequest &request);
    CacheStats cacheStats() const;
    void clear();

private:
    ColourTransformService() = default;

    mutable QMutex m_mutex;
    QHash<QByteArray, QColorTransform> m_qtTransforms;
    quint64 m_hits = 0;
    quint64 m_misses = 0;
};

QString ocioConfigSourceName(OcioConfigSource source);
QString colourProcessingCompatibilityName(ColourProcessingCompatibility compatibility);
QString displayTransformKindName(DisplayTransformKind kind);
QString untaggedImagePolicyName(UntaggedImagePolicy policy);
QString untaggedImagePolicyToken(UntaggedImagePolicy policy);
std::optional<UntaggedImagePolicy> untaggedImagePolicyFromToken(const QString &token);
QString inputProfileStatusName(InputProfileStatus status);
QByteArray colourProfileContentFingerprint(const QColorSpace &colourSpace);

} // namespace vfx
