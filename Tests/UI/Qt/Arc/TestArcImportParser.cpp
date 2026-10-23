/*
 * Tests for reading Arc's sidebar data into Arc-fork spaces. The fixture mimics Arc's StorableSidebar.json
 * layout with made-up contents.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/Arc/ArcImportParser.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace Ladybird::Arc;

static QJsonObject tab_item(QString const& id, QString const& url, QString const& saved_title, QJsonValue custom_title = QJsonValue::Null)
{
    return QJsonObject { { "id", id }, { "title", custom_title }, { "childrenIds", QJsonArray {} },
        { "data", QJsonObject { { "tab", QJsonObject { { "savedURL", url }, { "savedTitle", saved_title } } } } } };
}

static QJsonObject container_item(QString const& id, QJsonArray children, QJsonObject data = { { "itemContainer", QJsonObject {} } }, QJsonValue title = QJsonValue::Null)
{
    return QJsonObject { { "id", id }, { "title", title }, { "childrenIds", children }, { "data", data } };
}

static QByteArray fixture()
{
    QJsonArray items;
    auto add = [&](QJsonObject const& item) {
        items.append(item["id"]);
        items.append(item);
    };
    add(container_item("work-pinned", { "folder-1", "tab-renamed" }));
    add(container_item("folder-1", { "tab-a", "note" }, { { "list", QJsonObject {} } }, "Docs"));
    add(tab_item("tab-a", "https://docs.example/a", "Doc A"));
    add(container_item("note", {}, { { "easel", QJsonObject {} } }));
    add(tab_item("tab-renamed", "https://example.com/", "Example Domain", "My Example"));
    add(container_item("work-today", { "tab-today" }));
    add(tab_item("tab-today", "https://news.example/", "News"));
    add(container_item("side-pinned", {}));
    add(container_item("side-today", {}));
    add(container_item("favorites-default", { "fav-1", "fav-1b" }));
    add(tab_item("fav-1", "https://mail.example/", "Mail"));
    add(tab_item("fav-1b", "https://mail.example/", "Mail again"));
    add(container_item("favorites-custom", { "fav-2" }));
    add(tab_item("fav-2", "https://mail.example/", "Mail"));

    QJsonObject custom_profile { { "custom", QJsonObject { { "_0", QJsonObject { { "directoryBasename", "Profile 1" } } } } } };
    QJsonObject work {
        { "id", "space-work" }, { "title", "Work" }, { "profile", QJsonObject { { "default", true } } },
        { "containerIDs", QJsonArray { "unpinned", "work-today", "pinned", "work-pinned" } },
        { "customInfo", QJsonObject { { "iconType", QJsonObject { { "icon", "flash" } } } } },
    };
    QJsonObject side {
        { "id", "space-side" }, { "title", "Side" }, { "profile", custom_profile },
        { "containerIDs", QJsonArray { "unpinned", "side-today", "pinned", "side-pinned" } },
        { "customInfo", QJsonObject {
                            { "iconType", QJsonObject { { "emoji_v2", QString::fromUtf8("\xF0\x9F\x9A\x80") } } },
                            { "windowTheme", QJsonObject { { "primaryColorPalette", QJsonObject { { "midTone", QJsonObject { { "red", 1.0 }, { "green", 0.0 }, { "blue", 0.0 } } } } } } },
                        } },
    };

    QJsonObject container {
        { "spaces", QJsonArray { "space-work", work, "space-side", side } },
        { "items", items },
        { "topAppsContainerIDs", QJsonArray { QJsonObject { { "default", true } }, "favorites-default", custom_profile, "favorites-custom" } },
    };
    QJsonObject root { { "sidebar", QJsonObject { { "containers", QJsonArray { QJsonObject { { "global", QJsonObject {} } }, container } } } } };
    return QJsonDocument(root).toJson();
}

TEST_CASE(spaces_keep_their_pinned_tree_and_order)
{
    auto data = parse_arc_sidebar(fixture());
    VERIFY(data.spaces.size() == 2);
    auto const& work = data.spaces[0];
    EXPECT(work.title == "Work");
    EXPECT(work.source_id == "space-work");
    VERIFY(work.pinned.size() == 2); // Docs and the renamed tab; Today's tabs stay unpinned.

    auto const& docs = work.pinned[0];
    EXPECT(docs.is_folder());
    EXPECT(docs.title == "Docs");
    EXPECT(!docs.expanded);
    VERIFY(docs.children.size() == 1); // The easel is skipped.
    EXPECT(docs.children[0].url == "https://docs.example/a");
    EXPECT(docs.children[0].title.isEmpty()); // Not renamed: shows the page title.
    EXPECT(docs.children[0].saved_title == "Doc A");

    EXPECT(work.pinned[1].title == "My Example"); // Renamed in Arc.
    EXPECT(work.pinned[1].saved_title == "Example Domain");

    VERIFY(data.today_tabs.size() == 1);
    EXPECT(data.today_tabs[0].space_source_id == "space-work");
    EXPECT(data.today_tabs[0].url == "https://news.example/");
    EXPECT(data.today_tabs[0].title == "News");

    EXPECT_EQ(data.tabs, 2);
    EXPECT_EQ(data.folders, 1);
    EXPECT_EQ(data.skipped_items, 1);
}

TEST_CASE(space_icons_colors_and_profiles)
{
    auto data = parse_arc_sidebar(fixture());
    VERIFY(data.spaces.size() == 2);
    EXPECT(data.spaces[0].profile.isEmpty());
    EXPECT(!data.spaces[0].color.isValid()); // No Arc theme: the importer assigns one.
#if defined(AK_OS_MACOS)
    EXPECT(data.spaces[0].icon == "sf:bolt"); // Arc's "flash".
#endif
    EXPECT(sf_symbol_for_arc_icon("skull") == "arc.skull"); // Drawn here; SF Symbols has none.
#if defined(AK_OS_MACOS)
#endif
    EXPECT(data.spaces[1].profile == "Profile 1");
    EXPECT(data.spaces[1].icon == QString::fromUtf8("\xF0\x9F\x9A\x80"));
    EXPECT(data.spaces[1].color == QColor(255, 0, 0));
}

TEST_CASE(favorites_are_per_profile_without_duplicates)
{
    auto data = parse_arc_sidebar(fixture());
    // The default profile lists the same URL twice; the custom profile has it once more, which is fine.
    VERIFY(data.favorites.size() == 2);
    EXPECT(data.favorites[0].profile.isEmpty());
    EXPECT(data.favorites[0].title == "Mail");
    EXPECT(data.favorites[1].profile == "Profile 1");
}

TEST_CASE(malformed_input_yields_nothing_rather_than_crashing)
{
    auto data = parse_arc_sidebar("not json");
    EXPECT(data.spaces.isEmpty());
    EXPECT(!data.notes.isEmpty());
}
