#include "PresetManagerDialog.h"

#include "PresetCore.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSize>
#include <QSplitter>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace vfx {
namespace {

constexpr int EntryIdRole = Qt::UserRole + 91;

class MetadataDialog final : public QDialog {
public:
    MetadataDialog(const QString &title,
                   const QString &name,
                   const QString &category,
                   const QStringList &tags,
                   const bool includeName,
                   QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(title);
        setModal(true);
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        if (includeName) {
            m_name = new QLineEdit(name, this);
            m_name->setMaxLength(PresetStore::MaximumNameLength);
            form->addRow(tr("Name:"), m_name);
        }
        m_category = new QLineEdit(category, this);
        m_category->setMaxLength(PresetStore::MaximumCategoryLength);
        form->addRow(tr("Category:"), m_category);
        m_tags = new QLineEdit(tags.join(QStringLiteral(", ")), this);
        m_tags->setPlaceholderText(tr("Comma-separated tags"));
        form->addRow(tr("Tags:"), m_tags);
        layout->addLayout(form);

        auto *hint = new QLabel(
            tr("Categories and tags are searchable. Use at most %1 tags.")
                .arg(PresetStore::MaximumTagCount), this);
        hint->setObjectName(QStringLiteral("MutedLabel"));
        hint->setWordWrap(true);
        layout->addWidget(hint);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (m_name && m_name->text().trimmed().isEmpty()) {
                QMessageBox::information(this, tr("Preset Name Required"),
                                         tr("Enter a preset name."));
                return;
            }
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected,
                this, &QDialog::reject);
        layout->addWidget(buttons);

        const QFontMetrics metrics(font());
        setMinimumWidth(std::max(420, metrics.horizontalAdvance(QLatin1Char('M')) * 52));
    }

    QString name() const { return m_name ? m_name->text().trimmed() : QString(); }
    QString category() const { return m_category->text().trimmed(); }
    QString tags() const { return m_tags->text(); }

private:
    QLineEdit *m_name = nullptr;
    QLineEdit *m_category = nullptr;
    QLineEdit *m_tags = nullptr;
};

bool entryOrder(const PresetManagerEntry &left,
                const PresetManagerEntry &right)
{
    if (left.favourite != right.favourite) return left.favourite;
    if (left.lastUsedUtcMs != right.lastUsedUtcMs) {
        return left.lastUsedUtcMs > right.lastUsedUtcMs;
    }
    return QString::localeAwareCompare(left.name, right.name) < 0;
}

