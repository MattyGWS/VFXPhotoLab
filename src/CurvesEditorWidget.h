#pragma once

#include "Adjustment.h"
#include "HistogramService.h"

#include <QPointF>
#include <QRectF>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QMouseEvent;
class QKeyEvent;
class QPaintEvent;
class QPainter;
class QPushButton;
class QTimer;

namespace vfx {

class CurveCanvas final : public QWidget {
    Q_OBJECT

public:
    explicit CurveCanvas(QWidget *parent = nullptr);

    void setHistogram(const HistogramData &histogram);
    void setChannel(AdjustmentChannel channel);
    void setCurve(const CurveChannelParameters &curve);
    void setInterpolation(CurveInterpolation interpolation);
    void setLogarithmic(bool logarithmic);
    int selectedPoint() const;
    void selectPoint(int index);

signals:
    void interactionStarted();
    void curveChanged(const vfx::CurveChannelParameters &curve);
    void interactionFinished();
    void selectedPointChanged(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QRectF graphRect() const;
    QPointF pointPosition(const CurvePoint &point) const;
    int hitPoint(const QPointF &position) const;
    CurvePoint pointFromPosition(const QPointF &position) const;
    void moveSelected(CurvePoint point);
    void removeSelected();
    void drawHistogram(QPainter &painter, const QRectF &graph);

    HistogramData m_histogram;
    AdjustmentChannel m_channel = AdjustmentChannel::Rgb;
    CurveChannelParameters m_curve;
    CurveInterpolation m_interpolation = CurveInterpolation::Smooth;
    bool m_logarithmic = false;
    bool m_dragging = false;
    QPointF m_dragOffset;
    int m_selectedPoint = 0;
};

class CurvesEditorWidget final : public QWidget {
    Q_OBJECT

public:
    explicit CurvesEditorWidget(QWidget *parent = nullptr);

    void setCurves(const CurvesParameters &curves);
    CurvesParameters curves() const;
    void setHistogram(const HistogramData &histogram);
    HistogramScope histogramScope() const;
    void applyEyedropperSample(const QColor &colour);

signals:
    void interactionStarted();
    void curvesChanged(const vfx::CurvesParameters &curves);
    void interactionFinished();
    void histogramScopeChanged(vfx::HistogramScope scope);
    void eyedropperRequested();

private:
    void selectChannel(AdjustmentChannel channel);
    void refreshControls();
    void updateSelectedCurve(const CurveChannelParameters &curve, bool emitChange = true);
    void beginSpinInteraction();
    void finishSpinInteraction();
    void updatePointSpins();
    void resetSelectedChannel();
    void resetAllChannels();

    CurvesParameters m_curves;
    HistogramData m_histogram;
    AdjustmentChannel m_selectedChannel = AdjustmentChannel::Rgb;
    bool m_updating = false;
    bool m_spinInteractionActive = false;

    QComboBox *m_channelCombo = nullptr;
    QComboBox *m_scopeCombo = nullptr;
    QComboBox *m_interpolationCombo = nullptr;
    QCheckBox *m_logarithmicCheck = nullptr;
    CurveCanvas *m_canvas = nullptr;
    QDoubleSpinBox *m_inputSpin = nullptr;
    QDoubleSpinBox *m_outputSpin = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_sampleButton = nullptr;
    QLabel *m_histogramStatus = nullptr;
    QTimer *m_spinFinishTimer = nullptr;
};

} // namespace vfx
