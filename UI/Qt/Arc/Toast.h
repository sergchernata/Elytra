/*
 * Arc's toast: a small pill at the top right of the page confirming an action ("Copied URL"), which fades away.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

class QPropertyAnimation;
class QTimer;

namespace Ladybird {

class BrowserWindow;

namespace Arc {

class Toast final : public QWidget {
    Q_OBJECT

public:
    // Shows `text` (after an SF Symbol, if given) over `window`'s page, replacing any toast already there.
    static Toast* show_in(BrowserWindow& window, QString const& text, QString const& symbol = {});
    static Toast* current_in(BrowserWindow& window);

    QString text() const { return m_text; }

    static constexpr int VISIBLE_MS = 1600;
    static constexpr int FADE_MS = 180;
    static constexpr int MARGIN = 12; // From the page's top and right edges.

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual QSize sizeHint() const override;

private:
    Toast(BrowserWindow&, QString text, QString symbol);
    void place();
    void fade_out();

    QPointer<BrowserWindow> m_window;
    QString m_text;
    QString m_symbol;
    QTimer* m_timer { nullptr };
    QPropertyAnimation* m_fade { nullptr };
};

}

}
