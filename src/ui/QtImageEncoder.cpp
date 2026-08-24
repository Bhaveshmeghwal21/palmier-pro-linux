// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/QtImageEncoder.cpp — implementation (usable-editor tasks.md task 14).
// See QtImageEncoder.hpp for the contract.

#include "ui/QtImageEncoder.hpp"

#include <QByteArray>
#include <QImage>
#include <QImageWriter>
#include <QString>

#include "core/Error.hpp"

namespace palmier::ui {

namespace {

/// Qt's own Format_RGBA8888 byte layout matches gpu::RenderedFrame's documented
/// "row-major, tightly packed, width*height*4 bytes" RGBA8 exactly (the same
/// fact QtTextRasterizer's own doc comment relies on), so the pixel data reads
/// straight in with no channel reordering.
constexpr QImage::Format kQtFormat = QImage::Format_RGBA8888;

/// The format QImage::save() should write: the destination path's own
/// extension if Qt's writer registry recognizes it, else the documented
/// default (PNG).
[[nodiscard]] QByteArray formatFor(const std::filesystem::path& path) {
    const QString suffix = QString::fromStdString(path.extension().string());
    if (suffix.size() > 1) {
        // Drop the leading '.'; QImageWriter's own format names are bare
        // (e.g. "png", not ".png").
        const QByteArray candidate = suffix.mid(1).toUtf8().toUpper();
        if (QImageWriter::supportedImageFormats().contains(candidate)) {
            return candidate;
        }
    }
    return QByteArray(QtImageEncoder::kDefaultFormat);
}

} // namespace

Result<void> QtImageEncoder::encode(const std::uint8_t* rgba, std::uint32_t width,
                                    std::uint32_t height, const std::filesystem::path& path) {
    if (width == 0 || height == 0) {
        return err(invalidArgument("QtImageEncoder::encode: image has zero dimensions"));
    }

    // QImage's own constructor over raw data does not copy by default; this
    // one does not need to survive the QImage, so a deep copy is unnecessary
    // as long as the QImage is used (here, saved) before `rgba` might be
    // freed — which it always is, within this one function call.
    const QImage image(rgba, static_cast<int>(width), static_cast<int>(height),
                       static_cast<int>(width) * 4, kQtFormat);

    const QString qtPath = QString::fromStdString(path.string());
    if (!image.save(qtPath, formatFor(path).constData())) {
        return err(makeError(ErrorCode::Io,
                             "QtImageEncoder::encode: failed to write '" + path.string() + "'"));
    }
    return ok();
}

services::ImageEncoder QtImageEncoder::asEncoder() {
    return [this](const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                 const std::filesystem::path& path) -> Result<void> {
        return encode(rgba, width, height, path);
    };
}

} // namespace palmier::ui
