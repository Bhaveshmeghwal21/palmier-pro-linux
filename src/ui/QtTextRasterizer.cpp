// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/QtTextRasterizer.cpp — implementation (usable-editor task 12; Requirement 9).
// See QtTextRasterizer.hpp for the contract.

#include "ui/QtTextRasterizer.hpp"

#include <utility>

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QString>
#include <QTextOption>

#include "core/Error.hpp"

namespace palmier::ui {

namespace {

/// Qt's own Format_RGBA8888 byte layout matches gpu::SourceFrame's documented
/// "row-major, tightly packed, width*height*4 bytes" RGBA8 exactly, so the
/// pixel data copies straight across with no channel reordering.
constexpr QImage::Format kQtFormat = QImage::Format_RGBA8888;

/// Qt::Alignment flags for TextStyle::alignment, combined with vertically
/// centering the text block on its own bounding rect — TextStyle carries only
/// a single anchor position, not separate horizontal/vertical anchors, so the
/// natural reading is "the anchor is the centre of the text block" regardless
/// of which way it is aligned horizontally within that block.
[[nodiscard]] Qt::Alignment qtAlignment(TextAlignment alignment) noexcept {
    switch (alignment) {
        case TextAlignment::Left:   return Qt::AlignLeft | Qt::AlignVCenter;
        case TextAlignment::Right:  return Qt::AlignRight | Qt::AlignVCenter;
        case TextAlignment::Center: return Qt::AlignHCenter | Qt::AlignVCenter;
    }
    return Qt::AlignHCenter | Qt::AlignVCenter;
}

} // namespace

Result<gpu::SourceFrame> QtTextRasterizer::rasterize(const TextStyle& style,
                                                      std::uint32_t width,
                                                      std::uint32_t height) {
    if (width == 0 || height == 0) {
        return err<gpu::SourceFrame>(
            invalidArgument("QtTextRasterizer::rasterize: target has zero dimensions"));
    }

    // Requirement 9.6: substitute and report rather than fail when the
    // requested family is not one QFontDatabase::families() names on this
    // host. families() is queried fresh on every call rather than cached: the
    // installed font set is not this class's to assume is static, and a
    // substitution is a rare, cheap-enough path to not warrant caching it.
    const QString requested = QString::fromStdString(style.fontFamily);
    const bool available = QFontDatabase::families().contains(requested);
    const QString effectiveFamily =
        available ? requested : QString::fromLatin1(kDefaultFontFamily);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!available) {
            lastSubstitution_ =
                FontSubstitution{style.fontFamily, effectiveFamily.toStdString()};
        } else {
            lastSubstitution_.reset();
        }
    }

    QImage image(static_cast<int>(width), static_cast<int>(height), kQtFormat);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font(effectiveFamily);
    font.setPointSizeF(style.pointSize);
    painter.setFont(font);

    QColor color;
    color.setRedF(static_cast<float>(style.colorR));
    color.setGreenF(static_cast<float>(style.colorG));
    color.setBlueF(static_cast<float>(style.colorB));
    color.setAlphaF(static_cast<float>(style.colorA));
    painter.setPen(color);

    // TextStyle::x/y anchor the CENTRE of the text block (see qtAlignment's own
    // comment): the block is measured against the font metrics at an
    // unconstrained width, then centred on the normalized anchor position
    // scaled to the target canvas, and finally clamped so it never draws
    // entirely outside the visible frame even for an anchor at the very edge.
    const QFontMetricsF metrics(font);
    const QString text = QString::fromStdString(style.content);
    const QRectF unconstrained = metrics.boundingRect(
        QRectF(0, 0, static_cast<qreal>(width) * 4.0, metrics.height() * 2.0),
        static_cast<int>(qtAlignment(style.alignment)) | Qt::TextWordWrap, text);

    const qreal anchorX = style.x * static_cast<qreal>(width);
    const qreal anchorY = style.y * static_cast<qreal>(height);
    QRectF block(anchorX - unconstrained.width() / 2.0, anchorY - unconstrained.height() / 2.0,
                unconstrained.width(), unconstrained.height());

    painter.drawText(block, static_cast<int>(qtAlignment(style.alignment)) | Qt::TextWordWrap,
                     text);
    painter.end();

    gpu::SourceFrame frame;
    frame.width = width;
    frame.height = height;
    frame.rgba.assign(image.constBits(), image.constBits() + image.sizeInBytes());
    return frame;
}

gpu::TextRasterizer QtTextRasterizer::asRasterizer() {
    return [this](const TextStyle& style, std::uint32_t width, std::uint32_t height) {
        return rasterize(style, width, height);
    };
}

std::optional<FontSubstitution> QtTextRasterizer::lastSubstitution() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastSubstitution_;
}

} // namespace palmier::ui
