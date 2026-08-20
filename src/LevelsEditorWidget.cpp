#include "LevelsEditorWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineF>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPushButton>
#include <QPolygonF>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace vfx {
namespace {

constexpr double MinimumGap = 1.0 / 65535.0;

QColor channelColour(const AdjustmentChannel channel, const QPalette &palette)
{
    switch (channel) {
    case AdjustmentChannel::Red: return QColor(235, 82, 82);
    case AdjustmentChannel::Green: return QColor(80, 205, 112);
    case AdjustmentChannel::Blue: return QColor(90, 142, 245);
    case AdjustmentChannel::Rgb: return palette.color(QPalette::Text);
    }
    return palette.color(QPalette::Text);
}

QString channelName(const AdjustmentChannel channel)
{
    switch (channel) {
    case AdjustmentChannel::Rgb: return QObject::tr("RGB");
    case AdjustmentChannel::Red: return QObject::tr("Red");
    case AdjustmentChannel::Green: return QObject::tr("Green");
    case AdjustmentChannel::Blue: return QObject::tr("Blue");
    }
    return QObject::tr("RGB");
}

quint64 sumRange(const QVector<quint64> &values, int first, int last)
{
    if (values.isEmpty()) {
        return 0;
    }
    const int lastIndex = static_cast<int>(values.size()) - 1;
    first = std::clamp(first, 0, lastIndex);
    last = std::clamp(last, 0, lastIndex);
    if (last < first) {
        return 0;
    }
    quint64 total = 0;
    for (int index = first; index <= last; ++index) {
        total += values.at(index);
    }
    return total;
}

} // namespace

HistogramCanvas::HistogramCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    setToolTip(tr("Drag the upper handles for input black, midpoint and white. Drag the lower handles for output black and white."));
}

void HistogramCanvas::setHistogram(const HistogramData &histogram)
{
    m_histogram = histogram;
    update();
}

void HistogramCanvas::setChannel(const AdjustmentChannel channel)
{
    if (m_channel == channel) {
        return;
    }
    m_channel = channel;
    update();
}

void HistogramCanvas::setLevels(const LevelsChannelParameters &levels)
{
    m_levels = levels;
    m_levels.normalise();
    update();
}

void HistogramCanvas::setLogarithmic(const bool logarithmic)
{
    if (m_logarithmic == logarithmic) {
        return;
    }
    m_logarithmic = logarithmic;
    update();
}

QRectF HistogramCanvas::graphRect() const
{
    return QRectF(rect()).adjusted(8.0, 18.0, -8.0, -20.0);
}

double HistogramCanvas::handleValue(const Handle handle) const
{
    switch (handle) {
    case Handle::InputBlack: return m_levels.inputBlack;
    case Handle::Gamma: return m_levels.inputBlack
        + std::pow(0.5, m_levels.gamma)
            * (m_levels.inputWhite - m_levels.inputBlack);
    case Handle::InputWhite: return m_levels.inputWhite;
    case Handle::OutputBlack: return m_levels.outputBlack;
    case Handle::OutputWhite: return m_levels.outputWhite;
    case Handle::None: break;
    }
    return 0.0;
}

QPointF HistogramCanvas::handlePosition(const Handle handle) const
{
    const QRectF graph = graphRect();
    const double x = graph.left() + handleValue(handle) * graph.width();
    const bool output = handle == Handle::OutputBlack || handle == Handle::OutputWhite;
    return QPointF(x, output ? graph.bottom() + 8.0 : graph.top() - 8.0);
}

HistogramCanvas::Handle HistogramCanvas::hitTest(const QPointF &position) const
{
    const std::array<Handle, 5> handles {
        Handle::InputBlack, Handle::Gamma, Handle::InputWhite,
        Handle::OutputBlack, Handle::OutputWhite
    };
    Handle nearest = Handle::None;
    double nearestDistance = 13.0;
    for (const Handle handle : handles) {
        const double distance = QLineF(position, handlePosition(handle)).length();
        if (distance < nearestDistance) {
            nearest = handle;
            nearestDistance = distance;
        }
    }
    return nearest;
}

