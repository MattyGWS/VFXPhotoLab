#include "NewDocumentDialog.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>

namespace vfx {
namespace {

constexpr int PresetSizeRole = Qt::UserRole + 1;
constexpr int PresetResolutionRole = Qt::UserRole + 2;

QString readableBytes(const quint64 bytes)
{
    constexpr double KiB = 1024.0;
    constexpr double MiB = KiB * 1024.0;
    constexpr double GiB = MiB * 1024.0;
    if (bytes >= static_cast<quint64>(GiB)) {
        return QObject::tr("%1 GiB").arg(bytes / GiB, 0, 'f', 2);
    }
    if (bytes >= static_cast<quint64>(MiB)) {
        return QObject::tr("%1 MiB").arg(bytes / MiB, 0, 'f', 1);
    }
    return QObject::tr("%1 KiB").arg(bytes / KiB, 0, 'f', 1);
}

QColor validColour(const QColor &colour)
{
    return colour.isValid() ? colour : QColor(Qt::black);
}

} // namespace

NewDocumentDialog::NewDocumentDialog(const QColor &primaryColour, QWidget *parent)
    : QDialog(parent)
    , m_primaryColour(validColour(primaryColour))
    , m_customColour(m_primaryColour)
{
    setWindowTitle(tr("New Document"));
    setModal(true);
    resize(620, 620);

    auto *root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetMinimumSize);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(14);

