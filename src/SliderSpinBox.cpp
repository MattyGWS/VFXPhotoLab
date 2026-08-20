#include "SliderSpinBox.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyleHints>

#include <algorithm>
#include <cmath>
#include <functional>

namespace vfx {

namespace {

constexpr int kScrubFieldHeight = 30;

class ScrubDoubleSpinBox;

class ProgressLineEdit final : public QLineEdit {
public:
    explicit ProgressLineEdit(ScrubDoubleSpinBox *owner);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    ScrubDoubleSpinBox *m_owner = nullptr;
};

class ScrubDoubleSpinBox final : public QDoubleSpinBox {
public:
    explicit ScrubDoubleSpinBox(QWidget *parent = nullptr)
        : QDoubleSpinBox(parent)
    {
        setObjectName(QStringLiteral("ScrubbableNumericField"));
        setLineEdit(new ProgressLineEdit(this));
        setKeyboardTracking(false);
        setAccelerated(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(kScrubFieldHeight);
        setMinimumWidth(86);
        lineEdit()->installEventFilter(this);
        connect(this, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [this](double) { lineEdit()->update(); });
        connect(this, &QDoubleSpinBox::editingFinished,
                this, [this] { m_focusStartValue = value(); });
    }

    bool scrubbing() const { return m_scrubbing; }

    std::function<void()> scrubStartedCallback;
    std::function<void()> scrubFinishedCallback;
    std::function<void()> editCancelledCallback;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != lineEdit()) {
            return QDoubleSpinBox::eventFilter(watched, event);
        }

        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() != Qt::LeftButton) break;
            m_scrubCandidate = true;
            m_scrubbing = false;
            m_pressGlobal = mouse->globalPosition();
            m_pressValue = value();
            break;
        }
        case QEvent::MouseMove: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (!m_scrubCandidate || !(mouse->buttons() & Qt::LeftButton)) break;
            const qreal distance = std::abs(mouse->globalPosition().x()
                                            - m_pressGlobal.x());
            if (!m_scrubbing
                && distance >= QApplication::styleHints()->startDragDistance()) {
                m_scrubbing = true;
                lineEdit()->deselect();
                lineEdit()->setCursor(Qt::SizeHorCursor);
                lineEdit()->grabMouse();
                if (scrubStartedCallback) scrubStartedCallback();
            }
            if (!m_scrubbing) break;

            double sensitivity = scrubSensitivity();
            if (mouse->modifiers() & Qt::ShiftModifier) {
                sensitivity *= 0.1;
            }
            if (mouse->modifiers() & Qt::ControlModifier) {
                sensitivity *= 10.0;
            }
            const double delta = mouse->globalPosition().x() - m_pressGlobal.x();
            setValue(std::clamp(m_pressValue + delta * sensitivity,
                                minimum(), maximum()));
            return true;
        }
        case QEvent::MouseButtonRelease: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() != Qt::LeftButton) break;
            const bool finishedScrub = m_scrubbing;
            m_scrubCandidate = false;
            m_scrubbing = false;
            if (finishedScrub) {
                if (QWidget::mouseGrabber() == lineEdit()) {
                    lineEdit()->releaseMouse();
                }
                lineEdit()->unsetCursor();
                if (scrubFinishedCallback) scrubFinishedCallback();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonDblClick: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                lineEdit()->setFocus(Qt::MouseFocusReason);
                lineEdit()->selectAll();
                return true;
            }
            break;
        }
        case QEvent::FocusIn:
            m_focusStartValue = value();
            break;
        case QEvent::KeyPress: {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape) {
                // Re-publish the focus-entry value when an arrow or scrub
                // already changed the bound property. Uncommitted typed text
                // has not changed value() because keyboard tracking is off.
                setValue(m_focusStartValue);
                lineEdit()->setText(prefix()
                                    + textFromValue(m_focusStartValue)
                                    + suffix());
                lineEdit()->selectAll();
                lineEdit()->update();
                if (editCancelledCallback) editCancelledCallback();
                return true;
            }
            break;
        }
        case QEvent::UngrabMouse:
        case QEvent::WindowDeactivate:
        case QEvent::Hide:
            finishInterruptedScrub();
            break;
        default:
            break;
        }
        return QDoubleSpinBox::eventFilter(watched, event);
    }

    void focusInEvent(QFocusEvent *event) override
    {
        m_focusStartValue = value();
        QDoubleSpinBox::focusInEvent(event);
    }

private:
    double scrubSensitivity() const
    {
        const double span = maximum() - minimum();
        double sensitivity = singleStep();
        if (span <= 10'000.0) {
            // Ordinary bounded controls should cross their useful range in a
            // comfortable gesture instead of requiring thousands of pixels.
            const double travel = std::max(240.0,
                static_cast<double>(lineEdit() ? lineEdit()->width() * 2 : 240));
            sensitivity = std::max(sensitivity, span / travel);
        } else {
            // Very large technical bounds (for example vector stroke widths)
            // are not meaningful as a linear progress scale. Keep fine control
            // near small values and grow sensitivity with the current order of
            // magnitude; exact typing remains available at every value.
            sensitivity = std::max(sensitivity,
                std::max(1.0, std::abs(m_pressValue)) * 0.01);
        }
        return sensitivity;
    }

    void finishInterruptedScrub()
    {
        if (!m_scrubCandidate && !m_scrubbing) return;
        const bool wasScrubbing = m_scrubbing;
        m_scrubCandidate = false;
        m_scrubbing = false;
        if (lineEdit()) lineEdit()->unsetCursor();
        if (wasScrubbing && scrubFinishedCallback) {
            scrubFinishedCallback();
        }
    }

    bool m_scrubCandidate = false;
    bool m_scrubbing = false;
    QPointF m_pressGlobal;
    double m_pressValue = 0.0;
    double m_focusStartValue = 0.0;
};

