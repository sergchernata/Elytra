/*
 * Arc-style updates.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Arc/UpdateChecker.h>

#include <QAction>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QSettings>
#include <QThread>
#include <QTimer>

namespace Ladybird::Arc {

static constexpr int FIRST_CHECK_DELAY_MS = 15 * 1000;
static constexpr int CHECK_INTERVAL_MS = 6 * 60 * 60 * 1000;

UpdateChecker& UpdateChecker::the()
{
    static auto* checker = new UpdateChecker;
    return *checker;
}

void UpdateChecker::start()
{
    if (m_timer)
        return;
#if defined(ARC_RELEASE_TAG) && defined(ARC_RELEASE_REPOSITORY)
    m_current_tag = ARC_RELEASE_TAG;
    m_repository = ARC_RELEASE_REPOSITORY;
#endif
    if (auto tag = qEnvironmentVariable("ARC_DEV_RELEASE_TAG"); !tag.isEmpty())
        m_current_tag = tag;
    if (auto repository = qEnvironmentVariable("ARC_DEV_RELEASE_REPOSITORY"); !repository.isEmpty())
        m_repository = repository;
    if (m_current_tag.isEmpty() || m_repository.isEmpty())
        return; // Not a release build.

    m_timer = new QTimer(this);
    m_timer->setInterval(CHECK_INTERVAL_MS);
    connect(m_timer, &QTimer::timeout, this, &UpdateChecker::check);
    m_timer->start();
    QTimer::singleShot(FIRST_CHECK_DELAY_MS, this, &UpdateChecker::check);
}

void UpdateChecker::set_state(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit state_changed();
}

// GETs a URL with Ladybird's own network stack, without cookies, following redirects (GitHub's downloads redirect
// to its storage). Calls `done` with the body of a 200 response, or nothing.
void UpdateChecker::fetch(QString const& address, Function<void(Optional<QByteArray>)> done, int redirects_left)
{
    auto utf8 = address.toUtf8();
    auto url = URL::Parser::basic_parse(StringView { utf8.constData(), static_cast<size_t>(utf8.size()) });
    auto client = WebView::Application::request_server_client();
    if (!url.has_value() || client.is_error()) {
        done({});
        return;
    }
    auto headers = HTTP::HeaderList::create();
    headers->set({ "Accept"sv, "application/vnd.github+json, application/octet-stream"sv });
    headers->set({ "User-Agent"sv, "Ladybird-Arc"sv });
    m_request = client.value()->start_request("GET"sv, *url, *headers, {}, HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No);
    if (!m_request) {
        done({});
        return;
    }
    m_request->set_buffered_request_finished_callback(
        [this, address, redirects_left, done = move(done)](auto, auto const&, auto const& network_error, auto response_headers, auto response_code, auto const&, auto, auto, auto, auto payload) mutable {
            // Let go of this request once its callback has returned (not of one started since).
            Core::deferred_invoke([this, finished = m_request] {
                if (m_request == finished)
                    m_request = nullptr;
            });
            if (network_error.has_value() || !response_code.has_value()) {
                done({});
                return;
            }
            // The network process doesn't follow redirects (the page engine does, for pages); GitHub's release
            // downloads redirect to its asset storage. Follow those, and only those, a few times.
            auto location = response_headers->get("Location"sv);
            auto next = redirect_target(address, static_cast<int>(*response_code), location.has_value() ? QString::fromUtf8(location->characters(), static_cast<qsizetype>(location->length())) : QString());
            if (next.has_value()) {
                if (redirects_left <= 0) {
                    done({});
                    return;
                }
                auto target = next.release_value();
                Core::deferred_invoke([this, target, redirects_left, done = move(done)]() mutable { fetch(target, move(done), redirects_left - 1); });
                return;
            }
            if (*response_code != 200) {
                done({});
                return;
            }
            auto bytes = payload.bytes();
            done(QByteArray(reinterpret_cast<char const*>(bytes.data()), static_cast<qsizetype>(bytes.size())));
        });
}

void UpdateChecker::check()
{
    // Keep looking after a release was staged (a newer one supersedes it) or couldn't be prepared (a fixed one may
    // follow); only not while one is being prepared.
    if (m_request || m_state == State::Preparing)
        return;
    fetch(QString("https://api.github.com/repos/%1/releases/latest").arg(m_repository), [this](Optional<QByteArray> body) {
        if (!body.has_value())
            return;
        auto release = parse_latest_release(*body);
        if (!release.has_value() || !is_newer_release(m_current_tag, release->tag))
            return;
        if (m_release.has_value() && !is_newer_release(m_release->tag, release->tag) && m_state != State::PageOnly)
            return; // Already have it.
        found(release.release_value());
    });
}

// The Ladybird.app this process runs from, when it can be replaced (its folder is writable).
QString UpdateChecker::installed_app_path() const
{
    QDir bundle(QCoreApplication::applicationDirPath()); // .../Ladybird.app/Contents/MacOS
    if (!bundle.cdUp() || !bundle.cdUp() || !bundle.path().endsWith(".app"))
        return {};
    if (!QFileInfo(QFileInfo(bundle.path()).path()).isWritable())
        return {};
    return bundle.path();
}

QString UpdateChecker::staging_directory() const
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/ArcUpdate";
}

void UpdateChecker::found(ReleaseInfo release)
{
    m_release = move(release);
    if (!m_release->can_install() || installed_app_path().isEmpty()) {
        set_state(State::PageOnly);
        return;
    }
    set_state(State::Preparing);
    prepare();
}

void UpdateChecker::fail_preparing(QString const& why)
{
    warnln("Arc update: {}; offering the release page instead", why.toUtf8().constData());
    set_state(State::PageOnly);
}

// In the background: download the release and its checksum, check them, unpack the app and check it too.
void UpdateChecker::prepare()
{
    fetch(m_release->checksum_url, [this](Optional<QByteArray> checksum_file) {
        if (!checksum_file.has_value())
            return fail_preparing("couldn't download the checksum");
        auto expected = checksum_for(*checksum_file, m_release->archive_name);
        if (expected.isEmpty())
            return fail_preparing("the checksum file has no checksum for the download");

        fetch(m_release->archive_url, [this, expected](Optional<QByteArray> archive) {
            if (!archive.has_value())
                return fail_preparing("couldn't download the release");
            auto directory = QDir(staging_directory() + "/" + m_release->tag);
            auto zip_path = directory.filePath(m_release->archive_name);

            // Hashing and saving ~100 MB would stall the UI: do it on a thread, then carry on here.
            auto* worker = QThread::create([archive = archive.release_value(), expected, directory, zip_path, this] {
                QString error;
                if (QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex() != expected.toLatin1()) {
                    error = "the download doesn't match its checksum";
                } else {
                    QDir(directory).removeRecursively();
                    QFile zip(zip_path);
                    if (!QDir().mkpath(directory.path()) || !zip.open(QIODevice::WriteOnly) || zip.write(archive) != archive.size())
                        error = "couldn't save the download";
                }
                QMetaObject::invokeMethod(this, [this, error, directory, zip_path] {
                    if (!error.isEmpty())
                        return fail_preparing(error);
                    unpack(directory.path(), zip_path);
                });
            });
            connect(worker, &QThread::finished, worker, &QObject::deleteLater);
            worker->start();
        });
    });
}

void UpdateChecker::unpack(QString const& directory_path, QString const& zip_path)
{
    QDir directory(directory_path);
    // Unpack and check the app, off the main thread's critical path: the processes run on their own.
    auto* unpack = new QProcess(this);
    connect(unpack, &QProcess::finished, this, [this, unpack, directory](int exit_code) {
        unpack->deleteLater();
        auto staged = directory.filePath("Ladybird.app");
        if (exit_code != 0 || !QFileInfo::exists(staged))
            return fail_preparing("couldn't unpack the release");

        auto* verify = new QProcess(this);
        connect(verify, &QProcess::finished, this, [this, verify, staged](int verify_exit_code) {
            verify->deleteLater();
            if (verify_exit_code != 0)
                return fail_preparing("the release's app isn't validly signed");
            // The same app: its bundle identifier is ours (read from the Info.plist files, no processes).
            auto read_identifier = [](QString const& app) {
                QSettings plist(app + "/Contents/Info.plist", QSettings::NativeFormat);
                return plist.value("CFBundleIdentifier").toString();
            };
            auto identifier = read_identifier(staged);
            if (identifier.isEmpty() || identifier != read_identifier(installed_app_path()))
                return fail_preparing("the release's app isn't this app");
            m_staged_app = staged;
            set_state(State::Ready);
        });
        verify->start("/usr/bin/codesign", { "--verify", "--deep", staged });
    });
    unpack->start("/usr/bin/ditto", { "-x", "-k", zip_path, directory.path() });
}

void UpdateChecker::restart_into_update()
{
    if (m_state != State::Ready)
        return;
    auto installed = installed_app_path();
    if (installed.isEmpty())
        return fail_preparing("the app's folder isn't writable");

    // The swap starts only once the quit goes ahead (a page or an active download can cancel it), and waits for this
    // process to exit before touching the app.
    // Once, however often this is asked: two scripts would race on the same app.
    if (!m_install_armed) {
        m_install_armed = true;
        auto script = install_script(installed, m_staged_app, QCoreApplication::applicationPid(),
            QString("/usr/bin/open '%1'").arg(QString(installed).replace("'", "'\\''")));
        // If a page or a download cancels this quit, the update installs at the next one instead (as in Arc).
        connect(qApp, &QCoreApplication::aboutToQuit, this, [script] { QProcess::startDetached("/bin/sh", { "-c", script }); }, Qt::SingleShotConnection);
    }
    Application::the().quit_action()->trigger();
}

}
