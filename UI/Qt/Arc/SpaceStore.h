/*
 * Arc-style spaces: each space has a name, icon, color and a pinned tree of folders and tabs.
 * Favorites are shared by all spaces. Stored as ArcSpaces.json in the Ladybird profile's data directory.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QColor>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QJsonObject;

namespace Ladybird::Arc {

struct PinnedNode {
    enum class Kind {
        Tab,
        Folder,
    };

    QString id;
    Kind kind { Kind::Tab };
    QString title;       // Folders: name. Tabs: user-chosen name; empty means use the page title.
    QString saved_title; // Tabs only: last known page title, shown while the tab is unloaded.
    QString url;         // Tabs only: the saved URL. Navigating inside the tab doesn't change it.
    bool expanded { true };
    QList<PinnedNode> children;

    bool is_folder() const { return kind == Kind::Folder; }
};

struct Favorite {
    QString id;
    QString url;
    QString title;
};

// Remembers which open tabs belonged to which space, so restored tabs land back in place.
struct SessionEntry {
    QString url;
    QString space_id;
    QString node_id; // Pinned node or favorite the tab was showing, if any.
};

struct Space {
    QString id;
    QString title;
    QString icon; // An emoji or short text.
    QColor color;
    QList<PinnedNode> pinned;
};

// Where a node sits: which space, which parent folder (empty for the space's top level), and index.
struct NodeLocation {
    QString space_id;
    QString parent_id;
    int index { -1 };

    bool is_valid() const { return !space_id.isEmpty() && index >= 0; }
};

class SpaceStore final : public QObject {
    Q_OBJECT

public:
    static SpaceStore& the();

    QList<Space> const& spaces() const { return m_spaces; }
    QList<Favorite> const& favorites() const { return m_favorites; }
    QList<SessionEntry> const& session() const { return m_session; }

    Space const* space(QString const& id) const;
    QString first_space_id() const;
    QString last_active_space_id() const { return m_last_active_space_id; }
    void set_last_active_space_id(QString const&);

    PinnedNode const* node(QString const& id) const;
    NodeLocation location_of(QString const& node_id) const;
    Favorite const* favorite(QString const& id) const;

    // Spaces
    QString add_space(QString title, QString icon, QColor color);
    void update_space(QString const& id, QString title, QString icon, QColor color);
    void remove_space(QString const& id);
    void move_space(QString const& id, int new_index);

    // Pinned tree. An empty parent_id means the space's top level; index -1 appends.
    QString add_pinned_tab(QString const& space_id, QString const& parent_id, int index, QString url, QString saved_title);
    QString add_folder(QString const& space_id, QString const& parent_id, int index, QString title);
    void rename_node(QString const& id, QString title);
    void set_saved_title(QString const& id, QString title);
    void set_node_url(QString const& id, QString url);
    void set_folder_expanded(QString const& id, bool expanded);
    void remove_node(QString const& id);
    // Returns false if the move is impossible (for example a folder into its own subtree).
    bool move_node(QString const& id, QString const& space_id, QString const& parent_id, int index);

    // Favorites
    QString add_favorite(QString url, QString title, int index = -1);
    void remove_favorite(QString const& id);
    void move_favorite(QString const& id, int new_index);

    void set_session(QList<SessionEntry>);

    static QString new_id();

signals:
    void changed();

private:
    SpaceStore();

    void load();
    void save();
    void commit(); // save() + emit changed()
    void ensure_default_space();

    QList<PinnedNode>* children_list(QString const& space_id, QString const& parent_id);
    Space* mutable_space(QString const& id);
    PinnedNode* mutable_node(QString const& id);

    QString m_path;
    QList<Space> m_spaces;
    QList<Favorite> m_favorites;
    QList<SessionEntry> m_session;
    QString m_last_active_space_id;
};

}
