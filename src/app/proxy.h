// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>
#include <QWidget>

class QComboBox;
class QLineEdit;
class QSpinBox;

// An HTTP (CONNECT) or SOCKS5 proxy that SSH connections go out through.
// Stored as text on a session or folder ("proxy:") and in the Preferences:
//   ""                                   a session or folder: inherit
//   "none"                               connect directly
//   "http://[user@]host:port"
//   "socks5://[user@]host:port"
// The password is kept in the credential vault under vaultKey().
struct ProxySettings
{
    enum class Type { None, Http, Socks5 };
    Type type = Type::None;
    QString host;
    quint16 port = 0;
    QString user;
    QString password;

    // The stored value for connecting directly.
    static inline const QString Direct = QStringLiteral("none");
    // A label for a stored value, as in "HTTP proxy.corp:3128" or "none".
    static QString label(const QString &value);
    static ProxySettings parse(const QString &value);
    QString toString() const; // without the password
    // The credential vault entry for this proxy's password.
    QString vaultKey() const { return QStringLiteral("proxy:") + toString(); }
    bool isSet() const { return type != Type::None && !host.isEmpty() && port > 0; }
    bool operator==(const ProxySettings &) const = default;
};


// The "Proxy" tab of the session and folder windows and of the Preferences.
// inherited names what an empty value means; empty hides "Inherit".
class ProxyEditor final : public QWidget
{
    Q_OBJECT

public:
    ProxyEditor(const QString &value, const QString &password, const QString &inherited,
                QWidget *parent = nullptr);

    QString value() const;
    QString password() const;

signals:
    // The proxy itself changed (not the password).
    void changed();
    // The password field was left after editing.
    void passwordEntered();

private:
    void updateFields();

    QComboBox *m_type = nullptr;
    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_password = nullptr;
};
