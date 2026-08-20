#include "GradientMapEditorWidget.h"

#include "SliderSpinBox.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QPixmap>
#include <QPolygon>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace vfx {
namespace {

constexpr double MinimumStopGap = 1.0 / 65535.0;

QColor mixedColour(const QColor &left, const QColor &right, const double amount)
{
    const double t = std::clamp(amount, 0.0, 1.0);
    QColor result;
    result.setRgbF(left.redF() + (right.redF() - left.redF()) * t,
                   left.greenF() + (right.greenF() - left.greenF()) * t,
                   left.blueF() + (right.blueF() - left.blueF()) * t,
                   1.0);
    return result;
}

} // namespace

GradientBarWidget::GradientBarWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(64);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(tr("Click to add or select stops. Drag to move. Delete removes an interior stop; arrows nudge it, Ctrl+arrows select adjacent stops, and Enter edits colour."));
}

void GradientBarWidget::setParameters(const GradientMapParameters &parameters)
{
    m_parameters = parameters;
    m_parameters.normalise();
    m_selectedIndex = std::clamp(m_selectedIndex, 0, static_cast<int>(m_parameters.stops.size()) - 1);
    update();
    emit selectionChanged(m_selectedIndex);
}

QRect GradientBarWidget::gradientRect() const
{
    return rect().adjusted(8, 7, -8, -22);
}

int GradientBarWidget::nearestStop(const QPoint &position, const int maximumDistance) const
{
    const QRect area = gradientRect();
    if (area.width() <= 0) return -1;
    const int span = std::max(1, area.width() - 1);
    int best = -1;
    int bestDistance = maximumDistance + 1;
    for (int index = 0; index < static_cast<int>(m_parameters.stops.size()); ++index) {
        const int x = area.left() + qRound(m_parameters.stops[index].position * span);
        const int distance = std::abs(position.x() - x);
        if (distance <= maximumDistance && distance < bestDistance) {
            best = index;
            bestDistance = distance;
        }
    }
    return best;
}

QColor GradientBarWidget::colourAt(const double position) const
{
    // m_parameters is normalised whenever it enters or changes in the widget.
    // Avoid copying and re-sorting the stop vector for every painted column.
    // The editor displays the stored gradient left-to-right; Reverse changes
    // only how document luminance addresses it.
    const double source = std::clamp(position, 0.0, 1.0);
    const auto &stops = m_parameters.stops;
    if (source <= stops.first().position) return stops.first().colour;
    if (source >= stops.last().position) return stops.last().colour;
    int right = 1;
    while (right < static_cast<int>(stops.size()) && stops[right].position < source) ++right;
    const GradientStop &a = stops[std::max(0, right - 1)];
    const GradientStop &b = stops[std::min(right, static_cast<int>(stops.size()) - 1)];
    double t = (source - a.position) / std::max(MinimumStopGap, b.position - a.position);
    if (m_parameters.interpolation == GradientInterpolation::Constant) {
        t = source >= b.position ? 1.0 : 0.0;
    }
    else if (m_parameters.interpolation == GradientInterpolation::Smooth) t = t * t * (3.0 - 2.0 * t);
    return mixedColour(a.colour, b.colour, t);
}

bool GradientBarWidget::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::UngrabMouse:
    case QEvent::WindowDeactivate:
    case QEvent::Hide:
    case QEvent::EnabledChange:
        finishDrag();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void GradientBarWidget::finishDrag()
{
    if (!m_dragging) return;
    m_dragging = false;
    unsetCursor();
    if (QWidget::mouseGrabber() == this) releaseMouse();
    emit interactionFinished();
}

void GradientBarWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QRect area = gradientRect();

    const int checker = 7;
    for (int y = area.top(); y <= area.bottom(); y += checker) {
        for (int x = area.left(); x <= area.right(); x += checker) {
            const bool dark = ((x - area.left()) / checker + (y - area.top()) / checker) % 2;
            painter.fillRect(QRect(x, y, checker, checker), dark ? QColor(90, 90, 90) : QColor(145, 145, 145));
        }
    }
    const int span = std::max(1, area.width() - 1);
    for (int x = 0; x < area.width(); ++x) {
        painter.setPen(colourAt(x / static_cast<double>(std::max(1, area.width() - 1))));
        painter.drawLine(area.left() + x, area.top(), area.left() + x, area.bottom());
    }
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawRect(area.adjusted(0, 0, -1, -1));

    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int index = 0; index < static_cast<int>(m_parameters.stops.size()); ++index) {
        const int x = area.left() + qRound(m_parameters.stops[index].position * span);
        QPolygon marker;
        marker << QPoint(x, area.bottom() + 2)
               << QPoint(x - 6, area.bottom() + 12)
               << QPoint(x + 6, area.bottom() + 12);
        painter.setBrush(m_parameters.stops[index].colour);
        painter.setPen(index == m_selectedIndex
                           ? palette().color(QPalette::Highlight)
                           : palette().color(QPalette::Text));
        painter.drawPolygon(marker);
        if (index == m_selectedIndex) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
            painter.drawPolygon(marker.translated(0, 1));
        }
    }
}

void GradientBarWidget::selectIndex(const int index)
{
    const int bounded = std::clamp(index, 0, static_cast<int>(m_parameters.stops.size()) - 1);
    if (bounded == m_selectedIndex) return;
    m_selectedIndex = bounded;
    update();
    emit selectionChanged(m_selectedIndex);
}

void GradientBarWidget::emitChanged()
{
    m_parameters.normalise();
    m_selectedIndex = std::clamp(m_selectedIndex, 0, static_cast<int>(m_parameters.stops.size()) - 1);
    update();
    emit parametersChanged(m_parameters);
    emit selectionChanged(m_selectedIndex);
}

void GradientBarWidget::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::RightButton) {
        const int index = nearestStop(event->position().toPoint());
        if (index >= 0) selectIndex(index);
        removeSelectedStop();
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    const QRect area = gradientRect();
    int index = nearestStop(event->position().toPoint());
    bool created = false;
    if (index < 0 && area.contains(event->position().toPoint())) {
        if (m_parameters.stops.size() >= qsizetype(64)) {
            event->accept();
            return;
        }
        const double position = std::clamp((event->position().x() - area.left())
                                           / static_cast<double>(std::max(1, area.width() - 1)),
                                           0.0, 1.0);
        emit interactionStarted();
        GradientStop stop {position, colourAt(position)};
        m_parameters.stops.push_back(stop);
        m_parameters.normalise();
        index = 0;
        double best = 2.0;
        for (int candidate = 0; candidate < static_cast<int>(m_parameters.stops.size()); ++candidate) {
            const double distance = std::abs(m_parameters.stops[candidate].position - position);
            if (distance < best) { best = distance; index = candidate; }
        }
        m_selectedIndex = index;
        emitChanged();
        emit interactionFinished();
        created = true;
    } else if (index >= 0) {
        selectIndex(index);
    }
    if (index > 0 && index < static_cast<int>(m_parameters.stops.size()) - 1 && !created) {
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        grabMouse();
        emit interactionStarted();
    }
    event->accept();
}

void GradientBarWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) return;
    if (!(event->buttons() & Qt::LeftButton)
        || !(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
        finishDrag();
        return;
    }
    if (m_selectedIndex <= 0 || m_selectedIndex >= static_cast<int>(m_parameters.stops.size()) - 1) return;
    const QRect area = gradientRect();
    const double raw = (event->position().x() - area.left())
        / static_cast<double>(std::max(1, area.width() - 1));
    const double minimum = m_parameters.stops[m_selectedIndex - 1].position + MinimumStopGap;
    const double maximum = m_parameters.stops[m_selectedIndex + 1].position - MinimumStopGap;
    m_parameters.stops[m_selectedIndex].position = std::clamp(raw, minimum, maximum);
    emitChanged();
}

void GradientBarWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_dragging) return;
    finishDrag();
    event->accept();
}

void GradientBarWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    const int index = nearestStop(event->position().toPoint());
    if (index >= 0) {
        selectIndex(index);
        emit colourEditRequested();
    }
}

