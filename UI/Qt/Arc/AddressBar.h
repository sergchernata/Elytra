/*
 * Arc-style address bar: the page's URL as quiet, centered text (host bright, the rest dimmed) after a link icon,
 * instead of an editable field. Hovering the URL or the link icon shows a pill behind it; clicking the URL opens the
 * command bar to edit it; clicking the link icon copies it.
 *
 * It takes the place of upstream's LocationEdit in each tab's toolbar (see ArcSidebar::apply_toolbar_inset()). The
 * LocationEdit stays, hidden, and keeps tracking the page's URL; this shows what it holds.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QString>
#include <QUrl>
#include <QWidget>

class QTimer;

namespace Ladybird {

class LocationEdit;

namespace Arc {

// How a URL reads in the bar: "youtube.com" and "/watch?v=…" for https://www.youtube.com/watch?v=…. Pages without a
// host (about:, file:, data:) are all `host`. Both empty for no URL.
struct DisplayUrl {
    QString host;
    QString rest;
};
DisplayUrl display_url_parts(QUrl const&);

class AddressBar final : public QWidget {
    Q_OBJECT

public:
    explicit AddressBar(QWidget* parent = nullptr);

    // Takes the place of `edit` in its layout, showing the URL it holds. Returns the one already there, if any.
    static AddressBar* replace(LocationEdit& edit);

    // Empty for none (the new tab page): nothing shows.
    void set_url(QUrl const&);
    QUrl url() const { return m_url; }
    DisplayUrl parts() const { return m_parts; }

    // Layout, in this widget's coordinates; shared with the tests. Empty when there's no URL.
    QRectF icon_rect() const;
    QRectF text_rect() const;   // The URL text.
    QRectF pill_rect() const;   // The hover pill behind it.
    QRectF icon_pill_rect() const; // The hover pill behind the link (copy) icon.
    QString shown_host() const; // As painted (elided when short of room).
    QString shown_rest() const;

    bool is_text_hovered() const { return m_hover == Hover::Text; }
    bool is_icon_hovered() const { return m_hover == Hover::Icon; }
    bool shows_copied() const { return m_copied; }

    static constexpr int HEIGHT = 30;
    static constexpr int ICON_SIZE = 15;
    static constexpr int ICON_GAP = 10;
    static constexpr int PILL_PADDING = 8;
    static constexpr int TEXT_PIXEL_SIZE = 13;

signals:
    void edit_requested(); // The URL was clicked.
    void url_copied();

protected:
    virtual QSize sizeHint() const override;
    virtual QSize minimumSizeHint() const override;
    virtual void paintEvent(QPaintEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void leaveEvent(QEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void changeEvent(QEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;

private:
    enum class Hover {
        None,
        Icon,
        Text,
    };

    void relayout();
    void set_hover(Hover);
    Hover hover_at(QPointF) const;
    QFont host_font() const;
    QFont rest_font() const;

    QUrl m_url;
    DisplayUrl m_parts;
    QString m_shown_host;
    QString m_shown_rest;
    QRectF m_icon_rect;
    QRectF m_text_rect;
    Hover m_hover { Hover::None };
    Hover m_pressed { Hover::None };
    bool m_copied { false };
    QTimer* m_copied_timer { nullptr };
};

}

}