    auto *heading = new QLabel(tr("Create a blank VFX Photo Lab document"), this);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() + 2.0);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto *intro = new QLabel(
        tr("Choose pixel dimensions, integer precision, working colour space and the initial Background layer."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *dimensionsGroup = new QGroupBox(tr("Document"), this);
    auto *dimensionsForm = new QFormLayout(dimensionsGroup);
    dimensionsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_nameEdit = new QLineEdit(dimensionsGroup);
    m_nameEdit->setPlaceholderText(tr("Untitled Photo"));
    dimensionsForm->addRow(tr("Name"), m_nameEdit);

    m_presetCombo = new QComboBox(dimensionsGroup);
    m_presetCombo->addItem(tr("Custom"));
    const auto addPreset = [this](const QString &name, const QSize &size, const double dpi) {
        m_presetCombo->addItem(name);
        const int row = m_presetCombo->count() - 1;
        m_presetCombo->setItemData(row, size, PresetSizeRole);
        m_presetCombo->setItemData(row, dpi, PresetResolutionRole);
    };
    addPreset(tr("HD — 1920 × 1080"), QSize(1920, 1080), 72.0);
    addPreset(tr("4K UHD — 3840 × 2160"), QSize(3840, 2160), 72.0);
    addPreset(tr("Square — 512 × 512"), QSize(512, 512), 72.0);
    addPreset(tr("Square — 1024 × 1024"), QSize(1024, 1024), 72.0);
    addPreset(tr("Square — 2048 × 2048"), QSize(2048, 2048), 72.0);
    addPreset(tr("Square — 4096 × 4096"), QSize(4096, 4096), 72.0);
    addPreset(tr("A4 Portrait — 2480 × 3508 at 300 ppi"), QSize(2480, 3508), 300.0);
    addPreset(tr("A4 Landscape — 3508 × 2480 at 300 ppi"), QSize(3508, 2480), 300.0);
    dimensionsForm->addRow(tr("Preset"), m_presetCombo);

    auto *sizeWidget = new QWidget(dimensionsGroup);
    auto *sizeLayout = new QGridLayout(sizeWidget);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    sizeLayout->setHorizontalSpacing(8);
    sizeLayout->setVerticalSpacing(6);

    m_widthSpin = new QSpinBox(sizeWidget);
    m_widthSpin->setRange(1, 32768);
    m_widthSpin->setSuffix(tr(" px"));
    m_widthSpin->setGroupSeparatorShown(true);
    m_heightSpin = new QSpinBox(sizeWidget);
    m_heightSpin->setRange(1, 32768);
    m_heightSpin->setSuffix(tr(" px"));
    m_heightSpin->setGroupSeparatorShown(true);

    // Some Linux styles report a permissive minimum height for complex spin boxes.
    // When the dialog is first shown, the nested pixel-size grid could therefore be
    // compressed until the user manually enlarged the window. Keep both numeric
    // fields at their natural styled height and reserve that height in the grid.
    const int pixelSpinHeight = std::max(m_widthSpin->sizeHint().height(),
                                         m_heightSpin->sizeHint().height());
    m_widthSpin->setMinimumHeight(pixelSpinHeight);
    m_heightSpin->setMinimumHeight(pixelSpinHeight);
    m_widthSpin->setMinimumWidth(140);
    m_heightSpin->setMinimumWidth(140);

    m_swapButton = new QPushButton(tr("Swap"), sizeWidget);
    m_swapButton->setToolTip(tr("Exchange width and height"));

    sizeLayout->addWidget(new QLabel(tr("Width"), sizeWidget), 0, 0);
    sizeLayout->addWidget(m_widthSpin, 0, 1);
    sizeLayout->addWidget(new QLabel(tr("Height"), sizeWidget), 1, 0);
    sizeLayout->addWidget(m_heightSpin, 1, 1);
    sizeLayout->addWidget(m_swapButton, 0, 2, 2, 1);
    sizeLayout->setColumnStretch(1, 1);
    sizeLayout->setRowMinimumHeight(0, pixelSpinHeight);
    sizeLayout->setRowMinimumHeight(1, pixelSpinHeight);
    sizeWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    sizeWidget->setMinimumHeight((pixelSpinHeight * 2) + sizeLayout->verticalSpacing());
    dimensionsForm->addRow(tr("Pixel size"), sizeWidget);

    m_resolutionSpin = new QDoubleSpinBox(dimensionsGroup);
    m_resolutionSpin->setRange(1.0, 9600.0);
    m_resolutionSpin->setDecimals(1);
    m_resolutionSpin->setSuffix(tr(" ppi"));
    m_resolutionSpin->setMinimumHeight(m_resolutionSpin->sizeHint().height());
    m_resolutionSpin->setMinimumWidth(140);
    m_resolutionSpin->setToolTip(
        tr("Print metadata only; it does not change the chosen pixel dimensions."));
    dimensionsForm->addRow(tr("Resolution"), m_resolutionSpin);
    root->addWidget(dimensionsGroup);

    auto *colourGroup = new QGroupBox(tr("Colour and Background"), this);
    auto *colourForm = new QFormLayout(colourGroup);
    colourForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_colourModelCombo = new QComboBox(colourGroup);
    m_colourModelCombo->addItem(tr("RGB"), static_cast<int>(DocumentColourModel::Rgb));
    m_colourModelCombo->addItem(tr("Grayscale"), static_cast<int>(DocumentColourModel::Grayscale));
    colourForm->addRow(tr("Colour model"), m_colourModelCombo);

    m_bitDepthCombo = new QComboBox(colourGroup);
    m_bitDepthCombo->addItem(tr("8-bit integer per channel"), 8);
    m_bitDepthCombo->addItem(tr("16-bit integer per channel"), 16);
    colourForm->addRow(tr("Bit depth"), m_bitDepthCombo);

    m_colourSpaceCombo = new QComboBox(colourGroup);
    m_colourSpaceCombo->addItem(tr("sRGB (standard display)"), QStringLiteral("srgb"));
    m_colourSpaceCombo->addItem(tr("Linear sRGB (advanced VFX workflow)"), QStringLiteral("linear-srgb"));
    m_colourSpaceCombo->addItem(tr("Display P3 (wide-gamut display)"), QStringLiteral("display-p3"));
    m_colourSpaceCombo->addItem(tr("Adobe RGB (photographic print workflow)"), QStringLiteral("adobe-rgb"));
    m_colourSpaceCombo->addItem(tr("ProPhoto RGB (very wide gamut)"), QStringLiteral("prophoto-rgb"));
    colourForm->addRow(tr("Working space"), m_colourSpaceCombo);

    auto *backgroundWidget = new QWidget(colourGroup);
    auto *backgroundLayout = new QHBoxLayout(backgroundWidget);
    backgroundLayout->setContentsMargins(0, 0, 0, 0);
    backgroundLayout->setSpacing(8);
    m_backgroundCombo = new QComboBox(backgroundWidget);
    m_backgroundCombo->addItem(tr("White"), static_cast<int>(BackgroundChoice::White));
    m_backgroundCombo->addItem(tr("Black"), static_cast<int>(BackgroundChoice::Black));
    m_backgroundCombo->addItem(tr("Transparent"), static_cast<int>(BackgroundChoice::Transparent));
    m_backgroundCombo->addItem(tr("Primary colour"), static_cast<int>(BackgroundChoice::Primary));
    m_backgroundCombo->addItem(tr("Custom colour"), static_cast<int>(BackgroundChoice::Custom));
    m_customColourButton = new QPushButton(tr("Choose…"), backgroundWidget);
    backgroundLayout->addWidget(m_backgroundCombo, 1);
    backgroundLayout->addWidget(m_customColourButton);
    colourForm->addRow(tr("Background"), backgroundWidget);
    root->addWidget(colourGroup);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setObjectName(QStringLiteral("NewDocumentSummary"));
    root->addWidget(m_summaryLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    connect(buttons, &QDialogButtonBox::accepted, this, &NewDocumentDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &NewDocumentDialog::reject);
    root->addWidget(buttons);

    connect(m_presetCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](const int index) { applyPreset(index); });
    connect(m_widthSpin,
            qOverload<int>(&QSpinBox::valueChanged),
            this,
            [this] { markCustomPreset(); updateSummary(); });
    connect(m_heightSpin,
            qOverload<int>(&QSpinBox::valueChanged),
            this,
            [this] { markCustomPreset(); updateSummary(); });
    connect(m_resolutionSpin,
            qOverload<double>(&QDoubleSpinBox::valueChanged),
            this,
            [this] { markCustomPreset(); updateSummary(); });
    connect(m_swapButton, &QPushButton::clicked, this, [this] {
        m_updatingControls = true;
        const int oldWidth = m_widthSpin->value();
        m_widthSpin->setValue(m_heightSpin->value());
        m_heightSpin->setValue(oldWidth);
        m_updatingControls = false;
        markCustomPreset();
        updateSummary();
    });
    connect(m_colourModelCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this] { updateSummary(); });
    connect(m_bitDepthCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this] { updateSummary(); });
    connect(m_colourSpaceCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this] { updateSummary(); });
    connect(m_backgroundCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this] { updateBackgroundControls(); updateSummary(); });
    connect(m_customColourButton,
            &QPushButton::clicked,
            this,
            &NewDocumentDialog::chooseCustomBackground);

    restoreSettings();
    updateBackgroundControls();
    updateCustomColourButton();
    updateSummary();
    // Size the first presentation from the completed layout rather than from a
    // hard-coded geometry that may be too short under a user's font, DPI or style.
    root->activate();
    const QSize comfortableSize = sizeHint().expandedTo(QSize(620, 620));
    setMinimumSize(comfortableSize);
    resize(comfortableSize);

    m_nameEdit->setFocus();
    m_nameEdit->selectAll();
}

