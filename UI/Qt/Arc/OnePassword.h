/*
 * Fill from 1Password (Cmd+\): logins for the current site come from 1Password's command-line tool, `op`, which the
 * desktop app unlocks (with Touch ID) once "Integrate with 1Password CLI" is on in its Developer settings. Nothing
 * from the vault is stored: the chosen login is read when it's filled, and only into the page it was chosen for.
 *
 * The parsing, matching and fill script are plain functions, so they can be tested without 1Password.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

class QProcess;

namespace Ladybird::Arc::OnePassword {

struct Login {
    QString id;
    QString title;
    QString username; // What `op item list` shows under the title: usually the username.
    QString vault;
    QStringList urls;
};

struct Credentials {
    QString username;
    QString password;
};

// `op item list --categories Login --format json`.
QList<Login> parse_logins(QByteArray const& json);

// `op item get <id> --reveal --format json`: the fields 1Password marks as the username and password.
Optional<Credentials> parse_credentials(QByteArray const& json);

// Whether a login saved for `saved_url` belongs on `page`, and how well: 2 for the same host, 1 for another host on the
// same site (accounts.example.com for example.com), 0 for none. Saved URLs often leave out the scheme.
int match_strength(QString const& saved_url, QUrl const& page);

// The logins for `page`, best matches first.
QList<Login> logins_for(QList<Login> const&, QUrl const& page);

// Only secure pages are filled: https, or http on this computer.
bool can_fill(QUrl const& page);

// The origin a fill is for (scheme://host[:port]), as the page's script reports it.
QString origin_of(QUrl const& page);

// A script for the page: if it's still at `origin`, it fills the login form's username and password fields (or just
// the username, on a sign-in page that asks for it first) the way typing would, with input and change events.
QString fill_script(QString const& origin, Credentials const&);

// A short message for a failed `op` run, from its error output.
QString error_message(int exit_code, QString const& error_output);

// Where `op` is: $ARC_OP_PATH, Homebrew's locations, or the PATH. Apps opened from the Finder don't get the shell's
// PATH, so the usual install locations are checked by name.
QString find_op();

// Runs `op` without blocking: `done` gets its output, or an error message.
class Runner final : public QObject {
    Q_OBJECT

public:
    explicit Runner(QObject* parent);

    void list_logins(Function<void(QList<Login>)> done, Function<void(QString)> failed);
    void get_credentials(QString const& id, Function<void(Credentials)> done, Function<void(QString)> failed);

private:
    void run(QStringList arguments, Function<void(QByteArray)> done, Function<void(QString)> failed);
};

}
