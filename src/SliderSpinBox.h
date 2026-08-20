#pragma once

#include <QWidget>

class QDoubleSpinBox;

namespace vfx {

// A compact numeric editor that combines a value field and slider into the
// same footprint. The edit field shows a range-progress fill, supports normal
// typed entry, and scrubs horizontally after Qt's drag threshold is crossed.
class SliderSpinBox final : public QWidget {
    Q_OBJECT

public:
    explicit SliderSpinBox(QWidget *parent = nullptr);

    void configure(double minimum,
                   double maximum,
                   double step,
                   int decimals);
    void setValue(double value);
    double value() const;

    // Dock/inspector rows use the comfortable default height. Fixed-height
    // toolbars can request their own style-aligned row height without
    // changing the value editor's behaviour or the toolbar itself.
    void setControlHeight(int height);

    QDoubleSpinBox *spinBox() const { return m_spin; }

signals:
    void valueChanged(double value);
    void interactionStarted();
    void interactionFinished();

private:
    QDoubleSpinBox *m_spin = nullptr;
    double m_minimum = 0.0;
    double m_maximum = 100.0;
    bool m_interactionActive = false;
    bool m_scrubInteraction = false;
};

} // namespace vfx