NewDocumentSettings NewDocumentDialog::settings() const
{
    NewDocumentSettings result;
    result.name = m_nameEdit->text().trimmed();
    if (result.name.isEmpty()) {
        result.name = tr("Untitled Photo");
    }
    result.pixelSize = QSize(m_widthSpin->value(), m_heightSpin->value());
    result.bitDepth = m_bitDepthCombo->currentData().toInt();
    result.colourModel = static_cast<DocumentColourModel>(
        m_colourModelCombo->currentData().toInt());
    const QString colourSpaceToken = m_colourSpaceCombo->currentData().toString();
    if (colourSpaceToken == QStringLiteral("linear-srgb")) {
        result.colourSpace = QColorSpace(QColorSpace::SRgbLinear);
    } else if (colourSpaceToken == QStringLiteral("display-p3")) {
        result.colourSpace = QColorSpace(QColorSpace::DisplayP3);
    } else if (colourSpaceToken == QStringLiteral("adobe-rgb")) {
        result.colourSpace = QColorSpace(QColorSpace::AdobeRgb);
    } else if (colourSpaceToken == QStringLiteral("prophoto-rgb")) {
        result.colourSpace = QColorSpace(QColorSpace::ProPhotoRgb);
    } else {
        result.colourSpace = QColorSpace(QColorSpace::SRgb);
    }
    result.backgroundColour = selectedBackgroundColour();
    result.resolutionX = m_resolutionSpin->value();
    result.resolutionY = m_resolutionSpin->value();
    return result;
}