void GradientBarWidget::keyPressEvent(QKeyEvent *event)
{
    if (m_parameters.stops.isEmpty()) {
        QWidget::keyPressEvent(event);
        return;
    }

    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        removeSelectedStop();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        if (event->modifiers() & Qt::ControlModifier) {
            const int direction = event->key() == Qt::Key_Left ? -1 : 1;
            selectIndex(std::clamp(m_selectedIndex + direction,
                                   0,
                                   static_cast<int>(m_parameters.stops.size()) - 1));
        } else if (m_selectedIndex > 0
                   && m_selectedIndex < static_cast<int>(m_parameters.stops.size()) - 1) {
            const double step = event->modifiers() & Qt::ShiftModifier
                ? 1.0 / 65535.0 : 0.001;
            const double direction = event->key() == Qt::Key_Left ? -1.0 : 1.0;
            emit interactionStarted();
            setSelectedPosition(m_parameters.stops[m_selectedIndex].position
                                + direction * step);
            emit interactionFinished();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        emit colourEditRequested();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

QSize GradientBarWidget::sizeHint() const
{
    return {280, 72};
}

void GradientBarWidget::setSelectedPosition(const double position)
{
    if (m_selectedIndex <= 0 || m_selectedIndex >= static_cast<int>(m_parameters.stops.size()) - 1) return;
    const double minimum = m_parameters.stops[m_selectedIndex - 1].position + MinimumStopGap;
    const double maximum = m_parameters.stops[m_selectedIndex + 1].position - MinimumStopGap;
    m_parameters.stops[m_selectedIndex].position = std::clamp(position, minimum, maximum);
    emitChanged();
}

void GradientBarWidget::setSelectedColour(const QColor &colour)
{
    if (!colour.isValid() || m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_parameters.stops.size())) return;
    QColor opaque = colour;
    opaque.setAlpha(255);
    m_parameters.stops[m_selectedIndex].colour = opaque;
    emitChanged();
}

void GradientBarWidget::addStop()
{
    if (m_parameters.stops.size() >= qsizetype(64)) return;
    const int right = std::min(m_selectedIndex + 1, static_cast<int>(m_parameters.stops.size()) - 1);
    const int left = std::max(0, right - 1);
    const double position = (m_parameters.stops[left].position + m_parameters.stops[right].position) * 0.5;
    emit interactionStarted();
    m_parameters.stops.push_back({position, colourAt(position)});
    m_parameters.normalise();
    for (int index = 0; index < static_cast<int>(m_parameters.stops.size()); ++index) {
        if (std::abs(m_parameters.stops[index].position - position) < MinimumStopGap * 2.0) {
            m_selectedIndex = index;
            break;
        }
    }
    emitChanged();
    emit interactionFinished();
}

void GradientBarWidget::duplicateSelectedStop()
{
    if (m_parameters.stops.size() >= qsizetype(64)
        || m_selectedIndex < 0
        || m_selectedIndex >= static_cast<int>(m_parameters.stops.size())) {
        return;
    }
    const GradientStop selected = m_parameters.stops[m_selectedIndex];
    const double left = m_selectedIndex > 0
        ? m_parameters.stops[m_selectedIndex - 1].position : 0.0;
    const double right = m_selectedIndex + 1 < static_cast<int>(m_parameters.stops.size())
        ? m_parameters.stops[m_selectedIndex + 1].position : 1.0;
    double position = selected.position;
    if (m_selectedIndex + 1 < static_cast<int>(m_parameters.stops.size())) {
        position = (selected.position + right) * 0.5;
    } else if (m_selectedIndex > 0) {
        position = (left + selected.position) * 0.5;
    }
    if (position <= MinimumStopGap || position >= 1.0 - MinimumStopGap) return;

    emit interactionStarted();
    m_parameters.stops.push_back({position, selected.colour});
    m_parameters.normalise();
    int selectedIndex = 0;
    double best = 2.0;
    for (int index = 0; index < static_cast<int>(m_parameters.stops.size()); ++index) {
        const double distance = std::abs(m_parameters.stops[index].position - position);
        if (distance < best) {
            best = distance;
            selectedIndex = index;
        }
    }
    m_selectedIndex = selectedIndex;
    emitChanged();
    emit interactionFinished();
}

void GradientBarWidget::distributeStopsEvenly()
{
    if (m_parameters.stops.size() <= qsizetype(2)) return;
    emit interactionStarted();
    const int last = static_cast<int>(m_parameters.stops.size()) - 1;
    for (int index = 1; index < last; ++index) {
        m_parameters.stops[index].position = index / static_cast<double>(last);
    }
    emitChanged();
    emit interactionFinished();
}

void GradientBarWidget::removeSelectedStop()
{
    if (m_parameters.stops.size() <= qsizetype(2) || m_selectedIndex <= 0
        || m_selectedIndex >= static_cast<int>(m_parameters.stops.size()) - 1) return;
    emit interactionStarted();
    m_parameters.stops.remove(m_selectedIndex);
    m_selectedIndex = std::clamp(m_selectedIndex - 1, 0, static_cast<int>(m_parameters.stops.size()) - 1);
    emitChanged();
    emit interactionFinished();
}

GradientMapEditorWidget::GradientMapEditorWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    m_bar = new GradientBarWidget(this);
    layout->addWidget(m_bar);

    auto *positionRow = new QHBoxLayout;
    positionRow->addWidget(new QLabel(tr("Stop position"), this));
    m_position = new SliderSpinBox(this);
    m_position->configure(0.0, 100.0, 0.1, 1);
    positionRow->addWidget(m_position, 1);
    layout->addLayout(positionRow);

    auto *buttonRow = new QHBoxLayout;
    m_colour = new QPushButton(tr("Stop Colour…"), this);
    m_add = new QPushButton(tr("Add Stop"), this);
    m_duplicate = new QPushButton(tr("Duplicate"), this);
    m_remove = new QPushButton(tr("Remove"), this);
    buttonRow->addWidget(m_colour, 1);
    buttonRow->addWidget(m_add);
    buttonRow->addWidget(m_duplicate);
    buttonRow->addWidget(m_remove);
    layout->addLayout(buttonRow);

    m_distribute = new QPushButton(tr("Distribute Stops Evenly"), this);
    m_distribute->setToolTip(
        tr("Space all interior stops evenly while preserving their colours and order."));
    layout->addWidget(m_distribute);

    m_reverse = new QCheckBox(tr("Reverse gradient"), this);
    layout->addWidget(m_reverse);

    auto *interpolationRow = new QHBoxLayout;
    interpolationRow->addWidget(new QLabel(tr("Interpolation"), this));
    m_interpolation = new QComboBox(this);
    m_interpolation->addItem(tr("Linear"), static_cast<int>(GradientInterpolation::Linear));
    m_interpolation->addItem(tr("Smooth"), static_cast<int>(GradientInterpolation::Smooth));
    m_interpolation->addItem(tr("Constant"), static_cast<int>(GradientInterpolation::Constant));
    interpolationRow->addWidget(m_interpolation, 1);
    layout->addLayout(interpolationRow);

    connect(m_bar, &GradientBarWidget::parametersChanged, this,
            [this](const GradientMapParameters &parameters) {
        if (m_updating) return;
        // Do not continuously move the separate position slider underneath
        // the pointer while a stop is being dragged on the gradient itself.
        // Synchronise it once when that gesture completes instead.
        if (!m_barInteractionActive) syncControls();
        emit gradientChanged(parameters);
    });
    connect(m_bar, &GradientBarWidget::selectionChanged, this,
            [this](int) {
        if (!m_barInteractionActive) syncControls();
    });
    connect(m_bar, &GradientBarWidget::interactionStarted, this, [this] {
        m_barInteractionActive = true;
        emit interactionStarted();
    });
    connect(m_bar, &GradientBarWidget::interactionFinished, this, [this] {
        if (!m_barInteractionActive) return;
        m_barInteractionActive = false;
        syncControls();
        emit interactionFinished();
    });
    connect(m_bar, &GradientBarWidget::colourEditRequested,
            this, &GradientMapEditorWidget::chooseSelectedColour);

    connect(m_position, &SliderSpinBox::interactionStarted,
            this, &GradientMapEditorWidget::interactionStarted);
    connect(m_position, &SliderSpinBox::interactionFinished,
            this, &GradientMapEditorWidget::interactionFinished);
    connect(m_position, &SliderSpinBox::valueChanged, this, [this](const double value) {
        if (!m_updating) m_bar->setSelectedPosition(value / 100.0);
    });
    connect(m_colour, &QPushButton::clicked,
            this, &GradientMapEditorWidget::chooseSelectedColour);
    connect(m_add, &QPushButton::clicked, m_bar, &GradientBarWidget::addStop);
    connect(m_duplicate, &QPushButton::clicked,
            m_bar, &GradientBarWidget::duplicateSelectedStop);
    connect(m_remove, &QPushButton::clicked, m_bar, &GradientBarWidget::removeSelectedStop);
    connect(m_distribute, &QPushButton::clicked,
            m_bar, &GradientBarWidget::distributeStopsEvenly);

    connect(m_reverse, &QCheckBox::toggled, this, [this](const bool checked) {
        if (m_updating) return;
        emit interactionStarted();
        GradientMapParameters updated = m_bar->parameters();
        updated.reverse = checked;
        m_bar->setParameters(updated);
        emit gradientChanged(updated);
        emit interactionFinished();
    });
    connect(m_interpolation, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (m_updating) return;
        emit interactionStarted();
        GradientMapParameters updated = m_bar->parameters();
        updated.interpolation = static_cast<GradientInterpolation>(m_interpolation->currentData().toInt());
        m_bar->setParameters(updated);
        emit gradientChanged(updated);
        emit interactionFinished();
    });

    setParameters(GradientMapParameters {});
}

