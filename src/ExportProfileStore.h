#pragma once

#include "ImageExport.h"
#include "PresetCore.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace vfx {

struct ExportProfileData {
    static constexpr int JsonSchemaVersion = 1;

    QString formatSuffix = QStringLiteral("png");
    ImageExportBitDepth bitDepth = ImageExportBitDepth::Eight;
    ImageExportDither dither = ImageExportDither::BlueNoise64;
    ImageExportAlphaMode alphaMode = ImageExportAlphaMode::PreserveWhenSupported;
    bool convertToOutputProfile = true;
    OutputColourSettings output;
    int quality = 95;
    QColor matteColour = Qt::white;
    QString namingTemplate = QStringLiteral("{document}-edited");

    bool isValid(QString *errorMessage = nullptr) const;
    QJsonObject toJson(QString *errorMessage = nullptr) const;
    static bool fromJson(const QJsonObject &object,
                         ExportProfileData *profile,
                         QString *errorMessage = nullptr);

    bool operator==(const ExportProfileData &other) const = default;
};

struct ExportProfile {
    QString name;
    ExportProfileData data;
    bool builtIn = false;
    QString storagePath;
    PresetMetadata metadata;
};

class ExportProfileStore final {
public:
    static QVector<ExportProfile> profiles(QStringList *warnings = nullptr);
    static QVector<ExportProfile> builtInProfiles();
    static QString storageDirectory();

    static bool createUserProfile(const QString &name,
                                  const ExportProfileData &data,
                                  const QString &category,
                                  const QStringList &tags,
                                  QString *createdId = nullptr,
                                  QString *error = nullptr);
    static bool renameUserProfile(const ExportProfile &profile,
                                  const QString &newName,
                                  QString *error = nullptr);
    static bool duplicateUserProfile(const ExportProfile &profile,
                                     const QString &newName,
                                     QString *createdId = nullptr,
                                     QString *error = nullptr);
    static bool updateUserProfile(const ExportProfile &profile,
                                  const ExportProfileData &data,
                                  QString *error = nullptr);
    static bool updateMetadata(const ExportProfile &profile,
                               const QString &category,
                               const QStringList &tags,
                               QString *error = nullptr);
    static bool setFavourite(const ExportProfile &profile,
                             bool favourite,
                             QString *error = nullptr);
    static bool recordUse(const ExportProfile &profile,
                          QString *error = nullptr);
    static bool removeUserProfile(const ExportProfile &profile,
                                  QString *error = nullptr);
    static bool importProfileFile(const QString &sourcePath,
                                  QString *importedId = nullptr,
                                  QString *error = nullptr);
    static bool exportProfileFile(const ExportProfile &profile,
                                  const QString &destinationPath,
                                  QString *error = nullptr);
};

} // namespace vfx