ProgressLineEdit::ProgressLineEdit(ScrubDoubleSpinBox *owner)
    : QLineEdit(owner), m_owner(owner)
{
    setObjectName(QStringLiteral("ScrubbableNumericLineEdit"));
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    // The line edit paints its own range fill. Keep Qt's text/cursor/selection
    // handling, but do not let the global QLineEdit background cover the fill.
    setStyleSheet(QStringLiteral(
        "QLineEdit#ScrubbableNumericLineEdit { background: transparent; border: none; padding: 0; }"));
}

void ProgressLineEdit::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setClipRect(rect());
    painter.fillRect(rect(), palette().color(QPalette::Base));

    if (m_owner && m_owner->maximum() > m_owner->minimum()) {
        const double minimum = m_owner->minimum();
        const double maximum = m_owner->maximum();
        const double span = maximum - minimum;
        const auto xForValue = [this, minimum, span](const double value) {
            const double fraction = std::clamp((value - minimum) / span, 0.0, 1.0);
            return qRound(rect().left() + fraction * rect().width());
        };

        QColor progress = palette().color(QPalette::Highlight);
        progress.setAlpha(isEnabled() ? (hasFocus() ? 150 : 105) : 50);
        if (minimum < 0.0 && maximum > 0.0) {
            // Signed controls read naturally around their neutral point: zero
            // stays visibly centred in the range and the fill grows left for
            // negative values or right for positive values.
            const int zeroX = xForValue(0.0);
            const int valueX = xForValue(m_owner->value());
            QRect fill = rect();
            fill.setLeft(std::min(zeroX, valueX));
            fill.setRight(std::max(zeroX, valueX));
            painter.fillRect(fill, progress);

            QColor zeroMarker = palette().color(QPalette::Text);
            zeroMarker.setAlpha(isEnabled() ? 85 : 35);
            painter.setPen(zeroMarker);
            painter.drawLine(zeroX, rect().top() + 2,
                             zeroX, rect().bottom() - 2);
        } else {
            QRect fill = rect();
            fill.setWidth(std::max(0, xForValue(m_owner->value()) - rect().left()));
            painter.fillRect(fill, progress);
        }
    }
    painter.end();
    QLineEdit::paintEvent(event);
}

} // namespace

SliderSpinBox::SliderSpinBox(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *spin = new ScrubDoubleSpinBox(this);
    m_spin = spin;
    layout->addWidget(m_spin);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setControlHeight(kScrubFieldHeight);
    setMinimumWidth(86);

    spin->scrubStartedCallback = [this] {
        if (m_interactionActive) return;
        m_interactionActive = true;
        m_scrubInteraction = true;
        emit interactionStarted();
    };
    spin->scrubFinishedCallback = [this] {
        if (!m_interactionActive || !m_scrubInteraction) return;
        m_interactionActive = false;
        m_scrubInteraction = false;
        emit interactionFinished();
    };
    spin->editCancelledCallback = [this] {
        if (!m_interactionActive) return;
        m_interactionActive = false;
        m_scrubInteraction = false;
        emit interactionFinished();
    };

    connect(m_spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this, spin](const double actual) {
        if (!spin->scrubbing() && !m_interactionActive) {
            m_interactionActive = true;
            m_scrubInteraction = false;
            emit interactionStarted();
        }
        emit valueChanged(actual);
    });
    connect(m_spin, &QDoubleSpinBox::editingFinished, this, [this] {
        if (!m_interactionActive || m_scrubInteraction) return;
        m_interactionActive = false;
        emit interactionFinished();
    });
}

void SliderSpinBox::configure(const double minimum,
                              const double maximum,
                              const double step,
                              const int decimals)
{
    m_minimum = std::min(minimum, maximum);
    m_maximum = std::max(minimum, maximum);
    const QSignalBlocker blocker(m_spin);
    m_spin->setRange(m_minimum, m_maximum);
    m_spin->setSingleStep(std::max(std::abs(step), 1.0e-9));
    m_spin->setDecimals(std::max(0, decimals));
    m_interactionActive = false;
    m_scrubInteraction = false;
}

void SliderSpinBox::setValue(const double value)
{
    const QSignalBlocker blocker(m_spin);
    m_spin->setValue(std::clamp(value, m_minimum, m_maximum));
    m_spin->update();
    if (QLineEdit *editor = m_spin->findChild<QLineEdit *>()) editor->update();
}

double SliderSpinBox::value() const
{
    return m_spin->value();
}

void SliderSpinBox::setControlHeight(const int height)
{
    // Keep the wrapper and the native spin box on exactly the same integral
    // logical-pixel row. In particular, do not leave a 30 px child inside a
    // 28 px QWidgetAction geometry: Breeze clips the child's lower border in
    // that conflicting configuration, especially at fractional scale factors.
    const int resolvedHeight = std::max(1, height);
    setFixedHeight(resolvedHeight);
    if (m_spin) {
        m_spin->setFixedHeight(resolvedHeight);
    }
    updateGeometry();
}

} // namespace vfx

