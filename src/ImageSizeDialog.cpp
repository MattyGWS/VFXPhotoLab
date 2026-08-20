#include "ImageSizeDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace vfx {
namespace {

constexpr const char *SettingsGroup = "ImageSizeDialog";

bool validResolution(const double value)
{
    return std::isfinite(value) && value >= 1.0 && value <= 9600.0;
}

} // namespace

ImageSizeDialog::ImageSizeDialog(const QSize &currentSize,
                                 const double currentResolutionX,
                                 const double currentResolutionY,
                                 QWidget *parent)
    : QDialog(parent)
    , m_currentSize(currentSize)
    , m_requestedSize(currentSize)
    , m_lastResampledSize(currentSize)
    , m_currentResolutionX(validResolution(currentResolutionX)
                               ? currentResolutionX : 72.0)
    , m_currentResolutionY(validResolution(currentResolutionY)
                               ? currentResolutionY : 72.0)
{
    setWindowTitle(tr("Image Size"));
    setModal(true);
    setMinimumWidth(460);

    auto *root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetMinimumSize);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(14);

    auto *heading = new QLabel(tr("Resize the editable image"), this);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() + 2.0);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto *intro = new QLabel(
        tr("Image Size resamples every editable layer independently. Disable "
           "resampling to change only print resolution and physical size without "
           "touching any pixel, mask, selection, guide or transform."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *dimensionsGroup = new QGroupBox(tr("Dimensions and Resolution"), this);
    auto *form = new QFormLayout(dimensionsGroup);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_unitsCombo = new QComboBox(dimensionsGroup);
    m_unitsCombo->addItem(tr("Pixels"), static_cast<int>(UnitMode::Pixels));
    m_unitsCombo->addItem(tr("Percent"), static_cast<int>(UnitMode::Percent));
    m_unitsCombo->addItem(tr("Inches"), static_cast<int>(UnitMode::Inches));
    m_unitsCombo->addItem(tr("Centimetres"), static_cast<int>(UnitMode::Centimetres));
    m_unitsCombo->addItem(tr("Millimetres"), static_cast<int>(UnitMode::Millimetres));
    form->addRow(tr("Units"), m_unitsCombo);

    m_widthSpin = new QDoubleSpinBox(dimensionsGroup);
    m_heightSpin = new QDoubleSpinBox(dimensionsGroup);
    for (QDoubleSpinBox *spin : {m_widthSpin, m_heightSpin}) {
        spin->setGroupSeparatorShown(true);
        spin->setMinimumWidth(170);
        spin->setMinimumHeight(spin->sizeHint().height());
    }
    form->addRow(tr("Width"), m_widthSpin);
    form->addRow(tr("Height"), m_heightSpin);

    m_linkCheck = new QCheckBox(tr("Constrain proportions"), dimensionsGroup);
    m_linkCheck->setChecked(true);
    m_linkCheck->setToolTip(
        tr("Keep the current pixel aspect ratio while either dimension changes."));
    form->addRow(QString(), m_linkCheck);

    m_resolutionSpin = new QDoubleSpinBox(dimensionsGroup);
    m_resolutionSpin->setRange(1.0, 9600.0);
    m_resolutionSpin->setDecimals(1);
    m_resolutionSpin->setSingleStep(1.0);
    m_resolutionSpin->setSuffix(tr(" ppi"));
    m_resolutionSpin->setMinimumWidth(170);
    m_resolutionSpin->setValue(m_currentResolutionX);
    m_resolutionSpin->setToolTip(
        tr("Pixels per inch. Changing this value sets equal horizontal and vertical "
           "resolution; leaving it untouched preserves existing asymmetric metadata."));
    form->addRow(tr("Resolution"), m_resolutionSpin);

    m_resampleCheck = new QCheckBox(tr("Resample pixels"), dimensionsGroup);
    m_resampleCheck->setChecked(true);
    m_resampleCheck->setToolTip(
        tr("When disabled, pixel dimensions stay exact and only PPI metadata changes."));
    form->addRow(QString(), m_resampleCheck);
    root->addWidget(dimensionsGroup);

    auto *qualityGroup = new QGroupBox(tr("Resampling Quality"), this);
    auto *qualityForm = new QFormLayout(qualityGroup);
    qualityForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_methodCombo = new QComboBox(qualityGroup);
    m_methodCombo->addItem(tr("Bicubic — balanced"),
                           static_cast<int>(ImageResampleMethod::Bicubic));
    m_methodCombo->addItem(tr("Lanczos 3 — sharp detail"),
                           static_cast<int>(ImageResampleMethod::Lanczos3));
    m_methodCombo->addItem(tr("Area / Box — reduction"),
                           static_cast<int>(ImageResampleMethod::Area));
    m_methodCombo->addItem(tr("Bilinear — fast and smooth"),
                           static_cast<int>(ImageResampleMethod::Bilinear));
    m_methodCombo->addItem(tr("Nearest Neighbour — hard pixels"),
                           static_cast<int>(ImageResampleMethod::NearestNeighbour));
    qualityForm->addRow(tr("Method"), m_methodCombo);
    m_qualityNote = new QLabel(qualityGroup);
    m_qualityNote->setWordWrap(true);
    qualityForm->addRow(QString(), m_qualityNote);
    root->addWidget(qualityGroup);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setObjectName(QStringLiteral("ImageSizeSummary"));
    root->addWidget(m_summaryLabel);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply Image Size"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (requestedSize().isEmpty()) {
            return;
        }
        storeSettings();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected,
            this, &ImageSizeDialog::reject);
    root->addWidget(buttons);

    QSettings settings;
    settings.beginGroup(QString::fromLatin1(SettingsGroup));
    m_linkCheck->setChecked(settings.value(QStringLiteral("linkAspect"), true).toBool());
    m_resampleCheck->setChecked(settings.value(QStringLiteral("resamplePixels"), true).toBool());
    const int unit = settings.value(
        QStringLiteral("unitMode"), static_cast<int>(UnitMode::Pixels)).toInt();
    const int unitIndex = m_unitsCombo->findData(unit);
    if (unitIndex >= 0) {
        m_unitsCombo->setCurrentIndex(unitIndex);
    }
    const int method = settings.value(
        QStringLiteral("method"),
        static_cast<int>(ImageResampleMethod::Bicubic)).toInt();
    const int methodIndex = m_methodCombo->findData(method);
    if (methodIndex >= 0) {
        m_methodCombo->setCurrentIndex(methodIndex);
    }
    settings.endGroup();

    configureEditorsForMode();

    connect(m_unitsCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] {
                configureEditorsForMode();
                updateSummary();
            });
    connect(m_widthSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this] {
                if (m_updating) {
                    return;
                }
                updateLinkedDimension(true);
                updateSummary();
            });
    connect(m_heightSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this] {
                if (m_updating) {
                    return;
                }
                updateLinkedDimension(false);
                updateSummary();
            });
    connect(m_linkCheck, &QCheckBox::toggled, this, [this](const bool linked) {
        configureEditorsForMode();
        if (linked && resamplePixels()) {
            updateLinkedDimension(true);
        }
        updateSummary();
    });
    connect(m_resampleCheck, &QCheckBox::toggled, this, [this](const bool enabled) {
        if (!enabled) {
            m_lastResampledSize = m_requestedSize;
            m_requestedSize = m_currentSize;
        } else {
            m_requestedSize = m_lastResampledSize.isEmpty()
                ? m_currentSize : m_lastResampledSize;
        }
        configureEditorsForMode();
        updateSummary();
    });
    connect(m_resolutionSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this] {
                if (m_updating) {
                    return;
                }
                m_resolutionTouched = true;
                if (resamplePixels() && physicalUnitMode()) {
                    updateLinkedDimension(true);
                }
                configureEditorsForMode();
                updateSummary();
            });
    connect(m_methodCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ImageSizeDialog::updateSummary);

    updateSummary();
    root->activate();
    const QSize comfortable = sizeHint().expandedTo(QSize(540, 570));
    setMinimumSize(comfortable);
    resize(comfortable);
}

