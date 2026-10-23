/*
 * Arc's toast.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/SymbolIcon.h>
#include <UI/Qt/Arc/Toast.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/Tab.h>

#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QTimer>

namespace Ladybird::Arc {

static constexpr int HEIGHT = 32;
static constexpr int PADDING = 12;
static constexpr int ICON_SIZE = 13;
static constexpr int ICON_GAP = 7;
static constexpr int SHADOW = 8; // Room around the pill for its shadow.
static constexpr int TEXT_PIXEL_SIZE = 13;

static QFont toast_font(QFont font)
{
    font.setPixelSize(TEXT_PIXEL_SIZE);
    font.setWeight(QFont::Medium);
    return font;
}

Toast* Toast::current_in(BrowserWindow& window)
{
    return window.findChild<Toast*>(QString(), Qt::FindDirectChildrenOnly);
}

Toast* Toast::show_in(BrowserWindow& window, QString const& text, QString const& symbol)
{
    if (auto* old = current_in(window)) {
        old->hide();
        old->setParent(nullptr);
        old->deleteLater();
    }
    auto* toast = new Toast(window, text, symbol);
    toast->place();
    toast->show();
    return toast;
}

Toast::Toast(BrowserWindow& window, QString text, QString symbol)
    // Its own little window, as the command bar is: it shows over the page, which draws on its own.
    : QWidget(&window, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus)
    , m_window(&window)
    , m_text(AK::move(text))
    , m_symbol(AK::move(symbol))
{
    setObjectName("ArcToast");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    resize(sizeHint());

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(VISIBLE_MS);
    connect(m_timer, &QTimer::timeout, this, &Toast::fade_out);
    m_timer->start();
}

QSize Toast::sizeHint() const
{
    QFontMetrics metrics(toast_font(font()));
    auto width = PADDING * 2 + metrics.horizontalAdvance(m_text);
    if (!m_symbol.isEmpty())
        width += ICON_SIZE + ICON_GAP;
    return { width + SHADOW * 2, HEIGHT + SHADOW * 2 };
}

// The top right corner of the page (not the window, which includes the sidebar).
void Toast::place()
{
    if (!m_window)
        return;
    auto frame = m_window->geometry();
    if (auto* tab = m_window->current_tab(); tab && tab->view().isVisible()) {
        auto& view = tab->view();
        frame = QRect(view.mapToGlobal(QPoint(0, 0)), view.size());
    }
    move(frame.right() + 1 - MARGIN - width() + SHADOW, frame.top() + MARGIN - SHADOW);
}

void Toast::fade_out()
{
    m_fade = new QPropertyAnimation(this, "windowOpacity", this);
    m_fade->setDuration(FADE_MS);
    m_fade->setStartValue(windowOpacity());
    m_fade->setEndValue(0.0);
    connect(m_fade, &QPropertyAnimation::finished, this, [this] {
        hide();
        deleteLater();
    });
    m_fade->start();
}

void Toast::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QRectF pill = QRectF(rect()).adjusted(SHADOW, SHADOW, -SHADOW, -SHADOW);

    // A soft shadow, then a dark pill with light text, as in Arc (the same in light and dark mode).
    for (int i = SHADOW; i > 0; i -= 2) {
        QPainterPath shadow;
        shadow.addRoundedRect(pill.adjusted(-i, -i + 2, i, i + 2), HEIGHT / 2.0 + i, HEIGHT / 2.0 + i);
        painter.fillPath(shadow, QColor(0, 0, 0, 6));
    }
    QPainterPath path;
    path.addRoundedRect(pill, HEIGHT / 2.0, HEIGHT / 2.0);
    painter.fillPath(path, QColor(32, 32, 36, 242));
    painter.setPen(QPen(QColor(255, 255, 255, 28), 1));
    painter.drawPath(path);

    auto x = pill.left() + PADDING;
    auto text_color = QColor(255, 255, 255, 235);
    if (!m_symbol.isEmpty()) {
        auto icon = fitted_symbol_pixmap(m_symbol, ICON_SIZE, text_color, devicePixelRatioF());
        if (!icon.isNull()) {
            auto size = icon.deviceIndependentSize();
            painter.drawPixmap(QPointF(x + (ICON_SIZE - size.width()) / 2, pill.center().y() - size.height() / 2), icon);
        }
        x += ICON_SIZE + ICON_GAP;
    }
    painter.setFont(toast_font(font()));
    painter.setPen(text_color);
    painter.drawText(QRectF(x, pill.top(), pill.right() - x, pill.height()), Qt::AlignLeft | Qt::AlignVCenter, m_text);
}

}
