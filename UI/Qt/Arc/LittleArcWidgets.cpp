/*
 * Pieces of Little Arc's bar.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/LittleArcWidgets.h>
#include <UI/Qt/Arc/SymbolIcon.h>
#include <UI/Qt/ChromeStyle.h>

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

namespace Ladybird::Arc {

static constexpr int TEXT_PIXEL_SIZE = 12;

static QColor ink(QPalette const& palette, int alpha)
{
    return ChromeStyle::is_dark(palette) ? QColor(255, 255, 255, alpha) : QColor(0, 0, 0, alpha);
}

QString profile_label(QString const& profile)
{
    return profile.isEmpty() ? QString("Default") : profile;
}

// --- The profile ---------------------------------------------------------------------------------

static constexpr int PILL_HEIGHT = 22;
static constexpr int PILL_PADDING = 7;
static constexpr int FACE_SIZE = 12;
static constexpr int FACE_GAP = 5;

ProfilePill::ProfilePill(QWidget* parent)
    : QWidget(parent)
{
    auto pill_font = font();
    pill_font.setPixelSize(TEXT_PIXEL_SIZE - 1);
    pill_font.setWeight(QFont::DemiBold);
    setFont(pill_font);
    set_profile({});
}

void ProfilePill::set_profile(QString const& profile)
{
    auto label = profile_label(profile);
    setToolTip(QString("Browsing in the %1 profile").arg(label));
    if (m_label == label)
        return;
    m_label = label;
    updateGeometry();
    update();
}

QSize ProfilePill::sizeHint() const
{
    auto text = QFontMetricsF(font()).horizontalAdvance(m_label);
    return { static_cast<int>(qCeil(PILL_PADDING + FACE_SIZE + FACE_GAP + text + PILL_PADDING)), PILL_HEIGHT };
}

void ProfilePill::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    auto pill = QRectF(0, (height() - PILL_HEIGHT) / 2.0, width(), PILL_HEIGHT);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink(palette(), 18));
    painter.drawRoundedRect(pill, 6, 6);

    auto color = ink(palette(), 200);
    auto face = fitted_symbol_pixmap("face.smiling", FACE_SIZE, color, devicePixelRatioF());
    if (!face.isNull()) {
        auto size = face.deviceIndependentSize();
        painter.drawPixmap(QPointF(PILL_PADDING + (FACE_SIZE - size.width()) / 2, pill.center().y() - size.height() / 2), face);
    }
    painter.setPen(color);
    painter.drawText(pill.adjusted(PILL_PADDING + FACE_SIZE + FACE_GAP, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, m_label);
}

// --- Open in -------------------------------------------------------------------------------------

static constexpr int BUTTON_HEIGHT = 28;
static constexpr int BUTTON_PADDING = 10;
static constexpr int SHORTCUT_GAP = 9;
static constexpr int CHEVRON_WIDTH = 26;
static constexpr int CHEVRON_SIZE = 10;
static constexpr qreal BUTTON_RADIUS = 8;
static constexpr auto OPEN_IN = "Open in ";
static constexpr auto SHORTCUT = "⌘O";

OpenInButton::OpenInButton(QWidget* parent)
    : QWidget(parent)
{
    auto button_font = font();
    button_font.setPixelSize(TEXT_PIXEL_SIZE);
    setFont(button_font);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

void OpenInButton::set_space_title(QString const& title)
{
    if (m_space_title == title)
        return;
    m_space_title = title;
    setToolTip(QString("Open in %1 (⌘O)").arg(title));
    updateGeometry();
    update();
}

QFont OpenInButton::space_font() const
{
    auto bold = font();
    bold.setWeight(QFont::DemiBold);
    return bold;
}

qreal OpenInButton::main_width() const
{
    QFontMetricsF regular(font());
    QFontMetricsF bold(space_font());
    return BUTTON_PADDING + regular.horizontalAdvance(OPEN_IN) + bold.horizontalAdvance(m_space_title) + SHORTCUT_GAP
        + regular.horizontalAdvance(SHORTCUT) + BUTTON_PADDING;
}

QSize OpenInButton::sizeHint() const
{
    return { static_cast<int>(qCeil(main_width())) + CHEVRON_WIDTH, BUTTON_HEIGHT };
}

QRectF OpenInButton::main_rect() const
{
    auto top = (height() - BUTTON_HEIGHT) / 2.0;
    return QRectF(0, top, qMax<qreal>(0, width() - CHEVRON_WIDTH), BUTTON_HEIGHT);
}

QRectF OpenInButton::chevron_rect() const
{
    auto main = main_rect();
    return QRectF(main.right(), main.top(), width() - main.right(), BUTTON_HEIGHT);
}

OpenInButton::Part OpenInButton::part_at(QPointF position) const
{
    if (main_rect().contains(position))
        return Part::Main;
    if (chevron_rect().contains(position))
        return Part::Chevron;
    return Part::None;
}

void OpenInButton::mouseMoveEvent(QMouseEvent* event)
{
    auto part = part_at(event->position());
    if (part != m_hover) {
        m_hover = part;
        setToolTip(part == Part::Chevron ? QString("Open in Another Space (⌥⌘O)") : QString("Open in %1 (⌘O)").arg(m_space_title));
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void OpenInButton::leaveEvent(QEvent* event)
{
    m_hover = Part::None;
    update();
    QWidget::leaveEvent(event);
}

void OpenInButton::mousePressEvent(QMouseEvent* event)
{
    m_pressed = event->button() == Qt::LeftButton ? part_at(event->position()) : Part::None;
    event->accept();
    update();
}

void OpenInButton::mouseReleaseEvent(QMouseEvent* event)
{
    auto pressed = m_pressed;
    m_pressed = Part::None;
    update();
    if (pressed == Part::None || event->button() != Qt::LeftButton || part_at(event->position()) != pressed)
        return;
    if (pressed == Part::Main)
        emit open_requested();
    else
        emit spaces_requested();
}

void OpenInButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    auto main = main_rect();
    auto chevron = chevron_rect();
    auto outline = main.united(chevron).adjusted(0.5, 0.5, -0.5, -0.5);

    painter.setPen(QPen(ink(palette(), 34), 1));
    painter.setBrush(ink(palette(), 12));
    painter.drawRoundedRect(outline, BUTTON_RADIUS, BUTTON_RADIUS);

    // The hovered (or pressed) part lights up, inside the outline.
    auto lit = m_pressed != Part::None ? m_pressed : m_hover;
    if (lit != Part::None) {
        painter.save();
        QPainterPath clip;
        clip.addRoundedRect(outline, BUTTON_RADIUS, BUTTON_RADIUS);
        painter.setClipPath(clip);
        painter.fillRect(lit == Part::Main ? main : chevron, ink(palette(), m_pressed != Part::None ? 26 : 16));
        painter.restore();
    }

    // The divider.
    painter.setPen(QPen(ink(palette(), 34), 1));
    painter.drawLine(QPointF(chevron.left() + 0.5, outline.top() + 6), QPointF(chevron.left() + 0.5, outline.bottom() - 6));

    // "Open in", the space, the shortcut.
    QFontMetricsF regular(font());
    QFontMetricsF bold(space_font());
    auto x = main.left() + BUTTON_PADDING;
    auto draw = [&](QString const& text, QFont const& text_font, QColor color, qreal advance) {
        painter.setFont(text_font);
        painter.setPen(color);
        painter.drawText(QRectF(x, main.top(), advance + 1, main.height()), Qt::AlignLeft | Qt::AlignVCenter, text);
        x += advance;
    };
    draw(OPEN_IN, font(), ink(palette(), 175), regular.horizontalAdvance(OPEN_IN));
    draw(m_space_title, space_font(), ink(palette(), 235), bold.horizontalAdvance(m_space_title));
    x += SHORTCUT_GAP;
    draw(SHORTCUT, font(), ink(palette(), 110), regular.horizontalAdvance(SHORTCUT));

    auto arrow = fitted_symbol_pixmap("chevron.down", CHEVRON_SIZE, ink(palette(), 175), devicePixelRatioF());
    if (!arrow.isNull()) {
        auto size = arrow.deviceIndependentSize();
        painter.drawPixmap(QPointF(chevron.center().x() - size.width() / 2, chevron.center().y() - size.height() / 2), arrow);
    }
}

}
