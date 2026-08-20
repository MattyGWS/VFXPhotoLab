#include "ImageExportDialog.h"

#include "ColourConversion.h"
#include "DisplayColourManagement.h"
#include "ExportNamingTemplate.h"
#include "OcioIntegration.h"
#include "PresetManagerDialog.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace vfx {
namespace {

QString settingsPrefix(const ImageExportCapabilities &capabilities)
{
    return QStringLiteral("export/%1/").arg(capabilities.suffix);
}

QString profileDisplayName(const ColourSpaceDescriptor &profile)
{
    const QString name = profile.displayName.trimmed();
    return name.isEmpty() ? QObject::tr("Untagged") : name;
}

QString profileDescription(const ExportProfileData &data)
{
    QString result = QStringLiteral("%1 · %2 · %3")
        .arg(data.formatSuffix.toUpper(),
             imageExportBitDepthName(data.bitDepth),
             data.convertToOutputProfile
                 ? profileDisplayName(data.output.profile)
                 : QObject::tr("Keep working RGB"));
    result += QStringLiteral(" · %1").arg(
        imageExportAlphaModeName(data.alphaMode));
    result += QStringLiteral(" · %1").arg(data.namingTemplate);
    return result;
}

QString suffixForCombo(const QComboBox *combo)
{
    return combo ? combo->currentData().toString() : QStringLiteral("png");
}

} // namespace

