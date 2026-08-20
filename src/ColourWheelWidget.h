#pragma once

#include <QColor>
#include <QImage>
#include <QPolygonF>
#include <QTimer>
#include <QWidget>

namespace vfx {

class ColourWheelWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ColourWheelWidget(QWidget *parent = nullptr);

    QColor colour() const;
    void setColour(const QColor &colour);
    void setHsv(double hue, double saturation, double value, double alpha = 1.0);
    double hue() const;
    double saturation() const;
    double value() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    int heightForWidth(int width) const override;
    bool hasHeightForWidth() const override;

signals:
    void colourChanged(const QColor &colour);
    void colourEditingFinished(const QColor &colour);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    enum class DragRegion {
        None,
        HueRing,
        Triangle
    };

    void rebuildGeometry();
    void rebuildRingImage();
    void rebuildTriangleImage();
    void scheduleTriangleImageRebuild();
    void flushTriangleImageRebuild();
    void updateHueFromPoint(const QPointF &point);
    void updateSaturationValueFromPoint(const QPointF &point);
    QColor currentHsvColour() const;
    QPointF triangleMarkerPosition() const;
    bool pointInsideTriangle(const QPointF &point) const;

    static void barycentricWeights(const QPointF &point,
                                   const QPointF &a,
                                   const QPointF &b,
                                   const QPointF &c,
                                   double &weightA,
                                   double &weightB,
                                   double &weightC);

    QColor m_colour = QColor(Qt::black);
    double m_hue = 0.0;
    double m_saturation = 0.0;
    double m_value = 0.0;

    QPointF m_center;
    double m_outerRadius = 0.0;
    double m_innerRadius = 0.0;
    QPolygonF m_triangle;
    QImage m_ringImage;
    QImage m_triangleImage;
    DragRegion m_dragRegion = DragRegion::None;
    QTimer m_triangleRebuildTimer;
    bool m_triangleImageDirty = false;
};

} // namespace vfx
