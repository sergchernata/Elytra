/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Function.h>
#include <AK/QuickSort.h>
#include <LibURL/Parser.h>
#include <LibWebExtensions/Manifest.h>
#include <LibWebView/Application.h>
#include <LibWebView/ExtensionAPI/Action.h>
#include <LibWebView/ExtensionAPI/TabsDelegate.h>
#include <LibWebView/ExtensionRegistry.h>
#include <LibWebView/ExtensionRuntime.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Arc/Extensions.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/WebContentView.h>

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

namespace Ladybird::Arc {

static WebView::ExtensionRuntime& runtime() { return WebView::Application::extension_runtime(); }
static WebView::ExtensionRegistry& registry() { return WebView::Application::extension_registry(); }

// The windows extensions see. Private windows aren't among them: Chrome keeps extensions out of incognito unless the
// user allows each one, and Elytra doesn't offer that yet.
static QList<BrowserWindow*> browser_windows()
{
    QList<BrowserWindow*> windows;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = qobject_cast<BrowserWindow*>(widget); window && !window->isHidden() && window->is_private() == WebView::IsPrivate::No)
            windows.append(window);
    }
    return windows;
}

// Bounds an extension asks for, kept on the screen they're mostly on.
static QRect clamped_to_screen(QRect bounds)
{
    auto* screen = QGuiApplication::screenAt(bounds.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    auto available = screen->availableGeometry();
    bounds.setWidth(qBound(200, bounds.width(), available.width()));
    bounds.setHeight(qBound(150, bounds.height(), available.height()));
    bounds.moveLeft(qBound(available.left(), bounds.left(), available.right() - bounds.width() + 1));
    bounds.moveTop(qBound(available.top(), bounds.top(), available.bottom() - bounds.height() + 1));
    return bounds;
}

// chrome.tabs and chrome.windows: the browser windows, and their tabs, with IDs that last as long as they do.
class TabsDelegate final : public WebView::ExtensionTabsDelegate {
public:
    i64 id_of(QObject& object) const
    {
        // An entry whose object is gone is dropped: its address may be another object's now.
        if (auto it = m_ids.find(&object); it != m_ids.end()) {
            if (it->first)
                return it->second;
            m_ids.erase(it);
        }
        m_ids.insert(&object, { QPointer<QObject>(&object), m_next_id });
        return m_next_id++;
    }

    BrowserWindow* window_with_id(i64 id) const
    {
        for (auto* window : browser_windows()) {
            if (id_of(*window) == id)
                return window;
        }
        return nullptr;
    }

    struct TabPlace {
        BrowserWindow* window { nullptr };
        Ladybird::Tab* tab { nullptr };
        int index { -1 };
    };
    TabPlace tab_with_id(i64 id) const
    {
        for (auto* window : browser_windows()) {
            for (int i = 0; i < window->tab_count(); ++i) {
                if (auto* tab = window->tab_at(i); tab && id_of(*tab) == id)
                    return { window, tab, i };
            }
        }
        return {};
    }

    Optional<i64> tab_id_of_view(WebView::ViewImplementation const& view) const
    {
        for (auto* window : browser_windows()) {
            for (int i = 0; i < window->tab_count(); ++i) {
                auto* tab = window->tab_at(i);
                if (tab && tab->is_loaded() && static_cast<WebView::ViewImplementation const*>(&tab->view()) == &view)
                    return id_of(*tab);
            }
        }
        return {};
    }

    virtual Vector<Window> windows() const override
    {
        Vector<Window> windows;
        auto* active = Application::the().active_window_if_any();
        for (auto* window : browser_windows()) {
            Window description;
            description.id = id_of(*window);
            description.focused = window == active && window->isActiveWindow();
            description.incognito = window->is_private() == WebView::IsPrivate::Yes;
            description.type = window->is_popup_window() ? "popup"sv : "normal"sv;
            if (window->isMinimized())
                description.state = "minimized"sv;
            else if (window->isFullScreen())
                description.state = "fullscreen"sv;
            else if (window->isMaximized())
                description.state = "maximized"sv;
            auto geometry = window->frameGeometry();
            description.bounds = Gfx::IntRect { geometry.x(), geometry.y(), geometry.width(), geometry.height() };
            windows.append(description);
        }
        return windows;
    }

    virtual Vector<Tab> tabs() const override
    {
        Vector<Tab> tabs;
        for (auto* window : browser_windows()) {
            auto window_id = id_of(*window);
            for (int i = 0; i < window->tab_count(); ++i) {
                auto* tab = window->tab_at(i);
                if (!tab)
                    continue;
                Tab description;
                description.id = id_of(*tab);
                description.window_id = window_id;
                description.index = i;
                description.view = tab->is_loaded() ? &tab->view() : nullptr;
                description.active = tab == window->current_tab();
                description.highlighted = description.active;
                description.pinned = tab->is_pinned();
                description.discarded = !tab->is_loaded();
                description.incognito = window->is_private() == WebView::IsPrivate::Yes;
                if (tab->is_loaded())
                    description.muted = tab->view().page_mute_state() == Web::HTML::MuteState::Muted;
                description.title = ak_string_from_qstring(tab->title());
                tabs.append(description);
            }
        }
        return tabs;
    }

    virtual Optional<i64> last_focused_window_id() const override
    {
        if (auto* window = Application::the().active_window_if_any())
            return id_of(*window);
        auto windows = browser_windows();
        if (windows.isEmpty())
            return {};
        return id_of(*windows.first());
    }

    virtual ErrorOr<i64, String> create_tab(CreateTab details) override
    {
        auto* window = details.window_id.has_value() ? window_with_id(*details.window_id) : Application::the().active_window_if_any();
        if (!window)
            return details.window_id.has_value() ? MUST(String::formatted("No window with id: {}.", *details.window_id)) : "No window to open the tab in"_string;
        auto location = details.index.has_value() && *details.index >= 0 && *details.index <= window->tab_count()
            ? BrowserWindow::TabLocation::at_index(static_cast<int>(*details.index))
            : BrowserWindow::TabLocation::end();
        auto& tab = window->create_new_tab(details.active ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No, location);
        if (details.pinned)
            window->pin_tab(tab);
        return id_of(tab);
    }

    virtual ErrorOr<void, String> update_tab(i64 tab_id, UpdateTab details) override
    {
        auto place = tab_with_id(tab_id);
        if (!place.tab)
            return MUST(String::formatted("No tab with id: {}.", tab_id));
        if (details.pinned.has_value() && *details.pinned != place.tab->is_pinned()) {
            if (*details.pinned)
                place.window->pin_tab(*place.tab);
            else
                place.window->unpin_tab(*place.tab);
        }
        if (details.muted.has_value() && place.tab->is_loaded()) {
            auto muted = place.tab->view().page_mute_state() == Web::HTML::MuteState::Muted;
            if (muted != *details.muted)
                place.tab->view().toggle_page_mute_state();
        }
        if ((details.active.value_or(false) || details.highlighted.value_or(false)) && place.window->tab_index(place.tab) >= 0)
            place.window->activate_tab(place.window->tab_index(place.tab));
        return {};
    }

    virtual ErrorOr<void, String> move_tab(i64 tab_id, i64 window_id, i64 index) override
    {
        auto place = tab_with_id(tab_id);
        if (!place.tab)
            return MUST(String::formatted("No tab with id: {}.", tab_id));
        auto* target = window_with_id(window_id);
        if (!target)
            return MUST(String::formatted("No window with id: {}.", window_id));
        if (target == place.window) {
            auto last = target->tab_count() - 1;
            auto new_index = index < 0 || index > last ? last : static_cast<int>(index);
            target->move_tab(place.index, new_index);
            return {};
        }
        auto new_index = index < 0 || index > target->tab_count() ? target->tab_count() : static_cast<int>(index);
        place.window->move_tab_to_window(place.index, *target, new_index);
        return {};
    }

    virtual void close_tab(i64 tab_id) override
    {
        if (auto place = tab_with_id(tab_id); place.tab)
            place.window->request_to_close_tab(place.index);
    }

    virtual ErrorOr<i64, String> create_window(CreateWindow details) override
    {
        if (details.incognito)
            return "Extensions can't open private windows"_string;
        auto& window = Application::the().new_window({}, Application::the().configuration_for_new_window(),
            details.type == "popup"sv ? BrowserWindow::IsPopupWindow::Yes : BrowserWindow::IsPopupWindow::No,
            WebView::IsPrivate::No);
        if (details.bounds.has_value())
            window.setGeometry(clamped_to_screen({ details.bounds->x(), details.bounds->y(), details.bounds->width(), details.bounds->height() }));
        if (details.state.has_value())
            apply_state(window, *details.state);
        if (details.tab_id.has_value()) {
            if (auto place = tab_with_id(*details.tab_id); place.tab && place.window != &window)
                place.window->move_tab_to_window(place.index, window, 0);
        }
        if (details.focused) {
            window.raise();
            window.activateWindow();
        }
        return id_of(window);
    }

    virtual ErrorOr<void, String> update_window(i64 window_id, UpdateWindow details) override
    {
        auto* window = window_with_id(window_id);
        if (!window)
            return MUST(String::formatted("No window with id: {}.", window_id));
        if (details.state.has_value())
            apply_state(*window, *details.state);
        if (details.left.has_value() || details.top.has_value() || details.width.has_value() || details.height.has_value()) {
            auto geometry = window->geometry();
            window->setGeometry(clamped_to_screen({ details.left.value_or(geometry.x()), details.top.value_or(geometry.y()),
                details.width.value_or(geometry.width()), details.height.value_or(geometry.height()) }));
        }
        if (details.focused.value_or(false)) {
            window->raise();
            window->activateWindow();
        }
        if (details.draw_attention.value_or(false))
            QApplication::alert(window);
        return {};
    }

    virtual void close_window(i64 window_id) override
    {
        if (auto* window = window_with_id(window_id))
            window->close();
    }

    virtual void capture_visible_tab(i64 window_id, StringView format, u8 quality, Function<void(ErrorOr<String, String>)> on_complete) override
    {
        auto* window = window_with_id(window_id);
        auto* tab = window ? window->current_tab() : nullptr;
        if (!tab || !tab->is_loaded())
            return on_complete("No tab to capture"_string);
        auto pixmap = tab->view().tab_preview_pixmap(tab->view().size() * tab->view().devicePixelRatio());
        if (!pixmap.has_value())
            return on_complete("The tab has nothing to capture yet"_string);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        auto is_jpeg = format == "jpeg"sv;
        pixmap->save(&buffer, is_jpeg ? "JPEG" : "PNG", is_jpeg ? quality : -1);
        auto encoded = MUST(encode_base64({ bytes.constData(), static_cast<size_t>(bytes.size()) }));
        on_complete(MUST(String::formatted("data:image/{};base64,{}", is_jpeg ? "jpeg"sv : "png"sv, encoded)));
    }

private:
    static void apply_state(BrowserWindow& window, StringView state)
    {
        if (state == "minimized"sv)
            window.showMinimized();
        else if (state == "maximized"sv)
            window.showMaximized();
        else if (state == "fullscreen"sv)
            window.showFullScreen();
        else if (state == "normal"sv)
            window.showNormal();
    }

    mutable QHash<QObject const*, QPair<QPointer<QObject>, i64>> m_ids;
    mutable i64 m_next_id { 1 };
};

static TabsDelegate& tabs_delegate()
{
    static auto* delegate = new TabsDelegate;
    return *delegate;
}

// The tab a window shows, as extensions know it.
static Optional<i64> current_tab_id(BrowserWindow* window)
{
    if (!window || !window->current_tab())
        return {};
    return tabs_delegate().id_of(*window->current_tab());
}

// An extension's popup: its page, below its button, gone as soon as the user looks elsewhere.
class ExtensionPopup final : public QWidget {
public:
    static constexpr int MINIMUM_SIZE = 25;
    static constexpr int MAXIMUM_WIDTH = 800;
    static constexpr int MAXIMUM_HEIGHT = 600;

    ExtensionPopup(QWidget* anchor, URL::URL const& url)
        : QWidget(anchor, Qt::Popup | Qt::FramelessWindowHint)
        , m_anchor(anchor)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        m_view = new WebContentView(this);
        layout->addWidget(m_view);
        m_view->on_close = [this] { close(); };
        resize(320, 120);
        place();
        m_view->load(url);
        s_popups.append(this);
    }

    virtual ~ExtensionPopup() override
    {
        s_popups.removeAll(this);
    }

    WebView::ViewImplementation const& view() const { return *m_view; }

    // Fits the popup to its page, within Chrome's limits.
    void fit(int width, int height)
    {
        resize(qBound(MINIMUM_SIZE, width, MAXIMUM_WIDTH), qBound(MINIMUM_SIZE, height, MAXIMUM_HEIGHT));
        place();
    }

    static void close_all()
    {
        for (auto const& popup : s_popups) {
            if (popup)
                popup->close();
        }
    }

    static ExtensionPopup* showing(WebView::ViewImplementation const& view)
    {
        for (auto const& popup : s_popups) {
            if (popup && &popup->view() == &view)
                return popup;
        }
        return nullptr;
    }

    virtual void showEvent(QShowEvent* event) override
    {
        QWidget::showEvent(event);
        m_view->setFocus();
    }

private:
    void place()
    {
        if (!m_anchor)
            return;
        auto below = m_anchor->mapToGlobal(QPoint(m_anchor->width() / 2 - width() / 2, m_anchor->height() + 4));
        if (auto* screen = m_anchor->screen()) {
            auto available = screen->availableGeometry();
            below.setX(qBound(available.left() + 4, below.x(), available.right() - width() - 4));
            below.setY(qBound(available.top() + 4, below.y(), available.bottom() - height() - 4));
        }
        move(below);
    }

    QPointer<QWidget> m_anchor;
    WebContentView* m_view { nullptr };
    static inline QList<QPointer<ExtensionPopup>> s_popups;
};