QString askPresetName(QWidget *parent,
                      const QString &title,
                      const QString &labelText,
                      const QString &initialValue,
                      bool *accepted)
{
    if (accepted) *accepted = false;
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setModal(true);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(labelText, &dialog);
    layout->addWidget(label);
    auto *edit = new QLineEdit(initialValue, &dialog);
    edit->setMaxLength(PresetStore::MaximumNameLength);
    edit->selectAll();
    layout->addWidget(edit);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted,
                     &dialog, [&dialog, edit] {
        if (edit->text().trimmed().isEmpty()) {
            QMessageBox::information(
                &dialog, QObject::tr("Preset Name Required"),
                QObject::tr("Enter a preset name."));
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const QFontMetrics metrics(dialog.font());
    dialog.setMinimumWidth(std::max(
        420, metrics.horizontalAdvance(QLatin1Char('M')) * 52));
    if (dialog.exec() != QDialog::Accepted) return {};
    if (accepted) *accepted = true;
    return edit->text().trimmed();
}

} // namespace

PresetManagerDialog::PresetManagerDialog(
    const QString &title,
    const QString &introText,
    const QString &presetTypeName,
    PresetManagerCallbacks callbacks,
    QWidget *parent)
    : QDialog(parent)
    , m_presetTypeName(presetTypeName)
    , m_callbacks(std::move(callbacks))
{
    setWindowTitle(title);
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(introText, this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *filters = new QHBoxLayout;
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setPlaceholderText(tr("Search name, category or tags"));
    m_searchEdit->setAccessibleName(tr("Search presets"));
    filters->addWidget(m_searchEdit, 2);

    m_sourceFilter = new QComboBox(this);
    m_sourceFilter->addItem(tr("All Sources"), 0);
    m_sourceFilter->addItem(tr("Built-in"), 1);
    m_sourceFilter->addItem(tr("User"), 2);
    filters->addWidget(m_sourceFilter);

    m_categoryFilter = new QComboBox(this);
    filters->addWidget(m_categoryFilter);

    m_viewFilter = new QComboBox(this);
    m_viewFilter->addItem(tr("All Presets"), static_cast<int>(ViewFilter::All));
    m_viewFilter->addItem(tr("Favourites"), static_cast<int>(ViewFilter::Favourites));
    m_viewFilter->addItem(tr("Recently Used"), static_cast<int>(ViewFilter::Recent));
    filters->addWidget(m_viewFilter);
    layout->addLayout(filters);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    m_tree = new QTreeWidget(splitter);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("★"), tr("Name"), tr("Category"),
                             tr("Tags"), tr("Last Used")});
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setAlternatingRowColors(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_tree->header()->resizeSection(1, 240);

    m_details = new QTextBrowser(splitter);
    m_details->setOpenExternalLinks(false);
    m_details->setMinimumWidth(280);
    splitter->addWidget(m_tree);
    splitter->addWidget(m_details);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    m_warningsLabel = new QLabel(this);
    m_warningsLabel->setObjectName(QStringLiteral("WarningLabel"));
    m_warningsLabel->setWordWrap(true);
    m_warningsLabel->hide();
    layout->addWidget(m_warningsLabel);

    auto *primaryActions = new QHBoxLayout;
    m_applyButton = new QPushButton(tr("Apply"), this);
    m_createButton = new QPushButton(tr("Save Current…"), this);
    m_updateButton = new QPushButton(tr("Update from Current"), this);
    m_renameButton = new QPushButton(tr("Rename…"), this);
    m_duplicateButton = new QPushButton(tr("Duplicate…"), this);
    primaryActions->addWidget(m_applyButton);
    primaryActions->addWidget(m_createButton);
    primaryActions->addWidget(m_updateButton);
    primaryActions->addWidget(m_renameButton);
    primaryActions->addWidget(m_duplicateButton);
    primaryActions->addStretch(1);
    layout->addLayout(primaryActions);

    auto *secondaryActions = new QHBoxLayout;
    m_editDetailsButton = new QPushButton(tr("Edit Details…"), this);
    m_favouriteButton = new QToolButton(this);
    m_favouriteButton->setText(tr("Favourite"));
    m_favouriteButton->setCheckable(true);
    m_favouriteButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_deleteButton = new QPushButton(tr("Delete"), this);
    m_importButton = new QPushButton(tr("Import…"), this);
    m_exportButton = new QPushButton(tr("Export…"), this);
    secondaryActions->addWidget(m_editDetailsButton);
    secondaryActions->addWidget(m_favouriteButton);
    secondaryActions->addWidget(m_deleteButton);
    secondaryActions->addStretch(1);
    secondaryActions->addWidget(m_importButton);
    secondaryActions->addWidget(m_exportButton);
    layout->addLayout(secondaryActions);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttonBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    layout->addWidget(buttonBox);

    connect(m_searchEdit, &QLineEdit::textChanged,
            this, [this] { rebuildTree(selectedId()); });
    connect(m_sourceFilter, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { rebuildTree(selectedId()); });
    connect(m_categoryFilter, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { rebuildTree(selectedId()); });
    connect(m_viewFilter, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { rebuildTree(selectedId()); });
    connect(m_tree, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem *, QTreeWidgetItem *) {
                updateSelectionUi();
            });
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *item, int) {
                if (item && !item->data(0, EntryIdRole).toString().isEmpty()) {
                    m_applyButton->click();
                }
            });

    connect(m_applyButton, &QPushButton::clicked, this, [this] {
        const PresetManagerEntry *entry = selectedEntry();
        if (!entry || !m_callbacks.apply) return;
        const QString id = entry->id;
        QString error;
        if (!m_callbacks.apply(id, &error)) {
            showOperationError(tr("Preset Not Applied"), error);
            return;
        }
        reload(id);
    });

    connect(m_createButton, &QPushButton::clicked, this, [this] {
        if (!m_callbacks.createFromCurrent) return;
        MetadataDialog dialog(tr("Save %1 Preset").arg(m_presetTypeName),
                              {}, suggestedCategory(), {}, true, this);
        if (dialog.exec() != QDialog::Accepted) return;
        const QStringList tags = parseTags(dialog.tags());
        const QString validationError = tagValidationError(tags);
        if (!validationError.isEmpty()) {
            showOperationError(tr("Preset Not Saved"), validationError);
            return;
        }
        QString createdId;
        QString error;
        if (!m_callbacks.createFromCurrent(dialog.name(), dialog.category(),
                                           tags, &createdId, &error)) {
            showOperationError(tr("Preset Not Saved"), error);
            return;
        }
        reload(createdId);
    });

    connect(m_updateButton, &QPushButton::clicked, this, [this] {
        const PresetManagerEntry *entry = selectedEntry();
        if (!entry || entry->builtIn || !m_callbacks.updateFromCurrent) return;
        const QString id = entry->id;
        if (QMessageBox::question(
                this, tr("Update Preset"),
                tr("Replace the stored settings in “%1” with the current settings?")
                    .arg(entry->name),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes) {
            return;
        }
        QString error;
        if (!m_callbacks.updateFromCurrent(id, &error)) {
            showOperationError(tr("Preset Not Updated"), error);
            return;
        }
        reload(id);
    });

    connect(m_renameButton, &QPushButton::clicked, this, [this] {
        const PresetManagerEntry *entry = selectedEntry();
        if (!entry || entry->builtIn || !m_callbacks.rename) return;
        bool accepted = false;
        const QString name = askPresetName(
            this, tr("Rename Preset"), tr("Preset name:"),
            entry->name, &accepted);
        if (!accepted || name == entry->name) return;
        const QString id = entry->id;
        QString error;
        if (!m_callbacks.rename(id, name, &error)) {
            showOperationError(tr("Preset Not Renamed"), error);
            return;
        }
        reload(id);
    });

    connect(m_duplicateButton, &QPushButton::clicked, this, [this] {
        const PresetManagerEntry *entry = selectedEntry();
        if (!entry || !m_callbacks.duplicate) return;
        bool accepted = false;
        const QString suggested = uniqueCopyName(entry->name, m_entries);
        const QString name = askPresetName(
            this, tr("Duplicate Preset"), tr("New preset name:"),
            suggested, &accepted);
        if (!accepted) return;
        QString createdId;
        QString error;
        if (!m_callbacks.duplicate(entry->id, name, &createdId, &error)) {
            showOperationError(tr("Preset Not Duplicated"), error);
            return;
        }
        reload(createdId);
    });

    connect(m_editDetailsButton, &QPushButton::clicked, this, [this] {
        const PresetManagerEntry *entry = selectedEntry();
        if (!entry || entry->builtIn || !m_callbacks.updateMetadata) return;
        MetadataDialog dialog(tr("Edit Preset Details"), {}, entry->category,
                              entry->tags, false, this);
        if (dialog.exec() != QDialog::Accepted) return;
        const QStringList tags = parseTags(dialog.tags());
        const QString validationError = tagValidationError(tags);
        if (!validationError.isEmpty()) {
            showOperationError(tr("Preset Details Not Updated"),
                               validationError);
            return;
        }
        const QString id = entry->id;
        QString error;
        if (!m_callbacks.updateMetadata(id, dialog.category(), tags, &error)) {
            showOperationError(tr("Preset Details Not Updated"), error);
            return;
        }
        reload(id);
    });

    connect(m_favouriteButton, &QToolButton::clicked,
            this, [this](const bool checked) {
                const PresetManagerEntry *entry = selectedEntry();
                if (!entry || !m_callbacks.setFavourite) return;
                const QString id = entry->id;
                QString error;
                if (!m_callbacks.setFavourite(id, checked, &error)) {
                    showOperationError(tr("Favourite Not Updated"), error);
                    updateSelectionUi();
                    return;
                }
                reload(id);
            });

    connect(m_deleteButton, &QPushButton::clicked, this, [this] {
        const PresetManagerEntry *entry = selectedEntry();
        if (!entry || entry->builtIn || !m_callbacks.remove) return;
        const QString name = entry->name;
        if (QMessageBox::question(
                this, tr("Delete Preset"),
                tr("Delete the user preset “%1”? This cannot be undone.")
                    .arg(name),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes) {
            return;
        }
        QString error;
        if (!m_callbacks.remove(entry->id, &error)) {
            showOperationError(tr("Preset Not Deleted"), error);
            return;
        }
        reload();
    });

    connect(m_importButton, &QPushButton::clicked, this, [this] {
        if (!m_callbacks.importFile) return;
        QSettings settings;
        const QString initialDirectory = settings.value(
            QStringLiteral("PresetManager/lastDirectory"),
            QDir::homePath()).toString();
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Import %1 Preset").arg(m_presetTypeName),
            initialDirectory, presetFileFilter());
        if (path.isEmpty()) return;
        settings.setValue(QStringLiteral("PresetManager/lastDirectory"),
                          QFileInfo(path).absolutePath());
        QString importedId;
        QString error;
        if (!m_callbacks.importFile(path, &importedId, &error)) {
            showOperationError(tr("Preset Not Imported"), error);
            return;
        }
        reload(importedId);
    });

    connect(m_exportButton, &QPushButton::clicked, this, [this] {
        const PresetManagerEntry *entry = selectedEntry();
        if (!entry || !m_callbacks.exportFile) return;
        QSettings settings;
        const QString initialDirectory = settings.value(
            QStringLiteral("PresetManager/lastDirectory"),
            QDir::homePath()).toString();
        QString safeName = entry->name;
        safeName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
                         QStringLiteral("-"));
        safeName = safeName.trimmed();
        if (safeName.isEmpty()) safeName = QStringLiteral("preset");
        QString path = QFileDialog::getSaveFileName(
            this, tr("Export %1 Preset").arg(m_presetTypeName),
            QDir(initialDirectory).filePath(
                safeName + QStringLiteral(".vfxpreset.json")),
            presetFileFilter());
        if (path.isEmpty()) return;
        if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
            path += QStringLiteral(".vfxpreset.json");
        }
        settings.setValue(QStringLiteral("PresetManager/lastDirectory"),
                          QFileInfo(path).absolutePath());
        QString error;
        if (!m_callbacks.exportFile(entry->id, path, &error)) {
            showOperationError(tr("Preset Not Exported"), error);
        }
    });

    const QFontMetrics metrics(font());
    int minWidth = std::max(
        760, metrics.horizontalAdvance(QLatin1Char('M')) * 92);
    int minHeight = std::max(520, metrics.lineSpacing() * 31);
    QSize available;
    if (QScreen *screen = this->screen()) {
        available = screen->availableGeometry().size();
        minWidth = std::min(minWidth, std::max(640, available.width() - 80));
        minHeight = std::min(minHeight, std::max(420, available.height() - 80));
    }
    setMinimumSize(minWidth, minHeight);
    QSize target(std::max(940, minWidth), std::max(620, minHeight));
    if (available.isValid()) {
        target.setWidth(std::min(target.width(), available.width() - 80));
        target.setHeight(std::min(target.height(), available.height() - 80));
    }
    resize(target.expandedTo(minimumSize()));

    reload();
}

