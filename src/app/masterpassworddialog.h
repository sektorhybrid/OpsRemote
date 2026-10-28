// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QDialog>
#include <QString>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QProgressBar;

// How hard a password is to guess, from a rough entropy estimate in bits.
struct PasswordStrength
{
    int bits = 0;
    int level = 0; // 0 very weak … 4 very strong
    QString label;
    QString hint;
};

PasswordStrength estimatePasswordStrength(const QString &password);

// Asks for a new master password (twice), showing a strength bar. For
// changing it, the current password is asked as well.
class MasterPasswordDialog final : public QDialog
{
    Q_OBJECT

public:
    enum class Mode { Create, Change };

    explicit MasterPasswordDialog(Mode mode, QWidget *parent = nullptr);
    ~MasterPasswordDialog() override;

    QString currentPassword() const;
    QString newPassword() const;

private:
    void updateState();

    Mode m_mode;
    QLineEdit *m_current = nullptr;
    QLineEdit *m_new = nullptr;
    QLineEdit *m_repeat = nullptr;
    QProgressBar *m_bar = nullptr;
    QLabel *m_strength = nullptr;
    QLabel *m_problem = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};