// An extension's button: its icon, with its badge, and its title as the tooltip.
class ExtensionButton final : public QToolButton {
public:
    ExtensionButton(QWidget* parent, String extension_id)
        : QToolButton(parent)
        , m_extension_id(AK::move(extension_id))
    {
        setAutoRaise(true);
        setFixedSize(26, 26);
        // Like the sidebar's other buttons: no frame, a soft highlight under the pointer.
        setStyleSheet("QToolButton { border: none; background: transparent; border-radius: 6px; }"
                      "QToolButton:hover { background: rgba(127, 127, 127, 0.2); }"
                      "QToolButton:pressed { background: rgba(127, 127, 127, 0.3); }");
        setIconSize(QSize(16, 16));
        setFocusPolicy(Qt::NoFocus);
        setContextMenuPolicy(Qt::CustomContextMenu);
    }

    String const& extension_id() const { return m_extension_id; }

    void update_from(WebView::ActionAPI::State const& state)
    {
        // Plain text: an extension's title isn't to be rendered as rich text.
        setToolTip(QString("<qt>%1</qt>").arg(qstring_from_ak_string(state.title).left(100).toHtmlEscaped()));
        setEnabled(state.enabled);
        m_badge_text = qstring_from_ak_string(state.badge_text);
        m_badge_color = state.badge_background_color.has_value() && state.badge_background_color->alpha() > 0
            ? QColor(state.badge_background_color->red(), state.badge_background_color->green(), state.badge_background_color->blue(), state.badge_background_color->alpha())
            : QColor(0x5f, 0x63, 0x68);
        m_badge_text_color = state.badge_text_color.has_value() && state.badge_text_color->alpha() > 0
            ? QColor(state.badge_text_color->red(), state.badge_text_color->green(), state.badge_text_color->blue(), state.badge_text_color->alpha())
            : QColor(Qt::white);

        // The icon closest to what the screen needs.
        auto wanted = static_cast<u32>(qCeil(16 * devicePixelRatioF()));
        Optional<u32> best;
        for (auto const& [size, path] : state.icon_paths) {
            if (!best.has_value() || (size >= wanted && (*best < wanted || size < *best)) || (*best < wanted && size > *best))
                best = size;
        }
        auto icon_path = best.has_value() ? state.icon_paths.get(*best).value() : String {};
        if (icon_path == m_icon_path && !icon().isNull()) {
            update();
            return;
        }
        m_icon_path = icon_path;
        QIcon icon;
        if (best.has_value()) {
            if (auto const* extension = registry().find(m_extension_id)) {
                if (auto bytes = extension->read_file_bytes(state.icon_paths.get(*best).value(), 4 * MiB); bytes.has_value()) {
                    QPixmap pixmap;
                    if (pixmap.loadFromData(reinterpret_cast<uchar const*>(bytes->data()), bytes->size()))
                        icon = QIcon(pixmap);
                }
            }
        }
        if (icon.isNull())
            icon = QIcon::fromTheme("application-x-addon");
        setIcon(icon);
        update();
    }

protected:
    virtual void paintEvent(QPaintEvent* event) override
    {
        QToolButton::paintEvent(event);
        if (m_badge_text.isEmpty())
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto font = painter.font();
        font.setPixelSize(9);
        font.setBold(true);
        painter.setFont(font);
        auto text = m_badge_text.left(4);
        auto text_width = painter.fontMetrics().horizontalAdvance(text);
        QRectF badge(width() - text_width - 6, height() - 12, text_width + 6, 11);
        badge.moveRight(width() - 0.5);
        QPainterPath path;
        path.addRoundedRect(badge, 3, 3);
        painter.fillPath(path, m_badge_color);
        painter.setPen(m_badge_text_color);
        painter.drawText(badge, Qt::AlignCenter, text);
    }

private:
    String m_extension_id;
    String m_icon_path;
    QString m_badge_text;
    QColor m_badge_color;
    QColor m_badge_text_color;
};

