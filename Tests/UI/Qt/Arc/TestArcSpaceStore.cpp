/*
 * Tests for the Arc fork's spaces data: spaces, the pinned tree of folders and tabs, favorites, and
 * persistence.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/Arc/SpaceStore.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using namespace Ladybird::Arc;

static QString store_path(QTemporaryDir const& directory)
{
    return directory.filePath("ArcSpaces.json");
}

static QStringList titles(QList<PinnedNode> const& nodes)
{
    QStringList result;
    for (auto const& node : nodes)
        result.append(node.is_folder() ? node.title : node.saved_title);
    return result;
}

TEST_CASE(new_store_starts_with_one_symbol_space)
{
    QTemporaryDir directory;
    SpaceStore store(store_path(directory));

    EXPECT(store.is_new_profile());
    EXPECT_EQ(store.spaces().size(), 1);
    EXPECT(store.spaces().first().icon == "sf:house");
    EXPECT(QFile::exists(store_path(directory)));

    SpaceStore reopened(store_path(directory));
    EXPECT(!reopened.is_new_profile());
    EXPECT_EQ(reopened.spaces().size(), 1);
}

TEST_CASE(pinned_tabs_and_folders_can_be_added_moved_and_removed)
{
    QTemporaryDir directory;
    SpaceStore store(store_path(directory));
    auto space = store.first_space_id();

    auto folder = store.add_folder(space, {}, -1, "Reading");
    auto first = store.add_pinned_tab(space, {}, -1, "https://example.com/", "Example");
    auto second = store.add_pinned_tab(space, folder, -1, "https://example.org/", "Example Org");

    EXPECT(titles(store.space(space)->pinned) == QStringList({ "Reading", "Example" }));
    EXPECT(titles(store.node(folder)->children) == QStringList({ "Example Org" }));

    // Into the folder, ahead of what's there.
    EXPECT(store.move_node(first, space, folder, 0));
    EXPECT(titles(store.space(space)->pinned) == QStringList({ "Reading" }));
    EXPECT(titles(store.node(folder)->children) == QStringList({ "Example", "Example Org" }));

    auto location = store.location_of(second);
    EXPECT(location.space_id == space);
    EXPECT(location.parent_id == folder);
    EXPECT_EQ(location.index, 1);

    store.remove_node(folder);
    EXPECT(store.space(space)->pinned.isEmpty());
    EXPECT(store.node(first) == nullptr);
}

TEST_CASE(moving_within_the_same_list_accounts_for_the_removed_slot)
{
    QTemporaryDir directory;
    SpaceStore store(store_path(directory));
    auto space = store.first_space_id();
    auto a = store.add_pinned_tab(space, {}, -1, "https://a.example/", "A");
    store.add_pinned_tab(space, {}, -1, "https://b.example/", "B");
    store.add_pinned_tab(space, {}, -1, "https://c.example/", "C");

    // Dropping A "above C" (index 2) lands it between B and C.
    EXPECT(store.move_node(a, space, {}, 2));
    EXPECT(titles(store.space(space)->pinned) == QStringList({ "B", "A", "C" }));
}

TEST_CASE(a_folder_cannot_move_into_itself_or_its_children)
{
    QTemporaryDir directory;
    SpaceStore store(store_path(directory));
    auto space = store.first_space_id();
    auto outer = store.add_folder(space, {}, -1, "Outer");
    auto inner = store.add_folder(space, outer, -1, "Inner");

    EXPECT(!store.move_node(outer, space, outer, -1));
    EXPECT(!store.move_node(outer, space, inner, -1));
    // Nothing moved.
    EXPECT_EQ(store.location_of(outer).index, 0);
    EXPECT(store.location_of(inner).parent_id == outer);
}

TEST_CASE(nodes_can_move_between_spaces)
{
    QTemporaryDir directory;
    SpaceStore store(store_path(directory));
    auto personal = store.first_space_id();
    auto work = store.add_space("Work", "sf:briefcase", QColor("#9b7fd4"));
    auto tab = store.add_pinned_tab(personal, {}, -1, "https://example.com/", "Example");

    EXPECT(store.move_node(tab, work, {}, -1));
    EXPECT(store.space(personal)->pinned.isEmpty());
    EXPECT(store.location_of(tab).space_id == work);
}

TEST_CASE(state_survives_a_reload)
{
    QTemporaryDir directory;
    QString folder;
    QString work;
    {
        SpaceStore store(store_path(directory));
        auto space = store.first_space_id();
        folder = store.add_folder(space, {}, -1, "Reading");
        store.add_pinned_tab(space, folder, -1, "https://example.com/", "Example");
        store.set_folder_expanded(folder, false);
        work = store.add_space("Work", "sf:briefcase", QColor("#9b7fd4"));
        store.add_favorite("https://ladybird.org/", "Ladybird");
        store.set_last_active_space_id(work);
    }

    SpaceStore store(store_path(directory));
    EXPECT_EQ(store.spaces().size(), 2);
    EXPECT(store.spaces()[1].title == "Work");
    EXPECT(store.spaces()[1].icon == "sf:briefcase");
    EXPECT(store.spaces()[1].color == QColor("#9b7fd4"));
    EXPECT(store.last_active_space_id() == work);
    auto const* reading = store.node(folder);
    VERIFY(reading);
    EXPECT(!reading->expanded);
    EXPECT_EQ(reading->children.size(), 1);
    EXPECT(reading->children.first().url == "https://example.com/");
    EXPECT_EQ(store.favorites().size(), 1);
    EXPECT(store.favorites().first().title == "Ladybird");
}

TEST_CASE(the_old_house_emoji_icon_becomes_a_symbol)
{
    QTemporaryDir directory;
    QJsonObject space { { "id", "s1" }, { "title", "Personal" }, { "icon", QString::fromUtf8("\xF0\x9F\x8F\xA0") }, { "pinned", QJsonArray {} } };
    QFile file(store_path(directory));
    VERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject { { "spaces", QJsonArray { space } } }).toJson());
    file.close();

    SpaceStore store(store_path(directory));
    EXPECT(store.spaces().first().icon == "sf:house");
}

TEST_CASE(the_last_space_cannot_be_removed)
{
    QTemporaryDir directory;
    SpaceStore store(store_path(directory));
    store.remove_space(store.first_space_id());
    EXPECT_EQ(store.spaces().size(), 1);

    auto work = store.add_space("Work", {}, QColor());
    store.remove_space(work);
    EXPECT_EQ(store.spaces().size(), 1);
}

TEST_CASE(favorites_keep_their_order)
{
    QTemporaryDir directory;
    SpaceStore store(store_path(directory));
    auto a = store.add_favorite("https://a.example/", "A");
    store.add_favorite("https://b.example/", "B");
    store.add_favorite("https://c.example/", "C", 0);

    auto order = [&] {
        QStringList result;
        for (auto const& favorite : store.favorites())
            result.append(favorite.title);
        return result;
    };
    EXPECT(order() == QStringList({ "C", "A", "B" }));
    store.move_favorite(a, 2);
    EXPECT(order() == QStringList({ "C", "B", "A" }));
    store.remove_favorite(a);
    EXPECT(order() == QStringList({ "C", "B" }));
}

TEST_CASE(view_state_changes_do_not_trigger_a_rebuild)
{
    QTemporaryDir directory;
    SpaceStore store(store_path(directory));
    auto space = store.first_space_id();
    auto folder = store.add_folder(space, {}, -1, "Reading");
    auto tab = store.add_pinned_tab(space, {}, -1, "https://example.com/", "Example");

    int changes = 0;
    QObject::connect(&store, &SpaceStore::changed, [&] { ++changes; });

    // Folder expansion, page titles and the active space are bookkeeping: the sidebar already shows them.
    store.set_folder_expanded(folder, false);
    store.set_saved_title(tab, "New Title");
    store.set_last_active_space_id(space);
    EXPECT_EQ(changes, 0);

    store.rename_node(folder, "Later");
    EXPECT_EQ(changes, 1);
}
