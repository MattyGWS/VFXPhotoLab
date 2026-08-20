#include "ColourWheelWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QSizePolicy>

#include <algorithm>
#include <cmath>

namespace vfx {
namespace {

constexpr double Pi = 3.14159265358979323846;
constexpr double Epsilon = 1.0e-9;

QColor blendTriangleColour(const QColor &hueColour,
                           const double hueWeight,
                           const double whiteWeight)
{
    const double red = hueWeight * hueColour.redF() + whiteWeight;
    const double green = hueWeight * hueColour.greenF() + whiteWeight;
    const double blue = hueWeight * hueColour.blueF() + whiteWeight;
    return QColor::fromRgbF(std::clamp(red, 0.0, 1.0),
                            std::clamp(green, 0.0, 1.0),
                            std::clamp(blue, 0.0, 1.0));
}

} // namespace

ColourWheelWidget::ColourWheelWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    setMinimumSize(minimumSizeHint());

    m_triangleRebuildTimer.setSingleShot(true);
    m_triangleRebuildTimer.setInterval(16);
    connect(&m_triangleRebuildTimer, &QTimer::timeout, this, [this] {
        flushTriangleImageRebuild();
    });
}

QColor ColourWheelWidget::colour() const
{
    return m_colour;
}

void ColourWheelWidget::setColour(const QColor &colour)
{
    if (!colour.isValid() || colour == m_colour) {
        return;
    }

    const QColor hsv = colour.toHsv();
    if (hsv.hsvHueF() >= 0.0) {
        m_hue = hsv.hsvHueF() * 360.0;
    }
    m_saturation = hsv.hsvSaturationF();
    m_value = hsv.valueF();
    m_colour = colour;
    m_triangleRebuildTimer.stop();
    m_triangleImageDirty = false;
    rebuildTriangleImage();
    update();
}

void ColourWheelWidget::setHsv(const double hue,
                               const double saturation,
                               const double value,
                               const double alpha)
{
    m_hue = std::fmod(std::max(0.0, hue), 360.0);
    m_saturation = std::clamp(saturation, 0.0, 1.0);
    m_value = std::clamp(value, 0.0, 1.0);
    m_colour = QColor::fromHsvF(m_hue / 360.0,
                                m_saturation,
                                m_value,
                                std::clamp(alpha, 0.0, 1.0));
    m_triangleRebuildTimer.stop();
    m_triangleImageDirty = false;
    rebuildTriangleImage();
    update();
}

double ColourWheelWidget::hue() const
{
    return m_hue;
}

double ColourWheelWidget::saturation() const
{
    return m_saturation;
}

double ColourWheelWidget::value() const
{
    return m_value;
}

QSize ColourWheelWidget::sizeHint() const
{
    return QSize(200, 200);
}

QSize ColourWheelWidget::minimumSizeHint() const
{
    return QSize(160, 160);
}

int ColourWheelWidget::heightForWidth(const int width) const
{
    return width;
}

bool ColourWheelWidget::hasHeightForWidth() const
{
    return true;
}

void ColourWheelWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), palette().window());

    if (!m_ringImage.isNull()) {
        painter.drawImage(QPoint(0, 0), m_ringImage);
    }
    if (!m_triangleImage.isNull()) {
        painter.drawImage(QPoint(0, 0), m_triangleImage);
    }
    if (m_triangle.size() != 3) {
        return;
    }

    const double hueRadians = m_hue * Pi / 180.0;
    const double markerRadius = (m_outerRadius + m_innerRadius) * 0.5;
    const QPointF hueMarker(m_center.x() + std::cos(hueRadians) * markerRadius,
                            m_center.y() - std::sin(hueRadians) * markerRadius);

    painter.setPen(QPen(QColor(20, 18, 24, 230), 4.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(hueMarker, 6.0, 6.0);
    painter.setPen(QPen(Qt::white, 2.0));
    painter.drawEllipse(hueMarker, 6.0, 6.0);

    const QPointF triangleMarker = triangleMarkerPosition();
    painter.setPen(QPen(QColor(20, 18, 24, 230), 4.0));
    painter.drawEllipse(triangleMarker, 6.0, 6.0);
    painter.setPen(QPen(Qt::white, 2.0));
    painter.drawEllipse(triangleMarker, 6.0, 6.0);
}

void ColourWheelWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    rebuildGeometry();
    rebuildRingImage();
    m_triangleRebuildTimer.stop();
    m_triangleImageDirty = false;
    rebuildTriangleImage();
}

void ColourWheelWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    const QPointF delta = event->position() - m_center;
    const double distance = std::hypot(delta.x(), delta.y());
    if (distance >= m_innerRadius && distance <= m_outerRadius) {
        m_dragRegion = DragRegion::HueRing;
        updateHueFromPoint(event->position());
        event->accept();
        return;
    }

    if (pointInsideTriangle(event->position())) {
        m_dragRegion = DragRegion::Triangle;
        updateSaturationValueFromPoint(event->position());
        event->accept();
        return;
    }

    event->ignore();
}

void ColourWheelWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!(event->buttons() & Qt::LeftButton)) {
        event->ignore();
        return;
    }

    if (m_dragRegion == DragRegion::HueRing) {
        updateHueFromPoint(event->position());
        event->accept();
        return;
    }
    if (m_dragRegion == DragRegion::Triangle) {
        updateSaturationValueFromPoint(event->position());
        event->accept();
        return;
    }

    event->ignore();
}

void ColourWheelWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragRegion != DragRegion::None) {
        flushTriangleImageRebuild();
        m_dragRegion = DragRegion::None;
        emit colourEditingFinished(m_colour);
        event->accept();
        return;
    }
    event->ignore();
}

void ColourWheelWidget::rebuildGeometry()
{
    const double side = std::max(1, std::min(width(), height()));
    m_center = QPointF(width() * 0.5, height() * 0.5);
    m_outerRadius = side * 0.47;
    m_innerRadius = m_outerRadius * 0.76;

    const double triangleRadius = m_innerRadius * 0.88;
    const double halfHeight = triangleRadius * std::sqrt(3.0) * 0.5;
    m_triangle.clear();
    m_triangle << QPointF(m_center.x() + triangleRadius, m_center.y())
               << QPointF(m_center.x() - triangleRadius * 0.5,
                          m_center.y() + halfHeight)
               << QPointF(m_center.x() - triangleRadius * 0.5,
                          m_center.y() - halfHeight);
}

void ColourWheelWidget::rebuildRingImage()
{
    if (width() <= 0 || height() <= 0) {
        m_ringImage = {};
        return;
    }

    m_ringImage = QImage(size(), QImage::Format_ARGB32_Premultiplied);
    m_ringImage.fill(Qt::transparent);
    for (int y = 0; y < height(); ++y) {
        auto *scanline = reinterpret_cast<QRgb *>(m_ringImage.scanLine(y));
        for (int x = 0; x < width(); ++x) {
            const QPointF point(x + 0.5, y + 0.5);
            const QPointF delta = point - m_center;
            const double distance = std::hypot(delta.x(), delta.y());
            if (distance < m_innerRadius || distance > m_outerRadius) {
                continue;
            }

            double hue = std::atan2(-delta.y(), delta.x()) * 180.0 / Pi;
            if (hue < 0.0) {
                hue += 360.0;
            }
            scanline[x] = QColor::fromHsvF(hue / 360.0, 1.0, 1.0).rgba();
        }
    }
}

void ColourWheelWidget::rebuildTriangleImage()
{
    if (width() <= 0 || height() <= 0 || m_triangle.size() != 3) {
        m_triangleImage = {};
        return;
    }

    m_triangleImage = QImage(size(), QImage::Format_ARGB32_Premultiplied);
    m_triangleImage.fill(Qt::transparent);

    const QColor hueColour = QColor::fromHsvF(m_hue / 360.0, 1.0, 1.0);
    const QPointF a = m_triangle.at(0);
    const QPointF b = m_triangle.at(1);
    const QPointF c = m_triangle.at(2);
    const QRect bounds = m_triangle.boundingRect().toAlignedRect().intersected(rect());

    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
        auto *scanline = reinterpret_cast<QRgb *>(m_triangleImage.scanLine(y));
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            const QPointF point(x + 0.5, y + 0.5);
            double weightA = 0.0;
            double weightB = 0.0;
            double weightC = 0.0;
            barycentricWeights(point, a, b, c, weightA, weightB, weightC);
            if (weightA >= -0.001 && weightB >= -0.001 && weightC >= -0.001) {
                scanline[x] = blendTriangleColour(hueColour,
                                                  weightA,
                                                  weightB).rgba();
            }
        }
    }
}

