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
    // Only a signed download can be installed (older, unsigned releases only get their page offered).
    if (auto signature_url = urls_by_name.value(archive_name + ".sig"); !archive_url.isEmpty() && !signature_url.isEmpty()) {
        release.archive_name = archive_name;
        release.archive_url = archive_url;
        release.signature_url = signature_url;
    }
    return release;
}

// A tag's style and version numbers: arc-v1.2.3 -> {v, {1, 2, 3}}; arc-2026.10.23-abc -> {date, {2026, 10, 23}}.
struct TagVersion {
    bool dated { false };
    QList<int> parts;
};

static Optional<TagVersion> version_of(QString const& tag)
{
    static QRegularExpression const versioned(R"(^arc-v(\d+(?:\.\d+)*)$)");
    static QRegularExpression const dated(R"(^arc-(\d{4}\.\d{1,2}\.\d{1,2})-[0-9a-f]+$)");
    TagVersion version;
    auto match = versioned.match(tag);
    if (!match.hasMatch()) {
        match = dated.match(tag);
        if (!match.hasMatch())
            return {};
        version.dated = true;
    }
    for (auto const& part : match.captured(1).split('.'))
        version.parts.append(part.toInt());
    while (version.parts.size() < 3)
        version.parts.append(0);
    return version;
}

bool is_newer_release(QString const& current, QString const& candidate)
{
    auto current_version = version_of(current);
    auto candidate_version = version_of(candidate);
    // The two styles never compare, so a dated build made by hand can't outrank, and then block, real releases.
    if (!current_version.has_value() || !candidate_version.has_value() || current_version->dated != candidate_version->dated)
        return false;
    return std::lexicographical_compare(current_version->parts.begin(), current_version->parts.end(), candidate_version->parts.begin(), candidate_version->parts.end());
}

Optional<QString> redirect_target(QString const& from, int status, QString const& location)
{
    if ((status != 301 && status != 302 && status != 303 && status != 307 && status != 308) || location.isEmpty())
        return {};
    auto target = QUrl(from).resolved(QUrl(location));
    auto host = target.host().toLower();
    auto is_github = host == "github.com" || host.endsWith(".githubusercontent.com");
    if (target.scheme() != "https" || !is_github)
        return {};
    return target.toString(QUrl::FullyEncoded);
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


}