ImageExportDialog::ImageExportDialog(const QString &filePath,
                                     const QString &documentName,
                                     const QSize &documentSize,
                                     const DocumentColourState &colourState,
                                     const int sourceDepth,
                                     QWidget *parent)
    : QDialog(parent)
    , m_filePath(filePath)
    , m_documentName(documentName)
    , m_documentSize(documentSize)
    , m_colourState(colourState)
    , m_capabilities(imageExportCapabilitiesForPath(filePath))
    , m_matteColour(Qt::white)
    , m_namingTimestampUtc(QDateTime::currentDateTimeUtc())
{
    setWindowTitle(tr("Colour-Managed Export"));
    setModal(true);

    QSettings settings;
    const QString initialPrefix = settingsPrefix(m_capabilities);

    auto *outerLayout = new QVBoxLayout(this);
    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scrollArea);
    auto *layout = new QVBoxLayout(content);
    scrollArea->setWidget(content);
    outerLayout->addWidget(scrollArea, 1);

    auto *intro = new QLabel(
        tr("Quick export remains a single-image workflow. Choose a reusable export profile or adjust these settings directly; use File → Production Export for several independently resized outputs. Persistent queue execution arrives in the next 0.12.0 stage."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *presetGroup = new QGroupBox(tr("Export profile"), this);
    auto *presetLayout = new QHBoxLayout(presetGroup);
    m_exportProfileCombo = new QComboBox(presetGroup);
    m_manageProfiles = new QPushButton(tr("Manage…"), presetGroup);
    presetLayout->addWidget(m_exportProfileCombo, 1);
    presetLayout->addWidget(m_manageProfiles);
    layout->addWidget(presetGroup);

    auto *destinationGroup = new QGroupBox(tr("Destination"), this);
    auto *destinationForm = new QFormLayout(destinationGroup);
    m_formatCombo = new QComboBox(destinationGroup);
    const struct { const char *name; const char *suffix; } formats[] = {
        {"PNG", "png"}, {"JPEG", "jpg"}, {"TGA", "tga"},
        {"TIFF", "tiff"}, {"WebP", "webp"}, {"BMP", "bmp"}
    };
    for (const auto &format : formats) {
        m_formatCombo->addItem(QString::fromLatin1(format.name),
                               QString::fromLatin1(format.suffix));
    }
    int formatIndex = m_formatCombo->findData(m_capabilities.suffix);
    if (formatIndex < 0) formatIndex = 0;
    m_formatCombo->setCurrentIndex(formatIndex);
    destinationForm->addRow(tr("Format"), m_formatCombo);

    m_namingTemplate = new QLineEdit(destinationGroup);
    m_namingTemplate->setMaxLength(ExportNamingTemplate::MaximumTemplateLength);
    QString literalInitialName = QFileInfo(filePath).completeBaseName();
    literalInitialName.replace(QStringLiteral("{"), QStringLiteral("{{"));
    literalInitialName.replace(QStringLiteral("}"), QStringLiteral("}}"));
    m_namingTemplate->setText(literalInitialName);
    destinationForm->addRow(tr("Filename template"), m_namingTemplate);

    auto *tokenHelp = new QLabel(ExportNamingTemplate::tokenHelpText(),
                                 destinationGroup);
    tokenHelp->setWordWrap(true);
    tokenHelp->setProperty("class", "muted");
    destinationForm->addRow(QString(), tokenHelp);

    m_outputPathPreview = new QLabel(destinationGroup);
    m_outputPathPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_outputPathPreview->setWordWrap(true);
    destinationForm->addRow(tr("Output file"), m_outputPathPreview);

    m_formatValue = new QLabel(destinationGroup);
    m_formatValue->setWordWrap(true);
    destinationForm->addRow(tr("Encoding support"), m_formatValue);
    m_workingValue = new QLabel(profileDisplayName(colourState.workingSpace),
                                destinationGroup);
    m_workingValue->setWordWrap(true);
    destinationForm->addRow(tr("Document working space"), m_workingValue);
    layout->addWidget(destinationGroup);

    auto *colourGroup = new QGroupBox(tr("Output colour"), this);
    auto *colourForm = new QFormLayout(colourGroup);
    m_profileMode = new QComboBox(colourGroup);
    m_profileMode->addItem(tr("Convert to selected output profile"), true);
    m_profileMode->addItem(tr("Keep document working RGB values"), false);
    const bool storedConversion = settings.value(
        initialPrefix + QStringLiteral("convertToOutputProfile"), true).toBool();
    m_profileMode->setCurrentIndex(
        colourState.workingSpace.isUntagged() || !storedConversion ? 1 : 0);
    colourForm->addRow(tr("Conversion"), m_profileMode);

    m_profileCombo = new QComboBox(colourGroup);
    addOutputProfile(colourState.output.profile);
    addOutputProfile(colourState.workingSpace);
    for (const ColourSpaceDescriptor &profile : commonWorkingColourSpaces()) {
        addOutputProfile(profile);
    }
    if (colourState.ocioConfig.isConfigured()) {
        OcioConfigInspection inspection;
        QString error;
        if (resolveOcioConfiguration(colourState.ocioConfig, &inspection, &error)
            && inspection.available
            && inspection.fingerprintMatchesSavedReference) {
            for (const ColourSpaceDescriptor &profile
                 : ocioExportColourSpaces(inspection)) {
                addOutputProfile(profile);
            }
        }
    }
    const QByteArray savedOutputFingerprint =
        colourState.output.profile.stableFingerprint();
    int selectedProfile = 0;
    for (int index = 0; index < m_profiles.size(); ++index) {
        if (m_profiles.at(index).stableFingerprint()
            == savedOutputFingerprint) {
            selectedProfile = index;
            break;
        }
    }
    m_profileCombo->setCurrentIndex(selectedProfile);
    m_browseProfile = new QPushButton(tr("Choose ICC…"), colourGroup);
    auto *profileRow = new QWidget(colourGroup);
    auto *profileLayout = new QHBoxLayout(profileRow);
    profileLayout->setContentsMargins(0, 0, 0, 0);
    profileLayout->addWidget(m_profileCombo, 1);
    profileLayout->addWidget(m_browseProfile);
    colourForm->addRow(tr("Output profile"), profileRow);

    m_intentCombo = new QComboBox(colourGroup);
    const ColourRenderingIntent intents[] = {
        ColourRenderingIntent::Perceptual,
        ColourRenderingIntent::RelativeColorimetric,
        ColourRenderingIntent::Saturation,
        ColourRenderingIntent::AbsoluteColorimetric,
    };
    for (const ColourRenderingIntent intent : intents) {
        m_intentCombo->addItem(colourRenderingIntentName(intent),
                               static_cast<int>(intent));
    }
    const int intentIndex = m_intentCombo->findData(
        static_cast<int>(colourState.output.renderingIntent));
    if (intentIndex >= 0) m_intentCombo->setCurrentIndex(intentIndex);
    colourForm->addRow(tr("Rendering intent"), m_intentCombo);

    m_blackPointCompensation = new QCheckBox(
        tr("Black-point compensation where supported"), colourGroup);
    m_blackPointCompensation->setChecked(
        colourState.output.blackPointCompensation);
    colourForm->addRow(QString(), m_blackPointCompensation);

    m_embedProfile = new QCheckBox(tr("Embed ICC profile"), colourGroup);
    m_embedProfile->setChecked(colourState.output.embedProfile);
    colourForm->addRow(QString(), m_embedProfile);

    m_profileNote = new QLabel(colourGroup);
    m_profileNote->setWordWrap(true);
    m_profileNote->setProperty("class", "muted");
    colourForm->addRow(QString(), m_profileNote);
    layout->addWidget(colourGroup);

    auto *encodingGroup = new QGroupBox(tr("File encoding"), this);
    auto *encodingForm = new QFormLayout(encodingGroup);
    m_bitDepthCombo = new QComboBox(encodingGroup);
    const int defaultDepth = sourceDepth > 32 && m_capabilities.supportsSixteenBit
        ? 16 : 8;
    const int storedDepth = settings.value(
        initialPrefix + QStringLiteral("bitDepth"), defaultDepth).toInt();
    rebuildBitDepthChoices(storedDepth == 16
        ? ImageExportBitDepth::Sixteen : ImageExportBitDepth::Eight);
    encodingForm->addRow(tr("Bit depth"), m_bitDepthCombo);

    m_blueNoiseDither = new QCheckBox(
        tr("Blue-noise dither when reducing to 8-bit"), encodingGroup);
    m_blueNoiseDither->setChecked(settings.value(
        initialPrefix + QStringLiteral("blueNoiseDither"), true).toBool());
    encodingForm->addRow(QString(), m_blueNoiseDither);

    m_alphaModeCombo = new QComboBox(encodingGroup);
    m_alphaModeCombo->addItem(tr("Preserve Alpha when supported"),
                              static_cast<int>(ImageExportAlphaMode::PreserveWhenSupported));
    m_alphaModeCombo->addItem(tr("Flatten transparency to matte"),
                              static_cast<int>(ImageExportAlphaMode::FlattenToMatte));
    const int storedAlpha = settings.value(
        initialPrefix + QStringLiteral("alphaMode"),
        static_cast<int>(ImageExportAlphaMode::PreserveWhenSupported)).toInt();
    int alphaIndex = m_alphaModeCombo->findData(storedAlpha);
    if (alphaIndex < 0) alphaIndex = 0;
    m_alphaModeCombo->setCurrentIndex(alphaIndex);
    encodingForm->addRow(tr("Alpha"), m_alphaModeCombo);

    m_quality = new QSpinBox(encodingGroup);
    m_quality->setRange(0, 100);
    m_quality->setSuffix(tr(" %"));
    m_quality->setValue(settings.value(
        initialPrefix + QStringLiteral("quality"), 95).toInt());
    encodingForm->addRow(tr("Quality"), m_quality);

    m_matteColour = QColor(settings.value(
        initialPrefix + QStringLiteral("matteColour"),
        QColor(Qt::white).name(QColor::HexRgb)).toString());
    if (!m_matteColour.isValid()) m_matteColour = QColor(Qt::white);
    m_matteButton = new QPushButton(encodingGroup);
    updateMatteButton();
    encodingForm->addRow(tr("Transparency matte"), m_matteButton);

    m_alphaNote = new QLabel(encodingGroup);
    m_alphaNote->setWordWrap(true);
    m_alphaNote->setProperty("class", "muted");
    encodingForm->addRow(QString(), m_alphaNote);
    layout->addWidget(encodingGroup);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setProperty("class", "muted");
    layout->addWidget(m_summary);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));
    connect(buttons, &QDialogButtonBox::accepted,
            this, &ImageExportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &ImageExportDialog::reject);
    outerLayout->addWidget(buttons);

    connect(m_manageProfiles, &QPushButton::clicked,
            this, [this] { openProfileManager(); });
    connect(m_exportProfileCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](const int index) {
        if (m_applyingProfile) return;
        if (index <= 0) {
            m_selectedProfileId.clear();
            refreshControls();
            return;
        }
        QString error;
        const QString id = m_exportProfileCombo->itemData(index).toString();
        if (!applyExportProfile(id, &error)) {
            QMessageBox::warning(this, tr("Export Profile Not Applied"), error);
            reloadExportProfiles(m_selectedProfileId);
            refreshControls();
        }
    });
    connect(m_browseProfile, &QPushButton::clicked,
            this, [this] { chooseExternalIccProfile(); });
    connect(m_matteButton, &QPushButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(
            m_matteColour, this, tr("Choose Export Matte Colour"));
        if (!chosen.isValid()) return;
        m_matteColour = chosen;
        m_matteColour.setAlpha(255);
        updateMatteButton();
        markCustomSettings();
        refreshControls();
    });
    connect(m_formatCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] {
        updateCapabilitiesForFormat();
        markCustomSettings();
        refreshControls();
    });
    connect(m_namingTemplate, &QLineEdit::textChanged,
            this, [this] {
        markCustomSettings();
        refreshControls();
    });
    connect(m_profileMode, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { markCustomSettings(); refreshControls(); });
    connect(m_profileCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { markCustomSettings(); refreshControls(); });
    connect(m_intentCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { markCustomSettings(); refreshControls(); });
    connect(m_blackPointCompensation, &QCheckBox::toggled,
            this, [this] { markCustomSettings(); refreshControls(); });
    connect(m_bitDepthCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { markCustomSettings(); refreshControls(); });
    connect(m_blueNoiseDither, &QCheckBox::toggled,
            this, [this] { markCustomSettings(); refreshControls(); });
    connect(m_embedProfile, &QCheckBox::toggled,
            this, [this] { markCustomSettings(); refreshControls(); });
    connect(m_alphaModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { markCustomSettings(); refreshControls(); });
    connect(m_quality, qOverload<int>(&QSpinBox::valueChanged),
            this, [this] { markCustomSettings(); refreshControls(); });

    reloadExportProfiles();
    updateCapabilitiesForFormat(true);
    refreshControls();
    layout->activate();

    const QFontMetrics metrics(font());
    int minimumWidth = std::max(
        720, metrics.horizontalAdvance(QLatin1Char('M')) * 86);
    int minimumHeight = std::max(540, metrics.lineSpacing() * 31);
    QSize available;
    if (QScreen *screen = this->screen()) {
        available = screen->availableGeometry().size();
        minimumWidth = std::min(minimumWidth,
                                std::max(560, available.width() - 80));
        minimumHeight = std::min(minimumHeight,
                                 std::max(420, available.height() - 80));
    }
    setMinimumSize(minimumWidth, minimumHeight);
    QSize target(std::max(820, minimumWidth), std::max(700, minimumHeight));
    if (available.isValid()) {
        target.setWidth(std::min(target.width(), available.width() - 80));
        target.setHeight(std::min(target.height(), available.height() - 80));
    }
    resize(target.expandedTo(minimumSize()));
}