void PresetManagerDialog::reload(const QString &preferredId)
{
    m_warnings.clear();
    m_entries = m_callbacks.load ? m_callbacks.load(&m_warnings)
                                 : QVector<PresetManagerEntry>();
    rebuildCategoryFilter();
    rebuildTree(preferredId);
    m_warningsLabel->setVisible(!m_warnings.isEmpty());
    QStringList visibleWarnings = m_warnings.mid(0, 8);
    if (m_warnings.size() > visibleWarnings.size()) {
        visibleWarnings.push_back(
            tr("…and %1 more preset warnings.")
                .arg(m_warnings.size() - visibleWarnings.size()));
    }
    m_warningsLabel->setText(visibleWarnings.join(QLatin1Char('\n')));
}

const PresetManagerEntry *PresetManagerDialog::selectedEntry() const
{
    const QString id = selectedId();
    if (id.isEmpty()) return nullptr;
    const auto found = std::find_if(m_entries.cbegin(), m_entries.cend(),
        [&id](const PresetManagerEntry &entry) { return entry.id == id; });
    return found == m_entries.cend() ? nullptr : &*found;
}

QString PresetManagerDialog::selectedId() const
{
    QTreeWidgetItem *item = m_tree ? m_tree->currentItem() : nullptr;
    return item ? item->data(0, EntryIdRole).toString() : QString();
}

