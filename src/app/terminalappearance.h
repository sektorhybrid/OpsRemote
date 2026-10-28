// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QFont>
#include <QString>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QSpinBox;
class QTermWidget;

// Colors and font of a terminal. Empty fields mean "not set": in the default
// appearance they select QTermWidget's built-in scheme and font; in a session
// override they inherit the default appearance.
struct TerminalAppearance
{
    QString colorScheme;
    QString fontFamily;
    int fontSize = 0;

    bool isEmpty() const { return colorScheme.isEmpty() && fontFamily.isEmpty() && fontSize == 0; }
    // Returns this appearance with every field set in override replacing it.
    TerminalAppearance overriddenBy(const TerminalAppearance &override) const;
    QFont font() const;
    bool operator==(const TerminalAppearance &) const = default;
};

// Points QTermWidget at the color schemes and keyboard layouts shipped in the
// portable data directory.
void registerTerminalResources(QTermWidget *terminal);
QStringList availableColorSchemes();
// Folder for color schemes the user imports; it lives with the other user
// state in settings/ next to the executable.
QString userColorSchemeDirectory();
// Validates a Konsole-format .colorscheme file, copies it into
// userColorSchemeDirectory(), and registers it. Returns the scheme's name, or
// an empty string with *error set.
QString importColorScheme(const QString &sourcePath, QString *error);
void applyTerminalAppearance(QTermWidget *terminal, const TerminalAppearance &appearance);

// Color scheme and font pickers with a live preview terminal. In override
// mode every control can be left at "Default", which inherits the given
// default appearance.
class TerminalAppearanceEditor final : public QWidget
{
public:
    TerminalAppearanceEditor(const TerminalAppearance &value, bool overrideMode,
                             const TerminalAppearance &defaults = {}, QWidget *parent = nullptr);

    TerminalAppearance value() const;
    // Called after every change the user makes, for applying changes live.
    std::function<void()> changed;

private:
    void updatePreview();

    bool m_overrideMode = false;
    TerminalAppearance m_defaults;
    QComboBox *m_schemeCombo = nullptr;
    QCheckBox *m_overrideFont = nullptr;
    QFontComboBox *m_fontCombo = nullptr;
    QSpinBox *m_sizeSpin = nullptr;
    QTermWidget *m_preview = nullptr;
};
