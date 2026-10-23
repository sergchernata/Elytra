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
#include <UI/Qt/Arc/ArcImporter.h>
#include <UI/Qt/StringUtils.h>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QSettings>
#include <QUuid>

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

// --- StorableSidebar.json: spaces, pinned folders and tabs, favorites -------------------------

class SidebarReader {
public:
    SidebarReader(JsonObject const& container, Builder& builder)
        : m_builder(builder)
    {
        if (auto items = container.get_array("items"sv); items.has_value()) {
            items->for_each([&](JsonValue const& value) {
                if (!value.is_object())
                    return;
                auto const& item = value.as_object();
                if (auto id = item.get_string("id"sv); id.has_value())
                    m_items.set(*id, &item);
            });
        }
    }

    // Converts the children of a container (pinned/unpinned/favorites) into bookmark items.
    JsonArray children_of(StringView container_id)
    {
        JsonArray result;
        auto item = m_items.get(container_id);
        if (!item.has_value())
            return result;
        auto children = (*item)->get_array("childrenIds"sv);
        if (!children.has_value())
            return result;

        children->for_each([&](JsonValue const& child_id) {
            if (!child_id.is_string())
                return;
            auto child = m_items.get(child_id.as_string());
            if (!child.has_value())
                return;
            if (auto converted = convert(**child); converted.has_value())
                result.must_append(converted.release_value());
        });
        return result;
    }

private:
    Optional<JsonObject> convert(JsonObject const& item)
    {
        auto data = item.get_object("data"sv);
        if (!data.has_value())
            return {};
        auto custom_title = string_or_empty(item, "title"sv);

        if (auto tab = data->get_object("tab"sv); tab.has_value()) {
            auto url = string_or_empty(*tab, "savedURL"sv).value_or({});
            auto title = custom_title.has_value() ? *custom_title : string_or_empty(*tab, "savedTitle"sv).value_or({});
            auto result = m_builder.bookmark(url, title);
            if (result.has_value())
                ++m_builder.summary.tabs;
            return result;
        }

        if (data->has_object("list"sv)) {
            auto id = item.get_string("id"sv);
            ++m_builder.summary.folders;
            return m_builder.folder(custom_title.value_or("Untitled folder"_string), children_of(id.has_value() ? id->bytes_as_string_view() : ""sv));
        }

        // Easels, notes and other Arc-only item types have no Ladybird equivalent.
        return {};
    }

    Builder& m_builder;
    HashMap<String, JsonObject const*> m_items;
};

static QString profile_directory(JsonValue const& profile)
{
    if (profile.is_object()) {
        if (auto custom = profile.as_object().get_object("custom"sv); custom.has_value()) {
            if (auto inner = custom->get_object("_0"sv); inner.has_value()) {
                if (auto name = inner->get_string("directoryBasename"sv); name.has_value())
                    return qstring_from_ak_string(*name);
            }
        }
    }
    return "Default";
}

static QString space_color(JsonObject const& space)
{
    auto info = space.get_object("customInfo"sv);
    if (!info.has_value())
        return {};
    auto theme = info->get_object("windowTheme"sv);
    if (!theme.has_value())
        return {};
    auto palette = theme->get_object("primaryColorPalette"sv);
    if (!palette.has_value())
        return {};
    auto tone = palette->get_object("midTone"sv);
    if (!tone.has_value())
        return {};
    auto channel = [&](StringView key) { return qBound(0.0, tone->get_double_with_precision_loss(key).value_or(0), 1.0); };
    return QColor::fromRgbF(channel("red"sv), channel("green"sv), channel("blue"sv)).name();
}

static QString space_icon(JsonObject const& space)
{
    auto info = space.get_object("customInfo"sv);
    if (!info.has_value())
        return {};
    auto icon_type = info->get_object("iconType"sv);
    if (!icon_type.has_value())
        return {};
    for (auto key : { "emoji_v2"sv, "emoji"sv, "icon"sv }) {
        if (auto value = icon_type->get_string(key); value.has_value())
            return qstring_from_ak_string(*value);
    }
    return {};
}