void PresetManagerDialog::rebuildCategoryFilter()
{
    const QString previous = m_categoryFilter->currentData().toString();
    QStringList categories;
    for (const PresetManagerEntry &entry : std::as_const(m_entries)) {
        const QString category = entry.category.trimmed();
        if (!category.isEmpty()) categories.push_back(category);
    }
    categories.removeDuplicates();
    std::sort(categories.begin(), categories.end(),
              [](const QString &left, const QString &right) {
                  return QString::localeAwareCompare(left, right) < 0;
              });

    const QSignalBlocker blocker(m_categoryFilter);
    m_categoryFilter->clear();
    m_categoryFilter->addItem(tr("All Categories"), QString());
    for (const QString &category : std::as_const(categories)) {
        m_categoryFilter->addItem(category, category);
    }
    const int previousIndex = m_categoryFilter->findData(previous);
    m_categoryFilter->setCurrentIndex(previousIndex >= 0 ? previousIndex : 0);
}

void PresetManagerDialog::rebuildTree(const QString &preferredId)
{
    const QString fallbackId = preferredId.isEmpty() ? selectedId() : preferredId;
    m_tree->clear();

    QVector<PresetManagerEntry> builtIns;
    QVector<PresetManagerEntry> users;
    for (const PresetManagerEntry &entry : std::as_const(m_entries)) {
        if (!entryMatchesFilters(entry)) continue;
        (entry.builtIn ? builtIns : users).push_back(entry);
    }
    std::sort(builtIns.begin(), builtIns.end(), entryOrder);
    std::sort(users.begin(), users.end(), entryOrder);

    QTreeWidgetItem *preferredItem = nullptr;
    const auto addGroup = [&](const QString &title,
                              const QVector<PresetManagerEntry> &entries) {
        if (entries.isEmpty()) return;
        auto *group = new QTreeWidgetItem(m_tree);
        group->setText(0, title);
        group->setFirstColumnSpanned(true);
        group->setFlags(Qt::ItemIsEnabled);
        QFont groupFont = group->font(0);
        groupFont.setBold(true);
        group->setFont(0, groupFont);
        for (const PresetManagerEntry &entry : entries) {
            auto *item = new QTreeWidgetItem(group);
            item->setData(0, EntryIdRole, entry.id);
            item->setText(0, entry.favourite ? QStringLiteral("★") : QString());
            item->setTextAlignment(0, Qt::AlignCenter);
            item->setText(1, entry.name);
            item->setText(2, entry.category);
            item->setText(3, entry.tags.join(QStringLiteral(", ")));
            item->setText(4, displayDate(entry.lastUsedUtcMs));
            item->setToolTip(1, entry.storagePath.isEmpty()
                                    ? entry.id : entry.storagePath);
            item->setToolTip(3, entry.tags.join(QStringLiteral(", ")));
            if (entry.id == fallbackId) preferredItem = item;
        }
        group->setExpanded(true);
    };

    addGroup(tr("Built-in Presets (%1)").arg(builtIns.size()), builtIns);
    addGroup(tr("User Presets (%1)").arg(users.size()), users);

    if (preferredItem) {
        m_tree->setCurrentItem(preferredItem);
    } else {
        for (int groupIndex = 0; groupIndex < m_tree->topLevelItemCount(); ++groupIndex) {
            QTreeWidgetItem *group = m_tree->topLevelItem(groupIndex);
            if (group && group->childCount() > 0) {
                m_tree->setCurrentItem(group->child(0));
                break;
            }
        }
    }
    updateSelectionUi();
}

