// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "keymanager.h"
#include "appdirs.h"
#include "jumphost.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <libssh/libssh.h>

namespace {

// Split in two in the command, so the echoed command line does not match.
constexpr char InstalledMarker[] = "OPSREMOTE-KEY-INSTALLED";

QString keysFolder()
{
    return portableFilePath(QStringLiteral("settings/keys"));
}

QString keyPathFor(const QString &name)
{
    return QStringLiteral("settings/keys/") + name;
}

QLabel *helpLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

// Whether the private key needs a passphrase (it does not open without one).
bool hasPassphrase(const QString &path)
{
    ssh_key key = nullptr;
    const auto native = QDir::toNativeSeparators(path).toUtf8();
    const int result = ssh_pki_import_privkey_file(native.constData(), nullptr, nullptr, nullptr, &key);
    ssh_key_free(key);
    return result == SSH_ERROR;
}

// "type base64 comment" of a private key, or empty if it does not open.
QString publicKeyLine(const QString &path, const QString &passphrase, const QString &comment)
{
    ssh_key key = nullptr;
    const auto native = QDir::toNativeSeparators(path).toUtf8();
    const auto secret = passphrase.toUtf8();
    if (ssh_pki_import_privkey_file(native.constData(), secret.isEmpty() ? nullptr : secret.constData(),
                                    nullptr, nullptr, &key) != SSH_OK)
        return {};
    char *base64 = nullptr;
    QString line;
    if (ssh_pki_export_pubkey_base64(key, &base64) == SSH_OK)
        line = QStringLiteral("%1 %2 %3")
                   .arg(QString::fromUtf8(ssh_key_type_to_char(ssh_key_type(key))),
                        QString::fromLatin1(base64), comment)
                   .trimmed();
    ssh_string_free_char(base64);
    ssh_key_free(key);
    return line;
}

QString readPublicKey(const QString &privatePath)
{
    QFile file(privatePath + QStringLiteral(".pub"));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

bool writePublicKey(const QString &privatePath, const QString &line)
{
    QFile file(privatePath + QStringLiteral(".pub"));
    return file.open(QIODevice::WriteOnly) && file.write(line.toUtf8() + '\n') > 0;
}

QString defaultComment()
{
    const auto user = QString::fromLocal8Bit(qgetenv(
#ifdef Q_OS_WIN
        "USERNAME"
#else
        "USER"
#endif
        ));
    return QStringLiteral("%1@%2").arg(user, QHostInfo::localHostName());
}

}

KeyManagerDialog::KeyManagerDialog(SessionChoices sessions, SessionResolver resolver,
                                   UseKey useKey, RememberPassphrase rememberPassphrase,
                                   NewEntry::SessionMaker makeSession, QWidget *parent)
    : QDialog(parent), m_sessions(std::move(sessions)), m_resolver(std::move(resolver)),
      m_useKey(std::move(useKey)), m_rememberPassphrase(std::move(rememberPassphrase))
{
    setWindowTitle(QStringLiteral("SSH Keys"));
    resize(680, 560);
    auto *layout = new QVBoxLayout(this);

    // The keys
    layout->addWidget(new QLabel(QStringLiteral("Keys in settings/keys/ (they travel with "
                                                "OpsRemote):"), this));
    m_list = new QTreeWidget(this);
    m_list->setRootIsDecorated(false);
    m_list->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Type"),
                             QStringLiteral("Passphrase"), QStringLiteral("Comment")});
    m_list->header()->setStretchLastSection(true);
    layout->addWidget(m_list, 1);
    auto *keyButtons = new QHBoxLayout;
    auto *create = new QPushButton(QStringLiteral("New Key…"), this);
    auto *import = new QPushButton(QStringLiteral("Import…"), this);
    m_delete = new QPushButton(QStringLiteral("Delete…"), this);
    auto *folder = new QPushButton(QStringLiteral("Open Folder"), this);
    for (auto *button : {create, import, m_delete})
        keyButtons->addWidget(button);
    keyButtons->addStretch();
    keyButtons->addWidget(folder);
    layout->addLayout(keyButtons);

    m_publicKey = new QPlainTextEdit(this);
    m_publicKey->setReadOnly(true);
    m_publicKey->setMaximumHeight(80);
    m_publicKey->setPlaceholderText(QStringLiteral("Select a key to see its public key."));
    layout->addWidget(m_publicKey);
    m_copy = new QPushButton(QStringLiteral("Copy Public Key"), this);
    auto *copyRow = new QHBoxLayout;
    copyRow->addStretch();
    copyRow->addWidget(m_copy);
    layout->addLayout(copyRow);

    // Installing the selected key
    auto *installBox = new QGroupBox(QStringLiteral("Install the selected key on a server"), this);
    auto *installForm = new QFormLayout(installBox);
    m_server = new QComboBox(installBox);
    for (const auto &[id, label] : m_sessions())
        m_server->addItem(label, id);
    if (makeSession)
        NewEntry::add(
            m_server, std::move(makeSession), m_sessions, [](const QString &label) { return label; },
            [](const QString &id) { return QVariant(id); });
    m_useForSession = new QCheckBox(QStringLiteral("Make this session log on with the key"), installBox);
    m_useForSession->setChecked(true);
    m_installButton = new QPushButton(QStringLiteral("Install"), installBox);
    m_installStatus = new QLabel(installBox);
    m_installStatus->setWordWrap(true);
    installForm->addRow(QStringLiteral("SSH session:"), m_server);
    installForm->addRow(QString(), m_useForSession);
    installForm->addRow(m_installButton, m_installStatus);
    installForm->addRow(helpLabel(QStringLiteral(
        "Logs on to the server (with its password or current key) and adds the public key to "
        "~/.ssh/authorized_keys, like ssh-copy-id. For Linux and other Unix servers."), installBox));
    layout->addWidget(installBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_list, &QTreeWidget::currentItemChanged, this, &KeyManagerDialog::showSelected);
    connect(m_server, &QComboBox::currentIndexChanged, this, &KeyManagerDialog::showSelected);
    connect(create, &QPushButton::clicked, this, &KeyManagerDialog::createKey);
    connect(import, &QPushButton::clicked, this, &KeyManagerDialog::importKey);
    connect(m_delete, &QPushButton::clicked, this, &KeyManagerDialog::deleteKey);
    connect(folder, &QPushButton::clicked, this, [] {
        QDir().mkpath(keysFolder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(keysFolder()));
    });
    connect(m_copy, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(m_publicKey->toPlainText()); });
    connect(m_installButton, &QPushButton::clicked, this, &KeyManagerDialog::install);
    refresh();
}

