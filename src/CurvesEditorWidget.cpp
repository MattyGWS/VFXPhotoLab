#include "CurvesEditorWidget.h"
#include "TonalMapping.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vfx {
namespace {

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

} // namespace

CurveCanvas::CurveCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(210);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setToolTip(tr("Click to add a point. Drag points to shape the curve. Delete or right-click removes an interior point."));
}

void CurveCanvas::setHistogram(const HistogramData &histogram)
{
    m_histogram = histogram;
    update();
}

void CurveCanvas::setChannel(const AdjustmentChannel channel)
{
    m_channel = channel;
    update();
}

void CurveCanvas::setCurve(const CurveChannelParameters &curve)
{
    m_curve = curve;
    m_curve.normalise();
    m_selectedPoint = std::clamp(m_selectedPoint, 0, static_cast<int>(m_curve.points.size()) - 1);
    update();
}

void CurveCanvas::setInterpolation(const CurveInterpolation interpolation)
{
    m_interpolation = interpolation;
    update();
}

void CurveCanvas::setLogarithmic(const bool logarithmic)
{
    m_logarithmic = logarithmic;
    update();
}

int CurveCanvas::selectedPoint() const
{
    return m_selectedPoint;
}

void CurveCanvas::selectPoint(const int index)
{
    const int safe = std::clamp(index, 0, static_cast<int>(m_curve.points.size()) - 1);
    if (safe == m_selectedPoint) return;
    m_selectedPoint = safe;
    emit selectedPointChanged(safe);
    update();
}

QRectF CurveCanvas::graphRect() const
{
    return QRectF(rect()).adjusted(9.0, 9.0, -9.0, -9.0);
}

QPointF CurveCanvas::pointPosition(const CurvePoint &point) const
{
    const QRectF graph = graphRect();
    return {graph.left() + point.input * graph.width(),
            graph.bottom() - point.output * graph.height()};
}

int CurveCanvas::hitPoint(const QPointF &position) const
{
    int nearest = -1;
    double distance = 10.0;
    for (int index = 0; index < m_curve.points.size(); ++index) {
        const double candidate = QLineF(position, pointPosition(m_curve.points[index])).length();
        if (candidate < distance) {
            distance = candidate;
            nearest = index;
        }
    }
    return nearest;
}

CurvePoint CurveCanvas::pointFromPosition(const QPointF &position) const
{
    const QRectF graph = graphRect();
    return {std::clamp((position.x() - graph.left()) / graph.width(), 0.0, 1.0),
            std::clamp((graph.bottom() - position.y()) / graph.height(), 0.0, 1.0)};
}

void CurveCanvas::moveSelected(CurvePoint point)
{
    if (m_selectedPoint < 0 || m_selectedPoint >= m_curve.points.size()) return;
    if (m_selectedPoint == 0) point.input = 0.0;
    else if (m_selectedPoint == m_curve.points.size() - 1) point.input = 1.0;
    else {
        point.input = std::clamp(point.input,
                                 m_curve.points[m_selectedPoint - 1].input + 1.0 / 65535.0,
                                 m_curve.points[m_selectedPoint + 1].input - 1.0 / 65535.0);
    }
    m_curve.points[m_selectedPoint] = point;
    m_curve.normalise();
    emit curveChanged(m_curve);
    emit selectedPointChanged(m_selectedPoint);
    update();
}

void CurveCanvas::removeSelected()
{
    if (m_selectedPoint <= 0 || m_selectedPoint >= m_curve.points.size() - 1) return;
    m_curve.points.remove(m_selectedPoint);
    m_selectedPoint = std::clamp(m_selectedPoint - 1, 0, static_cast<int>(m_curve.points.size()) - 1);
    emit curveChanged(m_curve);
    emit selectedPointChanged(m_selectedPoint);
    update();
}

