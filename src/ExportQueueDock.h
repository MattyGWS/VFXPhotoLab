#pragma once

#include <QDockWidget>
#include <QString>

class QLabel;
class QProgressBar;
class QPushButton;
class QTextEdit;
class QTreeWidget;

namespace vfx {

class ExportQueueController;

class ExportQueueDock final : public QDockWidget {
    Q_OBJECT

public:
    explicit ExportQueueDock(ExportQueueController *controller,
                             QWidget *parent = nullptr);

    void showAndRaise();

private:
    QString selectedJobId() const;
    void refresh();
    void refreshDetails();
    void refreshButtons();

    ExportQueueController *m_controller = nullptr;
    QTreeWidget *m_jobs = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_summary = nullptr;
    QTextEdit *m_details = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_resumeButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_cancelAllButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_clearButton = nullptr;
};

} // namespace vfx