void HistogramCanvas::updateHandle(const QPointF &position)
{
    if (m_activeHandle == Handle::None) {
        return;
    }
    const QRectF graph = graphRect();
    const double value = std::clamp((position.x() - graph.left()) / graph.width(), 0.0, 1.0);
    switch (m_activeHandle) {
    case Handle::InputBlack:
        m_levels.inputBlack = std::min(value, m_levels.inputWhite - MinimumGap);
        break;
    case Handle::InputWhite:
        m_levels.inputWhite = std::max(value, m_levels.inputBlack + MinimumGap);
        break;
    case Handle::Gamma: {
        const double span = std::max(MinimumGap,
                                     m_levels.inputWhite - m_levels.inputBlack);
        const double midpoint = std::clamp((value - m_levels.inputBlack) / span,
                                           0.001,
                                           0.999);
        m_levels.gamma = std::clamp(std::log(midpoint) / std::log(0.5), 0.1, 10.0);
        break;
    }
    case Handle::OutputBlack:
        m_levels.outputBlack = std::min(value, m_levels.outputWhite);
        break;
    case Handle::OutputWhite:
        m_levels.outputWhite = std::max(value, m_levels.outputBlack);
        break;
    case Handle::None:
        break;
    }
    m_levels.normalise();
    emit levelsChanged(m_levels);
    update();
}

void HistogramCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF graph = graphRect();
    painter.fillRect(rect(), palette().color(QPalette::Base));
    painter.fillRect(graph, palette().color(QPalette::Window));
    painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
    painter.drawRect(graph);

    drawHistogram(painter, graph);

    QColor shadowClip(55, 125, 245, 48);
    QColor highlightClip(245, 80, 70, 48);
    const double blackX = graph.left() + m_levels.inputBlack * graph.width();
    const double whiteX = graph.left() + m_levels.inputWhite * graph.width();
    if (blackX > graph.left()) {
        painter.fillRect(QRectF(graph.left(), graph.top(),
                                blackX - graph.left(), graph.height()),
                         shadowClip);
    }
    if (whiteX < graph.right()) {
        painter.fillRect(QRectF(whiteX, graph.top(),
                                graph.right() - whiteX, graph.height()),
                         highlightClip);
    }
    drawHandles(painter, graph);
}

void HistogramCanvas::drawHistogram(QPainter &painter, const QRectF &rect)
{
    if (!m_histogram.isValid() || rect.width() < 2.0) {
        painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
        painter.drawText(rect, Qt::AlignCenter, tr("Calculating histogram…"));
        return;
    }

    const auto drawOne = [&](const QVector<quint64> &bins,
                             const QColor &colour,
                             const double opacity) {
        if (bins.isEmpty()) {
            return;
        }
        const int columns = std::max(1, static_cast<int>(std::floor(rect.width())));
        QVector<double> values(columns, 0.0);
        double maximum = 0.0;
        for (int column = 0; column < columns; ++column) {
            const int begin = static_cast<int>((static_cast<qint64>(column) * bins.size()) / columns);
            const int end = std::max(begin + 1,
                static_cast<int>((static_cast<qint64>(column + 1) * bins.size()) / columns));
            quint64 total = 0;
            const int boundedEnd = std::min(end, static_cast<int>(bins.size()));
            for (int index = begin; index < boundedEnd; ++index) {
                total += bins.at(index);
            }
            const double display = m_logarithmic ? std::log1p(static_cast<double>(total))
                                                  : static_cast<double>(total);
            values[column] = display;
            maximum = std::max(maximum, display);
        }
        if (maximum <= 0.0) {
            return;
        }
        QPainterPath path;
        path.moveTo(rect.left(), rect.bottom());
        for (int column = 0; column < columns; ++column) {
            const double x = rect.left() + column;
            const double y = rect.bottom() - values.at(column) / maximum * rect.height();
            path.lineTo(x, y);
        }
        path.lineTo(rect.right(), rect.bottom());
        path.closeSubpath();
        QColor fill = colour;
        fill.setAlphaF(opacity);
        painter.fillPath(path, fill);
        QColor line = colour;
        line.setAlphaF(std::min(1.0, opacity + 0.28));
        painter.setPen(QPen(line, 1.0));
        painter.drawPath(path);
    };

    if (m_channel == AdjustmentChannel::Rgb) {
        drawOne(m_histogram.red, QColor(235, 82, 82), 0.22);
        drawOne(m_histogram.green, QColor(80, 205, 112), 0.22);
        drawOne(m_histogram.blue, QColor(90, 142, 245), 0.22);
        drawOne(m_histogram.luminance, palette().color(QPalette::Text), 0.28);
    } else {
        const HistogramChannel histogramChannel = m_channel == AdjustmentChannel::Red
            ? HistogramChannel::Red
            : m_channel == AdjustmentChannel::Green
                ? HistogramChannel::Green
                : HistogramChannel::Blue;
        drawOne(m_histogram.channel(histogramChannel),
                channelColour(m_channel, palette()),
                0.50);
    }
}

