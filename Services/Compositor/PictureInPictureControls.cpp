/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Span.h>
#include <Compositor/FontSkia.h>
#include <Compositor/PictureInPictureControls.h>
#include <LibCompositing/UIOverlays.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaUtils.h>
#include <core/SkCanvas.h>
#include <core/SkFont.h>
#include <core/SkFontMetrics.h>
#include <core/SkBlurTypes.h>
#include <core/SkMaskFilter.h>
#include <core/SkPaint.h>
#include <core/SkPathBuilder.h>
#include <core/SkRRect.h>
#include <effects/SkImageFilters.h>

namespace Compositor {

// Fork hook (Arc): Arc's controls. Each is a rounded, frosted button (the video blurred behind a light tint) with an
// icon and, when the window is wide enough, a label; the site's name sits between them.

// The icons are stroked on a grid of 24 units.
static void add_back_to_tab_icon(SkPathBuilder& path)
{
    path.moveTo(17, 17);
    path.lineTo(7, 7);
    path.moveTo(7, 15);
    path.lineTo(7, 7);
    path.lineTo(15, 7);
}

static void add_minimize_icon(SkPathBuilder& path)
{
    path.moveTo(6, 12);
    path.lineTo(18, 12);
}

static void add_close_icon(SkPathBuilder& path)
{
    path.moveTo(7, 7);
    path.lineTo(17, 17);
    path.moveTo(17, 7);
    path.lineTo(7, 17);
}

static StringView label_for(Compositing::PictureInPictureControl control)
{
    switch (control) {
    case Compositing::PictureInPictureControl::BackToTab:
        return "Back to Tab"sv;
    case Compositing::PictureInPictureControl::Minimize:
        return "Minimize"sv;
    case Compositing::PictureInPictureControl::Close:
        return "Close"sv;
    }
    VERIFY_NOT_REACHED();
}

static Optional<SkFont> label_font(Optional<String> const& font_family, double device_pixel_ratio)
{
    // On macOS the interface font's family is a private name (".AppleSystemUIFont") that the font database can't look
    // up, so a family that looks alike follows it. The font is looked up once, not every frame of a fade.
    static Optional<String> cached_family;
    static RefPtr<Gfx::Font> cached_font;
    static bool looked_up = false;
    if (!looked_up || cached_family != font_family) {
        looked_up = true;
        cached_family = font_family;
        cached_font = nullptr;
        if (font_family.has_value())
            cached_font = Gfx::FontDatabase::the().get(FlyString { *font_family }, 10, 600, Gfx::FontWidth::Normal, 0);
        for (auto family : { "Helvetica Neue"sv, "Inter"sv, "Arial"sv, "SerenitySans"sv }) {
            if (cached_font)
                break;
            cached_font = Gfx::FontDatabase::the().get(MUST(FlyString::from_utf8(family)), 10, 600, Gfx::FontWidth::Normal, 0);
        }
    }
    if (!cached_font)
        return {};
    return skia_font(*cached_font, static_cast<float>(device_pixel_ratio));
}

void paint_picture_in_picture_controls(Gfx::PaintingSurface& surface, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::PictureInPictureControlsState const& state, float opacity)
{
    static constexpr auto button_tint = Color(0xff, 0xff, 0xff, 46);
    static constexpr auto hovered_button_tint = Color(0xff, 0xff, 0xff, 74);
    static constexpr auto foreground_color = Color(0xff, 0xff, 0xff);

    auto& canvas = surface.canvas();
    auto geometry = Compositing::picture_in_picture_controls_geometry(viewport_size, device_pixel_ratio);
    auto dpr = static_cast<float>(device_pixel_ratio);
    auto radius = 8 * dpr;
    auto font = label_font(state.font_family, device_pixel_ratio);

    SkFontMetrics metrics {};
    if (font.has_value())
        font->getMetrics(&metrics);
    auto baseline_for = [&](Gfx::IntRect const& rect) {
        return rect.center().y() - (metrics.fAscent + metrics.fDescent) / 2;
    };

    auto paint_button = [&](Gfx::IntRect const& button, Compositing::PictureInPictureControl control, void (*add_icon)(SkPathBuilder&)) {
        auto rrect = SkRRect::MakeRectXY(Gfx::to_skia_rect(button), radius, radius);
        auto bounds = Gfx::to_skia_rect(button);

        canvas.save();
        canvas.clipRRect(rrect, true);
        // The layer starts as the video behind it, blurred, and fades in and out as a whole.
        SkPaint layer_paint;
        layer_paint.setAlphaf(opacity);
        auto blur = SkImageFilters::Blur(14 * dpr, 14 * dpr, SkTileMode::kClamp, nullptr);
        canvas.saveLayer(SkCanvas::SaveLayerRec(&bounds, &layer_paint, blur.get(), 0));

        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(Gfx::to_skia_color(state.hovered_control == control ? hovered_button_tint : button_tint));
        canvas.drawRRect(rrect, paint);

        auto icon_size = 16 * dpr;
        auto label = label_for(control);
        float label_width = 0;
        if (geometry.has_labels && font.has_value() && control != Compositing::PictureInPictureControl::BackToTab)
            label_width = font->measureText(label.characters_without_null_termination(), label.length(), SkTextEncoding::kUTF8);
        auto gap = label_width > 0 ? 7 * dpr : 0;
        auto content_width = icon_size + gap + label_width;
        auto icon_left = button.x() + (button.width() - content_width) / 2;
        auto icon_top = button.center().y() - icon_size / 2;

        SkPathBuilder icon;
        add_icon(icon);
        canvas.save();
        canvas.translate(icon_left, icon_top);
        canvas.scale(icon_size / 24, icon_size / 24);
        SkPaint stroke;
        stroke.setAntiAlias(true);
        stroke.setStyle(SkPaint::kStroke_Style);
        stroke.setStrokeCap(SkPaint::kRound_Cap);
        stroke.setStrokeJoin(SkPaint::kRound_Join);
        stroke.setStrokeWidth(2.2f);
        stroke.setColor(Gfx::to_skia_color(foreground_color));
        canvas.drawPath(icon.detach(), stroke);
        canvas.restore();

        if (label_width > 0) {
            paint.setColor(Gfx::to_skia_color(foreground_color));
            canvas.drawSimpleText(label.characters_without_null_termination(), label.length(), SkTextEncoding::kUTF8,
                icon_left + icon_size + gap, baseline_for(button), *font, paint);
        }

        canvas.restore();
        canvas.restore();
    };

    paint_button(geometry.back_to_tab_button, Compositing::PictureInPictureControl::BackToTab, add_back_to_tab_icon);
    paint_button(geometry.minimize_button, Compositing::PictureInPictureControl::Minimize, add_minimize_icon);
    paint_button(geometry.close_button, Compositing::PictureInPictureControl::Close, add_close_icon);

    // The site's name, centered in the window if it fits between the buttons.
    if (font.has_value() && !state.caption.is_empty()) {
        auto caption = state.caption.bytes_as_string_view();
        auto width = font->measureText(caption.characters_without_null_termination(), caption.length(), SkTextEncoding::kUTF8);
        auto x = (viewport_size.width() - width) / 2;
        auto margin = 10 * dpr;
        if (x >= geometry.back_to_tab_button.right() + margin && x + width <= geometry.minimize_button.left() - margin) {
            auto y = baseline_for(geometry.back_to_tab_button);
            SkPaint shadow;
            shadow.setAntiAlias(true);
            shadow.setColor(SkColorSetARGB(140, 0, 0, 0));
            shadow.setAlphaf(shadow.getAlphaf() * opacity);
            shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 3 * dpr));
            canvas.drawSimpleText(caption.characters_without_null_termination(), caption.length(), SkTextEncoding::kUTF8, x, y + dpr, *font, shadow);
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(Gfx::to_skia_color(foreground_color));
            paint.setAlphaf(opacity);
            canvas.drawSimpleText(caption.characters_without_null_termination(), caption.length(), SkTextEncoding::kUTF8, x, y, *font, paint);
        }
    }
}

}