class ExtensionButtons;
static QList<QPointer<ExtensionButtons>>& all_button_rows()
{
    static auto* rows = new QList<QPointer<ExtensionButtons>>;
    return *rows;
}

// A window's extension buttons, right-aligned in its parent.
class ExtensionButtons final : public QWidget {
public:
    ExtensionButtons(BrowserWindow& window, QWidget* parent)
        : QWidget(parent)
        , m_window(&window)
    {
        m_layout = new QHBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(2);
        parent->installEventFilter(this);
        all_button_rows().append(this);
        rebuild();
    }

    virtual ~ExtensionButtons() override
    {
        all_button_rows().removeAll(this);
    }

    ExtensionButton* button_for(StringView extension_id) const
    {
        for (auto const& button : m_buttons) {
            if (button && button->extension_id() == extension_id)
                return button;
        }
        return nullptr;
    }

    BrowserWindow* window() const { return m_window; }

    void rebuild()
    {
        for (auto const& button : m_buttons) {
            if (button)
                button->deleteLater();
        }
        m_buttons.clear();

        registry().for_each_extension([&](WebView::Extension const& extension) {
            if (!extension.manifest().action.has_value())
                return;
            auto* button = new ExtensionButton(this, extension.id());
            connect(button, &QToolButton::clicked, this, [this, id = extension.id()] {
                runtime().action().clicked(id, current_tab_id(m_window), m_window ? Optional<i64> { tabs_delegate().id_of(*m_window) } : Optional<i64> {});
            });
            connect(button, &QWidget::customContextMenuRequested, this, [button, id = extension.id()](QPoint position) {
                QMenu menu;
                auto const* extension = registry().find(id);
                if (extension) {
                    auto* name = menu.addAction(qstring_from_ak_string(extension->manifest().name));
                    name->setEnabled(false);
                    menu.addSeparator();
                }
                menu.addAction("Manage Extensions…", [] { show_extensions_window(); });
                menu.exec(button->mapToGlobal(position));
            });
            m_layout->addWidget(button);
            m_buttons.append(button);
            update_button(*button);
            button->show();
        });
        setVisible(!m_buttons.isEmpty());
        adjustSize();
        place();
    }

