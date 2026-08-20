#pragma once

#include "PhotoDocument.h"

#include <QColor>
#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace vfx {

class NewDocumentDialog final : public QDialog {
    Q_OBJECT

public:
    explicit NewDocumentDialog(const QColor &primaryColour,
                               QWidget *parent = nullptr);

    NewDocumentSettings settings() const;

protected:
    void accept() override;

private:
    enum class BackgroundChoice {
        White,
        Black,
        Transparent,
        Primary,
        Custom
    };

    void applyPreset(int index);
    void markCustomPreset();
    void updateBackgroundControls();
    void chooseCustomBackground();
    void updateSummary();
    void updateCustomColourButton();
    void restoreSettings();
    void saveSettings() const;
    QColor selectedBackgroundColour() const;

    QColor m_primaryColour;
    QColor m_customColour;
    bool m_updatingControls = false;

    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_presetCombo = nullptr;
    QSpinBox *m_widthSpin = nullptr;
    QSpinBox *m_heightSpin = nullptr;
    QPushButton *m_swapButton = nullptr;
    QDoubleSpinBox *m_resolutionSpin = nullptr;
    QComboBox *m_colourModelCombo = nullptr;
    QComboBox *m_bitDepthCombo = nullptr;
    QComboBox *m_colourSpaceCombo = nullptr;
    QComboBox *m_backgroundCombo = nullptr;
    QPushButton *m_customColourButton = nullptr;
    QLabel *m_summaryLabel = nullptr;
};

} // namespace vfx