void PresetManagerDialog::updateSelectionUi()
{
    const PresetManagerEntry *entry = selectedEntry();
    const bool selected = entry != nullptr;
    const bool user = selected && !entry->builtIn;
    m_applyButton->setEnabled(selected && static_cast<bool>(m_callbacks.apply));
    m_createButton->setEnabled(static_cast<bool>(m_callbacks.createFromCurrent));
    m_updateButton->setEnabled(user && static_cast<bool>(m_callbacks.updateFromCurrent));
    m_renameButton->setEnabled(user && static_cast<bool>(m_callbacks.rename));
    m_duplicateButton->setEnabled(selected && static_cast<bool>(m_callbacks.duplicate));
    m_editDetailsButton->setEnabled(user && static_cast<bool>(m_callbacks.updateMetadata));
    m_favouriteButton->setEnabled(selected && static_cast<bool>(m_callbacks.setFavourite));
    m_deleteButton->setEnabled(user && static_cast<bool>(m_callbacks.remove));
    m_importButton->setEnabled(static_cast<bool>(m_callbacks.importFile));
    m_exportButton->setEnabled(selected && static_cast<bool>(m_callbacks.exportFile));
    {
        const QSignalBlocker blocker(m_favouriteButton);
        m_favouriteButton->setChecked(selected && entry->favourite);
    }

    if (!selected) {
        const bool filtered = !m_searchEdit->text().trimmed().isEmpty()
            || m_sourceFilter->currentData().toInt() != 0
            || !m_categoryFilter->currentData().toString().isEmpty()
            || m_viewFilter->currentData().toInt()
                != static_cast<int>(ViewFilter::All);
        m_details->setHtml(filtered
            ? tr("<p>No presets match the current filters.</p>")
            : tr("<p>No presets are available.</p>"));
        return;
    }

    const QString source = entry->builtIn ? tr("Built-in") : tr("User");
    const QString category = entry->category.trimmed().isEmpty()
        ? tr("Uncategorised") : entry->category;
    const QString tags = entry->tags.isEmpty()
        ? tr("None") : entry->tags.join(QStringLiteral(", "));
    const QString lastUsed = entry->lastUsedUtcMs > 0
        ? displayDate(entry->lastUsedUtcMs) : tr("Never");
    QString details = QStringLiteral("<h3>") + html(entry->name)
        + (entry->favourite ? QStringLiteral(" ★") : QString())
        + QStringLiteral("</h3><p><b>") + html(tr("Source"))
        + QStringLiteral(":</b> ") + html(source)
        + QStringLiteral("<br><b>") + html(tr("Category"))
        + QStringLiteral(":</b> ") + html(category)
        + QStringLiteral("<br><b>") + html(tr("Tags"))
        + QStringLiteral(":</b> ") + html(tags)
        + QStringLiteral("<br><b>") + html(tr("Last used"))
        + QStringLiteral(":</b> ") + html(lastUsed)
        + QStringLiteral(" &nbsp; <b>") + html(tr("Uses"))
        + QStringLiteral(":</b> ") + QString::number(entry->useCount);
    if (entry->createdUtcMs > 0) {
        details += QStringLiteral("<br><b>") + html(tr("Created"))
            + QStringLiteral(":</b> ")
            + html(displayDate(entry->createdUtcMs));
    }
    if (entry->modifiedUtcMs > 0) {
        details += QStringLiteral(" &nbsp; <b>") + html(tr("Modified"))
            + QStringLiteral(":</b> ")
            + html(displayDate(entry->modifiedUtcMs));
    }
    details += QStringLiteral("</p>");
    if (!entry->description.trimmed().isEmpty()) {
        details += QStringLiteral("<hr><p style=\"white-space:pre-wrap\">%1</p>")
                       .arg(html(entry->description));
    }
    if (!entry->storagePath.isEmpty()) {
        details += QStringLiteral("<hr><p><b>%1:</b><br><span style=\"font-family:monospace\">%2</span></p>")
                       .arg(tr("Stored at"), html(entry->storagePath));
    } else {
        details += QStringLiteral("<hr><p>%1</p>")
                       .arg(tr("This preset is built into VFX Photo Lab and cannot be renamed, updated or deleted."));
    }
    m_details->setHtml(details);
}

