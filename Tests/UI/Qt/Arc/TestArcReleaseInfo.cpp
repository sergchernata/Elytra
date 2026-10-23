/*
 * Tests for the Arc fork's update notice: reading GitHub's latest release and comparing release tags.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/Arc/ReleaseInfo.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

using namespace Ladybird::Arc;

TEST_CASE(reads_the_latest_release)
{
    auto release = parse_latest_release(R"({
        "tag_name": "arc-v0.2.0",
        "html_url": "https://github.com/someone/ladybird/releases/tag/arc-v0.2.0",
        "draft": false,
        "prerelease": false,
        "assets": [
            { "name": "Ladybird-arc-v0.2.0-macOS-arm64.zip.sha256", "browser_download_url": "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Ladybird-arc-v0.2.0-macOS-arm64.zip.sha256" },
            { "name": "Ladybird-arc-v0.2.0-macOS-arm64.zip", "browser_download_url": "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Ladybird-arc-v0.2.0-macOS-arm64.zip" }
        ]
    })");
    VERIFY(release.has_value());
    EXPECT(release->tag == "arc-v0.2.0");
    EXPECT(release->page_url == "https://github.com/someone/ladybird/releases/tag/arc-v0.2.0");
    EXPECT(release->archive_name == "Ladybird-arc-v0.2.0-macOS-arm64.zip");
    EXPECT(release->archive_url == "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Ladybird-arc-v0.2.0-macOS-arm64.zip");
    EXPECT(release->checksum_url == "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Ladybird-arc-v0.2.0-macOS-arm64.zip.sha256");
    EXPECT(release->can_install());
}

TEST_CASE(a_release_without_a_verified_mac_download_only_offers_its_page)
{
    // No checksum: no installing.
    auto release = parse_latest_release(R"({
        "tag_name": "arc-v0.2.0", "html_url": "https://github.com/someone/ladybird/releases/tag/arc-v0.2.0",
        "assets": [ { "name": "Ladybird-arc-v0.2.0-macOS-arm64.zip", "browser_download_url": "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Ladybird-arc-v0.2.0-macOS-arm64.zip" } ]
    })");
    VERIFY(release.has_value());
    EXPECT(!release->can_install());

    // Downloads from anywhere but GitHub aren't used.
    release = parse_latest_release(R"({
        "tag_name": "arc-v0.2.0", "html_url": "https://github.com/someone/ladybird/releases/tag/arc-v0.2.0",
        "assets": [
            { "name": "Ladybird-arc-v0.2.0-macOS-arm64.zip", "browser_download_url": "https://evil.example/Ladybird.zip" },
            { "name": "Ladybird-arc-v0.2.0-macOS-arm64.zip.sha256", "browser_download_url": "https://evil.example/Ladybird.zip.sha256" }
        ]
    })");
    VERIFY(release.has_value());
    EXPECT(!release->can_install());
}

TEST_CASE(reads_the_published_checksum)
{
    auto hash = QString(64, 'a');
    EXPECT(checksum_for(QString("%1  Ladybird-arc-v0.2.0-macOS-arm64.zip\n").arg(hash).toUtf8(), "Ladybird-arc-v0.2.0-macOS-arm64.zip") == hash);
    EXPECT(checksum_for(QString("%1  Other.zip\n").arg(hash).toUtf8(), "Ladybird-arc-v0.2.0-macOS-arm64.zip").isEmpty());
    EXPECT(checksum_for("not a checksum  Ladybird.zip", "Ladybird.zip").isEmpty());
}

TEST_CASE(the_install_script_swaps_the_app_once_the_browser_has_quit)
{
    QTemporaryDir directory;
    auto make_app = [&](QString const& name, QByteArray const& version) {
        QDir(directory.path()).mkpath(name + "/Contents");
        QFile file(directory.filePath(name + "/Contents/version"));
        VERIFY(file.open(QIODevice::WriteOnly));
        file.write(version);
    };
    make_app("Ladybird.app", "old");
    make_app("staged/Ladybird.app", "new");
    auto marker = directory.filePath("relaunched");

    // Stands in for the running browser: the script waits for it to exit.
    QProcess browser;
    browser.start("/bin/sleep", { "0.5" });
    VERIFY(browser.waitForStarted());

    auto script = install_script(directory.filePath("Ladybird.app"), directory.filePath("staged/Ladybird.app"), browser.processId(),
        QString("/usr/bin/touch '%1'").arg(marker));
    QElapsedTimer waited;
    waited.start();
    QProcess install;
    install.start("/bin/sh", { "-c", script });
    VERIFY(install.waitForFinished(10000));
    EXPECT_EQ(install.exitCode(), 0);

    QFile installed(directory.filePath("Ladybird.app/Contents/version"));
    VERIFY(installed.open(QIODevice::ReadOnly));
    EXPECT(installed.readAll() == "new");
    EXPECT(QFile::exists(marker));                                 // Relaunched.
    EXPECT(!QFile::exists(directory.filePath("Ladybird.app.old"))); // And cleaned up.
    EXPECT(waited.elapsed() >= 400); // Only after the browser had quit.
}

TEST_CASE(ignores_drafts_prereleases_and_junk)
{
    EXPECT(!parse_latest_release(R"({ "tag_name": "arc-v0.3.0", "html_url": "https://x/", "draft": true })").has_value());
    EXPECT(!parse_latest_release(R"({ "tag_name": "arc-v0.3.0", "html_url": "https://x/", "prerelease": true })").has_value());
    EXPECT(!parse_latest_release(R"({ "message": "Not Found" })").has_value());
    EXPECT(!parse_latest_release("not json").has_value());
    // Only GitHub pages are opened.
    EXPECT(!parse_latest_release(R"json({ "tag_name": "arc-v0.3.0", "html_url": "javascript:alert(1)" })json").has_value());
}

TEST_CASE(compares_release_tags)
{
    EXPECT(is_newer_release("arc-v0.1.0", "arc-v0.2.0"));
    EXPECT(is_newer_release("arc-v0.1.0", "arc-v0.1.1"));
    EXPECT(is_newer_release("arc-v0.9.0", "arc-v0.10.0")); // Numerically, not as text.
    EXPECT(is_newer_release("arc-v1", "arc-v1.0.1"));
    EXPECT(!is_newer_release("arc-v0.2.0", "arc-v0.2.0"));
    EXPECT(!is_newer_release("arc-v0.2.0", "arc-v0.1.9"));

    EXPECT(is_newer_release("arc-2026.10.23-abc1234", "arc-2026.11.2-def5678"));
    EXPECT(!is_newer_release("arc-2026.10.23-abc1234", "arc-2026.10.23-def5678")); // Same day: can't tell.

    // The two tag styles never compare: a dated build made by hand mustn't outrank (and so block) real releases.
    EXPECT(!is_newer_release("arc-v0.1.0", "arc-2026.10.24-abc1234"));
    EXPECT(!is_newer_release("arc-2026.10.24-abc1234", "arc-v0.2.0"));

    // Unknown shapes are never newer, so a stray tag can't nag.
    EXPECT(!is_newer_release("arc-v0.1.0", "nightly"));
    EXPECT(!is_newer_release("", "arc-v0.2.0"));
    EXPECT(!is_newer_release("arc-v0.1.0", ""));
}