void CurveCanvas::drawHistogram(QPainter &painter, const QRectF &graph)
{
    if (!m_histogram.isValid()) return;
    auto drawOne = [&](const QVector<quint64> &bins, QColor colour, const double opacity) {
        if (bins.isEmpty()) return;
        quint64 maximum = 0;
        for (const quint64 value : bins) maximum = std::max(maximum, value);
        if (maximum == 0) return;
        QPainterPath path;
        path.moveTo(graph.left(), graph.bottom());
        for (int pixel = 0; pixel <= static_cast<int>(graph.width()); ++pixel) {
            const int binCount = static_cast<int>(bins.size());
            const int first = std::clamp(static_cast<int>(pixel / graph.width() * binCount), 0, binCount - 1);
            const int last = std::clamp(static_cast<int>((pixel + 1.0) / graph.width() * binCount), first + 1, binCount);
            quint64 value = 0;
            for (int index = first; index < last; ++index) value = std::max(value, bins[index]);
            const double height = m_logarithmic
                ? std::log1p(static_cast<double>(value)) / std::log1p(static_cast<double>(maximum))
                : value / static_cast<double>(maximum);
            path.lineTo(graph.left() + pixel, graph.bottom() - height * graph.height());
        }
        path.lineTo(graph.right(), graph.bottom());
        path.closeSubpath();
        colour.setAlphaF(opacity);
        painter.fillPath(path, colour);
    };

    if (m_channel == AdjustmentChannel::Rgb) {
        drawOne(m_histogram.red, QColor(235, 82, 82), 0.18);
        drawOne(m_histogram.green, QColor(80, 205, 112), 0.18);
        drawOne(m_histogram.blue, QColor(90, 142, 245), 0.18);
        drawOne(m_histogram.luminance, palette().color(QPalette::Text), 0.22);
    } else {
        const QVector<quint64> *bins = m_channel == AdjustmentChannel::Red
            ? &m_histogram.red : m_channel == AdjustmentChannel::Green
                ? &m_histogram.green : &m_histogram.blue;
        drawOne(*bins, channelColour(m_channel, palette()), 0.42);
    }
}

void CurveCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF graph = graphRect();
    painter.fillRect(rect(), palette().color(QPalette::Base));
    painter.fillRect(graph, palette().color(QPalette::Window));
    painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
    for (int division = 0; division <= 4; ++division) {
        const double x = graph.left() + graph.width() * division / 4.0;
        const double y = graph.top() + graph.height() * division / 4.0;
        painter.drawLine(QPointF(x, graph.top()), QPointF(x, graph.bottom()));
        painter.drawLine(QPointF(graph.left(), y), QPointF(graph.right(), y));
    }
    drawHistogram(painter, graph);

    QPainterPath curvePath;
    for (int sample = 0; sample <= 256; ++sample) {
        const double input = sample / 256.0;
        const double output = evaluateCurveChannel(m_curve, m_interpolation, input);
        const QPointF point(graph.left() + input * graph.width(),
                            graph.bottom() - output * graph.height());
        if (sample == 0) curvePath.moveTo(point); else curvePath.lineTo(point);
    }
    painter.setPen(QPen(channelColour(m_channel, palette()), 2.0));
    painter.drawPath(curvePath);

    for (int index = 0; index < m_curve.points.size(); ++index) {
        const QPointF position = pointPosition(m_curve.points[index]);
        painter.setPen(QPen(index == m_selectedPoint
                                ? palette().color(QPalette::Highlight)
                                : palette().color(QPalette::Text), 1.5));
        painter.setBrush(index == m_selectedPoint
                             ? palette().color(QPalette::Highlight)
                             : palette().color(QPalette::Window));
        painter.drawEllipse(position, index == m_selectedPoint ? 5.5 : 4.5,
                            index == m_selectedPoint ? 5.5 : 4.5);
    }
}

