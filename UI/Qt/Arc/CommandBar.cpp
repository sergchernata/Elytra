/*
 * Arc-style command bar: a floating search/address panel opened with Cmd+T (or a double-click on the sidebar).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Arc/ArcIcons.h>
#include <UI/Qt/Arc/ArcSidebar.h>
#include <UI/Qt/Arc/CommandBar.h>
#include <UI/Qt/Arc/SymbolIcon.h>
#include <UI/Qt/Arc/TabMatching.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/WebContentView.h>

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace Ladybird::Arc {

static constexpr int BAR_WIDTH = 760;
static constexpr int SHADOW_MARGIN = 28;
static constexpr int CORNER_RADIUS = 14;
static constexpr int ROW_RADIUS = 8;
static constexpr int ICON_BOX = 24;
static constexpr int ARROW_BUTTON = 26;
static constexpr int TITLE_PIXEL_SIZE = 15;
static constexpr int INPUT_PIXEL_SIZE = 20;

int CommandBar::card_height_for_rows(int rows)
{
    if (rows <= 0)
        return HEADER_HEIGHT;
    return HEADER_HEIGHT + 1 + LIST_PADDING * 2 + rows * ROW_HEIGHT;
}

bool CommandBar::tab_matches(QString const& query, QString const& title, QString const& url)
{
    return tab_matches_query(query, title, url);
}

void CommandBar::toggle_for(BrowserWindow& window)
{
    auto* bar = window.findChild<CommandBar*>(QString(), Qt::FindDirectChildrenOnly);
    if (bar && bar->isVisible()) {
        bar->dismiss();
        return;
    }
    if (!bar)
        bar = new CommandBar(window);
    bar->present();
}

CommandBar::CommandBar(BrowserWindow& window)
    : QWidget(&window, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , m_window(&window)
    , m_omnibox(window.is_private())
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NativeWindow);
    setMouseTracking(true);

    m_edit = new QLineEdit(this);
    m_edit->setFrame(false);
    m_edit->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_edit->setPlaceholderText("Search or Enter URL…");
    auto font = m_edit->font();
    font.setPixelSize(INPUT_PIXEL_SIZE);
    font.setWeight(QFont::Medium);
    m_edit->setFont(font);
    m_edit->installEventFilter(this);

    m_omnibox.on_display_change = [this](WebView::Omnibox::Display const& display) {
        auto text = qstring_from_ak_string(display.text);
        if (m_edit->text() != text)
            m_edit->setText(text);
        m_edit->setCursorPosition(static_cast<int>(text.length()));
        if (display.selection_start.has_value()) {
            auto prefix = display.text.bytes_as_string_view().substring_view(0, *display.selection_start);
            auto start = static_cast<int>(qstring_from_ak_string(prefix).length());
            m_edit->cursorBackward(true, static_cast<int>(text.length()) - start);
        }
    };
    m_omnibox.on_suggestions_change = [this] { rebuild_rows(); };
    m_omnibox.on_selection_change = [this] {
        if (m_selection_is_explicit)
            return;
        if (auto selected = m_omnibox.selected_suggestion(); selected.has_value()) {
            for (int i = 0; i < m_rows.size(); ++i) {
                if (m_rows[i].kind == CommandBarRow::Kind::Suggestion && m_rows[i].suggestion_index == *selected)
                    select_row(i);
            }
        }
    };
    m_omnibox.on_commit = [this](String const& input) { commit(input); };

    connect(m_edit, &QLineEdit::textEdited, this, [this] {
        m_selection_is_explicit = false;
        auto cursor_at_end = !m_edit->hasSelectedText() && m_edit->cursorPosition() == m_edit->text().length();
        m_omnibox.text_edited(ak_string_from_qstring(m_edit->text()), cursor_at_end);
        rebuild_rows();
    });
    connect(m_edit, &QLineEdit::cursorPositionChanged, this, [this](int, int position) {
        auto at_end = position == m_edit->text().length() || (m_edit->hasSelectedText() && m_edit->selectionEnd() == m_edit->text().length());
        m_omnibox.cursor_moved(at_end);
    });

    window.installEventFilter(this);
}

CommandBar::~CommandBar()
{
    qApp->removeEventFilter(this);
}

void CommandBar::present()
{
    auto dark = ChromeStyle::is_dark(palette());
    auto edit_palette = m_edit->palette();
    edit_palette.setColor(QPalette::Base, Qt::transparent);
    edit_palette.setColor(QPalette::Text, dark ? QColor(255, 255, 255, 235) : QColor(0, 0, 0, 220));
    edit_palette.setColor(QPalette::PlaceholderText, dark ? QColor(255, 255, 255, 110) : QColor(0, 0, 0, 100));
    m_edit->setPalette(edit_palette);
    m_edit->setStyleSheet("QLineEdit { background: transparent; border: none; }");

    m_edit->clear();
    m_selection_is_explicit = false;
    m_omnibox.begin_editing({});
    rebuild_rows();
    show();
    raise();
    activateWindow();
    m_edit->setFocus(Qt::ShortcutFocusReason);
    qApp->installEventFilter(this);
}

void CommandBar::dismiss()
{
    qApp->removeEventFilter(this);
    if (m_omnibox.is_editing())
        m_omnibox.end_editing();
    hide();
    if (m_window) {
        m_window->activateWindow();
        if (auto* tab = m_window->current_tab())
            tab->view().setFocus();
    }
}

void CommandBar::commit(String const& input)
{
    // Mirrors LocationEdit's commit: upstream's URL-or-search resolution.
    auto destination_kind = m_omnibox.destination_kind_for_last_commit();
    auto append_tld = (QApplication::keyboardModifiers() & Qt::ControlModifier) ? WebView::AppendTLD::Yes : WebView::AppendTLD::No;
    auto url = WebView::sanitize_url(input, WebView::Application::settings().search_engine_settings().engine, append_tld);
    open_in_new_tab(input, AK::move(url), destination_kind);
}

void CommandBar::open_in_new_tab(String input, Optional<URL::URL> url, WebView::OmniboxDestinationKind destination_kind)
{
    dismiss();
    if (!m_window || input.is_empty())
        return;

    // Mirrors Tab::location_edit_return_pressed(), which is private upstream. The sidebar provides the tab: its ready
    // spare when there is one, in the current space's profile.
    auto* sidebar = ArcSidebar::for_window(*m_window);
    auto& tab = sidebar ? sidebar->new_tab_for_input() : m_window->create_new_tab(Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
    auto& view = tab.view();
    view.set_next_history_visit_transition(WebView::HistoryVisitTransition::Omnibox);
    if (destination_kind == WebView::OmniboxDestinationKind::Search) {
        if (url.has_value())
            view.load(*url);
        else
            view.load_navigation_error_page(input);
    } else {
        view.load_from_user_input(input, AK::move(url));
    }
    view.setFocus();
}

static QIcon suggestion_icon(WebView::AutocompleteSuggestion const& suggestion, QColor color, qreal device_pixel_ratio)
{
    if (suggestion.favicon_png.has_value()) {
        QPixmap pixmap;
        if (pixmap.loadFromData(suggestion.favicon_png->data(), static_cast<uint>(suggestion.favicon_png->size()), "PNG"))
            return QIcon(pixmap);
    }
    if (suggestion.source != WebView::AutocompleteSuggestionSource::Search) {
        if (auto cached = ArcIcons::icon_for_url(qstring_from_ak_string(suggestion.text)); !cached.isNull())
            return cached;
    }
    auto symbol = suggestion.source == WebView::AutocompleteSuggestionSource::Search ? "magnifyingglass" : "globe";
    return QIcon(fitted_symbol_pixmap(symbol, 16, color, device_pixel_ratio));
}

static bool is_blank_url(QString const& url)
{
    return url.isEmpty() || url.startsWith("about:");
}

void CommandBar::rebuild_rows()
{
    if (!m_window)
        return;
    m_rows.clear();
    auto query = m_edit->text().left(m_edit->hasSelectedText() ? m_edit->selectionStart() : m_edit->text().length()).trimmed();
    auto muted = ChromeStyle::is_dark(palette()) ? QColor(255, 255, 255, 170) : QColor(0, 0, 0, 150);

    // Upstream's suggestions (history, bookmarks, search) while typing.
    if (!query.isEmpty() && m_omnibox.is_popup_visible()) {
        auto const& suggestions = m_omnibox.suggestions();
        for (size_t i = 0; i < suggestions.size(); ++i) {
            auto const& suggestion = suggestions[i];
            CommandBarRow row;
            row.kind = CommandBarRow::Kind::Suggestion;
            row.suggestion_index = i;
            auto display = qstring_from_ak_string(WebView::autocomplete_suggestion_display_text(suggestion));
            auto title = suggestion.title.has_value() ? qstring_from_ak_string(*suggestion.title) : QString();
            row.title = title.isEmpty() ? display : title;
            if (suggestion.subtitle.has_value())
                row.subtitle = qstring_from_ak_string(*suggestion.subtitle);
            else if (row.title != display)
                row.subtitle = display;
            row.icon = suggestion_icon(suggestion, muted, devicePixelRatioF());
            m_rows.append(row);
        }
    }

    // Open tabs to switch to: the current space's first.
    if (auto* tabs = m_window->findChild<TabWidget*>()) {
        auto current_space = [&]() -> QString {
            if (auto* sidebar = ArcSidebar::for_window(*m_window))
                return sidebar->current_space_id();
            return {};
        }();
        QList<CommandBarRow> same_space;
        QList<CommandBarRow> other_spaces;
        for (int i = 0; i < tabs->count(); ++i) {
            auto* tab = tabs->tab(i);
            if (!tab || tab == m_window->current_tab())
                continue;
            auto url = qstring_from_ak_string(tab->view().url().serialize());
            if (is_blank_url(url) || !tab_matches(query, tab->title(), url))
                continue;
            CommandBarRow row;
            row.kind = CommandBarRow::Kind::SwitchToTab;
            row.title = tab->title().isEmpty() ? url : tab->title();
            row.icon = tab->favicon().isNull() ? ArcIcons::icon_for_url(url) : tab->favicon();
            row.tab = tab;
            (tab->property("arc_space").toString() == current_space ? same_space : other_spaces).append(row);
        }
        auto limit = query.isEmpty() ? MAX_TAB_ROWS_EMPTY : MAX_TAB_ROWS_MATCHING;
        for (auto const& row : same_space + other_spaces) {
            if (limit-- <= 0)
                break;
            m_rows.append(row);
        }
    }

    // Keep an explicit choice if it's still there; otherwise follow the omnibox's default row.
    if (!m_selection_is_explicit || m_selected_row >= m_rows.size()) {
        m_selection_is_explicit = false;
        m_selected_row = m_rows.isEmpty() ? -1 : 0;
        if (auto selected = m_omnibox.selected_suggestion(); selected.has_value()) {
            for (int i = 0; i < m_rows.size(); ++i) {
                if (m_rows[i].kind == CommandBarRow::Kind::Suggestion && m_rows[i].suggestion_index == *selected)
                    m_selected_row = i;
            }
        }
    }
    reposition();
    update();
}

void CommandBar::select_row(int row)
{
    if (row < -1 || row >= m_rows.size() || row == m_selected_row)
        return;
    m_selected_row = row;
    update();
}

void CommandBar::activate_row(int row)
{
    if (row < 0 || row >= m_rows.size()) {
        m_omnibox.return_pressed();
        return;
    }
    auto const& entry = m_rows[row];
    if (entry.kind == CommandBarRow::Kind::SwitchToTab) {
        QPointer<Tab> tab = entry.tab;
        dismiss();
        if (tab && m_window) {
            if (auto* tabs = m_window->findChild<TabWidget*>(); tabs && tabs->index_of(tab) >= 0)
                m_window->activate_tab(tabs->index_of(tab));
        }
        return;
    }
    // The omnibox's own default row goes through Return, which handles results lagging behind typing.
    if (!m_selection_is_explicit && m_omnibox.selected_suggestion() == entry.suggestion_index)
        m_omnibox.return_pressed();
    else
        m_omnibox.suggestion_clicked(entry.suggestion_index);
}

bool CommandBar::handle_edit_key(QKeyEvent& event)
{
    switch (event.key()) {
    case Qt::Key_Escape:
        dismiss();
        return true;
    case Qt::Key_Down:
    case Qt::Key_Up: {
        if (m_rows.isEmpty())
            return true;
        auto step = event.key() == Qt::Key_Down ? 1 : -1;
        auto next = m_selected_row < 0 ? 0 : (m_selected_row + step + m_rows.size()) % m_rows.size();
        m_selection_is_explicit = true;
        select_row(static_cast<int>(next));
        return true;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        auto query = m_edit->text().trimmed();
        // Typed text with tab matches only (suggestions not in yet): go where the text says, not to a tab.
        if (!query.isEmpty() && !m_selection_is_explicit && m_selected_row >= 0 && m_rows[m_selected_row].kind == CommandBarRow::Kind::SwitchToTab) {
            m_omnibox.return_pressed();
            return true;
        }
        if (query.isEmpty() && m_selected_row < 0)
            return true;
        activate_row(m_selected_row);
        return true;
    }
    case Qt::Key_Right:
    case Qt::Key_End:
    case Qt::Key_Tab:
        return m_omnibox.accept_completion() || event.key() == Qt::Key_Tab;
    case Qt::Key_Backspace:
    case Qt::Key_Delete:
        m_omnibox.will_delete_text();
        return false;
    default:
        return false;
    }
}

void CommandBar::reposition()
{
    if (!m_window)
        return;
    // Centered over the page, as in Arc, rather than the whole window (which includes the sidebar).
    auto frame = m_window->geometry();
    if (auto* tab = m_window->current_tab(); tab && tab->view().isVisible()) {
        auto& view = tab->view();
        frame = QRect(view.mapToGlobal(QPoint(0, 0)), view.size());
    }
    auto card_width = qMin(BAR_WIDTH, frame.width() - 80);
    auto width = card_width + SHADOW_MARGIN * 2;
    auto height = card_height_for_rows(static_cast<int>(m_rows.size())) + SHADOW_MARGIN * 2;
    auto x = frame.x() + (frame.width() - width) / 2;
    auto y = frame.y() + frame.height() / 7;
    setGeometry(x, y, width, height);

    auto card = card_rect().toRect();
    m_edit->setGeometry(card.left() + 54, card.top() + (HEADER_HEIGHT - 36) / 2, card.width() - 54 - 22, 36);
}

bool CommandBar::owns_widget(QWidget* widget) const
{
    for (auto* w = widget; w; w = w->parentWidget()) {
        if (w == this)
            return true;
    }
    return false;
}

bool CommandBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_window && (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        if (isVisible())
            reposition();
        return false;
    }
    if (watched == m_edit && event->type() == QEvent::KeyPress)
        return handle_edit_key(*static_cast<QKeyEvent*>(event));

    if (!isVisible())
        return false;

    if (watched == this && event->type() == QEvent::WindowDeactivate) {
        dismiss();
    } else if (event->type() == QEvent::MouseButtonPress) {
        if (auto* widget = qobject_cast<QWidget*>(watched); widget && !owns_widget(widget))
            dismiss();
    } else if (event->type() == QEvent::ApplicationDeactivate) {
        dismiss();
    }
    return false;
}

QRectF CommandBar::card_rect() const
{
    return QRectF(SHADOW_MARGIN, SHADOW_MARGIN, width() - SHADOW_MARGIN * 2, height() - SHADOW_MARGIN * 2);
}

QRectF CommandBar::row_rect(int row) const
{
    auto card = card_rect();
    return QRectF(card.left() + LIST_PADDING, card.top() + HEADER_HEIGHT + 1 + LIST_PADDING + row * ROW_HEIGHT, card.width() - LIST_PADDING * 2, ROW_HEIGHT);
}

int CommandBar::row_at(QPointF position) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (row_rect(i).contains(position))
            return i;
    }
    return -1;
}

void CommandBar::mousePressEvent(QMouseEvent* event)
{
    // Clicks in the transparent space around the card dismiss the bar.
    if (!card_rect().contains(event->position()))
        dismiss();
    QWidget::mousePressEvent(event);
}

void CommandBar::mouseMoveEvent(QMouseEvent* event)
{
    if (auto row = row_at(event->position()); row >= 0 && row != m_selected_row) {
        m_selection_is_explicit = true;
        select_row(row);
    }
    QWidget::mouseMoveEvent(event);
}

void CommandBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (auto row = row_at(event->position()); row >= 0) {
            m_selection_is_explicit = true;
            activate_row(row);
            return;
        }
    }
    QWidget::mouseReleaseEvent(event);
}

void CommandBar::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        dismiss();
        return;
    }
    QWidget::keyPressEvent(event);
}

static void draw_arrow(QPainter& painter, QRectF box, QColor color)
{
    painter.save();
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    auto center = box.center();
    painter.drawLine(center + QPointF(-6, 0), center + QPointF(6, 0));
    painter.drawLine(center + QPointF(1, -5), center + QPointF(6, 0));
    painter.drawLine(center + QPointF(1, 5), center + QPointF(6, 0));
    painter.restore();
}

void CommandBar::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    auto dark = ChromeStyle::is_dark(palette());
    auto card = card_rect();

    // Soft shadow: stacked translucent rounded rects, offset slightly downward.
    painter.setPen(Qt::NoPen);
    for (int i = SHADOW_MARGIN; i > 0; i -= 2) {
        auto alpha = static_cast<int>(44.0 * (1.0 - static_cast<double>(i) / SHADOW_MARGIN));
        painter.setBrush(QColor(0, 0, 0, qMax(alpha / 4, 1)));
        painter.drawRoundedRect(card.adjusted(-i, -i + 8, i, i + 8), CORNER_RADIUS + i, CORNER_RADIUS + i);
    }

    QPainterPath path;
    path.addRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), CORNER_RADIUS, CORNER_RADIUS);
    painter.fillPath(path, dark ? QColor(28, 28, 31) : QColor(252, 252, 253));
    painter.setPen(QPen(dark ? QColor(255, 255, 255, 34) : QColor(0, 0, 0, 30), 1));
    painter.drawPath(path);

    auto text = dark ? QColor(255, 255, 255, 235) : QColor(0, 0, 0, 220);
    auto muted = dark ? QColor(255, 255, 255, 120) : QColor(0, 0, 0, 115);
    auto ratio = devicePixelRatioF();

    // Search glyph.
    auto glass = fitted_symbol_pixmap("magnifyingglass", 17, text, ratio);
    if (!glass.isNull()) {
        auto size = glass.deviceIndependentSize();
        painter.drawPixmap(QPointF(card.left() + 32 - size.width() / 2, card.top() + HEADER_HEIGHT / 2.0 - size.height() / 2), glass);
    } else {
        painter.setPen(QPen(text, 1.8));
        painter.setBrush(Qt::NoBrush);
        auto center = QPointF(card.left() + 31, card.top() + HEADER_HEIGHT / 2.0 - 1);
        painter.drawEllipse(center, 6, 6);
        painter.drawLine(center + QPointF(4.5, 4.5), center + QPointF(8.5, 8.5));
    }

    if (m_rows.isEmpty())
        return;

    // Hairline under the input.
    auto line_y = card.top() + HEADER_HEIGHT + 0.5;
    painter.setPen(QPen(dark ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 20), 1));
    painter.drawLine(QPointF(card.left() + 12, line_y), QPointF(card.right() - 12, line_y));

    auto title_font = font();
    title_font.setPixelSize(TITLE_PIXEL_SIZE);
    title_font.setWeight(QFont::Medium);
    auto action_font = title_font;
    action_font.setWeight(QFont::DemiBold);
    QFontMetricsF title_metrics(title_font);
    QFontMetricsF action_metrics(action_font);

    for (int i = 0; i < m_rows.size(); ++i) {
        auto const& row = m_rows[i];
        auto rect = row_rect(i);
        auto selected = i == m_selected_row;

        if (selected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(dark ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 14));
            painter.drawRoundedRect(rect, ROW_RADIUS, ROW_RADIUS);
        }

        auto icon_box = QRectF(rect.left() + 12, rect.center().y() - ICON_BOX / 2.0, ICON_BOX, ICON_BOX);
        if (!row.icon.isNull())
            row.icon.paint(&painter, icon_box.adjusted(2, 2, -2, -2).toRect());

        auto right = rect.right() - 14;
        if (row.kind == CommandBarRow::Kind::SwitchToTab) {
            auto button = QRectF(right - ARROW_BUTTON, rect.center().y() - ARROW_BUTTON / 2.0, ARROW_BUTTON, ARROW_BUTTON);
            painter.setPen(Qt::NoPen);
            painter.setBrush(selected ? (dark ? QColor(255, 255, 255) : QColor(0, 0, 0, 200)) : (dark ? QColor(255, 255, 255, 18) : QColor(0, 0, 0, 10)));
            painter.drawRoundedRect(button, 6, 6);
            draw_arrow(painter, button, selected ? (dark ? QColor(28, 28, 31) : QColor(255, 255, 255)) : muted);

            QString label = "Switch to Tab";
            auto label_width = action_metrics.horizontalAdvance(label);
            auto label_rect = QRectF(button.left() - 12 - label_width, rect.top(), label_width + 1, rect.height());
            painter.setFont(action_font);
            painter.setPen(selected ? text : muted);
            painter.drawText(label_rect, Qt::AlignVCenter | Qt::AlignLeft, label);
            right = label_rect.left() - 16;
        }

        auto x = icon_box.right() + 12;
        painter.setFont(title_font);
        painter.setPen(text);
        auto title = title_metrics.elidedText(row.title, Qt::ElideRight, right - x);
        painter.drawText(QRectF(x, rect.top(), right - x, rect.height()), Qt::AlignVCenter | Qt::AlignLeft, title);
        x += title_metrics.horizontalAdvance(title);

        if (!row.subtitle.isEmpty() && right - x > 60) {
            painter.setPen(muted);
            auto subtitle = title_metrics.elidedText(QString::fromUtf8(" — ") + row.subtitle, Qt::ElideRight, right - x);
            painter.drawText(QRectF(x, rect.top(), right - x, rect.height()), Qt::AlignVCenter | Qt::AlignLeft, subtitle);
        }
    }
}

}
