/*
 * Tests for the Arc fork's sidebar styling and widgets: centered icons, uniform symbol sizes, the drawn
 * window controls, row geometry, and the downloads shelf's wording.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/Arc/FloatingPanel.h>
#include <UI/Qt/Arc/SidebarTree.h>
#include <UI/Qt/Arc/SidebarWidgets.h>
#include <UI/Qt/Arc/SymbolButton.h>
#include <UI/Qt/Arc/SymbolIcon.h>

#include <AK/Vector.h>
#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QElapsedTimer>
#include <QScrollBar>
#include <QTimer>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>

#include <cmath>

using namespace Ladybird::Arc;

// Bounding box of the visibly painted pixels, in logical coordinates.
static QRectF painted_bounds(QImage const& image)
{
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
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
    if (right < 0)
        return {};
    auto ratio = image.devicePixelRatio();
    return QRectF(left / ratio, top / ratio, (right - left + 1) / ratio, (bottom - top + 1) / ratio);
}

static QImage render(QWidget& widget)
{
    QImage image(widget.size() * 2, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(2);
    image.fill(Qt::transparent);
    // Without the window background, so only what the widget itself paints counts.
    widget.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    return image;
}

TEST_CASE(symbol_buttons_draw_their_icon_centered)
{
    // Letters work on every platform; on macOS this also covers SF Symbols.
    QStringList icons { "W" };
#if defined(AK_OS_MACOS)
    icons += { "sf:house", "sf:briefcase", "sf:plus", "sf:tray.and.arrow.down" };
#endif
    for (auto const& icon : icons) {
        SymbolButton button(nullptr);
        button.set_icon_text(icon);
        auto bounds = painted_bounds(render(button));
        VERIFY(!bounds.isEmpty());

        auto center = QRectF(button.rect()).center();
        EXPECT(std::abs(bounds.center().x() - center.x()) <= 1.5);
        EXPECT(std::abs(bounds.center().y() - center.y()) <= 1.5);
    }
}

#if defined(AK_OS_MACOS)
TEST_CASE(sidebar_symbols_are_fitted_to_one_size)
{
    // Wide, tall and square symbols all end up with their larger side exactly ICON_BOX.
    for (auto const* name : { "house", "briefcase", "plus", "tray.and.arrow.down", "sidebar.left", "folder" }) {
        auto pixmap = fitted_symbol_pixmap(name, ICON_BOX, Qt::black, 2);
        VERIFY(!pixmap.isNull());
        auto size = pixmap.deviceIndependentSize();
        EXPECT(std::abs(qMax(size.width(), size.height()) - ICON_BOX) <= 0.5);
    }
}

TEST_CASE(sidebar_symbols_prefer_the_filled_variant)
{
    auto opaque_pixels = [](QPixmap const& pixmap) {
        auto image = pixmap.toImage();
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x)
                count += qAlpha(image.pixel(x, y)) > 128;
        }
        return count;
    };
    auto fitted = fitted_symbol_pixmap("star", ICON_BOX, Qt::black, 2);
    auto outline = symbol_pixmap("star", ICON_BOX * 2, Qt::black, 2).scaled(fitted.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    EXPECT(opaque_pixels(fitted) > opaque_pixels(outline) * 3 / 2);
}
#endif

TEST_CASE(space_icons_are_stored_as_symbol_names)
{
    EXPECT(is_symbol_icon("sf:house"));
    EXPECT(symbol_name("sf:house") == "house");
    EXPECT(!is_symbol_icon(QString::fromUtf8("\xF0\x9F\x8F\xA0")));
}

TEST_CASE(drawn_window_controls_follow_the_native_metrics)
{
    TrafficLights lights(nullptr);
    lights.set_metrics(14, 23);
    EXPECT_EQ(lights.diameter(), 14);
    EXPECT_EQ(lights.sizeHint(), QSize(62, 18));
    EXPECT_EQ(lights.size(), lights.sizeHint());

    // Docked, the real controls sit over them: draw nothing, take no clicks.
    lights.set_drawn(false);
    EXPECT(lights.testAttribute(Qt::WA_TransparentForMouseEvents));
    EXPECT(painted_bounds(render(lights)).isEmpty());

    lights.set_drawn(true);
    EXPECT(!lights.testAttribute(Qt::WA_TransparentForMouseEvents));
    EXPECT(!painted_bounds(render(lights)).isEmpty());
}

TEST_CASE(sidebar_rows_size_and_place_their_buttons)
{
    QStandardItemModel model;
    auto* tab = new QStandardItem("Example");
    tab->setData(static_cast<int>(RowKind::TodayTab), KindRole);
    auto* new_tab = new QStandardItem;
    new_tab->setData(static_cast<int>(RowKind::NewTab), KindRole);
    model.appendRow(tab);
    model.appendRow(new_tab);

    SidebarDelegate delegate;
    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, 240, 36);
    EXPECT_EQ(delegate.sizeHint(option, tab->index()).height(), 36);
    // The New Tab row also carries the divider above it.
    EXPECT(delegate.sizeHint(option, new_tab->index()).height() > 36);

    // Close and chevron buttons sit inside the row, near its right edge, vertically centered.
    auto row = QRect(0, 0, 240, 36);
    for (auto button : { SidebarDelegate::close_button_rect(row), SidebarDelegate::chevron_rect(row) }) {
        EXPECT(row.contains(button));
        EXPECT(row.right() - button.right() <= 12);
        EXPECT(std::abs(button.center().y() - row.center().y()) <= 1);
    }
}

TEST_CASE(a_loading_pinned_tab_keeps_its_saved_title_and_icon)
{
    // Until the tab shows its page, the row keeps what it showed unloaded: no "New Tab" flash.
    EXPECT(!tab_shows_a_page(""));
    EXPECT(!tab_shows_a_page("about:blank"));
    EXPECT(!tab_shows_a_page("about:newtab"));
    EXPECT(tab_shows_a_page("https://example.com/"));

    auto title = [](QString custom, QString saved, QString live, QString tab_url) {
        return pinned_row_title(custom, saved, live, "https://example.com/", tab_url);
    };
    EXPECT(title("", "Saved", "about:blank", "about:blank") == "Saved");
    EXPECT(title("", "Saved", "", "https://example.com/") == "Saved");      // Loading: no title yet.
    EXPECT(title("", "Saved", "Live", "https://example.com/") == "Live");   // Loaded.
    EXPECT(title("Mine", "Saved", "Live", "https://example.com/") == "Mine"); // Renamed wins.
    EXPECT(title("", "", "", "about:blank") == "https://example.com/");     // Nothing better: the URL.
}

TEST_CASE(a_pinned_tab_is_away_only_on_another_page)
{
    // Trivial differences don't count as navigating away.
    EXPECT(pinned_urls_match("https://example.com/app", "https://example.com/app"));
    EXPECT(pinned_urls_match("https://example.com/app", "https://example.com/app/"));
    EXPECT(pinned_urls_match("https://example.com/app/", "https://example.com/app"));
    EXPECT(pinned_urls_match("https://example.com/app", "https://example.com/app#inbox"));
    EXPECT(pinned_urls_match("https://Example.com/app", "https://example.com/app"));
    EXPECT(pinned_urls_match("https://example.com", "https://example.com/"));

    EXPECT(!pinned_urls_match("https://example.com/app", "https://example.com/app/settings"));
    EXPECT(!pinned_urls_match("https://example.com/app", "https://example.com/app?tab=2"));
    EXPECT(!pinned_urls_match("https://example.com/app", "https://other.example/app"));
    EXPECT(!pinned_urls_match("https://example.com/app", "http://example.com/app"));
    // A tab that hasn't loaded anything yet hasn't gone anywhere.
    EXPECT(pinned_urls_match("https://example.com/app", ""));

    // Nor has one still on a blank page while its pinned URL loads: no "|" flashing in.
    EXPECT(!pinned_tab_is_away("https://example.com/app", "about:blank"));
    EXPECT(!pinned_tab_is_away("https://example.com/app", ""));
    EXPECT(!pinned_tab_is_away("https://example.com/app", "https://example.com/app/"));
    EXPECT(pinned_tab_is_away("https://example.com/app", "https://example.com/other"));
}

TEST_CASE(a_pinned_tab_icon_is_a_button_with_arcs_labels)
{
    auto row = QRect(0, 40, 240, 36);
    auto button = SidebarDelegate::pinned_icon_rect(row);
    EXPECT(row.contains(button));
    EXPECT(button.left() - row.left() <= 8);
    EXPECT(button.width() == button.height());
    EXPECT(std::abs(button.center().y() - row.center().y()) <= 1);

    EXPECT(SidebarDelegate::pinned_icon_label(false) == "Back to Pinned URL");
    EXPECT(SidebarDelegate::pinned_icon_label(true) == "Separate From Pinned Tab");
}

TEST_CASE(an_away_pinned_tab_shows_a_slash_and_explains_its_icon_on_hover)
{
    QStandardItemModel model;
    auto* item = new QStandardItem("Pinned");
    item->setData(static_cast<int>(RowKind::PinnedTab), KindRole);
    item->setData(true, IsLoadedRole);
    item->setData(true, NavigatedAwayRole);
    model.appendRow(item);
    auto index = item->index();
    auto row = QRect(0, 0, 240, 36);
    auto icon = SidebarDelegate::pinned_icon_rect(row).center();
    auto title = QPoint(120, 18);

    using State = SidebarDelegate::PinnedRowState;
    EXPECT(SidebarDelegate::pinned_row_state(index, row, {}, false) == State::Away);
    EXPECT(SidebarDelegate::pinned_row_state(index, row, title, false) == State::Away);
    EXPECT(SidebarDelegate::pinned_row_state(index, row, title, true) == State::Away);
    EXPECT(SidebarDelegate::pinned_row_state(index, row, icon, false) == State::BackToPinnedUrl);
    EXPECT(SidebarDelegate::pinned_row_state(index, row, icon, true) == State::SeparateFromPinnedTab);

    item->setData(false, NavigatedAwayRole);
    EXPECT(SidebarDelegate::pinned_row_state(index, row, icon, false) == State::OnPinnedUrl);
    item->setData(true, NavigatedAwayRole);
    item->setData(false, IsLoadedRole); // Not loaded: nowhere to go back from.
    EXPECT(SidebarDelegate::pinned_row_state(index, row, icon, false) == State::OnPinnedUrl);
}

TEST_CASE(clicking_an_away_pinned_tabs_icon_goes_back_or_separates)
{
    QStandardItemModel model;
    auto add_pinned = [&](bool away) {
        auto* item = new QStandardItem("Pinned");
        item->setData(static_cast<int>(RowKind::PinnedTab), KindRole);
        item->setData(true, IsLoadedRole);
        item->setData(away, NavigatedAwayRole);
        model.appendRow(item);
        return item;
    };
    auto* away = add_pinned(true);
    auto* home = add_pinned(false);

    SidebarTree tree(nullptr);
    tree.setModel(&model);
    tree.resize(260, 200);
    tree.show();

    struct Click {
        int row;
        bool separate;
    };
    Vector<Click> icon_clicks;
    Vector<int> row_clicks;
    QObject::connect(&tree, &SidebarTree::pinned_icon_clicked, [&](QModelIndex const& index, bool separate) { icon_clicks.append({ index.row(), separate }); });
    QObject::connect(&tree, &SidebarTree::row_clicked, [&](QModelIndex const& index) { row_clicks.append(index.row()); });

    auto click = [&](QStandardItem* item, QPoint point, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        (void)item;
        auto global = tree.viewport()->mapToGlobal(point);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(global), Qt::LeftButton, Qt::LeftButton, modifiers);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(global), Qt::LeftButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(tree.viewport(), &press);
        QApplication::sendEvent(tree.viewport(), &release);
    };
    auto icon_of = [&](QStandardItem* item) { return SidebarDelegate::pinned_icon_rect(tree.visualRect(item->index())).center(); };
    auto title_of = [&](QStandardItem* item) { return tree.visualRect(item->index()).center(); };

    click(away, icon_of(away));
    click(away, icon_of(away), Qt::ShiftModifier);
    VERIFY(icon_clicks.size() == 2);
    EXPECT(icon_clicks[0].row == 0 && !icon_clicks[0].separate);
    EXPECT(icon_clicks[1].row == 0 && icon_clicks[1].separate);
    EXPECT(row_clicks.is_empty());

    // On its pinned page, or outside the icon, the row just activates as before.
    click(home, icon_of(home));
    click(away, title_of(away));
    EXPECT(icon_clicks.size() == 2);
    EXPECT(row_clicks.size() == 2);
}

TEST_CASE(the_list_ends_with_empty_space_that_scrolls_with_it)
{
    // As in Arc: room after the last row to double-click for a new tab, part of the list rather than a band that
    // cuts it off.
    QStandardItemModel model;
    auto* divider = new QStandardItem;
    divider->setData(static_cast<int>(RowKind::NewTab), KindRole);
    auto* space = new QStandardItem;
    space->setData(static_cast<int>(RowKind::BottomSpace), KindRole);
    model.appendRow(divider);
    model.appendRow(space);

    SidebarDelegate delegate;
    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, 240, 36);
    EXPECT(delegate.sizeHint(option, space->index()).height() >= 40);
    // The space isn't a Today tab: no Clear on the divider above it.
    EXPECT(!SidebarDelegate::divider_has_today_tabs(divider->index()));

    SidebarTree tree(nullptr);
    tree.setModel(&model);
    tree.resize(260, 300);
    tree.show();
    int double_clicks = 0;
    QObject::connect(&tree, &SidebarTree::background_double_clicked, [&] { ++double_clicks; });
    auto point = tree.visualRect(space->index()).center();
    QMouseEvent event(QEvent::MouseButtonDblClick, QPointF(point), QPointF(tree.viewport()->mapToGlobal(point)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree.viewport(), &event);
    EXPECT_EQ(double_clicks, 1);
}

TEST_CASE(rebuilding_the_list_keeps_its_scroll_position)
{
    QStandardItemModel model;
    auto fill = [&] {
        for (int i = 0; i < 100; ++i) {
            auto* item = new QStandardItem(QString("Tab %1").arg(i));
            item->setData(static_cast<int>(RowKind::TodayTab), KindRole);
            model.appendRow(item);
        }
    };
    fill();

    SidebarTree tree(nullptr);
    tree.setModel(&model);
    tree.resize(260, 300);
    tree.show();
    QApplication::processEvents();
    tree.verticalScrollBar()->setValue(900);
    VERIFY(tree.verticalScrollBar()->value() == 900);

    // The list mustn't move at all on the way, not even for a frame: no intermediate positions.
    Vector<int> positions;
    auto connection = QObject::connect(tree.verticalScrollBar(), &QScrollBar::valueChanged, [&](int value) { positions.append(value); });
    tree.rebuild_keeping_scroll_position([&] {
        model.clear();
        fill();
    });
    EXPECT_EQ(tree.verticalScrollBar()->value(), 900);
    QApplication::processEvents(); // And it stays there once the view settles.
    EXPECT_EQ(tree.verticalScrollBar()->value(), 900);
    QObject::disconnect(connection);
    EXPECT(positions.is_empty());
}

TEST_CASE(clicking_a_row_that_rebuilds_the_list_keeps_its_scroll_position)
{
    // As in the sidebar: folders with tabs, and a click whose handling rebuilds the whole list.
    QStandardItemModel model;
    auto fill = [&] {
        for (int f = 0; f < 10; ++f) {
            auto* folder = new QStandardItem(QString("Folder %1").arg(f));
            folder->setData(static_cast<int>(RowKind::Folder), KindRole);
            folder->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            for (int t = 0; t < 5; ++t) {
                auto* tab = new QStandardItem(QString("Tab %1.%2").arg(f).arg(t));
                tab->setData(static_cast<int>(RowKind::PinnedTab), KindRole);
                tab->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
                folder->appendRow(tab);
            }
            model.appendRow(folder);
        }
    };
    fill();

    SidebarTree tree(nullptr);
    tree.setModel(&model);
    tree.resize(260, 300);
    tree.expandAll();
    tree.show();
    QApplication::processEvents();
    tree.verticalScrollBar()->setValue(1000);
    QApplication::processEvents();
    auto scrolled = tree.verticalScrollBar()->value();
    VERIFY(scrolled > 500);

    // As in the sidebar, the rebuild runs after the click, from the event loop (twice: tab added, then loaded).
    auto rebuild = [&] {
        tree.rebuild_keeping_scroll_position([&] {
            model.clear();
            fill();
            tree.expandAll();
        });
    };
    QObject::connect(&tree, &SidebarTree::row_clicked, [&](QModelIndex const&) {
        QTimer::singleShot(0, &tree, rebuild);
        QTimer::singleShot(30, &tree, rebuild);
    });
    auto point = QPoint(120, 150);
    VERIFY(tree.indexAt(point).isValid());
    auto global = tree.viewport()->mapToGlobal(point);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(global), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(global), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree.viewport(), &press);
    QApplication::sendEvent(tree.viewport(), &release);
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 200)
        QApplication::processEvents();
    EXPECT_EQ(tree.verticalScrollBar()->value(), scrolled);
}

TEST_CASE(a_list_rebuilt_while_hidden_keeps_its_scroll_position)
{
    // The floating sidebar is hidden or squeezed while it retracts; a rebuild then must not lose the position.
    QStandardItemModel model;
    auto fill = [&] {
        for (int i = 0; i < 100; ++i) {
            auto* item = new QStandardItem(QString("Tab %1").arg(i));
            item->setData(static_cast<int>(RowKind::TodayTab), KindRole);
            model.appendRow(item);
        }
    };
    fill();
    SidebarTree tree(nullptr);
    tree.setModel(&model);
    tree.resize(260, 300);
    tree.show();
    QApplication::processEvents();
    tree.verticalScrollBar()->setValue(900);

    for (auto squeeze : { false, true }) {
        if (squeeze)
            tree.resize(260, 0);
        else
            tree.hide();
        QApplication::processEvents();
        tree.rebuild_keeping_scroll_position([&] {
            model.clear();
            fill();
        });
        QApplication::processEvents();
        tree.resize(260, 300);
        tree.show();
        for (int i = 0; i < 5; ++i)
            QApplication::processEvents();
        EXPECT_EQ(tree.verticalScrollBar()->value(), 900);
    }
}

TEST_CASE(clicking_the_last_row_at_the_bottom_keeps_the_list_at_the_bottom)
{
    QStandardItemModel model;
    auto fill = [&] {
        for (int i = 0; i < 60; ++i) {
            auto* item = new QStandardItem(QString("Tab %1").arg(i));
            item->setData(static_cast<int>(RowKind::TodayTab), KindRole);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
            model.appendRow(item);
        }
        auto* space = new QStandardItem;
        space->setData(static_cast<int>(RowKind::BottomSpace), KindRole);
        model.appendRow(space);
    };
    fill();
    SidebarTree tree(nullptr);
    tree.setModel(&model);
    tree.resize(260, 300);
    tree.show();
    QApplication::processEvents();
    auto* bar = tree.verticalScrollBar();
    bar->setValue(bar->maximum());
    auto bottom = bar->value();
    VERIFY(bottom > 0);

    Vector<int> positions;
    QObject::connect(bar, &QScrollBar::valueChanged, [&](int value) { positions.append(value); });
    auto rebuild = [&] {
        tree.rebuild_keeping_scroll_position([&] {
            model.clear();
            fill();
        });
    };
    QObject::connect(&tree, &SidebarTree::row_clicked, [&](QModelIndex const&) {
        QTimer::singleShot(0, &tree, rebuild);
        QTimer::singleShot(30, &tree, rebuild);
    });

    // The last tab, just above the empty room at the end.
    auto last = model.index(59, 0);
    auto point = tree.visualRect(last).center();
    VERIFY(tree.viewport()->rect().contains(point));
    auto global = tree.viewport()->mapToGlobal(point);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(global), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(global), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree.viewport(), &press);
    QApplication::sendEvent(tree.viewport(), &release);
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 200)
        QApplication::processEvents();

    EXPECT_EQ(bar->value(), bottom);
    EXPECT(positions.is_empty());
}

TEST_CASE(divider_row_has_a_rule_strip_with_clear_at_its_end)
{
    auto row = QRect(0, 100, 240, 58);
    auto strip = SidebarDelegate::divider_strip_rect(row);
    auto clear = SidebarDelegate::clear_button_rect(row);

    // The strip sits above the New Tab button, which keeps a full row's height.
    EXPECT(strip.top() == row.top());
    EXPECT(row.bottom() - strip.bottom() >= 34);
    // Clear sits on the rule, at its right end.
    EXPECT(strip.contains(clear));
    EXPECT(row.right() - clear.right() <= 12);
    EXPECT(std::abs(clear.center().y() - strip.center().y()) <= 1);
}

TEST_CASE(downloads_shelf_wording)
{
    auto now = QDateTime(QDate(2026, 10, 23), QTime(12, 0));
    EXPECT(downloads_relative_time(now.addSecs(-30), now) == "Just now");
    EXPECT(downloads_relative_time(now.addSecs(-90), now) == "1 minute ago");
    EXPECT(downloads_relative_time(now.addSecs(-5 * 60), now) == "5 minutes ago");
    EXPECT(downloads_relative_time(now.addSecs(-90 * 60), now) == "1 hour ago");
    EXPECT(downloads_relative_time(now.addSecs(-5 * 3600), now) == "5 hours ago");
    EXPECT(downloads_relative_time(now.addDays(-1), now) == "1 day ago");
    EXPECT(downloads_relative_time(now.addDays(-3), now) == "3 days ago");
    EXPECT(downloads_relative_time(now.addDays(-45), now) == "Sep 8");
}

TEST_CASE(downloads_shelf_skips_unfinished_and_hidden_files)
{
    for (auto const* name : { "movie.mp4.download", "archive.zip.part", "setup.dmg.crdownload", ".DS_Store" })
        EXPECT(is_partial_download_name(name));
    for (auto const* name : { "report.pdf", "Photo 2026.jpg", "notes" })
        EXPECT(!is_partial_download_name(name));
}
