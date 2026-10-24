/*
 * Arc's keyboard shortcuts.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/ArcShortcuts.h>

namespace Ladybird::Arc {

Optional<ArcShortcut> arc_shortcut_for(int key, Qt::KeyboardModifiers modifiers)
{
    // Arrow keys carry the keypad modifier on macOS.
    modifiers &= Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier;
    auto const command = Qt::KeyboardModifiers(Qt::ControlModifier);
    auto const command_shift = Qt::ControlModifier | Qt::ShiftModifier;
    auto const command_option = Qt::ControlModifier | Qt::AltModifier;
    auto const command_option_shift = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;
#if defined(AK_OS_MACOS)
    auto const control = Qt::KeyboardModifiers(Qt::MetaModifier);
#else
    auto const control = Qt::KeyboardModifiers(Qt::ControlModifier);
#endif

    if (modifiers == command) {
        switch (key) {
        case Qt::Key_S:
            return ArcShortcut { ArcCommand::ToggleSidebar };
        case Qt::Key_D:
            return ArcShortcut { ArcCommand::TogglePin };
        case Qt::Key_PageDown:
            return ArcShortcut { ArcCommand::NextTab };
        case Qt::Key_PageUp:
            return ArcShortcut { ArcCommand::PreviousTab };
        case Qt::Key_Backslash:
            return ArcShortcut { ArcCommand::FillFromOnePassword };
        default:
            break;
        }
        if (key >= Qt::Key_1 && key <= Qt::Key_8)
            return ArcShortcut { ArcCommand::SidebarItem, key - Qt::Key_1 };
        if (key == Qt::Key_9)
            return ArcShortcut { ArcCommand::SidebarItem, -1 };
    }

    if (modifiers == command_shift) {
        switch (key) {
        case Qt::Key_C:
            return ArcShortcut { ArcCommand::CopyUrl };
        case Qt::Key_K:
            return ArcShortcut { ArcCommand::ClearToday };
        // With Shift held, macOS reports [ and ] as { and }.
        case Qt::Key_BracketRight:
        case Qt::Key_BraceRight:
            return ArcShortcut { ArcCommand::NextTab };
        case Qt::Key_BracketLeft:
        case Qt::Key_BraceLeft:
            return ArcShortcut { ArcCommand::PreviousTab };
        default:
            break;
        }
    }

    if (modifiers == command_option) {
        switch (key) {
        case Qt::Key_V:
            return ArcShortcut { ArcCommand::NewTabFromClipboard };
        case Qt::Key_Down:
            return ArcShortcut { ArcCommand::NextTab };
        case Qt::Key_Up:
            return ArcShortcut { ArcCommand::PreviousTab };
        case Qt::Key_Right:
            return ArcShortcut { ArcCommand::NextSpace };
        case Qt::Key_Left:
            return ArcShortcut { ArcCommand::PreviousSpace };
        default:
            break;
        }
    }

    if (modifiers == command_option_shift && key == Qt::Key_C)
        return ArcShortcut { ArcCommand::CopyUrlAsMarkdown };

    if (key == Qt::Key_Tab && modifiers == control)
        return ArcShortcut { ArcCommand::NextTab };
    // Shift+Tab arrives as Backtab.
    if ((key == Qt::Key_Backtab || key == Qt::Key_Tab) && modifiers == (control | Qt::ShiftModifier))
        return ArcShortcut { ArcCommand::PreviousTab };

    return {};
}

QString url_as_markdown(QString title, QString const& url)
{
    if (title.trimmed().isEmpty())
        title = url;
    // Brackets in the title would end the link text early. Parentheses in the URL are fine in CommonMark as long as
    // they're balanced; encode the others.
    title.replace("\\", "\\\\").replace("[", "\\[").replace("]", "\\]");
    auto target = url;
    if (target.count('(') != target.count(')'))
        target.replace("(", "%28").replace(")", "%29");
    target.replace(" ", "%20");
    return QString("[%1](%2)").arg(title.simplified(), target);
}

}
