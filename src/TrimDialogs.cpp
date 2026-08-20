#include "TrimDialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace vfx {

TrimTransparentDialog::TrimTransparentDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Trim Transparent Pixels"));
    setModal(true);
    setMinimumWidth(430);

    auto *layout = new QVBoxLayout(this);
    auto *heading = new QLabel(tr("Trim transparent canvas borders"), this);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    auto *description = new QLabel(
        tr("The visible merged composite is analysed. The canvas is fitted to "
           "every pixel whose final Alpha is greater than zero. A fully "
           "transparent document is left unchanged."),
        this);
    description->setWordWrap(true);
    layout->addWidget(description);

    m_deleteOutsideCanvas = new QCheckBox(
        tr("Delete pixels outside the trimmed canvas"), this);
    m_deleteOutsideCanvas->setChecked(false);
    m_deleteOutsideCanvas->setToolTip(
        tr("Off-canvas raster and mask storage is preserved unless this is enabled."));
    layout->addWidget(m_deleteOutsideCanvas);

    auto *warning = new QLabel(
        tr("Destructive deletion is exact and Undoable, but re-expanding the "
           "canvas afterwards will not reveal the deleted pixels."),
        this);
    warning->setWordWrap(true);
    layout->addWidget(warning);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Trim Canvas"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

bool TrimTransparentDialog::deleteOutsideCanvas() const
{
    return m_deleteOutsideCanvas && m_deleteOutsideCanvas->isChecked();
}

TrimCornerColourDialog::TrimCornerColourDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Trim by Corner Colour"));
    setModal(true);
    setMinimumWidth(470);

    auto *layout = new QVBoxLayout(this);
    auto *heading = new QLabel(tr("Trim borders matching a corner colour"), this);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    auto *description = new QLabel(
        tr("The colour is sampled from the visible merged composite. RGB and "
           "Alpha are compared with the selected tolerance. Fully transparent "
           "pixels match one another regardless of hidden RGB."),
        this);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *samplingGroup = new QGroupBox(tr("Sampling"), this);
    auto *samplingLayout = new QFormLayout(samplingGroup);
    m_corner = new QComboBox(samplingGroup);
    m_corner->addItem(tr("Top-left"),
                      static_cast<int>(TrimSampleCorner::TopLeft));
    m_corner->addItem(tr("Top-right"),
                      static_cast<int>(TrimSampleCorner::TopRight));
    m_corner->addItem(tr("Bottom-left"),
                      static_cast<int>(TrimSampleCorner::BottomLeft));
    m_corner->addItem(tr("Bottom-right"),
                      static_cast<int>(TrimSampleCorner::BottomRight));
    samplingLayout->addRow(tr("Sample corner"), m_corner);

    m_tolerance = new QSpinBox(samplingGroup);
    m_tolerance->setRange(0, 255);
    m_tolerance->setValue(0);
    m_tolerance->setSuffix(tr(" / 255"));
    m_tolerance->setToolTip(
        tr("Maximum inclusive difference allowed in each straight R, G, B and Alpha channel."));
    samplingLayout->addRow(tr("Tolerance"), m_tolerance);
    layout->addWidget(samplingGroup);

    auto *sidesGroup = new QGroupBox(tr("Sides to trim"), this);
    auto *sidesLayout = new QHBoxLayout(sidesGroup);
    m_trimTop = new QCheckBox(tr("Top"), sidesGroup);
    m_trimBottom = new QCheckBox(tr("Bottom"), sidesGroup);
    m_trimLeft = new QCheckBox(tr("Left"), sidesGroup);
    m_trimRight = new QCheckBox(tr("Right"), sidesGroup);
    for (QCheckBox *box : {m_trimTop, m_trimBottom, m_trimLeft, m_trimRight}) {
        box->setChecked(true);
        sidesLayout->addWidget(box);
        connect(box, &QCheckBox::toggled,
                this, &TrimCornerColourDialog::updateAcceptState);
    }
    sidesLayout->addStretch(1);
    layout->addWidget(sidesGroup);

    m_deleteOutsideCanvas = new QCheckBox(
        tr("Delete pixels outside the trimmed canvas"), this);
    m_deleteOutsideCanvas->setChecked(false);
    m_deleteOutsideCanvas->setToolTip(
        tr("Off-canvas raster and mask storage is preserved unless this is enabled."));
    layout->addWidget(m_deleteOutsideCanvas);

    m_buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Trim Canvas"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);
    updateAcceptState();
}

AutomaticTrimRequest TrimCornerColourDialog::request() const
{
    AutomaticTrimRequest output;
    output.mode = AutomaticTrimMode::CornerColour;
    output.sampleCorner = static_cast<TrimSampleCorner>(
        m_corner->currentData().toInt());
    output.tolerance = m_tolerance->value();
    output.trimTop = m_trimTop->isChecked();
    output.trimBottom = m_trimBottom->isChecked();
    output.trimLeft = m_trimLeft->isChecked();
    output.trimRight = m_trimRight->isChecked();
    output.deleteOutsideCanvas = m_deleteOutsideCanvas->isChecked();
    return output;
}

void TrimCornerColourDialog::updateAcceptState()
{
    const bool anySide = m_trimTop->isChecked()
        || m_trimBottom->isChecked()
        || m_trimLeft->isChecked()
        || m_trimRight->isChecked();
    if (m_buttons) {
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(anySide);
    }
}

} // namespace vfx
