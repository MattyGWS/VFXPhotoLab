#pragma once

#include "ImageSizeOperations.h"

#include <QDialog>
#include <QSize>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;

namespace vfx {

class ImageSizeDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ImageSizeDialog(const QSize &currentSize,
                             double currentResolutionX,
                             double currentResolutionY,
                             QWidget *parent = nullptr);

    QSize requestedSize() const;
    ImageResampleMethod resampleMethod() const;
    bool resamplePixels() const;
    double requestedResolutionX() const;
    double requestedResolutionY() const;

private:
    enum class UnitMode {
        Pixels,
        Percent,
        Inches,
        Centimetres,
        Millimetres
    };

    UnitMode unitMode() const;
    bool physicalUnitMode() const;
    double physicalUnitsPerInch() const;
    double effectiveResolutionX() const;
    double effectiveResolutionY() const;
    void configureEditorsForMode();
    void updateLinkedDimension(bool widthChanged);
    void updateSummary();
    void storeSettings();
    double displayedWidthForSize(const QSize &size) const;
    double displayedHeightForSize(const QSize &size) const;

    QSize m_currentSize;
    QSize m_requestedSize;
    QSize m_lastResampledSize;
    double m_currentResolutionX = 72.0;
    double m_currentResolutionY = 72.0;
    QComboBox *m_unitsCombo = nullptr;
    QDoubleSpinBox *m_widthSpin = nullptr;
    QDoubleSpinBox *m_heightSpin = nullptr;
    QCheckBox *m_linkCheck = nullptr;
    QCheckBox *m_resampleCheck = nullptr;
    QDoubleSpinBox *m_resolutionSpin = nullptr;
    QComboBox *m_methodCombo = nullptr;
    QLabel *m_qualityNote = nullptr;
    QLabel *m_summaryLabel = nullptr;
    bool m_resolutionTouched = false;
    bool m_updating = false;
};

} // namespace vfx
