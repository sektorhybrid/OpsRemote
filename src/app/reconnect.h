// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>

#include <functional>

class QCheckBox;
class QObject;
class QFormLayout;
class QSpinBox;
class QWidget;
namespace YAML { class Node; }

// Reconnecting after a dropped connection works the same for every session
// type that does it (SSH and its file tabs, RDP, VNC, Telnet, Rlogin): each
// type keeps a ReconnectOptions ("auto_reconnect", a wait and a number of
// tries) in its Preferences tab and in each session, edited with the same
// rows, and its pages count the tries with a Reconnector. This is the one
// code for it.

// Telnet and Rlogin with reconnecting on: how soon a dead network ends the
// connection with an error (keepalive probes, tcpkeepalive.h).
constexpr int DeadNetworkSeconds = 60;
// A Telnet or Rlogin close after this long without output from the server
// counts as a drop, not a logout (whose text comes right before it): on
// Windows, Qt reports a dead network found by the probes, which start after
// 30 s of silence, as an ordinary close.
constexpr int QuietCloseSeconds = 20;

// Whether another try is allowed after made ones.
inline bool mayReconnect(int made, int attempts)
{
    return attempts == 0 || made < attempts;
}
// "attempt 2 of 5", or "attempt 2" when it keeps trying.
QString reconnectAttemptText(int attempt, int attempts);

// The two fields (Preferences' pages; a session window uses
// ReconnectEditorRows). With inheritedDelay/inheritedAttempts given (a
// session's window), the lowest value is the inherited one: 0 s, and -1 tries.
QSpinBox *reconnectDelayBox(int value, int inheritedDelay = -1);
QSpinBox *reconnectAttemptsBox(int value, int inheritedAttempts = -2);

// The three settings as one value, in every session type's options and
// Preferences. In a session, -1 / 0 / -1 mean "the Preferences'".
struct ReconnectOptions
{
    int autoReconnect = -1; // 1 on, 0 off
    int delay = 0; // seconds
    int attempts = -1; // 0: keep trying

    ReconnectOptions overriddenBy(const ReconnectOptions &session) const;
    bool operator==(const ReconnectOptions &) const = default;

    static ReconnectOptions builtIn(); // off, 5 s, 5 tries
};

// auto_reconnect, reconnect_delay, reconnect_attempts of a section or a
// session's options; only set values are written.
ReconnectOptions reconnectOptionsFromYaml(const YAML::Node &node);
void reconnectOptionsToYaml(const ReconnectOptions &options, YAML::Node &node);

// A session window's rows: the switch (its inherited state for ↺), then Wait
// and Tries.
class ReconnectEditorRows
{
public:
    ReconnectEditorRows(QFormLayout *form, const QString &switchText, const ReconnectOptions &value,
                        const ReconnectOptions &defaults, QWidget *parent, const QString &toolTip = {});
    ReconnectOptions value() const;

private:
    QCheckBox *m_switch = nullptr;
    QSpinBox *m_delay = nullptr;
    QSpinBox *m_attempts = nullptr;
    int m_inheritedOn = 0;
};

// The tries of one tab or page: each drop that may reconnect counts one, a
// connection that works again starts them over.
class Reconnector
{
public:
    void worked() { m_made = 0; }
    // Unless the tries are used up: counts one, logs it ("Connection: what:
    // reconnecting in…"), and calls start after the wait (on context's
    // thread, not after context is gone). Returns "attempt 2 of 5" for the
    // text shown, or empty when no try is left.
    QString retry(const ReconnectOptions &options, QObject *context, const QString &what,
                  const std::function<void()> &start);

private:
    int m_made = 0;
};