void HistogramCanvas::drawHandles(QPainter &painter, const QRectF &)
{
    const auto triangle = [&](const Handle handle, const QColor &fill) {
        const QPointF centre = handlePosition(handle);
        const bool output = handle == Handle::OutputBlack || handle == Handle::OutputWhite;
        QPolygonF polygon;
        if (output) {
            polygon << QPointF(centre.x() - 6.0, centre.y() + 5.0)
                    << QPointF(centre.x() + 6.0, centre.y() + 5.0)
                    << QPointF(centre.x(), centre.y() - 6.0);
        } else {
            polygon << QPointF(centre.x() - 6.0, centre.y() - 5.0)
                    << QPointF(centre.x() + 6.0, centre.y() - 5.0)
                    << QPointF(centre.x(), centre.y() + 6.0);
        }
        painter.setPen(QPen(palette().color(QPalette::Shadow), 1.0));
        painter.setBrush(fill);
        painter.drawPolygon(polygon);
    };

    triangle(Handle::InputBlack, Qt::black);
    triangle(Handle::Gamma, palette().color(QPalette::Midlight));
    triangle(Handle::InputWhite, Qt::white);
    triangle(Handle::OutputBlack, Qt::black);
    triangle(Handle::OutputWhite, Qt::white);
}

void HistogramCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_activeHandle = hitTest(event->position());
    if (m_activeHandle == Handle::None) {
        QWidget::mousePressEvent(event);
        return;
    }
    emit interactionStarted();
    updateHandle(event->position());
    event->accept();
}

void HistogramCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_activeHandle != Handle::None && (event->buttons() & Qt::LeftButton)) {
        updateHandle(event->position());
        event->accept();
        return;
    }
    setCursor(hitTest(event->position()) == Handle::None
                  ? Qt::ArrowCursor
                  : Qt::SizeHorCursor);
    QWidget::mouseMoveEvent(event);
}

void HistogramCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_activeHandle != Handle::None) {
        updateHandle(event->position());
        m_activeHandle = Handle::None;
        emit interactionFinished();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

LevelsEditorWidget::LevelsEditorWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(6);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    m_channelCombo = new QComboBox(this);
    for (int index = 0; index < 4; ++index) {
        const auto channel = static_cast<AdjustmentChannel>(index);
        m_channelCombo->addItem(channelName(channel), index);
    }
    m_scopeCombo = new QComboBox(this);
    m_scopeCombo->addItem(tr("Document"), static_cast<int>(HistogramScope::Document));
    m_scopeCombo->addItem(tr("Selection"), static_cast<int>(HistogramScope::Selection));
    m_scopeCombo->setToolTip(tr("Limit histogram analysis to the active selection"));
    m_logarithmicCheck = new QCheckBox(tr("Log"), this);
    m_logarithmicCheck->setToolTip(tr("Use logarithmic histogram height"));
    toolbar->addWidget(m_channelCombo, 1);
    toolbar->addWidget(m_scopeCombo, 1);
    toolbar->addWidget(m_logarithmicCheck);
    root->addLayout(toolbar);

    m_canvas = new HistogramCanvas(this);
    root->addWidget(m_canvas);

    auto *inputGrid = new QGridLayout;
    inputGrid->setContentsMargins(0, 0, 0, 0);
    inputGrid->setHorizontalSpacing(5);
    inputGrid->setVerticalSpacing(3);
    const QStringList labels {tr("Black"), tr("Midpoint"), tr("White"),
                              tr("Out black"), tr("Out white")};
    m_inputBlackSpin = new QDoubleSpinBox(this);
    m_gammaSpin = new QDoubleSpinBox(this);
    m_inputWhiteSpin = new QDoubleSpinBox(this);
    m_outputBlackSpin = new QDoubleSpinBox(this);
    m_outputWhiteSpin = new QDoubleSpinBox(this);
    const QList<QDoubleSpinBox *> spins {m_inputBlackSpin, m_gammaSpin,
                                         m_inputWhiteSpin, m_outputBlackSpin,
                                         m_outputWhiteSpin};
    for (int index = 0; index < spins.size(); ++index) {
        auto *label = new QLabel(labels.at(index), this);
        label->setObjectName(QStringLiteral("MutedLabel"));
        inputGrid->addWidget(label, 0, index);
        spins.at(index)->setMinimumWidth(0);
        spins.at(index)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        inputGrid->addWidget(spins.at(index), 1, index);
    }
    for (QDoubleSpinBox *spin : {m_inputBlackSpin, m_inputWhiteSpin,
                                 m_outputBlackSpin, m_outputWhiteSpin}) {
        spin->setRange(0.0, 255.0);
        spin->setDecimals(0);
        spin->setSingleStep(1.0);
    }
    m_gammaSpin->setRange(0.10, 10.0);
    m_gammaSpin->setDecimals(3);
    m_gammaSpin->setSingleStep(0.01);
    root->addLayout(inputGrid);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    auto *autoButton = new QPushButton(tr("Auto"), this);
    auto *blackPicker = new QToolButton(this);
    auto *greyPicker = new QToolButton(this);
    auto *whitePicker = new QToolButton(this);
    auto *resetButton = new QPushButton(tr("Reset Channel"), this);
    blackPicker->setText(tr("B"));
    greyPicker->setText(tr("G"));
    whitePicker->setText(tr("W"));
    blackPicker->setToolTip(tr("Sample a black point from the rendered image"));
    greyPicker->setToolTip(tr("Sample a neutral grey point from the rendered image"));
    whitePicker->setToolTip(tr("Sample a white point from the rendered image"));
    buttonRow->addWidget(autoButton);
    buttonRow->addWidget(blackPicker);
    buttonRow->addWidget(greyPicker);
    buttonRow->addWidget(whitePicker);
    buttonRow->addStretch();
    buttonRow->addWidget(resetButton);
    root->addLayout(buttonRow);

    auto *autoClipGrid = new QGridLayout;
    autoClipGrid->setContentsMargins(0, 0, 0, 0);
    autoClipGrid->setHorizontalSpacing(5);
    autoClipGrid->setVerticalSpacing(3);
    auto *shadowClipLabel = new QLabel(tr("Auto shadows %"), this);
    auto *highlightClipLabel = new QLabel(tr("Auto highlights %"), this);
    shadowClipLabel->setObjectName(QStringLiteral("MutedLabel"));
    highlightClipLabel->setObjectName(QStringLiteral("MutedLabel"));
    m_autoShadowClipSpin = new QDoubleSpinBox(this);
    m_autoHighlightClipSpin = new QDoubleSpinBox(this);
    for (QDoubleSpinBox *spin : {m_autoShadowClipSpin, m_autoHighlightClipSpin}) {
        spin->setRange(0.0, 25.0);
        spin->setDecimals(3);
        spin->setSingleStep(0.05);
        spin->setSuffix(tr("%"));
        spin->setMinimumWidth(0);
        spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    autoClipGrid->addWidget(shadowClipLabel, 0, 0);
    autoClipGrid->addWidget(highlightClipLabel, 0, 1);
    autoClipGrid->addWidget(m_autoShadowClipSpin, 1, 0);
    autoClipGrid->addWidget(m_autoHighlightClipSpin, 1, 1);
    root->addLayout(autoClipGrid);

    m_clippingLabel = new QLabel(this);
    m_clippingLabel->setObjectName(QStringLiteral("MutedLabel"));
    m_histogramStatus = new QLabel(tr("Histogram pending"), this);
    m_histogramStatus->setObjectName(QStringLiteral("MutedLabel"));
    root->addWidget(m_clippingLabel);
    root->addWidget(m_histogramStatus);

    m_spinFinishTimer = new QTimer(this);
    m_spinFinishTimer->setSingleShot(true);
    m_spinFinishTimer->setInterval(350);
    connect(m_spinFinishTimer, &QTimer::timeout,
            this, &LevelsEditorWidget::finishSpinInteraction);

    connect(m_channelCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](const int index) {
        if (index >= 0) {
            selectChannel(static_cast<AdjustmentChannel>(
                m_channelCombo->itemData(index).toInt()));
        }
    });
    connect(m_scopeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](const int index) {
        if (index >= 0) {
            emit histogramScopeChanged(static_cast<HistogramScope>(
                m_scopeCombo->itemData(index).toInt()));
        }
    });
    connect(m_logarithmicCheck, &QCheckBox::toggled, this, [this](const bool checked) {
        if (m_updating) {
            return;
        }
        emit interactionStarted();
        m_levels.logarithmicHistogram = checked;
        m_canvas->setLogarithmic(checked);
        emit levelsChanged(m_levels);
        emit interactionFinished();
    });
    connect(m_canvas, &HistogramCanvas::interactionStarted,
            this, &LevelsEditorWidget::interactionStarted);
    connect(m_canvas, &HistogramCanvas::levelsChanged,
            this, [this](const LevelsChannelParameters &parameters) {
        updateSelectedChannel(parameters);
    });
    connect(m_canvas, &HistogramCanvas::interactionFinished,
            this, &LevelsEditorWidget::interactionFinished);

    const auto spinChanged = [this] {
        if (m_updating) {
            return;
        }
        beginSpinInteraction();
        LevelsChannelParameters parameters = m_levels.channel(m_selectedChannel);
        parameters.inputBlack = m_inputBlackSpin->value() / 255.0;
        parameters.gamma = m_gammaSpin->value();
        parameters.inputWhite = m_inputWhiteSpin->value() / 255.0;
        parameters.outputBlack = m_outputBlackSpin->value() / 255.0;
        parameters.outputWhite = m_outputWhiteSpin->value() / 255.0;
        parameters.normalise();
        updateSelectedChannel(parameters);
        m_spinFinishTimer->start();
    };
    for (QDoubleSpinBox *spin : spins) {
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [spinChanged](double) { spinChanged(); });
        connect(spin, &QDoubleSpinBox::editingFinished,
                this, &LevelsEditorWidget::finishSpinInteraction);
    }
    const auto autoClipChanged = [this] {
        if (m_updating) {
            return;
        }
        beginSpinInteraction();
        m_levels.autoClipShadows = m_autoShadowClipSpin->value() / 100.0;
        m_levels.autoClipHighlights = m_autoHighlightClipSpin->value() / 100.0;
        m_levels.normalise();
        emit levelsChanged(m_levels);
        m_spinFinishTimer->start();
    };
    for (QDoubleSpinBox *spin : {m_autoShadowClipSpin, m_autoHighlightClipSpin}) {
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [autoClipChanged](double) { autoClipChanged(); });
        connect(spin, &QDoubleSpinBox::editingFinished,
                this, &LevelsEditorWidget::finishSpinInteraction);
    }

    connect(autoButton, &QPushButton::clicked, this, &LevelsEditorWidget::autoLevels);
    connect(resetButton, &QPushButton::clicked,
            this, &LevelsEditorWidget::resetSelectedChannel);
    connect(blackPicker, &QToolButton::clicked, this, [this] {
        emit eyedropperRequested(LevelsEyedropperRole::Black);
    });
    connect(greyPicker, &QToolButton::clicked, this, [this] {
        emit eyedropperRequested(LevelsEyedropperRole::Grey);
    });
    connect(whitePicker, &QToolButton::clicked, this, [this] {
        emit eyedropperRequested(LevelsEyedropperRole::White);
    });

    refreshControls();
}