void NewDocumentDialog::accept()
{
    const quint64 bytesPerPixel = m_bitDepthCombo->currentData().toInt() == 16 ? 8u : 4u;
    const quint64 bytes = static_cast<quint64>(m_widthSpin->value())
        * static_cast<quint64>(m_heightSpin->value()) * bytesPerPixel;
    if (bytes >= 4ull * 1024ull * 1024ull * 1024ull) {
        const auto choice = QMessageBox::warning(
            this,
            tr("Very Large Document"),
            tr("The base pixel surface alone requires approximately %1. Layers, history and previews will require additional memory. Create it anyway?")
                .arg(readableBytes(bytes)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (choice != QMessageBox::Yes) {
            return;
        }
    }
    saveSettings();
    QDialog::accept();
}

void NewDocumentDialog::applyPreset(const int index)
{
    if (m_updatingControls || index <= 0) {
        return;
    }
    const QSize size = m_presetCombo->itemData(index, PresetSizeRole).toSize();
    const double resolution = m_presetCombo->itemData(index, PresetResolutionRole).toDouble();
    if (size.isEmpty()) {
        return;
    }
    m_updatingControls = true;
    m_widthSpin->setValue(size.width());
    m_heightSpin->setValue(size.height());
    m_resolutionSpin->setValue(resolution > 0.0 ? resolution : 72.0);
    m_updatingControls = false;
    updateSummary();
}

void NewDocumentDialog::markCustomPreset()
{
    if (m_updatingControls || m_presetCombo->currentIndex() == 0) {
        return;
    }
    m_updatingControls = true;
    m_presetCombo->setCurrentIndex(0);
    m_updatingControls = false;
}

void NewDocumentDialog::updateBackgroundControls()
{
    const auto choice = static_cast<BackgroundChoice>(m_backgroundCombo->currentData().toInt());
    m_customColourButton->setEnabled(choice == BackgroundChoice::Custom);
}

void NewDocumentDialog::chooseCustomBackground()
{
    const QColor chosen = QColorDialog::getColor(
        validColour(m_customColour),
        this,
        tr("Choose Background Colour"),
        QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) {
        return;
    }
    m_customColour = chosen;
    updateCustomColourButton();
    updateSummary();
}

void NewDocumentDialog::updateSummary()
{
    const int bitDepth = m_bitDepthCombo->currentData().toInt();
    const quint64 bytesPerPixel = bitDepth == 16 ? 8u : 4u;
    const quint64 bytes = static_cast<quint64>(m_widthSpin->value())
        * static_cast<quint64>(m_heightSpin->value()) * bytesPerPixel;
    const bool grayscale = static_cast<DocumentColourModel>(
        m_colourModelCombo->currentData().toInt()) == DocumentColourModel::Grayscale;
    const QString colourSpaceToken = m_colourSpaceCombo->currentData().toString();
    QString profile = tr("sRGB");
    if (colourSpaceToken == QStringLiteral("linear-srgb")) {
        profile = tr("Linear sRGB");
    } else if (colourSpaceToken == QStringLiteral("display-p3")) {
        profile = tr("Display P3");
    } else if (colourSpaceToken == QStringLiteral("adobe-rgb")) {
        profile = tr("Adobe RGB");
    } else if (colourSpaceToken == QStringLiteral("prophoto-rgb")) {
        profile = tr("ProPhoto RGB");
    }
    const QString model = grayscale ? tr("Grayscale") : tr("RGB");
    m_summaryLabel->setText(
        tr("%1 × %2 pixels • %3, %4-bit integer • %5 • base storage approximately %6. The document starts with one paintable Background layer.")
            .arg(m_widthSpin->value())
            .arg(m_heightSpin->value())
            .arg(model)
            .arg(bitDepth)
            .arg(profile)
            .arg(readableBytes(bytes)));
}

void NewDocumentDialog::updateCustomColourButton()
{
    const QColor colour = m_customColour.isValid() ? m_customColour : QColor(Qt::black);
    m_customColourButton->setStyleSheet(
        QStringLiteral("QPushButton { background-color: rgba(%1, %2, %3, %4); }")
            .arg(colour.red())
            .arg(colour.green())
            .arg(colour.blue())
            .arg(colour.alpha()));
}

void NewDocumentDialog::restoreSettings()
{
    QSettings stored;
    m_updatingControls = true;
    m_nameEdit->setText(tr("Untitled Photo"));
    m_widthSpin->setValue(stored.value(QStringLiteral("newDocument/width"), 1920).toInt());
    m_heightSpin->setValue(stored.value(QStringLiteral("newDocument/height"), 1080).toInt());
    m_resolutionSpin->setValue(stored.value(QStringLiteral("newDocument/resolution"), 72.0).toDouble());

    const int model = stored.value(QStringLiteral("newDocument/colourModel"),
                                   static_cast<int>(DocumentColourModel::Rgb)).toInt();
    const int modelIndex = m_colourModelCombo->findData(model);
    m_colourModelCombo->setCurrentIndex(std::max(0, modelIndex));

    const int bitDepth = stored.value(QStringLiteral("newDocument/bitDepth"), 8).toInt();
    const int depthIndex = m_bitDepthCombo->findData(bitDepth);
    m_bitDepthCombo->setCurrentIndex(std::max(0, depthIndex));

    const QString colourSpace = stored.value(QStringLiteral("newDocument/colourSpace"),
                                             QStringLiteral("srgb")).toString();
    const int colourSpaceIndex = m_colourSpaceCombo->findData(colourSpace);
    m_colourSpaceCombo->setCurrentIndex(std::max(0, colourSpaceIndex));

    const int background = stored.value(QStringLiteral("newDocument/background"),
                                        static_cast<int>(BackgroundChoice::White)).toInt();
    const int backgroundIndex = m_backgroundCombo->findData(background);
    m_backgroundCombo->setCurrentIndex(std::max(0, backgroundIndex));

    const QColor custom = stored.value(QStringLiteral("newDocument/customColour"),
                                       m_primaryColour).value<QColor>();
    if (custom.isValid()) {
        m_customColour = custom;
    }
    m_presetCombo->setCurrentIndex(0);
    m_updatingControls = false;
}

void NewDocumentDialog::saveSettings() const
{
    QSettings stored;
    stored.setValue(QStringLiteral("newDocument/width"), m_widthSpin->value());
    stored.setValue(QStringLiteral("newDocument/height"), m_heightSpin->value());
    stored.setValue(QStringLiteral("newDocument/resolution"), m_resolutionSpin->value());
    stored.setValue(QStringLiteral("newDocument/colourModel"),
                    m_colourModelCombo->currentData());
    stored.setValue(QStringLiteral("newDocument/bitDepth"),
                    m_bitDepthCombo->currentData());
    stored.setValue(QStringLiteral("newDocument/colourSpace"),
                    m_colourSpaceCombo->currentData());
    stored.setValue(QStringLiteral("newDocument/background"),
                    m_backgroundCombo->currentData());
    stored.setValue(QStringLiteral("newDocument/customColour"), m_customColour);
}

QColor NewDocumentDialog::selectedBackgroundColour() const
{
    const auto choice = static_cast<BackgroundChoice>(m_backgroundCombo->currentData().toInt());
    switch (choice) {
    case BackgroundChoice::White:
        return QColor(255, 255, 255, 255);
    case BackgroundChoice::Black:
        return QColor(0, 0, 0, 255);
    case BackgroundChoice::Transparent:
        return QColor(0, 0, 0, 0);
    case BackgroundChoice::Primary:
        return m_primaryColour;
    case BackgroundChoice::Custom:
        return m_customColour;
    }
    return QColor(Qt::white);
}

} // namespace vfx
