/*
 * Reads Arc's sidebar data (StorableSidebar.json) into Arc-fork spaces.
 *
 * Arc's layout, as of its last releases:
 *   sidebar.containers[] -> the object with "spaces", "items" and "topAppsContainerIDs"
 *   spaces: [id, {title, containerIDs: ["unpinned", id, "pinned", id], customInfo, profile}, ...]
 *   items:  [id, {id, title (custom, may be null), data: {tab: {savedURL, savedTitle}} | {list: {}} | ...,
 *                 childrenIds}, ...]
 *   topAppsContainerIDs: [profile, containerId, ...]  (favorites, per Arc profile)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/ArcImportParser.h>
#include <UI/Qt/Arc/SymbolIcon.h>

#include <QColor>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUrl>

namespace Ladybird::Arc {

// Arc's built-in space icons that differ from SF Symbols' names.
QString sf_symbol_for_arc_icon(QString const& arc_icon)
{
    static QHash<QString, QString> const renames {
        { "flash", "bolt" },
        { "lightning", "bolt" },
        { "planet", "globe.americas" },
        { "code", "chevron.left.forwardslash.chevron.right" },
        { "cart", "cart" },
        { "sparkle", "sparkles" },
        { "skull", "arc.skull" }, // Not in SF Symbols; drawn by CustomGlyphs.cpp.
    };
    auto name = renames.value(arc_icon, arc_icon);
    // Only use it if this macOS has it (and off macOS there are no symbols at all).
    if (fitted_symbol_pixmap(name, 15, Qt::black, 1).isNull())
        return {};
    return name;
}

// Arc's profile reference: {"default": true} or {"custom": {"_0": {"directoryBasename": "Profile 1"}}}.
// The default profile maps to an empty name, matching spaces and favorites created in Ladybird.
static QString profile_name(QJsonValue const& profile)
{
    return profile.toObject()["custom"].toObject()["_0"].toObject()["directoryBasename"].toString();
}

static QColor theme_color(QJsonObject const& space)
{
    auto tone = space["customInfo"].toObject()["windowTheme"].toObject()["primaryColorPalette"].toObject()["midTone"].toObject();
    if (tone.isEmpty())
        return {};
    auto channel = [&](char const* key) { return qBound(0.0, tone[key].toDouble(), 1.0); };
    return QColor::fromRgbF(channel("red"), channel("green"), channel("blue"));
}

// Only web pages come across. Arc data can hold links shared by others (shared spaces and folders), and Today tabs
// open as soon as the import ends: a javascript:, file: or data: link must not run or open local files that way.
bool is_importable_url(QString const& url)
{
    auto scheme = QUrl(url).scheme().toLower();
    return scheme == "http" || scheme == "https";
}

// Folders refer to their children by id, so nesting isn't bounded by the JSON parser: deeper than this is skipped,
// rather than recursing until the stack runs out.
static constexpr int MAX_FOLDER_DEPTH = 64;

namespace {

class Reader {
public:
    Reader(QJsonArray const& items, ArcSidebarData& data)
        : m_data(data)
    {
        for (auto const& value : items) {
            if (value.isObject())
                m_items.insert(value.toObject()["id"].toString(), value.toObject());
        }
    }

    QList<PinnedNode> children_of(QString const& container_id, int depth = 0)
    {
        QList<PinnedNode> result;
        if (depth > MAX_FOLDER_DEPTH) {
            ++m_data.skipped_items;
            return result;
        }
        for (auto const& child : m_items.value(container_id)["childrenIds"].toArray()) {
            auto id = child.toString();
            if (m_visited.contains(id))
                continue; // Malformed data: don't loop.
            m_visited.insert(id);
            if (auto node = convert(m_items.value(id), depth); !node.id.isEmpty())
                result.append(std::move(node));
        }
        return result;
    }

    QList<Favorite> favorites_in(QString const& container_id)
    {
        QList<Favorite> result;
        for (auto const& child : m_items.value(container_id)["childrenIds"].toArray()) {
            auto tab = m_items.value(child.toString())["data"].toObject()["tab"].toObject();
            auto url = tab["savedURL"].toString();
            if (url.isEmpty())
                continue;
            if (!is_importable_url(url)) {
                ++m_data.skipped_links;
                continue;
            }
            auto title = m_items.value(child.toString())["title"].toString();
            result.append({ {}, url, title.isEmpty() ? tab["savedTitle"].toString() : title, {} });
        }
        return result;
    }

private:
    PinnedNode convert(QJsonObject const& item, int depth)
    {
        PinnedNode node;
        auto data = item["data"].toObject();
        auto custom_title = item["title"].toString();

        if (data.contains("tab")) {
            auto tab = data["tab"].toObject();
            node.url = tab["savedURL"].toString();
            if (node.url.isEmpty())
                return {};
            if (!is_importable_url(node.url)) {
                ++m_data.skipped_links;
                return {};
            }
            node.id = SpaceStore::new_id();
            node.kind = PinnedNode::Kind::Tab;
            node.saved_title = tab["savedTitle"].toString();
            if (custom_title != node.saved_title)
                node.title = custom_title; // Renamed in Arc.
            ++m_data.tabs;
            return node;
        }

        if (data.contains("list")) {
            node.id = SpaceStore::new_id();
            node.kind = PinnedNode::Kind::Folder;
            node.title = custom_title.isEmpty() ? QString("Untitled") : custom_title;
            node.expanded = false; // Arc doesn't record it; collapsed keeps large imports tidy.
            node.children = children_of(item["id"].toString(), depth + 1);
            ++m_data.folders;
            return node;
        }

        ++m_data.skipped_items;
        return {};
    }

    ArcSidebarData& m_data;
    QHash<QString, QJsonObject> m_items;
    QSet<QString> m_visited;
};

}

ArcSidebarData parse_arc_sidebar(QByteArray const& json)
{
    ArcSidebarData data;
    auto root = QJsonDocument::fromJson(json).object();

    QJsonObject container;
    for (auto const& value : root["sidebar"].toObject()["containers"].toArray()) {
        if (value.toObject().contains("spaces"))
            container = value.toObject();
    }
    if (container.isEmpty()) {
        data.notes.append("No spaces found in Arc's sidebar data.");
        return data;
    }

    Reader reader(container["items"].toArray(), data);

    for (auto const& value : container["spaces"].toArray()) {
        if (!value.isObject())
            continue; // The ids interleaved with the space objects.
        auto object = value.toObject();

        QString pinned_id;
        QString unpinned_id;
        auto ids = object["containerIDs"].toArray();
        for (qsizetype i = 0; i + 1 < ids.size(); i += 2) {
            if (ids[i].toString() == "pinned")
                pinned_id = ids[i + 1].toString();
            else if (ids[i].toString() == "unpinned")
                unpinned_id = ids[i + 1].toString();
        }

        Space space;
        space.source_id = object["id"].toString();
        space.title = object["title"].toString();
        if (space.title.isEmpty())
            space.title = "Space";
        space.color = theme_color(object);
        space.profile = profile_name(object["profile"]);
        space.pinned = reader.children_of(pinned_id);

        // Today's (unpinned) tabs stay unpinned: they open as regular tabs below the divider.
        auto tabs_before = data.tabs;
        auto folders_before = data.folders;
        auto add_today = [&](auto const& self, QList<PinnedNode> const& nodes) -> void {
            for (auto const& node : nodes) {
                if (node.is_folder())
                    self(self, node.children);
                else
                    data.today_tabs.append({ space.source_id, node.url, node.title.isEmpty() ? node.saved_title : node.title });
            }
        };
        add_today(add_today, reader.children_of(unpinned_id));
        // Only pinned tabs and folders are counted; these are listed in today_tabs.
        data.tabs = tabs_before;
        data.folders = folders_before;

        // NB: const lookups (value()). A non-const operator[] inserts missing keys, which can invalidate other
        //     references into the same object mid-expression.
        auto const icon_type = object.value("customInfo").toObject().value("iconType").toObject();
        if (auto emoji = icon_type.value("emoji_v2").toString(icon_type.value("emoji").toString()); !emoji.isEmpty()) {
            space.icon = emoji;
        } else if (auto arc_icon = icon_type.value("icon").toString(); !arc_icon.isEmpty()) {
            if (auto symbol = sf_symbol_for_arc_icon(arc_icon); !symbol.isEmpty())
                space.icon = "sf:" + symbol;
            else
                data.notes.append(QString("\"%1\" uses Arc's \"%2\" icon, which has no SF Symbol; it shows its first letter.").arg(space.title, arc_icon));
        }

        data.spaces.append(std::move(space));
    }

    // Favorites are per Arc profile, as here: each space shows its own profile's. One per URL in a profile.
    QSet<QString> seen;
    auto top_apps = container["topAppsContainerIDs"].toArray();
    for (qsizetype i = 0; i + 1 < top_apps.size(); i += 2) {
        auto profile = profile_name(top_apps[i]);
        for (auto favorite : reader.favorites_in(top_apps[i + 1].toString())) {
            auto key = profile + '\n' + favorite.url;
            if (seen.contains(key))
                continue;
            seen.insert(key);
            favorite.profile = profile;
            data.favorites.append(favorite);
        }
    }

    if (data.skipped_links > 0)
        data.notes.append(QString("Skipped %1 links that aren't web pages (such as file: or javascript: links).").arg(data.skipped_links));

    return data;
}

}