void LevelsEditorWidget::setLevels(const LevelsParameters &levels)
{
    m_levels = levels;
    m_levels.normalise();
    refreshControls();
}

LevelsParameters LevelsEditorWidget::levels() const
{
    return m_levels;
}

void LevelsEditorWidget::setHistogram(const HistogramData &histogram)
{
    m_histogram = histogram;
    m_canvas->setHistogram(histogram);
    if (histogram.isValid()) {
        m_histogramStatus->setText(
            tr("%1-bit input • %2 analysed pixel(s)%3")
                .arg(histogram.sourceBitDepth)
                .arg(histogram.includedPixels)
                .arg(histogram.transparentPixels > 0
                         ? tr(" • %1 transparent excluded").arg(histogram.transparentPixels)
                         : QString()));
    } else {
        m_histogramStatus->setText(tr("Histogram unavailable"));
    }
    updateClippingText();
}

HistogramScope LevelsEditorWidget::histogramScope() const
{
    return static_cast<HistogramScope>(m_scopeCombo->currentData().toInt());
}

AdjustmentChannel LevelsEditorWidget::selectedChannel() const
{
    return m_selectedChannel;
}

void LevelsEditorWidget::selectChannel(const AdjustmentChannel channel)
{
    m_selectedChannel = channel;
    refreshControls();
}

