/*
 * The fork's releases on GitHub.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/ReleaseInfo.h>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QUrl>

namespace Ladybird::Arc {

static bool is_github_download(QUrl const& url)
{
    // Release downloads are served from github.com (which redirects to its own storage).
    return url.scheme() == "https" && url.host() == "github.com" && url.path().contains("/releases/download/");
}

Optional<ReleaseInfo> parse_latest_release(QByteArray const& json)
{
    auto object = QJsonDocument::fromJson(json).object();
    if (object.isEmpty() || object.value("draft").toBool() || object.value("prerelease").toBool())
        return {};
    ReleaseInfo release;
    release.tag = object.value("tag_name").toString();
    auto page = QUrl(object.value("html_url").toString());
    // Only ever open a GitHub page.
    if (release.tag.isEmpty() || page.scheme() != "https" || page.host() != "github.com")
        return {};
    release.page_url = page.toString();

    QString archive_url;
    QString archive_name;
    QHash<QString, QString> urls_by_name;
    for (auto const& value : object.value("assets").toArray()) {
        auto asset = value.toObject();
        auto name = asset.value("name").toString();
        auto url = QUrl(asset.value("browser_download_url").toString());
        if (!is_github_download(url))
            continue;
        urls_by_name.insert(name, url.toString());
        if (name.endsWith("-macOS-arm64.zip")) {
            archive_name = name;
            archive_url = url.toString();
        }
    }
    // Only a download with its published checksum can be installed.
    if (auto checksum_url = urls_by_name.value(archive_name + ".sha256"); !archive_url.isEmpty() && !checksum_url.isEmpty()) {
        release.archive_name = archive_name;
        release.archive_url = archive_url;
        release.checksum_url = checksum_url;
    }
    return release;
}

// A tag's version numbers: arc-v1.2.3 -> {1, 2, 3}; arc-2026.10.23-abc -> {2026, 10, 23}. Empty if neither.
static QList<int> version_of(QString const& tag)
{
    static QRegularExpression const pattern(R"(^arc-v?(\d+(?:\.\d+)*)(?:-[0-9a-f]+)?$)");
    auto match = pattern.match(tag);
    if (!match.hasMatch())
        return {};
    QList<int> parts;
    for (auto const& part : match.captured(1).split('.'))
        parts.append(part.toInt());
    while (parts.size() < 3)
        parts.append(0);
    return parts;
}

QString checksum_for(QByteArray const& checksum_file, QString const& file_name)
{
    static QRegularExpression const line(R"(^([0-9a-fA-F]{64})\s+\*?(.+)$)");
    for (auto const& text : QString::fromUtf8(checksum_file).split('\n')) {
        auto match = line.match(text.trimmed());
        if (match.hasMatch() && match.captured(2).trimmed() == file_name)
            return match.captured(1).toLower();
    }
    return {};
}

static QString shell_quoted(QString const& text)
{
    auto quoted = text;
    quoted.replace("'", "'\\''");
    return "'" + quoted + "'";
}

QString install_script(QString const& installed_app, QString const& staged_app, qint64 browser_pid, QString const& relaunch_command)
{
    return QString(R"(
APP=%1
NEW=%2
# Wait for the browser to quit (a zombie, exited but not yet reaped, counts as quit).
while ps -p %3 -o stat= 2>/dev/null | grep -qv Z; do sleep 0.2; done
rm -rf "$APP.old"
mv "$APP" "$APP.old" || exit 1
if ! mv "$NEW" "$APP"; then
    mv "$APP.old" "$APP"
    exit 1
fi
rm -rf "$APP.old"
%4
)")
        .arg(shell_quoted(installed_app), shell_quoted(staged_app), QString::number(browser_pid), relaunch_command);
}

bool is_newer_release(QString const& current, QString const& candidate)
{
    auto current_version = version_of(current);
    auto candidate_version = version_of(candidate);
    if (current_version.isEmpty() || candidate_version.isEmpty())
        return false;
    return std::lexicographical_compare(current_version.begin(), current_version.end(), candidate_version.begin(), candidate_version.end());
}

}
