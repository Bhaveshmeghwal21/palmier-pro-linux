// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/QtTextRasterizer.hpp — the production gpu::TextRasterizer implementation
// (usable-editor task 12; Requirement 9).
//
// gpu::Compositor is deliberately Qt-free (see Compositor.hpp's TextRasterizer
// doc comment and gpu/CMakeLists.txt, which links no Qt target at all), so the
// actual glyph shaping and rasterization — Qt's QPainter / QFontMetrics, the one
// text-rendering technology already in this tree — lives here instead, in the
// one module that does link Qt. The Composition_Root (src/app/main.cpp, inside
// its own PALMIER_HAVE_QT guard) installs QtTextRasterizer::rasterize bound to
// an instance of this class on the shared Compositor via setTextRasterizer(),
// exactly the way it installs the decoder-backed ClipFrameProvider — both are
// the identical injectable-seam pattern, just for a different kind of layer.
//
// Font substitution (Requirement 9.6): a requested family QFontDatabase does not
// carry on the host is substituted with a documented default
// (QtTextRasterizer::kDefaultFontFamily, "sans-serif" — the same default
// TextStyle::fontFamily itself already carries, see core/TextStyle.hpp) rather
// than failing the render, and the substitution is recorded so a caller can
// report it; lastSubstitution() exposes the most recent one for that purpose.

#ifndef PALMIER_UI_QTTEXTRASTERIZER_HPP
#define PALMIER_UI_QTTEXTRASTERIZER_HPP

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "core/Result.hpp"
#include "core/TextStyle.hpp"
#include "gpu/Compositor.hpp"

namespace palmier::ui {

/// One recorded font substitution: the family that was requested and the
/// family that was actually used to rasterize it (Requirement 9.6).
struct FontSubstitution {
    std::string requested;
    std::string substituted;
};

/// The production text rasterizer: renders a TextStyle into an RGBA8 buffer of
/// exactly the requested dimensions via QPainter, honouring the style's font
/// family (substituting and recording a documented default when the family is
/// unavailable on the host), point size, colour, horizontal alignment and
/// normalized anchor position. Thread-unsafe by the same convention
/// gpu::Compositor itself follows (playback-thread affinity; see
/// DecoderClipFrameProvider.hpp's own note) — the mutex here only protects the
/// lastSubstitution_ observability field from a benign cross-thread read, not
/// concurrent rasterize() calls.
class QtTextRasterizer {
public:
    /// The documented default family a substitution falls back to. Matches
    /// TextStyle::fontFamily's own default, so a caller who never set a family
    /// at all never triggers a reported substitution for it.
    static constexpr const char* kDefaultFontFamily = "sans-serif";

    QtTextRasterizer() = default;

    /// The gpu::TextRasterizer contract: rasterize `style` into a `width` x
    /// `height` RGBA8 buffer. Fails with InvalidArgument for a zero-sized
    /// target; never fails merely because the requested font family is
    /// unavailable — that case substitutes and is reported through
    /// lastSubstitution() instead (Requirement 9.6).
    [[nodiscard]] Result<gpu::SourceFrame> rasterize(const TextStyle& style,
                                                      std::uint32_t width,
                                                      std::uint32_t height);

    /// A gpu::TextRasterizer bound to this instance, for
    /// gpu::Compositor::setTextRasterizer. The rasterizer must outlive the
    /// binding.
    [[nodiscard]] gpu::TextRasterizer asRasterizer();

    /// The most recent font substitution this instance performed, or
    /// std::nullopt if every rasterize() call so far found its requested
    /// family available (or already asked for the default).
    [[nodiscard]] std::optional<FontSubstitution> lastSubstitution() const;

private:
    mutable std::mutex                  mutex_;
    std::optional<FontSubstitution>     lastSubstitution_;
};

} // namespace palmier::ui

#endif // PALMIER_UI_QTTEXTRASTERIZER_HPP