bool PresetManagerDialog::entryMatchesFilters(
    const PresetManagerEntry &entry) const
{
    const int source = m_sourceFilter->currentData().toInt();
    if ((source == 1 && !entry.builtIn) || (source == 2 && entry.builtIn)) {
        return false;
    }
    const QString category = m_categoryFilter->currentData().toString();
    if (!category.isEmpty()
        && entry.category.compare(category, Qt::CaseInsensitive) != 0) {
        return false;
    }
    const ViewFilter view = static_cast<ViewFilter>(
        m_viewFilter->currentData().toInt());
    if (view == ViewFilter::Favourites && !entry.favourite) return false;
    if (view == ViewFilter::Recent && entry.lastUsedUtcMs <= 0) return false;

    const QString query = m_searchEdit->text().trimmed().toCaseFolded();
    if (query.isEmpty()) return true;
    if (entry.name.toCaseFolded().contains(query)
        || entry.category.toCaseFolded().contains(query)) {
        return true;
    }
    for (const QString &tag : entry.tags) {
        if (tag.toCaseFolded().contains(query)) return true;
    }
    return false;
}

void PresetManagerDialog::showOperationError(const QString &title,
                                             const QString &error)
{
    QMessageBox::warning(this, title,
                         error.trimmed().isEmpty()
                             ? tr("The preset operation could not be completed.")
                             : error);
}

