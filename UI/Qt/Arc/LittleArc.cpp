/*
 * Little Arc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Arc/ArcSidebar.h>
#include <UI/Qt/Arc/AddressBar.h>
#include <UI/Qt/Arc/CommandBar.h>
#include <LibWebView/Menu.h>
#include <UI/Qt/Arc/LittleArc.h>
#include <UI/Qt/Arc/ProfileSessions.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Arc/SpaceStore.h>
#include <UI/Qt/Arc/SymbolButton.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/WebContentView.h>

#include <QApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QUrl>

namespace Ladybird::Arc {

static constexpr char const* LITTLE_ARC_PROPERTY = "arc_little_arc";
static constexpr int WIDTH = 880;
static constexpr int HEIGHT = 640;
static constexpr int BAR_HEIGHT = 40;
static constexpr int WINDOW_CONTROLS_WIDTH = 78; // macOS's buttons sit over the bar's left end.

#if !defined(AK_OS_MACOS)
void register_little_arc_hot_key()
{
}
#endif

bool is_little_arc(BrowserWindow const& window)
{
    return window.property(LITTLE_ARC_PROPERTY).toBool();
}

// The browser window a Little Arc's page moves to: the one used last.
static BrowserWindow* browser_window()
{
    BrowserWindow* found = nullptr;
    if (auto* active = Application::the().active_window_if_any(); active && ArcSidebar::for_window(*active))
        return active;
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* window = qobject_cast<BrowserWindow*>(widget);
        if (window && !is_little_arc(*window) && ArcSidebar::for_window(*window) && window->is_private() == WebView::IsPrivate::No) {
            found = window;
            if (window->isVisible())
                break;
        }
    }
    return found;
}

static QString space_title(QString const& id)
{
    auto const* space = SpaceStore::the().space(id);
    return space ? space->title : QString();
}

static QString most_recent_space()
{
    auto& store = SpaceStore::the();
    return store.space(store.last_active_space_id()) ? store.last_active_space_id() : store.first_space_id();
}

class LittleArcBar final : public QWidget {
public:
    // A popup's bar is the same, without "Open in": a popup belongs to the page that opened it.
    enum class Kind : u8 {
        LittleArc,
        Popup,
    };

    explicit LittleArcBar(BrowserWindow& window, Kind kind = Kind::LittleArc)
        : QWidget(&window)
        , m_window(&window)
        , m_kind(kind)
    {
        setObjectName("ArcLittleArcBar");
        setFixedHeight(BAR_HEIGHT);

        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(WINDOW_CONTROLS_WIDTH, 0, 8, 0);
        layout->setSpacing(6);

        m_back = new SymbolButton(this);
        m_back->set_symbol("chevron.left");
        m_back->setToolTip("Back");
        connect(m_back, &QAbstractButton::clicked, this, [this] {
            if (auto* tab = current_tab())
                tab->view().traverse_the_history_by_delta(-1);
        });
        layout->addWidget(m_back);

        // The page's address, as in the browser window's toolbar: click it to edit it in the command bar.
        m_address = new AddressBar(this);
        m_address->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        connect(m_address, &AddressBar::edit_requested, this, [this] {
            if (m_window)
                CommandBar::toggle_for(*m_window, CommandBar::Mode::EditCurrentTab, current_tab());
        });
        layout->addWidget(m_address, 1, Qt::AlignVCenter);

        m_open_in = new QPushButton(this);
        m_open_in->setObjectName("ArcLittleArcOpenIn");
        m_open_in->setCursor(Qt::PointingHandCursor);
        m_open_in->setFocusPolicy(Qt::NoFocus);
        m_open_in->setToolTip("Open in the browser window (⌘O)");
        connect(m_open_in, &QPushButton::clicked, this, [this] {
            if (m_window)
                move_little_arc_to_space(*m_window);
        });
        layout->addWidget(m_open_in);

        m_choose = new SymbolButton(this);
        m_choose->set_symbol("chevron.down");
        m_choose->set_symbol_point_size(11);
        m_choose->setToolTip("Open in Another Space (⌥⌘O)");
        connect(m_choose, &QAbstractButton::clicked, this, [this] {
            if (m_window)
                choose_space_for_little_arc(*m_window);
        });
        layout->addWidget(m_choose);

        if (kind == Kind::Popup) {
            m_open_in->hide();
            m_choose->hide();
        }

        restyle();
        update_contents();
    }

    void watch(Tab& tab)
    {
        connect(&tab, &Tab::title_changed, this, [this] { update_contents(); });
        connect(&tab, &Tab::loading_changed, this, [this] { update_contents(); });
    }

    QPoint menu_position() const { return m_open_in->mapToGlobal(QPoint(0, m_open_in->height() + 4)); }

    void update_contents()
    {
        auto* tab = current_tab();
        auto url = tab ? QUrl(qstring_from_ak_string(tab->view().url().serialize())) : QUrl();
        if (url.toString() == "about:blank")
            url = QUrl();
        m_address->set_url(url);
        m_back->setEnabled(tab && tab->view().navigate_back_action().enabled());

        if (m_kind == Kind::Popup)
            return;
        auto label = QString("Open in %1").arg(space_title(most_recent_space()));
        if (m_open_in->text() != label)
            m_open_in->setText(label);
    }

protected:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), ChromeStyle::chrome_background(palette()));
    }

    virtual void changeEvent(QEvent* event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::PaletteChange)
            restyle();
    }

private:
    Tab* current_tab() const { return m_window ? m_window->current_tab() : nullptr; }

    void restyle()
    {
        auto dark = ChromeStyle::is_dark(palette());
        auto text = ChromeStyle::chrome_text(palette());
        auto button_style = QString("QPushButton#ArcLittleArcOpenIn { color: %1; background: %2; border: none; border-radius: 7px;"
                                    " padding: 4px 10px; font-size: 12px; font-weight: 500; }"
                                    "QPushButton#ArcLittleArcOpenIn:hover { background: %3; }")
                                .arg(text.name(), dark ? "rgba(255,255,255,0.10)" : "rgba(0,0,0,0.06)", dark ? "rgba(255,255,255,0.16)" : "rgba(0,0,0,0.10)");
        if (m_open_in->styleSheet() != button_style)
            m_open_in->setStyleSheet(button_style);
    }

    QPointer<BrowserWindow> m_window;
    Kind m_kind { Kind::LittleArc };
    SymbolButton* m_back { nullptr };
    AddressBar* m_address { nullptr };
    QPushButton* m_open_in { nullptr };
    SymbolButton* m_choose { nullptr };
};

static LittleArcBar* bar_of(BrowserWindow& window)
{
    return static_cast<LittleArcBar*>(window.findChild<QWidget*>("ArcLittleArcBar"));
}

BrowserWindow& open_little_arc(Optional<URL::URL> const& url)
{
    auto& application = Application::the();
    auto* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    auto available = screen->availableGeometry();
    WindowConfiguration configuration;
    configuration.width = WIDTH;
    configuration.height = HEIGHT;
    configuration.x = available.center().x() - WIDTH / 2;
    configuration.y = available.top() + qMax(0, (available.height() - HEIGHT) / 3);

    // In the most recent space's profile, as Arc does.
    set_profile_for_new_views(ArcSidebar::profile_of_space(most_recent_space()));
    auto& window = application.new_window({ url.value_or(URL::about_blank()) }, configuration, BrowserWindow::IsPopupWindow::Yes,
        WebView::IsPrivate::No, nullptr, {}, ShowWindow::No);
    window.setProperty(LITTLE_ARC_PROPERTY, true);

    auto* bar = new LittleArcBar(window);
    if (auto* tabs = window.findChild<TabWidget*>())
        tabs->set_minimal_chrome_bar(bar);
    if (auto* tab = window.current_tab()) {
        bar->watch(*tab);
        bar->update_contents();
    }

    window.show();
    window.activateWindow();
    window.raise();

    // Nothing to show yet: ask what to look up.
    if (!url.has_value()) {
        QPointer<BrowserWindow> guarded = &window;
        QTimer::singleShot(0, &window, [guarded] {
            if (guarded)
                CommandBar::toggle_for(*guarded, CommandBar::Mode::EditCurrentTab, guarded->current_tab());
        });
    }
    return window;
}

void give_popup_slim_chrome(BrowserWindow& popup)
{
    if (is_little_arc(popup) || popup.findChild<QWidget*>("ArcLittleArcBar"))
        return;
    auto* tabs = popup.findChild<TabWidget*>();
    if (!tabs)
        return;
    auto* bar = new LittleArcBar(popup, LittleArcBar::Kind::Popup);
    tabs->set_minimal_chrome_bar(bar);
    QPointer<LittleArcBar> guarded_bar = bar;
    auto watch_current = [guarded_bar, window = QPointer<BrowserWindow>(&popup)] {
        if (!guarded_bar || !window)
            return;
        if (auto* tab = window->current_tab(); tab && !tab->property("arc_popup_bar_watched").toBool()) {
            tab->setProperty("arc_popup_bar_watched", true);
            guarded_bar->watch(*tab);
        }
        guarded_bar->update_contents();
    };
    QObject::connect(tabs, &TabWidget::tabs_changed, bar, watch_current);
    watch_current();
}

bool move_little_arc_to_space(BrowserWindow& little_arc, QString space_id)
{
    auto* target = browser_window();
    auto* sidebar = target ? ArcSidebar::for_window(*target) : nullptr;
    auto* tab = little_arc.current_tab();
    if (!sidebar || !tab)
        return false;
    if (space_id.isEmpty() || !SpaceStore::the().space(space_id))
        space_id = most_recent_space();
    sidebar->adopt_tab(little_arc, *tab, space_id);
    return true;
}

void choose_space_for_little_arc(BrowserWindow& little_arc)
{
    auto* menu = new QMenu(&little_arc);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addSection("Open in Space");
    QPointer<BrowserWindow> guarded = &little_arc;
    for (auto const& space : SpaceStore::the().spaces()) {
        auto id = space.id;
        menu->addAction(space.title, menu, [guarded, id] {
            if (guarded)
                move_little_arc_to_space(*guarded, id);
        });
    }
    menu->setActiveAction(menu->actions().value(1));
    auto* bar = bar_of(little_arc);
    menu->popup(bar ? bar->menu_position() : little_arc.mapToGlobal(little_arc.rect().center()));
}

}
