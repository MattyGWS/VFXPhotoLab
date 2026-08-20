#include "CanvasSizeDialog.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace vfx {
namespace {

QString anchorName(const CanvasAnchor anchor)
{
    switch (anchor) {
    case CanvasAnchor::TopLeft: return QObject::tr("Top Left");
    case CanvasAnchor::Top: return QObject::tr("Top Centre");
    case CanvasAnchor::TopRight: return QObject::tr("Top Right");
    case CanvasAnchor::Left: return QObject::tr("Centre Left");
    case CanvasAnchor::Centre: return QObject::tr("Centre");
    case CanvasAnchor::Right: return QObject::tr("Centre Right");
    case CanvasAnchor::BottomLeft: return QObject::tr("Bottom Left");
    case CanvasAnchor::Bottom: return QObject::tr("Bottom Centre");
    case CanvasAnchor::BottomRight: return QObject::tr("Bottom Right");
    }
    return QObject::tr("Centre");
}

QString readableBytes(const quint64 bytes)
{
    constexpr double MiB = 1024.0 * 1024.0;
    constexpr double GiB = MiB * 1024.0;
    if (bytes >= static_cast<quint64>(GiB)) {
        return QObject::tr("%1 GiB").arg(bytes / GiB, 0, 'f', 2);
    }
    return QObject::tr("%1 MiB").arg(bytes / MiB, 0, 'f', 1);
}

QString colourText(const QColor &colour)
{
    return colour.alpha() == 255
        ? colour.name(QColor::HexRgb).toUpper()
        : colour.name(QColor::HexArgb).toUpper();
}

QIcon colourIcon(const QColor &colour)
{
    QPixmap pixmap(48, 22);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    constexpr int Cell = 6;
    for (int y = 0; y < pixmap.height(); y += Cell) {
        for (int x = 0; x < pixmap.width(); x += Cell) {
            painter.fillRect(QRect(x, y, Cell, Cell),
                             ((x / Cell) + (y / Cell)) % 2 == 0
                                 ? QColor(215, 215, 215)
                                 : QColor(150, 150, 150));
        }
    }
    painter.fillRect(pixmap.rect().adjusted(1, 1, -1, -1), colour);
    painter.setPen(QColor(65, 65, 65));
    painter.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
    return QIcon(pixmap);
}

QString fillModeName(const CanvasFillMode mode)
{
    switch (mode) {
    case CanvasFillMode::Transparent: return QObject::tr("Transparent");
    case CanvasFillMode::Foreground: return QObject::tr("Foreground Colour");
    case CanvasFillMode::Background: return QObject::tr("Background Colour");
    case CanvasFillMode::Custom: return QObject::tr("Custom Colour");
    }
    return QObject::tr("Transparent");
}

} // namespace

