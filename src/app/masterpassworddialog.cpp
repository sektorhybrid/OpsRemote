// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "masterpassworddialog.h"
#include "credentialvault.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

#include <cmath>

PasswordStrength estimatePasswordStrength(const QString &password)
{
    PasswordStrength strength;
    if (password.isEmpty()) {
        strength.label = QStringLiteral("Type a password");
        return strength;
    }

    // Size of the alphabet the characters come from.
    bool lower = false, upper = false, digit = false, symbol = false, other = false;
    for (const QChar c : password) {
        if (c.unicode() > 127)
            other = true;
        else if (c.isLower())
            lower = true;
        else if (c.isUpper())
            upper = true;
        else if (c.isDigit())
            digit = true;
        else
            symbol = true;
    }
    const int pool = (lower ? 26 : 0) + (upper ? 26 : 0) + (digit ? 10 : 0) + (symbol ? 33 : 0)
                     + (other ? 100 : 0);

    // Characters that add little: repeats ("aaa") and runs ("abc", "321")
    // count for a third.
    double effective = 0;
    for (qsizetype index = 0; index < password.size(); ++index) {
        bool predictable = false;
        if (index >= 1) {
            const int step = password[index].unicode() - password[index - 1].unicode();
            const int previous = index >= 2 ? password[index - 1].unicode() - password[index - 2].unicode()
                                            : 99;
            predictable = step == 0 || (qAbs(step) == 1 && step == previous);
        }
        effective += predictable ? 1.0 / 3 : 1.0;
    }
    double bits = effective * std::log2(qMax(pool, 2));

    // Few distinct characters ("abababab") are easy too.
    const auto distinct = QSet<QChar>(password.begin(), password.end()).size();
    if (distinct < password.size() / 2)
        bits *= 0.6;

    // Common passwords and keyboard walks inside the password.
    static const char *const common[] = {
        "password", "passwort", "qwerty", "azerty", "123456", "letmein", "welcome", "admin",
        "iloveyou", "master", "dragon", "monkey", "abc123", "football", "sunshine", "princess",
        "secret", "asdf", "zxcv", "111111", "000000", "trustno1"};
    const QString folded = password.toLower();
    for (const char *word : common) {
        if (folded.contains(QLatin1String(word))) {
            bits -= 4.0 * static_cast<double>(qstrlen(word));
            strength.hint = QStringLiteral("Contains a very common password (\"%1\").")
                                .arg(QLatin1String(word));
        }
    }

    strength.bits = qMax(0, static_cast<int>(std::lround(bits)));
    strength.level = strength.bits < 30 ? 0 : strength.bits < 45 ? 1 : strength.bits < 60 ? 2
                     : strength.bits < 80 ? 3 : 4;
    static const char *const labels[] = {"Very weak", "Weak", "Fair", "Strong", "Very strong"};
    strength.label = QString::fromLatin1(labels[strength.level]);
    if (strength.hint.isEmpty() && strength.level < 3) {
        if (password.size() < 12)
            strength.hint = QStringLiteral("Longer is the easiest way to make it stronger, for "
                                           "example a few unrelated words.");
        else if (pool < 60)
            strength.hint = QStringLiteral("Mix upper and lower case, digits, and symbols.");
    }
    return strength;
}