void ImageExportDialog::addOutputProfile(
    const ColourSpaceDescriptor &profile)
{
    if (!profile.isValid() || profile.isUntagged()) return;
    const QByteArray fingerprint = profile.stableFingerprint();
    const bool alreadyPresent = std::any_of(
        m_profiles.cbegin(), m_profiles.cend(),
        [&](const ColourSpaceDescriptor &existing) {
            return existing.stableFingerprint() == fingerprint;
        });
    if (alreadyPresent) return;
    m_profiles.push_back(profile);
    m_profileCombo->addItem(profileDisplayName(profile));
}

void ImageExportDialog::chooseExternalIccProfile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose Output ICC Profile"), QString(),
        tr("ICC Profiles (*.icc *.icm);;All Files (*)"));
    if (path.isEmpty()) return;
    ColourSpaceDescriptor descriptor;
    QString error;
    if (!loadExternalIccProfile(path, &descriptor, &error)) {
        QMessageBox::warning(this, tr("Unsupported ICC Profile"),
                             error.isEmpty()
                                 ? tr("The selected ICC profile could not be loaded.")
                                 : error);
        return;
    }
    addOutputProfile(descriptor);
    m_profileCombo->setCurrentIndex(m_profiles.size() - 1);
    refreshControls();
}

ColourSpaceDescriptor ImageExportDialog::selectedOutputProfile() const
{
    const int index = m_profileCombo->currentIndex();
    if (index < 0 || index >= m_profiles.size()) {
        return ColourSpaceDescriptor::untagged();
    }
    return m_profiles.at(index);
}

