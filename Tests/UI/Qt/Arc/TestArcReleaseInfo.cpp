/*
 * Tests for the Arc fork's update notice: reading GitHub's latest release, comparing release tags and checking
 * release signatures.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/Arc/ReleaseInfo.h>
#include <UI/Qt/Arc/ReleaseSignature.h>

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
            { "name": "Ladybird-arc-v0.2.0-macOS-arm64.zip.sig", "browser_download_url": "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Ladybird-arc-v0.2.0-macOS-arm64.zip.sig" },
            { "name": "Ladybird-arc-v0.2.0-macOS-arm64.zip", "browser_download_url": "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Ladybird-arc-v0.2.0-macOS-arm64.zip" }
        ]
    })");
    VERIFY(release.has_value());
    EXPECT(release->tag == "arc-v0.2.0");
    EXPECT(release->page_url == "https://github.com/someone/ladybird/releases/tag/arc-v0.2.0");
    EXPECT(release->archive_name == "Ladybird-arc-v0.2.0-macOS-arm64.zip");
    EXPECT(release->archive_url == "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Ladybird-arc-v0.2.0-macOS-arm64.zip");
    EXPECT(release->signature_url == "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Ladybird-arc-v0.2.0-macOS-arm64.zip.sig");
    EXPECT(release->can_install());
}

TEST_CASE(a_release_without_a_verified_mac_download_only_offers_its_page)
{
    // No signature: no installing.
    auto release = parse_latest_release(R"({
        "tag_name": "arc-v0.2.0", "html_url": "https://github.com/someone/ladybird/releases/tag/arc-v0.2.0",
        "assets": [ { "name": "Ladybird-arc-v0.2.0-macOS-arm64.zip", "browser_download_url": "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Ladybird-arc-v0.2.0-macOS-arm64.zip" } ]
    })");
    VERIFY(release.has_value());
    EXPECT(!release->can_install());

    // A checksum alone isn't enough (it's published by the same release, so it proves nothing about who made it): the
    // releases from before signing, arc-v0.1.0 and arc-v0.1.1, look like this.
    release = parse_latest_release(R"({
        "tag_name": "arc-v0.1.1", "html_url": "https://github.com/someone/ladybird/releases/tag/arc-v0.1.1",
        "assets": [
            { "name": "Ladybird-arc-v0.1.1-macOS-arm64.zip", "browser_download_url": "https://github.com/someone/ladybird/releases/download/arc-v0.1.1/Ladybird-arc-v0.1.1-macOS-arm64.zip" },
            { "name": "Ladybird-arc-v0.1.1-macOS-arm64.zip.sha256", "browser_download_url": "https://github.com/someone/ladybird/releases/download/arc-v0.1.1/Ladybird-arc-v0.1.1-macOS-arm64.zip.sha256" }
        ]
    })");
    VERIFY(release.has_value());
    EXPECT(!release->can_install());

    // A signature for some other file doesn't count.
    release = parse_latest_release(R"({
        "tag_name": "arc-v0.2.0", "html_url": "https://github.com/someone/ladybird/releases/tag/arc-v0.2.0",
        "assets": [
            { "name": "Ladybird-arc-v0.2.0-macOS-arm64.zip", "browser_download_url": "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Ladybird-arc-v0.2.0-macOS-arm64.zip" },
            { "name": "Other.zip.sig", "browser_download_url": "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Other.zip.sig" }
        ]
    })");
    VERIFY(release.has_value());
    EXPECT(!release->can_install());

    // Downloads from anywhere but GitHub aren't used.
    release = parse_latest_release(R"({
        "tag_name": "arc-v0.2.0", "html_url": "https://github.com/someone/ladybird/releases/tag/arc-v0.2.0",
        "assets": [
            { "name": "Ladybird-arc-v0.2.0-macOS-arm64.zip", "browser_download_url": "https://evil.example/Ladybird.zip" },
            { "name": "Ladybird-arc-v0.2.0-macOS-arm64.zip.sig", "browser_download_url": "https://evil.example/Ladybird.zip.sig" }
        ]
    })");
    VERIFY(release.has_value());
    EXPECT(!release->can_install());
}

// Signed by the real release key (the one in the ARC_RELEASE_SIGNING_KEY secret), over this statement:
static QString const SIGNED_HASH = QString(64, 'a');
static QByteArray const REAL_SIGNATURE = "lateNvWyEhGlsBNWpvnbZ17ik6iyBpY4BiQP/f8x2fqgl6GJxHmH3hRcjZAJ5yRBGMl67DHzMqTD0HDi33eHDA==";
// The same statement signed by some other key, and that key.
static QByteArray const OTHER_SIGNATURE = "F26zjag/H8AaGr4/SzWisgDmAg0iWYBG1Z/iLDPnHk5rMds+0jbglys35yuB7Y8X2FvbfgkQP/f6F+aTASYXBA==";
static QByteArray const OTHER_PUBLIC_KEY = "S0AGTEiU0id8/lDWE6JBOztUTHru05VRmO/FL0R9qq4=";

static QByteArray signed_statement()
{
    return release_statement("arc-v0.2.0", "Ladybird-arc-v0.2.0-macOS-arm64.zip", SIGNED_HASH);
}

TEST_CASE(the_release_statement_matches_what_the_workflow_signs)
{
    // The workflow builds this with printf; any change here must change there too.
    EXPECT(signed_statement() == "ladybird-arc-release\ntag: arc-v0.2.0\nfile: Ladybird-arc-v0.2.0-macOS-arm64.zip\nsha256: " + SIGNED_HASH.toLatin1() + "\n");
    EXPECT(release_statement("arc-v0.2.0", "Ladybird-arc-v0.2.0-macOS-arm64.zip", SIGNED_HASH.toUpper()) == signed_statement());
}

TEST_CASE(a_release_signed_with_the_release_key_is_accepted)
{
    EXPECT(release_signature_is_valid(signed_statement(), REAL_SIGNATURE));
    EXPECT(release_signature_is_valid(signed_statement(), REAL_SIGNATURE + "\n")); // As written by base64.
}

TEST_CASE(a_signature_for_anything_else_is_refused)
{
    // A different file, hash or release than the one signed.
    EXPECT(!release_signature_is_valid(release_statement("arc-v9.0.0", "Ladybird-arc-v0.2.0-macOS-arm64.zip", SIGNED_HASH), REAL_SIGNATURE));
    EXPECT(!release_signature_is_valid(release_statement("arc-v0.2.0", "Ladybird-arc-v0.2.0-macOS-arm64.zip", QString(64, 'b')), REAL_SIGNATURE));
    EXPECT(!release_signature_is_valid(release_statement("arc-v0.2.0", "Other.zip", SIGNED_HASH), REAL_SIGNATURE));
    // Signed by another key.
    EXPECT(!release_signature_is_valid(signed_statement(), OTHER_SIGNATURE));
    EXPECT(release_signature_is_valid(signed_statement(), OTHER_SIGNATURE, OTHER_PUBLIC_KEY));
    EXPECT(!release_signature_is_valid(signed_statement(), REAL_SIGNATURE, OTHER_PUBLIC_KEY));
}

TEST_CASE(a_malformed_signature_is_refused)
{
    EXPECT(!release_signature_is_valid(signed_statement(), ""));
    EXPECT(!release_signature_is_valid(signed_statement(), "not base64 at all!"));
    EXPECT(!release_signature_is_valid(signed_statement(), REAL_SIGNATURE.left(40))); // Truncated.
    EXPECT(!release_signature_is_valid(signed_statement(), QByteArray(64, '\0').toBase64()));
    auto flipped = QByteArray::fromBase64(REAL_SIGNATURE);
    flipped[10] = static_cast<char>(flipped[10] ^ 1);
    EXPECT(!release_signature_is_valid(signed_statement(), flipped.toBase64()));
    // A malformed key.
    EXPECT(!release_signature_is_valid(signed_statement(), REAL_SIGNATURE, "short"));
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

TEST_CASE(follows_githubs_download_redirects_and_nothing_else)
{
    auto from = QString("https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Ladybird.zip");
    // GitHub serves release files from its asset storage.
    EXPECT(redirect_target(from, 302, "https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sig=x")
        == "https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sig=x");
    EXPECT(redirect_target(from, 302, "https://objects.githubusercontent.com/github-production-release-asset-2e65be/1/2").has_value());
    for (auto status : { 301, 303, 307, 308 })
        EXPECT(redirect_target(from, status, "https://objects.githubusercontent.com/x").has_value());

    // Not a redirect, nowhere to go, or somewhere else: not followed.
    EXPECT(!redirect_target(from, 200, "https://objects.githubusercontent.com/x").has_value());
    EXPECT(!redirect_target(from, 302, "").has_value());
    EXPECT(!redirect_target(from, 302, "https://evil.example/Ladybird.zip").has_value());
    EXPECT(!redirect_target(from, 302, "https://githubusercontent.com.evil.example/x").has_value());
    EXPECT(!redirect_target(from, 302, "http://objects.githubusercontent.com/x").has_value()); // Never downgraded.
    // Relative locations resolve against the request.
    EXPECT(redirect_target(from, 302, "/someone/ladybird/releases/download/arc-v0.2.0/Other.zip")
        == "https://github.com/someone/ladybird/releases/download/arc-v0.2.0/Other.zip");
}
