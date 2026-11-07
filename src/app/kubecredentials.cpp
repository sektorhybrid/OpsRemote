// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "kubecredentials.h"
#include "filedialogs.h"
#include "appdirs.h"
#include "localsessions.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <yaml-cpp/yaml.h>

namespace {
QString indexPath()
{
    return QDir(kubeCredentialsFolder()).filePath(QStringLiteral("credentials.yaml"));
}

bool writePrivate(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        return false;
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

QString readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

// A browse button beside a path field.
QWidget *pathRow(QLineEdit *edit, QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *row = new QHBoxLayout(widget);
    row->setContentsMargins(0, 0, 0, 0);
    auto *browse = new QPushButton(QStringLiteral("Browse…"), widget);
    row->addWidget(edit, 1);
    row->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, widget, [edit] {
        const auto file = FileDialogs::openFileName(edit->window(), QString(), QDir::homePath());
        if (!file.isEmpty())
            edit->setText(QDir::toNativeSeparators(file));
    });
    return widget;
}
}

QString kubeCredentialsFolder()
{
    return portableFilePath(QStringLiteral("settings/kube"));
}

QList<KubeCredential> loadKubeCredentials()
{
    QList<KubeCredential> credentials;
    try {
        const auto node = YAML::LoadFile(indexPath().toStdString());
        if (node.IsSequence()) {
            for (const auto &item : node) {
                KubeCredential credential;
                credential.name = QString::fromStdString(item["name"].as<std::string>(""));
                credential.encrypted = item["encrypted"].as<bool>(false);
                credential.file = QString::fromStdString(item["file"].as<std::string>(""));
                if (!credential.name.isEmpty())
                    credentials.append(credential);
            }
        }
    } catch (const YAML::Exception &) {
    }
    return credentials;
}

void saveKubeCredentials(const QList<KubeCredential> &credentials)
{
    YAML::Node node(YAML::NodeType::Sequence);
    for (const auto &credential : credentials) {
        YAML::Node item(YAML::NodeType::Map);
        item["name"] = credential.name.toStdString();
        if (credential.encrypted)
            item["encrypted"] = true;
        else
            item["file"] = credential.file.toStdString();
        node.push_back(item);
    }
    YAML::Emitter emitter;
    emitter << node;
    writePrivate(indexPath(), QByteArray(emitter.c_str()) + '\n');
}

QStringList kubeCredentialNames()
{
    QStringList names;
    for (const auto &credential : loadKubeCredentials())
        names << credential.name;
    return names;
}

QString kubeCredentialKey(const QString &name)
{
    return QStringLiteral("kube:") + name;
}

KubeconfigSummary summarizeKubeconfig(const QString &text)
{
    KubeconfigSummary summary;
    try {
        const auto node = YAML::Load(text.toStdString());
        if (!node.IsMap())
            return summary;
        for (const auto &context : node["contexts"])
            summary.contexts << QString::fromStdString(context["name"].as<std::string>(""));
        for (const auto &cluster : node["clusters"])
            summary.servers << QString::fromStdString(cluster["cluster"]["server"].as<std::string>(""));
        summary.currentContext = QString::fromStdString(node["current-context"].as<std::string>(""));
        summary.valid = !summary.contexts.isEmpty() || !summary.servers.isEmpty();
    } catch (const YAML::Exception &) {
    }
    return summary;
}

QString KubeCredentialsWindow::contents(const KubeCredential &credential, const Read &read)
{
    if (credential.encrypted)
        return read ? read(kubeCredentialKey(credential.name)) : QString();
    return readFile(QDir(kubeCredentialsFolder()).filePath(credential.file));
}