    void update_button(ExtensionButton& button)
    {
        auto state = runtime().action().state_for(button.extension_id(), current_tab_id(m_window));
        if (state.has_value())
            button.update_from(*state);
    }

    void update_extension(StringView extension_id)
    {
        if (!registry().find(extension_id) || !button_for(extension_id)) {
            rebuild();
            return;
        }
        update_button(*button_for(extension_id));
    }

    void update_all()
    {
        for (auto const& button : m_buttons) {
            if (button)
                update_button(*button);
        }
    }

protected:
    virtual bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == parent() && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
            place();
        return QWidget::eventFilter(watched, event);
    }

private:
    void place()
    {
        auto* parent_widget = parentWidget();
        if (!parent_widget)
            return;
        adjustSize();
        move(parent_widget->width() - width() - 8, (parent_widget->height() - height()) / 2 + 2);
        raise();
    }

    QPointer<BrowserWindow> m_window;
    QHBoxLayout* m_layout { nullptr };
    QList<QPointer<ExtensionButton>> m_buttons;
};

QWidget* create_extension_buttons(BrowserWindow& window, QWidget* parent)
{
    return new ExtensionButtons(window, parent);
}

// chrome.action: buttons change, and popups open below them.
class ActionDelegate final : public WebView::ExtensionActionDelegate {
public:
    virtual void action_changed(StringView extension_id) override
    {
        for (auto const& row : all_button_rows()) {
            if (row)
                row->update_extension(extension_id);
        }
    }