CanvasSizeDialog::CanvasSizeDialog(const QSize &currentSize,
                                   const QColor &foregroundColour,
                                   const QColor &backgroundColour,
                                   QWidget *parent)
    : QDialog(parent)
    , m_currentSize(currentSize)
    , m_targetSize(currentSize)
    , m_foregroundColour(foregroundColour.isValid()
                             ? foregroundColour : QColor(Qt::black))
    , m_backgroundColour(backgroundColour.isValid()
                             ? backgroundColour : QColor(Qt::white))
{
    setWindowTitle(tr("Canvas Size"));
    setModal(true);

    auto *root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetMinimumSize);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(14);

    auto *heading = new QLabel(tr("Change document bounds"), this);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() + 2.0);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto *intro = new QLabel(
        tr("Canvas Size changes the document bounds without scaling layers. "
           "By default, pixels outside a smaller canvas remain stored "
           "non-destructively."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *sizeGroup = new QGroupBox(tr("Dimensions"), this);
    auto *sizeForm = new QFormLayout(sizeGroup);
    sizeForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_modeCombo = new QComboBox(sizeGroup);
    m_modeCombo->addItem(tr("Absolute size"), false);
    m_modeCombo->addItem(tr("Relative change"), true);
    sizeForm->addRow(tr("Sizing"), m_modeCombo);

    auto *sizeWidget = new QWidget(sizeGroup);
    auto *sizeLayout = new QGridLayout(sizeWidget);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    sizeLayout->setHorizontalSpacing(8);
    sizeLayout->setVerticalSpacing(6);

    m_widthSpin = new QSpinBox(sizeWidget);
    m_heightSpin = new QSpinBox(sizeWidget);
    for (QSpinBox *spin : {m_widthSpin, m_heightSpin}) {
        spin->setSuffix(tr(" px"));
        spin->setGroupSeparatorShown(true);
        spin->setMinimumWidth(150);
        spin->setMinimumHeight(spin->sizeHint().height());
    }
    sizeLayout->addWidget(new QLabel(tr("Width"), sizeWidget), 0, 0);
    sizeLayout->addWidget(m_widthSpin, 0, 1);
    sizeLayout->addWidget(new QLabel(tr("Height"), sizeWidget), 1, 0);
    sizeLayout->addWidget(m_heightSpin, 1, 1);
    sizeLayout->setColumnStretch(1, 1);
    sizeForm->addRow(tr("Pixel size"), sizeWidget);
    root->addWidget(sizeGroup);

    auto *anchorGroupBox = new QGroupBox(tr("Anchor"), this);
    auto *anchorOuter = new QHBoxLayout(anchorGroupBox);
    anchorOuter->addStretch();
    auto *anchorGrid = new QGridLayout;
    anchorGrid->setSpacing(5);
    m_anchorGroup = new QButtonGroup(this);
    m_anchorGroup->setExclusive(true);

    struct AnchorButton {
        CanvasAnchor anchor;
        int row;
        int column;
        const char *symbol;
    };
    const AnchorButton buttons[] = {
        {CanvasAnchor::TopLeft, 0, 0, "↖"},
        {CanvasAnchor::Top, 0, 1, "↑"},
        {CanvasAnchor::TopRight, 0, 2, "↗"},
        {CanvasAnchor::Left, 1, 0, "←"},
        {CanvasAnchor::Centre, 1, 1, "●"},
        {CanvasAnchor::Right, 1, 2, "→"},
        {CanvasAnchor::BottomLeft, 2, 0, "↙"},
        {CanvasAnchor::Bottom, 2, 1, "↓"},
        {CanvasAnchor::BottomRight, 2, 2, "↘"},
    };
    for (const AnchorButton &entry : buttons) {
        auto *button = new QToolButton(anchorGroupBox);
        button->setText(QString::fromUtf8(entry.symbol));
        button->setCheckable(true);
        button->setAutoRaise(false);
        button->setFixedSize(40, 40);
        button->setToolTip(tr("Keep %1 fixed").arg(anchorName(entry.anchor)));
        button->setAccessibleName(anchorName(entry.anchor));
        const int id = static_cast<int>(entry.anchor);
        m_anchorGroup->addButton(button, id);
        anchorGrid->addWidget(button, entry.row, entry.column);
        if (entry.anchor == CanvasAnchor::Centre) {
            button->setChecked(true);
        }
    }
    anchorOuter->addLayout(anchorGrid);
    anchorOuter->addStretch();
    root->addWidget(anchorGroupBox);

    auto *behaviourGroup = new QGroupBox(tr("New Area and Clipping"), this);
    auto *behaviourForm = new QFormLayout(behaviourGroup);
    behaviourForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_fillCombo = new QComboBox(behaviourGroup);
    m_fillCombo->addItem(tr("Transparent"),
                         static_cast<int>(CanvasFillMode::Transparent));
    m_fillCombo->addItem(tr("Foreground Colour"),
                         static_cast<int>(CanvasFillMode::Foreground));
    m_fillCombo->addItem(tr("Background Colour"),
                         static_cast<int>(CanvasFillMode::Background));
    m_fillCombo->addItem(tr("Custom Colour"),
                         static_cast<int>(CanvasFillMode::Custom));
    behaviourForm->addRow(tr("New area fill"), m_fillCombo);

    m_customColourButton = new QPushButton(behaviourGroup);
    m_customColourButton->setMinimumHeight(30);
    m_customColourButton->setToolTip(tr("Choose the custom canvas extension colour"));
    behaviourForm->addRow(tr("Custom colour"), m_customColourButton);

    m_deleteOutsideCheck = new QCheckBox(
        tr("Delete pixels outside new canvas"), behaviourGroup);
    m_deleteOutsideCheck->setChecked(false);
    m_deleteOutsideCheck->setToolTip(
        tr("Permanently remove raster and mask storage outside the final "
           "document bounds. The complete operation remains one Undo step."));
    behaviourForm->addRow(QString(), m_deleteOutsideCheck);

    auto *warning = new QLabel(
        tr("Deleting outside pixels is destructive: later expansion or Reveal All "
           "cannot restore them after this history step is discarded."),
        behaviourGroup);
    warning->setWordWrap(true);
    warning->setObjectName(QStringLiteral("CanvasSizeDestructiveWarning"));
    behaviourForm->addRow(QString(), warning);
    root->addWidget(behaviourGroup);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setObjectName(QStringLiteral("CanvasSizeSummary"));
    root->addWidget(m_summaryLabel);

    auto *buttonsBox = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    buttonsBox->button(QDialogButtonBox::Ok)->setText(tr("Resize Canvas"));
    connect(buttonsBox, &QDialogButtonBox::accepted,
            this, &CanvasSizeDialog::accept);
    connect(buttonsBox, &QDialogButtonBox::rejected,
            this, &CanvasSizeDialog::reject);
    root->addWidget(buttonsBox);

    connect(m_modeCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](const int index) {
                setRelativeMode(m_modeCombo->itemData(index).toBool());
            });
    connect(m_widthSpin,
            qOverload<int>(&QSpinBox::valueChanged),
            this,
            [this] { updateTargetFromControls(); });
    connect(m_heightSpin,
            qOverload<int>(&QSpinBox::valueChanged),
            this,
            [this] { updateTargetFromControls(); });
    connect(m_anchorGroup,
            &QButtonGroup::idClicked,
            this,
            [this] { updateSummary(); });
    connect(m_fillCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this] {
                updateFillControls();
                updateSummary();
            });
    connect(m_customColourButton,
            &QPushButton::clicked,
            this,
            &CanvasSizeDialog::chooseCustomColour);
    connect(m_deleteOutsideCheck,
            &QCheckBox::toggled,
            this,
            [this] { updateSummary(); });

    restoreSettings();
    updateControlsFromTarget();
    updateFillControls();
    updateSummary();
    root->activate();
    const QSize comfortable = sizeHint().expandedTo(QSize(500, 680));
    setMinimumSize(comfortable);
    resize(comfortable);
    m_widthSpin->setFocus();
    m_widthSpin->selectAll();
}

