/*
 * Arc-style command bar: a floating search/address panel opened with Cmd+T.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibURL/URL.h>
#include <LibWebView/Omnibox.h>
#include <QPointer>
#include <QWidget>

namespace Ladybird {

class BrowserWindow;
class LocationEdit;

namespace Arc {

// A frameless top-level window centered over its browser window. It wraps the
// regular LocationEdit, so suggestions, search and URL handling stay upstream's.
// Committing opens the result in a new tab.
class CommandBar final : public QWidget {
    Q_OBJECT

public:
    static void toggle_for(BrowserWindow&);

    explicit CommandBar(BrowserWindow&);
    virtual ~CommandBar() override;

    void present();
    void dismiss();

protected:
    virtual bool eventFilter(QObject*, QEvent*) override;
    virtual void paintEvent(QPaintEvent*) override;
    virtual void keyPressEvent(QKeyEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;

private:
    void open_in_new_tab(String input, Optional<URL::URL>, WebView::OmniboxDestinationKind);
    void reposition();
    bool owns_widget(QWidget*) const;
    QRectF card_rect() const;

    QPointer<BrowserWindow> m_window;
    LocationEdit* m_edit { nullptr };
};

}

}
