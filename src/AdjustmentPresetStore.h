#pragma once

#include "Adjustment.h"
#include "PresetCore.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace vfx {

struct AdjustmentPreset {
    QString name;
    AdjustmentData adjustment;
    bool builtIn = false;
    QString storagePath;
    PresetMetadata metadata;
};

class AdjustmentPresetStore final {
public:
    static QVector<AdjustmentPreset> presets(AdjustmentType type,
                                             QStringList *warnings = nullptr);
    static QVector<AdjustmentPreset> builtInPresets(AdjustmentType type);
    static bool saveUserPreset(const QString &name,
                               const AdjustmentData &adjustment,
                               QString *error = nullptr,
                               bool overwrite = false);
    static bool createUserPreset(const QString &name,
                                 const AdjustmentData &adjustment,
                                 const QString &category,
                                 const QStringList &tags,
                                 QString *createdId = nullptr,
                                 QString *error = nullptr);
    static bool userPresetExists(const QString &name, AdjustmentType type);
    static bool renameUserPreset(const AdjustmentPreset &preset,
                                 const QString &newName,
                                 QString *error = nullptr);
    static bool duplicateUserPreset(const AdjustmentPreset &preset,
                                    const QString &newName,
                                    QString *error = nullptr);
    static bool updateUserPreset(const AdjustmentPreset &preset,
                                 const AdjustmentData &adjustment,
                                 QString *error = nullptr);
    static bool updateMetadata(const AdjustmentPreset &preset,
                               const QString &category,
                               const QStringList &tags,
                               QString *error = nullptr);
    static bool setFavourite(const AdjustmentPreset &preset,
                             bool favourite,
                             QString *error = nullptr);
    static bool recordUse(const AdjustmentPreset &preset,
                          QString *error = nullptr);
    static bool importPresetFile(const QString &sourcePath,
                                 AdjustmentType expectedType,
                                 QString *importedName = nullptr,
                                 QString *error = nullptr);
    static bool exportPresetFile(const AdjustmentPreset &preset,
                                 const QString &destinationPath,
                                 QString *error = nullptr);
    static bool removeUserPreset(const AdjustmentPreset &preset,
                                 QString *error = nullptr);
    static QString storageDirectory(AdjustmentType type);
};

} // namespace vfx