// The key pairs: private keys with a ".pub" next to them.
void KeyManagerDialog::refresh(const QString &select)
{
    const QString current = select.isEmpty() ? selectedName() : select;
    m_list->clear();
    const QDir folder(keysFolder());
    for (const auto &publicFile : folder.entryList({QStringLiteral("*.pub")}, QDir::Files, QDir::Name)) {
        const QString name = publicFile.chopped(4);
        const QString path = folder.filePath(name);
        if (!QFile::exists(path))
            continue;
        const auto parts = readPublicKey(path).split(QLatin1Char(' '));
        auto *item = new QTreeWidgetItem(
            m_list, {name, QString(parts.value(0)).remove(QStringLiteral("ssh-")),
                     hasPassphrase(path) ? QStringLiteral("Yes") : QStringLiteral("No"),
                     parts.mid(2).join(QLatin1Char(' '))});
        if (name == current)
            m_list->setCurrentItem(item);
    }
    for (int column = 0; column < 3; ++column)
        m_list->resizeColumnToContents(column);
    if (!m_list->currentItem() && m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
    showSelected();
}

QString KeyManagerDialog::selectedName() const
{
    auto *item = m_list->currentItem();
    return item ? item->text(0) : QString();
}

void KeyManagerDialog::showSelected()
{
    const QString name = selectedName();
    const bool selected = !name.isEmpty();
    m_publicKey->setPlainText(selected ? readPublicKey(QDir(keysFolder()).filePath(name)) : QString());
    m_copy->setEnabled(selected);
    m_delete->setEnabled(selected);
    m_installButton->setEnabled(selected && serverChosen() && !m_ssh && !m_chain);
    if (!serverChosen())
        m_installStatus->setText(QStringLiteral("You have no saved SSH sessions; choose New… to add one."));
    else if (!m_ssh && !m_chain)
        m_installStatus->setText(selected ? QString() : QStringLiteral("Select or create a key first."));
}

bool KeyManagerDialog::serverChosen() const
{
    const int index = m_server->currentIndex();
    return index >= 0 && !NewEntry::isNew(m_server, index);
}

void KeyManagerDialog::createKey()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New SSH Key"));
    dialog.setMinimumWidth(440);
    auto *form = new QFormLayout(&dialog);
    auto *type = new QComboBox(&dialog);
    type->addItem(QStringLiteral("Ed25519 (recommended)"), QStringLiteral("ed25519"));
    type->addItem(QStringLiteral("RSA 4096 (for old servers)"), QStringLiteral("rsa"));
    auto *name = new QLineEdit(QStringLiteral("id_ed25519"), &dialog);
    auto *comment = new QLineEdit(defaultComment(), &dialog);
    auto *passphrase = new QLineEdit(&dialog);
    passphrase->setEchoMode(QLineEdit::Password);
    passphrase->setPlaceholderText(QStringLiteral("Optional"));
    auto *confirm = new QLineEdit(&dialog);
    confirm->setEchoMode(QLineEdit::Password);
    auto *remember = new QCheckBox(QStringLiteral("Remember the passphrase in the credential vault"),
                                   &dialog);
    remember->setChecked(true);
    form->addRow(QStringLiteral("Type:"), type);
    form->addRow(QStringLiteral("File name:"), name);
    form->addRow(QStringLiteral("Comment:"), comment);
    form->addRow(QStringLiteral("Passphrase:"), passphrase);
    form->addRow(QStringLiteral("Repeat passphrase:"), confirm);
    form->addRow(QString(), remember);
    form->addRow(helpLabel(QStringLiteral(
        "A passphrase protects the key if someone copies the folder. OpsRemote asks for it when "
        "logging on, unless it is remembered in the (encrypted) vault."), &dialog));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create"));
    form->addRow(buttons);
    const auto updateRemember = [passphrase, confirm, remember] {
        const bool any = !passphrase->text().isEmpty();
        confirm->setEnabled(any);
        remember->setEnabled(any);
    };
    connect(passphrase, &QLineEdit::textChanged, &dialog, updateRemember);
    updateRemember();
    connect(type, &QComboBox::currentIndexChanged, &dialog, [type, name] {
        const bool rsa = type->currentData().toString() == QStringLiteral("rsa");
        if (name->text() == QStringLiteral("id_ed25519") || name->text() == QStringLiteral("id_rsa"))
            name->setText(rsa ? QStringLiteral("id_rsa") : QStringLiteral("id_ed25519"));
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const QString file = name->text().trimmed();
        QString problem;
        if (file.isEmpty() || file.contains(QLatin1Char('/')) || file.contains(QLatin1Char('\\')))
            problem = QStringLiteral("Enter a file name without folders.");
        else if (QFile::exists(QDir(keysFolder()).filePath(file)))
            problem = QStringLiteral("settings/keys/%1 already exists; choose another name.").arg(file);
        else if (passphrase->text() != confirm->text())
            problem = QStringLiteral("The passphrases do not match.");
        if (problem.isEmpty())
            dialog.accept();
        else
            QMessageBox::warning(&dialog, dialog.windowTitle(), problem);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString file = name->text().trimmed();
    const QString path = QDir(keysFolder()).filePath(file);
    QDir().mkpath(keysFolder());
    QFile::setPermissions(keysFolder(),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const bool rsa = type->currentData().toString() == QStringLiteral("rsa");
    QApplication::setOverrideCursor(Qt::WaitCursor);
    ssh_key key = nullptr;
#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 12, 0)
    ssh_pki_ctx context = ssh_pki_ctx_new();
    int bits = 4096;
    if (rsa)
        ssh_pki_ctx_options_set(context, SSH_PKI_OPTION_RSA_KEY_SIZE, &bits);
    const int generated = ssh_pki_generate_key(rsa ? SSH_KEYTYPE_RSA : SSH_KEYTYPE_ED25519,
                                               context, &key);
    ssh_pki_ctx_free(context);
#else
    const int generated = ssh_pki_generate(rsa ? SSH_KEYTYPE_RSA : SSH_KEYTYPE_ED25519,
                                           rsa ? 4096 : 0, &key);
#endif
    QApplication::restoreOverrideCursor();
    if (generated != SSH_OK || !key) {
        QMessageBox::warning(this, QStringLiteral("New SSH Key"), QStringLiteral("Could not create the key."));
        return;
    }
    const auto native = QDir::toNativeSeparators(path).toUtf8();
    auto secret = passphrase->text().toUtf8();
    const bool saved = ssh_pki_export_privkey_file(key, secret.isEmpty() ? nullptr : secret.constData(),
                                                   nullptr, nullptr, native.constData())
                       == SSH_OK;
    secret.fill('\0');
    char *base64 = nullptr;
    QString line;
    if (saved && ssh_pki_export_pubkey_base64(key, &base64) == SSH_OK)
        line = QStringLiteral("%1 %2 %3")
                   .arg(QString::fromUtf8(ssh_key_type_to_char(ssh_key_type(key))),
                        QString::fromLatin1(base64), comment->text().trimmed())
                   .trimmed();
    ssh_string_free_char(base64);
    ssh_key_free(key);
    if (line.isEmpty() || !writePublicKey(path, line)) {
        QFile::remove(path);
        QMessageBox::warning(this, QStringLiteral("New SSH Key"),
                             QStringLiteral("Could not save the key in settings/keys/."));
        return;
    }
    restrictToOwner(path);
    if (!passphrase->text().isEmpty() && remember->isChecked() && m_rememberPassphrase
        && !m_rememberPassphrase(keyPathFor(file), passphrase->text()))
        QMessageBox::information(this, QStringLiteral("New SSH Key"),
                                 QStringLiteral("The key was created, but its passphrase was not "
                                                "remembered: OpsRemote will ask for it."));
    refresh(file);
}

// Copies an existing private key (and its .pub) into settings/keys/.
void KeyManagerDialog::importKey()
{
    const QString source = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import a Private Key"), QDir::home().filePath(QStringLiteral(".ssh")));
    if (source.isEmpty())
        return;
    const QString name = QFileInfo(source).fileName();
    const QString target = QDir(keysFolder()).filePath(name);
    if (QFile::exists(target)) {
        QMessageBox::warning(this, QStringLiteral("Import Key"),
                             QStringLiteral("settings/keys/%1 already exists.").arg(name));
        return;
    }
    QString line = readPublicKey(source);
    QString passphrase;
    if (line.isEmpty()) {
        // No .pub next to it: derive it, which needs the passphrase if there is one.
        if (hasPassphrase(source)) {
            bool ok = false;
            passphrase = QInputDialog::getText(this, QStringLiteral("Import Key"),
                                               QStringLiteral("Passphrase for %1:").arg(name),
                                               QLineEdit::Password, QString(), &ok);
            if (!ok)
                return;
        }
        line = publicKeyLine(source, passphrase, defaultComment());
        if (line.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Import Key"),
                                 QStringLiteral("%1 is not a private key, or the passphrase is wrong.")
                                     .arg(name));
            return;
        }
    }
    QDir().mkpath(keysFolder());
    if (!QFile::copy(source, target) || !writePublicKey(target, line)) {
        QFile::remove(target);
        QMessageBox::warning(this, QStringLiteral("Import Key"), QStringLiteral("Could not copy the key."));
        return;
    }
    restrictToOwner(target);
    refresh(name);
}

void KeyManagerDialog::deleteKey()
{
    const QString name = selectedName();
    if (name.isEmpty()
        || QMessageBox::question(this, QStringLiteral("Delete Key"),
                                 QStringLiteral("Delete the key %1? Servers and sessions that use "
                                                "it can no longer log on with it.").arg(name))
               != QMessageBox::Yes)
        return;
    const QString path = QDir(keysFolder()).filePath(name);
    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".pub"));
    if (m_rememberPassphrase)
        m_rememberPassphrase(keyPathFor(name), QString());
    refresh();
}

