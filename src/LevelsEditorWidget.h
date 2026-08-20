#pragma once

#include "Adjustment.h"
#include "HistogramService.h"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTimer;

namespace vfx {

enum class LevelsEyedropperRole {
    None,
    Black,
    Grey,
    White
};

class HistogramCanvas final : public QWidget {
    Q_OBJECT

public:
    explicit HistogramCanvas(QWidget *parent = nullptr);

    void setHistogram(const HistogramData &histogram);
    void setChannel(AdjustmentChannel channel);
    void setLevels(const LevelsChannelParameters &levels);
    void setLogarithmic(bool logarithmic);

signals:
    void interactionStarted();
    void levelsChanged(const vfx::LevelsChannelParameters &levels);
    void interactionFinished();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    enum class Handle {
        None,
        InputBlack,
        Gamma,
        InputWhite,
        OutputBlack,
        OutputWhite
    };

    QRectF graphRect() const;
    double handleValue(Handle handle) const;
    QPointF handlePosition(Handle handle) const;
    Handle hitTest(const QPointF &position) const;
    void updateHandle(const QPointF &position);
    void drawHistogram(QPainter &painter, const QRectF &rect);
    void drawHandles(QPainter &painter, const QRectF &rect);

    HistogramData m_histogram;
    AdjustmentChannel m_channel = AdjustmentChannel::Rgb;
    LevelsChannelParameters m_levels;
    bool m_logarithmic = false;
    Handle m_activeHandle = Handle::None;
};

class LevelsEditorWidget final : public QWidget {
    Q_OBJECT

public:
    explicit LevelsEditorWidget(QWidget *parent = nullptr);

    void setLevels(const LevelsParameters &levels);
    LevelsParameters levels() const;
    void setHistogram(const HistogramData &histogram);
    HistogramScope histogramScope() const;
    AdjustmentChannel selectedChannel() const;
    void applyEyedropperSample(LevelsEyedropperRole role, const QColor &colour);

signals:
    void interactionStarted();
    void levelsChanged(const vfx::LevelsParameters &levels);
    void interactionFinished();
    void histogramScopeChanged(vfx::HistogramScope scope);
    void eyedropperRequested(vfx::LevelsEyedropperRole role);

private:
    void selectChannel(AdjustmentChannel channel);
    void refreshControls();
    void updateSelectedChannel(const LevelsChannelParameters &parameters,
                               bool emitChange = true);
    void beginSpinInteraction();
    void finishSpinInteraction();
    void autoLevels();
    void resetSelectedChannel();
    void updateClippingText();
    const QVector<quint64> &selectedHistogram() const;
    double sampleComponent(const QColor &colour, AdjustmentChannel channel) const;

    LevelsParameters m_levels;
    HistogramData m_histogram;
    AdjustmentChannel m_selectedChannel = AdjustmentChannel::Rgb;
    bool m_updating = false;
    bool m_spinInteractionActive = false;

    QComboBox *m_channelCombo = nullptr;
    QComboBox *m_scopeCombo = nullptr;
    QCheckBox *m_logarithmicCheck = nullptr;
    HistogramCanvas *m_canvas = nullptr;
    QDoubleSpinBox *m_inputBlackSpin = nullptr;
    QDoubleSpinBox *m_gammaSpin = nullptr;
    QDoubleSpinBox *m_inputWhiteSpin = nullptr;
    QDoubleSpinBox *m_outputBlackSpin = nullptr;
    QDoubleSpinBox *m_outputWhiteSpin = nullptr;
    QDoubleSpinBox *m_autoShadowClipSpin = nullptr;
    QDoubleSpinBox *m_autoHighlightClipSpin = nullptr;
    QLabel *m_clippingLabel = nullptr;
    QLabel *m_histogramStatus = nullptr;
    QTimer *m_spinFinishTimer = nullptr;
};

} // namespace vfx
