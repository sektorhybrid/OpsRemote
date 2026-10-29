// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "kvantumthemes.h"
#include "appdirs.h"

#include <Kvantum.h>

#include <QDir>
#include <QSaveFile>

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
