// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QWidget>

class QComboBox;
class QLineEdit;
class StatusLine;

// A program on this computer with its arguments, for fields like the editor
// of remote files. The same in every window: a list of the programs found
// (Linux: installed applications; Windows: known and registered ones), a
// Browse… button for any executable, the editable command line, and a line
// under it saying whether the program is there. Empty is allowed and means
// whatever placeholder says.
class ProgramPicker final : public QWidget
{
    Q_OBJECT

public:
    // Which programs the list offers.
    enum class Kind { TextEditor };

    ProgramPicker(Kind kind, const QString &value, const QString &placeholder, QWidget *parent = nullptr);

    QString value() const; // the command line, trimmed

signals:
    // After typing (editing finished), choosing from the list, or browsing.
    void changed();

private:
    void check();

    QComboBox *m_list = nullptr;
    QLineEdit *m_command = nullptr;
    StatusLine *m_status = nullptr;
};