void ColourWheelWidget::scheduleTriangleImageRebuild()
{
    m_triangleImageDirty = true;
    if (!m_triangleRebuildTimer.isActive()) {
        m_triangleRebuildTimer.start();
    }
}

void ColourWheelWidget::flushTriangleImageRebuild()
{
    if (!m_triangleImageDirty) {
        return;
    }
    m_triangleRebuildTimer.stop();
    m_triangleImageDirty = false;
    rebuildTriangleImage();
    update();
}

void ColourWheelWidget::updateHueFromPoint(const QPointF &point)
{
    const QPointF delta = point - m_center;
    double hue = std::atan2(-delta.y(), delta.x()) * 180.0 / Pi;
    if (hue < 0.0) {
        hue += 360.0;
    }
    m_hue = hue;
    m_colour = currentHsvColour();
    scheduleTriangleImageRebuild();
    update();
    emit colourChanged(m_colour);
}

void ColourWheelWidget::updateSaturationValueFromPoint(const QPointF &point)
{
    double hueWeight = 0.0;
    double whiteWeight = 0.0;
    double blackWeight = 0.0;
    barycentricWeights(point,
                       m_triangle.at(0),
                       m_triangle.at(1),
                       m_triangle.at(2),
                       hueWeight,
                       whiteWeight,
                       blackWeight);

    hueWeight = std::max(0.0, hueWeight);
    whiteWeight = std::max(0.0, whiteWeight);
    blackWeight = std::max(0.0, blackWeight);
    const double total = hueWeight + whiteWeight + blackWeight;
    if (total <= Epsilon) {
        return;
    }
    hueWeight /= total;
    whiteWeight /= total;
    blackWeight /= total;

    m_value = std::clamp(1.0 - blackWeight, 0.0, 1.0);
    m_saturation = m_value > Epsilon
        ? std::clamp(hueWeight / m_value, 0.0, 1.0)
        : 0.0;

    m_colour = currentHsvColour();
    update();
    emit colourChanged(m_colour);
}

QColor ColourWheelWidget::currentHsvColour() const
{
    QColor colour = QColor::fromHsvF(m_hue / 360.0,
                                     m_saturation,
                                     m_value,
                                     m_colour.alphaF());
    return colour;
}

QPointF ColourWheelWidget::triangleMarkerPosition() const
{
    const double blackWeight = 1.0 - m_value;
    const double hueWeight = m_saturation * m_value;
    const double whiteWeight = (1.0 - m_saturation) * m_value;
    return m_triangle.at(0) * hueWeight
        + m_triangle.at(1) * whiteWeight
        + m_triangle.at(2) * blackWeight;
}

bool ColourWheelWidget::pointInsideTriangle(const QPointF &point) const
{
    if (m_triangle.size() != 3) {
        return false;
    }
    double a = 0.0;
    double b = 0.0;
    double c = 0.0;
    barycentricWeights(point,
                       m_triangle.at(0),
                       m_triangle.at(1),
                       m_triangle.at(2),
                       a,
                       b,
                       c);
    return a >= 0.0 && b >= 0.0 && c >= 0.0;
}

void ColourWheelWidget::barycentricWeights(const QPointF &point,
                                           const QPointF &a,
                                           const QPointF &b,
                                           const QPointF &c,
                                           double &weightA,
                                           double &weightB,
                                           double &weightC)
{
    const double denominator = (b.y() - c.y()) * (a.x() - c.x())
        + (c.x() - b.x()) * (a.y() - c.y());
    if (std::abs(denominator) <= Epsilon) {
        weightA = weightB = weightC = -1.0;
        return;
    }

    weightA = ((b.y() - c.y()) * (point.x() - c.x())
               + (c.x() - b.x()) * (point.y() - c.y()))
        / denominator;
    weightB = ((c.y() - a.y()) * (point.x() - c.x())
               + (a.x() - c.x()) * (point.y() - c.y()))
        / denominator;
    weightC = 1.0 - weightA - weightB;
}

} // namespace vfx
