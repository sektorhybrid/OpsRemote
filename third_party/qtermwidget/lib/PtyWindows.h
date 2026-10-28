/*
    SPDX-License-Identifier: LGPL-2.0-or-later
    SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

    OpsRemote addition to QTermWidget (LGPL-2.0-or-later, like the
    rest of this library).

    Windows replacement for Konsole's Unix Pty: the same interface, built on
    QProcess, so Session and the emulation compile unchanged. It does not
    start a local process yet (that will use ConPTY); terminals driven by an
    external transport use startTerminalTeletype(), which needs no process.
*/

#ifndef PTYWINDOWS_H
#define PTYWINDOWS_H

#include <QByteArray>
#include <QProcess>
#include <QSize>
#include <QStringList>

namespace Konsole {

// Stands in for KPtyDevice: there is no slave device on Windows.
class PtyDevice
{
public:
    int slaveFd() const { return -1; }
    int masterFd() const { return -1; }
    QByteArray ttyName() const { return {}; }
};

class Pty : public QProcess
{
    Q_OBJECT

public:
    explicit Pty(QObject *parent = nullptr);
    explicit Pty(int ptyMasterFd, QObject *parent = nullptr);
    ~Pty() override;

    // Not supported yet on Windows; returns -1.
    int start(const QString &program, const QStringList &arguments,
              const QStringList &environment, ulong winid, bool addToUtmp);

    void setEmptyPTYProperties() {}
    void setWriteable(bool) {}
    void setFlowControlEnabled(bool on) { _xonXoff = on; }
    bool flowControlEnabled() const { return _xonXoff; }
    void setWindowSize(int lines, int cols);
    QSize windowSize() const { return {_windowColumns, _windowLines}; }
    void setErase(char erase) { _eraseChar = erase; }
    char erase() const { return _eraseChar; }
    void setInitialWorkingDirectory(const QString &dir) { setWorkingDirectory(dir); }
    int foregroundProcessGroup() const { return 0; }
    void closePty() {}
    PtyDevice *pty() const { return &_device; }

public slots:
    void setUtf8Mode(bool) {}
    void lockPty(bool) {}
    void sendData(const char *buffer, int length);

signals:
    void receivedData(const char *buffer, int length);

private:
    int _windowColumns = 0;
    int _windowLines = 0;
    char _eraseChar = 0;
    bool _xonXoff = true;
    mutable PtyDevice _device;
};

}

#endif // PTYWINDOWS_H
