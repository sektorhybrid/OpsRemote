// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "runcommand.h"

#include "jumphost.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

enum Column { HostColumn, ResultColumn, ExitColumn, TimeColumn, OutputColumn };

QString seconds(qint64 milliseconds)
{
    return QStringLiteral("%1 s").arg(double(milliseconds) / 1000.0, 0, 'f', 1);
}

// The first line worth showing in the table.
QString firstLine(const QByteArray &output)
{
    for (const auto &line : QString::fromUtf8(output).split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            return trimmed.left(200);
    }
    return QString();
}

}

RunCommandWindow::RunCommandWindow(const QString &folder, const QList<Host> &hosts, int skipped,
                                   Resolver resolver, SnippetSource snippets, QWidget *parent)
    : QWidget(parent, Qt::Window), m_resolver(std::move(resolver)), m_snippets(std::move(snippets)),
      m_folder(folder), m_skipped(skipped)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Run on Hosts: %1").arg(folder));
    resize(1000, 720);
    const auto defaults = loadRunCommandPreferences();
    auto *layout = new QVBoxLayout(this);

    // The command, with recent ones and snippets to start from.
    auto *commandRow = new QHBoxLayout;
    auto *commandLabel = new QLabel(QStringLiteral("Command:"), this);
    commandLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_commandEdit = new QPlainTextEdit(this);
    m_commandEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_commandEdit->setPlaceholderText(QStringLiteral("uptime; df -h /"));
    m_commandEdit->setAccessibleName(QStringLiteral("Command"));
    m_commandEdit->setToolTip(QStringLiteral(
        "Runs through each account's shell, as \"ssh host command\" does; several lines run one after another."));
    m_commandEdit->setFixedHeight(m_commandEdit->fontMetrics().lineSpacing() * 4 + 12);
    m_recent = new QToolButton(this);
    m_recent->setText(QStringLiteral("Recent and Snippets"));
    m_recent->setPopupMode(QToolButton::InstantPopup);
    m_recent->setMenu(new QMenu(m_recent));
    connect(m_recent->menu(), &QMenu::aboutToShow, this, &RunCommandWindow::fillRecentMenu);
    commandRow->addWidget(commandLabel);
    commandRow->addWidget(m_commandEdit, 1);
    commandRow->addWidget(m_recent, 0, Qt::AlignTop);
    layout->addLayout(commandRow);

    // This window's choices, starting from Preferences → Run on Hosts.
    auto *options = new QHBoxLayout;
    m_parallel = new QSpinBox(this);
    m_parallel->setRange(1, 50);
    m_parallel->setValue(defaults.parallel);
    m_parallel->setToolTip(QStringLiteral("How many hosts run the command at the same time"));
    m_timeout = new QSpinBox(this);
    m_timeout->setRange(1, 24 * 3600);
    m_timeout->setSuffix(QStringLiteral(" s"));
    m_timeout->setValue(defaults.timeoutSeconds);
    m_timeout->setToolTip(QStringLiteral("A command still running after this is stopped on that host"));
    m_terminal = new QCheckBox(QStringLiteral("Use a terminal (pty)"), this);
    m_terminal->setChecked(defaults.terminal);
    m_terminal->setToolTip(QStringLiteral(
        "For commands that need one, such as some sudo setups. Output then comes as a terminal "
        "shows it, and a command waiting for input waits until the time limit."));
    m_run = new QPushButton(QStringLiteral("▶ Run on Checked Hosts"), this);
    m_run->setDefault(true);
    m_stop = new QPushButton(QStringLiteral("■ Stop"), this);
    m_stop->setEnabled(false);
    m_again = new QPushButton(QStringLiteral("Run Again Where It Failed"), this);
    m_again->setEnabled(false);
    options->addWidget(new QLabel(QStringLiteral("At once:"), this));
    options->addWidget(m_parallel);
    options->addSpacing(8);
    options->addWidget(new QLabel(QStringLiteral("Time limit:"), this));
    options->addWidget(m_timeout);
    options->addSpacing(8);
    options->addWidget(m_terminal);
    options->addStretch();
    options->addWidget(m_run);
    options->addWidget(m_stop);
    options->addWidget(m_again);
    layout->addLayout(options);

    // One row per host; the checked ones run.
    auto *splitter = new QSplitter(Qt::Vertical, this);
    m_table = new QTreeWidget(splitter);
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setAccessibleName(QStringLiteral("Hosts"));
    m_table->setHeaderLabels({QStringLiteral("Host"), QStringLiteral("Result"), QStringLiteral("Exit"),
                              QStringLiteral("Time"), QStringLiteral("Output")});
    m_table->header()->setStretchLastSection(true);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(HostColumn, Qt::AscendingOrder);
    for (const auto &host : hosts) {
        Job job;
        job.host = host;
        job.item = new QTreeWidgetItem(m_table);
        job.item->setText(HostColumn, QStringLiteral("%1 (%2)").arg(host.name, host.address));
        job.item->setCheckState(HostColumn, Qt::Checked);
        job.item->setData(HostColumn, Qt::UserRole, int(m_jobs.size()));
        m_jobs.append(job);
    }
    for (int column = HostColumn; column < OutputColumn; ++column)
        m_table->resizeColumnToContents(column);
    m_output = new QPlainTextEdit(splitter);
    m_output->setReadOnly(true);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_output->setPlaceholderText(QStringLiteral("Choose a host to see all of its output."));
    m_output->setAccessibleName(QStringLiteral("Output"));
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    auto *bottom = new QHBoxLayout;
    auto *checkAll = new QToolButton(this);
    checkAll->setText(QStringLiteral("Check All"));
    auto *checkNone = new QToolButton(this);
    checkNone->setText(QStringLiteral("Check None"));
    auto *checkFailed = new QToolButton(this);
    checkFailed->setText(QStringLiteral("Check Failed"));
    m_summary = new QLabel(this);
    auto *copy = new QPushButton(QStringLiteral("Copy Results"), this);
    copy->setToolTip(QStringLiteral("Host, result, exit status, time and first line, tab-separated"));
    auto *save = new QPushButton(QStringLiteral("Save as CSV…"), this);
    bottom->addWidget(checkAll);
    bottom->addWidget(checkNone);
    bottom->addWidget(checkFailed);
    bottom->addSpacing(12);
    bottom->addWidget(m_summary, 1);
    bottom->addWidget(copy);
    bottom->addWidget(save);
    layout->addLayout(bottom);

    const auto checkWhere = [this](const std::function<bool(const Job &)> &which) {
        for (const auto &job : std::as_const(m_jobs))
            job.item->setCheckState(HostColumn, which(job) ? Qt::Checked : Qt::Unchecked);
    };
    connect(checkAll, &QToolButton::clicked, this, [checkWhere] { checkWhere([](const Job &) { return true; }); });
    connect(checkNone, &QToolButton::clicked, this, [checkWhere] { checkWhere([](const Job &) { return false; }); });
    connect(checkFailed, &QToolButton::clicked, this, [checkWhere] {
        checkWhere([](const Job &job) {
            return job.state == State::Failed || job.state == State::Error || job.state == State::Stopped;
        });
    });
    connect(m_run, &QPushButton::clicked, this, [this] { run(false); });
    connect(m_again, &QPushButton::clicked, this, [this] { run(true); });
    connect(m_stop, &QPushButton::clicked, this, &RunCommandWindow::stop);
    connect(m_table, &QTreeWidget::currentItemChanged, this, &RunCommandWindow::showSelected);
    connect(m_table, &QTreeWidget::itemChanged, this, [this] { updateSummary(); });
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(resultsText(QLatin1Char('\t'), false));
    });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save Results"),
                                                          m_folder + QStringLiteral(".csv"),
                                                          QStringLiteral("CSV files (*.csv)"));
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)
            || file.write(resultsText(QLatin1Char(','), true).toUtf8()) < 0 || !file.commit())
            QMessageBox::warning(this, QStringLiteral("Save Results"),
                                 QStringLiteral("Could not save %1: %2").arg(path, file.errorString()));
    });
    updateSummary();
}