ImageSizeDialog::UnitMode ImageSizeDialog::unitMode() const
{
    return static_cast<UnitMode>(m_unitsCombo->currentData().toInt());
}

bool ImageSizeDialog::physicalUnitMode() const
{
    return unitMode() == UnitMode::Inches
        || unitMode() == UnitMode::Centimetres
        || unitMode() == UnitMode::Millimetres;
}

double ImageSizeDialog::physicalUnitsPerInch() const
{
    switch (unitMode()) {
    case UnitMode::Centimetres: return 2.54;
    case UnitMode::Millimetres: return 25.4;
    default: return 1.0;
    }
}

double ImageSizeDialog::effectiveResolutionX() const
{
    return m_resolutionTouched ? m_resolutionSpin->value() : m_currentResolutionX;
}

double ImageSizeDialog::effectiveResolutionY() const
{
    return m_resolutionTouched ? m_resolutionSpin->value() : m_currentResolutionY;
}

void ImageSizeDialog::configureEditorsForMode()
{
    m_updating = true;
    const QSignalBlocker widthBlocker(m_widthSpin);
    const QSignalBlocker heightBlocker(m_heightSpin);
    const double maximumLinkedScale = std::min(
        32768.0 / std::max(1, m_currentSize.width()),
        32768.0 / std::max(1, m_currentSize.height()));

    if (unitMode() == UnitMode::Pixels) {
        m_widthSpin->setDecimals(0);
        m_heightSpin->setDecimals(0);
        const double maximumWidth = m_linkCheck->isChecked()
            ? std::max(1.0, std::floor(
                  m_currentSize.width() * maximumLinkedScale + 1.0e-9))
            : 32768.0;
        const double maximumHeight = m_linkCheck->isChecked()
            ? std::max(1.0, std::floor(
                  m_currentSize.height() * maximumLinkedScale + 1.0e-9))
            : 32768.0;
        m_widthSpin->setRange(1.0, maximumWidth);
        m_heightSpin->setRange(1.0, maximumHeight);
        m_widthSpin->setSingleStep(1.0);
        m_heightSpin->setSingleStep(1.0);
        m_widthSpin->setSuffix(tr(" px"));
        m_heightSpin->setSuffix(tr(" px"));
    } else if (unitMode() == UnitMode::Percent) {
        m_widthSpin->setDecimals(2);
        m_heightSpin->setDecimals(2);
        const double maximumWidthPercent = m_linkCheck->isChecked()
            ? maximumLinkedScale * 100.0
            : 32768.0 * 100.0 / std::max(1, m_currentSize.width());
        const double maximumHeightPercent = m_linkCheck->isChecked()
            ? maximumLinkedScale * 100.0
            : 32768.0 * 100.0 / std::max(1, m_currentSize.height());
        m_widthSpin->setRange(0.01, maximumWidthPercent);
        m_heightSpin->setRange(0.01, maximumHeightPercent);
        m_widthSpin->setSingleStep(1.0);
        m_heightSpin->setSingleStep(1.0);
        m_widthSpin->setSuffix(tr(" %"));
        m_heightSpin->setSuffix(tr(" %"));
    } else {
        m_widthSpin->setDecimals(3);
        m_heightSpin->setDecimals(3);
        const double unitScale = physicalUnitsPerInch();
        const double maximumWidthPixels = m_linkCheck->isChecked()
            ? std::max(1.0, std::floor(
                  m_currentSize.width() * maximumLinkedScale + 1.0e-9))
            : 32768.0;
        const double maximumHeightPixels = m_linkCheck->isChecked()
            ? std::max(1.0, std::floor(
                  m_currentSize.height() * maximumLinkedScale + 1.0e-9))
            : 32768.0;
        m_widthSpin->setRange(
            0.001, maximumWidthPixels / effectiveResolutionX() * unitScale);
        m_heightSpin->setRange(
            0.001, maximumHeightPixels / effectiveResolutionY() * unitScale);
        m_widthSpin->setSingleStep(unitMode() == UnitMode::Millimetres ? 1.0 : 0.1);
        m_heightSpin->setSingleStep(unitMode() == UnitMode::Millimetres ? 1.0 : 0.1);
        const QString suffix = unitMode() == UnitMode::Inches
            ? tr(" in")
            : unitMode() == UnitMode::Centimetres ? tr(" cm") : tr(" mm");
        m_widthSpin->setSuffix(suffix);
        m_heightSpin->setSuffix(suffix);
    }

    const QSize displayedSize = resamplePixels() ? m_requestedSize : m_currentSize;
    m_widthSpin->setValue(displayedWidthForSize(displayedSize));
    m_heightSpin->setValue(displayedHeightForSize(displayedSize));
    m_widthSpin->setEnabled(resamplePixels());
    m_heightSpin->setEnabled(resamplePixels());
    m_linkCheck->setEnabled(resamplePixels());
    m_methodCombo->setEnabled(resamplePixels());
    m_updating = false;
}

