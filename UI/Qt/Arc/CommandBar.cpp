/*
 * Arc-style command bar: a floating search/address panel opened with Cmd+T.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/CommandBar.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/WebContentView.h>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

namespace Ladybird::Arc {

static constexpr int BAR_WIDTH = 640;
static constexpr int SHADOW_MARGIN = 24;
static constexpr int CORNER_RADIUS = 14;
static constexpr int INNER_PADDING = 10;
// Upstream's suggestion popup is clipped to the window that owns the input, so leave room for it below.
static constexpr int SUGGESTIONS_SPACE = 380;

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
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NativeWindow);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(SHADOW_MARGIN + INNER_PADDING, SHADOW_MARGIN + INNER_PADDING, SHADOW_MARGIN + INNER_PADDING, SHADOW_MARGIN + INNER_PADDING);

    m_edit = new LocationEdit(this, window.is_private());
    m_edit->setMinimumHeight(40);
    layout->addWidget(m_edit);
    layout->addStretch(1);

    m_edit->on_navigation = [this](auto input, auto url, auto destination_kind) {
        open_in_new_tab(AK::move(input), AK::move(url), destination_kind);
    };
    connect(m_edit, &LocationEdit::focus_return_requested, this, &CommandBar::dismiss);

    window.installEventFilter(this);
}

CommandBar::~CommandBar()
{
    qApp->removeEventFilter(this);
}

void CommandBar::present()
{
    m_edit->set_url({});
    m_edit->clear();
    reposition();
    show();
    raise();
    activateWindow();
    m_edit->setFocus(Qt::ShortcutFocusReason);
    qApp->installEventFilter(this);
}

void CommandBar::dismiss()
{
    qApp->removeEventFilter(this);
    hide();
    if (m_window) {
        m_window->activateWindow();
        if (auto* tab = m_window->current_tab())
            tab->view().setFocus();
    }
}

void CommandBar::open_in_new_tab(String input, Optional<URL::URL> url, WebView::OmniboxDestinationKind destination_kind)
{
    dismiss();
    if (!m_window || input.is_empty())
        return;

    // Mirrors Tab::location_edit_return_pressed(), which is private upstream.
    auto& tab = m_window->create_new_tab(Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
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

void CommandBar::reposition()
{
    if (!m_window)
        return;
    auto frame = m_window->geometry();
    auto width = qMin(BAR_WIDTH, frame.width() - 80) + SHADOW_MARGIN * 2;
    auto height = SHADOW_MARGIN * 2 + INNER_PADDING * 2 + m_edit->sizeHint().height() + SUGGESTIONS_SPACE;
    auto x = frame.x() + (frame.width() - width) / 2;
    auto y = frame.y() + frame.height() / 5;
    setGeometry(x, y, width, height);
}

bool CommandBar::owns_widget(QWidget* widget) const
{
    // The suggestion popup is its own top-level window parented to us, so walk up through windows too.
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
    // Just around the input; the suggestion popup draws its own frame below it.
    auto bottom = m_edit->geometry().bottom() + INNER_PADDING + 1;
    return QRectF(SHADOW_MARGIN, SHADOW_MARGIN, width() - SHADOW_MARGIN * 2, bottom - SHADOW_MARGIN);
}

void CommandBar::mousePressEvent(QMouseEvent* event)
{
    // Clicks in the transparent space around the card dismiss the bar.
    if (!card_rect().contains(event->position()))
        dismiss();
    QWidget::mousePressEvent(event);
}

void CommandBar::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        dismiss();
        return;
    }
    QWidget::keyPressEvent(event);
}

void CommandBar::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    auto card = card_rect();

    // Soft shadow: stacked translucent rounded rects, offset slightly downward.
    painter.setPen(Qt::NoPen);
    for (int i = SHADOW_MARGIN; i > 0; i -= 2) {
        auto alpha = static_cast<int>(36.0 * (1.0 - static_cast<double>(i) / SHADOW_MARGIN));
        painter.setBrush(QColor(0, 0, 0, qMax(alpha / 4, 1)));
        painter.drawRoundedRect(card.adjusted(-i, -i + 6, i, i + 6), CORNER_RADIUS + i, CORNER_RADIUS + i);
    }

    QPainterPath path;
    path.addRoundedRect(card, CORNER_RADIUS, CORNER_RADIUS);

    painter.fillPath(path, ChromeStyle::chrome_surface(palette()));
    painter.setPen(QPen(ChromeStyle::chrome_border(palette()), 1));
    painter.drawPath(path);
}

}
