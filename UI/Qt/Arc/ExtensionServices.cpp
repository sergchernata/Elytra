/*
 * Extensions' notifications and context menu items, in the Arc UI.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebExtensions/Manifest.h>
#include <LibWebView/Application.h>
#include <LibWebView/ExtensionAPI/ContextMenus.h>
#include <LibWebView/ExtensionAPI/Notifications.h>
#include <LibWebView/ExtensionAPI/TabsDelegate.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>
#include <LibWebView/Menu.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Arc/ExtensionServices.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/WebContentView.h>

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QDialog>
#include <QEvent>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace Ladybird::Arc {

static WebView::ExtensionRuntime& runtime() { return WebView::Application::extension_runtime(); }
static WebView::ExtensionRegistry& registry() { return WebView::Application::extension_registry(); }

// Notifications

static constexpr int NOTIFICATION_WIDTH = 340;
static constexpr int NOTIFICATION_MARGIN = 12; // From the page's top and right edges, and between cards.
static constexpr int NOTIFICATION_ICON_SIZE = 40;
// How long a notification shows unless it requires interaction (longer when the extension says it matters).
static constexpr int NOTIFICATION_TIMEOUT_MS = 8000;
static constexpr int IMPORTANT_NOTIFICATION_TIMEOUT_MS = 25000;
// The image fits in a box this size, and a card is no taller than the maximum; only a few show at once.
static constexpr int NOTIFICATION_IMAGE_WIDTH = 240;
static constexpr int NOTIFICATION_IMAGE_HEIGHT = 240;
static constexpr int NOTIFICATION_MAXIMUM_HEIGHT = 420;
static constexpr int NOTIFICATION_MAXIMUM_TEXT_LENGTH = 1000;
static constexpr int NOTIFICATION_CARDS_SHOWN = 3;
// Images larger than this aren't decoded at all.
static constexpr int NOTIFICATION_IMAGE_MAXIMUM_SIDE = 8192;
static constexpr qint64 NOTIFICATION_IMAGE_MAXIMUM_PIXELS = 16'000'000;

// An extension's image, decoded only when it's of a sensible size, and made to fit the box (never larger).
static QPixmap bounded_pixmap(ReadonlyBytes bytes, QSize box)
{
    if (bytes.is_empty() || bytes.size() > static_cast<size_t>(NumericLimits<int>::max()))
        return {};
    auto data = QByteArray::fromRawData(reinterpret_cast<char const*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
    QBuffer buffer(&data);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer);
    auto size = reader.size();
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0 || size.width() > NOTIFICATION_IMAGE_MAXIMUM_SIDE
        || size.height() > NOTIFICATION_IMAGE_MAXIMUM_SIDE || static_cast<qint64>(size.width()) * size.height() > NOTIFICATION_IMAGE_MAXIMUM_PIXELS)
        return {};
    auto target = size.width() > box.width() || size.height() > box.height() ? size.scaled(box, Qt::KeepAspectRatio) : size;
    target = target.expandedTo(QSize(1, 1));
    reader.setScaledSize(target);
    auto image = reader.read();
    if (image.isNull())
        return {};
    // Not every format scales as it decodes.
    if (image.width() > box.width() || image.height() > box.height())
        image = image.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return QPixmap::fromImage(image);
}

// Whether the browser is asking the user something in the window (a sheet or dialog): cards stay out of its way.
static bool browser_prompt_is_open(BrowserWindow& window)
{
    if (QApplication::activeModalWidget())
        return true;
    for (auto* dialog : window.findChildren<QDialog*>()) {
        if (dialog->isVisible())
            return true;
    }
    return false;
}

class NotificationCard;
static QList<QPointer<NotificationCard>>& notification_cards()
{
    static QList<QPointer<NotificationCard>> cards;
    return cards;
}
static void place_notification_cards();

// One notification, over the top right of the window's page; the newest at the top. Clicking it, or one of its
// buttons, tells the extension; its × closes it.
class NotificationCard final : public QWidget {
public:
    NotificationCard(BrowserWindow& window, WebView::ExtensionNotification const& notification)
        // Its own little window, as the toast is: it shows over the page, which draws on its own.
        : QWidget(&window, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus)
        , m_window(&window)
        , m_extension_id(notification.extension_id)
        , m_id(notification.id)
    {
        setObjectName("ArcExtensionNotification");
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_DeleteOnClose);
        setFixedWidth(NOTIFICATION_WIDTH);
        setMaximumHeight(NOTIFICATION_MAXIMUM_HEIGHT);
        setCursor(Qt::PointingHandCursor);
        setStyleSheet(R"(
            #ArcExtensionNotification { background: rgb(32, 32, 36); border: 1px solid rgba(255, 255, 255, 30); border-radius: 12px; }
            QLabel { color: rgba(255, 255, 255, 235); background: transparent; }
            QLabel#title { font-weight: 600; }
            QLabel#context, QLabel#source { color: rgba(255, 255, 255, 140); }
            QPushButton { color: white; background: rgba(255, 255, 255, 30); border: none; border-radius: 6px; padding: 4px 10px; }
            QPushButton:hover { background: rgba(255, 255, 255, 50); }
            QToolButton { color: rgba(255, 255, 255, 160); background: transparent; border: none; }
            QProgressBar { background: rgba(255, 255, 255, 30); border: none; border-radius: 3px; max-height: 6px; }
            QProgressBar::chunk { background: rgb(91, 141, 239); border-radius: 3px; }
        )");
        setAttribute(Qt::WA_StyledBackground);

        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, [this] {
            // Without a notification center to keep it in, a notification that times out is closed.
            runtime().notifications().closed(m_extension_id, m_id, false);
            dismiss();
        });

        update_from(notification);
        window.installEventFilter(this);
    }

    bool is_for(StringView extension_id, StringView id) const { return m_extension_id == extension_id && m_id == id; }
    BrowserWindow* browser_window() const { return m_window; }

    void update_from(WebView::ExtensionNotification const& notification)
    {
        if (auto* old = layout()) {
            QLayoutItem* item;
            while ((item = old->takeAt(0))) {
                if (auto* widget = item->widget())
                    widget->deleteLater();
                if (auto* child_layout = item->layout())
                    delete_layout(child_layout);
                delete item;
            }
            delete old;
        }

        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(12, 10, 8, 10);
        layout->setSpacing(10);

        auto scale = devicePixelRatioF();
        auto icon = bounded_pixmap(notification.icon, QSize(NOTIFICATION_ICON_SIZE * scale, NOTIFICATION_ICON_SIZE * scale));
        if (!icon.isNull()) {
            auto* icon_label = new QLabel(this);
            auto scaled = icon;
            scaled.setDevicePixelRatio(scale);
            icon_label->setPixmap(scaled);
            icon_label->setFixedSize(NOTIFICATION_ICON_SIZE, NOTIFICATION_ICON_SIZE);
            layout->addWidget(icon_label, 0, Qt::AlignTop);
        }

        auto* text = new QVBoxLayout;
        text->setSpacing(3);
        auto add_label = [&](QString const& string, char const* name) {
            if (string.isEmpty())
                return;
            auto* label = new QLabel(string.left(NOTIFICATION_MAXIMUM_TEXT_LENGTH), this);
            label->setObjectName(name);
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(true);
            text->addWidget(label);
        };
        if (auto const* extension = registry().find(notification.extension_id))
            add_label(qstring_from_ak_string(extension->display_name()), "source");
        add_label(qstring_from_ak_string(notification.title), "title");
        add_label(qstring_from_ak_string(notification.message), "message");
        for (auto const& item : notification.items)
            add_label(QString("%1  %2").arg(qstring_from_ak_string(item.title), qstring_from_ak_string(item.message)), "item");
        if (notification.progress.has_value()) {
            auto* progress = new QProgressBar(this);
            progress->setRange(0, 100);
            progress->setValue(*notification.progress);
            progress->setTextVisible(false);
            text->addWidget(progress);
        }
        if (notification.image.has_value()) {
            auto image = bounded_pixmap(*notification.image, QSize(NOTIFICATION_IMAGE_WIDTH * scale, NOTIFICATION_IMAGE_HEIGHT * scale));
            if (!image.isNull()) {
                image.setDevicePixelRatio(scale);
                auto* image_label = new QLabel(this);
                image_label->setPixmap(image);
                image_label->setMaximumSize(NOTIFICATION_IMAGE_WIDTH, NOTIFICATION_IMAGE_HEIGHT);
                text->addWidget(image_label);
            }
        }
        add_label(qstring_from_ak_string(notification.context_message), "context");
        if (!notification.buttons.is_empty()) {
            auto* buttons = new QHBoxLayout;
            buttons->setSpacing(6);
            for (size_t i = 0; i < notification.buttons.size(); ++i) {
                auto* button = new QPushButton(qstring_from_ak_string(notification.buttons[i]), this);
                button->setCursor(Qt::PointingHandCursor);
                connect(button, &QPushButton::clicked, this, [this, i] {
                    runtime().notifications().button_clicked(m_extension_id, m_id, i);
                });
                buttons->addWidget(button);
            }
            buttons->addStretch();
            text->addLayout(buttons);
        }
        layout->addLayout(text, 1);

        auto* close = new QToolButton(this);
        close->setText(QString::fromUtf8("✕"));
        close->setToolTip("Close");
        close->setCursor(Qt::ArrowCursor);
        connect(close, &QToolButton::clicked, this, [this] {
            runtime().notifications().closed(m_extension_id, m_id, true);
            dismiss();
        });
        layout->addWidget(close, 0, Qt::AlignTop);

        adjustSize();
        resize(width(), qMin(height(), NOTIFICATION_MAXIMUM_HEIGHT));
        if (notification.require_interaction)
            m_timer->stop();
        else
            m_timer->start(notification.priority > 0 ? IMPORTANT_NOTIFICATION_TIMEOUT_MS : NOTIFICATION_TIMEOUT_MS);
    }

    void dismiss()
    {
        m_timer->stop();
        hide();
        notification_cards().removeAll(this);
        deleteLater();
        place_notification_cards();
    }

protected:
    virtual void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton)
            runtime().notifications().clicked(m_extension_id, m_id);
    }

    virtual bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_window && (event->type() == QEvent::Move || event->type() == QEvent::Resize))
            place_notification_cards();
        return QWidget::eventFilter(watched, event);
    }

private:
    static void delete_layout(QLayout* layout)
    {
        QLayoutItem* item;
        while ((item = layout->takeAt(0))) {
            if (auto* widget = item->widget())
                widget->deleteLater();
            if (auto* child = item->layout())
                delete_layout(child);
            delete item;
        }
        delete layout;
    }

    QPointer<BrowserWindow> m_window;
    String m_extension_id;
    String m_id;
    QTimer* m_timer { nullptr };
};

// "+N more", below a window's cards, when there are more than show.
static QHash<BrowserWindow*, QPointer<QLabel>>& more_labels()
{
    static QHash<BrowserWindow*, QPointer<QLabel>> labels;
    return labels;
}

static QLabel* more_label_for(BrowserWindow& window)
{
    auto& label = more_labels()[&window];
    if (!label) {
        label = new QLabel(&window, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        label->setAttribute(Qt::WA_ShowWithoutActivating);
        label->setObjectName("ArcExtensionNotificationMore");
        label->setTextFormat(Qt::PlainText);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet("#ArcExtensionNotificationMore { color: rgba(255, 255, 255, 220); background: rgb(32, 32, 36); border: 1px solid rgba(255, 255, 255, 30); border-radius: 10px; padding: 4px 10px; }");
    }
    return label;
}

// While cards show, they follow prompts opening and closing.
static QTimer& card_watch_timer()
{
    static auto* timer = [] {
        auto* timer = new QTimer(qApp);
        timer->setInterval(250);
        QObject::connect(timer, &QTimer::timeout, [] { place_notification_cards(); });
        return timer;
    }();
    return *timer;
}

// The cards of each window, from the top right of its page down, inside the page (below the toolbar and address bar):
// the newest few, and how many more there are. None while the browser asks the user something.
static void place_notification_cards()
{
    struct Placement {
        QRect frame;
        int next_top { 0 };
        int shown { 0 };
        int hidden { 0 };
        bool suspended { false };
    };
    // Room is kept for the "+N more" label below the cards when some don't show.
    static constexpr int more_label_room = 40;
    QHash<BrowserWindow*, int> remaining;
    for (auto const& card : notification_cards()) {
        if (card && card->browser_window())
            ++remaining[card->browser_window()];
    }
    QHash<BrowserWindow*, Placement> placements;
    for (auto const& card : notification_cards()) {
        if (!card || !card->browser_window())
            continue;
        auto after_this = --remaining[card->browser_window()];
        auto* window = card->browser_window();
        if (!placements.contains(window)) {
            Placement placement;
            placement.frame = window->geometry();
            if (auto* tab = window->current_tab(); tab && tab->view().isVisible())
                placement.frame = QRect(tab->view().mapToGlobal(QPoint(0, 0)), tab->view().size());
            placement.next_top = placement.frame.top() + NOTIFICATION_MARGIN;
            placement.suspended = browser_prompt_is_open(*window) || window->isMinimized() || !window->isVisible();
            placements.insert(window, placement);
        }
        auto& placement = placements[window];
        auto bottom = placement.next_top + card->height();
        auto room_below = after_this > 0 ? more_label_room : 0;
        if (placement.suspended || placement.shown >= NOTIFICATION_CARDS_SHOWN || bottom > placement.frame.bottom() - NOTIFICATION_MARGIN - room_below) {
            card->hide();
            ++placement.hidden;
            continue;
        }
        card->move(qMax(placement.frame.left(), placement.frame.right() + 1 - NOTIFICATION_MARGIN - card->width()), placement.next_top);
        card->show();
        placement.next_top = bottom + NOTIFICATION_MARGIN / 2;
        ++placement.shown;
    }
    for (auto it = more_labels().begin(); it != more_labels().end();) {
        auto placement = placements.find(it.key());
        if (!it.value() || placement == placements.end()) {
            if (it.value())
                it.value()->deleteLater();
            it = more_labels().erase(it);
            continue;
        }
        ++it;
    }
    for (auto it = placements.begin(); it != placements.end(); ++it) {
        auto const& placement = it.value();
        if (placement.suspended || placement.hidden == 0) {
            if (auto label = more_labels().value(it.key()))
                label->hide();
            continue;
        }
        auto* label = more_label_for(*it.key());
        label->setText(QString("+%1 more").arg(placement.hidden));
        label->adjustSize();
        label->move(placement.frame.right() + 1 - NOTIFICATION_MARGIN - label->width(), placement.next_top);
        if (placement.next_top + label->height() <= placement.frame.bottom())
            label->show();
        else
            label->hide();
    }
    auto& timer = card_watch_timer();
    if (placements.isEmpty())
        timer.stop();
    else if (!timer.isActive())
        timer.start();
}

class NotificationsDelegate final : public WebView::ExtensionNotificationsDelegate {
public:
    virtual void show_notification(WebView::ExtensionNotification const& notification) override
    {
        for (auto const& card : notification_cards()) {
            if (card && card->is_for(notification.extension_id, notification.id)) {
                card->update_from(notification);
                place_notification_cards();
                return;
            }
        }
        // Never over a private window: extensions don't run there.
        auto* window = Application::the().non_private_window_if_any();
        if (!window)
            return;
        auto* card = new NotificationCard(*window, notification);
        notification_cards().prepend(card);
        place_notification_cards();
    }

    virtual void close_notification(StringView extension_id, StringView id) override
    {
        for (auto const& card : notification_cards()) {
            if (card && card->is_for(extension_id, id)) {
                card->dismiss();
                return;
            }
        }
    }
};

static NotificationsDelegate& notifications_delegate()
{
    static NotificationsDelegate delegate;
    return delegate;
}

// Context menus

static constexpr auto EXTENSION_ITEM_PROPERTY = "ElytraExtensionMenuItem";

static Optional<i64> tab_id_of(WebView::ViewImplementation const& view)
{
    auto* delegate = runtime().tabs_delegate();
    if (!delegate)
        return {};
    for (auto const& tab : delegate->tabs()) {
        if (tab.view == &view)
            return tab.id;
    }
    return {};
}

static QIcon extension_icon(StringView extension_id)
{
    auto const* extension = registry().find(extension_id);
    if (!extension)
        return {};
    auto path = WebExtensions::best_icon_for_size(extension->manifest().icons, 32);
    if (!path.has_value())
        return {};
    auto bytes = extension->read_file_bytes(*path, 4 * MiB);
    QPixmap pixmap;
    if (!bytes.has_value() || !pixmap.loadFromData(bytes->data(), bytes->size()))
        return {};
    return QIcon(pixmap);
}

static void remove_extension_items(QMenu& menu)
{
    for (auto* action : menu.actions()) {
        if (!action->property(EXTENSION_ITEM_PROPERTY).toBool())
            continue;
        menu.removeAction(action);
        if (auto* submenu = action->menu())
            submenu->deleteLater();
        action->deleteLater();
    }
}

// Titles are the extension's text: '&' isn't a shortcut marker, a tab doesn't start a shortcut's text, and control
// characters and very long titles don't show.
QString extension_menu_title(StringView title)
{
    static constexpr int maximum_length = 80;
    auto text = QString::fromUtf8(title.characters_without_null_termination(), static_cast<qsizetype>(title.length()));
    QString cleaned;
    cleaned.reserve(qMin(text.size(), maximum_length * 2));
    for (auto character : text) {
        auto category = character.category();
        if (character == '\t' || character == '\n' || character == '\r' || character.isSpace()) {
            if (!cleaned.isEmpty() && !cleaned.endsWith(' '))
                cleaned.append(' ');
            continue;
        }
        if (category == QChar::Other_Control || category == QChar::Other_Format || character == QChar::LineSeparator || character == QChar::ParagraphSeparator)
            continue;
        cleaned.append(character);
        if (cleaned.size() > maximum_length * 2)
            break;
    }
    cleaned = cleaned.trimmed();
    if (cleaned.size() > maximum_length) {
        cleaned.truncate(maximum_length - 1);
        if (cleaned.back().isHighSurrogate())
            cleaned.chop(1);
        cleaned = cleaned.trimmed() + QChar(0x2026);
    }
    return cleaned.replace('&', "&&");
}

static void append_items(QMenu& menu, Vector<WebView::ContextMenusAPI::MenuItem> const& items, WebView::ContextMenusAPI::Target const& target, bool top_level)
{
    for (auto const& item : items) {
        auto title = extension_menu_title(item.title);
        QAction* action = nullptr;
        if (item.type == "separator"sv) {
            action = menu.addSeparator();
        } else if (!item.children.is_empty()) {
            auto* submenu = new QMenu(title, &menu);
            append_items(*submenu, item.children, target, false);
            action = menu.addMenu(submenu);
        } else {
            action = menu.addAction(title);
            if (item.type == "checkbox"sv || item.type == "radio"sv) {
                action->setCheckable(true);
                action->setChecked(item.checked);
            }
            if (!item.key.is_empty()) {
                QObject::connect(action, &QAction::triggered, [extension_id = item.extension_id, key = item.key, target] {
                    runtime().context_menus().clicked(extension_id, key, target);
                });
            }
        }
        action->setEnabled(item.enabled);
        if (top_level) {
            action->setProperty(EXTENSION_ITEM_PROPERTY, true);
            if (item.type != "separator"sv)
                action->setIcon(extension_icon(item.extension_id));
        }
    }
}

static WebView::ContextMenusAPI::Target page_target_for(WebContentView& view)
{
    WebView::ContextMenusAPI::Target target;
    target.page_url = view.url();
    auto const& clicked = view.context_menu_target();
    target.link_url = clicked.link_url;
    target.src_url = clicked.src_url;
    if (clicked.media_type.has_value())
        target.media_type = MUST(String::from_utf8(*clicked.media_type));
    target.selection_text = clicked.selection_text;
    target.editable = clicked.editable;
    target.tab_id = tab_id_of(view);
    return target;
}

static void add_page_items(QMenu& menu, WebView::ContextMenusAPI::Target const& target)
{
    remove_extension_items(menu);
    auto items = runtime().context_menus().items_for(target);
    if (items.is_empty())
        return;
    auto* separator = menu.addSeparator();
    separator->setProperty(EXTENSION_ITEM_PROPERTY, true);
    append_items(menu, items, target, true);
}

void add_extension_items_to_context_menus(WebContentView& view, Vector<ExtensionContextMenu> menus)
{
    for (auto& [menu, qmenu] : menus) {
        if (!menu || !qmenu)
            continue;
        auto show = move(menu->on_activation);
        menu->on_activation = [view = QPointer { &view }, qmenu = qmenu, show = move(show)](Gfx::IntPoint position) {
            if (view && qmenu)
                add_page_items(*qmenu, page_target_for(*view));
            if (show)
                show(position);
        };
    }
}

void add_extension_page_items_for_testing(QMenu& menu, BrowserWindow& window)
{
    if (auto* tab = window.current_tab()) {
        auto target = page_target_for(tab->view());
        // Nothing in particular was right-clicked: the page.
        target.link_url.clear();
        target.src_url.clear();
        target.media_type.clear();
        target.selection_text.clear();
        target.editable = false;
        add_page_items(menu, target);
    }
}

void add_extension_action_items(QMenu& menu, StringView extension_id, BrowserWindow* window)
{
    WebView::ContextMenusAPI::Target target;
    target.action_of_extension = MUST(String::from_utf8(extension_id));
    if (window && window->current_tab()) {
        auto& view = window->current_tab()->view();
        target.page_url = view.url();
        target.tab_id = tab_id_of(view);
    }
    auto items = runtime().context_menus().items_for(target);
    if (items.is_empty())
        return;
    append_items(menu, items, target, false);
    menu.addSeparator();
}

void show_extension_notification_for_testing(WebView::ExtensionNotification const& notification)
{
    notifications_delegate().show_notification(notification);
}

void close_extension_notification_for_testing(StringView extension_id, StringView id)
{
    notifications_delegate().close_notification(extension_id, id);
}

QList<QWidget*> extension_notification_cards_for_testing()
{
    QList<QWidget*> cards;
    for (auto const& card : notification_cards()) {
        if (card)
            cards.append(card);
    }
    return cards;
}

QWidget* extension_notification_more_label_for_testing(BrowserWindow& window)
{
    return more_labels().value(&window);
}

void install_extension_services()
{
    runtime().notifications().set_delegate(&notifications_delegate());
}

}
