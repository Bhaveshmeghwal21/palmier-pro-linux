// SPDX-License-Identifier: GPL-3.0-or-later
//
// The export_coordinator_property_test cases that stayed in this file after it was split.
//
// Suites here: ExportCoordinatorProperties.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "export_coordinator_property_test_support.hpp"

namespace palmier::services {

using namespace palmier::services::
    testsupport_exportcoordinatorpropertytest;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// Property 33
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 33: Export runs exactly the requested
// parameters and never touches the project — for any valid export request and any forced outcome
// (success, mid-export failure, cancellation), the export engine receives a specification whose
// output path, container, codec, resolution, frame rate, bit rate and audio inclusion equal the
// request field for field; the project snapshot before and after the export compares byte-equal; and
// the session reports the project unmodified; and the request issued through the export dialog and
// through `timeline.export` produce equal outcome fields.
//
// **Validates: Requirements 7.1, 7.2**
RC_GTEST_PROP(ExportCoordinatorProperties,
              ExportRunsExactlyTheRequestedParametersAndNeverTouchesTheProject,
              ()) {
    const Resolution   resolution = kGeometries[*rc::gen::inRange<int>(0, kGeometryCount)];
    const FrameRate    fps = kFrameRates[*rc::gen::inRange<int>(0, kFrameRateCount)];
    const std::string  container = kContainers[*rc::gen::inRange<int>(0, kContainerCount)];
    const gpu::CodecId codec = kCodecs[*rc::gen::inRange<int>(0, kCodecCount)];
    const auto         bitrateKbps =
        *rc::gen::inRange<std::int64_t>(kMinExportBitrateKbps, kMaxExportBitrateKbps + 1);
    const bool includeAudio = *rc::gen::arbitrary<bool>();
    const bool preferHardware = *rc::gen::arbitrary<bool>();
    /// The three outcomes the design's generator forces.
    const int  forcedOutcome = *rc::gen::inRange<int>(0, 3); // 0 success, 1 failure, 2 cancel
    const bool editDuringExport = *rc::gen::arbitrary<bool>();
    const int  nameIndex = *rc::gen::inRange<int>(0, 1'000);

    // A forced failure or cancellation needs a frame to happen after, so those
    // cases export at least two frames.
    const int drawnFrames = *rc::gen::inRange<int>(1, maxFramesFor(resolution) + 1);
    const int frames = std::max(forcedOutcome == 0 ? 1 : 2, drawnFrames);

    CaseDir                     dir("p33");
    const std::filesystem::path out =
        dir.file("export_" + std::to_string(nameIndex) + "." + toLowerAscii(container));

    ManualClock  clock;
    ExportScript script;
    script.outputPath = out;
    script.clock = &clock;
    if (forcedOutcome == 1) script.failVideoOnFrame = 1; // fails after one frame.

    // Gates shared with the worker. Declared before the Harness so they outlive the
    // coordinator no matter how this body exits.
    std::promise<void>       parked;
    std::shared_future<void> hasParked(parked.get_future());
    std::promise<void>       release;
    std::shared_future<void> released(release.get_future());
    std::atomic<bool>        parkedOnce{false};

    Harness harness;
    RC_ASSERT(harness.install(makeTimeline(frames, fps, resolution, includeAudio)));

    const Project     snapshotBefore = harness.session.engine().snapshot();
    const std::size_t planned = media::ExportEngine::plannedFrameCount(snapshotBefore, fps);
    RC_ASSERT(planned >= 1);
    if (forcedOutcome != 0) RC_ASSERT(planned >= 2);

    ExportCoordinatorOptions options = hostlessOptions(&script, &clock, resolution);
    if (preferHardware) {
        // The hardware lane, reachable on a host with neither a GPU nor a vendor
        // SDK because capabilities and compiled-in paths are values.
        options.caps = hardwareCapsFor(codec);
        options.availability = gpu::BridgeAvailability::all();
    }
    harness.make(std::move(options));

    // Releases the worker however this body exits, so a failed assertion cannot
    // leave the export parked on a gate nobody will open.
    ReleaseOnExit releaseGuard(release);

    ExportCoordinator* coordinator = harness.coordinator.get();
    {
        std::lock_guard<std::mutex> lock(script.mutex);
        script.encodeHook = [&parked, &released, &parkedOnce, coordinator, editDuringExport,
                             forcedOutcome](int index) {
            if (editDuringExport && index == 0 && !parkedOnce.exchange(true)) {
                parked.set_value();
                released.wait();
            }
            if (forcedOutcome == 2 && index == 0) coordinator->cancel();
        };
    }

    const ExportRequest2 request = makeRequest(out, container, codec, resolution, fps, bitrateKbps,
                                               includeAudio, preferHardware);
    RC_ASSERT(ExportCoordinator::validate(request).isOk());
    RC_ASSERT(harness.coordinator->begin(request).isOk());

    // The reference the project is compared against is taken AFTER the mid-export
    // edit when there is one, so what is asserted is that the EXPORT changed
    // nothing — not that nothing at all changed.
    std::size_t plannedAfterEdit = planned;
    if (editDuringExport) {
        RC_ASSERT(hasParked.wait_for(kWaitBudget) == std::future_status::ready);
        // The worker is parked inside the first frame: the file it created really is
        // on disk right now, which is what makes the cleanup assertions of
        // Properties 36 and 37 non-vacuous.
        RC_ASSERT(pathExists(out));
        // A real edit to the LIVE session while the export runs.
        Project longer = harness.session.engine().snapshot();
        longer.tracks = makeTimeline(frames * 2, fps, resolution, includeAudio).tracks;
        RC_ASSERT(harness.session.engine().reset(longer).isOk());
        plannedAfterEdit = media::ExportEngine::plannedFrameCount(longer, fps);
        // The edit must really change the planned frame count, or the isolation
        // check below would have no teeth.
        RC_ASSERT(plannedAfterEdit > planned);
    }
    const std::string   expectedSerialized = harness.serialized();
    const std::uint64_t expectedRevision = harness.session.revision();
    const bool          expectedModified = harness.session.modified();
    releaseGuard.release();

    RC_ASSERT(harness.coordinator->awaitCompletion(kWaitBudget) > 0);

    // --- The engine received exactly the requested parameters (Req 7.1, 7.2) ---
    RC_ASSERT(script.factoryCalls.load() >= 1);
    media::EncodeSpec spec;
    {
        std::lock_guard<std::mutex> lock(script.mutex);
        spec = script.spec;
    }
    RC_ASSERT(spec.outputPath == out);
    RC_ASSERT(spec.containerFormat == toLowerAscii(container));
    RC_ASSERT(spec.codec == codec);
    RC_ASSERT(spec.resolution == resolution);
    RC_ASSERT(spec.frameRate == fps);
    RC_ASSERT(spec.bitrateBitsPerSecond == bitrateKbps * 1'000);
    RC_ASSERT(spec.audio.has_value() == includeAudio);

    // --- The project is untouched, whatever the outcome (Requirement 7.1) ------
    RC_ASSERT(harness.serialized() == expectedSerialized);
    RC_ASSERT(harness.session.revision() == expectedRevision);
    RC_ASSERT(harness.session.modified() == expectedModified);

    const std::optional<ExportOutcome> outcome = harness.coordinator->lastOutcome();
    RC_ASSERT(outcome.has_value());
    RC_ASSERT(!outcome->projectModified);
    RC_ASSERT(outcome->outputPath == out);
    // The planner ran on the SNAPSHOT: a timeline edited mid-export cannot change
    // the size of the export in flight.
    RC_ASSERT(outcome->plannedFrames == planned);
    if (editDuringExport) RC_ASSERT(outcome->plannedFrames < plannedAfterEdit);
    // Requirement 8.8: hardware use and software fallback are never both reported.
    RC_ASSERT(!(outcome->usedHardwareEncode && outcome->usedSoftwareFallback));

    // Anti-vacuity for the two cleanup cases: a file really was created, and real
    // bytes really were written, at exactly this path.
    RC_ASSERT(script.created.load());
    RC_ASSERT(script.existedAfterOpen.load());
    RC_ASSERT(script.bytesWritten.load() > 0);

    switch (forcedOutcome) {
        case 0:
            RC_ASSERT(!harness.coordinator->lastError().has_value());
            RC_ASSERT(!outcome->cancelled);
            RC_ASSERT(outcome->framesEncoded == planned);
            RC_ASSERT(outcome->containsAudio == includeAudio);
            RC_ASSERT(pathExists(out));
            break;
        case 1:
            RC_ASSERT(harness.coordinator->lastError().has_value());
            RC_ASSERT(!pathExists(out));
            break;
        default:
            RC_ASSERT(outcome->cancelled);
            RC_ASSERT(!pathExists(out));
            break;
    }

    // --- The outcome is a function of the request and the snapshot -------------
    // Both the export dialog and `timeline.export` enter through begin(); re-issuing
    // the identical request reproduces every outcome field but the path, which is
    // the fact that makes the two callers agree (Requirement 7.2).
    if (forcedOutcome == 0 && !editDuringExport) {
        const ExportOutcome          first = *outcome;
        const std::filesystem::path second =
            dir.file("export_" + std::to_string(nameIndex) + "_again." + toLowerAscii(container));
        {
            std::lock_guard<std::mutex> lock(script.mutex);
            script.outputPath = second;
            script.encodeHook = {};
        }
        ExportRequest2 again = request;
        again.outputPath = second;
        RC_ASSERT(harness.coordinator->begin(again).isOk());
        RC_ASSERT(harness.coordinator->awaitCompletion(kWaitBudget) > 0);
        const std::optional<ExportOutcome> repeat = harness.coordinator->lastOutcome();
        RC_ASSERT(repeat.has_value());
        RC_ASSERT(repeat->framesEncoded == first.framesEncoded);
        RC_ASSERT(repeat->plannedFrames == first.plannedFrames);
        RC_ASSERT(repeat->encoderName == first.encoderName);
        RC_ASSERT(repeat->usedHardwareEncode == first.usedHardwareEncode);
        RC_ASSERT(repeat->usedSoftwareFallback == first.usedSoftwareFallback);
        RC_ASSERT(repeat->containsAudio == first.containsAudio);
        RC_ASSERT(repeat->audioFrames == first.audioFrames);
        RC_ASSERT(repeat->duration == first.duration);
        RC_ASSERT(repeat->cancelled == first.cancelled);
        RC_ASSERT(!repeat->projectModified);
        RC_ASSERT(harness.serialized() == expectedSerialized);
    }
}
// ===========================================================================
// Property 36
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 36: Any failure after encoding begins leaves no
// file and no project change — for any failure injected after at least one frame has been encoded —
// a compositor error, a video encode error, an audio encode or mux error, or a hardware encode
// failure — the export stops, no file remains at the requested path, the project is reported
// unmodified and is byte-identical, and the error names the failing stage (identifying a mid-export
// hardware encode failure where that is the injected cause).
//
// **Validates: Requirements 6.10, 7.5, 8.11**
RC_GTEST_PROP(ExportCoordinatorProperties,
              AnyFailureAfterEncodingBeginsLeavesNoFileAndNoProjectChange,
              ()) {
    // The failure stages of the design's generator.
    enum Stage {
        kCompositor = 0,
        kVideoEncode = 1,
        kAudioEncode = 2,
        kFinalize = 3,
        kHardware = 4,
    };
    const int stage = *rc::gen::inRange<int>(0, 5);

    const Resolution resolution = kGeometries[*rc::gen::inRange<int>(0, kGeometryCount)];
    const FrameRate  fps = kFrameRates[*rc::gen::inRange<int>(0, kFrameRateCount)];
    // At least two frames, so a failure can land after at least one encoded frame.
    const int frames = std::max(2, *rc::gen::inRange<int>(2, maxFramesFor(resolution) + 2));
    // The failing index, always >= 1: Requirement 8.11's "after at least one frame
    // has been encoded".
    const int drawnFailAt = *rc::gen::inRange<int>(1, frames);
    // The hardware lane routes H.264 and HEVC to a vendor encoder.
    const gpu::CodecId codec = stage == kHardware ? kCodecs[*rc::gen::inRange<int>(0, 2)]
                                                 : kCodecs[*rc::gen::inRange<int>(0, kCodecCount)];
    const bool includeAudio = stage == kAudioEncode ? true : *rc::gen::arbitrary<bool>();

    CaseDir                     dir("p36");
    const std::filesystem::path out = dir.file("failure.mp4");

    ManualClock      clock;
    ExportScript     script;
    std::atomic<int> providerCalls{0};
    script.outputPath = out;
    script.clock = &clock;

    Harness harness;
    RC_ASSERT(harness.install(makeTimeline(frames, fps, resolution, includeAudio)));
    const std::string   serializedBefore = harness.serialized();
    const std::uint64_t revisionBefore = harness.session.revision();
    const bool          modifiedBefore = harness.session.modified();
    const std::size_t   planned =
        media::ExportEngine::plannedFrameCount(harness.session.engine().snapshot(), fps);
    RC_ASSERT(planned >= 2);
    const int failAt = std::min(drawnFailAt, static_cast<int>(planned) - 1);
    RC_ASSERT(failAt >= 1);

    switch (stage) {
        case kVideoEncode:
        case kHardware:
            script.failVideoOnFrame = failAt;
            break;
        case kAudioEncode:
            script.failAudioOnBlock = failAt;
            break;
        case kFinalize:
            script.failFinish = true;
            break;
        default:
            break; // the compositor failure is injected through the frame provider.
    }

    ExportCoordinatorOptions options = hostlessOptions(&script, &clock, resolution);
    if (stage == kCompositor) {
        // A compositor error: the source pixels for the failing frame cannot be
        // produced, exactly as a decode failure of a named asset presents.
        options.frameProvider = [resolution, failAt, &providerCalls](
                                    const Clip&, Duration) -> Result<gpu::SourceFrame> {
            const int index = providerCalls.fetch_add(1);
            if (index == failAt) {
                return err<gpu::SourceFrame>(
                    makeError(ErrorCode::Io, "decoding the source asset failed: mock-asset.mov"));
            }
            return gpu::SourceFrame::solid(resolution.width, resolution.height,
                                           gpu::RgbaColor{31, 63, 95, 255});
        };
    }
    if (stage == kHardware) {
        options.caps = hardwareCapsFor(codec);
        options.availability = gpu::BridgeAvailability::all();
    }
    harness.make(std::move(options));

    RC_ASSERT(harness.coordinator
                  ->begin(makeRequest(out, "mp4", codec, resolution, fps, 6'000, includeAudio,
                                      /*preferHardware=*/stage == kHardware))
                  .isOk());
    RC_ASSERT(harness.coordinator->awaitCompletion(kWaitBudget) > 0);

    // --- The export stopped and named the failing stage -----------------------
    const std::optional<Error> error = harness.coordinator->lastError();
    RC_ASSERT(error.has_value());
    const std::string message = error->message();
    RC_ASSERT(!message.empty());
    switch (stage) {
        case kCompositor:
            // The cause is carried through, naming the failing asset.
            RC_ASSERT(message.find("mock-asset.mov") != std::string::npos);
            break;
        case kVideoEncode:
            RC_ASSERT(message.find("video encoding failed") != std::string::npos);
            break;
        case kAudioEncode:
            // Requirement 6.10: audio encoding or muxing is named as the stage.
            RC_ASSERT(message.find("audio encoding or muxing failed") != std::string::npos);
            break;
        case kFinalize:
            RC_ASSERT(message.find("finalizing the output file failed") != std::string::npos);
            break;
        default:
            // Requirement 8.11: a hardware failure after at least one encoded frame
            // is reported as a MID-EXPORT hardware encode failure — a different fact
            // from an initialization failure, which becomes a software fallback.
            RC_ASSERT(harness.coordinator->lastSelection().has_value());
            RC_ASSERT(harness.coordinator->lastSelection()->selection.isHardware());
            RC_ASSERT(message.find("mid-export hardware encode failure") != std::string::npos);
            break;
    }

    // --- No file remains, and it is not vacuously absent ----------------------
    RC_ASSERT(script.created.load());
    RC_ASSERT(script.existedAfterOpen.load());
    RC_ASSERT(script.bytesWritten.load() > kHeaderMarker.size());
    RC_ASSERT(!pathExists(out));

    // --- The project is unmodified and byte-identical -------------------------
    RC_ASSERT(harness.serialized() == serializedBefore);
    RC_ASSERT(harness.session.revision() == revisionBefore);
    RC_ASSERT(harness.session.modified() == modifiedBefore);
    const std::optional<ExportOutcome> outcome = harness.coordinator->lastOutcome();
    RC_ASSERT(outcome.has_value());
    RC_ASSERT(!outcome->projectModified);
    RC_ASSERT(!harness.coordinator->running());

    // At least one frame really had been encoded before the failure, which is the
    // antecedent of Requirements 7.5 and 8.11.
    RC_ASSERT(outcome->framesEncoded >= 1);
    switch (stage) {
        case kFinalize:
            RC_ASSERT(outcome->framesEncoded == planned);
            break;
        case kAudioEncode:
            // The render loop submits the frame, then that interval's audio: the
            // failing block's own frame is already encoded.
            RC_ASSERT(outcome->framesEncoded == static_cast<std::size_t>(failAt) + 1);
            break;
        default:
            RC_ASSERT(outcome->framesEncoded == static_cast<std::size_t>(failAt));
            break;
    }
}

}  // namespace palmier::services
