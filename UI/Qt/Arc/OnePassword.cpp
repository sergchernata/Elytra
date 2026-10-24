/*
 * Fill from 1Password.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibURL/Parser.h>
#include <UI/Qt/Arc/OnePassword.h>
#include <UI/Qt/StringUtils.h>

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <memory>

namespace Ladybird::Arc::OnePassword {

QList<Login> parse_logins(QByteArray const& json)
{
    QList<Login> logins;
    for (auto const& value : QJsonDocument::fromJson(json).array()) {
        auto item = value.toObject();
        Login login;
        login.id = item["id"].toString();
        login.title = item["title"].toString();
        login.username = item["additional_information"].toString();
        login.vault = item["vault"].toObject()["name"].toString();
        for (auto const& url : item["urls"].toArray()) {
            auto href = url.toObject()["href"].toString();
            if (!href.isEmpty())
                login.urls.append(href);
        }
        if (!login.id.isEmpty())
            logins.append(login);
    }
    return logins;
}

Optional<Credentials> parse_credentials(QByteArray const& json)
{
    auto item = QJsonDocument::fromJson(json).object();
    if (item.isEmpty())
        return {};
    Credentials credentials;
    bool found = false;
    for (auto const& value : item["fields"].toArray()) {
        auto field = value.toObject();
        auto purpose = field["purpose"].toString();
        if (purpose == "USERNAME" && credentials.username.isEmpty()) {
            credentials.username = field["value"].toString();
            found = true;
        } else if (purpose == "PASSWORD" && credentials.password.isEmpty()) {
            credentials.password = field["value"].toString();
            found = true;
        }
    }
    if (!found)
        return {};
    return credentials;
}

static Optional<URL::URL> parse_url(QString const& string)
{
    auto bytes = string.trimmed().toUtf8();
    return URL::Parser::basic_parse(StringView(bytes.constData(), static_cast<size_t>(bytes.size())));
}

static Optional<URL::URL> parse_saved_url(QString const& saved)
{
    auto trimmed = saved.trimmed();
    if (trimmed.isEmpty())
        return {};
    if (!trimmed.contains("://"))
        trimmed.prepend("https://");
    return parse_url(trimmed);
}

int match_strength(QString const& saved_url, QUrl const& page)
{
    auto saved = parse_saved_url(saved_url);
    auto current = parse_url(page.toString(QUrl::FullyEncoded));
    if (!saved.has_value() || !current.has_value() || !saved->host().has_value() || !current->host().has_value())
        return 0;
    // An app's saved address (android://, ssh://) isn't a website.
    if (!saved->scheme().is_one_of("http"sv, "https"sv))
        return 0;
    auto const& saved_host = *saved->host();
    auto const& page_host = *current->host();
    if (saved_host.serialize() == page_host.serialize())
        return 2;
    // IP addresses and hosts like "localhost" have no registrable domain: those match exactly or not at all.
    auto saved_site = saved_host.registrable_domain();
    auto page_site = page_host.registrable_domain();
    if (saved_site.has_value() && page_site.has_value() && *saved_site == *page_site)
        return 1;
    return 0;
}

QList<Login> logins_for(QList<Login> const& logins, QUrl const& page)
{
    QList<QPair<int, Login>> matches;
    for (auto const& login : logins) {
        int best = 0;
        for (auto const& url : login.urls)
            best = qMax(best, match_strength(url, page));
        if (best > 0)
            matches.append({ best, login });
    }
    std::stable_sort(matches.begin(), matches.end(), [](auto const& a, auto const& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second.title.localeAwareCompare(b.second.title) < 0;
    });
    QList<Login> result;
    for (auto const& match : matches)
        result.append(match.second);
    return result;
}

bool can_fill(QUrl const& page)
{
    if (page.scheme() == "https")
        return !page.host().isEmpty();
    if (page.scheme() != "http")
        return false;
    auto url = parse_url(page.toString(QUrl::FullyEncoded));
    return url.has_value() && url->host().has_value() && url->host()->is_loopback_or_localhost();
}

QString origin_of(QUrl const& page)
{
    auto url = parse_url(page.toString(QUrl::FullyEncoded));
    if (!url.has_value())
        return {};
    return qstring_from_ak_string(url->origin().serialize());
}

QString fill_script(QString const& origin, Credentials const& credentials)
{
    // The values go in as JSON string literals, which are JavaScript string literals too: nothing in them can end
    // the string early.
    QJsonArray values { origin, credentials.username, credentials.password };
    auto literals = QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact));

    // The page may have moved on since the login was chosen: fill only the origin it was chosen for.
    return QString(R"js((() => {
const [origin, username, password] = %1;
if (location.origin !== origin) return;
const shown = (e) => {
    if (e.disabled || e.readOnly) return false;
    const box = e.getBoundingClientRect();
    const style = getComputedStyle(e);
    return box.width > 0 && box.height > 0 && style.visibility !== "hidden" && style.display !== "none";
};
const inputs = Array.from(document.querySelectorAll("input")).filter(shown);
const passwords = inputs.filter((e) => e.type === "password");
const password_field = passwords.find((e) => /current-password/.test(e.autocomplete))
    || passwords.find((e) => !/new-password/.test(e.autocomplete))
    || passwords[0];
const texty = (e) => ["text", "email", "tel"].includes(e.type);
const before = (e) => !password_field || (e.compareDocumentPosition(password_field) & Node.DOCUMENT_POSITION_FOLLOWING);
const same_form = (e) => !password_field || e.form === password_field.form;
const candidates = inputs.filter((e) => texty(e) && before(e) && same_form(e));
const score = (e) => {
    const hint = `${e.name} ${e.id} ${e.autocomplete} ${e.placeholder}`;
    if (/\busername\b/.test(e.autocomplete)) return 4;
    if (e.type === "email") return 3;
    if (/user|e-?mail|login|account|identifier/i.test(hint)) return 2;
    return 1;
};
let username_field = null;
for (const e of candidates) {
    // The best hint wins; between equals, the one nearest the password field (the last before it).
    if (!username_field || score(e) >= score(username_field)) username_field = e;
}
const value_setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value").set;
const fill = (e, value) => {
    e.focus();
    value_setter.call(e, value);
    e.dispatchEvent(new Event("input", { bubbles: true }));
    e.dispatchEvent(new Event("change", { bubbles: true }));
};
if (username_field && username) fill(username_field, username);
if (password_field && password) fill(password_field, password);
})();)js")
        .arg(literals);
}

QString error_message(int exit_code, QString const& error_output)
{
    auto text = error_output.trimmed();
    auto lower = text.toLower();
    if (lower.contains("dismissed") || lower.contains("cancel"))
        return "1Password: cancelled";
    if (lower.contains("no accounts configured") || lower.contains("not currently signed in") || lower.contains("desktop app")
        || lower.contains("connect") || lower.contains("signin") || lower.contains("sign in"))
        return "Turn on “Integrate with 1Password CLI” in 1Password’s Developer settings";
    // "[ERROR] 2026/10/24 12:00:00 message": just the message.
    auto line = text.section('\n', -1).remove(QRegularExpression(R"(^\[ERROR\]\s+\S+\s+\S+\s+)"));
    if (line.isEmpty())
        return QString("1Password couldn’t be reached (exit %1)").arg(exit_code);
    return QString("1Password: %1").arg(line.left(120));
}

QString find_op()
{
    if (auto path = qEnvironmentVariable("ARC_OP_PATH"); !path.isEmpty())
        return QFileInfo(path).isExecutable() ? path : QString();
    for (auto const* path : { "/opt/homebrew/bin/op", "/usr/local/bin/op" }) {
        if (QFileInfo(path).isExecutable())
            return path;
    }
    return QStandardPaths::findExecutable("op");
}

Runner::Runner(QObject* parent)
    : QObject(parent)
{
}

void Runner::run(QStringList arguments, Function<void(QByteArray)> done, Function<void(QString)> failed)
{
    auto op = find_op();
    if (op.isEmpty()) {
        failed("Install 1Password’s command-line tool: brew install 1password-cli");
        return;
    }
    auto* process = new QProcess(this);
    process->setProgram(op);
    process->setArguments(arguments);
    process->setProcessChannelMode(QProcess::SeparateChannels);

    // Waiting on Touch ID can take a while, but not forever.
    auto* timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, [process] { process->kill(); });
    timeout->start(120'000);

    auto on_done = std::make_shared<Function<void(QByteArray)>>(AK::move(done));
    auto on_failed = std::make_shared<Function<void(QString)>>(AK::move(failed));
    connect(process, &QProcess::finished, this, [process, on_done, on_failed](int exit_code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (status != QProcess::NormalExit || exit_code != 0) {
            (*on_failed)(error_message(exit_code, QString::fromUtf8(process->readAllStandardError())));
            return;
        }
        (*on_done)(process->readAllStandardOutput());
    });
    connect(process, &QProcess::errorOccurred, this, [process, on_failed](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        (*on_failed)("1Password’s command-line tool couldn’t be started");
    });
    process->start();
}

void Runner::list_logins(Function<void(QList<Login>)> done, Function<void(QString)> failed)
{
    run({ "item", "list", "--categories", "Login", "--format", "json" },
        [done = AK::move(done)](QByteArray output) { done(parse_logins(output)); },
        AK::move(failed));
}

void Runner::get_credentials(QString const& id, Function<void(Credentials)> done, Function<void(QString)> failed)
{
    auto on_failed = std::make_shared<Function<void(QString)>>(AK::move(failed));
    run({ "item", "get", id, "--reveal", "--format", "json" },
        [done = AK::move(done), on_failed](QByteArray output) {
            auto credentials = parse_credentials(output);
            // The output held the password: don't keep it around.
            output.fill('\0');
            if (!credentials.has_value()) {
                (*on_failed)("That 1Password item has no username or password");
                return;
            }
            done(credentials.release_value());
        },
        [on_failed](QString message) { (*on_failed)(AK::move(message)); });
}

}
