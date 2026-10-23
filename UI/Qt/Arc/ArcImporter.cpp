/*
 * Imports Arc's spaces, folders, pinned tabs, favorites and bookmarks into Ladybird bookmarks.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibWebView/Application.h>
#include <LibWebView/BookmarkStore.h>
#include <UI/Qt/Arc/ArcIcons.h>
#include <UI/Qt/Arc/ArcImportParser.h>
#include <UI/Qt/Arc/ArcImporter.h>
#include <UI/Qt/Arc/ArcSidebar.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/Arc/SpaceStore.h>
#include <UI/Qt/StringUtils.h>

#include <QColor>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>
#include <QStringList>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QSettings>
#include <QUuid>

#include <sqlite3.h>

namespace Ladybird::Arc {

static constexpr auto ROOT_FOLDER_TITLE = "Arc"sv;
static constexpr auto IMPORT_ROOT_SETTING = "import/root_folder_id";

QString default_arc_data_directory()
{
    return QDir::homePath() + "/Library/Application Support/Arc";
}

static String new_id()
{
    return ak_string_from_qstring(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

static ErrorOr<JsonValue> read_json(QString const& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Error::from_string_literal("Could not open file");
    auto bytes = file.readAll();
    return JsonValue::from_string(StringView { bytes.constData(), static_cast<size_t>(bytes.size()) });
}

struct Builder {
    ImportSummary summary;

    JsonObject folder(StringView title, JsonArray children, Optional<String> id = {})
    {
        JsonObject object;
        object.set("id"sv, id.has_value() ? id.release_value() : new_id());
        object.set("type"sv, "folder"_string);
        object.set("title"sv, MUST(String::from_utf8(title)));
        object.set("children"sv, move(children));
        return object;
    }

    Optional<JsonObject> bookmark(StringView url, StringView title)
    {
        if (url.is_empty())
            return {};
        JsonObject object;
        object.set("id"sv, new_id());
        object.set("type"sv, "bookmark"_string);
        object.set("url"sv, MUST(String::from_utf8(url)));
        if (!title.is_empty())
            object.set("title"sv, MUST(String::from_utf8(title)));
        return object;
    }
};

// Arc stores many lists as [key, value, key, value, ...].
template<typename Callback>
static void for_each_pair(JsonArray const& array, Callback callback)
{
    for (size_t i = 0; i + 1 < array.size(); i += 2)
        callback(array.at(i), array.at(i + 1));
}

static Optional<String> string_or_empty(JsonObject const& object, StringView key)
{
    if (auto value = object.get_string(key); value.has_value())
        return *value;
    return {};
}

// --- User Data/<profile>/Bookmarks: Arc's Chromium bookmark files ----------------------------

static JsonArray convert_chromium_nodes(JsonArray const& nodes, Builder& builder)
{
    JsonArray result;
    nodes.for_each([&](JsonValue const& value) {
        if (!value.is_object())
            return;
        auto const& node = value.as_object();
        auto type = string_or_empty(node, "type"sv).value_or({});
        auto name = string_or_empty(node, "name"sv).value_or({});

        if (type == "url"sv) {
            if (auto bookmark = builder.bookmark(string_or_empty(node, "url"sv).value_or({}), name); bookmark.has_value()) {
                ++builder.summary.bookmarks;
                result.must_append(bookmark.release_value());
            }
        } else if (type == "folder"sv) {
            auto children = node.get_array("children"sv);
            auto converted = children.has_value() ? convert_chromium_nodes(*children, builder) : JsonArray {};
            if (converted.size() > 0)
                result.must_append(builder.folder(name, move(converted)));
        }
    });
    return result;
}

static void import_chromium_bookmarks(QString const& arc_directory, Builder& builder, JsonArray& root_children)
{
    QDir user_data(arc_directory + "/User Data");
    QStringList profiles { "Default" };
    profiles += user_data.entryList({ "Profile *" }, QDir::Dirs);

    for (auto const& profile : profiles) {
        auto json = read_json(user_data.filePath(profile + "/Bookmarks"));
        if (json.is_error() || !json.value().is_object())
            continue;
        auto roots = json.value().as_object().get_object("roots"sv);
        if (!roots.has_value())
            continue;

        JsonArray children;
        for (auto root : { "bookmark_bar"sv, "other"sv, "synced"sv }) {
            auto node = roots->get_object(root);
            if (!node.has_value())
                continue;
            if (auto nodes = node->get_array("children"sv); nodes.has_value()) {
                auto converted = convert_chromium_nodes(*nodes, builder);
                converted.for_each([&](JsonValue const& item) { children.must_append(item); });
            }
        }
        if (children.size() > 0)
            root_children.must_append(builder.folder(ak_string_from_qstring(QString("Bookmarks (%1)").arg(profile)), move(children)));
    }
}

// Pulls icons for the imported URLs out of Arc's (Chromium) icon databases, one per Arc profile. Works on a
// copy, since Arc keeps the original open.
static int import_icons(QString const& arc_directory, QSet<QString> const& urls)
{
    int imported = 0;
    QDir user_data(arc_directory + "/User Data");
    QStringList profiles { "Default" };
    profiles += user_data.entryList({ "Profile *" }, QDir::Dirs);

    for (auto const& profile : profiles) {
        QTemporaryDir temporary;
        auto copy = temporary.filePath("Favicons");
        if (!QFile::copy(user_data.filePath(profile + "/Favicons"), copy))
            continue;
        sqlite3* database = nullptr;
        if (sqlite3_open_v2(copy.toUtf8().constData(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            sqlite3_close(database);
            continue;
        }

        // Exact page first; otherwise any page on the same site (a range over the page_url index).
        sqlite3_stmt* exact = nullptr;
        sqlite3_stmt* site = nullptr;
        sqlite3_prepare_v2(database,
            "SELECT b.image_data FROM icon_mapping m JOIN favicon_bitmaps b ON b.icon_id = m.icon_id "
            "WHERE m.page_url = ?1 ORDER BY b.width DESC LIMIT 1",
            -1, &exact, nullptr);
        sqlite3_prepare_v2(database,
            "SELECT b.image_data FROM icon_mapping m JOIN favicon_bitmaps b ON b.icon_id = m.icon_id "
            "WHERE (m.page_url >= ?1 AND m.page_url < ?2) OR (m.page_url >= ?3 AND m.page_url < ?4) "
            "ORDER BY b.width DESC LIMIT 1",
            -1, &site, nullptr);

        auto first_blob = [](sqlite3_stmt* statement) -> QByteArray {
            QByteArray result;
            if (sqlite3_step(statement) == SQLITE_ROW) {
                auto const* data = static_cast<char const*>(sqlite3_column_blob(statement, 0));
                result = QByteArray(data, sqlite3_column_bytes(statement, 0));
            }
            sqlite3_reset(statement);
            return result;
        };

        for (auto const& url : urls) {
            if (!exact || !site || ArcIcons::has_icon(url))
                continue;
            auto url_bytes = url.toUtf8();
            sqlite3_bind_text(exact, 1, url_bytes.constData(), -1, SQLITE_TRANSIENT);
            auto png = first_blob(exact);
            if (png.isEmpty()) {
                auto host = QUrl(url).host();
                QByteArray bounds[4] = { ("https://" + host + "/").toUtf8(), ("https://" + host + "0").toUtf8(),
                    ("http://" + host + "/").toUtf8(), ("http://" + host + "0").toUtf8() };
                for (int i = 0; i < 4; ++i)
                    sqlite3_bind_text(site, i + 1, bounds[i].constData(), -1, SQLITE_TRANSIENT);
                png = first_blob(site);
            }
            if (!png.isEmpty() && ArcIcons::store_png(url, png))
                ++imported;
        }

        sqlite3_finalize(exact);
        sqlite3_finalize(site);
        sqlite3_close(database);
    }
    return imported;
}

static void collect_urls(QList<PinnedNode> const& nodes, QSet<QString>& urls)
{
    for (auto const& node : nodes) {
        if (node.is_folder())
            collect_urls(node.children, urls);
        else
            urls.insert(node.url);
    }
}

static constexpr char const* SPACE_COLORS[] = { "#6b8fd6", "#9b7fd4", "#d47fb0", "#6fb37f", "#d9965b", "#5fb3b3", "#d46f6f", "#cdb85c" };

ErrorOr<ImportSummary> import_from_arc(QString const& arc_directory)
{
    QFile sidebar_file(arc_directory + "/StorableSidebar.json");
    if (!sidebar_file.open(QIODevice::ReadOnly))
        return Error::from_string_literal("Arc data not found");

    // Spaces, pinned folders and tabs, favorites -> the Arc sidebar.
    auto sidebar = parse_arc_sidebar(sidebar_file.readAll());
    for (int i = 0; i < sidebar.spaces.size(); ++i) {
        if (!sidebar.spaces[i].color.isValid())
            sidebar.spaces[i].color = QColor(SPACE_COLORS[i % std::size(SPACE_COLORS)]);
    }

    ImportSummary summary;
    QSet<QString> urls;
    for (auto const& space : sidebar.spaces)
        collect_urls(space.pinned, urls);
    for (auto const& favorite : sidebar.favorites)
        urls.insert(favorite.url);
    for (auto const& tab : sidebar.today_tabs)
        urls.insert(tab.url);
    summary.icons = import_icons(arc_directory, urls);
    summary.folders = sidebar.folders;
    summary.tabs = sidebar.tabs;
    summary.skipped_items = sidebar.skipped_items;
    summary.notes = sidebar.notes;
    auto merged = SpaceStore::the().merge_imported(std::move(sidebar.spaces), sidebar.favorites);
    summary.spaces_added = merged.spaces_added;
    summary.spaces_updated = merged.spaces_updated;
    summary.favorites = merged.favorites_added;

    // Arc's Today tabs open as regular (unpinned) tabs in their spaces, in the active window.
    QList<SessionEntry> today;
    for (auto const& tab : sidebar.today_tabs) {
        for (auto const& space : SpaceStore::the().spaces()) {
            if (space.source_id == tab.space_source_id)
                today.append({ tab.url, space.id, {} });
        }
    }
    if (auto* window = Application::the().active_window_if_any(); window && !today.isEmpty()) {
        if (auto* arc_sidebar = ArcSidebar::for_window(*window))
            summary.today_tabs = arc_sidebar->open_background_tabs(today);
    }

    // Arc's ordinary bookmarks (Chromium bookmark files, one per Arc profile) -> Ladybird bookmarks.
    Builder builder;
    JsonArray bookmark_folders;
    import_chromium_bookmarks(arc_directory, builder, bookmark_folders);
    summary.bookmarks = builder.summary.bookmarks;
    if (bookmark_folders.size() > 0) {
        auto& store = WebView::Application::bookmark_store();
        QSettings arc_settings("Ladybird", "ArcChrome");
        // Replace the previous import rather than piling up duplicates.
        if (auto previous = arc_settings.value(IMPORT_ROOT_SETTING).toString(); !previous.isEmpty()) {
            auto previous_id = ak_string_from_qstring(previous);
            if (store.find_item_by_id(previous_id).has_value())
                store.remove_item(previous_id);
        }
        // import_items() wraps everything in one new top-level folder, then saves once.
        store.import_items(bookmark_folders);
        if (!store.root_items().is_empty()) {
            auto const& root = store.root_items().last();
            store.edit_folder(root.id, MUST(String::from_utf8(ROOT_FOLDER_TITLE)));
            arc_settings.setValue(IMPORT_ROOT_SETTING, qstring_from_ak_string(root.id));
        }
    }

    return summary;
}

QString describe(ImportSummary const& summary)
{
    QStringList lines;
    lines.append(QString("Spaces: %1 added, %2 updated.").arg(summary.spaces_added).arg(summary.spaces_updated));
    lines.append(QString("Pinned: %1 tabs in %2 folders.").arg(summary.tabs).arg(summary.folders));
    lines.append(QString("Favorites: %1 added.").arg(summary.favorites));
    if (summary.today_tabs > 0)
        lines.append(QString("Today: %1 open tabs reopened.").arg(summary.today_tabs));
    lines.append(QString("Site icons: %1 imported.").arg(summary.icons));
    lines.append(QString("Bookmarks: %1 (in the \"Arc\" bookmarks folder).").arg(summary.bookmarks));
    if (summary.skipped_items > 0)
        lines.append(QString("Skipped %1 Arc-only items (notes, easels).").arg(summary.skipped_items));
    for (auto const& note : summary.notes)
        lines.append(note);
    return lines.join("\n");
}

void run_import_dialog(QWidget* parent)
{
    auto directory = default_arc_data_directory();
    if (!QFile::exists(directory + "/StorableSidebar.json")) {
        QMessageBox::information(parent, "Import from Arc", "Couldn't find Arc's data on this Mac.");
        return;
    }

    auto answer = QMessageBox::question(parent, "Import from Arc",
        "Import your Arc spaces, folders, pinned tabs, favorites and bookmarks?\n\n"
        "Each Arc space becomes a space here. Importing again updates those spaces instead of duplicating them. "
        "Logins and cookies can't be imported.");
    if (answer != QMessageBox::Yes)
        return;

    auto result = import_from_arc(directory);
    if (result.is_error()) {
        QMessageBox::warning(parent, "Import from Arc", QString("Import failed: %1").arg(qstring_from_ak_string(result.error().string_literal())));
        return;
    }
    QMessageBox::information(parent, "Import from Arc", describe(result.value()));
}

}
