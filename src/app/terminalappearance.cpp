// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "appdirs.h"
#include "terminalappearance.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFontInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>

#include <qtermwidget.h>

namespace {
// QTermWidget's own defaults, used when no font is configured.
constexpr char BuiltInFontFamily[] = "Monospace";
constexpr int BuiltInFontSize = 10;
constexpr int MinimumFontSize = 6;
constexpr int MaximumFontSize = 72;
constexpr qint64 MaximumColorSchemeFileSize = 64 * 1024;

QString resourceDirectory()
{
    return portableFilePath(QStringLiteral("data/qtermwidget"));
}

// Names of the schemes in one folder that QTermWidget actually loaded.
QStringList loadedSchemesIn(const QString &folder, const QStringList &loaded)
{
    QStringList names;
    const auto files = QDir(folder).entryInfoList({QStringLiteral("*.colorscheme")}, QDir::Files);
    for (const auto &file : files) {
        if (loaded.contains(file.completeBaseName()))
            names.append(file.completeBaseName());
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

QString schemeLabel(const QString &scheme)
{
    return scheme.isEmpty() ? QStringLiteral("Built-in") : scheme;
}

// Sample output covering prompt, directory listing colors, the ANSI palette,
// and bold text.
constexpr char PreviewText[] =
    "\x1b[2J\x1b[H"
    "\x1b[1;32muser@server\x1b[0m:\x1b[1;34m~\x1b[0m$ ls\r\n"
    "\x1b[1;34mdocs\x1b[0m  \x1b[1;32mbuild.sh\x1b[0m  notes.txt  \x1b[1;36mlatest\x1b[0m  "
    "\x1b[31marchive.tar.gz\x1b[0m\r\n"
    "\x1b[30m\x1b[47m black \x1b[0m \x1b[31mred\x1b[0m \x1b[32mgreen\x1b[0m \x1b[33myellow\x1b[0m "
    "\x1b[34mblue\x1b[0m \x1b[35mmagenta\x1b[0m \x1b[36mcyan\x1b[0m \x1b[37mwhite\x1b[0m\r\n"
    "\x1b[1mbold\x1b[0m normal \x1b[4munderline\x1b[0m \x1b[7mreverse\x1b[0m\r\n"
    "\x1b[1;32muser@server\x1b[0m:\x1b[1;34m~\x1b[0m$ ";
}

TerminalAppearance TerminalAppearance::overriddenBy(const TerminalAppearance &override) const
{
    TerminalAppearance result = *this;
    if (!override.colorScheme.isEmpty())
        result.colorScheme = override.colorScheme;
    if (!override.fontFamily.isEmpty())
        result.fontFamily = override.fontFamily;
    if (override.fontSize > 0)
        result.fontSize = override.fontSize;
    return result;
}

QFont TerminalAppearance::font() const
{
    QFont result = QApplication::font();
    result.setFamily(fontFamily.isEmpty() ? QString::fromLatin1(BuiltInFontFamily) : fontFamily);
    result.setPointSize(fontSize > 0 ? fontSize : BuiltInFontSize);
    result.setStyleHint(QFont::TypeWriter);
    return result;
}

void registerTerminalResources(QTermWidget *terminal)
{
    // QTermWidget scans these folders once, on first use, and skips folders
    // that do not exist yet. When names collide, the first folder wins.
    static const bool schemesAdded = [] {
        QTermWidget::addCustomColorSchemeDir(
            QDir(resourceDirectory()).filePath(QStringLiteral("color-schemes")));
        QTermWidget::addCustomColorSchemeDir(portableFilePath(QStringLiteral("data/color-schemes")));
        QDir().mkpath(userColorSchemeDirectory());
        QTermWidget::addCustomColorSchemeDir(userColorSchemeDirectory());
        return true;
    }();
    Q_UNUSED(schemesAdded);
    if (terminal) {
        terminal->setCustomKeyBindingsDir(
            QDir(resourceDirectory()).filePath(QStringLiteral("kb-layouts")));
        // QTermWidget's constructor already looked up the "default" key table
        // in its compiled-in directory, which a portable build does not have,
        // and fell back to a table that maps only Tab (so arrow keys, Home,
        // End, and function keys sent nothing). Load it again from data/.
        terminal->setKeyBindings(QStringLiteral("default"));
    }
}

QStringList availableColorSchemes()
{
    registerTerminalResources(nullptr);
    auto schemes = QTermWidget::availableColorSchemes();
    schemes.removeDuplicates();
    schemes.sort(Qt::CaseInsensitive);
    return schemes;
}

QString userColorSchemeDirectory()
{
    return portableFilePath(QStringLiteral("settings/color-schemes"));
}

QString importColorScheme(const QString &sourcePath, QString *error)
{
    const QFileInfo info(sourcePath);
    if (!info.isFile() || info.suffix() != QStringLiteral("colorscheme")) {
        *error = QStringLiteral("Choose a .colorscheme file.");
        return {};
    }
    if (info.size() > MaximumColorSchemeFileSize) {
        *error = QStringLiteral("The file is too large to be a color scheme.");
        return {};
    }
    // QTermWidget names a scheme after its file name up to the first dot.
    const auto name = info.completeBaseName();
    static const QRegularExpression allowedName(QStringLiteral(R"(^[\w \-+()&']{1,64}$)"));
    if (!allowedName.match(name).hasMatch()) {
        *error = QStringLiteral("\"%1\" is not a usable scheme name. Use letters, digits, spaces, "
                                "and - + ( ) & ' only (no dots).").arg(name);
        return {};
    }
    if (availableColorSchemes().contains(name, Qt::CaseInsensitive)) {
        *error = QStringLiteral("A color scheme named \"%1\" already exists.").arg(name);
        return {};
    }

    QSettings scheme(sourcePath, QSettings::IniFormat);
    if (scheme.status() != QSettings::NoError) {
        *error = QStringLiteral("The file is not a valid color scheme.");
        return {};
    }
    QStringList required{QStringLiteral("Background"), QStringLiteral("Foreground")};
    for (int index = 0; index < 8; ++index)
        required.append(QStringLiteral("Color%1").arg(index));
    for (const auto &group : std::as_const(required)) {
        // QSettings reads "r,g,b" as a list of three strings.
        const auto parts = scheme.value(group + QStringLiteral("/Color")).toStringList();
        bool valid = parts.size() == 3;
        for (const auto &part : parts) {
            bool isNumber = false;
            const int value = part.trimmed().toInt(&isNumber);
            valid = valid && isNumber && value >= 0 && value <= 255;
        }
        if (!valid) {
            *error = QStringLiteral("The file has no valid [%1] color.").arg(group);
            return {};
        }
    }

    const auto destination = QDir(userColorSchemeDirectory())
        .filePath(name + QStringLiteral(".colorscheme"));
    if (!QDir().mkpath(userColorSchemeDirectory()) || QFile::exists(destination)
        || !QFile::copy(sourcePath, destination)) {
        *error = QStringLiteral("Could not copy the scheme into %1.")
                     .arg(QDir::toNativeSeparators(userColorSchemeDirectory()));
        return {};
    }
    // Setting a scheme by file path registers it with QTermWidget's scheme
    // list, which was already scanned at startup.
    QTermWidget loader(0);
    loader.setColorScheme(destination);
    return name;
}

void applyTerminalAppearance(QTermWidget *terminal, const TerminalAppearance &appearance)
{
    // An empty or unknown name selects QTermWidget's built-in scheme.
    terminal->setColorScheme(appearance.colorScheme);
    terminal->setTerminalFont(appearance.font());
}

TerminalAppearanceEditor::TerminalAppearanceEditor(const TerminalAppearance &value,
                                                   bool overrideMode,
                                                   const TerminalAppearance &defaults,
                                                   QWidget *parent)
    : QWidget(parent), m_overrideMode(overrideMode), m_defaults(defaults)
{
    auto *form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);

    m_schemeCombo = new QComboBox(this);
    if (overrideMode)
        m_schemeCombo->addItem(QStringLiteral("Default (%1)").arg(schemeLabel(defaults.colorScheme)),
                               QString());
    else
        m_schemeCombo->addItem(schemeLabel(QString()), QString());
    // The schemes that ship with QTermWidget come first, then the curated
    // iTerm2-Color-Schemes set, then the user's imported schemes.
    const auto loaded = availableColorSchemes();
    QStringList listed;
    for (const auto &folder : {QDir(resourceDirectory()).filePath(QStringLiteral("color-schemes")),
                               portableFilePath(QStringLiteral("data/color-schemes")),
                               userColorSchemeDirectory()}) {
        QStringList group;
        for (const auto &name : loadedSchemesIn(folder, loaded)) {
            if (!listed.contains(name))
                group.append(name);
        }
        if (group.isEmpty())
            continue;
        m_schemeCombo->insertSeparator(m_schemeCombo->count());
        for (const auto &name : std::as_const(group))
            m_schemeCombo->addItem(name, name);
        listed += group;
    }
    m_schemeCombo->setCurrentIndex(qMax(0, m_schemeCombo->findData(value.colorScheme)));
    auto *schemeRow = new QHBoxLayout;
    schemeRow->addWidget(m_schemeCombo, 1);
    auto *importButton = new QPushButton(QStringLiteral("Import…"), this);
    importButton->setToolTip(QStringLiteral("Add a Konsole-format .colorscheme file; it is "
                                            "stored in the settings folder"));
    schemeRow->addWidget(importButton);
    form->addRow(QStringLiteral("Color scheme:"), schemeRow);
    connect(importButton, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import Color Scheme"), QDir::homePath(),
            QStringLiteral("Konsole color schemes (*.colorscheme)"));
        if (path.isEmpty())
            return;
        QString error;
        const auto name = importColorScheme(path, &error);
        if (name.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Could not import color scheme"), error);
            return;
        }
        m_schemeCombo->addItem(name, name);
        m_schemeCombo->setCurrentIndex(m_schemeCombo->count() - 1);
    });

    // In override mode the font is overridden as a whole (family and size).
    const bool fontOverridden = !value.fontFamily.isEmpty() || value.fontSize > 0;
    const auto shownFont = overrideMode && !fontOverridden ? defaults.font()
                                                           : defaults.overriddenBy(value).font();
    if (overrideMode) {
        const auto defaultFont = defaults.font();
        m_overrideFont = new QCheckBox(
            QStringLiteral("Use a different font than the default (%1, %2 pt)")
                .arg(QFontInfo(defaultFont).family()).arg(defaultFont.pointSize()),
            this);
        m_overrideFont->setChecked(fontOverridden);
        form->addRow(QString(), m_overrideFont);
    }
    auto *fontRow = new QHBoxLayout;
    m_fontCombo = new QFontComboBox(this);
    m_fontCombo->setFontFilters(QFontComboBox::MonospacedFonts);
    // Aliases such as "Monospace" are not listed; show the font they resolve
    // to, or the combo box would silently pick its first entry.
    m_fontCombo->setCurrentFont(QFont(QFontInfo(shownFont).family()));
    fontRow->addWidget(m_fontCombo, 1);
    m_sizeSpin = new QSpinBox(this);
    m_sizeSpin->setRange(MinimumFontSize, MaximumFontSize);
    m_sizeSpin->setSuffix(QStringLiteral(" pt"));
    m_sizeSpin->setValue(shownFont.pointSize());
    fontRow->addWidget(m_sizeSpin);
    form->addRow(QStringLiteral("Font:"), fontRow);

    m_preview = new QTermWidget(0, this);
    registerTerminalResources(m_preview);
    m_preview->setScrollBarPosition(QTermWidget::NoScrollBar);
    m_preview->setFixedHeight(130);
    m_preview->setFocusPolicy(Qt::NoFocus);
    for (auto *child : m_preview->findChildren<QWidget *>())
        child->setFocusPolicy(Qt::NoFocus);
    m_preview->startTerminalTeletype();
    form->addRow(QStringLiteral("Preview:"), m_preview);

    auto refresh = [this] {
        const bool fontEnabled = !m_overrideFont || m_overrideFont->isChecked();
        m_fontCombo->setEnabled(fontEnabled);
        m_sizeSpin->setEnabled(fontEnabled);
        updatePreview();
        if (changed)
            changed();
    };
    connect(m_schemeCombo, &QComboBox::currentIndexChanged, this, refresh);
    connect(m_fontCombo, &QFontComboBox::currentFontChanged, this, refresh);
    connect(m_sizeSpin, &QSpinBox::valueChanged, this, refresh);
    if (m_overrideFont)
        connect(m_overrideFont, &QCheckBox::toggled, this, [this, refresh](bool checked) {
            if (!checked) {
                const auto defaultFont = m_defaults.font();
                m_fontCombo->setCurrentFont(QFont(QFontInfo(defaultFont).family()));
                m_sizeSpin->setValue(defaultFont.pointSize());
            }
            refresh();
        });
    refresh();
}

TerminalAppearance TerminalAppearanceEditor::value() const
{
    TerminalAppearance result;
    result.colorScheme = m_schemeCombo->currentData().toString();
    if (!m_overrideFont || m_overrideFont->isChecked()) {
        result.fontFamily = m_fontCombo->currentFont().family();
        result.fontSize = m_sizeSpin->value();
    }
    // Keep the built-in default symbolic, so it follows the system's
    // monospace font instead of whatever that resolves to today.
    const auto builtIn = TerminalAppearance().font();
    if (!m_overrideMode && result.fontFamily == QFontInfo(builtIn).family()
        && result.fontSize == builtIn.pointSize()) {
        result.fontFamily.clear();
        result.fontSize = 0;
    }
    return result;
}

void TerminalAppearanceEditor::updatePreview()
{
    const auto shown = m_overrideMode ? m_defaults.overriddenBy(value()) : value();
    applyTerminalAppearance(m_preview, shown);
    m_preview->receiveData(PreviewText, static_cast<int>(sizeof PreviewText - 1));
}
