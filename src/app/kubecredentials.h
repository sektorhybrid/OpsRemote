// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QTreeWidget;
class QPushButton;
class QLabel;

// A saved kubeconfig (Tools → Kubernetes Credentials): a private file in
// settings/kube/, or encrypted in the credential vault or the system's
// store (as Preferences → Security and vault → Password storage chooses).
struct KubeCredential
{
    QString name;
    bool encrypted = false; // false: a file in settings/kube/
    QString file; // the file's name in settings/kube/

    bool operator==(const KubeCredential &) const = default;
};

QString kubeCredentialsFolder();
QList<KubeCredential> loadKubeCredentials();
void saveKubeCredentials(const QList<KubeCredential> &credentials);
QStringList kubeCredentialNames();
// The key of an encrypted credential in the vault or system store.
QString kubeCredentialKey(const QString &name);

// What a kubeconfig holds: its contexts and the servers of its clusters.
struct KubeconfigSummary
{
    bool valid = false;
    QStringList contexts;
    QString currentContext;
    QStringList servers;
};
KubeconfigSummary summarizeKubeconfig(const QString &text);

// Lists, adds (from a file, pasted text, or a server with a token or client
// certificate), shows and deletes the saved credentials.
class KubeCredentialsWindow final : public QDialog
{
    Q_OBJECT

public:
    // The encrypted ones are read, written and removed through these (the
    // window does not unlock the vault itself); false with error.
    using Read = std::function<QString(const QString &key)>;
    using Write = std::function<bool(const QString &key, const QString &value, QString *error)>;
    using Remove = std::function<void(const QString &key)>;

    KubeCredentialsWindow(Read read, Write write, Remove remove, QWidget *parent = nullptr);
    // A credential's kubeconfig, wherever it is kept; empty when missing.
    static QString contents(const KubeCredential &credential, const Read &read);

signals:
    void credentialsChanged();

private:
    void refresh(const QString &select = QString());
    QString selectedName() const;
    void updateButtons();
    void importFile();
    void paste();
    void fromServer(bool certificate);
    void show();
    void remove();
    // Saves a new credential (asking its name and where to keep it); false
    // when cancelled or not saved.
    bool add(const QString &text, const QString &suggestedName);

    Read m_read;
    Write m_write;
    Remove m_remove;
    QTreeWidget *m_list = nullptr;
    QPushButton *m_show = nullptr;
    QPushButton *m_delete = nullptr;
    QLabel *m_details = nullptr;
};