KubeCredentialsWindow::KubeCredentialsWindow(Read read, Write write, Remove remove, QWidget *parent)
    : QDialog(parent), m_read(std::move(read)), m_write(std::move(write)), m_remove(std::move(remove))
{
    setWindowTitle(QStringLiteral("Kubernetes Credentials"));
    resize(720, 460);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral("Kubeconfigs for Container (Kubernetes) sessions, kept with "
                                            "OpsRemote: as a private file in settings/kube/, or encrypted "
                                            "(the vault, or the system's store). Cloud clusters (EKS, GKE, "
                                            "AKS) and OIDC log on through the helper their kubeconfig runs."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_list = new QTreeWidget(this);
    m_list->setRootIsDecorated(false);
    m_list->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Kept in"), QStringLiteral("Contexts"),
                             QStringLiteral("Server")});
    m_list->header()->setStretchLastSection(true);
    layout->addWidget(m_list, 1);
    m_details = new QLabel(this);
    m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_details);
    auto *buttons = new QHBoxLayout;
    auto *import = new QPushButton(QStringLiteral("Import File…"), this);
    auto *pasteButton = new QPushButton(QStringLiteral("Paste…"), this);
    auto *token = new QPushButton(QStringLiteral("From Token…"), this);
    auto *certificate = new QPushButton(QStringLiteral("From Certificate…"), this);
    m_show = new QPushButton(QStringLiteral("Show…"), this);
    m_delete = new QPushButton(QStringLiteral("Delete…"), this);
    auto *folder = new QPushButton(QStringLiteral("Open Folder"), this);
    for (auto *button : {import, pasteButton, token, certificate, m_show, m_delete})
        buttons->addWidget(button);
    buttons->addStretch();
    buttons->addWidget(folder);
    layout->addLayout(buttons);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(import, &QPushButton::clicked, this, &KubeCredentialsWindow::importFile);
    connect(pasteButton, &QPushButton::clicked, this, &KubeCredentialsWindow::paste);
    connect(token, &QPushButton::clicked, this, [this] { fromServer(false); });
    connect(certificate, &QPushButton::clicked, this, [this] { fromServer(true); });
    connect(m_show, &QPushButton::clicked, this, &KubeCredentialsWindow::show);
    connect(m_delete, &QPushButton::clicked, this, &KubeCredentialsWindow::remove);
    connect(folder, &QPushButton::clicked, this, [] {
        QDir().mkpath(kubeCredentialsFolder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(kubeCredentialsFolder()));
    });
    connect(m_list, &QTreeWidget::currentItemChanged, this, &KubeCredentialsWindow::updateButtons);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, &KubeCredentialsWindow::show);
    refresh();
}