void LevelsEditorWidget::refreshControls()
{
    m_updating = true;
    const LevelsChannelParameters &parameters = m_levels.channel(m_selectedChannel);
    m_inputBlackSpin->setValue(parameters.inputBlack * 255.0);
    m_gammaSpin->setValue(parameters.gamma);
    m_inputWhiteSpin->setValue(parameters.inputWhite * 255.0);
    m_outputBlackSpin->setValue(parameters.outputBlack * 255.0);
    m_outputWhiteSpin->setValue(parameters.outputWhite * 255.0);
    m_autoShadowClipSpin->setValue(m_levels.autoClipShadows * 100.0);
    m_autoHighlightClipSpin->setValue(m_levels.autoClipHighlights * 100.0);
    {
        const QSignalBlocker blocker(m_logarithmicCheck);
        m_logarithmicCheck->setChecked(m_levels.logarithmicHistogram);
    }
    m_canvas->setChannel(m_selectedChannel);
    m_canvas->setLevels(parameters);
    m_canvas->setLogarithmic(m_levels.logarithmicHistogram);
    m_updating = false;
    updateClippingText();
}

void LevelsEditorWidget::updateSelectedChannel(
    const LevelsChannelParameters &parameters,
    const bool emitChange)
{
    LevelsChannelParameters safe = parameters;
    safe.normalise();
    m_levels.channel(m_selectedChannel) = safe;
    m_levels.normalise();
    refreshControls();
    if (emitChange) {
        emit levelsChanged(m_levels);
    }
}

void LevelsEditorWidget::beginSpinInteraction()
{
    if (m_spinInteractionActive) {
        return;
    }
    m_spinInteractionActive = true;
    emit interactionStarted();
}

void LevelsEditorWidget::finishSpinInteraction()
{
    m_spinFinishTimer->stop();
    if (!m_spinInteractionActive) {
        return;
    }
    m_spinInteractionActive = false;
    emit interactionFinished();
}

const QVector<quint64> &LevelsEditorWidget::selectedHistogram() const
{
    static const QVector<quint64> empty;
    if (!m_histogram.isValid()) {
        return empty;
    }
    switch (m_selectedChannel) {
    case AdjustmentChannel::Rgb: return m_histogram.luminance;
    case AdjustmentChannel::Red: return m_histogram.red;
    case AdjustmentChannel::Green: return m_histogram.green;
    case AdjustmentChannel::Blue: return m_histogram.blue;
    }
    return empty;
}

void LevelsEditorWidget::autoLevels()
{
    const QVector<quint64> &bins = selectedHistogram();
    if (bins.isEmpty()) {
        return;
    }
    quint64 total = 0;
    for (const quint64 value : bins) {
        total += value;
    }
    if (total == 0) {
        return;
    }
    const quint64 lowTarget = static_cast<quint64>(
        std::llround(total * m_levels.autoClipShadows));
    const quint64 highTarget = static_cast<quint64>(
        std::llround(total * m_levels.autoClipHighlights));
    quint64 cumulative = 0;
    int low = 0;
    for (; low < bins.size() - 1; ++low) {
        cumulative += bins.at(low);
        if (cumulative >= lowTarget) {
            break;
        }
    }
    cumulative = 0;
    int high = bins.size() - 1;
    for (; high > low; --high) {
        cumulative += bins.at(high);
        if (cumulative >= highTarget) {
            break;
        }
    }
    LevelsChannelParameters parameters = m_levels.channel(m_selectedChannel);
    parameters.inputBlack = low / static_cast<double>(bins.size() - 1);
    parameters.inputWhite = high / static_cast<double>(bins.size() - 1);
    parameters.gamma = 1.0;
    emit interactionStarted();
    updateSelectedChannel(parameters);
    emit interactionFinished();
}