RunCommandWindow::~RunCommandWindow()
{
    for (auto &job : m_jobs)
        closeJob(job);
}

void RunCommandWindow::closeEvent(QCloseEvent *event)
{
    if (running()
        && QMessageBox::question(this, QStringLiteral("Run on Hosts"),
                                 QStringLiteral("The command is still running on some hosts. Stop it and close?"))
               != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    stop();
    event->accept();
}

bool RunCommandWindow::running() const
{
    for (const auto &job : m_jobs) {
        if (job.state == State::Waiting || job.state == State::Connecting || job.state == State::Running)
            return true;
    }
    return false;
}

void RunCommandWindow::run(bool failedOnly)
{
    const QString command = m_commandEdit->toPlainText().trimmed();
    if (command.isEmpty() || running())
        return;
    QStringList ids;
    QList<int> chosen;
    for (int index = 0; index < m_jobs.size(); ++index) {
        const auto &job = m_jobs[index];
        const bool failed = job.state == State::Failed || job.state == State::Error || job.state == State::Stopped;
        if (failedOnly ? failed : job.item->checkState(HostColumn) == Qt::Checked) {
            chosen << index;
            ids << job.host.id;
        }
    }
    if (chosen.isEmpty()) {
        m_summary->setText(QStringLiteral("Check the hosts to run on first."));
        return;
    }
    rememberCommand(command);
    m_command = command;
    const auto resolved = m_resolver(ids);
    for (int at = 0; at < chosen.size(); ++at) {
        auto &job = m_jobs[chosen[at]];
        closeJob(job);
        job.output.clear();
        job.truncated = false;
        job.exitStatus = -1;
        job.milliseconds = 0;
        job.message.clear();
        const auto result = resolved.value(at);
        job.settings = result.settings;
        job.settings.openShell = false;
        job.settings.filesOnly = false;
        job.state = State::Waiting;
        if (!result.problem.isEmpty()) {
            job.state = State::Error;
            job.message = result.problem;
        }
        showJob(job);
    }
    m_stop->setEnabled(true);
    m_run->setEnabled(false);
    m_again->setEnabled(false);
    startNext();
    updateSummary();
}

void RunCommandWindow::stop()
{
    for (int index = 0; index < m_jobs.size(); ++index) {
        auto &job = m_jobs[index];
        if (job.state == State::Waiting || job.state == State::Connecting || job.state == State::Running)
            finishJob(index, State::Stopped, QStringLiteral("Stopped."));
    }
}

void RunCommandWindow::startNext()
{
    for (int index = 0; index < m_jobs.size() && m_running < m_parallel->value(); ++index) {
        auto &job = m_jobs[index];
        if (job.state != State::Waiting)
            continue;
        ++m_running;
        job.state = State::Connecting;
        job.timer.start();
        showJob(job);
        if (job.settings.jumps.empty()) {
            connectJob(index, 0);
            continue;
        }
        job.chain = new JumpChain(this);
        connect(job.chain, &JumpChain::ready, this, [this, index](quint16 port) { connectJob(index, port); });
        connect(job.chain, &JumpChain::failed, this, [this, index](const QString &reason, bool) {
            finishJob(index, State::Error, reason);
        });
        job.chain->open(job.settings.jumps, job.settings.host, job.settings.port);
    }
    if (m_running == 0 && !running()) {
        m_stop->setEnabled(false);
        m_run->setEnabled(true);
        bool failures = false;
        for (const auto &job : std::as_const(m_jobs))
            failures = failures || job.state == State::Failed || job.state == State::Error
                       || job.state == State::Stopped;
        m_again->setEnabled(failures);
        // Room for the results; a long reason is in full below and in its tooltip.
        for (int column = HostColumn; column < OutputColumn; ++column)
            m_table->resizeColumnToContents(column);
        m_table->setColumnWidth(ResultColumn, qMin(m_table->columnWidth(ResultColumn), 320));
    }
    updateSummary();
}

void RunCommandWindow::connectJob(int index, quint16 viaLocalPort)
{
    auto &job = m_jobs[index];
    auto settings = job.settings;
    settings.jumps.clear();
    settings.viaLocalPort = viaLocalPort;
    const QString server = job.host.name;
    auto *ssh = new SshSession(this);
    job.ssh = ssh;
    connect(ssh, &SshSession::hostKeyVerificationRequired, this,
            [this, ssh, server](SshHostKeyStatus status, const QString &keyType, const QString &fingerprint) {
                askSshHostKey(this, ssh, server, status, keyType, fingerprint);
            });
    connect(ssh, &SshSession::inputRequired, this,
            [this, ssh, server](const QString &instruction, const QStringList &prompts, const QList<bool> &echo) {
                askSshLogOn(this, ssh, server, instruction, prompts, echo);
            });
    connect(ssh, &SshSession::connected, this, [this, index, ssh] {
        auto &job = m_jobs[index];
        if (job.ssh != ssh)
            return;
        job.state = State::Running;
        showJob(job);
        const auto preferences = loadRunCommandPreferences();
        ssh->runCommand(m_command, m_terminal->isChecked(), m_timeout->value(), preferences.outputKb * 1024);
    });
    connect(ssh, &SshSession::commandFinished, this,
            [this, index, ssh](int, int exitStatus, const QByteArray &output, bool truncated,
                               const QString &error, int) {
                auto &job = m_jobs[index];
                if (job.ssh != ssh)
                    return;
                job.exitStatus = exitStatus;
                job.output = output;
                job.truncated = truncated;
                if (!error.isEmpty())
                    finishJob(index, State::Error, error);
                else
                    finishJob(index, exitStatus == 0 ? State::Done : State::Failed, QString());
            });
    connect(ssh, &SshSession::disconnected, this, [this, index, ssh](const QString &reason, bool) {
        auto &job = m_jobs[index];
        if (job.ssh != ssh)
            return;
        finishJob(index, State::Error, reason.isEmpty() ? QStringLiteral("The connection ended.") : reason);
    });
    ssh->connectToHost(settings);
    settings.password.fill(QChar(0));
}

void RunCommandWindow::finishJob(int index, State state, const QString &message)
{
    auto &job = m_jobs[index];
    const bool wasActive = job.state == State::Connecting || job.state == State::Running;
    if (job.state != State::Waiting && !wasActive)
        return;
    if (job.ssh && job.state == State::Running && state == State::Stopped)
        job.ssh->cancelCommands();
    closeJob(job);
    job.state = state;
    job.message = message;
    job.milliseconds = wasActive ? job.timer.elapsed() : 0;
    job.settings.password.fill(QChar(0));
    showJob(job);
    if (wasActive)
        --m_running;
    if (m_table->currentItem() == job.item)
        showSelected();
    startNext();
}

void RunCommandWindow::closeJob(Job &job)
{
    for (QObject *object : {static_cast<QObject *>(job.chain.data()), static_cast<QObject *>(job.ssh.data())}) {
        if (object) {
            object->disconnect(this);
            object->deleteLater();
        }
    }
    job.chain = nullptr;
    job.ssh = nullptr;
}

void RunCommandWindow::showJob(Job &job)
{
    QString result;
    QColor colour;
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    const QColor good = dark ? QColor(0x6c, 0xc6, 0x7a) : QColor(0x1e, 0x7b, 0x34);
    const QColor bad = dark ? QColor(0xf2, 0x7a, 0x6e) : QColor(0xb3, 0x26, 0x1e);
    switch (job.state) {
    case State::Idle:
        break;
    case State::Waiting:
        result = QStringLiteral("Waiting");
        break;
    case State::Connecting:
        result = QStringLiteral("… Connecting");
        break;
    case State::Running:
        result = QStringLiteral("… Running");
        break;
    case State::Done:
        result = QStringLiteral("✓ Done");
        colour = good;
        break;
    case State::Failed:
        result = QStringLiteral("✕ Failed");
        colour = bad;
        break;
    case State::Error:
        result = QStringLiteral("✕ %1").arg(job.message.section(QLatin1Char('\n'), 0, 0));
        colour = bad;
        break;
    case State::Stopped:
        result = QStringLiteral("■ Stopped");
        break;
    }
    job.item->setText(ResultColumn, result);
    job.item->setForeground(ResultColumn, colour.isValid() ? QBrush(colour) : palette().text());
    job.item->setText(ExitColumn, job.exitStatus >= 0 ? QString::number(job.exitStatus) : QString());
    job.item->setText(TimeColumn, job.milliseconds > 0 ? seconds(job.milliseconds) : QString());
    job.item->setText(OutputColumn, firstLine(job.output));
    job.item->setToolTip(ResultColumn, job.message);
    updateSummary();
}

void RunCommandWindow::showSelected()
{
    auto *item = m_table->currentItem();
    if (!item) {
        m_output->clear();
        return;
    }
    const auto &job = m_jobs[item->data(HostColumn, Qt::UserRole).toInt()];
    QString text = QString::fromUtf8(job.output);
    if (job.truncated)
        text += QStringLiteral("\n[More output was left out: see Preferences → Run on Hosts.]");
    if (!job.message.isEmpty())
        text += (text.isEmpty() ? QString() : QStringLiteral("\n")) + QStringLiteral("[%1]").arg(job.message);
    m_output->setPlainText(text);
}

void RunCommandWindow::updateSummary()
{
    int done = 0, failed = 0, errors = 0, active = 0, checked = 0;
    for (const auto &job : std::as_const(m_jobs)) {
        done += job.state == State::Done ? 1 : 0;
        failed += job.state == State::Failed ? 1 : 0;
        errors += job.state == State::Error || job.state == State::Stopped ? 1 : 0;
        active += job.state == State::Waiting || job.state == State::Connecting || job.state == State::Running ? 1 : 0;
        checked += job.item->checkState(HostColumn) == Qt::Checked ? 1 : 0;
    }
    QStringList parts;
    parts << QStringLiteral("%1 of %2 hosts checked").arg(checked).arg(m_jobs.size());
    if (active)
        parts << QStringLiteral("%1 running or waiting").arg(active);
    if (done)
        parts << QStringLiteral("%1 done").arg(done);
    if (failed)
        parts << QStringLiteral("%1 failed").arg(failed);
    if (errors)
        parts << QStringLiteral("%1 could not run").arg(errors);
    if (m_skipped)
        parts << (m_skipped == 1 ? QStringLiteral("1 non-SSH session left out")
                                 : QStringLiteral("%1 non-SSH sessions left out").arg(m_skipped));
    m_summary->setText(parts.join(QStringLiteral(" · ")));
}

void RunCommandWindow::rememberCommand(const QString &command)
{
    auto preferences = loadRunCommandPreferences();
    preferences.recent.removeAll(command);
    preferences.recent.prepend(command);
    while (preferences.recent.size() > preferences.keepRecent)
        preferences.recent.removeLast();
    saveRunCommandPreferences(preferences);
}

void RunCommandWindow::fillRecentMenu()
{
    auto *menu = m_recent->menu();
    menu->clear();
    const auto use = [this](const QString &text) { m_commandEdit->setPlainText(text); };
    const auto recent = loadRunCommandPreferences().recent;
    menu->addSection(QStringLiteral("Recent"));
    if (recent.isEmpty())
        menu->addAction(QStringLiteral("(none yet)"))->setEnabled(false);
    for (const auto &command : recent) {
        QString label = command.simplified();
        if (label.size() > 80)
            label = label.left(77) + QStringLiteral("…");
        menu->addAction(label, this, [use, command] { use(command); });
    }
    menu->addSection(QStringLiteral("Snippets"));
    const auto snippets = m_snippets ? m_snippets() : QList<Snippet>();
    if (snippets.isEmpty())
        menu->addAction(QStringLiteral("(none: Preferences → Snippets)"))->setEnabled(false);
    for (const auto &snippet : snippets)
        menu->addAction(snippet.name, this, [use, text = snippet.text] { use(text); });
}

QString RunCommandWindow::resultsText(QChar separator, bool quote) const
{
    const auto field = [quote, separator](QString text) {
        text.replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (!quote) {
            text.replace(separator, QLatin1Char(' '));
            return text;
        }
        text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        // A leading =, +, - or @ would be a formula in a spreadsheet.
        if (!text.isEmpty() && QStringLiteral("=+-@").contains(text.front()))
            text.prepend(QLatin1Char('\''));
        return QStringLiteral("\"%1\"").arg(text);
    };
    QStringList lines;
    lines << QStringList{field(QStringLiteral("Host")), field(QStringLiteral("Address")), field(QStringLiteral("Result")),
                         field(QStringLiteral("Exit")), field(QStringLiteral("Seconds")), field(QStringLiteral("Output"))}
                 .join(separator);
    for (int row = 0; row < m_table->topLevelItemCount(); ++row) {
        const auto &job = m_jobs[m_table->topLevelItem(row)->data(HostColumn, Qt::UserRole).toInt()];
        if (job.state == State::Idle)
            continue;
        lines << QStringList{field(job.host.name), field(job.host.address),
                             field(job.item->text(ResultColumn)),
                             field(job.exitStatus >= 0 ? QString::number(job.exitStatus) : QString()),
                             field(job.milliseconds > 0 ? QString::number(double(job.milliseconds) / 1000.0, 'f', 1)
                                                        : QString()),
                             field(quote ? QString::fromUtf8(job.output).trimmed() : firstLine(job.output))}
                     .join(separator);
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}
