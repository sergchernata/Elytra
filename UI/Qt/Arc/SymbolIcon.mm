/*
 * Monochrome symbol icons for the Arc chrome, drawn from Apple's SF Symbols.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Arc/SymbolIcon.h>

#include <QHash>
#include <QImage>
#include <QPainter>

#include <cmath>

#import <AppKit/AppKit.h>

namespace Ladybird::Arc {

QPixmap symbol_pixmap(QString const& name, int point_size, QColor const& color, qreal device_pixel_ratio)
{
    auto cache_key = QString("%1|%2|%3|%4").arg(name).arg(point_size).arg(color.name(QColor::HexArgb)).arg(device_pixel_ratio);
    static QHash<QString, QPixmap> cache;
    if (auto it = cache.find(cache_key); it != cache.end())
        return *it;

    QPixmap result;
    @autoreleasepool {
        NSImage* image = [NSImage imageWithSystemSymbolName:name.toNSString() accessibilityDescription:nil];
        if (image) {
            auto* configuration = [NSImageSymbolConfiguration configurationWithPointSize:point_size weight:NSFontWeightMedium scale:NSImageSymbolScaleMedium];
            image = [image imageWithSymbolConfiguration:configuration];

            auto size = image.size;
            auto width = static_cast<int>(std::ceil(size.width * device_pixel_ratio));
            auto height = static_cast<int>(std::ceil(size.height * device_pixel_ratio));

            if (width > 0 && height > 0) {
                // Draw the (black, template) symbol straight into the QImage's memory, then recolor it.
                QImage bitmap(width, height, QImage::Format_ARGB32_Premultiplied);
                bitmap.fill(Qt::transparent);

                auto color_space = CGColorSpaceCreateDeviceRGB();
                auto context = CGBitmapContextCreate(bitmap.bits(), width, height, 8, bitmap.bytesPerLine(), color_space,
                    static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Little));
                if (context) {
                    auto* graphics_context = [NSGraphicsContext graphicsContextWithCGContext:context flipped:NO];
                    [NSGraphicsContext saveGraphicsState];
                    [NSGraphicsContext setCurrentContext:graphics_context];
                    [image drawInRect:NSMakeRect(0, 0, width, height) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];
                    [NSGraphicsContext restoreGraphicsState];
                    CGContextRelease(context);
                }
                CGColorSpaceRelease(color_space);

                QPainter painter(&bitmap);
                painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
                painter.fillRect(bitmap.rect(), color);
                painter.end();

                result = QPixmap::fromImage(bitmap);
                result.setDevicePixelRatio(device_pixel_ratio);
            }
        }
    }

    cache.insert(cache_key, result);
    return result;
}

QPixmap fitted_symbol_pixmap(QString const& name, int box, QColor const& color, qreal device_pixel_ratio)
{
    // Called while painting (rows, buttons, the command bar): keep the finished pixmaps, not just the renders.
    static QHash<QString, QPixmap> cache;
    auto key = QString("%1|%2|%3|%4").arg(name).arg(box).arg(color.rgba()).arg(device_pixel_ratio);
    if (auto it = cache.find(key); it != cache.end())
        return it.value();

    QPixmap result;
    if (name.startsWith("arc.")) {
        result = custom_glyph_pixmap(name, box, color, device_pixel_ratio);
    } else {
        // Render large, then scale down to the box, so every symbol ends up the same visual size.
        auto rendered = symbol_pixmap(name + ".fill", box * 2, color, device_pixel_ratio);
        if (rendered.isNull())
            rendered = symbol_pixmap(name, box * 2, color, device_pixel_ratio);
        if (!rendered.isNull()) {
            auto target = static_cast<int>(std::round(box * device_pixel_ratio));
            result = rendered.scaled(target, target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            result.setDevicePixelRatio(device_pixel_ratio);
        }
    }
    if (cache.size() > 512)
        cache.clear(); // Bounded: colors change with spaces and themes.
    cache.insert(key, result);
    return result;
}

QStringList const& space_symbol_names()
{
    static QStringList const names {
        "house", "briefcase", "star", "bolt", "terminal", "heart", "book", "graduationcap",
        "cart", "bag", "creditcard", "dollarsign.circle", "chart.bar", "chart.line.uptrend.xyaxis", "building.2", "car",
        "airplane", "globe", "map", "leaf", "flame", "drop", "moon", "sun.max",
        "cloud", "snowflake", "pawprint", "fork.knife", "cup.and.saucer", "gamecontroller", "music.note", "headphones",
        "camera", "photo", "paintbrush", "pencil", "hammer", "wrench.and.screwdriver", "cpu", "laptopcomputer",
        "chevron.left.forwardslash.chevron.right", "shippingbox", "gift", "lightbulb", "brain", "figure.run", "person", "person.2",
        "envelope", "bubble.left", "bell", "calendar", "doc.text", "folder", "bookmark", "tag",
        "flag", "pin", "sparkles", "atom", "puzzlepiece", "arc.skull",
    };
    return names;
}

}
