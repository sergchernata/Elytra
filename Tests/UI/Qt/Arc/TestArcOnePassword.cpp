/*
 * Tests for Fill from 1Password: reading `op`'s output, which logins belong on a page, the fill script, and running
 * `op` (a stand-in script here; the real vault is never touched).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/Arc/OnePassword.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

using namespace Ladybird::Arc::OnePassword;

// As `op item list --categories Login --format json` prints it.
static QByteArray const LIST = R"json([
  {"id":"aaa","title":"Example","version":3,"vault":{"id":"v1","name":"Personal"},"category":"LOGIN",
   "additional_information":"me@example.com","urls":[{"label":"website","primary":true,"href":"https://example.com/login"}]},
  {"id":"bbb","title":"Example (accounts)","vault":{"id":"v1","name":"Personal"},"category":"LOGIN",
   "additional_information":"work@example.com","urls":[{"primary":true,"href":"accounts.example.com"}]},
  {"id":"ccc","title":"Elsewhere","vault":{"id":"v2","name":"Work"},"category":"LOGIN","urls":[{"href":"https://other.org"}]},
  {"id":"ddd","title":"No website","vault":{"id":"v2","name":"Work"},"category":"LOGIN"}
])json";

// As `op item get <id> --reveal --format json` prints it: the labels are the user's to rename, the purposes aren't.
static QByteArray const ITEM = R"json({"id":"aaa","title":"Example","category":"LOGIN","fields":[
  {"id":"notesPlain","type":"STRING","purpose":"NOTES","label":"notesPlain","value":"not this"},
  {"id":"username","type":"STRING","purpose":"USERNAME","label":"Email address","value":"me@example.com"},
  {"id":"password","type":"CONCEALED","purpose":"PASSWORD","label":"pw","value":"p\"a'ss\\w</script>\n"},
  {"id":"other","type":"CONCEALED","label":"password","value":"not this either"}
]})json";

TEST_CASE(reads_the_login_list)
{
    auto logins = parse_logins(LIST);
    EXPECT_EQ(logins.size(), 4);
    EXPECT(logins[0].id == "aaa");
    EXPECT(logins[0].title == "Example");
    EXPECT(logins[0].username == "me@example.com");
    EXPECT(logins[0].vault == "Personal");
    EXPECT(logins[0].urls == QStringList { "https://example.com/login" });
    EXPECT(logins[3].urls.isEmpty());
    EXPECT(parse_logins("not json").isEmpty());
}

TEST_CASE(reads_the_username_and_password_by_their_purpose)
{
    auto credentials = parse_credentials(ITEM);
    VERIFY(credentials.has_value());
    EXPECT(credentials->username == "me@example.com");
    EXPECT(credentials->password == "p\"a'ss\\w</script>\n");
    EXPECT(!parse_credentials(R"({"id":"x","fields":[{"purpose":"NOTES","value":"n"}]})").has_value());
    EXPECT(!parse_credentials("").has_value());
}

TEST_CASE(logins_match_the_same_site_only)
{
    QUrl page("https://www.example.com/sign-in");
    EXPECT_EQ(match_strength("https://www.example.com", page), 2);
    EXPECT_EQ(match_strength("www.example.com/anything", page), 2); // Saved without a scheme.
    EXPECT_EQ(match_strength("https://example.com/login", page), 1);
    EXPECT_EQ(match_strength("accounts.example.com", page), 1);
    // Look-alikes aren't the site.
    EXPECT_EQ(match_strength("https://example.com.evil.net", page), 0);
    EXPECT_EQ(match_strength("https://notexample.com", page), 0);
    EXPECT_EQ(match_strength("https://example.org", page), 0);
    // Two sites under one public suffix are two sites.
    EXPECT_EQ(match_strength("https://alice.github.io", QUrl("https://bob.github.io")), 0);
    EXPECT_EQ(match_strength("https://shop.co.uk", QUrl("https://bank.co.uk")), 0);
    // Addresses match exactly or not at all.
    EXPECT_EQ(match_strength("http://127.0.0.1:8080", QUrl("http://127.0.0.1:9000/login")), 2);
    EXPECT_EQ(match_strength("http://127.0.0.2", QUrl("http://127.0.0.1/login")), 0);
    // An app's address isn't a website.
    EXPECT_EQ(match_strength("android://abc@com.example", page), 0);
    EXPECT_EQ(match_strength("", page), 0);
}

TEST_CASE(best_matches_come_first)
{
    auto logins = logins_for(parse_logins(LIST), QUrl("https://accounts.example.com/"));
    EXPECT_EQ(logins.size(), 2);
    EXPECT(logins[0].id == "bbb"); // Same host.
    EXPECT(logins[1].id == "aaa"); // Same site.
    EXPECT(logins_for(parse_logins(LIST), QUrl("https://unknown.net")).isEmpty());
}

TEST_CASE(fills_only_secure_pages)
{
    EXPECT(can_fill(QUrl("https://example.com/login")));
    EXPECT(can_fill(QUrl("http://localhost:3000/")));
    EXPECT(can_fill(QUrl("http://127.0.0.1:8080/login")));
    EXPECT(!can_fill(QUrl("http://example.com/login")));
    EXPECT(!can_fill(QUrl("data:text/html,<input type=password>")));
    EXPECT(!can_fill(QUrl("file:///tmp/login.html")));
    EXPECT(!can_fill(QUrl("about:blank")));
}

TEST_CASE(origin_is_scheme_host_and_port)
{
    EXPECT(origin_of(QUrl("https://example.com/a/b?c#d")) == "https://example.com");
    EXPECT(origin_of(QUrl("http://127.0.0.1:8080/login")) == "http://127.0.0.1:8080");
}

TEST_CASE(fill_script_carries_the_values_intact)
{
    // Quotes, backslashes, newlines, a closing script tag and line separators can't end the string early: the
    // values read back exactly from the script.
    Credentials credentials { "a\"b'c\\d", QString("x</script>\ny") + QChar(0x2028) + "`${z}`" };
    auto script = fill_script("https://example.com", credentials);
    auto start = script.indexOf("const [origin, username, password] = ");
    VERIFY(start >= 0);
    start += QString("const [origin, username, password] = ").size();
    auto end = script.indexOf(";\n", start);
    auto values = QJsonDocument::fromJson(script.mid(start, end - start).toUtf8()).array();
    EXPECT_EQ(values.size(), 3);
    EXPECT(values[0].toString() == "https://example.com");
    EXPECT(values[1].toString() == credentials.username);
    EXPECT(values[2].toString() == credentials.password);
    // It checks the page is still where the login was chosen for before filling anything.
    EXPECT(script.indexOf("if (location.origin !== origin) return;") < script.indexOf("value_setter"));
}

TEST_CASE(error_messages_say_what_to_do)
{
    EXPECT(error_message(1, "[ERROR] 2026/10/24 10:00:00 authorization prompt dismissed, please try again") == "1Password: cancelled");
    EXPECT(error_message(1, "[ERROR] 2026/10/24 10:00:00 No accounts configured for use with 1Password CLI.").contains("Integrate with 1Password CLI"));
    EXPECT(error_message(1, "[ERROR] 2026/10/24 10:00:00 You are not currently signed in.").contains("Integrate with 1Password CLI"));
    EXPECT(error_message(1, "[ERROR] 2026/10/24 10:00:00 something odd") == "1Password: something odd");
    EXPECT(error_message(3, "").contains("exit 3"));
}

// --- Running `op` ---------------------------------------------------------------------------------------------------

static QString write_fake_op(QTemporaryDir const& dir, QString const& body)
{
    auto path = dir.filePath("op");
    QFile file(path);
    VERIFY(file.open(QIODevice::WriteOnly));
    file.write(("#!/bin/sh\n" + body).toUtf8());
    file.close();
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

template<typename Predicate>
static bool wait_for(Predicate predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

TEST_CASE(runs_op_for_the_list_and_the_login)
{
    QTemporaryDir dir;
    auto list_file = dir.filePath("list.json");
    auto item_file = dir.filePath("item.json");
    {
        QFile file(list_file);
        VERIFY(file.open(QIODevice::WriteOnly));
        file.write(LIST);
    }
    {
        QFile file(item_file);
        VERIFY(file.open(QIODevice::WriteOnly));
        file.write(ITEM);
    }
    // It must ask for exactly these.
    auto op = write_fake_op(dir, QString(R"(
if [ "$*" = "item list --categories Login --format json" ]; then cat '%1'; exit 0; fi
if [ "$*" = "item get aaa --reveal --format json" ]; then cat '%2'; exit 0; fi
echo "[ERROR] 2026/10/24 10:00:00 unexpected: $*" >&2; exit 1
)").arg(list_file, item_file));
    qputenv("ARC_OP_PATH", op.toUtf8());
    EXPECT(find_op() == op);

    Runner runner(nullptr);
    Optional<QList<Login>> logins;
    QString error;
    runner.list_logins([&](QList<Login> result) { logins = result; }, [&](QString message) { error = message; });
    EXPECT(wait_for([&] { return logins.has_value() || !error.isEmpty(); }));
    EXPECT(error.isEmpty());
    EXPECT(logins.has_value() && logins->size() == 4);

    Optional<Credentials> credentials;
    runner.get_credentials("aaa", [&](Credentials result) { credentials = result; }, [&](QString message) { error = message; });
    EXPECT(wait_for([&] { return credentials.has_value() || !error.isEmpty(); }));
    EXPECT(credentials.has_value() && credentials->username == "me@example.com");

    // A failure comes back as its message.
    error.clear();
    credentials.clear();
    runner.get_credentials("zzz", [&](Credentials result) { credentials = result; }, [&](QString message) { error = message; });
    EXPECT(wait_for([&] { return credentials.has_value() || !error.isEmpty(); }));
    EXPECT(!credentials.has_value());
    EXPECT(error == "1Password: unexpected: item get zzz --reveal --format json");
    qunsetenv("ARC_OP_PATH");
}

TEST_CASE(says_how_to_install_op_when_it_is_missing)
{
    qputenv("ARC_OP_PATH", "/nonexistent/op");
    Runner runner(nullptr);
    QString error;
    runner.list_logins([](QList<Login>) {}, [&](QString message) { error = message; });
    EXPECT(error.contains("brew install 1password-cli"));
    qunsetenv("ARC_OP_PATH");
}
