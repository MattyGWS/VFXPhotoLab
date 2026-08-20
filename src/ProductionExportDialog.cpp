#include "ProductionExportDialog.h"

#include "ExportNamingTemplate.h"
#include "PresetManagerDialog.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QScrollArea>
#include <QStandardPaths>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace vfx {
namespace {

QString profileDescription(const ExportProfileData &data)
{
    const QString colour = data.convertToOutputProfile
        ? data.output.profile.displayName : QObject::tr("Keep working RGB");
    return QStringLiteral("%1 · %2 · %3 · %4")
        .arg(data.formatSuffix.toUpper(),
             imageExportBitDepthName(data.bitDepth),
             colour.trimmed().isEmpty() ? QObject::tr("Untagged") : colour,
             imageExportAlphaModeName(data.alphaMode));
}

QString defaultOutputDirectory()
{
    const QString pictures = QStandardPaths::writableLocation(
        QStandardPaths::PicturesLocation);
    return pictures.isEmpty() ? QDir::homePath() : pictures;
}

ProductionExportCollisionPolicy policyFromCombo(const QComboBox *combo)
{
    return static_cast<ProductionExportCollisionPolicy>(
        combo->currentData().toInt());
}

} // namespace

ProductionExportDialog::ProductionExportDialog(
    const QString &documentName,
    const QSize &documentSize,
    const DocumentColourState &colourState,
    QWidget *parent)
    : QDialog(parent)
    , m_documentName(documentName)
    , m_documentSize(documentSize)
    , m_colourState(colourState)
    , m_timestampUtc(QDateTime::currentDateTimeUtc())
{
    setWindowTitle(tr("Production Export"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        tr("Configure several independently named and resized outputs from one captured document snapshot. The job will be added to the Export Queue, where completed files remain intact if another output fails."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *destinationGroup = new QGroupBox(tr("Destination and collisions"), this);
    auto *destinationForm = new QFormLayout(destinationGroup);
    auto *directoryRow = new QWidget(destinationGroup);
    auto *directoryLayout = new QHBoxLayout(directoryRow);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    QSettings settings;
    QString initialDirectory = settings.value(
        QStringLiteral("productionExport/outputDirectory"),
        defaultOutputDirectory()).toString();
    if (!QFileInfo(initialDirectory).isDir()) {
        initialDirectory = defaultOutputDirectory();
    }
    m_directoryEdit = new QLineEdit(initialDirectory, directoryRow);
    auto *browse = new QPushButton(tr("Browse…"), directoryRow);
    directoryLayout->addWidget(m_directoryEdit, 1);
    directoryLayout->addWidget(browse);
    destinationForm->addRow(tr("Output directory"), directoryRow);
    m_collisionPolicy = new QComboBox(destinationGroup);
    m_collisionPolicy->addItem(tr("Ask before replacing existing files"),
        static_cast<int>(ProductionExportCollisionPolicy::AskBeforeStart));
    m_collisionPolicy->addItem(tr("Replace existing files"),
        static_cast<int>(ProductionExportCollisionPolicy::Overwrite));
    m_collisionPolicy->addItem(tr("Skip existing files"),
        static_cast<int>(ProductionExportCollisionPolicy::SkipExisting));
    m_collisionPolicy->addItem(tr("Auto-rename new files"),
        static_cast<int>(ProductionExportCollisionPolicy::AutoRename));
    const int storedPolicy = settings.value(
        QStringLiteral("productionExport/collisionPolicy"),
        static_cast<int>(ProductionExportCollisionPolicy::AskBeforeStart)).toInt();
    const int storedPolicyIndex = m_collisionPolicy->findData(storedPolicy);
    if (storedPolicyIndex >= 0) {
        m_collisionPolicy->setCurrentIndex(storedPolicyIndex);
    }
    destinationForm->addRow(tr("Existing files"), m_collisionPolicy);
    layout->addWidget(destinationGroup);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    auto *outputsPanel = new QWidget(splitter);
    auto *outputsLayout = new QVBoxLayout(outputsPanel);
    m_table = new QTableWidget(outputsPanel);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({tr("On"), tr("Profile"), tr("Size"),
                                       tr("Encoding / colour"), tr("Output path")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    outputsLayout->addWidget(m_table, 1);
    auto *outputButtons = new QHBoxLayout;
    m_addButton = new QPushButton(tr("Add"), outputsPanel);
    m_duplicateButton = new QPushButton(tr("Duplicate"), outputsPanel);
    m_removeButton = new QPushButton(tr("Remove"), outputsPanel);
    m_upButton = new QPushButton(tr("Up"), outputsPanel);
    m_downButton = new QPushButton(tr("Down"), outputsPanel);
    outputButtons->addWidget(m_addButton);
    outputButtons->addWidget(m_duplicateButton);
    outputButtons->addWidget(m_removeButton);
    outputButtons->addStretch(1);
    outputButtons->addWidget(m_upButton);
    outputButtons->addWidget(m_downButton);
    outputsLayout->addLayout(outputButtons);

    auto *editorScroll = new QScrollArea(splitter);
    editorScroll->setWidgetResizable(true);
    editorScroll->setFrameShape(QFrame::NoFrame);
    auto *editorPanel = new QWidget(editorScroll);
    auto *editorLayout = new QVBoxLayout(editorPanel);
    editorScroll->setWidget(editorPanel);
    auto *settingsGroup = new QGroupBox(tr("Selected output"), editorPanel);
    auto *form = new QFormLayout(settingsGroup);
    m_enabledCheck = new QCheckBox(tr("Include this output"), settingsGroup);
    form->addRow(QString(), m_enabledCheck);
    auto *profileRow = new QWidget(settingsGroup);
    auto *profileLayout = new QHBoxLayout(profileRow);
    profileLayout->setContentsMargins(0, 0, 0, 0);
    m_profileCombo = new QComboBox(profileRow);
    m_manageProfilesButton = new QPushButton(tr("Manage…"), profileRow);
    profileLayout->addWidget(m_profileCombo, 1);
    profileLayout->addWidget(m_manageProfilesButton);
    form->addRow(tr("Export profile"), profileRow);
    m_profileSummary = new QLabel(settingsGroup);
    m_profileSummary->setWordWrap(true);
    form->addRow(QString(), m_profileSummary);
    m_namingTemplate = new QLineEdit(settingsGroup);
    m_namingTemplate->setMaxLength(ExportNamingTemplate::MaximumTemplateLength);
    form->addRow(tr("Filename template"), m_namingTemplate);
    auto *tokens = new QLabel(ExportNamingTemplate::tokenHelpText(), settingsGroup);
    tokens->setWordWrap(true);
    tokens->setProperty("class", "muted");
    form->addRow(QString(), tokens);

    m_resizeMode = new QComboBox(settingsGroup);
    m_resizeMode->addItem(tr("Original document size"),
        static_cast<int>(ProductionExportResizeMode::OriginalSize));
    m_resizeMode->addItem(tr("Fit inside exact pixel dimensions"),
        static_cast<int>(ProductionExportResizeMode::ExactPixels));
    m_resizeMode->addItem(tr("Set long edge"),
        static_cast<int>(ProductionExportResizeMode::LongEdge));
    m_resizeMode->addItem(tr("Scale by percentage"),
        static_cast<int>(ProductionExportResizeMode::Percentage));
    form->addRow(tr("Resize"), m_resizeMode);
    m_width = new QSpinBox(settingsGroup);
    m_width->setRange(1, 32768);
    m_width->setValue(documentSize.width());
    form->addRow(tr("Maximum width"), m_width);
    m_height = new QSpinBox(settingsGroup);
    m_height->setRange(1, 32768);
    m_height->setValue(documentSize.height());
    form->addRow(tr("Maximum height"), m_height);
    m_preserveAspect = new QCheckBox(tr("Preserve aspect ratio"), settingsGroup);
    m_preserveAspect->setChecked(true);
    form->addRow(QString(), m_preserveAspect);
    m_longEdge = new QSpinBox(settingsGroup);
    m_longEdge->setRange(1, 32768);
    m_longEdge->setValue(std::max(documentSize.width(), documentSize.height()));
    form->addRow(tr("Long edge"), m_longEdge);
    m_percentage = new QDoubleSpinBox(settingsGroup);
    m_percentage->setRange(0.1, 3200.0);
    m_percentage->setDecimals(1);
    m_percentage->setSuffix(QStringLiteral("%"));
    m_percentage->setValue(100.0);
    form->addRow(tr("Percentage"), m_percentage);
    m_resampleMethod = new QComboBox(settingsGroup);
    const ImageResampleMethod methods[] = {
        ImageResampleMethod::NearestNeighbour,
        ImageResampleMethod::Bilinear,
        ImageResampleMethod::Bicubic,
        ImageResampleMethod::Lanczos3,
        ImageResampleMethod::Area
    };
    for (const ImageResampleMethod method : methods) {
        m_resampleMethod->addItem(imageResampleMethodName(method),
                                  static_cast<int>(method));
    }
    m_resampleMethod->setCurrentIndex(
        m_resampleMethod->findData(static_cast<int>(ImageResampleMethod::Bicubic)));
    form->addRow(tr("Resampling"), m_resampleMethod);
    m_pathPreview = new QLabel(settingsGroup);
    m_pathPreview->setWordWrap(true);
    m_pathPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Resolved output"), m_pathPreview);
    editorLayout->addWidget(settingsGroup);
    m_planSummary = new QLabel(editorPanel);
    m_planSummary->setWordWrap(true);
    editorLayout->addWidget(m_planSummary);
    editorLayout->addStretch(1);

    splitter->addWidget(outputsPanel);
    splitter->addWidget(editorScroll);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Add to Queue"));
    layout->addWidget(buttons);

    connect(browse, &QPushButton::clicked, this,
            &ProductionExportDialog::chooseDirectory);
    connect(m_addButton, &QPushButton::clicked, this, [this] {
        addOutput(m_profileCombo->currentData().toString());
    });
    connect(m_duplicateButton, &QPushButton::clicked, this,
            &ProductionExportDialog::duplicateSelectedOutput);
    connect(m_removeButton, &QPushButton::clicked, this,
            &ProductionExportDialog::removeSelectedOutput);
    connect(m_upButton, &QPushButton::clicked, this, [this] { moveSelectedOutput(-1); });
    connect(m_downButton, &QPushButton::clicked, this, [this] { moveSelectedOutput(1); });
    connect(m_manageProfilesButton, &QPushButton::clicked, this,
            &ProductionExportDialog::openProfileManager);
    connect(m_table, &QTableWidget::itemSelectionChanged, this,
            &ProductionExportDialog::loadSelectedOutput);
    connect(buttons, &QDialogButtonBox::accepted, this,
            &ProductionExportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this,
            &ProductionExportDialog::reject);

    const auto changed = [this] {
        if (m_loadingEditor) return;
        const int row = selectedOutputIndex();
        storeSelectedOutput();
        rebuildTable(row, false);
    };
    const auto valueEdited = [this] {
        if (m_loadingEditor) return;
        storeSelectedOutput();
        refreshEditor();
    };
    const auto finishValueEdit = [this] {
        if (m_loadingEditor) return;
        rebuildTable(selectedOutputIndex(), false);
    };
    connect(m_enabledCheck, &QCheckBox::toggled, this, changed);
    connect(m_profileCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, changed);
    connect(m_namingTemplate, &QLineEdit::textChanged, this, valueEdited);
    connect(m_namingTemplate, &QLineEdit::editingFinished,
            this, finishValueEdit);
    connect(m_resizeMode, qOverload<int>(&QComboBox::currentIndexChanged),
            this, changed);
    connect(m_width, qOverload<int>(&QSpinBox::valueChanged), this, valueEdited);
    connect(m_width, &QSpinBox::editingFinished, this, finishValueEdit);
    connect(m_height, qOverload<int>(&QSpinBox::valueChanged), this, valueEdited);
    connect(m_height, &QSpinBox::editingFinished, this, finishValueEdit);
    connect(m_preserveAspect, &QCheckBox::toggled, this, changed);
    connect(m_longEdge, qOverload<int>(&QSpinBox::valueChanged), this, valueEdited);
    connect(m_longEdge, &QSpinBox::editingFinished, this, finishValueEdit);
    connect(m_percentage, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, valueEdited);
    connect(m_percentage, &QDoubleSpinBox::editingFinished,
            this, finishValueEdit);
    connect(m_resampleMethod, qOverload<int>(&QComboBox::currentIndexChanged),
            this, changed);
    connect(m_directoryEdit, &QLineEdit::textChanged, this,
            &ProductionExportDialog::refreshPreview);
    connect(m_directoryEdit, &QLineEdit::editingFinished, this, [this] {
        rebuildTable(selectedOutputIndex());
    });
    connect(m_collisionPolicy, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { rebuildTable(selectedOutputIndex()); });

    reloadProfiles();
    addOutput();
    const QSize available = screen() ? screen()->availableGeometry().size()
                                     : QSize(1600, 900);
    const QSize target(std::min(1180, std::max(360, available.width() - 60)),
                       std::min(760, std::max(360, available.height() - 60)));
    setMinimumSize(QSize(std::min(820, target.width()),
                         std::min(500, target.height())));
    resize(target.expandedTo(minimumSize()));
}

ProductionExportPlan ProductionExportDialog::plan() const
{
    ProductionExportPlan result;
    result.outputDirectory = m_directoryEdit->text().trimmed();
    result.documentName = m_documentName;
    result.documentSize = m_documentSize;
    result.workingSpaceName = m_colourState.workingSpace.displayName;
    result.timestampUtc = m_timestampUtc;
    result.collisionPolicy = policyFromCombo(m_collisionPolicy);
    result.outputs = m_outputs;
    return result;
}

void ProductionExportDialog::reloadProfiles(const QString &preferredProfileId)
{
    QStringList warnings;
    m_profiles = ExportProfileStore::profiles(&warnings);
    const QString current = preferredProfileId.isEmpty()
        ? m_profileCombo->currentData().toString() : preferredProfileId;
    QSignalBlocker blocker(m_profileCombo);
    m_profileCombo->clear();
    for (const ExportProfile &profile : std::as_const(m_profiles)) {
        QString label = profile.name;
        if (profile.metadata.favourite) label.prepend(QStringLiteral("★ "));
        if (profile.builtIn) label += tr("  [Built-in]");
        m_profileCombo->addItem(label, profile.metadata.id);
    }
    int index = m_profileCombo->findData(current);
    if (index < 0 && !current.isEmpty()) {
        QString missingName = tr("Missing profile snapshot");
        const auto output = std::find_if(m_outputs.cbegin(), m_outputs.cend(),
            [&current](const ProductionExportOutput &candidate) {
                return candidate.profileId == current;
            });
        if (output != m_outputs.cend() && !output->profileName.trimmed().isEmpty()) {
            missingName = output->profileName;
        }
        m_profileCombo->insertItem(0,
            tr("%1  [Missing — using stored settings]").arg(missingName),
            current);
        index = 0;
    }
    if (index < 0) index = 0;
    m_profileCombo->setCurrentIndex(index);
}

const ExportProfile *ProductionExportDialog::profileById(const QString &id) const
{
    const auto found = std::find_if(m_profiles.cbegin(), m_profiles.cend(),
        [&id](const ExportProfile &profile) {
            return profile.metadata.id == id;
        });
    return found == m_profiles.cend() ? nullptr : &*found;
}

void ProductionExportDialog::addOutput(const QString &profileId)
{
    if (m_outputs.size() >= 32) {
        QMessageBox::warning(this, tr("Production Output Limit"),
                             tr("A production export may contain at most 32 outputs."));
        return;
    }
    if (m_profiles.isEmpty()) {
        QMessageBox::warning(this, tr("No Export Profiles"),
                             tr("No valid export profiles are available."));
        return;
    }
    const ExportProfile *profile = profileById(profileId);
    if (!profile) profile = &m_profiles.constFirst();
    ProductionExportOutput output;
    output.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    output.profileId = profile->metadata.id;
    output.profileName = profile->name;
    output.profile = profile->data;
    output.namingTemplate = profile->data.namingTemplate;
    output.resize.width = m_documentSize.width();
    output.resize.height = m_documentSize.height();
    output.resize.longEdge = std::max(m_documentSize.width(), m_documentSize.height());
    m_outputs.push_back(std::move(output));
    rebuildTable(m_outputs.size() - 1);
}

void ProductionExportDialog::duplicateSelectedOutput()
{
    const int row = selectedOutputIndex();
    if (row < 0) return;
    if (m_outputs.size() >= 32) {
        QMessageBox::warning(this, tr("Production Output Limit"),
                             tr("A production export may contain at most 32 outputs."));
        return;
    }
    ProductionExportOutput copy = m_outputs.at(row);
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_outputs.insert(row + 1, std::move(copy));
    rebuildTable(row + 1);
}

void ProductionExportDialog::removeSelectedOutput()
{
    const int row = selectedOutputIndex();
    if (row < 0) return;
    m_outputs.removeAt(row);
    rebuildTable(std::min(row, static_cast<int>(m_outputs.size()) - 1));
}

void ProductionExportDialog::moveSelectedOutput(const int delta)
{
    const int row = selectedOutputIndex();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= m_outputs.size()) return;
    m_outputs.swapItemsAt(row, target);
    rebuildTable(target);
}

int ProductionExportDialog::selectedOutputIndex() const
{
    const QModelIndexList rows = m_table->selectionModel()
        ? m_table->selectionModel()->selectedRows() : QModelIndexList();
    return rows.isEmpty() ? -1 : rows.constFirst().row();
}

void ProductionExportDialog::rebuildTable(const int preferredRow,
                                                const bool reloadEditor)
{
    const int previous = preferredRow >= 0 ? preferredRow : selectedOutputIndex();
    QSignalBlocker blocker(m_table);
    m_table->setRowCount(m_outputs.size());
    for (int row = 0; row < m_outputs.size(); ++row) {
        const ProductionExportOutput &output = m_outputs.at(row);
        QString sizeError;
        const QSize size = output.resize.resolvedSize(m_documentSize, &sizeError);
        QString outputPath = output.namingTemplate;
        QString pathStatus;
        ProductionExportPlan previewPlan = plan();
        previewPlan.outputs = {output};
        previewPlan.outputs[0].enabled = true;
        previewPlan.collisionPolicy = policyFromCombo(m_collisionPolicy);
        QVector<ResolvedProductionExportOutput> previewOutputs;
        QString previewError;
        if (resolveProductionExportPlan(previewPlan, m_colourState,
                                        &previewOutputs, nullptr,
                                        &previewError)
            && !previewOutputs.isEmpty()) {
            outputPath = previewOutputs.constFirst().request.filePath;
            if (QFileInfo::exists(outputPath)) {
                switch (policyFromCombo(m_collisionPolicy)) {
                case ProductionExportCollisionPolicy::AskBeforeStart:
                    pathStatus = tr("  [confirmation required]");
                    break;
                case ProductionExportCollisionPolicy::Overwrite:
                    pathStatus = tr("  [replace]");
                    break;
                case ProductionExportCollisionPolicy::SkipExisting:
                    pathStatus = tr("  [skip]");
                    break;
                case ProductionExportCollisionPolicy::AutoRename:
                    pathStatus = tr("  [auto-rename]");
                    break;
                }
            }
        } else if (!previewError.isEmpty()) {
            outputPath = tr("Invalid: %1").arg(previewError);
        }
        if (!output.enabled) pathStatus = tr("  [disabled]");
        const QString colourName = output.profile.convertToOutputProfile
            ? output.profile.output.profile.displayName
            : tr("Working RGB");
        const QString encoding = QStringLiteral("%1 · %2 · %3")
            .arg(output.profile.formatSuffix.toUpper(),
                 imageExportBitDepthName(output.profile.bitDepth),
                 colourName.trimmed().isEmpty() ? tr("Untagged") : colourName);
        const QString cells[] = {
            output.enabled ? tr("Yes") : tr("No"),
            output.profileName,
            size.isEmpty() ? tr("Invalid")
                : QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()),
            encoding,
            outputPath + pathStatus
        };
        for (int column = 0; column < 5; ++column) {
            auto *item = new QTableWidgetItem(cells[column]);
            item->setToolTip(column == 4 ? outputPath
                                         : profileDescription(output.profile));
            if (!output.enabled) {
                item->setForeground(
                    palette().color(QPalette::Disabled, QPalette::Text));
            }
            m_table->setItem(row, column, item);
        }
    }
    if (!m_outputs.isEmpty()) {
        const int row = std::clamp(
            previous, 0, static_cast<int>(m_outputs.size()) - 1);
        m_table->selectRow(row);
        if (reloadEditor) {
            loadSelectedOutput();
        } else {
            refreshEditor();
        }
    } else {
        loadSelectedOutput();
    }
}

void ProductionExportDialog::loadSelectedOutput()
{
    const int row = selectedOutputIndex();
    const bool available = row >= 0 && row < m_outputs.size();
    m_loadingEditor = true;
    m_enabledCheck->setEnabled(available);
    m_profileCombo->setEnabled(available);
    m_namingTemplate->setEnabled(available);
    m_resizeMode->setEnabled(available);
    m_resampleMethod->setEnabled(available);
    m_duplicateButton->setEnabled(available);
    m_removeButton->setEnabled(available);
    m_upButton->setEnabled(available && row > 0);
    m_downButton->setEnabled(available && row + 1 < m_outputs.size());
    if (available) {
        const ProductionExportOutput &output = m_outputs.at(row);
        m_enabledCheck->setChecked(output.enabled);
        bool containsSyntheticProfile = false;
        for (int index = 0; index < m_profileCombo->count(); ++index) {
            const QString candidateId = m_profileCombo->itemData(index).toString();
            if (!candidateId.isEmpty() && !profileById(candidateId)) {
                containsSyntheticProfile = true;
                break;
            }
        }
        int profileIndex = m_profileCombo->findData(output.profileId);
        if (containsSyntheticProfile
            || (profileIndex < 0 && !output.profileId.isEmpty())) {
            // A synthetic missing-profile entry belongs only to the output that
            // requested it. Rebuild the combo when selection changes so another
            // output cannot accidentally choose a stale snapshot placeholder.
            reloadProfiles(output.profileId);
            profileIndex = m_profileCombo->findData(output.profileId);
        }
        if (profileIndex < 0) profileIndex = 0;
        m_profileCombo->setCurrentIndex(profileIndex);
        m_namingTemplate->setText(output.namingTemplate);
        m_resizeMode->setCurrentIndex(m_resizeMode->findData(
            static_cast<int>(output.resize.mode)));
        m_width->setValue(std::max(1, output.resize.width));
        m_height->setValue(std::max(1, output.resize.height));
        m_preserveAspect->setChecked(output.resize.preserveAspect);
        m_longEdge->setValue(std::max(1, output.resize.longEdge));
        m_percentage->setValue(output.resize.percentage);
        m_resampleMethod->setCurrentIndex(m_resampleMethod->findData(
            static_cast<int>(output.resize.method)));
    }
    m_loadingEditor = false;
    refreshEditor();
}

void ProductionExportDialog::storeSelectedOutput()
{
    const int row = selectedOutputIndex();
    if (row < 0 || row >= m_outputs.size()) return;
    ProductionExportOutput &output = m_outputs[row];
    output.enabled = m_enabledCheck->isChecked();
    const QString previousProfileId = output.profileId;
    const QString profileId = m_profileCombo->currentData().toString();
    if (previousProfileId != profileId) {
        if (const ExportProfile *profile = profileById(profileId)) {
            output.profileId = profile->metadata.id;
            output.profileName = profile->name;
            output.profile = profile->data;
            output.namingTemplate = profile->data.namingTemplate;
            QSignalBlocker blocker(m_namingTemplate);
            m_namingTemplate->setText(output.namingTemplate);
        }
    }
    output.namingTemplate = m_namingTemplate->text();
    output.resize.mode = static_cast<ProductionExportResizeMode>(
        m_resizeMode->currentData().toInt());
    output.resize.width = m_width->value();
    output.resize.height = m_height->value();
    output.resize.preserveAspect = m_preserveAspect->isChecked();
    output.resize.longEdge = m_longEdge->value();
    output.resize.percentage = m_percentage->value();
    output.resize.method = static_cast<ImageResampleMethod>(
        m_resampleMethod->currentData().toInt());
}

void ProductionExportDialog::refreshEditor()
{
    const int row = selectedOutputIndex();
    const bool available = row >= 0 && row < m_outputs.size();
    const ProductionExportResizeMode mode = static_cast<ProductionExportResizeMode>(
        m_resizeMode->currentData().toInt());
    m_width->setEnabled(available && mode == ProductionExportResizeMode::ExactPixels);
    m_height->setEnabled(available && mode == ProductionExportResizeMode::ExactPixels);
    m_preserveAspect->setEnabled(available && mode == ProductionExportResizeMode::ExactPixels);
    m_longEdge->setEnabled(available && mode == ProductionExportResizeMode::LongEdge);
    m_percentage->setEnabled(available && mode == ProductionExportResizeMode::Percentage);
    m_resampleMethod->setEnabled(available && mode != ProductionExportResizeMode::OriginalSize);
    m_profileSummary->setText(available
        ? profileDescription(selectedOutputProfileData()) : tr("No output selected."));
    refreshPreview();
}

void ProductionExportDialog::refreshPreview()
{
    ProductionExportPlan current = plan();
    QVector<ResolvedProductionExportOutput> resolved;
    QStringList warnings;
    QString error;
    if (!resolveProductionExportPlan(current, m_colourState,
                                     &resolved, &warnings, &error)) {
        m_pathPreview->setText(error);
        m_planSummary->setText(tr("The production-export plan is not ready."));
        return;
    }
    const int row = selectedOutputIndex();
    QString selectedPath;
    if (row >= 0 && row < m_outputs.size()) {
        const QString selectedId = m_outputs.at(row).id;
        const auto found = std::find_if(resolved.cbegin(), resolved.cend(),
            [&selectedId](const ResolvedProductionExportOutput &output) {
                return output.id == selectedId;
            });
        if (found != resolved.cend()) selectedPath = found->request.filePath;
    }
    m_pathPreview->setText(selectedPath.isEmpty()
        ? tr("Select an enabled valid output to preview its path.")
        : selectedPath);
    int skipped = 0;
    for (const auto &output : resolved) if (output.skipExisting) ++skipped;
    m_planSummary->setText(
        tr("%1 enabled output(s) · %2 existing file(s) detected · %3")
            .arg(resolved.size())
            .arg(warnings.size() + skipped)
            .arg(productionExportCollisionPolicyName(current.collisionPolicy)));
}

void ProductionExportDialog::chooseDirectory()
{
    const QString selected = QFileDialog::getExistingDirectory(
        this, tr("Choose Production Export Directory"),
        m_directoryEdit->text().trimmed());
    if (!selected.isEmpty()) {
        m_directoryEdit->setText(selected);
        rebuildTable(selectedOutputIndex());
    }
}

ExportProfileData ProductionExportDialog::selectedOutputProfileData() const
{
    const int row = selectedOutputIndex();
    if (row < 0 || row >= m_outputs.size()) return ExportProfileData();
    ExportProfileData data = m_outputs.at(row).profile;
    data.namingTemplate = m_outputs.at(row).namingTemplate;
    return data;
}

void ProductionExportDialog::openProfileManager()
{
    PresetManagerCallbacks callbacks;
    callbacks.load = [this](QStringList *warnings) {
        m_profiles = ExportProfileStore::profiles(warnings);
        QVector<PresetManagerEntry> entries;
        entries.reserve(m_profiles.size());
        for (const ExportProfile &profile : std::as_const(m_profiles)) {
            PresetManagerEntry entry;
            entry.id = profile.metadata.id;
            entry.name = profile.name;
            entry.category = profile.metadata.category;
            entry.tags = profile.metadata.tags;
            entry.favourite = profile.metadata.favourite;
            entry.builtIn = profile.builtIn;
            entry.createdUtcMs = profile.metadata.createdUtcMs;
            entry.modifiedUtcMs = profile.metadata.modifiedUtcMs;
            entry.lastUsedUtcMs = profile.metadata.lastUsedUtcMs;
            entry.useCount = profile.metadata.useCount;
            entry.storagePath = profile.storagePath;
            entry.description = profileDescription(profile.data);
            entries.push_back(std::move(entry));
        }
        return entries;
    };
    callbacks.apply = [this](const QString &id, QString *error) {
        const ExportProfile *profilePointer = profileById(id);
        if (!profilePointer) {
            if (error) *error = tr("The selected export profile is no longer available.");
            return false;
        }
        const ExportProfile profile = *profilePointer;
        const int row = selectedOutputIndex();
        if (row < 0) {
            if (error) *error = tr("Select a production output first.");
            return false;
        }
        m_outputs[row].profileId = profile.metadata.id;
        m_outputs[row].profileName = profile.name;
        m_outputs[row].profile = profile.data;
        m_outputs[row].namingTemplate = profile.data.namingTemplate;
        QString usageError;
        ExportProfileStore::recordUse(profile, &usageError);
        reloadProfiles(id);
        rebuildTable(row);
        return true;
    };
    const auto requireProfile = [this](const QString &id,
                                           QString *error) -> const ExportProfile * {
        const ExportProfile *profile = profileById(id);
        if (!profile && error) {
            *error = tr("The selected export profile is no longer available.");
        }
        return profile;
    };
    callbacks.createFromCurrent = [this](const QString &name,
        const QString &category, const QStringList &tags,
        QString *createdId, QString *error) {
        if (selectedOutputIndex() < 0) {
            if (error) *error = tr("Select a production output first.");
            return false;
        }
        return ExportProfileStore::createUserProfile(
            name, selectedOutputProfileData(), category, tags,
            createdId, error);
    };
    callbacks.updateFromCurrent = [this, requireProfile](const QString &id,
                                                         QString *error) {
        const ExportProfile *profile = requireProfile(id, error);
        return profile && ExportProfileStore::updateUserProfile(
            *profile, selectedOutputProfileData(), error);
    };
    callbacks.rename = [requireProfile](const QString &id, const QString &name,
                                        QString *error) {
        const ExportProfile *profile = requireProfile(id, error);
        return profile && ExportProfileStore::renameUserProfile(*profile, name, error);
    };
    callbacks.duplicate = [requireProfile](const QString &id, const QString &name,
                                           QString *createdId, QString *error) {
        const ExportProfile *profile = requireProfile(id, error);
        return profile && ExportProfileStore::duplicateUserProfile(
            *profile, name, createdId, error);
    };
    callbacks.updateMetadata = [requireProfile](const QString &id,
        const QString &category, const QStringList &tags, QString *error) {
        const ExportProfile *profile = requireProfile(id, error);
        return profile && ExportProfileStore::updateMetadata(
            *profile, category, tags, error);
    };
    callbacks.setFavourite = [requireProfile](const QString &id,
        const bool favourite, QString *error) {
        const ExportProfile *profile = requireProfile(id, error);
        return profile && ExportProfileStore::setFavourite(
            *profile, favourite, error);
    };
    callbacks.remove = [requireProfile](const QString &id, QString *error) {
        const ExportProfile *profile = requireProfile(id, error);
        return profile && ExportProfileStore::removeUserProfile(*profile, error);
    };
    callbacks.importFile = [](const QString &source, QString *importedId,
                              QString *error) {
        return ExportProfileStore::importProfileFile(source, importedId, error);
    };
    callbacks.exportFile = [requireProfile](const QString &id,
        const QString &destination, QString *error) {
        const ExportProfile *profile = requireProfile(id, error);
        return profile && ExportProfileStore::exportProfileFile(
            *profile, destination, error);
    };

    PresetManagerDialog manager(
        tr("Export Profiles"),
        tr("Profiles define colour, encoding, Alpha and quality settings. Applying a profile updates the selected production output; its naming template and resize remain independently editable."),
        tr("Export Profile"), std::move(callbacks), this);
    manager.exec();
    const int row = selectedOutputIndex();
    const QString currentId = row >= 0 ? m_outputs.at(row).profileId : QString();
    reloadProfiles(currentId);
    // Existing outputs retain their captured profile name and payload. Library
    // edits only affect a configured output when the user explicitly applies
    // that profile again through the manager or the output combo.
    rebuildTable(row);
}

void ProductionExportDialog::accept()
{
    storeSelectedOutput();
    QVector<ResolvedProductionExportOutput> resolved;
    QStringList warnings;
    QString error;
    if (!resolveProductionExportPlan(plan(), m_colourState,
                                     &resolved, &warnings, &error)) {
        QMessageBox::warning(this, tr("Invalid Production Export"), error);
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("productionExport/outputDirectory"),
                      plan().outputDirectory);
    settings.setValue(QStringLiteral("productionExport/collisionPolicy"),
                      static_cast<int>(plan().collisionPolicy));
    QDialog::accept();
}

} // namespace vfx
