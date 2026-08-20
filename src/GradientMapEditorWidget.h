#pragma once

#include "Adjustment.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QPushButton;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;

namespace vfx {

class SliderSpinBox;

class GradientBarWidget final : public QWidget {
    Q_OBJECT

public:
    explicit GradientBarWidget(QWidget *parent = nullptr);

    void setParameters(const GradientMapParameters &parameters);
    const GradientMapParameters &parameters() const { return m_parameters; }
    int selectedIndex() const { return m_selectedIndex; }
    void setSelectedPosition(double position);
    void setSelectedColour(const QColor &colour);
    void addStop();
    void duplicateSelectedStop();
    void distributeStopsEvenly();
    void removeSelectedStop();

signals:
    void parametersChanged(const vfx::GradientMapParameters &parameters);
    void selectionChanged(int index);
    void interactionStarted();
    void interactionFinished();
    void colourEditRequested();

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    QSize sizeHint() const override;

private:
    QRect gradientRect() const;
    int nearestStop(const QPoint &position, int maximumDistance = 12) const;
    QColor colourAt(double position) const;
    void selectIndex(int index);
    void emitChanged();
    void finishDrag();

    GradientMapParameters m_parameters;
    int m_selectedIndex = 0;
    bool m_dragging = false;
};

class GradientMapEditorWidget final : public QWidget {
    Q_OBJECT

public:
    explicit GradientMapEditorWidget(QWidget *parent = nullptr);

    void setParameters(const GradientMapParameters &parameters);
    GradientMapParameters parameters() const;

signals:
    void gradientChanged(const vfx::GradientMapParameters &parameters);
    void interactionStarted();
    void interactionFinished();

private:
    void syncControls();
    void chooseSelectedColour();

    GradientBarWidget *m_bar = nullptr;
    SliderSpinBox *m_position = nullptr;
    QPushButton *m_colour = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_duplicate = nullptr;
    QPushButton *m_distribute = nullptr;
    QPushButton *m_remove = nullptr;
    QCheckBox *m_reverse = nullptr;
    QComboBox *m_interpolation = nullptr;
    bool m_updating = false;
    bool m_barInteractionActive = false;
};

} // namespace vfx
