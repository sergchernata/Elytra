/*
 * The SpaceStore for the current Ladybird profile. Kept apart from SpaceStore.cpp so the store itself has no
 * dependency on LibWebView and can be tested on its own.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/Profile.h>
#include <UI/Qt/Arc/SpaceStore.h>

namespace Ladybird::Arc {

SpaceStore& SpaceStore::the()
{
    static SpaceStore* store = [] {
        // One file per Ladybird profile, next to the profile's other data.
        auto const& data_directory = WebView::Application::profile().paths().data;
        auto directory = QString::fromUtf8(data_directory.characters(), static_cast<qsizetype>(data_directory.length()));
        return new SpaceStore(directory + "/ArcSpaces.json");
    }();
    return *store;
}

}
