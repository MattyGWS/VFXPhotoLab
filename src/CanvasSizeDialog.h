#pragma once

#include "CanvasSizeOperations.h"

#include <QColor>
#include <QDialog>
#include <QSize>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace vfx {

class CanvasSizeDialog final : public QDialog {
    Q_OBJECT

public:
    explicit CanvasSizeDialog(const QSize &currentSize,
                              const QColor &foregroundColour,
                              const QColor &backgroundColour,
                              QWidget *parent = nullptr);

    QSize requestedSize() const;
    CanvasAnchor anchor() const;
    CanvasFillMode fillMode() const;
    QColor fillColour() const;
    bool deleteOutsideCanvas() const;

protected:
    void accept() override;

private:
    void setRelativeMode(bool relative);
    void updateTargetFromControls();
    void updateControlsFromTarget();
    void updateFillControls();
    void chooseCustomColour();
    void updateSummary();
    void restoreSettings();
    void saveSettings() const;

    QSize m_currentSize;
    QSize m_targetSize;
    QColor m_foregroundColour;
    QColor m_backgroundColour;
    QColor m_customColour = QColor(Qt::white);
    bool m_relative = false;
    bool m_updating = false;

    QComboBox *m_modeCombo = nullptr;
    QSpinBox *m_widthSpin = nullptr;
    QSpinBox *m_heightSpin = nullptr;
    QButtonGroup *m_anchorGroup = nullptr;
    QComboBox *m_fillCombo = nullptr;
    QPushButton *m_customColourButton = nullptr;
    QCheckBox *m_deleteOutsideCheck = nullptr;
    QLabel *m_summaryLabel = nullptr;
};

} // namespace vfx
