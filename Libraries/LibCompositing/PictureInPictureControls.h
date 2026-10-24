/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Optional.h>
#include <LibCompositing/OverlayGeometry.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

namespace Compositing {

enum class PictureInPictureControl : u8 {
    BackToTab,
    Close,
    // Fork hook (Arc): closes the window and leaves the video playing in its tab.
    Minimize,
};

inline Optional<PictureInPictureControl> picture_in_picture_control_from_underlying(u8 value)
{
    switch (value) {
    case to_underlying(PictureInPictureControl::BackToTab):
        return PictureInPictureControl::BackToTab;
    case to_underlying(PictureInPictureControl::Close):
        return PictureInPictureControl::Close;
    case to_underlying(PictureInPictureControl::Minimize):
        return PictureInPictureControl::Minimize;
    default:
        return {};
    }
}

// Fork hook (Arc): Arc's controls, a bar across the top: "Back to Tab" on the left, then the site's name, then
// "Minimize" and "Close" on the right. A window too narrow for their labels shows just their icons.
struct PictureInPictureControlsGeometry {
    Gfx::IntRect back_to_tab_button;
    Gfx::IntRect minimize_button;
    Gfx::IntRect close_button;
    bool has_labels { true };

    Array<OverlayButton<PictureInPictureControl>, 3> buttons() const
    {
        using Button = OverlayButton<PictureInPictureControl>;
        return {
            Button { .action = PictureInPictureControl::BackToTab, .bounds = back_to_tab_button },
            Button { .action = PictureInPictureControl::Minimize, .bounds = minimize_button },
            Button { .action = PictureInPictureControl::Close, .bounds = close_button },
        };
    }
};

// In CSS pixels.
static constexpr int picture_in_picture_controls_minimum_width_for_labels = 400;
static constexpr int picture_in_picture_control_height = 28;

inline PictureInPictureControlsGeometry picture_in_picture_controls_geometry(Gfx::IntSize viewport_size, double device_pixel_ratio)
{
    auto css_width = viewport_size.width() / max(device_pixel_ratio, 0.01);
    auto has_labels = css_width >= picture_in_picture_controls_minimum_width_for_labels;
    auto height = scaled_overlay_length(picture_in_picture_control_height, device_pixel_ratio);
    auto inset = scaled_overlay_length(has_labels ? 14 : 8, device_pixel_ratio);
    auto gap = scaled_overlay_length(8, device_pixel_ratio);
    auto width_of = [&](int labeled_width) { return has_labels ? scaled_overlay_length(labeled_width, device_pixel_ratio) : height; };

    auto close_width = width_of(76);
    auto minimize_width = width_of(98);
    Gfx::IntRect close_button { viewport_size.width() - inset - close_width, inset, close_width, height };
    Gfx::IntRect minimize_button { close_button.x() - gap - minimize_width, inset, minimize_width, height };
    return {
        .back_to_tab_button = { inset, inset, width_of(112), height },
        .minimize_button = minimize_button,
        .close_button = close_button,
        .has_labels = has_labels,
    };
}

}
