/*
 * Non-macOS stand-in for WindowControls.mm: window controls are drawn by Qt there, nothing to hide.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/WindowControls.h>

namespace Ladybird::Arc {

void set_window_controls_hidden(QWidget&, bool)
{
}

}
