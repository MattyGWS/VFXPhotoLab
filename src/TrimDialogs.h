#pragma once

#include "TrimOperations.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QSpinBox;

namespace vfx {

class TrimTransparentDialog final : public QDialog {
    Q_OBJECT

public:
    explicit TrimTransparentDialog(QWidget *parent = nullptr);

    bool deleteOutsideCanvas() const;

private:
    QCheckBox *m_deleteOutsideCanvas = nullptr;
};

class TrimCornerColourDialog final : public QDialog {
    Q_OBJECT

public:
    explicit TrimCornerColourDialog(QWidget *parent = nullptr);

    AutomaticTrimRequest request() const;

private:
    void updateAcceptState();

    QComboBox *m_corner = nullptr;
    QSpinBox *m_tolerance = nullptr;
    QCheckBox *m_trimTop = nullptr;
    QCheckBox *m_trimBottom = nullptr;
    QCheckBox *m_trimLeft = nullptr;
    QCheckBox *m_trimRight = nullptr;
    QCheckBox *m_deleteOutsideCanvas = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

} // namespace vfx
