// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ExportToolSurface.cpp - the agent-facing edge of exporting: parsing a
// tool call's arguments into a validated ExportRequest, rendering an outcome back
// as JSON, and the handler the tool registry publishes.
//
// Split out of services/ExportCoordinator.cpp; the shared contract is
// services/ExportCoordinatorInternal.hpp. Every definition was moved, not rewritten.

#include "services/ExportCoordinatorInternal.hpp"

namespace palmier::services {

using namespace palmier::services::detail;  // NOLINT(google-build-using-namespace)

namespace {

constexpr const char* kArgOutputPath = "outputPath";

constexpr const char* kArgFormat = "format";

constexpr const char* kArgWidth = "width";

constexpr const char* kArgHeight = "height";

constexpr const char* kArgCodec = "codec";

constexpr const char* kArgFps = "fps";

constexpr const char* kArgBitrateKbps = "bitrateKbps";

constexpr const char* kArgIncludeAudio = "includeAudio";

constexpr const char* kArgPreferHardware = "preferHardware";

constexpr const char* kArgOverwrite = "overwrite";

[[nodiscard]] std::optional<gpu::CodecId> parseExportCodec(const std::string& text) {
    const std::string codec = toLowerAscii(text);
    if (codec == "h264") return gpu::CodecId::H264;
    if (codec == "hevc") return gpu::CodecId::HEVC;
    if (codec == "vp9") return gpu::CodecId::VP9;
    return std::nullopt;
}

[[nodiscard]] FrameRate frameRateFromFps(double fps) {
    const double rounded = std::round(fps);
    if (std::abs(fps - rounded) < 1e-9 && rounded >= 1.0) {
        return FrameRate{static_cast<std::int64_t>(rounded), 1};
    }
    struct PullDown {
        double value;
        FrameRate rate;
    };
    const PullDown pullDowns[] = {{24000.0 / 1001.0, FrameRate::fps23_976()},
                                  {30000.0 / 1001.0, FrameRate::fps29_97()},
                                  {60000.0 / 1001.0, FrameRate::fps59_94()}};
    for (const PullDown& candidate : pullDowns) {
        if (std::abs(fps - candidate.value) < 0.0005) return candidate.rate;
    }
    return FrameRate{static_cast<std::int64_t>(std::llround(fps * 1000.0)), 1000};
}

[[nodiscard]] Result<std::int64_t> optionalInt(const Json& input, const char* name,
                                               std::int64_t fallback) {
    const Json* value = input.find(name);
    if (value == nullptr || value->isNull()) return Result<std::int64_t>(fallback);
    if (!value->isNumber()) {
        return err<std::int64_t>(
            invalidArgument(std::string("timeline.export: '") + name + "' must be a number"));
    }
    return Result<std::int64_t>(value->isInt() ? value->asInt()
                                               : static_cast<std::int64_t>(value->asDouble()));
}

[[nodiscard]] Result<bool> optionalBool(const Json& input, const char* name, bool fallback) {
    const Json* value = input.find(name);
    if (value == nullptr || value->isNull()) return Result<bool>(fallback);
    if (!value->isBool()) {
        return err<bool>(
            invalidArgument(std::string("timeline.export: '") + name + "' must be a boolean"));
    }
    return Result<bool>(value->asBool());
}

}  // namespace

Result<ExportRequest2> exportRequestFromToolArguments(const Json& input, const Project& project) {
    const Json* outputPath = input.find(kArgOutputPath);
    if (outputPath == nullptr || !outputPath->isString() || outputPath->asString().empty()) {
        return err<ExportRequest2>(
            invalidArgument("timeline.export: 'outputPath' is required and must be a non-empty "
                            "string"));
    }
    const Json* format = input.find(kArgFormat);
    if (format == nullptr || !format->isString() || format->asString().empty()) {
        return err<ExportRequest2>(
            invalidArgument("timeline.export: 'format' is required and must be a non-empty "
                            "string naming the output container (mp4, mov, mkv or webm)"));
    }

    ExportRequest2 request;
    request.outputPath = std::filesystem::path(outputPath->asString());
    request.container = toLowerAscii(format->asString());

    // The codec: the requested one, or the container's natural pairing. A webm
    // carries VP9; everything else defaults to H.264. An unsupported container is
    // NOT diagnosed here — validate() owns that message and names the whole
    // supported set (Requirement 7.9).
    if (const Json* codec = input.find(kArgCodec); codec != nullptr && !codec->isNull()) {
        if (!codec->isString()) {
            return err<ExportRequest2>(
                invalidArgument("timeline.export: 'codec' must be a string"));
        }
        const std::optional<gpu::CodecId> parsed = parseExportCodec(codec->asString());
        if (!parsed.has_value()) {
            return err<ExportRequest2>(
                unsupported("timeline.export: video codec \"" + codec->asString() +
                            "\" is not supported; the supported codecs are h264, hevc and vp9"));
        }
        request.codec = *parsed;
    } else {
        request.codec = request.container == "webm" ? gpu::CodecId::VP9 : gpu::CodecId::H264;
    }

    // Geometry and cadence default to the PROJECT's own canvas and timeline frame
    // rate, so an agent that names only a path and a container exports the project
    // as authored rather than at an invented size.
    Result<std::int64_t> width =
        optionalInt(input, kArgWidth, static_cast<std::int64_t>(project.canvas.width));
    if (width.isError()) return err<ExportRequest2>(std::move(width).error());
    Result<std::int64_t> height =
        optionalInt(input, kArgHeight, static_cast<std::int64_t>(project.canvas.height));
    if (height.isError()) return err<ExportRequest2>(std::move(height).error());

    // Clamp only the CAST, never the value: a negative or absurd number must reach
    // validate() so it is rejected by name with its accepted range, not folded into
    // a plausible-looking resolution by an unsigned conversion.
    const std::int64_t clampedWidth = std::clamp<std::int64_t>(width.value(), 0, 1'000'000);
    const std::int64_t clampedHeight = std::clamp<std::int64_t>(height.value(), 0, 1'000'000);
    request.resolution = Resolution{static_cast<std::uint32_t>(clampedWidth),
                                    static_cast<std::uint32_t>(clampedHeight)};

    if (const Json* fps = input.find(kArgFps); fps != nullptr && !fps->isNull()) {
        if (!fps->isNumber()) {
            return err<ExportRequest2>(invalidArgument("timeline.export: 'fps' must be a number"));
        }
        const double requested = fps->asDouble();
        // Out-of-range values are validate()'s to reject; only a rate the rational
        // conversion cannot express at all is refused here.
        if (!(requested > 0.0) || requested > 1'000'000.0) {
            return err<ExportRequest2>(outOfRange(
                "timeline.export: 'fps' must be a positive frame rate; the accepted range is " +
                std::to_string(static_cast<int>(kMinExportFps)) + " to " +
                std::to_string(static_cast<int>(kMaxExportFps)) + " frames per second"));
        }
        request.frameRate = frameRateFromFps(requested);
    } else {
        request.frameRate = project.timelineFps;
    }

    Result<std::int64_t> bitrate = optionalInt(input, kArgBitrateKbps, request.bitrateKbps);
    if (bitrate.isError()) return err<ExportRequest2>(std::move(bitrate).error());
    request.bitrateKbps = bitrate.value();

    Result<bool> includeAudio = optionalBool(input, kArgIncludeAudio, true);
    if (includeAudio.isError()) return err<ExportRequest2>(std::move(includeAudio).error());
    request.includeAudio = includeAudio.value();

    Result<bool> preferHardware = optionalBool(input, kArgPreferHardware, true);
    if (preferHardware.isError()) return err<ExportRequest2>(std::move(preferHardware).error());
    request.preferHardware = preferHardware.value();

    // Requirement 7.11: the acknowledgement is never implied. Absent the argument
    // an existing destination is preserved and the request rejected.
    Result<bool> overwrite = optionalBool(input, kArgOverwrite, false);
    if (overwrite.isError()) return err<ExportRequest2>(std::move(overwrite).error());
    request.overwrite = overwrite.value();

    return Result<ExportRequest2>(std::move(request));
}

Json exportOutcomeToJson(const ExportOutcome& outcome) {
    Json out = Json::object();
    out.set("outputPath", outcome.outputPath.string());
    out.set("framesEncoded", static_cast<std::int64_t>(outcome.framesEncoded));
    out.set("plannedFrames", static_cast<std::int64_t>(outcome.plannedFrames));
    out.set("encoderName", outcome.encoderName);
    // Requirement 7.2's boolean, plus the fallback flag and its reason: the two
    // cannot both be true (Requirement 8.8 makes that unconstructible), so a caller
    // reading both learns whether hardware ran, and if not, why.
    out.set("usedHardwareEncode", outcome.usedHardwareEncode);
    out.set("usedSoftwareFallback", outcome.usedSoftwareFallback);
    if (!outcome.fallbackReason.empty()) out.set("fallbackReason", outcome.fallbackReason);
    out.set("containsAudio", outcome.containsAudio);
    if (outcome.containsAudio) {
        out.set("audioFrames", static_cast<std::int64_t>(outcome.audioFrames));
    }
    out.set("durationNs", static_cast<std::int64_t>(outcome.duration.nanoseconds()));
    // Requirements 7.1/7.2: reported so a caller can ASSERT that the export left
    // the project alone rather than assume it. Always false — the worker runs on a
    // value-copy snapshot and holds no session reference.
    out.set("projectModified", outcome.projectModified);
    // Requirement 10.3: present iff a sidecar subtitle file was written
    // alongside the video output (i.e. the exported project had at least one
    // non-muted caption cue and the write succeeded); absent otherwise.
    if (!outcome.captionsSidecarPath.empty()) {
        out.set("captionsSidecarPath", outcome.captionsSidecarPath.string());
    }
    return out;
}

Tool::Handler makeExportToolHandler(ExportCoordinator& coordinator, ProjectSession& session,
                                    ExportToolOptions options) {
    return [&coordinator, &session, options](const Json& input) -> Result<Json> {
        Result<ExportRequest2> request =
            exportRequestFromToolArguments(input, session.engine().snapshot());
        if (request.isError()) return err<Json>(std::move(request).error());

        // The SAME entry point the export dialog uses (Requirement 7.2): admission,
        // the value-copy snapshot, the worker and the cleanup guard all belong to the
        // coordinator. A rejection here — an invalid parameter, an empty timeline, an
        // unacknowledged existing destination, an export already running — is returned
        // verbatim, having created no file.
        const ExportRequest2 parsed = std::move(request).value();
        if (Result<void> started = coordinator.begin(parsed); started.isError()) {
            return err<Json>(std::move(started).error());
        }

        // Bounded wait, pumped on this thread: the progress reports and the outcome
        // are delivered here, never from the worker.
        if (coordinator.awaitCompletion(options.budget) == 0) {
            // The export did not finish inside the budget. Cancelling stops it at its
            // next frame boundary and the coordinator's guard removes the partial
            // output, so the timeout leaves no file either.
            coordinator.cancel();
            (void)coordinator.awaitCompletion(options.budget);
            return err<Json>(
                makeError(ErrorCode::Timeout,
                          "timeline.export did not complete within " +
                              std::to_string(options.budget.count()) +
                              " ms; the export was cancelled and the partial output removed"));
        }

        if (const std::optional<Error>& failure = coordinator.lastError(); failure.has_value()) {
            return err<Json>(*failure);
        }
        const std::optional<ExportOutcome>& outcome = coordinator.lastOutcome();
        if (!outcome.has_value()) {
            return err<Json>(makeError(ErrorCode::Internal,
                                       "timeline.export: the export outcome was never delivered"));
        }
        if (outcome->cancelled) {
            return err<Json>(makeError(ErrorCode::Cancelled,
                                       "timeline.export was cancelled; no file remains at " +
                                           outcome->outputPath.string()));
        }
        return Result<Json>(exportOutcomeToJson(*outcome));
    };
}

}  // namespace palmier::services
