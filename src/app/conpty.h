// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "streamconnection.h"

#include <QStringList>

#include <atomic>
#include <thread>

// A local program (PowerShell, cmd, WSL, ...) in a Windows pseudo console
// (ConPTY, Windows 10 version 1809 and newer), shown in a terminal tab like
// any other stream. The pseudo console turns the program's console output
// into terminal escape sequences and keyboard input back.
class ConPtyConnection final : public StreamConnection
{
    Q_OBJECT

public:
    ConPtyConnection(const QString &program, const QStringList &arguments, const QString &label,
                     const QString &workingDirectory, QObject *parent = nullptr);
    ~ConPtyConnection() override;

    void open() override;
    void write(const QByteArray &data) override;
    void resize(int columns, int rows) override;
    void close() override;
    QString description() const override;
    bool announcesStart() const override { return false; }

    // Whether this Windows has pseudo consoles.
    static bool isAvailable();

private:
    void finished(unsigned long exitCode);

    QString m_program;
    QStringList m_arguments;
    QString m_label;
    QString m_workingDirectory;
    int m_columns = 80;
    int m_rows = 24;
    void *m_console = nullptr; // HPCON
    void *m_input = nullptr; // our end of the program's input
    void *m_output = nullptr; // our end of the program's output
    void *m_process = nullptr;
    std::thread m_reader;
    std::thread m_waiter;
    std::atomic<bool> m_closing{false};
};

// A local program run as administrator, in a tab like any other. Windows
// does not let an elevated program use this (unelevated) process's pseudo
// console, so an elevated copy of OpsRemote.exe (started through UAC with
// --elevated-console, see runElevatedConsoleHelper()) runs it in a pseudo
// console of its own and relays the screen and the keys over two named
// pipes, one per direction. Each side checks the other's process id, so no
// other program can take the pipes over.
class ElevatedConsoleConnection final : public StreamConnection
{
    Q_OBJECT

public:
    ElevatedConsoleConnection(const QString &program, const QStringList &arguments,
                              const QString &label, const QString &workingDirectory,
                              QObject *parent = nullptr);
    ~ElevatedConsoleConnection() override;

    void open() override;
    void write(const QByteArray &data) override;
    void resize(int columns, int rows) override;
    void close() override;
    QString description() const override;
    bool announcesStart() const override { return false; }

private:
    void sendFrame(char type, const QByteArray &payload);
    void finished(unsigned long exitCode);

    QString m_program;
    QStringList m_arguments;
    QString m_label;
    QString m_workingDirectory;
    QString m_pipeName;
    int m_columns = 80;
    int m_rows = 24;
    void *m_toHelper = nullptr; // keys and sizes
    void *m_fromHelper = nullptr; // the screen and the exit code
    void *m_helper = nullptr; // the elevated process
    std::thread m_reader;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_closing{false};
};

// Windows 11's sudo, when an administrator turned it on in inline mode (the
// only mode that runs inside this terminal).
bool windowsSudoInlineEnabled();
QString windowsSudoPath();

// The elevated side: OpsRemote.exe --elevated-console <pipe> <owner pid>
// <columns> <rows> <folder> <program> [arguments...]. Returns the exit code.
int runElevatedConsoleHelper();