void ImageExportDialog::updateMatteButton()
{
    m_matteButton->setText(m_matteColour.name(QColor::HexRgb).toUpper());
    m_matteButton->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: %2; }")
            .arg(m_matteColour.name(QColor::HexRgb),
                 m_matteColour.lightnessF() > 0.5
                     ? QStringLiteral("#111111")
                     : QStringLiteral("#f5f5f5")));
}

void ImageExportDialog::rebuildBitDepthChoices(
    const ImageExportBitDepth preferredDepth)
{
    QSignalBlocker blocker(m_bitDepthCombo);
    m_bitDepthCombo->clear();
    if (m_capabilities.supportsEightBit) {
        m_bitDepthCombo->addItem(tr("8 bits per channel"),
                                 static_cast<int>(ImageExportBitDepth::Eight));
    }
    if (m_capabilities.supportsSixteenBit) {
        m_bitDepthCombo->addItem(tr("16 bits per channel"),
                                 static_cast<int>(ImageExportBitDepth::Sixteen));
    }
    int index = m_bitDepthCombo->findData(static_cast<int>(preferredDepth));
    if (index < 0) index = 0;
    m_bitDepthCombo->setCurrentIndex(index);
}

void ImageExportDialog::updateCapabilitiesForFormat(const bool preserveDepth)
{
    ImageExportBitDepth depth = ImageExportBitDepth::Eight;
    if (preserveDepth && m_bitDepthCombo && m_bitDepthCombo->currentIndex() >= 0) {
        depth = static_cast<ImageExportBitDepth>(
            m_bitDepthCombo->currentData().toInt());
    }
    const QString suffix = suffixForCombo(m_formatCombo);
    m_capabilities = imageExportCapabilitiesForPath(
        QStringLiteral("output.%1").arg(suffix));
    if (m_bitDepthCombo) rebuildBitDepthChoices(depth);
    if (m_alphaModeCombo && !m_capabilities.supportsAlpha) {
        QSignalBlocker blocker(m_alphaModeCombo);
        const int flattenIndex = m_alphaModeCombo->findData(
            static_cast<int>(ImageExportAlphaMode::FlattenToMatte));
        if (flattenIndex >= 0) m_alphaModeCombo->setCurrentIndex(flattenIndex);
    }
}

