#pragma once

#include "PresetCore.h"
#include "VectorLayer.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace vfx {

struct VectorAppearancePreset {
    QString name;
    VectorAppearance appearance;
    QString storagePath;
    PresetMetadata metadata;
};

class VectorAppearancePresetStore final {
public:
    static constexpr int CurrentFileVersion = PresetStore::CurrentSchemaVersion;
    static constexpr int LegacyFileVersion = 1;
    static constexpr int MaximumPresetCount = 512;
    static constexpr qsizetype MaximumPresetBytes = 128 * 1024;

    static QVector<VectorAppearancePreset> presets(
        QStringList *warnings = nullptr);
    static bool saveUserPreset(const QString &name,
                               const VectorAppearance &appearance,
                               QString *error = nullptr);
    static bool createUserPreset(const QString &name,
                                 const VectorAppearance &appearance,
                                 const QString &category,
                                 const QStringList &tags,
                                 QString *createdId = nullptr,
                                 QString *error = nullptr);
    static bool renameUserPreset(const VectorAppearancePreset &preset,
                                 const QString &newName,
                                 QString *error = nullptr);
    static bool duplicateUserPreset(const VectorAppearancePreset &preset,
                                    const QString &newName,
                                    QString *error = nullptr);
    static bool updateUserPreset(const VectorAppearancePreset &preset,
                                 const VectorAppearance &appearance,
                                 QString *error = nullptr);
    static bool updateMetadata(const VectorAppearancePreset &preset,
                               const QString &category,
                               const QStringList &tags,
                               QString *error = nullptr);
    static bool setFavourite(const VectorAppearancePreset &preset,
                             bool favourite,
                             QString *error = nullptr);
    static bool recordUse(const VectorAppearancePreset &preset,
                          QString *error = nullptr);
    static bool importPresetFile(const QString &sourcePath,
                                 QString *importedName = nullptr,
                                 QString *error = nullptr);
    static bool exportPresetFile(const VectorAppearancePreset &preset,
                                 const QString &destinationPath,
                                 QString *error = nullptr);
    static bool removeUserPreset(const VectorAppearancePreset &preset,
                                 QString *error = nullptr);
    static QString storageDirectory();
};

} // namespace vfx
