#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace vfx {

enum class PresetKind : quint8 {
    Adjustment,
    VectorAppearance,
    ExportProfile
};

struct PresetMetadata {
    QString id;
    QString name;
    QString category;
    QStringList tags;
    bool favourite = false;
    bool builtIn = false;
    qint64 createdUtcMs = 0;
    qint64 modifiedUtcMs = 0;
    qint64 lastUsedUtcMs = 0;
    quint64 useCount = 0;

    bool isValid(QString *errorMessage = nullptr) const;
};

struct PresetEnvelope {
    PresetKind kind = PresetKind::Adjustment;
    PresetMetadata metadata;
    QJsonObject payload;
    QString storagePath;
    bool legacyFormat = false;

    bool isValid(QString *errorMessage = nullptr) const;
};

struct PresetUsageMetadata {
    bool favourite = false;
    qint64 lastUsedUtcMs = 0;
    quint64 useCount = 0;
};

class PresetUsageStore final {
public:
    static PresetUsageMetadata usageFor(const QString &presetId);
    static bool setFavourite(const QString &presetId,
                             bool favourite,
                             QString *errorMessage = nullptr);
    static bool recordUse(const QString &presetId,
                          QString *errorMessage = nullptr);
};

class PresetStore final {
public:
    static constexpr int CurrentSchemaVersion = 2;
    static constexpr int MaximumTagCount = 32;
    static constexpr int MaximumNameLength = 128;
    static constexpr int MaximumCategoryLength = 64;
    static constexpr int MaximumTagLength = 64;

    static QString kindName(PresetKind kind);
    static bool kindFromName(const QString &name, PresetKind *kind);

    static QString storageRoot();
    static QString storageDirectory(PresetKind kind,
                                    const QString &subtype = {});

    static QString stableBuiltInId(PresetKind kind,
                                   const QString &scope,
                                   const QString &name);
    static QString stableLegacyId(PresetKind kind,
                                  const QString &scope,
                                  const QString &name,
                                  const QByteArray &canonicalPayload);
    static QString newUserId(PresetKind kind);

    static QByteArray serialise(const PresetEnvelope &envelope,
                                QString *errorMessage = nullptr);
    static bool parse(const QByteArray &bytes,
                      PresetKind expectedKind,
                      PresetEnvelope *envelope,
                      QString *errorMessage = nullptr);

    static bool readFile(const QString &path,
                         PresetKind expectedKind,
                         qint64 maximumBytes,
                         PresetEnvelope *envelope,
                         QString *errorMessage = nullptr);
    static bool writeFile(const QString &path,
                          const PresetEnvelope &envelope,
                          qint64 maximumBytes,
                          QString *errorMessage = nullptr);

    static QString filePathForId(const QString &directory,
                                 const QString &id);
    static bool pathIsManagedByDirectory(const QString &path,
                                         const QString &directory);
};

} // namespace vfx
