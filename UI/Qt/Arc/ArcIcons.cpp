/*
 * Site icons for the Arc sidebar, kept per Ladybird profile.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/ArcIcons.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QHash>
#include <QPixmap>
#include <QUrl>

namespace Ladybird::Arc::ArcIcons {

static QString& directory()
{
    static QString path;
    return path;
}

static QHash<QString, QIcon>& memory()
{
    static QHash<QString, QIcon> cache;
    return cache;
}

void set_directory(QString const& path)
{
    directory() = path;
    QDir().mkpath(path);
}

static QString site_of(QString const& url)
{
    return QUrl(url).host().toLower();
}

static QString file_for_site(QString const& site)
{
    // Hashed so any host is a safe file name.
    auto name = QCryptographicHash::hash(site.toUtf8(), QCryptographicHash::Sha1).toHex();
    return directory() + "/" + QString::fromLatin1(name) + ".png";
}

bool has_icon(QString const& url)
{
    auto site = site_of(url);
    return !site.isEmpty() && !directory().isEmpty() && (memory().contains(site) || QFile::exists(file_for_site(site)));
}

QIcon icon_for_url(QString const& url)
{
    auto site = site_of(url);
    if (site.isEmpty() || directory().isEmpty())
        return {};
    if (auto it = memory().find(site); it != memory().end())
        return *it;
    QIcon icon;
    if (QPixmap pixmap(file_for_site(site)); !pixmap.isNull())
        icon = QIcon(pixmap);
    memory().insert(site, icon); // Also remembers misses, so the disk is checked once per site.
    return icon;
}

bool store_png(QString const& url, QByteArray const& png)
{
    auto site = site_of(url);
    if (site.isEmpty() || directory().isEmpty() || QFile::exists(file_for_site(site)))
        return false;
    QFile file(file_for_site(site));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(png);
    memory().remove(site);
    return true;
}

void store(QString const& url, QIcon const& icon)
{
    auto site = site_of(url);
    if (site.isEmpty() || icon.isNull() || directory().isEmpty())
        return;
    // Called for every row on every refresh: the same icon again changes nothing, and each site's file is looked for
    // once (hashing its name and checking the disk each time added up).
    if (auto known = memory().constFind(site); known != memory().constEnd() && known->cacheKey() == icon.cacheKey())
        return;
    memory().insert(site, icon);
    static QSet<QString> checked;
    auto file = file_for_site(site);
    if (checked.contains(file))
        return;
    checked.insert(file);
    if (QFile::exists(file))
        return;
    icon.pixmap(QSize(32, 32)).save(file, "PNG");
}

}
