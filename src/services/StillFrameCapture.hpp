// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/StillFrameCapture.hpp — still-frame capture (usable-editor tasks.md
// task 14; the `capture frame` tool category `docs/UPSTREAM_PARITY.md` records
// as absent).
//
// Writes the presented frame at a given timeline position to an image file.
// There is no dedicated numbered Requirement for this feature (tasks.md's own
// task 14 carries no `(Requirement N)` annotation, unlike tasks 8-13); its
// acceptance criteria are exactly task 14's own two subtasks: an operation that
// writes the frame at the playhead to an image file, and a test proving the
// written image matches the preview frame within the existing GPU/CPU parity
// tolerance (tests/gpu/invert_colors_property_test.cpp's own `kPathTolerance`).
//
// Deliberately Qt-free, mirroring core/TextStyle.hpp + gpu::TextRasterizer's own
// seam split from Task 12: this module's ImageEncoder is an injectable function
// type (Result<void>(RGBA8 pixels, width, height, path)), with the concrete
// PNG-writing implementation living in ui::QtImageEncoder (needs QImage), kept
// out of this Qt-free translation unit. `captureFrame()` itself is the pure
// orchestration: render through the supplied Compositor at the supplied
// position, then hand the resulting pixels to the supplied encoder — no Qt
// symbol appears anywhere in this file or its .cpp.

#ifndef PALMIER_SERVICES_STILL_FRAME_CAPTURE_HPP
#define PALMIER_SERVICES_STILL_FRAME_CAPTURE_HPP

#include <cstdint>
#include <filesystem>
#include <functional>

#include "core/Duration.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "gpu/Compositor.hpp"

namespace palmier::services {

/// Encodes RGBA8, row-major, tightly-packed pixels of the given dimensions to
/// an image file at `path`. The production implementation
/// (`ui::QtImageEncoder`) writes PNG via `QImage::save`; a test injects a
/// synthetic encoder that simply records what it was asked to write, so this
/// module is exercisable with no Qt anywhere in the call chain.
using ImageEncoder = std::function<Result<void>(const std::uint8_t* rgba, std::uint32_t width,
                                                std::uint32_t height,
                                                const std::filesystem::path& path)>;

/// Render `project` at `position` through `compositor` (the SAME live
/// Compositor instance the preview surface renders through — this is what
/// makes the written image match the preview frame rather than merely
/// resemble it) and hand the resulting pixels to `encoder`.
///
/// Fails with:
///   * whatever `compositor.renderAt()` fails with (InvalidArgument, Unsupported,
///     FailedPrecondition — e.g. no frame provider or text rasterizer installed
///     for a visible clip);
///   * whatever `encoder` fails with (e.g. an unwritable destination).
[[nodiscard]] Result<void> captureFrame(gpu::Compositor& compositor, const Project& project,
                                        Duration position, const std::filesystem::path& path,
                                        const ImageEncoder& encoder);

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_STILL_FRAME_CAPTURE_HPP