MasterPasswordDialog::MasterPasswordDialog(Mode mode, QWidget *parent)
    : QDialog(parent), m_mode(mode)
{
    setWindowTitle(mode == Mode::Create ? QStringLiteral("Create Credential Vault")
                                        : QStringLiteral("Change Master Password"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        mode == Mode::Create
            ? QStringLiteral("The vault keeps session passwords encrypted with this master "
                             "password. It cannot be recovered if you forget it.")
            : QStringLiteral("The vault is re-encrypted with the new password. Copies of the "
                             "settings folder made earlier keep the old one."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    const auto passwordEdit = [this] {
        auto *edit = new QLineEdit(this);
        edit->setEchoMode(QLineEdit::Password);
        connect(edit, &QLineEdit::textChanged, this, &MasterPasswordDialog::updateState);
        return edit;
    };
    if (mode == Mode::Change) {
        m_current = passwordEdit();
        form->addRow(QStringLiteral("Current password:"), m_current);
    }
    m_new = passwordEdit();
    m_new->setPlaceholderText(QStringLiteral("At least %1 characters")
                                  .arg(CredentialVault::MinimumPasswordLength));
    form->addRow(mode == Mode::Create ? QStringLiteral("Master password:")
                                      : QStringLiteral("New password:"),
                 m_new);

    // The strength bar sits right under the new password.
    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 100);
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(8);
    m_strength = new QLabel(this);
    auto *meter = new QVBoxLayout;
    meter->setSpacing(2);
    meter->addWidget(m_bar);
    meter->addWidget(m_strength);
    form->addRow(QString(), meter);

    m_repeat = passwordEdit();
    form->addRow(QStringLiteral("Repeat:"), m_repeat);
    auto *show = new QCheckBox(QStringLiteral("Show passwords"), this);
    connect(show, &QCheckBox::toggled, this, [this](bool on) {
        for (auto *edit : {m_current, m_new, m_repeat}) {
            if (edit)
                edit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        }
    });
    form->addRow(QString(), show);
    layout->addLayout(form);

    m_problem = new QLabel(this);
    m_problem->setWordWrap(true);
    m_problem->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    layout->addWidget(m_problem);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)
        ->setText(mode == Mode::Create ? QStringLiteral("Create") : QStringLiteral("Change"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);
    setMinimumWidth(420);
    updateState();
}

MasterPasswordDialog::~MasterPasswordDialog()
{
    for (auto *edit : {m_current, m_new, m_repeat}) {
        if (edit)
            edit->clear();
    }
}

QString MasterPasswordDialog::currentPassword() const
{
    return m_current ? m_current->text() : QString();
}

QString MasterPasswordDialog::newPassword() const
{
    return m_new->text();
}

void MasterPasswordDialog::updateState()
{
    const auto password = m_new->text();
    const auto strength = estimatePasswordStrength(password);
    // Red to green through orange and yellow.
    static const char *const colours[] = {"#d9534f", "#f0883e", "#e3b341", "#7cbf4a", "#2ea043"};
    m_bar->setValue(password.isEmpty() ? 0 : qBound(8, strength.bits, 100));
    // The empty track: a light tint of the text colour over the window, so it
    // shows in every colour theme.
    const auto window = palette().color(QPalette::Window);
    const auto text = palette().color(QPalette::Text);
    const auto track = QColor::fromRgbF(window.redF() + (text.redF() - window.redF()) * 0.15,
                                        window.greenF() + (text.greenF() - window.greenF()) * 0.15,
                                        window.blueF() + (text.blueF() - window.blueF()) * 0.15);
    m_bar->setStyleSheet(QStringLiteral("QProgressBar { border: none; border-radius: 4px;"
                                        " background: %1; }"
                                        "QProgressBar::chunk { border-radius: 4px; background: %2; }")
                             .arg(track.name(), QLatin1String(colours[strength.level])));
    m_strength->setText(password.isEmpty()
                            ? QString()
                            : QStringLiteral("<b>%1</b>%2").arg(
                                  strength.label,
                                  strength.hint.isEmpty() ? QString()
                                                          : QStringLiteral(" · ") + strength.hint.toHtmlEscaped()));
    m_strength->setWordWrap(true);

    QString problem;
    if (m_current && m_current->text().isEmpty())
        problem = QStringLiteral("Enter the current password.");
    else if (password.size() < CredentialVault::MinimumPasswordLength)
        problem = QStringLiteral("The password needs at least %1 characters.")
                      .arg(CredentialVault::MinimumPasswordLength);
    else if (m_repeat->text() != password)
        problem = m_repeat->text().isEmpty() ? QStringLiteral("Repeat the password.")
                                             : QStringLiteral("The passwords do not match.");
    m_problem->setText(problem);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(problem.isEmpty());
}