void CurveCanvas::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::RightButton) {
        const int hit = hitPoint(event->position());
        if (hit > 0 && hit < m_curve.points.size() - 1) {
            m_selectedPoint = hit;
            emit interactionStarted();
            removeSelected();
            emit interactionFinished();
        }
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || !graphRect().contains(event->position())) {
        QWidget::mousePressEvent(event);
        return;
    }
    int hit = hitPoint(event->position());
    if (hit < 0 && m_curve.points.size() >= 64) {
        event->accept();
        return;
    }
    emit interactionStarted();
    if (hit < 0) {
        m_dragOffset = {};
        const CurvePoint point = pointFromPosition(event->position());
        int insertion = 1;
        while (insertion < m_curve.points.size()
               && m_curve.points[insertion].input < point.input) ++insertion;
        m_curve.points.insert(insertion, point);
        m_curve.normalise();
        m_selectedPoint = 0;
        double nearestDistance = std::numeric_limits<double>::max();
        for (int index = 0; index < m_curve.points.size(); ++index) {
            const CurvePoint &candidate = m_curve.points[index];
            const double distance = std::abs(candidate.input - point.input)
                + std::abs(candidate.output - point.output);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                m_selectedPoint = index;
            }
        }
        emit curveChanged(m_curve);
    } else {
        m_selectedPoint = hit;
        m_dragOffset = pointPosition(m_curve.points[hit]) - event->position();
    }
    m_dragging = true;
    emit selectedPointChanged(m_selectedPoint);
    if (hit < 0) {
        moveSelected(pointFromPosition(event->position()));
    }
    event->accept();
}

void CurveCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        moveSelected(pointFromPosition(event->position() + m_dragOffset));
        event->accept();
        return;
    }
    setCursor(hitPoint(event->position()) >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
    QWidget::mouseMoveEvent(event);
}

void CurveCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        moveSelected(pointFromPosition(event->position() + m_dragOffset));
        m_dragging = false;
        m_dragOffset = {};
        emit interactionFinished();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void CurveCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (m_selectedPoint > 0 && m_selectedPoint < m_curve.points.size() - 1) {
            emit interactionStarted();
            removeSelected();
            emit interactionFinished();
        }
        event->accept();
        return;
    }
    const double step = (event->modifiers() & Qt::ShiftModifier)
        ? 1.0 / 65535.0 : 1.0 / 255.0;
    CurvePoint point = m_curve.points.value(m_selectedPoint);
    bool handled = true;
    switch (event->key()) {
    case Qt::Key_Left: point.input -= step; break;
    case Qt::Key_Right: point.input += step; break;
    case Qt::Key_Down: point.output -= step; break;
    case Qt::Key_Up: point.output += step; break;
    default: handled = false; break;
    }
    if (handled) {
        emit interactionStarted();
        moveSelected(point);
        emit interactionFinished();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

CurvesEditorWidget::CurvesEditorWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(6);

    auto *toolbar = new QHBoxLayout;
    m_channelCombo = new QComboBox(this);
    for (int index = 0; index < 4; ++index) {
        const auto channel = static_cast<AdjustmentChannel>(index);
        m_channelCombo->addItem(channelName(channel), index);
    }
    m_scopeCombo = new QComboBox(this);
    m_scopeCombo->addItem(tr("Document"), static_cast<int>(HistogramScope::Document));
    m_scopeCombo->addItem(tr("Selection"), static_cast<int>(HistogramScope::Selection));
    m_interpolationCombo = new QComboBox(this);
    m_interpolationCombo->addItem(tr("Smooth"), static_cast<int>(CurveInterpolation::Smooth));
    m_interpolationCombo->addItem(tr("Linear"), static_cast<int>(CurveInterpolation::Linear));
    m_logarithmicCheck = new QCheckBox(tr("Log"), this);
    toolbar->addWidget(m_channelCombo, 1);
    toolbar->addWidget(m_scopeCombo, 1);
    toolbar->addWidget(m_interpolationCombo, 1);
    toolbar->addWidget(m_logarithmicCheck);
    root->addLayout(toolbar);

    m_canvas = new CurveCanvas(this);
    root->addWidget(m_canvas);

    auto *pointRow = new QGridLayout;
    auto *inputLabel = new QLabel(tr("Input"), this);
    auto *outputLabel = new QLabel(tr("Output"), this);
    inputLabel->setObjectName(QStringLiteral("MutedLabel"));
    outputLabel->setObjectName(QStringLiteral("MutedLabel"));
    m_inputSpin = new QDoubleSpinBox(this);
    m_outputSpin = new QDoubleSpinBox(this);
    for (QDoubleSpinBox *spin : {m_inputSpin, m_outputSpin}) {
        spin->setRange(0.0, 255.0);
        spin->setDecimals(0);
        spin->setSingleStep(1.0);
    }
    m_deleteButton = new QPushButton(tr("Delete Point"), this);
    m_sampleButton = new QPushButton(tr("Sample Point"), this);
    m_sampleButton->setToolTip(
        tr("Click the rendered image to add a point at the sampled input value for the selected channel."));
    auto *resetButton = new QPushButton(tr("Reset Channel"), this);
    auto *resetAllButton = new QPushButton(tr("Reset All"), this);
    pointRow->addWidget(inputLabel, 0, 0);
    pointRow->addWidget(outputLabel, 0, 1);
    pointRow->addWidget(m_inputSpin, 1, 0);
    pointRow->addWidget(m_outputSpin, 1, 1);
    pointRow->addWidget(m_deleteButton, 2, 0);
    pointRow->addWidget(m_sampleButton, 2, 1);
    pointRow->addWidget(resetButton, 3, 0);
    pointRow->addWidget(resetAllButton, 3, 1);
    root->addLayout(pointRow);

    m_histogramStatus = new QLabel(tr("Histogram pending"), this);
    m_histogramStatus->setObjectName(QStringLiteral("MutedLabel"));
    root->addWidget(m_histogramStatus);

    m_spinFinishTimer = new QTimer(this);
    m_spinFinishTimer->setSingleShot(true);
    m_spinFinishTimer->setInterval(350);
    connect(m_spinFinishTimer, &QTimer::timeout,
            this, &CurvesEditorWidget::finishSpinInteraction);

    connect(m_channelCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0) return;
        finishSpinInteraction();
        selectChannel(static_cast<AdjustmentChannel>(m_channelCombo->itemData(index).toInt()));
    });
    connect(m_scopeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0) emit histogramScopeChanged(static_cast<HistogramScope>(m_scopeCombo->itemData(index).toInt()));
    });
    connect(m_interpolationCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_updating || index < 0) return;
        finishSpinInteraction();
        emit interactionStarted();
        m_curves.interpolation = static_cast<CurveInterpolation>(m_interpolationCombo->itemData(index).toInt());
        m_canvas->setInterpolation(m_curves.interpolation);
        emit curvesChanged(m_curves);
        emit interactionFinished();
    });
    connect(m_logarithmicCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_updating) return;
        finishSpinInteraction();
        emit interactionStarted();
        m_curves.logarithmicHistogram = checked;
        m_canvas->setLogarithmic(checked);
        emit curvesChanged(m_curves);
        emit interactionFinished();
    });
    connect(m_canvas, &CurveCanvas::interactionStarted, this, [this] {
        finishSpinInteraction();
        emit interactionStarted();
    });
    connect(m_canvas, &CurveCanvas::curveChanged, this, [this](const CurveChannelParameters &curve) {
        updateSelectedCurve(curve);
    });
    connect(m_canvas, &CurveCanvas::interactionFinished, this, &CurvesEditorWidget::interactionFinished);
    connect(m_canvas, &CurveCanvas::selectedPointChanged, this, [this](int) { updatePointSpins(); });

    const auto spinChanged = [this] {
        if (m_updating) return;
        beginSpinInteraction();
        CurveChannelParameters curve = m_curves.channel(m_selectedChannel);
        const int index = m_canvas->selectedPoint();
        if (index >= 0 && index < curve.points.size()) {
            CurvePoint point = curve.points[index];
            point.input = m_inputSpin->value() / 255.0;
            point.output = m_outputSpin->value() / 255.0;
            if (index == 0) point.input = 0.0;
            if (index == curve.points.size() - 1) point.input = 1.0;
            if (index > 0 && index + 1 < curve.points.size()) {
                point.input = std::clamp(point.input,
                                         curve.points[index - 1].input + 1.0 / 65535.0,
                                         curve.points[index + 1].input - 1.0 / 65535.0);
            }
            curve.points[index] = point;
            updateSelectedCurve(curve);
            m_canvas->setCurve(curve);
            m_canvas->selectPoint(index);
        }
    };
    connect(m_inputSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, spinChanged);
    connect(m_outputSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, spinChanged);
    connect(m_inputSpin, &QDoubleSpinBox::editingFinished,
            this, &CurvesEditorWidget::finishSpinInteraction);
    connect(m_outputSpin, &QDoubleSpinBox::editingFinished,
            this, &CurvesEditorWidget::finishSpinInteraction);
    connect(m_deleteButton, &QPushButton::clicked, this, [this] {
        finishSpinInteraction();
        CurveChannelParameters curve = m_curves.channel(m_selectedChannel);
        const int index = m_canvas->selectedPoint();
        if (index <= 0 || index >= curve.points.size() - 1) return;
        emit interactionStarted();
        curve.points.remove(index);
        updateSelectedCurve(curve);
        m_canvas->setCurve(curve);
        m_canvas->selectPoint(std::max(0, index - 1));
        emit interactionFinished();
    });
    connect(m_sampleButton, &QPushButton::clicked, this, [this] {
        finishSpinInteraction();
        emit eyedropperRequested();
    });
    connect(resetButton, &QPushButton::clicked, this, &CurvesEditorWidget::resetSelectedChannel);
    connect(resetAllButton, &QPushButton::clicked, this, &CurvesEditorWidget::resetAllChannels);

    refreshControls();
}