void ImageSizeDialog::updateLinkedDimension(const bool widthChanged)
{
    if (m_updating || m_currentSize.isEmpty()) {
        return;
    }
    if (!resamplePixels()) {
        m_requestedSize = m_currentSize;
        return;
    }

    m_updating = true;
    int width = m_requestedSize.width();
    int height = m_requestedSize.height();
    if (unitMode() == UnitMode::Pixels) {
        width = std::clamp(qRound(m_widthSpin->value()), 1, 32768);
        height = std::clamp(qRound(m_heightSpin->value()), 1, 32768);
    } else if (unitMode() == UnitMode::Percent) {
        width = std::clamp(qRound(m_currentSize.width()
                                  * m_widthSpin->value() / 100.0), 1, 32768);
        height = std::clamp(qRound(m_currentSize.height()
                                   * m_heightSpin->value() / 100.0), 1, 32768);
    } else {
        const double unitsPerInch = physicalUnitsPerInch();
        width = std::clamp(qRound(m_widthSpin->value()
                                  / unitsPerInch * effectiveResolutionX()), 1, 32768);
        height = std::clamp(qRound(m_heightSpin->value()
                                   / unitsPerInch * effectiveResolutionY()), 1, 32768);
    }

    if (m_linkCheck->isChecked()) {
        if (widthChanged) {
            height = std::clamp(qRound(width * m_currentSize.height()
                                       / static_cast<double>(m_currentSize.width())),
                                1, 32768);
            const QSignalBlocker blocker(m_heightSpin);
            m_heightSpin->setValue(displayedHeightForSize(QSize(width, height)));
        } else {
            width = std::clamp(qRound(height * m_currentSize.width()
                                      / static_cast<double>(m_currentSize.height())),
                               1, 32768);
            const QSignalBlocker blocker(m_widthSpin);
            m_widthSpin->setValue(displayedWidthForSize(QSize(width, height)));
        }
    }
    m_requestedSize = QSize(width, height);
    m_lastResampledSize = m_requestedSize;
    m_updating = false;
}

