#pragma once

#include "ExportProfileStore.h"
#include "ImageExport.h"

#include <QDateTime>
#include <QDialog>
#include <QSize>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace vfx {

class ImageExportDialog final : public QDialog {
public:
    ImageExportDialog(const QString &filePath,
                      const QString &documentName,
                      const QSize &documentSize,
                      const DocumentColourState &colourState,
                      int sourceDepth,
                      QWidget *parent = nullptr);

    ImageExportRequest request() const;

protected:
    void accept() override;

private:
    void addOutputProfile(const ColourSpaceDescriptor &profile);
    void chooseExternalIccProfile();
    ColourSpaceDescriptor selectedOutputProfile() const;
    void refreshControls();
    void updateMatteButton();
    void persistFormatSettings() const;
    void updateCapabilitiesForFormat(bool preserveDepth = true);
    void rebuildBitDepthChoices(ImageExportBitDepth preferredDepth);
    void markCustomSettings();

    void reloadExportProfiles(const QString &preferredId = {});
    const ExportProfile *profileById(const QString &id) const;
    bool applyExportProfile(const QString &id, QString *errorMessage = nullptr);
    ExportProfileData currentProfileData() const;
    void openProfileManager();

    QString resolvedFilePath(QString *errorMessage = nullptr) const;
    QString currentProfileName() const;

    QString m_filePath;
    QString m_documentName;
    QSize m_documentSize;
    DocumentColourState m_colourState;
    ImageExportCapabilities m_capabilities;
    QVector<ColourSpaceDescriptor> m_profiles;
    QVector<ExportProfile> m_exportProfiles;
    QString m_selectedProfileId;
    QColor m_matteColour;
    QDateTime m_namingTimestampUtc;
    bool m_applyingProfile = false;

    QComboBox *m_exportProfileCombo = nullptr;
    QPushButton *m_manageProfiles = nullptr;
    QComboBox *m_formatCombo = nullptr;
    QLineEdit *m_namingTemplate = nullptr;
    QLabel *m_outputPathPreview = nullptr;
    QLabel *m_formatValue = nullptr;
    QLabel *m_workingValue = nullptr;
    QComboBox *m_profileMode = nullptr;
    QComboBox *m_profileCombo = nullptr;
    QPushButton *m_browseProfile = nullptr;
    QComboBox *m_intentCombo = nullptr;
    QCheckBox *m_blackPointCompensation = nullptr;
    QCheckBox *m_embedProfile = nullptr;
    QComboBox *m_bitDepthCombo = nullptr;
    QCheckBox *m_blueNoiseDither = nullptr;
    QComboBox *m_alphaModeCombo = nullptr;
    QSpinBox *m_quality = nullptr;
    QPushButton *m_matteButton = nullptr;
    QLabel *m_alphaNote = nullptr;
    QLabel *m_profileNote = nullptr;
    QLabel *m_summary = nullptr;
};

} // namespace vfx
