// SPDX-License-Identifier: GPL-3.0-or-later
//
// Part 2 of the export_coordinator_property_test cases, split out of export_coordinator_property_test.cpp.
//
// Suites here: ExportCoordinatorProperties.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "export_coordinator_property_test_support.hpp"

namespace palmier::services {

using namespace palmier::services::
    testsupport_exportcoordinatorpropertytest;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// Property 34
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 34: Progress is monotonic, bounded and timely —
// for any timeline and any frame rate, the sequence of progress reports emitted during an export is
// monotonically non-decreasing, every value lies in 0–100, consecutive reports are at most 1 second
// apart, the first report is 0 and a successful export's last report is 100.
//
// **Validates: Requirements 7.3**
RC_GTEST_PROP(ExportCoordinatorProperties, ProgressIsMonotonicBoundedAndTimely, ()) {
    const FrameRate fps = kFrameRates[*rc::gen::inRange<int>(0, kFrameRateCount)];
    const int frames = *rc::gen::inRange<int>(1, 49);
    // How long each frame "takes", in the injected clock's virtual time: values
    // below, at and above the one-second ceiling.
    const auto advanceMs = *rc::gen::inRange<std::int64_t>(0, 1'501);
    // Requested cadences, including one above the ceiling that must be clamped.
    const std::chrono::milliseconds requestedInterval{
        *rc::gen::element<std::int64_t>(0, 100, 250, 500, 1'000, 5'000)};
    const bool includeAudio = *rc::gen::arbitrary<bool>();

    const Resolution resolution{128, 128};
    CaseDir dir("p34");
    const std::filesystem::path out = dir.file("progress.mp4");

    ManualClock clock;
    ExportScript script;
    script.outputPath = out;
    script.clock = &clock;
    script.advancePerFrame = std::chrono::milliseconds{advanceMs};

    Harness harness;
    RC_ASSERT(harness.install(makeTimeline(frames, fps, resolution, includeAudio)));
    const std::size_t planned =
        media::ExportEngine::plannedFrameCount(harness.session.engine().snapshot(), fps);
    RC_ASSERT(planned >= 1);

    ExportCoordinatorOptions options = hostlessOptions(&script, &clock, resolution);
    options.progressInterval = requestedInterval;
    harness.make(std::move(options));

    // Requirement 7.3 is a CEILING: a longer configured interval is clamped down,
    // so the guarantee cannot be configured away.
    const std::chrono::milliseconds ceiling{1'000};
    const std::chrono::milliseconds effective = std::min(requestedInterval, ceiling);
    RC_ASSERT(harness.coordinator->progressInterval() == effective);

    std::vector<ExportProgressReport> seen;
    std::thread::id deliveringThread{};
    RC_ASSERT(harness.coordinator
                  ->begin(makeRequest(out, "mp4", gpu::CodecId::H264, resolution, fps, 4'000,
                                      includeAudio, /*preferHardware=*/false),
                          [&seen, &deliveringThread](const ExportProgressReport& report) {
                              deliveringThread = std::this_thread::get_id();
                              seen.push_back(report);
                          })
                  .isOk());
    RC_ASSERT(harness.coordinator->awaitCompletion(kWaitBudget) > 0);
    RC_ASSERT(harness.coordinator->lastOutcome().has_value());
    RC_ASSERT(!harness.coordinator->lastError().has_value());
    RC_ASSERT(!seen.empty());

    // Marshalled to the owning thread, never delivered from the worker — which is
    // what keeps the Editor_Shell free to process its own window events.
    RC_ASSERT(deliveringThread == std::this_thread::get_id());

    // --- Monotonic and bounded ------------------------------------------------
    RC_ASSERT(seen.front().percent == 0);
    RC_ASSERT(seen.back().percent == 100);
    for (std::size_t i = 0; i < seen.size(); ++i) {
        RC_ASSERT(seen[i].percent >= 0);
        RC_ASSERT(seen[i].percent <= 100);
        RC_ASSERT(seen[i].totalFrames == planned);
        RC_ASSERT(seen[i].framesEncoded <= planned);
        // The percentage is the frame count's own percentage, so no report can claim
        // progress that no encoded frame backs.
        const int expectedPercent =
            static_cast<int>((static_cast<std::uint64_t>(seen[i].framesEncoded) * 100u) / planned);
        RC_ASSERT(seen[i].percent == expectedPercent);
        if (i > 0) {
            RC_ASSERT(seen[i].percent >= seen[i - 1].percent);
            RC_ASSERT(seen[i].framesEncoded >= seen[i - 1].framesEncoded);
        }
    }

    // --- Timely: the exact cadence the ≤1 s ceiling prescribes ----------------
    //
    // The coordinator is offered progress at known virtual-time instants — a forced
    // report before the export starts, the engine's own initial report, one per
    // rendered frame (the engine reports every frame, because the coordinator owns
    // the cadence), and a forced final report — and emits an offer only when it is
    // forced or at least `effective` has elapsed since the last emission. Replaying
    // that rule over the generated schedule predicts the exact sequence of reports,
    // so BOTH halves of the guarantee are checked: nothing is emitted early, and
    // nothing due is withheld. No sleeping is involved anywhere.
    struct Offer {
        std::size_t frames;
        std::int64_t atMs;
        bool forced;
    };
    std::vector<Offer> offers;
    const std::int64_t endMs = static_cast<std::int64_t>(planned) * advanceMs;
    offers.push_back({0, 0, true});   // the coordinator's initial 0% report.
    offers.push_back({0, 0, false});  // the engine's own initial report.
    for (std::size_t i = 1; i <= planned; ++i) {
        offers.push_back({i, static_cast<std::int64_t>(i) * advanceMs, false});
    }
    offers.push_back({planned, endMs, false});  // the engine's completion report.
    offers.push_back({planned, endMs, true});   // the coordinator's forced final 100%.

    std::vector<std::size_t> expectedFrames;
    std::vector<std::int64_t> expectedAtMs;
    std::int64_t lastEmitMs = 0;
    bool everEmitted = false;
    for (const Offer& offer : offers) {
        const bool due =
            offer.forced || !everEmitted || (offer.atMs - lastEmitMs) >= effective.count();
        if (!due) continue;
        expectedFrames.push_back(offer.frames);
        expectedAtMs.push_back(offer.atMs);
        lastEmitMs = offer.atMs;
        everEmitted = true;
    }

    RC_ASSERT(seen.size() == expectedFrames.size());
    for (std::size_t i = 0; i < seen.size(); ++i) {
        RC_ASSERT(seen[i].framesEncoded == expectedFrames[i]);
    }

    // The consequence Requirement 7.3 states: consecutive reports are at most one
    // second apart, allowing for the fact that a report can only be produced at a
    // frame boundary — so no gap exceeds the ceiling plus one frame's duration.
    for (std::size_t i = 1; i < expectedAtMs.size(); ++i) {
        RC_ASSERT(expectedAtMs[i] >= expectedAtMs[i - 1]);
        RC_ASSERT(expectedAtMs[i] - expectedAtMs[i - 1] <= effective.count() + advanceMs);
    }

    // And when frames are at least as slow as the ceiling, EVERY frame is reported:
    // the coordinator never batches progress away.
    if (advanceMs >= effective.count() && advanceMs > 0) {
        std::set<std::size_t> reported;
        for (const ExportProgressReport& report : seen) reported.insert(report.framesEncoded);
        for (std::size_t i = 1; i <= planned; ++i) RC_ASSERT(reported.count(i) == 1);
    }
}
// ===========================================================================
// Property 35
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 35: A successful export matches the planner —
// for any timeline and any frame rate, a successful export produces a non-empty, probeable,
// decodable file whose decoded frame count equals `ExportEngine::plannedFrameCount` for that
// timeline and frame rate and whose reported duration differs from the timeline duration by at most
// one frame interval.
//
// **Validates: Requirements 7.4**
RC_GTEST_PROP(ExportCoordinatorProperties, ASuccessfulExportMatchesThePlanner, ()) {
    const Resolution resolution = kGeometries[*rc::gen::inRange<int>(0, kGeometryCount)];
    const FrameRate fps = kFrameRates[*rc::gen::inRange<int>(0, kFrameRateCount)];
    const int frames = *rc::gen::inRange<int>(1, maxFramesFor(resolution) + 1);
    const bool includeAudio = *rc::gen::arbitrary<bool>();
    const std::string container = kContainers[*rc::gen::inRange<int>(0, kContainerCount)];

    CaseDir dir("p35");
    const std::filesystem::path out = dir.file("planned." + toLowerAscii(container));

    ManualClock clock;
    ExportScript script;
    script.outputPath = out;
    script.clock = &clock;

    Harness harness;
    RC_ASSERT(harness.install(makeTimeline(frames, fps, resolution, includeAudio)));
    const Project snapshot = harness.session.engine().snapshot();
    const std::size_t planned = media::ExportEngine::plannedFrameCount(snapshot, fps);
    RC_ASSERT(planned >= 1);

    harness.make(hostlessOptions(&script, &clock, resolution));
    RC_ASSERT(harness.coordinator
                  ->begin(makeRequest(out, container, gpu::CodecId::H264, resolution, fps, 6'000,
                                      includeAudio, /*preferHardware=*/false))
                  .isOk());
    RC_ASSERT(harness.coordinator->awaitCompletion(kWaitBudget) > 0);

    RC_ASSERT(!harness.coordinator->lastError().has_value());
    const std::optional<ExportOutcome> outcome = harness.coordinator->lastOutcome();
    RC_ASSERT(outcome.has_value());
    RC_ASSERT(!outcome->cancelled);

    // The deliverable exists and is not empty.
    RC_ASSERT(pathExists(out));
    RC_ASSERT(std::filesystem::file_size(out) > 0);
    RC_ASSERT(script.finishCalls.load() == 1);

    // The frame count recovered from the FILE'S OWN BYTES equals the planner's
    // count — an independent count, not the coordinator's.
    RC_ASSERT(frameMarkersInFile(out) == planned);
    RC_ASSERT(outcome->framesEncoded == planned);
    RC_ASSERT(outcome->plannedFrames == planned);

    // The reported duration differs from the timeline duration by at most one frame
    // interval.
    const Duration timeline = timelineDuration(snapshot);
    const Duration interval = fps.frameDuration();
    RC_ASSERT((outcome->duration - timeline).abs() <= interval);

    // One audio stream when audio was asked for and none when it was not
    // (Requirement 6.5); the audio really was written, one block per frame interval.
    RC_ASSERT(outcome->containsAudio == includeAudio);
    if (includeAudio) {
        RC_ASSERT(outcome->audioFrames > 0);
        RC_ASSERT(static_cast<std::size_t>(script.audioCalls.load()) == planned);
    } else {
        RC_ASSERT(outcome->audioFrames == 0);
        RC_ASSERT(script.audioCalls.load() == 0);
    }
}
// ===========================================================================
// Property 37
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 37: Cancellation leaves no file and no project
// change — for any timeline and any cancellation instant during an export, encoding stops within 2
// seconds, no file remains at the requested path, the outcome reports the export cancelled, and the
// project is reported unmodified.
//
// **Validates: Requirements 7.7**
RC_GTEST_PROP(ExportCoordinatorProperties, CancellationLeavesNoFileAndNoProjectChange, ()) {
    const Resolution resolution = kGeometries[*rc::gen::inRange<int>(0, kGeometryCount)];
    const FrameRate fps = kFrameRates[*rc::gen::inRange<int>(0, kFrameRateCount)];
    const int frames = *rc::gen::inRange<int>(1, maxFramesFor(resolution) + 1);
    const bool includeAudio = *rc::gen::arbitrary<bool>();
    // Drawn before the planned count is known, and clamped into range below.
    const int drawnCancelAt = *rc::gen::inRange<int>(-1, 49);

    CaseDir dir("p37");
    const std::filesystem::path out = dir.file("cancelled.mp4");

    ManualClock clock;
    ExportScript script;
    script.outputPath = out;
    script.clock = &clock;

    // The instant the cancel flag was set, as seen by the worker: Requirement 7.7's
    // two-second bound is measured from here, never slept for.
    std::mutex cancelMutex;
    std::optional<std::chrono::steady_clock::time_point> cancelledAt;
    auto markCancelled = [&cancelMutex, &cancelledAt]() {
        std::lock_guard<std::mutex> lock(cancelMutex);
        if (!cancelledAt.has_value()) cancelledAt = std::chrono::steady_clock::now();
    };

    std::promise<void> parked;
    std::shared_future<void> hasParked(parked.get_future());
    std::promise<void> release;
    std::shared_future<void> released(release.get_future());
    std::atomic<bool> parkedOnce{false};

    Harness harness;
    RC_ASSERT(harness.install(makeTimeline(frames, fps, resolution, includeAudio)));
    const std::string serializedBefore = harness.serialized();
    const std::uint64_t revisionBefore = harness.session.revision();
    const bool modifiedBefore = harness.session.modified();
    const std::size_t planned =
        media::ExportEngine::plannedFrameCount(harness.session.engine().snapshot(), fps);
    RC_ASSERT(planned >= 1);

    // The cancellation instant: -1 cancels BEFORE the first frame (from inside the
    // backend factory, once the output file exists); 0..planned-1 cancels from
    // inside that frame's encode(). The last of those is the design generator's
    // "after the last frame" boundary.
    const int cancelAt =
        drawnCancelAt < 0 ? -1 : static_cast<int>(drawnCancelAt % static_cast<int>(planned));

    harness.make(hostlessOptions(&script, &clock, resolution));
    ReleaseOnExit releaseGuard(release);
    ExportCoordinator* coordinator = harness.coordinator.get();

    {
        std::lock_guard<std::mutex> lock(script.mutex);
        if (cancelAt < 0) {
            script.afterCreateHook = [&parked, &released, &parkedOnce, &markCancelled,
                                      coordinator]() {
                if (parkedOnce.exchange(true)) return;
                parked.set_value();
                released.wait();
                markCancelled();
                coordinator->cancel();
            };
        } else {
            script.encodeHook = [&markCancelled, coordinator, cancelAt](int index) {
                if (index == cancelAt) {
                    markCancelled();
                    coordinator->cancel();
                }
            };
        }
    }

    RC_ASSERT(harness.coordinator
                  ->begin(makeRequest(out, "mp4", gpu::CodecId::H264, resolution, fps, 6'000,
                                      includeAudio, /*preferHardware=*/false))
                  .isOk());

    if (cancelAt < 0) {
        // The worker is parked in the factory: the file it created really is on
        // disk, so the "no file remains" assertion below cannot be vacuous.
        RC_ASSERT(hasParked.wait_for(kWaitBudget) == std::future_status::ready);
        RC_ASSERT(pathExists(out));
        releaseGuard.release();
    }

    RC_ASSERT(harness.coordinator->waitForCompletion(kWaitBudget));
    const auto observedAt = std::chrono::steady_clock::now();
    RC_ASSERT(harness.coordinator->awaitCompletion(kWaitBudget) > 0);

    // The cancellation really happened, and the export stopped within 2 seconds of
    // it (Requirement 7.7) — a measured bound, so a coordinator that dawdles fails.
    {
        std::lock_guard<std::mutex> lock(cancelMutex);
        RC_ASSERT(cancelledAt.has_value());
        RC_ASSERT(observedAt - *cancelledAt <= kCancelStopBudget);
    }
    RC_ASSERT(harness.coordinator->cancelRequested());
    RC_ASSERT(!harness.coordinator->running());

    const std::optional<ExportOutcome> outcome = harness.coordinator->lastOutcome();
    RC_ASSERT(outcome.has_value());

    // Anti-vacuity: a real file was created at this exact path.
    RC_ASSERT(script.created.load());
    RC_ASSERT(script.existedAfterOpen.load());
    RC_ASSERT(script.bytesWritten.load() > 0);

    if (cancelAt == static_cast<int>(planned) - 1) {
        // The boundary the design's generator names: a cancellation that arrives
        // once the last frame has been encoded cannot un-write a finished export, so
        // the deliverable survives and the outcome is a success. Asserted rather
        // than skipped, because it is the edge of the guarantee.
        RC_ASSERT(!outcome->cancelled);
        RC_ASSERT(outcome->framesEncoded == planned);
        RC_ASSERT(pathExists(out));
    } else {
        RC_ASSERT(outcome->cancelled);
        RC_ASSERT(!harness.coordinator->lastError().has_value());
        // Encoding stopped at the frame boundary following the cancellation.
        RC_ASSERT(outcome->framesEncoded == static_cast<std::size_t>(cancelAt + 1));
        RC_ASSERT(outcome->framesEncoded < planned);
        // No file remains at the requested path.
        RC_ASSERT(!pathExists(out));
    }

    // The project is unmodified either way.
    RC_ASSERT(!outcome->projectModified);
    RC_ASSERT(harness.serialized() == serializedBefore);
    RC_ASSERT(harness.session.revision() == revisionBefore);
    RC_ASSERT(harness.session.modified() == modifiedBefore);
}
// ===========================================================================
// Property 39
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 39: Invalid export requests are rejected before
// any file exists — for any export request violating exactly one constraint — output path longer
// than 4096 characters, a parent directory that does not exist or is not writable, an unsupported
// container or codec, a resolution, frame rate or bit rate outside its range, or an existing
// destination without an overwrite acknowledgement — the request is rejected before encoding begins,
// no file is created at the requested path, any pre-existing file at that path is byte-for-byte
// preserved, and the error names the rejected parameter together with its accepted range or
// supported values.
//
// **Validates: Requirements 7.6, 7.9, 7.11**
RC_GTEST_PROP(ExportCoordinatorProperties, InvalidExportRequestsAreRejectedBeforeAnyFileExists,
              ()) {
    enum Perturbation {
        kOverlongPath = 0,
        kMissingParent = 1,
        kUnsupportedContainer = 2,
        kUnsupportedCodec = 3,
        kResolutionOutOfRange = 4,
        kFrameRateOutOfRange = 5,
        kBitrateOutOfRange = 6,
        kExistingDestination = 7,
        kEmptyTimeline = 8,
        kUnwritableParent = 9,
    };
    // The unwritable-parent perturbation is generated only where the operating
    // system lets this process observe it: running as root bypasses the permission
    // bits entirely, and asserting a rejection that cannot happen would make the
    // property false rather than strong.
    const int perturbationCount = unwritableDirectoriesAreObservable() ? 10 : 9;
    const int perturbation = *rc::gen::inRange<int>(0, perturbationCount);

    const Resolution resolution = kGeometries[*rc::gen::inRange<int>(0, kGeometryCount)];
    const FrameRate fps = kFrameRates[*rc::gen::inRange<int>(0, kFrameRateCount)];
    const std::string container = kContainers[*rc::gen::inRange<int>(0, kContainerCount)];
    const gpu::CodecId codec = kCodecs[*rc::gen::inRange<int>(0, kCodecCount)];
    const auto bitrateKbps =
        *rc::gen::inRange<std::int64_t>(kMinExportBitrateKbps, kMaxExportBitrateKbps + 1);
    const bool includeAudio = *rc::gen::arbitrary<bool>();
    const int frames = *rc::gen::inRange<int>(1, 5);

    CaseDir dir("p39");
    const std::filesystem::path out = dir.file("rejected.mp4");

    ManualClock clock;
    ExportScript script;
    script.outputPath = out;
    script.clock = &clock;

    Harness harness;
    RC_ASSERT(harness.install(makeTimeline(frames, fps, resolution, includeAudio)));
    harness.make(hostlessOptions(&script, &clock, resolution));

    // The unperturbed request, which MUST be admissible: the rejection below is then
    // attributable to the single generated perturbation and to nothing else.
    const ExportRequest2 base = makeRequest(out, container, codec, resolution, fps, bitrateKbps,
                                            includeAudio, /*preferHardware=*/false);
    RC_ASSERT(ExportCoordinator::validate(base).isOk());
    RC_ASSERT(ExportCoordinator::validateTimeline(harness.session.engine().snapshot()).isOk());

    ExportRequest2 request = base;
    ErrorCode expectedCode = ErrorCode::InvalidArgument;
    std::string expectedInMessage;

    /// The bytes an existing destination must still hold afterwards.
    std::string preExistingBytes;
    std::filesystem::path unwritableDir;

    switch (perturbation) {
        case kOverlongPath: {
            request.outputPath = dir.file(std::string(kMaxExportPathLength + 1, 'p'));
            expectedCode = ErrorCode::OutOfRange;
            expectedInMessage = std::to_string(kMaxExportPathLength);
            break;
        }
        case kMissingParent: {
            request.outputPath = dir.path() / "no_such_directory" / "out.mp4";
            expectedCode = ErrorCode::NotFound;
            expectedInMessage = "does not exist";
            break;
        }
        case kUnsupportedContainer: {
            request.container = *rc::gen::element<std::string>("avi", "flv", "ogg", "mpg", "");
            expectedCode = ErrorCode::Unsupported;
            expectedInMessage = "mp4";  // the supported containers are quoted back.
            break;
        }
        case kUnsupportedCodec: {
            request.codec =
                *rc::gen::element(gpu::CodecId::AV1, gpu::CodecId::MPEG2, gpu::CodecId::Unknown);
            expectedCode = ErrorCode::Unsupported;
            expectedInMessage = "H.264";  // the supported codecs are quoted back.
            break;
        }
        case kResolutionOutOfRange: {
            switch (*rc::gen::inRange<int>(0, 4)) {
                case 0:
                    request.resolution = Resolution{kMinExportWidth - 1, resolution.height};
                    break;
                case 1:
                    request.resolution = Resolution{kMaxExportWidth + 2, resolution.height};
                    break;
                case 2:
                    request.resolution = Resolution{resolution.width, kMinExportHeight - 1};
                    break;
                default:
                    request.resolution = Resolution{resolution.width, kMaxExportHeight + 2};
                    break;
            }
            expectedCode = ErrorCode::OutOfRange;
            expectedInMessage = "out of range";
            break;
        }
        case kFrameRateOutOfRange: {
            const bool tooFast = *rc::gen::arbitrary<bool>();
            request.frameRate = tooFast ? FrameRate{*rc::gen::inRange<std::int64_t>(121, 481), 1}
                                        : FrameRate{1, *rc::gen::inRange<std::int64_t>(2, 61)};
            expectedCode = ErrorCode::OutOfRange;
            expectedInMessage = "out of range";
            break;
        }
        case kBitrateOutOfRange: {
            const bool tooHigh = *rc::gen::arbitrary<bool>();
            request.bitrateKbps = tooHigh
                                      ? *rc::gen::inRange<std::int64_t>(kMaxExportBitrateKbps + 1,
                                                                        kMaxExportBitrateKbps * 4)
                                      : *rc::gen::inRange<std::int64_t>(0, kMinExportBitrateKbps);
            expectedCode = ErrorCode::OutOfRange;
            expectedInMessage = "out of range";
            break;
        }
        case kExistingDestination: {
            preExistingBytes =
                *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::arbitrary<char>()));
            std::ofstream file(request.outputPath, std::ios::binary);
            file.write(preExistingBytes.data(),
                       static_cast<std::streamsize>(preExistingBytes.size()));
            file.close();
            RC_ASSERT(pathExists(request.outputPath));
            request.overwrite = false;
            expectedCode = ErrorCode::AlreadyExists;
            expectedInMessage = "already exists";
            break;
        }
        case kEmptyTimeline: {
            // Requirement 7.6: a timeline with zero media segments. The request
            // itself stays valid, so the rejection is about the project alone.
            Project emptied = harness.session.engine().snapshot();
            for (Track& track : emptied.tracks) track.clips.clear();
            RC_ASSERT(harness.session.engine().reset(emptied).isOk());
            expectedCode = ErrorCode::FailedPrecondition;
            expectedInMessage = "empty";
            break;
        }
        default: {
            unwritableDir = dir.path() / "readonly_parent";
            std::error_code ec;
            std::filesystem::create_directories(unwritableDir, ec);
            std::filesystem::permissions(
                unwritableDir,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                std::filesystem::perm_options::replace, ec);
            request.outputPath = unwritableDir / "out.mp4";
            expectedCode = ErrorCode::PermissionDenied;
            expectedInMessage = "not writable";
            break;
        }
    }

    const std::string serializedBefore = harness.serialized();
    const std::uint64_t revisionBefore = harness.session.revision();

    // --- Rejected before anything is created ---------------------------------
    const Result<void> begun = harness.coordinator->begin(request);
    RC_ASSERT(begun.isError());
    RC_ASSERT(begun.error().code() == expectedCode);
    RC_ASSERT(begun.error().message().find(expectedInMessage) != std::string::npos);

    // The empty-timeline rejection is the one that is a fact about the PROJECT;
    // every other perturbation is also rejected by the pure static validate(),
    // which creates nothing whatsoever.
    if (perturbation == kEmptyTimeline) {
        RC_ASSERT(ExportCoordinator::validate(request).isOk());
    } else {
        const Result<void> validated = ExportCoordinator::validate(request);
        RC_ASSERT(validated.isError());
        RC_ASSERT(validated.error().code() == expectedCode);
    }

    // No worker was started and no encoder — hence no file — was ever created.
    RC_ASSERT(!harness.coordinator->running());
    RC_ASSERT(script.factoryCalls.load() == 0);
    RC_ASSERT(!script.created.load());
    RC_ASSERT(!harness.coordinator->lastOutcome().has_value());

    if (perturbation == kExistingDestination) {
        // Requirement 7.11: byte-for-byte, not merely "still there".
        RC_ASSERT(pathExists(request.outputPath));
        RC_ASSERT(readAllBytes(request.outputPath) == preExistingBytes);
        RC_ASSERT(std::filesystem::file_size(request.outputPath) == preExistingBytes.size());
        // ...and the acknowledgement is what unlocks it, so the rejection is the
        // absence of consent rather than an unusable path.
        ExportRequest2 acknowledged = request;
        acknowledged.overwrite = true;
        RC_ASSERT(ExportCoordinator::validate(acknowledged).isOk());
        RC_ASSERT(readAllBytes(request.outputPath) == preExistingBytes);
    } else {
        RC_ASSERT(!pathExists(request.outputPath));
    }

    // The project is untouched by the rejection itself.
    RC_ASSERT(harness.serialized() == serializedBefore);
    RC_ASSERT(harness.session.revision() == revisionBefore);

    if (!unwritableDir.empty()) {
        std::error_code ec;
        std::filesystem::permissions(unwritableDir, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
    }
}

}  // namespace palmier::services