void ImageSizeDialog::updateSummary()
{
    if (!m_updating) {
        updateLinkedDimension(true);
    }
    const QSize size = requestedSize();
    const double scaleX = size.width()
        / static_cast<double>(std::max(1, m_currentSize.width()));
    const double scaleY = size.height()
        / static_cast<double>(std::max(1, m_currentSize.height()));
    const double currentWidthInches = m_currentSize.width() / m_currentResolutionX;
    const double currentHeightInches = m_currentSize.height() / m_currentResolutionY;
    const double resultWidthInches = size.width() / requestedResolutionX();
    const double resultHeightInches = size.height() / requestedResolutionY();

    m_summaryLabel->setText(
        tr("Current: %1 × %2 px, %3 × %4 in at %5 × %6 ppi\n"
           "Result: %7 × %8 px, %9 × %10 in at %11 × %12 ppi  (%13% × %14%)")
            .arg(m_currentSize.width())
            .arg(m_currentSize.height())
            .arg(currentWidthInches, 0, 'f', 3)
            .arg(currentHeightInches, 0, 'f', 3)
            .arg(m_currentResolutionX, 0, 'f', 1)
            .arg(m_currentResolutionY, 0, 'f', 1)
            .arg(size.width())
            .arg(size.height())
            .arg(resultWidthInches, 0, 'f', 3)
            .arg(resultHeightInches, 0, 'f', 3)
            .arg(requestedResolutionX(), 0, 'f', 1)
            .arg(requestedResolutionY(), 0, 'f', 1)
            .arg(scaleX * 100.0, 0, 'f', 2)
            .arg(scaleY * 100.0, 0, 'f', 2));

    if (!resamplePixels()) {
        m_qualityNote->setText(
            tr("Metadata only: the exact pixel document, layers, masks, selection, "
               "guides, transforms, Alpha and hidden RGB will remain untouched."));
    } else if (resampleMethod() == ImageResampleMethod::Area) {
        m_qualityNote->setText(
            tr("Area/Box integrates the exact source-pixel area on reducing axes. "
               "Expanding axes use Bilinear interpolation."));
    } else if (resampleMethod() == ImageResampleMethod::Lanczos3) {
        m_qualityNote->setText(
            tr("Lanczos 3 uses a six-lobe separable window with anti-alias widening "
               "during reduction. It is sharp but may produce subtle ringing."));
    } else if (resampleMethod() == ImageResampleMethod::Bicubic) {
        m_qualityNote->setText(
            tr("Bicubic uses Catmull-Rom cubic convolution with anti-alias widening "
               "during reduction for a balanced detailed result."));
    } else {
        m_qualityNote->setText(
            tr("Every editable raster, mask and selection is resampled independently. "
               "Straight RGB beneath zero Alpha is preserved."));
    }
}

