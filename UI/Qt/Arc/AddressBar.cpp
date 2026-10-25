/*
 * Arc-style address bar.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/AddressBar.h>
#include <UI/Qt/Arc/SymbolIcon.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/StringUtils.h>

#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QMouseEvent>
#include <QPointer>
#include <QPainter>
#include <QTimer>

namespace Ladybird::Arc {

static constexpr int COPIED_FEEDBACK_MS = 1500;
static constexpr int MIN_REST_WIDTH = 40; // Below this, the path is dropped rather than squeezed to "…".
static constexpr qreal PILL_RADIUS = 7;

DisplayUrl display_url_parts(QUrl const& url)
{
    if (!url.isValid() || url.isEmpty())
        return {};
    // The host as the network sees it (punycode for international names): shown decoded, a name in lookalike letters
    // ("аррӏе.com" in Cyrillic) would pass for the site it imitates.
    auto host = url.host(QUrl::FullyEncoded);
    if (host.isEmpty())
        return { url.toDisplayString(), {} };
    if (host.startsWith("www."))
        host = host.mid(4);
    auto default_port = url.scheme() == "https" ? 443 : url.scheme() == "http" ? 80 : -1;
    if (url.port() != -1 && url.port() != default_port)
        host += ":" + QString::number(url.port());

    auto rest = url.path(QUrl::PrettyDecoded);
    if (url.hasQuery())
        rest += "?" + url.query(QUrl::PrettyDecoded);
    if (url.hasFragment())
        rest += "#" + url.fragment(QUrl::PrettyDecoded);
    if (rest == "/")
        rest.clear();
    return { host, rest };
}

AddressBar::AddressBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("ArcAddressBar");
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(HEIGHT);

    m_copied_timer = new QTimer(this);
    m_copied_timer->setSingleShot(true);
    m_copied_timer->setInterval(COPIED_FEEDBACK_MS);
    connect(m_copied_timer, &QTimer::timeout, this, [this] {
        m_copied = false;
        update();
    });

    // Centered on the toolbar, so it follows the toolbar's other buttons coming and going.
    if (parent)
        parent->installEventFilter(this);
}

AddressBar* AddressBar::replace(LocationEdit& edit)
{
    auto* container = edit.parentWidget();
    if (!container)
        return nullptr;
    if (auto* existing = container->findChild<AddressBar*>(QString(), Qt::FindDirectChildrenOnly))
        return existing;
    auto* layout = qobject_cast<QBoxLayout*>(container->layout());
    if (!layout)
        return nullptr;
    auto* bar = new AddressBar(container);
    auto index = layout->indexOf(&edit);
    edit.hide();
    layout->insertWidget(index < 0 ? layout->count() : index + 1, bar, 1, Qt::AlignVCenter);

    // The hidden LocationEdit is told every URL change (and blanks itself for the new tab page).
    auto update = [bar, edit = QPointer<LocationEdit>(&edit)] {
        QUrl url;
        if (edit && !edit->url_is_hidden()) {
            if (auto edit_url = edit->url(); edit_url.has_value())
                url = QUrl(qstring_from_ak_string(edit_url->serialize()));
        }
        bar->set_url(url);
    };
    connect(&edit, &QLineEdit::textChanged, bar, update);
    update();
    return bar;
}

void AddressBar::set_url(QUrl const& url)
{
    if (url == m_url)
        return;
    m_url = url;
    m_parts = display_url_parts(url);
    m_copied = false;
    relayout();
    update();
}

QFont AddressBar::host_font() const
{
    auto font = this->font();
    font.setPixelSize(TEXT_PIXEL_SIZE);
    font.setWeight(QFont::DemiBold);
    return font;
}

QFont AddressBar::rest_font() const
{
    auto font = this->font();
    font.setPixelSize(TEXT_PIXEL_SIZE);
    font.setWeight(QFont::Normal);
    return font;
}

void AddressBar::relayout()
{
    m_shown_host.clear();
    m_shown_rest.clear();
    m_icon_rect = {};
    m_text_rect = {};
    if (m_parts.host.isEmpty())
        return;

    QFontMetricsF host_metrics(host_font());
    QFontMetricsF rest_metrics(rest_font());
    // The icon's hover pill is wider than the icon: leave room for it at either edge.
    auto edge = (height() - 4.0 - ICON_SIZE) / 2;
    auto available = width() - edge * 2 - ICON_SIZE - ICON_GAP - PILL_PADDING * 2.0;
    if (available <= 0)
        return;

    m_shown_host = m_parts.host;
    m_shown_rest = m_parts.rest;
    auto host_width = host_metrics.horizontalAdvance(m_shown_host);
    if (host_width + rest_metrics.horizontalAdvance(m_shown_rest) > available) {
        // Short of room: the path goes first (elided, then dropped), then the host is elided.
        auto rest_room = available - host_width;
        if (rest_room >= MIN_REST_WIDTH) {
            m_shown_rest = rest_metrics.elidedText(m_shown_rest, Qt::ElideRight, rest_room);
        } else {
            m_shown_rest.clear();
            m_shown_host = host_metrics.elidedText(m_shown_host, Qt::ElideRight, available);
            host_width = host_metrics.horizontalAdvance(m_shown_host);
        }
    }
    auto text_width = host_width + rest_metrics.horizontalAdvance(m_shown_rest);
    auto content_width = ICON_SIZE + ICON_GAP + PILL_PADDING + text_width + PILL_PADDING;

    // Centered on the toolbar (as in Arc), as far as the room here allows.
    qreal center = width() / 2.0;
    for (auto* ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        if (ancestor->objectName() == "LadybirdNavigationToolbar") {
            center = mapFrom(ancestor, QPointF(ancestor->width() / 2.0, 0)).x();
            break;
        }
    }
    auto left = qBound(edge, center - content_width / 2.0, qMax(edge, width() - content_width));

    auto middle = height() / 2.0;
    m_icon_rect = QRectF(left, middle - ICON_SIZE / 2.0, ICON_SIZE, ICON_SIZE);
    auto text_height = qMax(host_metrics.height(), rest_metrics.height());
    m_text_rect = QRectF(m_icon_rect.right() + ICON_GAP + PILL_PADDING, middle - text_height / 2.0, text_width, text_height);
}

QRectF AddressBar::icon_rect() const
{
    return m_icon_rect;
}

QRectF AddressBar::text_rect() const
{
    return m_text_rect;
}

QRectF AddressBar::pill_rect() const
{
    if (m_text_rect.isEmpty())
        return {};
    return QRectF(m_text_rect.left() - PILL_PADDING, 2, m_text_rect.width() + PILL_PADDING * 2, height() - 4);
}

QRectF AddressBar::icon_pill_rect() const
{
    if (m_icon_rect.isEmpty())
        return {};
    auto side = height() - 4.0;
    return QRectF(m_icon_rect.center().x() - side / 2, 2, side, side);
}

QString AddressBar::shown_host() const
{
    return m_shown_host;
}

QString AddressBar::shown_rest() const
{
    return m_shown_rest;
}

QSize AddressBar::sizeHint() const
{
    return { 400, HEIGHT };
}

QSize AddressBar::minimumSizeHint() const
{
    return { ICON_SIZE + ICON_GAP + PILL_PADDING * 2 + 40, HEIGHT };
}

AddressBar::Hover AddressBar::hover_at(QPointF position) const
{
    if (icon_pill_rect().contains(position))
        return Hover::Icon;
    if (pill_rect().contains(position))
        return Hover::Text;
    return Hover::None;
}

void AddressBar::set_hover(Hover hover)
{
    if (m_hover == hover)
        return;
    m_hover = hover;
    setCursor(hover == Hover::None ? Qt::ArrowCursor : Qt::PointingHandCursor);
    setToolTip(hover == Hover::Icon ? "Copy Link" : QString());
    update();
}

void AddressBar::mouseMoveEvent(QMouseEvent* event)
{
    set_hover(hover_at(event->position()));
    QWidget::mouseMoveEvent(event);
}

void AddressBar::leaveEvent(QEvent* event)
{
    set_hover(Hover::None);
    QWidget::leaveEvent(event);
}

void AddressBar::mousePressEvent(QMouseEvent* event)
{
    m_pressed = event->button() == Qt::LeftButton ? hover_at(event->position()) : Hover::None;
    if (m_pressed == Hover::None) {
        // Empty space: leave it to the toolbar (which drags the window).
        event->ignore();
        return;
    }
    event->accept();
}

void AddressBar::mouseReleaseEvent(QMouseEvent* event)
{
    auto pressed = m_pressed;
    m_pressed = Hover::None;
    if (pressed == Hover::None || event->button() != Qt::LeftButton || hover_at(event->position()) != pressed) {
        event->ignore();
        return;
    }
    event->accept();
    if (pressed == Hover::Icon) {
        if (m_url.isEmpty())
            return;
        QApplication::clipboard()->setText(m_url.toString(QUrl::FullyEncoded));
        m_copied = true;
        m_copied_timer->start();
        update();
        emit url_copied();
        return;
    }
    emit edit_requested();
}

void AddressBar::resizeEvent(QResizeEvent* event)
{
    relayout();
    QWidget::resizeEvent(event);
}

void AddressBar::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange || event->type() == QEvent::PaletteChange) {
        relayout();
        update();
    }
    QWidget::changeEvent(event);
}

bool AddressBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        relayout();
        update();
    }
    return false;
}

void AddressBar::paintEvent(QPaintEvent*)
{
    if (m_text_rect.isEmpty())
        return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    auto dark = ChromeStyle::is_dark(palette());
    auto host_color = dark ? QColor(255, 255, 255, 225) : QColor(0, 0, 0, 215);
    auto rest_color = dark ? QColor(255, 255, 255, 135) : QColor(0, 0, 0, 120);
    auto icon_color = m_hover == Hover::Icon || m_copied ? host_color : rest_color;

    if (m_hover != Hover::None) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(dark ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 14));
        painter.drawRoundedRect(m_hover == Hover::Text ? pill_rect() : icon_pill_rect(), PILL_RADIUS, PILL_RADIUS);
    }

    auto icon = fitted_symbol_pixmap(m_copied ? "checkmark" : "link", ICON_SIZE, icon_color, devicePixelRatioF());
    if (!icon.isNull()) {
        auto size = icon.deviceIndependentSize();
        painter.drawPixmap(QPointF(m_icon_rect.center().x() - size.width() / 2, m_icon_rect.center().y() - size.height() / 2), icon);
    }

    painter.setFont(host_font());
    painter.setPen(host_color);
    QFontMetricsF host_metrics(host_font());
    painter.drawText(m_text_rect, Qt::AlignLeft | Qt::AlignVCenter, m_shown_host);
    if (!m_shown_rest.isEmpty()) {
        painter.setFont(rest_font());
        painter.setPen(rest_color);
        auto rest_rect = m_text_rect.adjusted(host_metrics.horizontalAdvance(m_shown_host), 0, 0, 0);
        painter.drawText(rest_rect, Qt::AlignLeft | Qt::AlignVCenter, m_shown_rest);
    }
}

}