QSize CanvasSizeDialog::requestedSize() const
{
    return m_targetSize;
}

CanvasAnchor CanvasSizeDialog::anchor() const
{
    const int id = m_anchorGroup ? m_anchorGroup->checkedId()
                                 : static_cast<int>(CanvasAnchor::Centre);
    if (id < static_cast<int>(CanvasAnchor::TopLeft)
        || id > static_cast<int>(CanvasAnchor::BottomRight)) {
        return CanvasAnchor::Centre;
    }
    return static_cast<CanvasAnchor>(id);
}

CanvasFillMode CanvasSizeDialog::fillMode() const
{
    if (!m_fillCombo) {
        return CanvasFillMode::Transparent;
    }
    const int value = m_fillCombo->currentData().toInt();
    if (value < static_cast<int>(CanvasFillMode::Transparent)
        || value > static_cast<int>(CanvasFillMode::Custom)) {
        return CanvasFillMode::Transparent;
    }
    return static_cast<CanvasFillMode>(value);
}

QColor CanvasSizeDialog::fillColour() const
{
    switch (fillMode()) {
    case CanvasFillMode::Transparent:
        return QColor(0, 0, 0, 0);
    case CanvasFillMode::Foreground:
        return m_foregroundColour;
    case CanvasFillMode::Background:
        return m_backgroundColour;
    case CanvasFillMode::Custom:
        return m_customColour;
    }
    return QColor(0, 0, 0, 0);
}

bool CanvasSizeDialog::deleteOutsideCanvas() const
{
    return m_deleteOutsideCheck && m_deleteOutsideCheck->isChecked();
}