void CurvesEditorWidget::setCurves(const CurvesParameters &curves)
{
    m_curves = curves;
    m_curves.normalise();
    refreshControls();
}

CurvesParameters CurvesEditorWidget::curves() const
{
    return m_curves;
}

void CurvesEditorWidget::setHistogram(const HistogramData &histogram)
{
    m_histogram = histogram;
    m_canvas->setHistogram(histogram);
    m_histogramStatus->setText(histogram.isValid()
        ? tr("Exact %1-bit input histogram · %2 pixels")
              .arg(histogram.sourceBitDepth).arg(histogram.includedPixels)
        : tr("Histogram unavailable"));
}

HistogramScope CurvesEditorWidget::histogramScope() const
{
    return static_cast<HistogramScope>(m_scopeCombo->currentData().toInt());
}

void CurvesEditorWidget::applyEyedropperSample(const QColor &colour)
{
    if (!colour.isValid()) return;
    finishSpinInteraction();

    double input = 0.0;
    switch (m_selectedChannel) {
    case AdjustmentChannel::Rgb:
        // Curves' composite channel is sampled using the same encoded Rec.709
        // luminance convention as its histogram, while component channels use
        // their exact straight-RGB code value.
        input = colour.redF() * 0.2126
            + colour.greenF() * 0.7152
            + colour.blueF() * 0.0722;
        break;
    case AdjustmentChannel::Red: input = colour.redF(); break;
    case AdjustmentChannel::Green: input = colour.greenF(); break;
    case AdjustmentChannel::Blue: input = colour.blueF(); break;
    }
    input = std::clamp(input, 0.0, 1.0);

    CurveChannelParameters curve = m_curves.channel(m_selectedChannel);
    curve.normalise();
    constexpr double ExistingPointTolerance = 1.5 / 255.0;
    int nearest = -1;
    double nearestDistance = std::numeric_limits<double>::max();
    for (int index = 0; index < curve.points.size(); ++index) {
        const double distance = std::abs(curve.points[index].input - input);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = index;
        }
    }
    if (nearest >= 0 && nearestDistance <= ExistingPointTolerance) {
        m_canvas->selectPoint(nearest);
        updatePointSpins();
        return;
    }
    if (curve.points.size() >= 64) return;

    const double output = evaluateCurveChannel(curve, m_curves.interpolation, input);
    emit interactionStarted();
    curve.points.push_back({input, output});
    curve.normalise();
    int selected = 0;
    nearestDistance = std::numeric_limits<double>::max();
    for (int index = 0; index < curve.points.size(); ++index) {
        const double distance = std::abs(curve.points[index].input - input);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            selected = index;
        }
    }
    updateSelectedCurve(curve);
    m_canvas->setCurve(curve);
    m_canvas->selectPoint(selected);
    emit interactionFinished();
}