void ImageExportDialog::markCustomSettings()
{
    if (m_applyingProfile || !m_exportProfileCombo) return;
    m_selectedProfileId.clear();
    QSignalBlocker blocker(m_exportProfileCombo);
    m_exportProfileCombo->setCurrentIndex(0);
}

QString ImageExportDialog::currentProfileName() const
{
    const ExportProfile *profile = profileById(m_selectedProfileId);
    return profile ? profile->name : tr("Custom");
}

QString ImageExportDialog::resolvedFilePath(QString *errorMessage) const
{
    ExportNamingContext context;
    context.documentName = m_documentName;
    context.profileName = currentProfileName();
    context.formatSuffix = suffixForCombo(m_formatCombo);
    context.bitDepth = m_bitDepthCombo->currentData().toInt();
    context.imageSize = m_documentSize;
    context.workingSpaceName = profileDisplayName(m_colourState.workingSpace);
    const bool convert = m_profileMode->currentData().toBool();
    context.outputSpaceName = profileDisplayName(
        convert ? selectedOutputProfile() : m_colourState.workingSpace);
    context.timestampUtc = m_namingTimestampUtc;
    const QString stem = ExportNamingTemplate::resolve(
        m_namingTemplate->text(), context, errorMessage);
    if (stem.isEmpty()) return {};
    return QDir(QFileInfo(m_filePath).absolutePath()).filePath(
        stem + QLatin1Char('.') + context.formatSuffix);
}

void ImageExportDialog::refreshControls()
{
    const bool convert = m_profileMode->currentData().toBool();
    const ColourSpaceDescriptor outputProfile = convert
        ? selectedOutputProfile() : m_colourState.workingSpace;
    m_profileCombo->setEnabled(convert);
    m_browseProfile->setEnabled(convert);
    m_intentCombo->setEnabled(convert);
    m_blackPointCompensation->setEnabled(convert);

    const bool profileCanBeEmbedded = m_capabilities.supportsIccProfile
        && outputProfile.kind != ColourSpaceKind::Ocio
        && !outputProfile.isUntagged()
        && outputProfile.toQColorSpace().isValid();
    m_embedProfile->setEnabled(profileCanBeEmbedded);

    const ImageExportBitDepth depth = static_cast<ImageExportBitDepth>(
        m_bitDepthCombo->currentData().toInt());
    m_blueNoiseDither->setEnabled(depth == ImageExportBitDepth::Eight);
    m_quality->setEnabled(m_capabilities.supportsQuality);
    m_alphaModeCombo->setEnabled(m_capabilities.supportsAlpha);
    const ImageExportAlphaMode alphaMode = static_cast<ImageExportAlphaMode>(
        m_alphaModeCombo->currentData().toInt());
    const bool flatten = alphaMode == ImageExportAlphaMode::FlattenToMatte
        || !m_capabilities.supportsAlpha;
    m_matteButton->setEnabled(flatten);

    if (!m_capabilities.supportsAlpha) {
        m_alphaNote->setText(
            tr("%1 has no reliable Alpha channel. Transparency will be flattened against the selected matte after output-space conversion.")
                .arg(m_capabilities.displayName));
    } else if (flatten) {
        m_alphaNote->setText(
            tr("Transparency will be flattened explicitly. The encoded image is opaque even though %1 supports Alpha.")
                .arg(m_capabilities.displayName));
    } else {
        m_alphaNote->setText(
            tr("The document Alpha channel is preserved. Hidden RGB remains intact beneath fully transparent pixels."));
    }

    if (outputProfile.kind == ColourSpaceKind::Ocio) {
        m_profileNote->setText(
            tr("The pixels will be converted into the selected OCIO colour space. OCIO spaces do not contain an embeddable ICC payload, so the file is saved untagged."));
    } else if (!m_capabilities.supportsIccProfile) {
        m_profileNote->setText(
            tr("%1 does not carry ICC metadata in this export path. Colour conversion can still be applied, but the resulting file will be untagged.")
                .arg(m_capabilities.displayName));
    } else if (!convert) {
        m_profileNote->setText(
            tr("RGB values remain in the document working space. The working ICC profile is embedded when enabled and supported."));
    } else {
        m_profileNote->setText(
            tr("The full-resolution working-space composite is converted once into the selected output profile before encoding."));
    }

    QString pathError;
    const QString outputPath = resolvedFilePath(&pathError);
    if (outputPath.isEmpty()) {
        m_outputPathPreview->setText(pathError);
        m_outputPathPreview->setStyleSheet(QStringLiteral("color: #d86868;"));
    } else {
        m_outputPathPreview->setText(outputPath);
        m_outputPathPreview->setStyleSheet(QString());
    }
    m_formatValue->setText(
        tr("%1 · %2 · %3")
            .arg(m_capabilities.displayName,
                 m_capabilities.supportsSixteenBit
                     ? tr("8/16-bit") : tr("8-bit"),
                 m_capabilities.supportsAlpha
                     ? tr("Alpha capable") : tr("opaque")));

    QString summary = convert
        ? tr("%1 → %2")
              .arg(profileDisplayName(m_colourState.workingSpace),
                   profileDisplayName(outputProfile))
        : tr("Keep %1").arg(profileDisplayName(m_colourState.workingSpace));
    summary += tr(" · %1").arg(imageExportBitDepthName(depth));
    if (depth == ImageExportBitDepth::Eight) {
        summary += m_blueNoiseDither->isChecked()
            ? tr(" · blue-noise RGB dithering")
            : tr(" · nearest quantisation");
    }
    summary += flatten ? tr(" · flattened to matte")
                       : tr(" · Alpha preserved");
    m_summary->setText(summary);
}

