/*
 * A square button that draws one SF Symbol (or a letter) exactly in its center, as in Arc's bottom bar.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QAbstractButton>

namespace Ladybird::Arc {

// Every symbol in the sidebar (bottom bar, space header) is fitted into a square of this size.
static constexpr int ICON_BOX = 15;

class SymbolButton final : public QAbstractButton {
    Q_OBJECT

public:
    explicit SymbolButton(QWidget* parent);

    // A space icon ("sf:<name>" or text), or a bare symbol name with set_symbol().
    void set_icon_text(QString const&);
    void set_symbol(QString const& name);
    void set_dimmed(bool);
    void set_symbol_point_size(int);

    virtual QSize sizeHint() const override { return { 30, 30 }; }

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void enterEvent(QEnterEvent*) override;
    virtual void leaveEvent(QEvent*) override;

private:
    QString m_symbol;
    QString m_text;
    bool m_dimmed { false };
    bool m_hovered { false };
    int m_point_size { ICON_BOX };
};

}