void CanvasSizeDialog::accept()
{
    updateTargetFromControls();
    const quint64 pixels = static_cast<quint64>(m_targetSize.width())
        * static_cast<quint64>(m_targetSize.height());
    // The estimate covers a 16-bit canvas plus at least one full-size working
    // surface. A colour extension or destructive clip can temporarily require
    // further layer-sized allocations, which the operation performs off-thread.
    const quint64 estimatedBytes = pixels * 16u;
    if (estimatedBytes >= 4ull * 1024ull * 1024ull * 1024ull) {
        const auto choice = QMessageBox::warning(
            this,
            tr("Very Large Canvas"),
            tr("The canvas and working surfaces may require approximately %1 "
               "before existing layers and history memory. Resize it anyway?")
                .arg(readableBytes(estimatedBytes)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (choice != QMessageBox::Yes) {
            return;
        }
    }
    saveSettings();
    QDialog::accept();
}

void CanvasSizeDialog::setRelativeMode(const bool relative)
{
    if (m_relative == relative) {
        return;
    }
    m_relative = relative;
    updateControlsFromTarget();
    updateSummary();
}

void CanvasSizeDialog::updateTargetFromControls()
{
    if (m_updating) {
        return;
    }
    if (m_relative) {
        m_targetSize = QSize(m_currentSize.width() + m_widthSpin->value(),
                             m_currentSize.height() + m_heightSpin->value());
    } else {
        m_targetSize = QSize(m_widthSpin->value(), m_heightSpin->value());
    }
    m_targetSize.setWidth(std::clamp(m_targetSize.width(), 1, 32768));
    m_targetSize.setHeight(std::clamp(m_targetSize.height(), 1, 32768));
    updateSummary();
}

void CanvasSizeDialog::updateControlsFromTarget()
{
    m_updating = true;
    const QSignalBlocker widthBlocker(m_widthSpin);
    const QSignalBlocker heightBlocker(m_heightSpin);
    if (m_relative) {
        m_widthSpin->setRange(1 - m_currentSize.width(),
                              32768 - m_currentSize.width());
        m_heightSpin->setRange(1 - m_currentSize.height(),
                               32768 - m_currentSize.height());
        m_widthSpin->setValue(m_targetSize.width() - m_currentSize.width());
        m_heightSpin->setValue(m_targetSize.height() - m_currentSize.height());
    } else {
        m_widthSpin->setRange(1, 32768);
        m_heightSpin->setRange(1, 32768);
        m_widthSpin->setValue(m_targetSize.width());
        m_heightSpin->setValue(m_targetSize.height());
    }
    m_updating = false;
}

void CanvasSizeDialog::updateFillControls()
{
    if (!m_customColourButton) {
        return;
    }
    const bool custom = fillMode() == CanvasFillMode::Custom;
    m_customColourButton->setEnabled(custom);
    m_customColourButton->setIcon(colourIcon(m_customColour));
    m_customColourButton->setIconSize(QSize(48, 22));
    m_customColourButton->setText(colourText(m_customColour));
}

void CanvasSizeDialog::chooseCustomColour()
{
    const QColor chosen = QColorDialog::getColor(
        m_customColour,
        this,
        tr("Choose Canvas Extension Colour"),
        QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) {
        return;
    }
    m_customColour = chosen;
    updateFillControls();
    updateSummary();
}

void CanvasSizeDialog::updateSummary()
{
    if (!m_summaryLabel) {
        return;
    }
    const QRect requested = canvasSizeDocumentRect(
        m_currentSize, m_targetSize, anchor());
    const QPoint oldCanvasPosition(-requested.x(), -requested.y());
    const QRect oldBounds(QPoint(), m_currentSize);
    const bool exposesNewArea = !oldBounds.contains(requested);

    QString fillDescription;
    if (!exposesNewArea) {
        fillDescription = tr("No newly exposed canvas area will be created.");
    } else if (fillMode() == CanvasFillMode::Transparent) {
        fillDescription = tr("Newly exposed pixels remain transparent and no fill layer is created.");
    } else {
        fillDescription = tr("Newly exposed regions are placed on a bottom “Canvas Extension” layer using %1 (%2).")
            .arg(fillModeName(fillMode()), colourText(fillColour()));
    }

    const QString clippingDescription = deleteOutsideCanvas()
        ? tr(" Raster and mask storage outside the final bounds will be permanently deleted.")
        : tr(" Off-canvas raster and mask storage remains preserved.");

    m_summaryLabel->setText(
        tr("%1 × %2 px → %3 × %4 px. Anchor: %5. "
           "The existing canvas begins at %6, %7 in the resized document. %8%9")
            .arg(m_currentSize.width())
            .arg(m_currentSize.height())
            .arg(m_targetSize.width())
            .arg(m_targetSize.height())
            .arg(anchorName(anchor()))
            .arg(oldCanvasPosition.x())
            .arg(oldCanvasPosition.y())
            .arg(fillDescription, clippingDescription));
}

void CanvasSizeDialog::restoreSettings()
{
    QSettings settings;
    const bool relative = settings.value(
        QStringLiteral("canvasSize/relative"), false).toBool();
    const int anchorId = settings.value(
        QStringLiteral("canvasSize/anchor"),
        static_cast<int>(CanvasAnchor::Centre)).toInt();
    const QColor storedCustom = settings.value(
        QStringLiteral("canvasSize/customColour"), QColor(Qt::white)).value<QColor>();
    if (storedCustom.isValid()) {
        m_customColour = storedCustom;
    }
    m_relative = relative;
    const int modeIndex = m_modeCombo->findData(relative);
    if (modeIndex >= 0) {
        const QSignalBlocker blocker(m_modeCombo);
        m_modeCombo->setCurrentIndex(modeIndex);
    }
    if (QAbstractButton *button = m_anchorGroup->button(anchorId)) {
        button->setChecked(true);
    }
    // Transparent and non-destructive are intentionally restored as the safe
    // defaults each time the dialog is opened.
    const int transparentIndex = m_fillCombo->findData(
        static_cast<int>(CanvasFillMode::Transparent));
    if (transparentIndex >= 0) {
        const QSignalBlocker blocker(m_fillCombo);
        m_fillCombo->setCurrentIndex(transparentIndex);
    }
    m_deleteOutsideCheck->setChecked(false);
}

void CanvasSizeDialog::saveSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("canvasSize/relative"), m_relative);
    settings.setValue(QStringLiteral("canvasSize/anchor"),
                      static_cast<int>(anchor()));
    settings.setValue(QStringLiteral("canvasSize/customColour"), m_customColour);
}

} // namespace vfx