void GradientMapEditorWidget::setParameters(const GradientMapParameters &parameters)
{
    m_updating = true;
    m_bar->setParameters(parameters);
    syncControls();
    m_updating = false;
}

GradientMapParameters GradientMapEditorWidget::parameters() const
{
    return m_bar->parameters();
}

void GradientMapEditorWidget::syncControls()
{
    const bool wasUpdating = m_updating;
    m_updating = true;
    const GradientMapParameters parameters = m_bar->parameters();
    const int index = std::clamp(m_bar->selectedIndex(), 0, static_cast<int>(parameters.stops.size()) - 1);
    const GradientStop &stop = parameters.stops[index];
    m_position->setValue(stop.position * 100.0);
    m_position->setEnabled(index > 0 && index < static_cast<int>(parameters.stops.size()) - 1);
    m_add->setEnabled(parameters.stops.size() < qsizetype(64));
    m_duplicate->setEnabled(parameters.stops.size() < qsizetype(64));
    m_remove->setEnabled(parameters.stops.size() > qsizetype(2) && index > 0 && index < static_cast<int>(parameters.stops.size()) - 1);
    m_distribute->setEnabled(parameters.stops.size() > qsizetype(2));
    QPixmap swatch(16, 16);
    swatch.fill(Qt::transparent);
    {
        QPainter painter(&swatch);
        painter.fillRect(QRect(1, 1, 14, 14), stop.colour);
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawRect(QRect(1, 1, 13, 13));
    }
    m_colour->setStyleSheet(QString());
    m_colour->setIcon(QIcon(swatch));
    m_colour->setIconSize(QSize(16, 16));
    m_reverse->setChecked(parameters.reverse);
    const int comboIndex = m_interpolation->findData(static_cast<int>(parameters.interpolation));
    if (comboIndex >= 0) m_interpolation->setCurrentIndex(comboIndex);
    m_updating = wasUpdating;
}

void GradientMapEditorWidget::chooseSelectedColour()
{
    const GradientMapParameters parameters = m_bar->parameters();
    const int index = std::clamp(m_bar->selectedIndex(), 0, static_cast<int>(parameters.stops.size()) - 1);
    const QColor colour = QColorDialog::getColor(parameters.stops[index].colour,
                                                  this, tr("Choose Gradient Stop Colour"));
    if (!colour.isValid()) return;
    emit interactionStarted();
    m_bar->setSelectedColour(colour);
    emit interactionFinished();
}

} // namespace vfx
