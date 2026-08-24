// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/QtImageEncoder.hpp — the production services::ImageEncoder implementation
// (usable-editor tasks.md task 14; still-frame capture).
//
// services::captureFrame() is deliberately Qt-free (see StillFrameCapture.hpp's
// own doc comment), so the actual image encoding — Qt's QImage::save, the one
// still-image writer already in this tree — lives here instead, in the one
// module that does link Qt. The Composition_Root (src/app/main.cpp, inside its
// own PALMIER_HAVE_QT guard) binds services::ImageEncoder to an instance of this
// class's encode() method, exactly the way it binds gpu::TextRasterizer to
// QtTextRasterizer — both are the identical injectable-seam pattern.
//
// Format is chosen from the destination path's own extension via Qt's own
// format registry (QImageWriter::supportedImageFormats()); an unrecognized or
// absent extension defaults to PNG (kDefaultFormat), matching the "capture
// frame" feature's own minimal scope (task 14 asks for "an image file", not a
// format selector).

#ifndef PALMIER_UI_QTIMAGEENCODER_HPP
#define PALMIER_UI_QTIMAGEENCODER_HPP

#include <cstdint>
#include <filesystem>

#include "core/Result.hpp"
#include "services/StillFrameCapture.hpp"

namespace palmier::ui {

/// The production image encoder: writes RGBA8 pixels to `path` via
/// QImage::save. Thread-unsafe by the same convention QtTextRasterizer follows
/// (playback-thread affinity) — this class carries no mutable state at all, so
/// concurrent calls are safe as long as each targets a distinct path.
class QtImageEncoder {
public:
    /// The format written when `path`'s extension names none Qt recognizes.
    static constexpr const char* kDefaultFormat = "PNG";

    QtImageEncoder() = default;

    /// The services::ImageEncoder contract: encode `width` x `height` RGBA8
    /// pixels at `rgba` to `path`. Fails with InvalidArgument for a zero-sized
    /// image, and with Io if Qt's own save() reports failure (an unwritable
    /// destination, an unsupported format, etc.).
    [[nodiscard]] Result<void> encode(const std::uint8_t* rgba, std::uint32_t width,
                                      std::uint32_t height, const std::filesystem::path& path);

    /// A services::ImageEncoder bound to this instance, for
    /// services::captureFrame(). The encoder must outlive the binding.
    [[nodiscard]] services::ImageEncoder asEncoder();
};

} // namespace palmier::ui

#endif // PALMIER_UI_QTIMAGEENCODER_HPP
