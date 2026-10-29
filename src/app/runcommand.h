// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "preferences.h"
#include "sshsession.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>

#include <functional>

class JumpChain;
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// Runs one command on many saved SSH sessions (a folder's), a few at a
// time, each over a connection of its own, and shows a table of results:
// exit status, time, and the output of the host chosen below it.
class RunCommandWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Host
    {
        QString id;
        QString name;
        QString address; // user@host[:port], as shown
    };
    // A host's connection (jump hosts, proxy, password), or why it cannot be
    // made; asked once per run for all chosen hosts.
    struct Resolved
    {
        SshConnectionSettings settings;
        QString problem;
    };
    using Resolver = std::function<QList<Resolved>(const QStringList &ids)>;
    using SnippetSource = std::function<QList<Snippet>()>;

    // skipped: sessions in the folder that are not SSH (left out, said).
    RunCommandWindow(const QString &folder, const QList<Host> &hosts, int skipped, Resolver resolver,
                     SnippetSource snippets, QWidget *parent);
    ~RunCommandWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    enum class State { Idle, Waiting, Connecting, Running, Done, Failed, Error, Stopped };
    struct Job
    {
        Host host;
        QTreeWidgetItem *item = nullptr;
        State state = State::Idle;
        SshConnectionSettings settings;
        QPointer<SshSession> ssh;
        QPointer<JumpChain> chain;
        QElapsedTimer timer;
        int exitStatus = -1;
        QByteArray output;
        bool truncated = false;
        QString message;
        qint64 milliseconds = 0;
    };

    void run(bool failedOnly);
    void stop();
    void startNext();
    void connectJob(int index, quint16 viaLocalPort);
    void finishJob(int index, State state, const QString &message);
    void closeJob(Job &job);
    void showJob(Job &job);
    void showSelected();
    void updateSummary();
    void rememberCommand(const QString &command);
    void fillRecentMenu();
    QString resultsText(QChar separator, bool quote) const;
    bool running() const;

    Resolver m_resolver;
    SnippetSource m_snippets;
    QString m_folder;
    int m_skipped = 0;
    QList<Job> m_jobs;
    QString m_command;
    int m_running = 0;

    QPlainTextEdit *m_commandEdit = nullptr;
    QToolButton *m_recent = nullptr;
    QSpinBox *m_parallel = nullptr;
    QSpinBox *m_timeout = nullptr;
    QCheckBox *m_terminal = nullptr;
    QPushButton *m_run = nullptr;
    QPushButton *m_stop = nullptr;
    QPushButton *m_again = nullptr;
    QTreeWidget *m_table = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QLabel *m_summary = nullptr;
};
