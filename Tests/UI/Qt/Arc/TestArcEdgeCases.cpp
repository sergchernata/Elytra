/*
 * Edge cases of the Arc fork's sidebar logic, widgets and update installer.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/Arc/ArcIcons.h>
#include <UI/Qt/Arc/ReleaseInfo.h>
#include <UI/Qt/Arc/SidebarTree.h>
#include <UI/Qt/Arc/SymbolButton.h>
#include <UI/Qt/Arc/SymbolIcon.h>
#include <UI/Qt/Arc/TabMatching.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QStandardItemModel>
#include <QTemporaryDir>

using namespace Ladybird::Arc;

// --- Pinned URLs ---------------------------------------------------------------------------------

TEST_CASE(pinned_urls_ignore_default_ports_and_equivalent_encodings)
{
    EXPECT(pinned_urls_match("https://example.com/app", "https://example.com:443/app"));
    EXPECT(pinned_urls_match("http://example.com/app", "http://example.com:80/app"));
    EXPECT(!pinned_urls_match("https://example.com/app", "https://example.com:8443/app"));
    EXPECT(pinned_urls_match("https://example.com/a b", "https://example.com/a%20b"));
    EXPECT(!pinned_urls_match("https://example.com/App", "https://example.com/app")); // Paths are case-sensitive.
    EXPECT(pinned_urls_match("https://example.com/app?x=1#a", "https://example.com/app?x=1#b"));
    EXPECT(!pinned_urls_match("https://example.com/app?x=1", "https://example.com/app?x=2"));
    EXPECT(pinned_urls_match("https://example.com///", "https://example.com"));
}

TEST_CASE(away_needs_a_real_page)
{
    for (auto const* url : { "", "about:blank", "about:newtab", "about:srcdoc" })
        EXPECT(!pinned_tab_is_away("https://example.com/", url));
    EXPECT(pinned_tab_is_away("https://example.com/", "https://example.com/other"));
    EXPECT(pinned_tab_is_away("https://example.com/", "file:///tmp/x.html"));
}

TEST_CASE(pinned_row_titles_skip_blank_titles)
{
    auto pinned = QString("https://example.com/");
    EXPECT(pinned_row_title("   ", "Saved", "Live", pinned, pinned) == "Live"); // A blank custom title isn't one.
    EXPECT(pinned_row_title("", "Saved", "   ", pinned, pinned) == "Saved");   // Nor a blank page title.
    EXPECT(pinned_row_title("", "  ", "", pinned, "about:blank") == pinned);
}

// --- Releases --------------------------------------------------------------------------------------

TEST_CASE(release_tags_in_odd_shapes)
{
    EXPECT(is_newer_release("arc-v0.9.0", "arc-v0.10"));
    EXPECT(!is_newer_release("arc-v0.02.0", "arc-v0.2.0")); // Same version, written differently.
    EXPECT(!is_newer_release("arc-v0.1.0", "arc-v0.2.0-rc1")); // A prerelease-looking tag isn't offered.
    EXPECT(!is_newer_release("arc-v0.1.0", " arc-v0.2.0"));
    EXPECT(!is_newer_release("arc-v0.1.0", "ARC-V0.2.0"));
}

static void make_app(QString const& path, QByteArray const& version)
{
    QDir().mkpath(path + "/Contents");
    QFile file(path + "/Contents/version");
    VERIFY(file.open(QIODevice::WriteOnly));
    file.write(version);
}

static QByteArray app_version(QString const& path)
{
    QFile file(path + "/Contents/version");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static int run_script(QString const& script)
{
    QProcess process;
    process.start("/bin/sh", { "-c", script });
    VERIFY(process.waitForFinished(10000));
    return process.exitCode();
}

TEST_CASE(install_script_handles_awkward_paths)
{
    QTemporaryDir directory;
    auto base = directory.filePath("My Apps/it's here");
    make_app(base + "/Ladybird.app", "old");
    make_app(base + "/staged dir/Ladybird.app", "new");
    auto marker = directory.filePath("relaunched");
    EXPECT_EQ(run_script(install_script(base + "/Ladybird.app", base + "/staged dir/Ladybird.app", 999999, QString("/usr/bin/touch '%1'").arg(marker))), 0);
    EXPECT(app_version(base + "/Ladybird.app") == "new");
    EXPECT(QFile::exists(marker));
}

TEST_CASE(a_failed_install_keeps_the_old_app_and_doesnt_relaunch)
{
    QTemporaryDir directory;
    make_app(directory.filePath("Ladybird.app"), "old");
    auto marker = directory.filePath("relaunched");
    // The staged app is missing (e.g. cleaned from the cache meanwhile).
    auto exit_code = run_script(install_script(directory.filePath("Ladybird.app"), directory.filePath("gone/Ladybird.app"), 999999,
        QString("/usr/bin/touch '%1'").arg(marker)));
    EXPECT(exit_code != 0);
    EXPECT(app_version(directory.filePath("Ladybird.app")) == "old");
    EXPECT(!QFile::exists(directory.filePath("Ladybird.app.old")));
    EXPECT(!QFile::exists(marker));
}

TEST_CASE(an_install_replaces_a_leftover_backup)
{
    QTemporaryDir directory;
    make_app(directory.filePath("Ladybird.app"), "old");
    make_app(directory.filePath("Ladybird.app.old"), "older"); // From an earlier interrupted install.
    make_app(directory.filePath("staged/Ladybird.app"), "new");
    EXPECT_EQ(run_script(install_script(directory.filePath("Ladybird.app"), directory.filePath("staged/Ladybird.app"), 999999, "true")), 0);
    EXPECT(app_version(directory.filePath("Ladybird.app")) == "new");
    EXPECT(!QFile::exists(directory.filePath("Ladybird.app.old")));
}

// --- Command bar -------------------------------------------------------------------------------------

TEST_CASE(command_bar_matches_tabs_by_every_word)
{
    EXPECT(tab_matches_query("", "Anything", "https://x/"));
    EXPECT(tab_matches_query("   ", "Anything", "https://x/"));
    EXPECT(tab_matches_query("git hub", "GitHub", "https://github.com/"));
    EXPECT(tab_matches_query("GITHUB", "GitHub", "https://github.com/"));
    EXPECT(tab_matches_query("pulls", "GitHub", "https://github.com/pulls"));    // By URL.
    EXPECT(!tab_matches_query("github pulls inbox", "GitHub", "https://github.com/pulls"));
}

// --- Sidebar clicks ----------------------------------------------------------------------------------

struct Clicks {
    Vector<int> rows;
    Vector<int> closes;
    int clears { 0 };
};

static void click(SidebarTree& tree, QPoint point)
{
    auto global = tree.viewport()->mapToGlobal(point);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(global), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(global), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree.viewport(), &press);
    QApplication::sendEvent(tree.viewport(), &release);
}

TEST_CASE(close_buttons_only_close_loaded_tabs_and_the_divider_strip_does_nothing)
{
    QStandardItemModel model;
    auto add = [&](RowKind kind, bool loaded) {
        auto* item = new QStandardItem("Row");
        item->setData(static_cast<int>(kind), KindRole);
        item->setData(loaded, IsLoadedRole);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        model.appendRow(item);
        return item;
    };
    auto* unloaded = add(RowKind::PinnedTab, false);
    auto* loaded = add(RowKind::PinnedTab, true);
    auto* divider = add(RowKind::NewTab, false);
    divider->setFlags(Qt::ItemIsEnabled);

    SidebarTree tree(nullptr);
    tree.setModel(&model);
    tree.resize(260, 300);
    tree.show();
    Clicks clicks;
    QObject::connect(&tree, &SidebarTree::row_clicked, [&](QModelIndex const& index) { clicks.rows.append(index.row()); });
    QObject::connect(&tree, &SidebarTree::close_clicked, [&](QModelIndex const& index) { clicks.closes.append(index.row()); });
    QObject::connect(&tree, &SidebarTree::clear_clicked, [&] { ++clicks.clears; });

    click(tree, SidebarDelegate::close_button_rect(tree.visualRect(unloaded->index())).center());
    click(tree, SidebarDelegate::close_button_rect(tree.visualRect(loaded->index())).center());
    EXPECT(clicks.closes.size() == 1 && clicks.closes[0] == 1);
    EXPECT(clicks.rows.size() == 1 && clicks.rows[0] == 0); // The unloaded tab's spot is just the row.

    // The rule's strip: neither the New Tab button nor, with no Today tabs, a Clear.
    auto strip = SidebarDelegate::divider_strip_rect(tree.visualRect(divider->index()));
    click(tree, strip.center());
    click(tree, SidebarDelegate::clear_button_rect(tree.visualRect(divider->index())).center());
    EXPECT_EQ(clicks.rows.size(), 1u);
    EXPECT_EQ(clicks.clears, 0);

    // With a Today tab below, Clear works.
    auto* today = new QStandardItem("Today");
    today->setData(static_cast<int>(RowKind::TodayTab), KindRole);
    model.appendRow(today);
    click(tree, SidebarDelegate::clear_button_rect(tree.visualRect(divider->index())).center());
    EXPECT_EQ(clicks.clears, 1);
}

TEST_CASE(rows_have_their_heights)
{
    QStandardItemModel model;
    SidebarDelegate delegate;
    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, 240, 36);
    auto height_of = [&](RowKind kind) {
        auto* item = new QStandardItem;
        item->setData(static_cast<int>(kind), KindRole);
        model.appendRow(item);
        return delegate.sizeHint(option, item->index()).height();
    };
    EXPECT_EQ(height_of(RowKind::PinnedTab), 36);
    EXPECT_EQ(height_of(RowKind::Folder), 36);
    EXPECT_EQ(height_of(RowKind::TodayTab), 36);
    EXPECT(height_of(RowKind::NewTab) > 36);
    EXPECT(height_of(RowKind::BottomSpace) >= 40);
}

// --- Icons -----------------------------------------------------------------------------------------

TEST_CASE(site_icons_are_cached_per_site)
{
    QTemporaryDir directory;
    ArcIcons::set_directory(directory.path());
    EXPECT(!ArcIcons::has_icon("https://example.com/"));

    QImage image(16, 16, QImage::Format_ARGB32);
    image.fill(Qt::red);
    ArcIcons::store("https://example.com/some/page", QIcon(QPixmap::fromImage(image)));
    EXPECT(ArcIcons::has_icon("https://example.com/other"));      // Same site, other page.
    EXPECT(ArcIcons::has_icon("https://EXAMPLE.com/"));           // Hosts aren't case-sensitive.
    EXPECT(!ArcIcons::has_icon("https://other.example/"));
    EXPECT(!ArcIcons::icon_for_url("https://example.com/").isNull());
    EXPECT(ArcIcons::icon_for_url("https://other.example/").isNull());
    EXPECT(!ArcIcons::has_icon("about:blank"));
    ArcIcons::store_png("not a url", QByteArray("junk")); // Ignored, not crashing.
}

static double average_alpha(QWidget& widget)
{
    QImage image(widget.size() * 2, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(2);
    image.fill(Qt::transparent);
    widget.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    double total = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            total += qAlpha(image.pixel(x, y));
    }
    return total / (image.width() * image.height());
}

TEST_CASE(dimmed_buttons_are_fainter)
{
    SymbolButton normal(nullptr);
    normal.set_icon_text("W");
    SymbolButton dimmed(nullptr);
    dimmed.set_icon_text("W");
    dimmed.set_dimmed(true);
    EXPECT(average_alpha(dimmed) < average_alpha(normal) * 0.7);
}

TEST_CASE(the_skull_is_drawn_at_the_symbols_optical_size)
{
    auto pixmap = custom_glyph_pixmap("arc.skull", 20, Qt::black, 2);
    VERIFY(!pixmap.isNull());
    auto image = pixmap.toImage();
    int left = image.width(), right = -1, top = image.height(), bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) < 40)
                continue;
            left = qMin(left, x);
            right = qMax(right, x);
            top = qMin(top, y);
            bottom = qMax(bottom, y);
        }
    }
    VERIFY(right >= 0);
    auto extent = qMax(right - left + 1, bottom - top + 1) / 2.0;
    EXPECT(extent <= 20 * 0.85); // Not the whole box: a solid glyph looks bigger than outlined symbols.
    EXPECT(extent >= 20 * 0.7);
    EXPECT(custom_glyph_pixmap("arc.unknown", 20, Qt::black, 2).isNull());
}
