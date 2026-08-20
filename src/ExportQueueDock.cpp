#include "ExportQueueDock.h"

#include "ExportQueueController.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace vfx {
namespace {

constexpr int JobIdRole = Qt::UserRole + 41;

QString localTimestamp(const QDateTime &utc)
{
    if (!utc.isValid()) return QStringLiteral("—");
    return utc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString outputCounts(const ExportQueueJobInfo &job)
{
    if (job.state == ExportQueueJobState::Pending
        || job.state == ExportQueueJobState::Recovered) {
        return QObject::tr("%1 configured").arg(job.totalOutputs);
    }
    return QObject::tr("%1 done · %2 skipped · %3 failed")
        .arg(job.completedOutputs)
        .arg(job.skippedOutputs)
        .arg(job.failedOutputs);
}

} // namespace

ExportQueueDock::ExportQueueDock(ExportQueueController *controller,
                                 QWidget *parent)
    : QDockWidget(parent), m_controller(controller)
{
    setObjectName(QStringLiteral("ExportQueueDock"));
    setWindowTitle(tr("Export Queue"));
    setAllowedAreas(Qt::BottomDockWidgetArea | Qt::LeftDockWidgetArea
                    | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable
                | QDockWidget::DockWidgetMovable
                | QDockWidget::DockWidgetFloatable);

    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    m_jobs = new QTreeWidget(content);
    m_jobs->setColumnCount(4);
    m_jobs->setHeaderLabels({tr("Job"), tr("State"), tr("Progress"), tr("Outputs")});
    m_jobs->setRootIsDecorated(false);
    m_jobs->setAlternatingRowColors(true);
    m_jobs->setSelectionMode(QAbstractItemView::SingleSelection);
    m_jobs->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_jobs->header()->setStretchLastSection(false);
    m_jobs->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_jobs->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_jobs->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_jobs->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_jobs->setMinimumHeight(fontMetrics().height() * 6);
    layout->addWidget(m_jobs, 2);

    m_progress = new QProgressBar(content);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    layout->addWidget(m_progress);

    m_summary = new QLabel(content);
    m_summary->setWordWrap(true);
    m_summary->setObjectName(QStringLiteral("MutedLabel"));
    layout->addWidget(m_summary);

    m_details = new QTextEdit(content);
    m_details->setReadOnly(true);
    m_details->setAcceptRichText(false);
    m_details->setMinimumHeight(fontMetrics().lineSpacing() * 4);
    layout->addWidget(m_details, 1);

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    m_pauseButton = new QPushButton(tr("Pause Queue"), content);
    m_resumeButton = new QPushButton(tr("Resume Recovered"), content);
    m_cancelButton = new QPushButton(tr("Cancel Job"), content);
    m_cancelAllButton = new QPushButton(tr("Cancel All"), content);
    m_removeButton = new QPushButton(tr("Remove"), content);
    m_clearButton = new QPushButton(tr("Clear Finished"), content);
    buttons->addWidget(m_pauseButton);
    buttons->addWidget(m_resumeButton);
    buttons->addWidget(m_cancelButton);
    buttons->addWidget(m_cancelAllButton);
    buttons->addStretch(1);
    buttons->addWidget(m_removeButton);
    buttons->addWidget(m_clearButton);
    layout->addLayout(buttons);

    setWidget(content);

    connect(m_jobs, &QTreeWidget::itemSelectionChanged,
            this, [this] {
        refreshDetails();
        refreshButtons();
    });
    connect(m_pauseButton, &QPushButton::clicked, this, [this] {
        if (m_controller) m_controller->setPaused(!m_controller->isPaused());
    });
    connect(m_resumeButton, &QPushButton::clicked, this, [this] {
        if (!m_controller) return;
        const QString jobId = selectedJobId();
        QStringList collisions;
        QString error;
        if (m_controller->resumeRecoveredJob(jobId, false,
                                              &collisions, &error)) {
            return;
        }
        if (!collisions.isEmpty()) {
            QStringList shown = collisions.mid(0, 12);
            if (collisions.size() > shown.size()) {
                shown.push_back(tr("…and %1 more existing file(s).")
                                    .arg(collisions.size() - shown.size()));
            }
            if (QMessageBox::question(
                    this, tr("Resume Recovered Export?"),
                    tr("This recovered job currently resolves onto existing files:\n\n%1\n\nReplace those files atomically when the job runs?")
                        .arg(shown.join(QLatin1Char('\n'))),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel) == QMessageBox::Yes) {
                collisions.clear();
                error.clear();
                if (m_controller->resumeRecoveredJob(jobId, true,
                                                      &collisions, &error)) {
                    return;
                }
            } else {
                return;
            }
        }
        if (!error.isEmpty()) {
            QMessageBox::critical(this, tr("Could Not Resume Export"), error);
        }
    });
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        if (m_controller) m_controller->cancelJob(selectedJobId());
    });
    connect(m_cancelAllButton, &QPushButton::clicked, this, [this] {
        if (!m_controller || !m_controller->hasUnfinishedJobs()) return;
        if (QMessageBox::question(
                this, tr("Cancel Export Queue?"),
                tr("Cancel the active export and every pending job? Already completed files will be kept."),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) == QMessageBox::Yes) {
            m_controller->cancelAll();
        }
    });
    connect(m_removeButton, &QPushButton::clicked, this, [this] {
        if (m_controller) m_controller->removeJob(selectedJobId());
    });
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_controller) m_controller->clearFinished();
    });
    if (m_controller) {
        connect(m_controller, &ExportQueueController::jobsChanged,
                this, &ExportQueueDock::refresh);
        connect(m_controller, &ExportQueueController::queueStateChanged,
                this, &ExportQueueDock::refreshButtons);
    }
    refresh();
}