ExportProfileData ImageExportDialog::currentProfileData() const
{
    ExportProfileData data;
    data.formatSuffix = suffixForCombo(m_formatCombo);
    data.bitDepth = static_cast<ImageExportBitDepth>(
        m_bitDepthCombo->currentData().toInt());
    data.dither = data.bitDepth == ImageExportBitDepth::Eight
                  && m_blueNoiseDither->isChecked()
        ? ImageExportDither::BlueNoise64 : ImageExportDither::None;
    data.alphaMode = static_cast<ImageExportAlphaMode>(
        m_alphaModeCombo->currentData().toInt());
    data.convertToOutputProfile = m_profileMode->currentData().toBool();
    data.output = m_colourState.output;
    data.output.profile = selectedOutputProfile();
    data.output.renderingIntent = static_cast<ColourRenderingIntent>(
        m_intentCombo->currentData().toInt());
    data.output.blackPointCompensation =
        m_blackPointCompensation->isChecked();
    data.output.embedProfile = m_embedProfile->isChecked();
    data.quality = m_quality->value();
    data.matteColour = m_matteColour;
    data.namingTemplate = m_namingTemplate->text();
    return data;
}

ImageExportRequest ImageExportDialog::request() const
{
    const ExportProfileData profile = currentProfileData();
    ImageExportRequest result;
    result.filePath = resolvedFilePath();
    result.quality = profile.quality;
    result.bitDepth = profile.bitDepth;
    result.dither = profile.dither;
    result.alphaMode = profile.alphaMode;
    result.convertToOutputProfile = profile.convertToOutputProfile;
    result.output = profile.output;
    result.matteColour = profile.matteColour;
    return result;
}

void ImageExportDialog::persistFormatSettings() const
{
    const ImageExportRequest selected = request();
    const QString prefix = settingsPrefix(
        imageExportCapabilitiesForPath(selected.filePath));
    QSettings settings;
    settings.setValue(prefix + QStringLiteral("convertToOutputProfile"),
                      selected.convertToOutputProfile);
    settings.setValue(prefix + QStringLiteral("bitDepth"),
                      static_cast<int>(selected.bitDepth));
    settings.setValue(prefix + QStringLiteral("blueNoiseDither"),
                      selected.dither == ImageExportDither::BlueNoise64);
    settings.setValue(prefix + QStringLiteral("alphaMode"),
                      static_cast<int>(selected.alphaMode));
    settings.setValue(prefix + QStringLiteral("quality"), selected.quality);
    settings.setValue(prefix + QStringLiteral("matteColour"),
                      selected.matteColour.name(QColor::HexRgb));
}

void ImageExportDialog::reloadExportProfiles(const QString &preferredId)
{
    QStringList warnings;
    m_exportProfiles = ExportProfileStore::profiles(&warnings);
    const QString wanted = preferredId.isEmpty()
        ? m_selectedProfileId : preferredId;
    QSignalBlocker blocker(m_exportProfileCombo);
    m_exportProfileCombo->clear();
    m_exportProfileCombo->addItem(tr("Current custom settings"), QString());
    for (const ExportProfile &profile : std::as_const(m_exportProfiles)) {
        QString label = profile.name;
        if (profile.metadata.favourite) label.prepend(QStringLiteral("★ "));
        if (profile.builtIn) label += tr("  [Built-in]");
        m_exportProfileCombo->addItem(label, profile.metadata.id);
    }
    int index = wanted.isEmpty() ? 0
        : m_exportProfileCombo->findData(wanted);
    if (index < 0) {
        index = 0;
        if (wanted == m_selectedProfileId) m_selectedProfileId.clear();
    }
    m_exportProfileCombo->setCurrentIndex(index);
}