    virtual ErrorOr<void, String> open_popup(StringView extension_id, URL::URL const& popup, Optional<i64> window_id) override
    {
        auto* window = window_id.has_value() ? tabs_delegate().window_with_id(*window_id) : Application::the().active_window_if_any();
        if (!window)
            return "No window to open the popup in"_string;
        QWidget* anchor = nullptr;
        for (auto const& row : all_button_rows()) {
            if (row && row->window() == window && row->button_for(extension_id))
                anchor = row->button_for(extension_id);
        }
        if (!anchor)
            anchor = window;
        // Only in the window the user is in, and one at a time: a popup takes the keyboard.
        if (!window->isActiveWindow())
            return "The window isn't active"_string;
        ExtensionPopup::close_all();
        auto* popup_window = new ExtensionPopup(anchor, popup);
        popup_window->show();
        return {};
    }
};

// Keyboard commands: Chrome's key names, as manifests give them.
static Optional<QKeyCombination> parse_shortcut(StringView shortcut)
{
    if (shortcut.is_empty())
        return {};
    Qt::KeyboardModifiers modifiers;
    Optional<Qt::Key> key;
    for (auto part : shortcut.split_view('+')) {
        // On macOS, Qt's Control is the Command key and its Meta is Control.
        if (part == "Ctrl"sv || part == "Command"sv)
            modifiers |= Qt::ControlModifier;
        else if (part == "MacCtrl"sv)
            modifiers |= Qt::MetaModifier;
        else if (part == "Alt"sv || part == "Option"sv)
            modifiers |= Qt::AltModifier;
        else if (part == "Shift"sv)
            modifiers |= Qt::ShiftModifier;
        else if (part.length() == 1 && is_ascii_alphanumeric(part[0]))
            key = static_cast<Qt::Key>(to_ascii_uppercase(part[0]));
        else if (part == "Comma"sv)
            key = Qt::Key_Comma;
        else if (part == "Period"sv)
            key = Qt::Key_Period;
        else if (part == "Space"sv)
            key = Qt::Key_Space;
        else if (part == "Home"sv)
            key = Qt::Key_Home;
        else if (part == "End"sv)
            key = Qt::Key_End;
        else if (part == "PageUp"sv)
            key = Qt::Key_PageUp;
        else if (part == "PageDown"sv)
            key = Qt::Key_PageDown;
        else if (part == "Insert"sv)
            key = Qt::Key_Insert;
        else if (part == "Delete"sv)
            key = Qt::Key_Delete;
        else if (part == "Up"sv)
            key = Qt::Key_Up;
        else if (part == "Down"sv)
            key = Qt::Key_Down;
        else if (part == "Left"sv)
            key = Qt::Key_Left;
        else if (part == "Right"sv)
            key = Qt::Key_Right;
        else if (part == "MediaNextTrack"sv)
            key = Qt::Key_MediaNext;
        else if (part == "MediaPrevTrack"sv)
            key = Qt::Key_MediaPrevious;
        else if (part == "MediaPlayPause"sv)
            key = Qt::Key_MediaTogglePlayPause;
        else if (part == "MediaStop"sv)
            key = Qt::Key_MediaStop;
        else
            return {};
    }
    // Chrome requires Ctrl or Alt (or MacCtrl) with every key but the media keys.
    if (!key.has_value())
        return {};
    return QKeyCombination(modifiers, *key);
}

