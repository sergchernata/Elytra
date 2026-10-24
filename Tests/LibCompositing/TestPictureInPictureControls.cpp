/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/PictureInPictureControls.h>
#include <LibTest/TestCase.h>

using namespace Compositing;

// Fork (Arc): Arc's bar of labelled controls across the top.
TEST_CASE(buttons_sit_along_the_top)
{
    auto geometry = picture_in_picture_controls_geometry({ 800, 600 }, 1.0);

    EXPECT(geometry.has_labels);
    EXPECT_EQ(geometry.back_to_tab_button, (Gfx::IntRect { 14, 14, 112, 28 }));
    EXPECT_EQ(geometry.close_button, (Gfx::IntRect { 710, 14, 76, 28 }));
    EXPECT_EQ(geometry.minimize_button, (Gfx::IntRect { 604, 14, 98, 28 }));
}

TEST_CASE(geometry_scales_with_device_pixel_ratio)
{
    auto geometry = picture_in_picture_controls_geometry({ 1600, 1200 }, 2.0);

    EXPECT(geometry.has_labels);
    EXPECT_EQ(geometry.back_to_tab_button, (Gfx::IntRect { 28, 28, 224, 56 }));
    EXPECT_EQ(geometry.close_button, (Gfx::IntRect { 1420, 28, 152, 56 }));
}

TEST_CASE(a_narrow_window_shows_icons_only)
{
    auto geometry = picture_in_picture_controls_geometry({ 320, 180 }, 1.0);

    EXPECT(!geometry.has_labels);
    EXPECT_EQ(geometry.back_to_tab_button, (Gfx::IntRect { 8, 8, 28, 28 }));
    EXPECT_EQ(geometry.close_button, (Gfx::IntRect { 284, 8, 28, 28 }));
    EXPECT_EQ(geometry.minimize_button, (Gfx::IntRect { 248, 8, 28, 28 }));
}

TEST_CASE(controls_are_hit_inside_their_bounds)
{
    auto geometry = picture_in_picture_controls_geometry({ 800, 600 }, 1.0);
    auto buttons = geometry.buttons();

    EXPECT(overlay_action_at(buttons, geometry.back_to_tab_button.center()) == PictureInPictureControl::BackToTab);
    EXPECT(overlay_action_at(buttons, geometry.minimize_button.center()) == PictureInPictureControl::Minimize);
    EXPECT(overlay_action_at(buttons, geometry.close_button.center()) == PictureInPictureControl::Close);
    EXPECT(!overlay_action_at(buttons, { 790, 10 }).has_value());
    EXPECT(!overlay_action_at(buttons, { 400, 22 }).has_value());
    EXPECT(!overlay_action_at(buttons, { 706, 22 }).has_value());
}

TEST_CASE(control_values_are_validated)
{
    EXPECT(picture_in_picture_control_from_underlying(to_underlying(PictureInPictureControl::BackToTab)) == PictureInPictureControl::BackToTab);
    EXPECT(picture_in_picture_control_from_underlying(to_underlying(PictureInPictureControl::Close)) == PictureInPictureControl::Close);
    EXPECT(picture_in_picture_control_from_underlying(to_underlying(PictureInPictureControl::Minimize)) == PictureInPictureControl::Minimize);
    EXPECT(!picture_in_picture_control_from_underlying(255).has_value());
}