QString PresetManagerDialog::suggestedCategory() const
{
    const QString selected = m_categoryFilter->currentData().toString();
    return selected;
}

QString PresetManagerDialog::presetFileFilter() const
{
    return tr("VFX Photo Lab Presets (*.vfxpreset.json *.json);;JSON Files (*.json);;All Files (*)");
}

QStringList PresetManagerDialog::parseTags(const QString &text)
{
    QStringList tags;
    const QStringList pieces = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString tag : pieces) {
        tag = tag.trimmed();
        if (!tag.isEmpty()) tags.push_back(tag);
    }
    tags.removeDuplicates();
    return tags;
}

QString PresetManagerDialog::tagValidationError(const QStringList &tags)
{
    if (tags.size() > PresetStore::MaximumTagCount) {
        return QObject::tr("A preset can contain at most %1 tags.")
            .arg(PresetStore::MaximumTagCount);
    }
    const auto oversized = std::find_if(tags.cbegin(), tags.cend(),
        [](const QString &tag) {
            return tag.size() > PresetStore::MaximumTagLength;
        });
    if (oversized != tags.cend()) {
        return QObject::tr("The tag ‘%1’ is longer than the %2-character limit.")
            .arg(*oversized)
            .arg(PresetStore::MaximumTagLength);
    }
    return {};
}

QString PresetManagerDialog::uniqueCopyName(
    const QString &name,
    const QVector<PresetManagerEntry> &entries)
{
    const auto exists = [&entries](const QString &candidate) {
        return std::any_of(entries.cbegin(), entries.cend(),
            [&candidate](const PresetManagerEntry &entry) {
                return entry.name.compare(candidate, Qt::CaseInsensitive) == 0;
            });
    };
    for (int suffix = 1; suffix < 10000; ++suffix) {
        const QString suffixText = suffix == 1
            ? QObject::tr(" Copy") : QObject::tr(" Copy %1").arg(suffix);
        const qsizetype baseLength = std::max<qsizetype>(
            1, PresetStore::MaximumNameLength - suffixText.size());
        const QString candidate = name.left(baseLength).trimmed() + suffixText;
        if (!exists(candidate)) return candidate;
    }
    return name.left(PresetStore::MaximumNameLength - 9)
        + QStringLiteral(" Copy New");
}

QString PresetManagerDialog::displayDate(const qint64 utcMs)
{
    if (utcMs <= 0) return QStringLiteral("—");
    return QLocale().toString(
        QDateTime::fromMSecsSinceEpoch(utcMs).toLocalTime(),
        QLocale::ShortFormat);
}

QString PresetManagerDialog::html(const QString &text)
{
    return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
}

} // namespace vfx