const ExportProfile *ImageExportDialog::profileById(const QString &id) const
{
    const auto found = std::find_if(
        m_exportProfiles.cbegin(), m_exportProfiles.cend(),
        [&id](const ExportProfile &profile) {
            return profile.metadata.id == id;
        });
    return found == m_exportProfiles.cend() ? nullptr : &*found;
}

bool ImageExportDialog::applyExportProfile(const QString &id,
                                           QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    const ExportProfile *selected = profileById(id);
    if (!selected) {
        if (errorMessage) *errorMessage = tr("The selected export profile is no longer available.");
        return false;
    }
    QString validationError;
    if (!selected->data.isValid(&validationError)) {
        if (errorMessage) *errorMessage = validationError;
        return false;
    }
    if (selected->data.convertToOutputProfile
        && selected->data.output.profile.kind == ColourSpaceKind::Ocio
        && (selected->data.output.profile.ocioConfigId
                != m_colourState.ocioConfig.identifier
            || selected->data.output.profile.ocioConfigFingerprint
                != m_colourState.ocioConfig.fingerprint)) {
        if (errorMessage) {
            *errorMessage = tr("This profile references an OCIO space from a different configuration. Select a profile compatible with this document or update the profile from current settings.");
        }
        return false;
    }

    m_applyingProfile = true;
    int formatIndex = m_formatCombo->findData(selected->data.formatSuffix);
    if (formatIndex < 0) formatIndex = 0;
    m_formatCombo->setCurrentIndex(formatIndex);
    updateCapabilitiesForFormat(false);
    rebuildBitDepthChoices(selected->data.bitDepth);
    m_blueNoiseDither->setChecked(
        selected->data.dither == ImageExportDither::BlueNoise64);
    int alphaIndex = m_alphaModeCombo->findData(
        static_cast<int>(selected->data.alphaMode));
    if (alphaIndex < 0) alphaIndex = 0;
    m_alphaModeCombo->setCurrentIndex(alphaIndex);
    if (!m_capabilities.supportsAlpha) {
        const int flattenIndex = m_alphaModeCombo->findData(
            static_cast<int>(ImageExportAlphaMode::FlattenToMatte));
        if (flattenIndex >= 0) m_alphaModeCombo->setCurrentIndex(flattenIndex);
    }
    m_profileMode->setCurrentIndex(
        selected->data.convertToOutputProfile ? 0 : 1);
    const ColourSpaceDescriptor profileToSelect =
        selected->data.convertToOutputProfile
        ? selected->data.output.profile : m_colourState.output.profile;
    addOutputProfile(profileToSelect);
    const QByteArray fingerprint = profileToSelect.stableFingerprint();
    for (int index = 0; index < m_profiles.size(); ++index) {
        if (m_profiles.at(index).stableFingerprint() == fingerprint) {
            m_profileCombo->setCurrentIndex(index);
            break;
        }
    }
    int intentIndex = m_intentCombo->findData(
        static_cast<int>(selected->data.output.renderingIntent));
    if (intentIndex >= 0) m_intentCombo->setCurrentIndex(intentIndex);
    m_blackPointCompensation->setChecked(
        selected->data.output.blackPointCompensation);
    m_embedProfile->setChecked(selected->data.output.embedProfile);
    m_quality->setValue(selected->data.quality);
    m_matteColour = selected->data.matteColour;
    updateMatteButton();
    m_namingTemplate->setText(selected->data.namingTemplate);
    m_selectedProfileId = selected->metadata.id;
    int comboIndex = m_exportProfileCombo->findData(m_selectedProfileId);
    if (comboIndex >= 0) {
        QSignalBlocker blocker(m_exportProfileCombo);
        m_exportProfileCombo->setCurrentIndex(comboIndex);
    }
    m_applyingProfile = false;
    refreshControls();

    QString usageError;
    ExportProfileStore::recordUse(*selected, &usageError);
    reloadExportProfiles(m_selectedProfileId);
    return true;
}