void LevelsEditorWidget::resetSelectedChannel()
{
    emit interactionStarted();
    updateSelectedChannel(LevelsChannelParameters {});
    emit interactionFinished();
}

void LevelsEditorWidget::updateClippingText()
{
    const QVector<quint64> &bins = selectedHistogram();
    if (bins.isEmpty()) {
        m_clippingLabel->setText(tr("Clipping: unavailable"));
        return;
    }
    quint64 total = 0;
    for (const quint64 value : bins) {
        total += value;
    }
    if (total == 0) {
        m_clippingLabel->setText(tr("Clipping: no opaque input pixels"));
        return;
    }
    const LevelsChannelParameters &parameters = m_levels.channel(m_selectedChannel);
    const int blackBin = static_cast<int>(std::floor(
        parameters.inputBlack * (bins.size() - 1)));
    const int whiteBin = static_cast<int>(std::ceil(
        parameters.inputWhite * (bins.size() - 1)));
    const quint64 shadows = blackBin > 0 ? sumRange(bins, 0, blackBin - 1) : 0;
    const quint64 highlights = whiteBin < bins.size() - 1
        ? sumRange(bins, whiteBin + 1, bins.size() - 1)
        : 0;
    m_clippingLabel->setText(
        tr("Input clipping — shadows %1% • highlights %2%")
            .arg(100.0 * shadows / static_cast<double>(total), 0, 'f', 2)
            .arg(100.0 * highlights / static_cast<double>(total), 0, 'f', 2));
}

double LevelsEditorWidget::sampleComponent(const QColor &colour,
                                            const AdjustmentChannel channel) const
{
    switch (channel) {
    case AdjustmentChannel::Red: return colour.redF();
    case AdjustmentChannel::Green: return colour.greenF();
    case AdjustmentChannel::Blue: return colour.blueF();
    case AdjustmentChannel::Rgb:
        return colour.redF() * 0.2126 + colour.greenF() * 0.7152
            + colour.blueF() * 0.0722;
    }
    return 0.0;
}

void LevelsEditorWidget::applyEyedropperSample(const LevelsEyedropperRole role,
                                               const QColor &colour)
{
    if (role == LevelsEyedropperRole::None || !colour.isValid()) {
        return;
    }
    emit interactionStarted();
    const auto applyToChannel = [this, role, &colour](const AdjustmentChannel channel) {
        LevelsChannelParameters parameters = m_levels.channel(channel);
        const double sample = sampleComponent(colour, channel);
        if (role == LevelsEyedropperRole::Black) {
            parameters.inputBlack = std::min(sample,
                                              parameters.inputWhite - MinimumGap);
        } else if (role == LevelsEyedropperRole::White) {
            parameters.inputWhite = std::max(sample,
                                              parameters.inputBlack + MinimumGap);
        } else {
            const double span = std::max(MinimumGap,
                                         parameters.inputWhite - parameters.inputBlack);
            const double normalised = std::clamp(
                (sample - parameters.inputBlack) / span, 0.001, 0.999);
            parameters.gamma = std::clamp(std::log(normalised) / std::log(0.5),
                                           0.1,
                                           10.0);
        }
        parameters.normalise();
        m_levels.channel(channel) = parameters;
    };

    if (m_selectedChannel == AdjustmentChannel::Rgb) {
        applyToChannel(AdjustmentChannel::Red);
        applyToChannel(AdjustmentChannel::Green);
        applyToChannel(AdjustmentChannel::Blue);
    } else {
        applyToChannel(m_selectedChannel);
    }
    m_levels.normalise();
    refreshControls();
    emit levelsChanged(m_levels);
    emit interactionFinished();
}

} // namespace vfx
