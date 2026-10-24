/*
 * Arc's media player at the bottom of the sidebar.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/MiniPlayer.h>
#include <UI/Qt/Arc/SymbolIcon.h>

#include <QEasingCurve>
#include <QEnterEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

namespace Ladybird::Arc {

static constexpr int BUTTON_SIZE = 30;
static constexpr int SMALL_BUTTON_SIZE = 26;
static constexpr int SIDE_PADDING = 8;
static constexpr qreal RADIUS = 10;

// The player is dark whatever the theme, as in Arc.
static QColor card_color() { return QColor(20, 20, 22); }
static QColor foreground_color() { return QColor(238, 238, 240); }

PlayerButton::PlayerButton(QWidget* parent, int size)
    : QAbstractButton(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(size, size);
}

void PlayerButton::set_symbol(QString const& symbol)
{
    if (m_symbol == symbol)
        return;
    m_symbol = symbol;
    m_picture = {};
    update();
}

void PlayerButton::set_picture(QIcon const& picture)
{
    if (!m_symbol.isEmpty() || picture.cacheKey() != m_picture.cacheKey()) {
        m_symbol.clear();
        m_picture = picture;
        update();
    }
}

void PlayerButton::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void PlayerButton::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void PlayerButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (isEnabled() && (m_hovered || isDown())) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, isDown() ? 44 : 28));
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 7, 7);
    }

    if (!m_picture.isNull()) {
        auto box = 16;
        auto pixmap = m_picture.pixmap(QSize(box, box), devicePixelRatioF());
        painter.drawPixmap(QRect((width() - box) / 2, (height() - box) / 2, box, box), pixmap);
        return;
    }
    if (m_symbol.isEmpty())
        return;
    painter.setOpacity(isEnabled() ? 0.95 : 0.3);
    auto box = width() >= BUTTON_SIZE ? 15 : 13;
    auto pixmap = fitted_symbol_pixmap(m_symbol, box, foreground_color(), devicePixelRatioF());
    if (pixmap.isNull())
        return;
    auto logical = pixmap.deviceIndependentSize();
    painter.drawPixmap(QPointF((width() - logical.width()) / 2.0, (height() - logical.height()) / 2.0), pixmap);
}

MiniPlayer::MiniPlayer(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("ArcMiniPlayer");
    setAttribute(Qt::WA_Hover);

    auto make_button = [this](int size, QString const& symbol, QString const& tooltip) {
        auto* button = new PlayerButton(this, size);
        button->set_symbol(symbol);
        button->setToolTip(tooltip);
        return button;
    };
    m_icon_button = make_button(BUTTON_SIZE, {}, "Go to Tab");
    // No page tells Ladybird about its tracks (there's no Media Session API yet), so these stay off, as Arc shows them
    // for sites without tracks.
    m_previous_button = make_button(BUTTON_SIZE, "backward.end.fill", "Previous");
    m_previous_button->setEnabled(false);
    m_play_button = make_button(BUTTON_SIZE, "play.fill", "Play");
    m_next_button = make_button(BUTTON_SIZE, "forward.end.fill", "Next");
    m_next_button->setEnabled(false);
    m_mute_button = make_button(BUTTON_SIZE, "speaker.fill", "Mute");
    m_picture_in_picture_button = make_button(SMALL_BUTTON_SIZE, "pip.enter", "Picture in Picture");
    m_close_button = make_button(SMALL_BUTTON_SIZE, "xmark", "Close");

    connect(m_icon_button, &QAbstractButton::clicked, this, &MiniPlayer::go_to_tab);
    connect(m_play_button, &QAbstractButton::clicked, this, &MiniPlayer::toggle_playback);
    connect(m_mute_button, &QAbstractButton::clicked, this, &MiniPlayer::toggle_mute);
    connect(m_picture_in_picture_button, &QAbstractButton::clicked, this, &MiniPlayer::picture_in_picture);
    connect(m_close_button, &QAbstractButton::clicked, this, &MiniPlayer::dismiss);

    m_close_button->hide();
    m_picture_in_picture_button->hide();

    m_reveal_animation = new QVariantAnimation(this);
    m_reveal_animation->setDuration(REVEAL_MS);
    m_reveal_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_reveal_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        set_reveal(value.toReal());
    });
}

void MiniPlayer::set_title(QString const& title)
{
    if (m_title == title)
        return;
    m_title = title;
    if (m_reveal > 0)
        update(QRect(0, 0, width(), height() - CONTROLS_HEIGHT));
}

void MiniPlayer::set_icon(QIcon const& icon)
{
    m_icon_button->set_picture(icon.isNull() ? QIcon() : icon);
    if (icon.isNull())
        m_icon_button->set_symbol("globe");
}

void MiniPlayer::set_playing(bool playing)
{
    m_playing = playing;
    m_play_button->set_symbol(playing ? "pause.fill" : "play.fill");
    m_play_button->setToolTip(playing ? "Pause" : "Play");
}

void MiniPlayer::set_muted(bool muted)
{
    m_muted = muted;
    m_mute_button->set_symbol(muted ? "speaker.slash.fill" : "speaker.fill");
    m_mute_button->setToolTip(muted ? "Unmute" : "Mute");
}

void MiniPlayer::set_can_picture_in_picture(bool can)
{
    m_picture_in_picture_button->setEnabled(can);
}

void MiniPlayer::set_slot(QRect slot)
{
    if (m_slot == slot)
        return;
    m_slot = slot;
    place();
}

void MiniPlayer::set_revealed(bool revealed)
{
    auto target = revealed ? 1.0 : 0.0;
    if (m_reveal_animation->state() == QAbstractAnimation::Running && m_reveal_animation->endValue().toReal() == target)
        return;
    if (m_reveal == target && m_reveal_animation->state() != QAbstractAnimation::Running)
        return;
    m_reveal_animation->stop();
    m_reveal_animation->setStartValue(m_reveal);
    m_reveal_animation->setEndValue(target);
    m_reveal_animation->start();
}

void MiniPlayer::set_reveal(qreal reveal)
{
    if (m_reveal == reveal)
        return;
    m_reveal = reveal;
    place();
}

// Grows upward from the slot by as much of the title row as is showing.
void MiniPlayer::place()
{
    auto height = CONTROLS_HEIGHT + qRound(TITLE_HEIGHT * m_reveal);
    if (height != m_requested_height) {
        m_requested_height = height;
        emit height_changed(height);
    }
    auto geometry = QRect(m_slot.left(), m_slot.bottom() + 1 - height, m_slot.width(), height);
    if (geometry != this->geometry())
        setGeometry(geometry);
    else
        lay_out_children();
}

void MiniPlayer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    lay_out_children();
}

void MiniPlayer::lay_out_children()
{
    // The controls along the bottom, evenly spread.
    auto controls_top = height() - CONTROLS_HEIGHT + (CONTROLS_HEIGHT - BUTTON_SIZE) / 2;
    PlayerButton* controls[] = { m_icon_button, m_previous_button, m_play_button, m_next_button, m_mute_button };
    auto span = width() - SIDE_PADDING * 2 - BUTTON_SIZE;
    for (int i = 0; i < 5; ++i)
        controls[i]->move(SIDE_PADDING + qRound(span * i / 4.0), controls_top);

    // The title row sits on top of them, and comes into view as the player grows.
    auto title_top = height() - CONTROLS_HEIGHT - TITLE_HEIGHT;
    auto button_top = title_top + (TITLE_HEIGHT - SMALL_BUTTON_SIZE) / 2 + 3;
    m_close_button->move(width() - SIDE_PADDING - SMALL_BUTTON_SIZE, button_top);
    m_picture_in_picture_button->move(m_close_button->x() - SMALL_BUTTON_SIZE - 2, button_top);
    auto shows_title_row = m_reveal > 0;
    if (m_close_button->isHidden() == shows_title_row) {
        m_close_button->setVisible(shows_title_row);
        m_picture_in_picture_button->setVisible(shows_title_row);
    }
}

void MiniPlayer::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath card;
    card.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), RADIUS, RADIUS);
    painter.fillPath(card, card_color());
    painter.setPen(QPen(QColor(255, 255, 255, 18), 1));
    painter.drawPath(card);

    if (m_reveal <= 0 || m_title.isEmpty())
        return;
    auto title_top = height() - CONTROLS_HEIGHT - TITLE_HEIGHT;
    auto font = painter.font();
    font.setPixelSize(13);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setOpacity(m_reveal);
    painter.setPen(foreground_color());
    auto text_rect = QRect(SIDE_PADDING + 6, title_top + 3, m_picture_in_picture_button->x() - SIDE_PADDING - 12, TITLE_HEIGHT);
    painter.drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft, painter.fontMetrics().elidedText(m_title, Qt::ElideRight, text_rect.width()));
}

void MiniPlayer::enterEvent(QEnterEvent* event)
{
    set_revealed(true);
    QWidget::enterEvent(event);
}

void MiniPlayer::leaveEvent(QEvent* event)
{
    set_revealed(false);
    QWidget::leaveEvent(event);
}

}
