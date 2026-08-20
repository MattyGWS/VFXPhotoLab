#pragma once

#include "ProductionExport.h"

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QTableWidget;

namespace vfx {

class ProductionExportDialog final : public QDialog {
public:
    ProductionExportDialog(const QString &documentName,
                           const QSize &documentSize,
                           const DocumentColourState &colourState,
                           QWidget *parent = nullptr);

    ProductionExportPlan plan() const;

protected:
    void accept() override;

private:
    void reloadProfiles(const QString &preferredProfileId = {});
    const ExportProfile *profileById(const QString &id) const;
    void addOutput(const QString &profileId = {});
    void duplicateSelectedOutput();
    void removeSelectedOutput();
    void moveSelectedOutput(int delta);
    int selectedOutputIndex() const;
    void rebuildTable(int preferredRow = -1, bool reloadEditor = true);
    void loadSelectedOutput();
    void storeSelectedOutput();
    void refreshEditor();
    void refreshPreview();
    void chooseDirectory();
    void openProfileManager();
    ExportProfileData selectedOutputProfileData() const;

    QString m_documentName;
    QSize m_documentSize;
    DocumentColourState m_colourState;
    QDateTime m_timestampUtc;
    QVector<ExportProfile> m_profiles;
    QVector<ProductionExportOutput> m_outputs;
    bool m_loadingEditor = false;

    QLineEdit *m_directoryEdit = nullptr;
    QComboBox *m_collisionPolicy = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_duplicateButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_upButton = nullptr;
    QPushButton *m_downButton = nullptr;
    QPushButton *m_manageProfilesButton = nullptr;

    QCheckBox *m_enabledCheck = nullptr;
    QComboBox *m_profileCombo = nullptr;
    QLineEdit *m_namingTemplate = nullptr;
    QComboBox *m_resizeMode = nullptr;
    QSpinBox *m_width = nullptr;
    QSpinBox *m_height = nullptr;
    QCheckBox *m_preserveAspect = nullptr;
    QSpinBox *m_longEdge = nullptr;
    QDoubleSpinBox *m_percentage = nullptr;
    QComboBox *m_resampleMethod = nullptr;
    QLabel *m_profileSummary = nullptr;
    QLabel *m_pathPreview = nullptr;
    QLabel *m_planSummary = nullptr;
};

} // namespace vfx