// Keys the browser itself uses, which no extension may take.
static bool is_browser_shortcut(QKeyCombination keys)
{
    // Those every Mac app has, whether or not a menu shows them.
    static constexpr QKeyCombination reserved[] = {
        QKeyCombination(Qt::ControlModifier, Qt::Key_Q),
        QKeyCombination(Qt::ControlModifier, Qt::Key_W),
        QKeyCombination(Qt::ControlModifier, Qt::Key_H),
        QKeyCombination(Qt::ControlModifier, Qt::Key_M),
        QKeyCombination(Qt::ControlModifier, Qt::Key_Comma),
        QKeyCombination(Qt::ControlModifier, Qt::Key_Period),
        QKeyCombination(Qt::ControlModifier, Qt::Key_Tab),
        QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Delete),
    };
    for (auto const& key : reserved) {
        if (key == keys)
            return true;
    }
    // And every menu item's.
    for (auto* widget : QApplication::topLevelWidgets()) {
        for (auto* action : widget->findChildren<QAction*>()) {
            for (auto const& shortcut : action->shortcuts()) {
                if (shortcut.count() == 1 && shortcut[0] == keys)
                    return true;
            }
        }
    }
    return false;
}

bool handle_extension_shortcut(QKeyEvent const& event, BrowserWindow* window)
{
    if (event.isAutoRepeat())
        return false;
    auto pressed = QKeyCombination(event.modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier | Qt::ShiftModifier), static_cast<Qt::Key>(event.key()));
    // Commands need a modifier, but media keys (ActionAPI::commands_for() has the rules).
    if (pressed.keyboardModifiers() == Qt::NoModifier && pressed.key() != Qt::Key_MediaNext && pressed.key() != Qt::Key_MediaPrevious
        && pressed.key() != Qt::Key_MediaTogglePlayPause && pressed.key() != Qt::Key_MediaStop)
        return false;
    if (is_browser_shortcut(pressed))
        return false;

    // When two extensions want the same keys, the one installed first has them, as in Chrome.
    Vector<WebView::Extension const*> extensions;
    registry().for_each_extension([&](WebView::Extension const& extension) { extensions.append(&extension); });
    auto installed_at = [](WebView::Extension const& extension) {
        auto installed = registry().installed_extension(extension.id());
        return installed.has_value() ? installed->install_time.seconds_since_epoch() : NumericLimits<i64>::max();
    };
    quick_sort(extensions, [&](auto const* a, auto const* b) {
        auto a_time = installed_at(*a);
        auto b_time = installed_at(*b);
        return a_time != b_time ? a_time < b_time : a->id() < b->id();
    });

    for (auto const* extension : extensions) {
        for (auto const& command : runtime().action().commands_for(extension->id())) {
            auto keys = parse_shortcut(command.shortcut);
            if (!keys.has_value() || *keys != pressed)
                continue;
            if (event.type() == QEvent::KeyPress) {
                runtime().action().command(extension->id(), command.name, current_tab_id(window),
                    window ? Optional<i64> { tabs_delegate().id_of(*window) } : Optional<i64> {});
            }
            return true;
        }
    }
    return false;
}

// The Extensions window: what's installed, and installing more.
class ExtensionsWindow final : public QWidget {
public:
    ExtensionsWindow()
        : QWidget(nullptr, Qt::Window)
    {
        setWindowTitle("Extensions");
        resize(560, 520);
        auto* layout = new QVBoxLayout(this);

        auto* install_row = new QHBoxLayout;
        m_store_input = new QLineEdit(this);
        m_store_input->setPlaceholderText("Chrome Web Store link or extension ID");
        auto* install_button = new QPushButton("Add", this);
        auto* unpacked_button = new QPushButton("Load Unpacked…", this);
        install_row->addWidget(m_store_input, 1);
        install_row->addWidget(install_button);
        install_row->addWidget(unpacked_button);
        layout->addLayout(install_row);

        m_status = new QLabel(this);
        m_status->setTextFormat(Qt::PlainText);
        m_status->setWordWrap(true);
        layout->addWidget(m_status);

        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        m_list = new QWidget(scroll);
        m_list_layout = new QVBoxLayout(m_list);
        m_list_layout->setAlignment(Qt::AlignTop);
        scroll->setWidget(m_list);
        layout->addWidget(scroll, 1);

        auto* updates = new QPushButton("Check for Updates", this);
        layout->addWidget(updates, 0, Qt::AlignRight);

        connect(install_button, &QPushButton::clicked, this, [this] { install_from_store(); });
        connect(m_store_input, &QLineEdit::returnPressed, this, [this] { install_from_store(); });
        connect(unpacked_button, &QPushButton::clicked, this, [this] {
            auto directory = QFileDialog::getExistingDirectory(this, "Choose an extension's folder");
            if (directory.isEmpty())
                return;
            auto result = registry().install_unpacked(ak_byte_string_from_qstring(directory));
            set_status(result.is_error() ? QString("Couldn't load the extension: %1").arg(qstring_from_ak_string(MUST(String::formatted("{}", result.error())))) : QString("Loaded."));
        });
        connect(updates, &QPushButton::clicked, this, [this] {
            set_status("Checking for updates…");
            registry().check_for_updates([self = QPointer(this)](Vector<WebView::ExtensionRegistry::UpdateResult> results) {
                if (!self)
                    return;
                QStringList lines;
                for (auto const& result : results) {
                    if (result.error.has_value())
                        lines.append(QString("%1: %2").arg(qstring_from_ak_string(result.id), qstring_from_ak_string(*result.error)));
                    else if (result.updated_to.has_value())
                        lines.append(QString("%1 updated to %2").arg(qstring_from_ak_string(result.id), qstring_from_ak_string(*result.updated_to)));
                }
                self->set_status(lines.isEmpty() ? QString("Everything is up to date.") : lines.join('\n'));
            });
        });
        rebuild();
    }

