/*
 * Edge cases of the Arc fork's sidebar logic, widgets and update installer.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/Arc/AddressBar.h>
#include <UI/Qt/Arc/ArcIcons.h>
#include <UI/Qt/Arc/ArcShortcuts.h>
#include <UI/Qt/Arc/ReleaseInfo.h>
#include <UI/Qt/Arc/SidebarTree.h>
#include <UI/Qt/Arc/SymbolButton.h>
#include <UI/Qt/Arc/SymbolIcon.h>
#include <UI/Qt/Arc/TabMatching.h>

#include <QApplication>
#include <QClipboard>
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
    // Where the pinned URL redirected to is home too: away only once the tab leaves it.
    EXPECT(!pinned_tab_is_away("https://example.com/", "https://example.com/home?session=1", "https://example.com/home?session=1"));
    EXPECT(!pinned_tab_is_away("https://example.com/", "https://example.com/", "https://example.com/home"));
    EXPECT(pinned_tab_is_away("https://example.com/", "https://example.com/other", "https://example.com/home"));
    EXPECT(pinned_tab_is_away("https://example.com/", "https://example.com/other", ""));
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

// --- Address bar ---------------------------------------------------------------------------------

TEST_CASE(address_bar_shows_the_host_and_the_rest_of_the_url)
{
    auto parts = display_url_parts(QUrl("https://www.youtube.com/watch?v=jdm8qnlnzqc"));
    EXPECT(parts.host == "youtube.com");
    EXPECT(parts.rest == "/watch?v=jdm8qnlnzqc");

    parts = display_url_parts(QUrl("https://example.com/"));
    EXPECT(parts.host == "example.com" && parts.rest.isEmpty()); // A bare "/" isn't shown.
    parts = display_url_parts(QUrl("https://example.com"));
    EXPECT(parts.host == "example.com" && parts.rest.isEmpty());

    parts = display_url_parts(QUrl("http://localhost:8080/a#top"));
    EXPECT(parts.host == "localhost:8080"); // A non-default port stays.
    EXPECT(parts.rest == "/a#top");
    parts = display_url_parts(QUrl("https://example.com:443/a"));
    EXPECT(parts.host == "example.com"); // A default port written out isn't shown.
    EXPECT(parts.rest == "/a");

    parts = display_url_parts(QUrl("https://wwwexample.com/"));
    EXPECT(parts.host == "wwwexample.com"); // Only a "www." label is dropped.

    // An international name shows as it is sent (punycode): lookalike letters from another script can't pass for
    // the site they imitate.
    parts = display_url_parts(QUrl(QString::fromUtf8("https://\xD0\xB0\xD1\x80\xD1\x80\xD3\x8F\xD0\xB5.com/")));
    EXPECT(parts.host == "xn--80ak6aa92e.com");
    parts = display_url_parts(QUrl("https://xn--80ak6aa92e.com/"));
    EXPECT(parts.host == "xn--80ak6aa92e.com");

    parts = display_url_parts(QUrl("https://example.com/caf%C3%A9?q=a%20b"));
    EXPECT(parts.rest == QString::fromUtf8("/café?q=a b")); // Readable, not percent-encoded.

    // Without a host: the whole URL.
    parts = display_url_parts(QUrl("about:blank"));
    EXPECT(parts.host == "about:blank" && parts.rest.isEmpty());
    parts = display_url_parts(QUrl("file:///Users/someone/page.html"));
    EXPECT(parts.host == "file:///Users/someone/page.html");

    parts = display_url_parts(QUrl());
    EXPECT(parts.host.isEmpty() && parts.rest.isEmpty());
}

static void send_mouse(QWidget& widget, QEvent::Type type, QPointF position)
{
    auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    auto buttons = type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton;
    QMouseEvent event(type, position, widget.mapToGlobal(position), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

static void click(QWidget& widget, QPointF position)
{
    send_mouse(widget, QEvent::MouseMove, position);
    send_mouse(widget, QEvent::MouseButtonPress, position);
    send_mouse(widget, QEvent::MouseButtonRelease, position);
}

TEST_CASE(address_bar_lays_out_the_url_centered_with_its_icon)
{
    AddressBar bar;
    bar.resize(800, AddressBar::HEIGHT);
    bar.set_url(QUrl("https://www.youtube.com/watch?v=jdm8qnlnzqc"));
    EXPECT(bar.shown_host() == "youtube.com");
    EXPECT(bar.shown_rest() == "/watch?v=jdm8qnlnzqc");

    auto icon = bar.icon_rect();
    auto text = bar.text_rect();
    EXPECT(!icon.isEmpty() && !text.isEmpty());
    EXPECT(icon.right() < text.left()); // Icon first.
    // Centered (no toolbar here: on the bar itself), icon pill to text pill.
    auto content_center = (bar.icon_pill_rect().left() + bar.pill_rect().right()) / 2;
    EXPECT(qAbs(content_center - 400) < 6);
    // The two hover pills don't touch, and both sit inside the bar.
    EXPECT(bar.icon_pill_rect().right() < bar.pill_rect().left());
    EXPECT(bar.icon_pill_rect().left() >= 0 && bar.pill_rect().right() <= bar.width());
    EXPECT(bar.pill_rect().contains(text));
    EXPECT(bar.icon_pill_rect().contains(icon));
}

TEST_CASE(address_bar_drops_the_path_before_the_host_when_narrow)
{
    AddressBar bar;
    bar.show(); // Hidden widgets get their resize events only when shown.
    bar.set_url(QUrl("https://example.com/a/very/long/path/that/does/not/fit/anywhere/near/here?with=a&long=query"));
    bar.resize(330, AddressBar::HEIGHT);
    EXPECT(bar.shown_host() == "example.com");
    EXPECT(bar.shown_rest().endsWith(QChar(0x2026))); // Elided, still there.
    EXPECT(bar.pill_rect().right() <= bar.width());

    bar.resize(150, AddressBar::HEIGHT);
    EXPECT(bar.shown_rest().isEmpty()); // No room for a useful bit of path.
    EXPECT(bar.shown_host() == "example.com" || bar.shown_host().endsWith(QChar(0x2026)));
    EXPECT(bar.pill_rect().right() <= bar.width());

    bar.resize(20, AddressBar::HEIGHT); // Less than nothing: no crash, nothing laid out.
    EXPECT(bar.text_rect().isEmpty() || bar.pill_rect().right() <= bar.width() + 1);

    bar.resize(800, AddressBar::HEIGHT); // Room again: all of it.
    EXPECT(!bar.shown_rest().endsWith(QChar(0x2026)) && bar.shown_rest().startsWith("/a/very"));
}

TEST_CASE(address_bar_shows_nothing_without_a_url)
{
    AddressBar bar;
    bar.resize(600, AddressBar::HEIGHT);
    bar.set_url(QUrl("https://example.com/x"));
    EXPECT(!bar.text_rect().isEmpty());
    bar.set_url(QUrl());
    EXPECT(bar.text_rect().isEmpty() && bar.icon_rect().isEmpty() && bar.pill_rect().isEmpty());

    // Clicking where the URL was does nothing.
    int edits = 0;
    QObject::connect(&bar, &AddressBar::edit_requested, [&] { ++edits; });
    click(bar, QPointF(300, AddressBar::HEIGHT / 2.0));
    EXPECT(edits == 0);
}

TEST_CASE(address_bar_hover_pills_follow_the_mouse)
{
    AddressBar bar;
    bar.resize(800, AddressBar::HEIGHT);
    bar.set_url(QUrl("https://example.com/page"));

    send_mouse(bar, QEvent::MouseMove, bar.text_rect().center());
    EXPECT(bar.is_text_hovered() && !bar.is_icon_hovered());
    send_mouse(bar, QEvent::MouseMove, bar.icon_rect().center());
    EXPECT(bar.is_icon_hovered() && !bar.is_text_hovered());
    EXPECT(bar.toolTip() == "Copy Link");
    send_mouse(bar, QEvent::MouseMove, QPointF(5, 5)); // Empty space.
    EXPECT(!bar.is_icon_hovered() && !bar.is_text_hovered());
    EXPECT(bar.toolTip().isEmpty());

    send_mouse(bar, QEvent::MouseMove, bar.text_rect().center());
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&bar, &leave);
    EXPECT(!bar.is_text_hovered()); // Leaving clears it.
}

TEST_CASE(address_bar_clicks_edit_or_copy)
{
    AddressBar bar;
    bar.resize(800, AddressBar::HEIGHT);
    bar.set_url(QUrl("https://example.com/a%20b?q=1"));
    int edits = 0;
    int copies = 0;
    QObject::connect(&bar, &AddressBar::edit_requested, [&] { ++edits; });
    QObject::connect(&bar, &AddressBar::url_copied, [&] { ++copies; });

    click(bar, bar.text_rect().center());
    EXPECT(edits == 1 && copies == 0);

    click(bar, bar.icon_rect().center());
    EXPECT(copies == 1 && edits == 1);
    EXPECT(bar.shows_copied());
    // Offscreen, the clipboard is the test's own (never the desktop's): check what was copied.
    if (QGuiApplication::platformName() == "offscreen")
        EXPECT(QApplication::clipboard()->text() == "https://example.com/a%20b?q=1");

    // A new page clears the "copied" check.
    bar.set_url(QUrl("https://example.com/other"));
    EXPECT(!bar.shows_copied());

    // Pressing on the URL and releasing on the icon (or outside) does neither.
    send_mouse(bar, QEvent::MouseButtonPress, bar.text_rect().center());
    send_mouse(bar, QEvent::MouseButtonRelease, bar.icon_rect().center());
    send_mouse(bar, QEvent::MouseButtonPress, bar.icon_rect().center());
    send_mouse(bar, QEvent::MouseButtonRelease, QPointF(5, 5));
    EXPECT(edits == 1 && copies == 1);
}

// --- Arc's keyboard shortcuts --------------------------------------------------------------------

static Optional<ArcCommand> command_for(int key, Qt::KeyboardModifiers modifiers)
{
    auto shortcut = arc_shortcut_for(key, modifiers);
    if (!shortcut.has_value())
        return {};
    return shortcut->command;
}

TEST_CASE(arc_shortcuts_map_to_their_commands)
{
    // Qt's Ctrl is Cmd on macOS.
    EXPECT(command_for(Qt::Key_S, Qt::ControlModifier) == ArcCommand::ToggleSidebar);
    EXPECT(command_for(Qt::Key_D, Qt::ControlModifier) == ArcCommand::TogglePin);
    EXPECT(command_for(Qt::Key_Backslash, Qt::ControlModifier) == ArcCommand::FillFromOnePassword);
    EXPECT(command_for(Qt::Key_N, Qt::ControlModifier | Qt::AltModifier) == ArcCommand::NewLittleArc);
    EXPECT(command_for(Qt::Key_O, Qt::ControlModifier) == ArcCommand::OpenInSpace);
    EXPECT(command_for(Qt::Key_O, Qt::ControlModifier | Qt::AltModifier) == ArcCommand::ChooseSpace);
    EXPECT(command_for(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier) == ArcCommand::CopyUrl);
    EXPECT(command_for(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier) == ArcCommand::CopyUrlAsMarkdown);
    EXPECT(command_for(Qt::Key_V, Qt::ControlModifier | Qt::AltModifier) == ArcCommand::NewTabFromClipboard);
    EXPECT(command_for(Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier) == ArcCommand::ClearToday);
    EXPECT(command_for(Qt::Key_Down, Qt::ControlModifier | Qt::AltModifier) == ArcCommand::NextTab);
    EXPECT(command_for(Qt::Key_Up, Qt::ControlModifier | Qt::AltModifier) == ArcCommand::PreviousTab);
    EXPECT(command_for(Qt::Key_Right, Qt::ControlModifier | Qt::AltModifier) == ArcCommand::NextSpace);
    EXPECT(command_for(Qt::Key_Left, Qt::ControlModifier | Qt::AltModifier) == ArcCommand::PreviousSpace);
    EXPECT(command_for(Qt::Key_PageDown, Qt::ControlModifier) == ArcCommand::NextTab);
    EXPECT(command_for(Qt::Key_PageUp, Qt::ControlModifier) == ArcCommand::PreviousTab);
}

TEST_CASE(arc_shortcuts_take_the_shapes_macos_reports)
{
    // Arrow keys carry the keypad modifier on macOS.
    EXPECT(command_for(Qt::Key_Down, Qt::ControlModifier | Qt::AltModifier | Qt::KeypadModifier) == ArcCommand::NextTab);
    // Cmd+Shift+] arrives as }, and Shift+Tab as Backtab.
    EXPECT(command_for(Qt::Key_BraceRight, Qt::ControlModifier | Qt::ShiftModifier) == ArcCommand::NextTab);
    EXPECT(command_for(Qt::Key_BracketLeft, Qt::ControlModifier | Qt::ShiftModifier) == ArcCommand::PreviousTab);
    EXPECT(command_for(Qt::Key_BraceLeft, Qt::ControlModifier | Qt::ShiftModifier) == ArcCommand::PreviousTab);
#if defined(AK_OS_MACOS)
    auto control = Qt::MetaModifier;
#else
    auto control = Qt::ControlModifier;
#endif
    EXPECT(command_for(Qt::Key_Tab, control) == ArcCommand::NextTab);
    EXPECT(command_for(Qt::Key_Backtab, control | Qt::ShiftModifier) == ArcCommand::PreviousTab);
}

TEST_CASE(arc_shortcuts_need_their_exact_modifiers)
{
    EXPECT(!arc_shortcut_for(Qt::Key_S, Qt::NoModifier).has_value());
    EXPECT(!arc_shortcut_for(Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier).has_value()); // Save As, not ours.
    EXPECT(!arc_shortcut_for(Qt::Key_C, Qt::ControlModifier).has_value());                     // Plain copy.
    EXPECT(!arc_shortcut_for(Qt::Key_V, Qt::ControlModifier).has_value());                     // Plain paste.
    EXPECT(!arc_shortcut_for(Qt::Key_Down, Qt::ControlModifier).has_value());                  // End of a text field.
    EXPECT(!arc_shortcut_for(Qt::Key_Right, Qt::AltModifier).has_value());                     // Word right.
    EXPECT(!arc_shortcut_for(Qt::Key_T, Qt::ControlModifier).has_value());                     // Upstream's (routed already).
    EXPECT(!arc_shortcut_for(Qt::Key_I, Qt::ControlModifier | Qt::ShiftModifier).has_value()); // Dev tools stay.
}

TEST_CASE(arc_shortcuts_number_the_sidebar_items)
{
    EXPECT(arc_shortcut_for(Qt::Key_1, Qt::ControlModifier) == ArcShortcut { ArcCommand::SidebarItem, 0 });
    EXPECT(arc_shortcut_for(Qt::Key_8, Qt::ControlModifier) == ArcShortcut { ArcCommand::SidebarItem, 7 });
    EXPECT(arc_shortcut_for(Qt::Key_9, Qt::ControlModifier) == ArcShortcut { ArcCommand::SidebarItem, -1 }); // The last.
    EXPECT(!arc_shortcut_for(Qt::Key_0, Qt::ControlModifier).has_value()); // Zoom reset.
}

TEST_CASE(urls_as_markdown_links)
{
    EXPECT_EQ(url_as_markdown("Example", "https://example.com/"), "[Example](https://example.com/)");
    EXPECT_EQ(url_as_markdown("", "https://example.com/"), "[https://example.com/](https://example.com/)");
    EXPECT_EQ(url_as_markdown("[Draft] a\\b", "https://example.com/"), "[\\[Draft\\] a\\\\b](https://example.com/)");
    EXPECT_EQ(url_as_markdown("Two\nlines", "https://example.com/"), "[Two lines](https://example.com/)");
    EXPECT_EQ(url_as_markdown("Wiki", "https://en.wikipedia.org/wiki/C_(language)"), "[Wiki](https://en.wikipedia.org/wiki/C_(language))");
    EXPECT_EQ(url_as_markdown("Odd", "https://example.com/a)b"), "[Odd](https://example.com/a%29b)");
}

TEST_CASE(inline_rename_field_sits_over_the_title)
{
    QRect row(0, 100, 240, 36);
    auto field = SidebarDelegate::title_edit_rect(row);
    EXPECT(field.left() < 36 && field.left() > 16); // Clear of the icon (10 to 26), around the title (36).
    EXPECT(field.right() < row.right());
    EXPECT(field.top() > row.top() && field.bottom() < row.bottom());
}
