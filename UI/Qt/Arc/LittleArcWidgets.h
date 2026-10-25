/*
 * Pieces of Little Arc's bar, as in Arc: the profile it browses in ("Default"), and the "Open in <space> ⌘O" button,
 * whose chevron chooses another space.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QRectF>
#include <QString>
#include <QWidget>

namespace Ladybird::Arc {

// How a profile is named in the bar: "Default" for the default profile (an empty name).
QString profile_label(QString const& profile);

// The profile, as a quiet pill: a face, then its name.
class ProfilePill final : public QWidget {
public:
    explicit ProfilePill(QWidget* parent = nullptr);

    void set_profile(QString const& profile);
    QString label() const { return m_label; }

    virtual QSize sizeHint() const override;
    virtual QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    virtual void paintEvent(QPaintEvent*) override;

private:
    QString m_label;
};

// "Open in **Hustle** ⌘O | ⌄", outlined: the main part opens the page in the space, the chevron offers the others.
class OpenInButton final : public QWidget {
    Q_OBJECT

public:
    explicit OpenInButton(QWidget* parent = nullptr);

    void set_space_title(QString const&);
    QString space_title() const { return m_space_title; }

    // In this widget's coordinates.
    QRectF main_rect() const;
    QRectF chevron_rect() const;

    virtual QSize sizeHint() const override;
    virtual QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void open_requested();
    void spaces_requested();

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void leaveEvent(QEvent*) override;

private:
    enum class Part {
        None,
        Main,
        Chevron,
    };
    Part part_at(QPointF) const;
    QFont space_font() const;
    qreal main_width() const;

    QString m_space_title;
    Part m_hover { Part::None };
    Part m_pressed { Part::None };
};

}