void KeyManagerDialog::install()
{
    const QString name = selectedName();
    m_installKey = readPublicKey(QDir(keysFolder()).filePath(name));
    if (m_installKey.isEmpty() || m_installKey.contains(QLatin1Char('\'')) || !serverChosen()
        || m_ssh || m_chain)
        return;
    m_installName = name;
    m_target = SshConnectionSettings();
    if (!m_resolver(m_server->currentData().toString(), &m_target))
        return;
    m_installButton->setEnabled(false);
    m_output.clear();
    m_installStatus->setText(QStringLiteral("Connecting to %1…").arg(m_target.host));
    if (m_target.jumps.empty()) {
        installThrough(0);
        return;
    }
    m_chain = new JumpChain(this);
    connect(m_chain, &JumpChain::ready, this, &KeyManagerDialog::installThrough);
    connect(m_chain, &JumpChain::failed, this,
            [this](const QString &reason, bool) { finishInstall(reason, false); });
    m_chain->open(m_target.jumps, m_target.host, m_target.port);
}

void KeyManagerDialog::installThrough(quint16 viaLocalPort)
{
    auto settings = m_target;
    settings.jumps.clear();
    settings.viaLocalPort = viaLocalPort;
    settings.forwards.clear();
    settings.forwardAgent = false;
    m_target.password.fill(QChar(0));
    const QString server = settings.host;
    m_ssh = new SshSession(this);
    connect(m_ssh, &SshSession::hostKeyVerificationRequired, this,
            [this, server](SshHostKeyStatus status, const QString &keyType, const QString &fingerprint) {
                askSshHostKey(this, m_ssh, server, status, keyType, fingerprint);
            });
    connect(m_ssh, &SshSession::inputRequired, this,
            [this, server](const QString &instruction, const QStringList &prompts,
                           const QList<bool> &echo) {
                askSshLogOn(this, m_ssh, server, instruction, prompts, echo);
            });
    connect(m_ssh, &SshSession::connected, this, [this] {
        m_installStatus->setText(QStringLiteral("Adding the key…"));
        const QString marker = QString::fromLatin1(InstalledMarker);
        // A public key line has no single quotes (checked in install()).
        const QString command = QStringLiteral(
            "umask 077; mkdir -p ~/.ssh && touch ~/.ssh/authorized_keys && "
            "{ grep -qxF '%1' ~/.ssh/authorized_keys || echo '%1' >> ~/.ssh/authorized_keys; } && "
            "echo '%2''%3'; exit\n")
            .arg(m_installKey, marker.left(10), marker.mid(10));
        m_ssh->write(command.toUtf8());
    });
    connect(m_ssh, &SshSession::dataReceived, this,
            [this](const QByteArray &data) { m_output += data; });
    connect(m_ssh, &SshSession::disconnected, this, [this](const QString &reason, bool) {
        if (m_output.contains(InstalledMarker))
            finishInstall(QString(), true);
        else
            finishInstall(reason, false);
    });
    m_ssh->connectToHost(settings);
    settings.password.fill(QChar(0));
}

void KeyManagerDialog::finishInstall(const QString &message, bool success)
{
    for (QObject *object : {static_cast<QObject *>(m_ssh.data()), static_cast<QObject *>(m_chain.data())}) {
        if (object) {
            object->disconnect(this);
            object->deleteLater();
        }
    }
    m_ssh = nullptr;
    m_chain = nullptr;
    m_installButton->setEnabled(!selectedName().isEmpty());
    if (!success) {
        m_installStatus->setText(QStringLiteral("Could not install the key: %1").arg(message));
        return;
    }
    QString text = QStringLiteral("%1 installed on %2.").arg(m_installName, m_server->currentText());
    if (m_useForSession->isChecked() && m_useKey) {
        m_useKey(m_server->currentData().toString(), keyPathFor(m_installName));
        text += QStringLiteral(" The session now logs on with it.");
    }
    m_installStatus->setText(text);
}
