// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/PeakEnvelopeExtractor.cpp — decoder-driven envelope extraction.

#include "media/PeakEnvelopeExtractor.hpp"

#include <optional>
#include <string>
#include <utility>

#include "core/Error.hpp"

namespace palmier::media {

Result<PeakEnvelope> extractPeakEnvelope(const std::filesystem::path& path,
                                         Duration bucketDuration, const DecodePrefs& prefs,
                                         const DecodeBackendFactory& factory,
                                         EnvelopeExtractionLimits limits) {
    if (bucketDuration.ticks() <= 0) {
        return err<PeakEnvelope>(makeError(
            ErrorCode::InvalidArgument, "a peak envelope needs a positive bucket duration"));
    }

    auto opened = MediaDecoder::open(path, prefs, factory);
    if (opened.isError()) return err<PeakEnvelope>(std::move(opened).error());
    MediaDecoder decoder = std::move(opened).value();

    // Requirement 2.6: no audio stream is a legitimate, cheap answer — an empty
    // envelope, reported as success — not a failure to put in front of the user.
    if (!decoder.hasAudio()) {
        return ok(PeakEnvelope{});
    }

    // A refused stream (an out-of-range sample rate or channel count) IS a
    // failure: the asset claims audio this build will not decode, and saying so
    // once is more useful than silently drawing nothing.
    if (auto selected = decoder.openAudioStream(); selected.isError()) {
        return err<PeakEnvelope>(std::move(selected).error());
    }

    // Prefer the container's declared sample rate, exactly as AudioEngine does; a
    // source that declares none reveals it in its first decoded block.
    int declaredRate = 0;
    for (const auto& stream : decoder.info().streams) {
        if (stream.isAudio() && stream.index == decoder.audioStreamIndex()) {
            declaredRate = stream.sampleRate;
            break;
        }
    }

    std::optional<PeakEnvelopeBuilder> builder;
    if (declaredRate > 0) {
        builder.emplace(bucketDuration, declaredRate);
    }

    std::size_t blocks = 0;
    while (true) {
        if (++blocks > limits.maxBlocks) {
            return err<PeakEnvelope>(
                makeError(ErrorCode::OutOfRange,
                          "audio envelope extraction exceeded its decode-call limit for '" +
                              path.string() + "'"));
        }

        auto next = decoder.nextAudioFrame();
        if (next.isError()) return err<PeakEnvelope>(std::move(next).error());
        AudioFrame frame = std::move(next).value();
        if (frame.endOfStream) break;

        if (!builder.has_value()) {
            // Undeclared parameters: adopt the first block that reveals a usable
            // rate. Blocks before that carry nothing measurable.
            if (frame.sampleRate() <= 0) continue;
            builder.emplace(bucketDuration, frame.sampleRate());
        }

        builder->add(frame.buffer);
        if (builder->framesConsumed() > limits.maxFrames) {
            return err<PeakEnvelope>(
                makeError(ErrorCode::OutOfRange,
                          "audio envelope extraction exceeded its frame limit for '" +
                              path.string() + "'"));
        }
    }

    // A stream that yielded nothing at all is silence, not a failure — the same
    // conclusion AudioEngine reaches for the same situation.
    if (!builder.has_value()) {
        return ok(PeakEnvelope{});
    }
    return ok(builder->finish());
}

} // namespace palmier::media
