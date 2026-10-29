// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "appdirs.h"
#include "icons.h"

#include <QPainter>
#include <QPixmap>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QMimeDatabase>
#include <QSettings>

#include <algorithm>

namespace {
QString iconThemeDirectory()
{
    return portableFilePath(QStringLiteral("data/icons"));
}

// The bundled theme in use; empty means Qt's built-in icons. Tracked here
// because QIcon::setThemeName("") falls back to the desktop's default theme
// name (for example "Adwaita" on GNOME), which may match a bundled theme.
QString currentTheme;

QIcon styleIcon(QStyle::StandardPixmap pixmap)
{
    return QApplication::style()->standardIcon(pixmap);
}
}

void initializeIconThemes()
{
    // Only the bundled themes: a portable build must not depend on, or mix
    // in, whatever icon themes the host happens to have installed.
    QIcon::setThemeSearchPaths({iconThemeDirectory()});
    QIcon::setFallbackSearchPaths({});
    setIconTheme(QString());
}

QList<QPair<QString, QString>> availableIconThemes()
{
    QList<QPair<QString, QString>> themes;
    const auto folders = QDir(iconThemeDirectory()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &folder : folders) {
        const auto index = QDir(iconThemeDirectory()).filePath(folder + QStringLiteral("/index.theme"));
        if (!QFile::exists(index))
            continue;
        QSettings theme(index, QSettings::IniFormat);
        themes.append({folder, theme.value(QStringLiteral("Icon Theme/Name"), folder).toString()});
    }
    std::sort(themes.begin(), themes.end(), [](const auto &left, const auto &right) {
        return left.second.compare(right.second, Qt::CaseInsensitive) < 0;
    });
    return themes;
}

void setIconTheme(const QString &themeName)
{
    currentTheme = themeName;
    // A name that matches no folder keeps Qt from picking a theme by itself.
    QIcon::setThemeName(themeName.isEmpty() ? QStringLiteral("opsremote-built-in")
                                            : themeName);
}

QIcon themedIcon(const QString &name, QStyle::StandardPixmap fallback)
{
    if (!currentTheme.isEmpty() && QIcon::hasThemeIcon(name))
        return QIcon::fromTheme(name);
    return styleIcon(fallback);
}

QIcon fileTypeIcon(const QString &fileName, bool isDirectory, bool isSymlink)
{
    if (isDirectory)
        return themedIcon(QStringLiteral("folder"),
                          isSymlink ? QStyle::SP_DirLinkIcon : QStyle::SP_DirIcon);
    const auto fallback = isSymlink ? QStyle::SP_FileLinkIcon : QStyle::SP_FileIcon;
    if (currentTheme.isEmpty())
        return styleIcon(fallback);

    // Most specific first: the type's own icon (text-x-python), its generic
    // icon (text-x-script), then a plain document.
    static const QMimeDatabase database;
    const auto type = database.mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);
    for (const auto &name : {type.iconName(), type.genericIconName(),
                             QStringLiteral("text-x-generic"), QStringLiteral("unknown")}) {
        if (!name.isEmpty() && QIcon::hasThemeIcon(name))
            return QIcon::fromTheme(name);
    }
    return styleIcon(fallback);
}

QIcon fixedSizeIcon(const QIcon &icon, const QSize &size)
{
    QIcon fixed;
    for (const qreal scale : {1.0, 2.0}) {
        QPixmap tile(size * scale);
        tile.setDevicePixelRatio(scale);
        tile.fill(Qt::transparent);
        // Never scaled up (that blurs): a smaller icon sits in the middle.
        const QPixmap pixmap = icon.pixmap(size, scale);
        const QSizeF drawn = QSizeF(pixmap.size()) / pixmap.devicePixelRatio();
        QPainter painter(&tile);
        painter.drawPixmap(QPointF((size.width() - drawn.width()) / 2,
                                   (size.height() - drawn.height()) / 2),
                           pixmap);
        painter.end();
        fixed.addPixmap(tile);
    }
    return fixed;
}
