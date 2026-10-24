/*
 * Arc-style spaces: each space has a name, icon, color and a pinned tree of folders and tabs.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/SpaceStore.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace Ladybird::Arc {

static constexpr int SAVE_DELAY_MS = 400;
static constexpr int BOOKKEEPING_SAVE_DELAY_MS = 10'000;

static constexpr int FILE_VERSION = 1;

SpaceStore::SpaceStore(QString file_path)
    : m_path(std::move(file_path))
{
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    m_is_new_profile = !QFile::exists(m_path);

    // Changes are written shortly after they're made, batched: bookkeeping (titles, the active space, folders) can
    // change many times a second.
    m_save_timer = new QTimer(this);
    m_save_timer->setSingleShot(true);
    m_save_timer->setInterval(SAVE_DELAY_MS);
    connect(m_save_timer, &QTimer::timeout, this, &SpaceStore::write_now);

    load();
    ensure_default_space();
    flush(); // A new profile's file exists from the start.
}

SpaceStore::~SpaceStore()
{
    flush();
}

QString SpaceStore::new_id()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// --- Serialization ------------------------------------------------------------------------------

static QJsonObject node_to_json(PinnedNode const& node)
{
    QJsonObject object;
    object["id"] = node.id;
    object["type"] = node.is_folder() ? "folder" : "tab";
    if (!node.title.isEmpty())
        object["title"] = node.title;
    if (node.is_folder()) {
        object["expanded"] = node.expanded;
        QJsonArray children;
        for (auto const& child : node.children)
            children.append(node_to_json(child));
        object["children"] = children;
    } else {
        object["url"] = node.url;
        if (!node.saved_title.isEmpty())
            object["savedTitle"] = node.saved_title;
    }
    return object;
}

static PinnedNode node_from_json(QJsonObject const& object)
{
    PinnedNode node;
    node.id = object["id"].toString(SpaceStore::new_id());
    node.kind = object["type"].toString() == "folder" ? PinnedNode::Kind::Folder : PinnedNode::Kind::Tab;
    node.title = object["title"].toString();
    node.url = object["url"].toString();
    node.saved_title = object["savedTitle"].toString();
    node.expanded = object["expanded"].toBool(true);
    for (auto const& child : object["children"].toArray())
        node.children.append(node_from_json(child.toObject()));
    return node;
}

void SpaceStore::load()
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return;
    auto root = document.object();

    for (auto const& value : root["spaces"].toArray()) {
        auto object = value.toObject();
        Space space;
        space.id = object["id"].toString(new_id());
        space.source_id = object["sourceId"].toString();
        space.title = object["title"].toString("Space");
        space.icon = object["icon"].toString();
        if (space.icon == QString::fromUtf8("\xF0\x9F\x8F\xA0"))
            space.icon = "sf:house"; // Earlier builds used a house emoji for the default space.
        space.color = QColor(object["color"].toString());
        space.profile = object["profile"].toString();
        for (auto const& node : object["pinned"].toArray())
            space.pinned.append(node_from_json(node.toObject()));
        m_spaces.append(std::move(space));
    }

    for (auto const& value : root["favorites"].toArray()) {
        auto object = value.toObject();
        m_favorites.append({ object["id"].toString(new_id()), object["url"].toString(), object["title"].toString(), object["profile"].toString() });
    }

    for (auto const& value : root["session"].toArray()) {
        auto object = value.toObject();
        m_session.append({ object["url"].toString(), object["space"].toString(), object["node"].toString(), object["home"].toString() });
    }

    m_last_active_space_id = root["activeSpace"].toString();
}

void SpaceStore::flush()
{
    if (m_save_timer && m_save_timer->isActive()) {
        m_save_timer->stop();
        write_now();
    }
}

void SpaceStore::save()
{
    m_save_timer->setInterval(SAVE_DELAY_MS);
    m_save_timer->start();
}

// Bookkeeping (saved titles, the session) changes all the time, e.g. a pinned tab whose title counts unread mail: it's
// written within a while (or at quit), not shortly after each change.
void SpaceStore::save_later()
{
    if (m_save_timer->isActive())
        return;
    m_save_timer->setInterval(BOOKKEEPING_SAVE_DELAY_MS);
    m_save_timer->start();
}

void SpaceStore::write_now()
{
    ++m_write_count;
    QJsonObject root;
    root["version"] = FILE_VERSION;
    root["activeSpace"] = m_last_active_space_id;

    QJsonArray spaces;
    for (auto const& space : m_spaces) {
        QJsonObject object;
        object["id"] = space.id;
        if (!space.source_id.isEmpty())
            object["sourceId"] = space.source_id;
        object["title"] = space.title;
        object["icon"] = space.icon;
        object["color"] = space.color.isValid() ? space.color.name() : QString();
        if (!space.profile.isEmpty())
            object["profile"] = space.profile;
        QJsonArray pinned;
        for (auto const& node : space.pinned)
            pinned.append(node_to_json(node));
        object["pinned"] = pinned;
        spaces.append(object);
    }
    root["spaces"] = spaces;

    QJsonArray favorites;
    for (auto const& favorite : m_favorites) {
        QJsonObject object { { "id", favorite.id }, { "url", favorite.url }, { "title", favorite.title } };
        if (!favorite.profile.isEmpty())
            object["profile"] = favorite.profile;
        favorites.append(object);
    }
    root["favorites"] = favorites;

    QJsonArray session;
    for (auto const& entry : m_session) {
        QJsonObject object { { "url", entry.url }, { "space", entry.space_id }, { "node", entry.node_id } };
        if (!entry.home.isEmpty())
            object["home"] = entry.home;
        session.append(object);
    }
    root["session"] = session;

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}

void SpaceStore::commit()
{
    save();
    emit changed();
}

void SpaceStore::ensure_default_space()
{
    if (!m_spaces.isEmpty())
        return;
    m_spaces.append({ new_id(), {}, "Personal", "sf:house", QColor("#7a9cc6"), {}, {} });
    save();
}

// --- Lookup -------------------------------------------------------------------------------------

Space const* SpaceStore::space(QString const& id) const
{
    for (auto const& space : m_spaces) {
        if (space.id == id)
            return &space;
    }
    return nullptr;
}

Space* SpaceStore::mutable_space(QString const& id)
{
    return const_cast<Space*>(space(id));
}

QString SpaceStore::first_space_id() const
{
    return m_spaces.isEmpty() ? QString() : m_spaces.first().id;
}

void SpaceStore::set_last_active_space_id(QString const& id)
{
    if (m_last_active_space_id == id)
        return;
    m_last_active_space_id = id;
    save(); // Not a content change, so no changed() signal.
}

static PinnedNode const* find_node(QList<PinnedNode> const& nodes, QString const& id)
{
    for (auto const& node : nodes) {
        if (node.id == id)
            return &node;
        if (auto const* found = find_node(node.children, id))
            return found;
    }
    return nullptr;
}

PinnedNode const* SpaceStore::node(QString const& id) const
{
    for (auto const& space : m_spaces) {
        if (auto const* found = find_node(space.pinned, id))
            return found;
    }
    return nullptr;
}

PinnedNode* SpaceStore::mutable_node(QString const& id)
{
    return const_cast<PinnedNode*>(node(id));
}

static bool locate(QList<PinnedNode> const& nodes, QString const& parent_id, QString const& id, NodeLocation& location)
{
    for (int i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == id) {
            location.parent_id = parent_id;
            location.index = i;
            return true;
        }
        if (locate(nodes[i].children, nodes[i].id, id, location))
            return true;
    }
    return false;
}

NodeLocation SpaceStore::location_of(QString const& node_id) const
{
    for (auto const& space : m_spaces) {
        NodeLocation location;
        if (locate(space.pinned, {}, node_id, location)) {
            location.space_id = space.id;
            return location;
        }
    }
    return {};
}

QList<PinnedNode>* SpaceStore::children_list(QString const& space_id, QString const& parent_id)
{
    auto* space = mutable_space(space_id);
    if (!space)
        return nullptr;
    if (parent_id.isEmpty())
        return &space->pinned;
    auto* parent = mutable_node(parent_id);
    if (!parent || !parent->is_folder())
        return nullptr;
    return &parent->children;
}

Favorite const* SpaceStore::favorite(QString const& id) const
{
    for (auto const& favorite : m_favorites) {
        if (favorite.id == id)
            return &favorite;
    }
    return nullptr;
}

// --- Spaces -------------------------------------------------------------------------------------

QString SpaceStore::add_space(QString title, QString icon, QColor color)
{
    auto id = new_id();
    m_spaces.append({ id, {}, std::move(title), std::move(icon), color, {}, {} });
    commit();
    return id;
}

void SpaceStore::update_space(QString const& id, QString title, QString icon, QColor color)
{
    auto* space = mutable_space(id);
    if (!space)
        return;
    space->title = std::move(title);
    space->icon = std::move(icon);
    space->color = color;
    commit();
}

void SpaceStore::remove_space(QString const& id)
{
    if (m_spaces.size() <= 1)
        return;
    m_spaces.removeIf([&](auto const& space) { return space.id == id; });
    if (m_last_active_space_id == id)
        m_last_active_space_id = first_space_id();
    commit();
}

void SpaceStore::move_space(QString const& id, int new_index)
{
    for (int i = 0; i < m_spaces.size(); ++i) {
        if (m_spaces[i].id == id) {
            m_spaces.move(i, qBound(0, new_index, static_cast<int>(m_spaces.size()) - 1));
            commit();
            return;
        }
    }
}

// --- Pinned tree --------------------------------------------------------------------------------

static void insert_clamped(QList<PinnedNode>& list, int index, PinnedNode node)
{
    if (index < 0 || index > list.size())
        index = list.size();
    list.insert(index, std::move(node));
}

QString SpaceStore::add_pinned_tab(QString const& space_id, QString const& parent_id, int index, QString url, QString saved_title)
{
    auto* list = children_list(space_id, parent_id);
    if (!list)
        return {};
    PinnedNode node;
    node.id = new_id();
    node.kind = PinnedNode::Kind::Tab;
    node.url = std::move(url);
    node.saved_title = std::move(saved_title);
    auto id = node.id;
    insert_clamped(*list, index, std::move(node));
    commit();
    return id;
}

QString SpaceStore::add_folder(QString const& space_id, QString const& parent_id, int index, QString title)
{
    auto* list = children_list(space_id, parent_id);
    if (!list)
        return {};
    PinnedNode node;
    node.id = new_id();
    node.kind = PinnedNode::Kind::Folder;
    node.title = std::move(title);
    auto id = node.id;
    insert_clamped(*list, index, std::move(node));
    commit();
    return id;
}

void SpaceStore::rename_node(QString const& id, QString title)
{
    if (auto* node = mutable_node(id)) {
        node->title = std::move(title);
        commit();
    }
}

void SpaceStore::set_saved_title(QString const& id, QString title)
{
    if (auto* node = mutable_node(id); node && !node->is_folder() && node->saved_title != title) {
        node->saved_title = std::move(title);
        save_later(); // The sidebar shows live titles for loaded tabs already.
    }
}

void SpaceStore::set_node_url(QString const& id, QString url)
{
    if (auto* node = mutable_node(id); node && !node->is_folder()) {
        node->url = std::move(url);
        commit();
    }
}

void SpaceStore::set_folder_expanded(QString const& id, bool expanded)
{
    if (auto* node = mutable_node(id); node && node->is_folder() && node->expanded != expanded) {
        node->expanded = expanded;
        save(); // Expansion is view state; the sidebar already shows it, so skip changed().
    }
}

void SpaceStore::remove_node(QString const& id)
{
    auto location = location_of(id);
    if (!location.is_valid())
        return;
    children_list(location.space_id, location.parent_id)->removeAt(location.index);
    commit();
}

bool SpaceStore::move_node(QString const& id, QString const& space_id, QString const& parent_id, int index)
{
    auto from = location_of(id);
    if (!from.is_valid())
        return false;

    // A folder can't move into itself or its own subtree.
    if (!parent_id.isEmpty()) {
        if (parent_id == id)
            return false;
        auto const* moving = node(id);
        if (moving && find_node(moving->children, parent_id))
            return false;
    }

    auto* source = children_list(from.space_id, from.parent_id);
    auto node = source->takeAt(from.index);

    // Removing from the same list shifts later indices down by one.
    if (from.space_id == space_id && from.parent_id == parent_id && index > from.index)
        --index;

    auto* target = children_list(space_id, parent_id);
    if (!target) {
        source->insert(from.index, std::move(node));
        return false;
    }
    insert_clamped(*target, index, std::move(node));
    commit();
    return true;
}

// --- Favorites ----------------------------------------------------------------------------------

QString SpaceStore::add_favorite(QString url, QString title, int index, QString profile)
{
    // One favorite per URL in a profile.
    for (auto const& favorite : m_favorites) {
        if (favorite.url == url && favorite.profile == profile)
            return favorite.id;
    }
    auto id = new_id();
    if (index < 0 || index > m_favorites.size())
        index = m_favorites.size();
    m_favorites.insert(index, { id, std::move(url), std::move(title), std::move(profile) });
    commit();
    return id;
}

QList<Favorite> SpaceStore::favorites_for_profile(QString const& profile) const
{
    QList<Favorite> result;
    for (auto const& favorite : m_favorites) {
        if (favorite.profile == profile)
            result.append(favorite);
    }
    return result;
}

void SpaceStore::remove_favorite(QString const& id)
{
    m_favorites.removeIf([&](auto const& favorite) { return favorite.id == id; });
    commit();
}

void SpaceStore::move_favorite(QString const& id, int new_index)
{
    for (int i = 0; i < m_favorites.size(); ++i) {
        if (m_favorites[i].id == id) {
            m_favorites.move(i, qBound(0, new_index, static_cast<int>(m_favorites.size()) - 1));
            commit();
            return;
        }
    }
}

SpaceStore::MergeResult SpaceStore::merge_imported(QList<Space> spaces, QList<Favorite> const& favorites)
{
    MergeResult result;
    for (auto& imported : spaces) {
        Space* existing = nullptr;
        for (auto& space : m_spaces) {
            if (!imported.source_id.isEmpty() && space.source_id == imported.source_id)
                existing = &space;
        }
        if (existing) {
            // Keep our id (open tabs and the remembered active space refer to it); take everything else.
            imported.id = existing->id;
            *existing = std::move(imported);
            ++result.spaces_updated;
        } else {
            if (imported.id.isEmpty())
                imported.id = new_id();
            m_spaces.append(std::move(imported));
            ++result.spaces_added;
        }
    }

    for (auto const& favorite : favorites) {
        auto known = std::any_of(m_favorites.begin(), m_favorites.end(), [&](auto const& existing) {
            return existing.url == favorite.url && existing.profile == favorite.profile;
        });
        if (known || favorite.url.isEmpty())
            continue;
        m_favorites.append({ new_id(), favorite.url, favorite.title, favorite.profile });
        ++result.favorites_added;
    }

    // The untouched starter space would just sit beside the imported ones (often with the same name).
    if (result.spaces_added > 0) {
        m_spaces.removeIf([](Space const& space) {
            return space.source_id.isEmpty() && space.pinned.isEmpty() && space.title == "Personal" && space.icon == "sf:house";
        });
        if (!space(m_last_active_space_id))
            m_last_active_space_id = first_space_id();
    }

    commit();
    return result;
}

void SpaceStore::move_favorite_within_profile(QString const& id, int delta)
{
    int from = -1;
    for (int i = 0; i < m_favorites.size(); ++i) {
        if (m_favorites[i].id == id)
            from = i;
    }
    if (from < 0)
        return;
    for (int i = from + delta; i >= 0 && i < m_favorites.size(); i += delta) {
        if (m_favorites[i].profile == m_favorites[from].profile) {
            m_favorites.swapItemsAt(from, i);
            commit();
            return;
        }
    }
}

void SpaceStore::set_session(QList<SessionEntry> session)
{
    // Saved after every page load: most loads leave it as it was.
    auto same = session.size() == m_session.size()
        && std::equal(session.begin(), session.end(), m_session.begin(), [](SessionEntry const& a, SessionEntry const& b) {
               return a.url == b.url && a.space_id == b.space_id && a.node_id == b.node_id && a.home == b.home;
           });
    if (same)
        return;
    m_session = std::move(session);
    save_later();
}

}
