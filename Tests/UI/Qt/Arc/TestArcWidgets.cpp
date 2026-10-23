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

#include <QImage>
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
