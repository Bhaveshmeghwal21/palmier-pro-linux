// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/StillFrameCapture.cpp — see StillFrameCapture.hpp for the contract.

#include "services/StillFrameCapture.hpp"

namespace palmier::services {

Result<void> captureFrame(gpu::Compositor& compositor, const Project& project, Duration position,
                          const std::filesystem::path& path, const ImageEncoder& encoder) {
    const gpu::RenderTarget target =
        gpu::RenderTarget::forCanvas(project.canvas, gpu::RgbaColor::opaqueBlack());

    Result<gpu::RenderedFrame> rendered = compositor.renderAt(project, position, target);
    if (rendered.isError()) {
        return err(std::move(rendered).error());
    }

    const gpu::RenderedFrame& frame = rendered.value();
    const auto* pixels = static_cast<const std::uint8_t*>(frame.hostData());
    return encoder(pixels, frame.width(), frame.height(), path);
}

}  // namespace palmier::services
