/*
 * Arc-style command bar: a floating search/address panel opened with Cmd+T (or a double-click on the sidebar).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibURL/URL.h>
#include <LibWebView/Omnibox.h>
#include <QIcon>
#include <QList>
#include <QPointer>
#include <QWidget>

class QLineEdit;

namespace Ladybird {

class BrowserWindow;
class Tab;

namespace Arc {

// One row in the command bar: an open tab to switch to, or one of upstream's suggestions (history, bookmarks,
// search). Plain data so the layout can be tested.
struct CommandBarRow {
    enum class Kind {
        SwitchToTab,
        Suggestion,
    };
    Kind kind { Kind::Suggestion };
    QString title;
    QString subtitle;
    QIcon icon;
    QPointer<Tab> tab;           // SwitchToTab rows.
    size_t suggestion_index { 0 }; // Suggestion rows: index into the omnibox's suggestions.
};

// A frameless window centered over its browser window: one card holding the search field and the results,
// as in Arc. Upstream's Omnibox model drives suggestions, inline completion and URL-or-search resolution;
// committing opens the result in a new tab, and open tabs can be switched to.
class CommandBar final : public QWidget {
    Q_OBJECT

public:
    static void toggle_for(BrowserWindow&);

    explicit CommandBar(BrowserWindow&);
    virtual ~CommandBar() override;

    void present();
    void dismiss();

    // Layout, shared with the tests.
    static constexpr int HEADER_HEIGHT = 62;
    static constexpr int ROW_HEIGHT = 50;
    static constexpr int LIST_PADDING = 8;
    static constexpr int MAX_TAB_ROWS_EMPTY = 6;
    static constexpr int MAX_TAB_ROWS_MATCHING = 3;
    static int card_height_for_rows(int rows);

    // Which open tabs to offer: all of them (minus the current and blank ones) for an empty query, otherwise
    // those whose title or URL contains every word of it.
    static bool tab_matches(QString const& query, QString const& title, QString const& url);

protected:
    virtual bool eventFilter(QObject*, QEvent*) override;
    virtual void paintEvent(QPaintEvent*) override;
    virtual void keyPressEvent(QKeyEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;

private:
    void open_in_new_tab(String input, Optional<URL::URL>, WebView::OmniboxDestinationKind);
    void commit(String const& input);
    void rebuild_rows();
    void activate_row(int row);
    void select_row(int row);
    void reposition();
    bool owns_widget(QWidget*) const;
    bool handle_edit_key(QKeyEvent&);
    QRectF card_rect() const;
    QRectF row_rect(int row) const;
    int row_at(QPointF) const;

    QPointer<BrowserWindow> m_window;
    QLineEdit* m_edit { nullptr };
    WebView::Omnibox m_omnibox;
    QList<CommandBarRow> m_rows;
    int m_selected_row { -1 };
    bool m_selection_is_explicit { false };
};

}

}