void CurvesEditorWidget::selectChannel(const AdjustmentChannel channel)
{
    m_selectedChannel = channel;
    m_canvas->setChannel(channel);
    m_canvas->setCurve(m_curves.channel(channel));
    m_canvas->selectPoint(0);
    updatePointSpins();
}

void CurvesEditorWidget::refreshControls()
{
    m_updating = true;
    m_channelCombo->setCurrentIndex(static_cast<int>(m_selectedChannel));
    m_interpolationCombo->setCurrentIndex(m_curves.interpolation == CurveInterpolation::Linear ? 1 : 0);
    m_logarithmicCheck->setChecked(m_curves.logarithmicHistogram);
    m_canvas->setChannel(m_selectedChannel);
    m_canvas->setCurve(m_curves.channel(m_selectedChannel));
    m_canvas->setInterpolation(m_curves.interpolation);
    m_canvas->setLogarithmic(m_curves.logarithmicHistogram);
    m_canvas->setHistogram(m_histogram);
    m_updating = false;
    updatePointSpins();
}

void CurvesEditorWidget::updateSelectedCurve(const CurveChannelParameters &curve, const bool emitChange)
{
    CurveChannelParameters safe = curve;
    safe.normalise();
    m_curves.channel(m_selectedChannel) = safe;
    m_curves.normalise();
    if (emitChange) emit curvesChanged(m_curves);
    updatePointSpins();
}

void CurvesEditorWidget::beginSpinInteraction()
{
    if (!m_spinInteractionActive) {
        m_spinInteractionActive = true;
        emit interactionStarted();
    }
    m_spinFinishTimer->start();
}

void CurvesEditorWidget::finishSpinInteraction()
{
    if (!m_spinInteractionActive) return;
    m_spinInteractionActive = false;
    emit interactionFinished();
}

void CurvesEditorWidget::updatePointSpins()
{
    const CurveChannelParameters &curve = m_curves.channel(m_selectedChannel);
    const int index = std::clamp(m_canvas->selectedPoint(), 0, static_cast<int>(curve.points.size()) - 1);
    const CurvePoint point = curve.points[index];
    m_updating = true;
    m_inputSpin->setValue(point.input * 255.0);
    m_outputSpin->setValue(point.output * 255.0);
    m_inputSpin->setEnabled(index > 0 && index < curve.points.size() - 1);
    m_deleteButton->setEnabled(index > 0 && index < curve.points.size() - 1);
    m_updating = false;
}

void CurvesEditorWidget::resetSelectedChannel()
{
    finishSpinInteraction();
    emit interactionStarted();
    m_curves.channel(m_selectedChannel) = CurveChannelParameters {};
    m_curves.normalise();
    m_canvas->setCurve(m_curves.channel(m_selectedChannel));
    m_canvas->selectPoint(0);
    emit curvesChanged(m_curves);
    emit interactionFinished();
}

void CurvesEditorWidget::resetAllChannels()
{
    finishSpinInteraction();
    emit interactionStarted();
    m_curves = CurvesParameters {};
    m_curves.normalise();
    refreshControls();
    emit curvesChanged(m_curves);
    emit interactionFinished();
}

} // namespace vfx