void KubeCredentialsWindow::refresh(const QString &select)
{
    m_list->clear();
    for (const auto &credential : loadKubeCredentials()) {
        // Encrypted ones are read only when shown; the vault may be locked.
        const auto text = credential.encrypted ? QString() : contents(credential, m_read);
        const auto summary = summarizeKubeconfig(text);
        auto *item = new QTreeWidgetItem(
            m_list, {credential.name,
                     credential.encrypted ? QStringLiteral("Encrypted") : QStringLiteral("settings/kube/%1").arg(credential.file),
                     credential.encrypted ? QStringLiteral("—") : summary.contexts.join(QStringLiteral(", ")),
                     credential.encrypted ? QString() : summary.servers.join(QStringLiteral(", "))});
        if (credential.name == select)
            m_list->setCurrentItem(item);
    }
    for (int column = 0; column < 3; ++column)
        m_list->resizeColumnToContents(column);
    if (!m_list->currentItem() && m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
    updateButtons();
}

QString KubeCredentialsWindow::selectedName() const
{
    const auto *item = m_list->currentItem();
    return item ? item->text(0) : QString();
}

void KubeCredentialsWindow::updateButtons()
{
    const bool selected = !selectedName().isEmpty();
    m_show->setEnabled(selected);
    m_delete->setEnabled(selected);
    m_details->setText(selected ? QString()
                                : QStringLiteral("No credentials yet: import a kubeconfig, paste one, or make "
                                                 "one from a server's token or client certificate."));
}

bool KubeCredentialsWindow::add(const QString &text, const QString &suggestedName)
{
    const auto summary = summarizeKubeconfig(text);
    if (!summary.valid) {
        QMessageBox::warning(this, windowTitle(), QStringLiteral("This is not a kubeconfig (no contexts or clusters)."));
        return false;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Save Credential"));
    auto *form = new QFormLayout(&dialog);
    auto *name = new QLineEdit(suggestedName, &dialog);
    auto *where = new QComboBox(&dialog);
    where->addItem(QStringLiteral("Encrypted (vault or system store)"), true);
    where->addItem(QStringLiteral("A private file in settings/kube/"), false);
    form->addRow(QStringLiteral("Name:"), name);
    form->addRow(QStringLiteral("Keep it:"), where);
    auto *info = new QLabel(QStringLiteral("Contexts: %1").arg(summary.contexts.join(QStringLiteral(", "))), &dialog);
    info->setWordWrap(true);
    form->addRow(info);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    auto credentials = loadKubeCredentials();
    const auto chosen = name->text().trimmed();
    for (const auto &credential : credentials) {
        if (credential.name == chosen) {
            QMessageBox::warning(this, windowTitle(), QStringLiteral("There is a credential called %1 already.").arg(chosen));
            return false;
        }
    }
    if (chosen.isEmpty())
        return false;
    KubeCredential credential;
    credential.name = chosen;
    credential.encrypted = where->currentData().toBool();
    if (credential.encrypted) {
        QString error;
        if (!m_write || !m_write(kubeCredentialKey(chosen), text, &error)) {
            QMessageBox::warning(this, windowTitle(),
                                 error.isEmpty() ? QStringLiteral("The vault stays locked; not saved.") : error);
            return false;
        }
    } else {
        static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9._-]+"));
        auto base = QString(chosen).replace(unsafe, QStringLiteral("-"));
        credential.file = base + QStringLiteral(".yaml");
        for (int number = 2; QFile::exists(QDir(kubeCredentialsFolder()).filePath(credential.file)); ++number)
            credential.file = QStringLiteral("%1-%2.yaml").arg(base).arg(number);
        if (!writePrivate(QDir(kubeCredentialsFolder()).filePath(credential.file), text.toUtf8())) {
            QMessageBox::warning(this, windowTitle(), QStringLiteral("Could not write settings/kube/%1.").arg(credential.file));
            return false;
        }
    }
    credentials.append(credential);
    saveKubeCredentials(credentials);
    refresh(chosen);
    emit credentialsChanged();
    return true;
}

void KubeCredentialsWindow::importFile()
{
    const auto start = qEnvironmentVariable("KUBECONFIG").section(QDir::listSeparator(), 0, 0);
    const auto path = FileDialogs::openFileName(
        this, QStringLiteral("Import a Kubeconfig"),
        start.isEmpty() ? QDir::home().filePath(QStringLiteral(".kube/config")) : start);
    if (path.isEmpty())
        return;
    const auto text = readFile(path);
    const auto summary = summarizeKubeconfig(text);
    add(text, summary.currentContext.isEmpty() ? QFileInfo(path).completeBaseName() : summary.currentContext);
}

void KubeCredentialsWindow::paste()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Paste a Kubeconfig"));
    dialog.resize(560, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QPlainTextEdit(&dialog);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setPlaceholderText(QStringLiteral("apiVersion: v1\nkind: Config\nclusters: …"));
    layout->addWidget(text);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto summary = summarizeKubeconfig(text->toPlainText());
    add(text->toPlainText(), summary.currentContext);
}

void KubeCredentialsWindow::fromServer(bool certificate)
{
    QDialog dialog(this);
    dialog.setWindowTitle(certificate ? QStringLiteral("From a Client Certificate") : QStringLiteral("From a Token"));
    dialog.setMinimumWidth(520);
    auto *form = new QFormLayout(&dialog);
    auto *server = new QLineEdit(&dialog);
    server->setPlaceholderText(QStringLiteral("https://api.example.com:6443"));
    auto *token = new QLineEdit(&dialog);
    token->setEchoMode(QLineEdit::Password);
    auto *ca = new QLineEdit(&dialog);
    ca->setPlaceholderText(QStringLiteral("The server's CA (PEM); empty: the system's"));
    auto *insecure = new QCheckBox(QStringLiteral("Do not check the server's certificate (test clusters only)"), &dialog);
    auto *clientCertificate = new QLineEdit(&dialog);
    auto *clientKey = new QLineEdit(&dialog);
    auto *kubeNamespace = new QLineEdit(&dialog);
    kubeNamespace->setPlaceholderText(QStringLiteral("default"));
    form->addRow(QStringLiteral("API server:"), server);
    if (certificate) {
        form->addRow(QStringLiteral("Client certificate:"), pathRow(clientCertificate, &dialog));
        form->addRow(QStringLiteral("Client key:"), pathRow(clientKey, &dialog));
    } else {
        form->addRow(QStringLiteral("Token:"), token);
    }
    form->addRow(QStringLiteral("CA certificate:"), pathRow(ca, &dialog));
    form->addRow(QString(), insecure);
    form->addRow(QStringLiteral("Namespace:"), kubeNamespace);
    auto *note = new QLabel(certificate ? QStringLiteral("The certificate and key files stay where they are; the "
                                                         "credential points to them.")
                                        : QStringLiteral("The token is saved inside the credential."),
                            &dialog);
    note->setWordWrap(true);
    form->addRow(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (server->text().trimmed().isEmpty() || (!certificate && token->text().isEmpty()))
            QMessageBox::warning(&dialog, dialog.windowTitle(), QStringLiteral("Fill in the server and the %1.")
                                                                    .arg(certificate ? QStringLiteral("files") : QStringLiteral("token")));
        else
            dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    LocalSessionOptions options;
    options.kubeAuth = certificate ? QStringLiteral("certificate") : QStringLiteral("token");
    options.server = server->text().trimmed();
    options.insecure = insecure->isChecked();
    options.caFile = ca->text().trimmed();
    options.clientCertificate = clientCertificate->text().trimmed();
    options.clientKey = clientKey->text().trimmed();
    options.kubeNamespace = kubeNamespace->text().trimmed();
    auto host = QUrl(options.server).host();
    add(generatedKubeconfig(options, token->text()), host.isEmpty() ? QStringLiteral("cluster") : host);
}

void KubeCredentialsWindow::show()
{
    const auto name = selectedName();
    for (const auto &credential : loadKubeCredentials()) {
        if (credential.name != name)
            continue;
        const auto text = contents(credential, m_read);
        if (text.isEmpty()) {
            QMessageBox::warning(this, windowTitle(),
                                 credential.encrypted ? QStringLiteral("The credential could not be read (is the vault unlocked?).")
                                                      : QStringLiteral("settings/kube/%1 is missing.").arg(credential.file));
            return;
        }
        const auto summary = summarizeKubeconfig(text);
        m_details->setText(QStringLiteral("%1: contexts %2 (current: %3); servers %4.")
                               .arg(name, summary.contexts.join(QStringLiteral(", ")),
                                    summary.currentContext.isEmpty() ? QStringLiteral("none") : summary.currentContext,
                                    summary.servers.join(QStringLiteral(", "))));
        return;
    }
}

void KubeCredentialsWindow::remove()
{
    const auto name = selectedName();
    if (name.isEmpty()
        || QMessageBox::question(this, windowTitle(), QStringLiteral("Delete the credential %1? Sessions that use it "
                                                                     "will not log on until you choose another.")
                                                          .arg(name))
               != QMessageBox::Yes)
        return;
    auto credentials = loadKubeCredentials();
    for (int index = 0; index < credentials.size(); ++index) {
        if (credentials[index].name != name)
            continue;
        if (credentials[index].encrypted) {
            if (m_remove)
                m_remove(kubeCredentialKey(name));
        } else {
            QFile::remove(QDir(kubeCredentialsFolder()).filePath(credentials[index].file));
        }
        credentials.removeAt(index);
        break;
    }
    saveKubeCredentials(credentials);
    refresh();
    emit credentialsChanged();
}