void ExportQueueDock::showAndRaise()
{
    show();
    raise();
    activateWindow();
}

QString ExportQueueDock::selectedJobId() const
{
    if (!m_jobs) return {};
    const QList<QTreeWidgetItem *> selected = m_jobs->selectedItems();
    return selected.isEmpty()
        ? QString() : selected.constFirst()->data(0, JobIdRole).toString();
}

void ExportQueueDock::refresh()
{
    if (!m_controller || !m_jobs) return;
    const QString selectedBefore = selectedJobId();
    const QVector<ExportQueueJobInfo> queue = m_controller->jobs();
    m_jobs->clear();
    QTreeWidgetItem *selection = nullptr;
    QTreeWidgetItem *active = nullptr;
    for (const ExportQueueJobInfo &job : queue) {
        auto *item = new QTreeWidgetItem(m_jobs);
        item->setData(0, JobIdRole, job.id);
        item->setText(0, job.title);
        item->setToolTip(0, job.outputDirectory);
        item->setText(1, exportQueueJobStateName(job.state));
        item->setText(2, tr("%1%").arg(
            exportQueueProgressPercent(job.progressValue, job.progressMaximum)));
        item->setText(3, outputCounts(job));
        if (job.id == selectedBefore) selection = item;
        if (job.id == m_controller->activeJobId()) active = item;
    }
    if (!selection) selection = active;
    if (!selection && m_jobs->topLevelItemCount() > 0) {
        selection = m_jobs->topLevelItem(m_jobs->topLevelItemCount() - 1);
    }
    if (selection) {
        m_jobs->setCurrentItem(selection);
        selection->setSelected(true);
    }
    refreshDetails();
    refreshButtons();
}

void ExportQueueDock::refreshDetails()
{
    if (!m_controller) return;
    const ExportQueueJobInfo job = m_controller->job(selectedJobId());
    if (job.id.isEmpty()) {
        m_progress->setRange(0, 100);
        m_progress->setValue(0);
        m_summary->setText(tr("No export job selected."));
        m_details->clear();
        return;
    }
    if (job.state == ExportQueueJobState::Running
        && job.progressValue == 0) {
        m_progress->setRange(0, 0);
    } else {
        m_progress->setRange(0, 100);
        m_progress->setValue(exportQueueProgressPercent(
            job.progressValue, job.progressMaximum));
    }
    m_summary->setText(
        tr("%1 — %2\n%3")
            .arg(exportQueueJobStateName(job.state),
                 outputCounts(job),
                 job.statusText));

    QStringList lines;
    lines << tr("Document: %1").arg(
        job.documentName.isEmpty() ? tr("Untitled") : job.documentName)
          << tr("Destination: %1").arg(job.outputDirectory)
          << tr("Created: %1").arg(localTimestamp(job.createdUtc))
          << tr("Started: %1").arg(localTimestamp(job.startedUtc))
          << tr("Finished: %1").arg(localTimestamp(job.finishedUtc))
          << tr("Warnings: %1").arg(job.warningCount);
    if (!job.details.isEmpty()) {
        lines << QString() << tr("Issues and warnings:");
        lines.append(job.details);
    }
    m_details->setPlainText(lines.join(QLatin1Char('\n')));
}

void ExportQueueDock::refreshButtons()
{
    if (!m_controller) return;
    const ExportQueueJobInfo selected = m_controller->job(selectedJobId());
    m_pauseButton->setText(m_controller->isPaused()
        ? tr("Resume Queue") : tr("Pause Queue"));
    m_pauseButton->setEnabled(m_controller->hasUnfinishedJobs());
    m_resumeButton->setEnabled(
        !selected.id.isEmpty() && selected.state == ExportQueueJobState::Recovered);
    m_cancelButton->setEnabled(
        !selected.id.isEmpty() && exportQueueJobStateCanCancel(selected.state));
    m_cancelAllButton->setEnabled(m_controller->hasUnfinishedJobs());
    m_removeButton->setEnabled(
        !selected.id.isEmpty() && exportQueueJobStateCanRemove(selected.state));
    const QVector<ExportQueueJobInfo> queue = m_controller->jobs();
    m_clearButton->setEnabled(std::any_of(
        queue.cbegin(), queue.cend(),
        [](const ExportQueueJobInfo &job) {
            return exportQueueJobStateIsTerminal(job.state);
        }));
}

} // namespace vfx