    void rebuild()
    {
        while (auto* item = m_list_layout->takeAt(0)) {
            if (auto* widget = item->widget())
                widget->deleteLater();
            delete item;
        }

        auto installed = registry().installed_extensions();
        if (installed.is_empty()) {
            auto* empty = new QLabel("No extensions yet. Paste a Chrome Web Store link above to add one.", m_list);
            empty->setWordWrap(true);
            m_list_layout->addWidget(empty);
        }
        for (auto const& entry : installed) {
            auto* row = new QWidget(m_list);
            auto* row_layout = new QHBoxLayout(row);
            auto* text = new QLabel(row);
            text->setTextFormat(Qt::PlainText);
            text->setText(QString("%1  %2%3").arg(qstring_from_ak_string(entry.name), qstring_from_ak_string(entry.version), entry.install_type == WebView::InstalledExtension::InstallType::Development ? QString("  (unpacked)") : QString()));
            text->setToolTip(qstring_from_ak_string(entry.id));
            auto* enabled = new QCheckBox("On", row);
            enabled->setChecked(entry.enabled);
            auto* file_access = new QCheckBox("Local files", row);
            file_access->setToolTip("Let the extension run in pages opened from files on this Mac");
            file_access->setChecked(entry.allows_file_access);
            auto* remove = new QPushButton("Remove", row);
            row_layout->addWidget(text, 1);
            row_layout->addWidget(enabled);
            row_layout->addWidget(file_access);
            row_layout->addWidget(remove);
            m_list_layout->addWidget(row);

            connect(enabled, &QCheckBox::toggled, this, [this, id = entry.id](bool on) {
                if (auto result = registry().set_enabled(id, on); result.is_error())
                    set_status(qstring_from_ak_string(MUST(String::formatted("{}", result.error()))));
            });
            connect(file_access, &QCheckBox::toggled, this, [id = entry.id](bool on) {
                registry().set_allows_file_access(id, on);
            });
            connect(remove, &QPushButton::clicked, this, [this, id = entry.id, name = entry.name] {
                auto answer = QMessageBox::question(this, "Remove Extension", QString("Remove %1?").arg(qstring_from_ak_string(name).toHtmlEscaped()));
                if (answer != QMessageBox::Yes)
                    return;
                if (auto result = registry().uninstall(id); result.is_error())
                    set_status(qstring_from_ak_string(MUST(String::formatted("{}", result.error()))));
            });
        }
    }

private:
    void set_status(QString const& text)
    {
        m_status->setText(text);
    }

    void install_from_store()
    {
        auto input = ak_string_from_qstring(m_store_input->text().trimmed());
        if (!WebView::ExtensionRegistry::extension_id_from_web_store_input(input).has_value()) {
            set_status("That isn't a Chrome Web Store link or an extension ID.");
            return;
        }
        set_status("Downloading…");
        registry().install_from_web_store(input, [self = QPointer(this)](ErrorOr<WebView::Extension const*> result) {
            if (!self)
                return;
            if (result.is_error()) {
                self->set_status(QString("Couldn't add the extension: %1").arg(qstring_from_ak_string(MUST(String::formatted("{}", result.error())))));
                return;
            }
            self->m_store_input->clear();
            self->set_status(result.value() ? QString("Added %1.").arg(qstring_from_ak_string(result.value()->manifest().name)) : QString("Added."));
        });
    }

    QLineEdit* m_store_input { nullptr };
    QLabel* m_status { nullptr };
    QWidget* m_list { nullptr };
    QVBoxLayout* m_list_layout { nullptr };
};

static QPointer<ExtensionsWindow>& extensions_window()
{
    static auto* window = new QPointer<ExtensionsWindow>;
    return *window;
}

void show_extensions_window()
{
    auto& window = extensions_window();
    if (!window)
        window = new ExtensionsWindow;
    window->show();
    window->raise();
    window->activateWindow();
}

void add_extensions_action_to(QMenu& menu)
{
    static QAction* action = [] {
        auto* action = new QAction("Extensions…", qApp);
        QObject::connect(action, &QAction::triggered, action, [] { show_extensions_window(); });
        return action;
    }();
    if (!menu.actions().contains(action))
        menu.addAction(action);
}