static void import_sidebar(QString const& arc_directory, Builder& builder, JsonArray& root_children, QSettings& arc_settings)
{
    auto json = read_json(arc_directory + "/StorableSidebar.json");
    if (json.is_error() || !json.value().is_object())
        return;

    auto sidebar = json.value().as_object().get_object("sidebar"sv);
    if (!sidebar.has_value())
        return;
    auto containers = sidebar->get_array("containers"sv);
    if (!containers.has_value())
        return;

    JsonObject const* container = nullptr;
    containers->for_each([&](JsonValue const& value) {
        if (value.is_object() && value.as_object().has_array("spaces"sv))
            container = &value.as_object();
    });
    if (!container)
        return;

    SidebarReader reader(*container, builder);

    arc_settings.remove("spaces");
    arc_settings.beginWriteArray("spaces");
    int space_index = 0;

    container->get_array("spaces"sv)->for_each([&](JsonValue const& value) {
        if (!value.is_object())
            return;
        auto const& space = value.as_object();
        auto title = string_or_empty(space, "title"sv).value_or("Space"_string);

        Optional<String> pinned_id;
        Optional<String> unpinned_id;
        if (auto ids = space.get_array("containerIDs"sv); ids.has_value()) {
            for_each_pair(*ids, [&](JsonValue const& key, JsonValue const& id) {
                if (!key.is_string() || !id.is_string())
                    return;
                if (key.as_string() == "pinned"sv)
                    pinned_id = id.as_string();
                else if (key.as_string() == "unpinned"sv)
                    unpinned_id = id.as_string();
            });
        }

        auto children = pinned_id.has_value() ? reader.children_of(*pinned_id) : JsonArray {};
        if (unpinned_id.has_value()) {
            if (auto today = reader.children_of(*unpinned_id); today.size() > 0)
                children.must_append(builder.folder("Today"sv, move(today)));
        }

        auto folder_id = new_id();
        root_children.must_append(builder.folder(title, move(children), folder_id));
        ++builder.summary.spaces;

        JsonValue profile;
        if (auto value = space.get("profile"sv); value.has_value())
            profile = *value;

        arc_settings.setArrayIndex(space_index++);
        arc_settings.setValue("bookmark_folder_id", qstring_from_ak_string(folder_id));
        arc_settings.setValue("title", qstring_from_ak_string(title));
        arc_settings.setValue("color", space_color(space));
        arc_settings.setValue("icon", space_icon(space));
        arc_settings.setValue("arc_profile", profile_directory(profile));
    });
    arc_settings.endArray();

    // Favorites (Arc's "top apps") are per profile, not per space.
    if (auto top_apps = container->get_array("topAppsContainerIDs"sv); top_apps.has_value()) {
        for_each_pair(*top_apps, [&](JsonValue const& profile, JsonValue const& id) {
            if (!id.is_string())
                return;
            auto favorites = reader.children_of(id.as_string());
            if (favorites.size() == 0)
                return;
            auto title = ak_string_from_qstring(QString("Favorites (%1)").arg(profile_directory(profile)));
            root_children.must_append(builder.folder(title, move(favorites)));
        });
    }
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

ErrorOr<ImportSummary> import_from_arc(QString const& arc_directory)
{
    if (!QFile::exists(arc_directory + "/StorableSidebar.json"))
        return Error::from_string_literal("Arc data not found");

    Builder builder;
    JsonArray root_children;
    QSettings arc_settings("Ladybird", "ArcChrome");

    import_sidebar(arc_directory, builder, root_children, arc_settings);
    import_chromium_bookmarks(arc_directory, builder, root_children);

    if (root_children.size() == 0)
        return Error::from_string_literal("No spaces or bookmarks found in Arc data");

    auto& store = WebView::Application::bookmark_store();

    // Replace the previous import rather than piling up duplicates.
    if (auto previous = arc_settings.value(IMPORT_ROOT_SETTING).toString(); !previous.isEmpty()) {
        auto previous_id = ak_string_from_qstring(previous);
        if (store.find_item_by_id(previous_id).has_value())
            store.remove_item(previous_id);
    }

    // import_items() wraps everything in one new top-level folder, then saves once.
    store.import_items(root_children);
    if (store.root_items().is_empty())
        return Error::from_string_literal("Import failed");

    auto const& root = store.root_items().last();
    store.edit_folder(root.id, MUST(String::from_utf8(ROOT_FOLDER_TITLE)));
    arc_settings.setValue(IMPORT_ROOT_SETTING, qstring_from_ak_string(root.id));

    return builder.summary;
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
        "They go into an \"Arc\" bookmarks folder. Importing again replaces the previous import.");
    if (answer != QMessageBox::Yes)
        return;

    auto result = import_from_arc(directory);
    if (result.is_error()) {
        QMessageBox::warning(parent, "Import from Arc", QString("Import failed: %1").arg(qstring_from_ak_string(result.error().string_literal())));
        return;
    }

    auto const& summary = result.value();
    QMessageBox::information(parent, "Import from Arc",
        QString("Imported %1 spaces, %2 folders, %3 tabs and %4 bookmarks.")
            .arg(summary.spaces)
            .arg(summary.folders)
            .arg(summary.tabs)
            .arg(summary.bookmarks));
}

}