void ImageExportDialog::openProfileManager()
{
    PresetManagerCallbacks callbacks;
    callbacks.load = [this](QStringList *warnings) {
        m_exportProfiles = ExportProfileStore::profiles(warnings);
        QVector<PresetManagerEntry> entries;
        entries.reserve(m_exportProfiles.size());
        for (const ExportProfile &profile : std::as_const(m_exportProfiles)) {
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
        reloadExportProfiles(m_selectedProfileId);
        return entries;
    };
    callbacks.apply = [this](const QString &id, QString *error) {
        return applyExportProfile(id, error);
    };
    callbacks.createFromCurrent = [this](
        const QString &name, const QString &category,
        const QStringList &tags, QString *createdId, QString *error) {
        const bool created = ExportProfileStore::createUserProfile(
            name, currentProfileData(), category, tags, createdId, error);
        if (created && createdId) m_selectedProfileId = *createdId;
        return created;
    };
    callbacks.updateFromCurrent = [this](const QString &id, QString *error) {
        const ExportProfile *profile = profileById(id);
        if (!profile) {
            if (error) *error = tr("The selected export profile is no longer available.");
            return false;
        }
        const bool updated = ExportProfileStore::updateUserProfile(
            *profile, currentProfileData(), error);
        if (updated) m_selectedProfileId = id;
        return updated;
    };
    callbacks.rename = [this](const QString &id, const QString &name,
                              QString *error) {
        const ExportProfile *profile = profileById(id);
        if (!profile) {
            if (error) *error = tr("The selected export profile is no longer available.");
            return false;
        }
        return ExportProfileStore::renameUserProfile(*profile, name, error);
    };
    callbacks.duplicate = [this](const QString &id, const QString &name,
                                 QString *createdId, QString *error) {
        const ExportProfile *profile = profileById(id);
        if (!profile) {
            if (error) *error = tr("The selected export profile is no longer available.");
            return false;
        }
        return ExportProfileStore::duplicateUserProfile(
            *profile, name, createdId, error);
    };
    callbacks.updateMetadata = [this](
        const QString &id, const QString &category,
        const QStringList &tags, QString *error) {
        const ExportProfile *profile = profileById(id);
        if (!profile) {
            if (error) *error = tr("The selected export profile is no longer available.");
            return false;
        }
        return ExportProfileStore::updateMetadata(
            *profile, category, tags, error);
    };
    callbacks.setFavourite = [this](const QString &id, const bool favourite,
                                    QString *error) {
        const ExportProfile *profile = profileById(id);
        if (!profile) {
            if (error) *error = tr("The selected export profile is no longer available.");
            return false;
        }
        return ExportProfileStore::setFavourite(
            *profile, favourite, error);
    };
    callbacks.remove = [this](const QString &id, QString *error) {
        const ExportProfile *profile = profileById(id);
        if (!profile) {
            if (error) *error = tr("The selected export profile is no longer available.");
            return false;
        }
        return ExportProfileStore::removeUserProfile(*profile, error);
    };
    callbacks.importFile = [](const QString &sourcePath, QString *importedId,
                              QString *error) {
        return ExportProfileStore::importProfileFile(
            sourcePath, importedId, error);
    };
    callbacks.exportFile = [this](const QString &id,
                                  const QString &destinationPath,
                                  QString *error) {
        const ExportProfile *profile = profileById(id);
        if (!profile) {
            if (error) *error = tr("The selected export profile is no longer available.");
            return false;
        }
        return ExportProfileStore::exportProfileFile(
            *profile, destinationPath, error);
    };

    PresetManagerDialog manager(
        tr("Export Profiles"),
        tr("Export profiles store one output format, encoded precision, colour conversion, Alpha handling, quality and a portable filename template. They can be reused by both quick export and File → Production Export; persistent queue execution remains a later stage."),
        tr("Export Profile"), std::move(callbacks), this);
    manager.exec();
    reloadExportProfiles(m_selectedProfileId);
    refreshControls();
}

void ImageExportDialog::accept()
{
    QString namingError;
    const QString outputPath = resolvedFilePath(&namingError);
    if (outputPath.isEmpty()) {
        QMessageBox::warning(this, tr("Invalid Filename Template"), namingError);
        return;
    }
    const ImageExportCapabilities capabilities =
        imageExportCapabilitiesForPath(outputPath);
    if (!imageExportWriterAvailable(capabilities)) {
        QMessageBox::warning(
            this, tr("Export Plugin Unavailable"),
            tr("The Qt image plugin required to write %1 is not available on this system.")
                .arg(capabilities.displayName));
        return;
    }
    const ExportProfileData profile = currentProfileData();
    QString profileError;
    if (!profile.isValid(&profileError)) {
        QMessageBox::warning(this, tr("Invalid Export Profile Settings"),
                             profileError);
        return;
    }
    const ImageExportRequest selected = request();
    QString error;
    if (!validateImageExportRequest(selected, m_colourState, &error)) {
        QMessageBox::warning(this, tr("Invalid Export Settings"),
                             error.isEmpty()
                                 ? tr("The export settings are not valid for this file format.")
                                 : error);
        return;
    }
    persistFormatSettings();
    QDialog::accept();
}

} // namespace vfx
