#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <functional>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace vfx {

struct PresetManagerEntry {
    QString id;
    QString name;
    QString category;
    QStringList tags;
    bool favourite = false;
    bool builtIn = false;
    qint64 createdUtcMs = 0;
    qint64 modifiedUtcMs = 0;
    qint64 lastUsedUtcMs = 0;
    quint64 useCount = 0;
    QString storagePath;
    QString description;
};

struct PresetManagerCallbacks {
    std::function<QVector<PresetManagerEntry>(QStringList *warnings)> load;
    std::function<bool(const QString &id, QString *error)> apply;
    std::function<bool(const QString &name,
                       const QString &category,
                       const QStringList &tags,
                       QString *createdId,
                       QString *error)> createFromCurrent;
    std::function<bool(const QString &id, QString *error)> updateFromCurrent;
    std::function<bool(const QString &id,
                       const QString &newName,
                       QString *error)> rename;
    std::function<bool(const QString &id,
                       const QString &newName,
                       QString *createdId,
                       QString *error)> duplicate;
    std::function<bool(const QString &id,
                       const QString &category,
                       const QStringList &tags,
                       QString *error)> updateMetadata;
    std::function<bool(const QString &id,
                       bool favourite,
                       QString *error)> setFavourite;
    std::function<bool(const QString &id, QString *error)> remove;
    std::function<bool(const QString &sourcePath,
                       QString *importedId,
                       QString *error)> importFile;
    std::function<bool(const QString &id,
                       const QString &destinationPath,
                       QString *error)> exportFile;
};

class PresetManagerDialog final : public QDialog {
public:
    PresetManagerDialog(const QString &title,
                        const QString &introText,
                        const QString &presetTypeName,
                        PresetManagerCallbacks callbacks,
                        QWidget *parent = nullptr);

    void reload(const QString &preferredId = {});

private:
    enum class ViewFilter {
        All,
        Favourites,
        Recent
    };

    const PresetManagerEntry *selectedEntry() const;
    QString selectedId() const;
    void rebuildCategoryFilter();
    void rebuildTree(const QString &preferredId = {});
    void updateSelectionUi();
    bool entryMatchesFilters(const PresetManagerEntry &entry) const;
    void showOperationError(const QString &title, const QString &error);
    QString suggestedCategory() const;
    QString presetFileFilter() const;
    static QStringList parseTags(const QString &text);
    static QString tagValidationError(const QStringList &tags);
    static QString uniqueCopyName(const QString &name,
                                  const QVector<PresetManagerEntry> &entries);
    static QString displayDate(qint64 utcMs);
    static QString html(const QString &text);

    QString m_presetTypeName;
    PresetManagerCallbacks m_callbacks;
    QVector<PresetManagerEntry> m_entries;
    QStringList m_warnings;

    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_sourceFilter = nullptr;
    QComboBox *m_categoryFilter = nullptr;
    QComboBox *m_viewFilter = nullptr;
    QTreeWidget *m_tree = nullptr;
    QTextBrowser *m_details = nullptr;
    QLabel *m_warningsLabel = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_createButton = nullptr;
    QPushButton *m_updateButton = nullptr;
    QPushButton *m_renameButton = nullptr;
    QPushButton *m_duplicateButton = nullptr;
    QPushButton *m_editDetailsButton = nullptr;
    QToolButton *m_favouriteButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_importButton = nullptr;
    QPushButton *m_exportButton = nullptr;
};

} // namespace vfx