void ImageSizeDialog::storeSettings()
{
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(SettingsGroup));
    settings.setValue(QStringLiteral("linkAspect"), m_linkCheck->isChecked());
    settings.setValue(QStringLiteral("resamplePixels"), resamplePixels());
    settings.setValue(QStringLiteral("unitMode"), static_cast<int>(unitMode()));
    settings.setValue(QStringLiteral("method"), static_cast<int>(resampleMethod()));
    settings.endGroup();
}

double ImageSizeDialog::displayedWidthForSize(const QSize &size) const
{
    if (unitMode() == UnitMode::Pixels) {
        return size.width();
    }
    if (unitMode() == UnitMode::Percent) {
        return size.width() * 100.0 / std::max(1, m_currentSize.width());
    }
    return size.width() / effectiveResolutionX() * physicalUnitsPerInch();
}

double ImageSizeDialog::displayedHeightForSize(const QSize &size) const
{
    if (unitMode() == UnitMode::Pixels) {
        return size.height();
    }
    if (unitMode() == UnitMode::Percent) {
        return size.height() * 100.0 / std::max(1, m_currentSize.height());
    }
    return size.height() / effectiveResolutionY() * physicalUnitsPerInch();
}

QSize ImageSizeDialog::requestedSize() const
{
    if (!resamplePixels()) {
        return m_currentSize;
    }
    return m_requestedSize;
}

ImageResampleMethod ImageSizeDialog::resampleMethod() const
{
    return static_cast<ImageResampleMethod>(m_methodCombo->currentData().toInt());
}

bool ImageSizeDialog::resamplePixels() const
{
    return m_resampleCheck && m_resampleCheck->isChecked();
}

double ImageSizeDialog::requestedResolutionX() const
{
    return effectiveResolutionX();
}

double ImageSizeDialog::requestedResolutionY() const
{
    return effectiveResolutionY();
}

} // namespace vfx
