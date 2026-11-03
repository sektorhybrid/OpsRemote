// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "kvantumthemes.h"
#include "appdirs.h"

#include <Kvantum.h>

#include <QDir>
#include <QSaveFile>
#include <QSettings>

// Where the Kvantum sources look (see third_party/kvantum/opsremote-paths.h):
// <config home>/Kvantum/kvantum.kvconfig and user themes, <data>/Kvantum/.
QString opsremoteKvantumConfigHome()
{
    return portableFilePath(QStringLiteral("settings"));
}

QString opsremoteKvantumDataDir()
{
    return portableFilePath(QStringLiteral("data"));
}

// Kvantum's fallback theme (:/Kvantum/default.*) is in a static library, so
// its resources must be registered by hand, from the global namespace.
static void initKvantumResources()
{
    Q_INIT_RESOURCE(defaulttheme);
}

namespace {

QStringList themesIn(const QString &folder)
{
    QStringList themes;
    const QDir dir(QDir(folder).filePath(QStringLiteral("Kvantum")));
    for (const auto &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QFile::exists(dir.filePath(QStringLiteral("%1/%1.kvconfig").arg(name))))
            themes.append(name);
    }
    return themes;
}

}

QStringList KvantumThemes::available()
{
    QStringList themes = themesIn(opsremoteKvantumDataDir()) + themesIn(opsremoteKvantumConfigHome());
    themes.removeDuplicates();
    themes.sort(Qt::CaseInsensitive);
    return themes;
}

QList<QColor> KvantumThemes::colours(const QString &styleKey)
{
    const QString theme = styleKey.mid(Prefix.size());
    // The user's copy first, as Kvantum itself prefers it.
    for (const auto &folder : {opsremoteKvantumConfigHome(), opsremoteKvantumDataDir()}) {
        const QString file = QDir(folder).filePath(QStringLiteral("Kvantum/%1/%1.kvconfig").arg(theme));
        if (!QFile::exists(file))
            continue;
        QSettings config(file, QSettings::IniFormat);
        QList<QColor> result;
        for (const char *key : {"window.color", "base.color", "text.color", "highlight.color"}) {
            QString value = config.value(QStringLiteral("GeneralColors/") + QLatin1String(key)).toString().trimmed();
            // Kvantum writes #rrggbbaa; the preview shows the colour without its alpha.
            if (value.startsWith(QLatin1Char('#')) && value.size() == 9)
                value.chop(2);
            const QColor colour(value);
            if (!colour.isValid())
                return {};
            result.append(colour);
        }
        return result;
    }
    return {};
}

bool KvantumThemes::isKvantumKey(const QString &styleKey)
{
    return styleKey.startsWith(Prefix, Qt::CaseInsensitive);
}

QStyle *KvantumThemes::create(const QString &styleKey)
{
    const QString theme = styleKey.mid(Prefix.size());
    if (!available().contains(theme))
        return nullptr;
    initKvantumResources();
    // Kvantum reads its theme from kvantum.kvconfig when the style is made.
    const QString folder = QDir(opsremoteKvantumConfigHome()).filePath(QStringLiteral("Kvantum"));
    QDir().mkpath(folder);
    QSaveFile file(QDir(folder).filePath(QStringLiteral("kvantum.kvconfig")));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QStringLiteral("[General]\ntheme=%1\n").arg(theme).toUtf8());
        file.commit();
    }
    return new Kvantum::Style(false);
}