// What an API permission lets an extension do, as the permission prompt says it; nothing for what isn't one.
static Optional<QString> permission_description(StringView permission)
{
    static constexpr struct {
        StringView name;
        StringView description;
    } descriptions[] = {
        { "alarms"sv, "Run on a schedule"sv },
        { "bookmarks"sv, "Read and change your bookmarks"sv },
        { "clipboardRead"sv, "Read what you copy"sv },
        { "clipboardWrite"sv, "Change what you copy"sv },
        { "contextMenus"sv, "Add items to menus"sv },
        { "cookies"sv, "Read and change cookies of sites it can access"sv },
        { "declarativeNetRequest"sv, "Block and change requests sites make"sv },
        { "declarativeNetRequestWithHostAccess"sv, "Block and change requests to sites it can access"sv },
        { "downloads"sv, "Manage your downloads"sv },
        { "history"sv, "Read and change your browsing history"sv },
        { "idle"sv, "Know when you're away"sv },
        { "management"sv, "Manage your extensions"sv },
        { "nativeMessaging"sv, "Talk to apps on this Mac"sv },
        { "notifications"sv, "Show notifications"sv },
        { "privacy"sv, "Change your privacy settings"sv },
        { "scripting"sv, "Run scripts in pages it can access"sv },
        { "storage"sv, "Store data"sv },
        { "tabs"sv, "Read your tabs and the pages they show"sv },
        { "unlimitedStorage"sv, "Store unlimited data"sv },
        { "webNavigation"sv, "Know which pages you visit"sv },
        { "webRequest"sv, "See the requests sites make"sv },
    };
    for (auto const& entry : descriptions) {
        if (entry.name == permission)
            return QString::fromUtf8(entry.description.characters_without_null_termination(), static_cast<qsizetype>(entry.description.length()));
    }
    return {};
}

void install_extensions_ui()
{
    static ActionDelegate action_delegate;
    runtime().action().set_delegate(&action_delegate);
    runtime().set_tabs_delegate(&tabs_delegate());

    runtime().on_document_size = [](WebView::ExtensionRuntime::Context const& context, i32 width, i32 height) {
        if (!context.page)
            return;
        if (auto* popup = ExtensionPopup::showing(context.page->view()))
            popup->fit(width, height);
    };

    runtime().open_options_page = [](WebView::Extension const&, URL::URL const& url) {
        if (auto* window = Application::the().active_window_if_any())
            window->new_tab_from_url(url, Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
    };
    runtime().request_permissions_prompt = [](WebView::Extension const& extension, Vector<String> permissions, Vector<String> origins, Function<void(bool)> on_answer) {
        // One question at a time per extension: a request while one is open is denied.
        static auto* asking = new QSet<QString>;
        auto id = qstring_from_ak_string(extension.id());
        if (asking->contains(id))
            return on_answer(false);

        QStringList wanted;
        for (auto const& permission : permissions) {
            auto description = permission_description(permission);
            // What the browser can't describe, it won't grant.
            if (!description.has_value())
                return on_answer(false);
            wanted.append(*description);
        }
        for (auto const& origin : origins)
            wanted.append(origin == "<all_urls>"sv || origin == "*://*/*"sv ? QString("Read and change your data on all websites") : QString("Read and change your data on %1").arg(qstring_from_ak_string(origin).left(200)));

        auto name = qstring_from_ak_string(extension.manifest().name).simplified().left(80);
        auto* box = new QMessageBox(QMessageBox::Question, "Extension Permissions",
            QString("\u201C%1\u201D asks to:\n\n\u2022 %2").arg(name, wanted.join("\n\u2022 ")),
            QMessageBox::Yes | QMessageBox::No, Application::the().active_window_if_any());
        box->setTextFormat(Qt::PlainText);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->button(QMessageBox::Yes)->setText("Allow");
        box->button(QMessageBox::No)->setText("Deny");
        box->setDefaultButton(QMessageBox::No);
        asking->insert(id);
        auto* answer = new Function<void(bool)>(AK::move(on_answer));
        QObject::connect(box, &QMessageBox::finished, [id, answer](int result) {
            asking->remove(id);
            (*answer)(result == QMessageBox::Yes);
            delete answer;
        });
        box->open();
    };
    runtime().confirm_uninstall = [](WebView::Extension const& extension, Function<void(bool)> on_answer) {
        QMessageBox box(QMessageBox::Question, "Remove Extension", QString("Remove %1?").arg(qstring_from_ak_string(extension.manifest().name)), QMessageBox::Yes | QMessageBox::No);
        box.setTextFormat(Qt::PlainText);
        on_answer(box.exec() == QMessageBox::Yes);
    };

    // The registry tells the application of changes; the buttons and the Extensions window follow.
    registry().add_change_observer([] {
        for (auto const& row : all_button_rows()) {
            if (row)
                row->rebuild();
        }
        if (auto& window = extensions_window())
            window->rebuild();
    });

    // Extensions hear of tabs opening, closing, moving and changing: the runtime works out what changed.
    auto* timer = new QTimer(qApp);
    timer->setInterval(400);
    QObject::connect(timer, &QTimer::timeout, [] {
        bool any = false;
        registry().for_each_extension([&](auto const&) { any = true; });
        if (!any)
            return;
        runtime().tabs_changed();
        // A badge or title can be per tab, so the buttons follow the tab shown.
        for (auto const& row : all_button_rows()) {
            if (row)
                row->update_all();
        }
    });
    timer->start();
}

}
