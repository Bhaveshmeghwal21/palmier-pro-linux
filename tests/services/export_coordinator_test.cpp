// SPDX-License-Identifier: GPL-3.0-or-later
//
// The export_coordinator_test cases that stayed in this file after it was split.
//
// Suites here: ExportCoordinatorTest, ExportCoordinatorValidate.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "export_coordinator_test_support.hpp"

namespace palmier::services {
namespace {

using namespace palmier::services::
    testsupport_exportcoordinatortest;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// validate() — rejection before anything is created (Requirements 7.9, 7.11)
// ===========================================================================

TEST(ExportCoordinatorValidate, AcceptsARequestInsideEveryRange) {
    ExportRequest2 r;
    r.outputPath = scratchPath("valid");
    r.container = "mp4";
    r.codec = gpu::CodecId::H264;
    r.resolution = Resolution{1920, 1080};
    r.frameRate = FrameRate::fps30();
    r.bitrateKbps = 8'000;
    EXPECT_TRUE(ExportCoordinator::validate(r).isOk());

    // The range endpoints themselves are accepted.
    r.resolution = Resolution{kMinExportWidth, kMinExportHeight};
    r.bitrateKbps = kMinExportBitrateKbps;
    r.frameRate = FrameRate{1, 1};
    EXPECT_TRUE(ExportCoordinator::validate(r).isOk());

    r.resolution = Resolution{kMaxExportWidth, kMaxExportHeight};
    r.bitrateKbps = kMaxExportBitrateKbps;
    r.frameRate = FrameRate{120, 1};
    EXPECT_TRUE(ExportCoordinator::validate(r).isOk());
}
TEST(ExportCoordinatorValidate, RejectsAnEmptyPath) {
    ExportRequest2 r;
    r.outputPath.clear();
    auto result = ExportCoordinator::validate(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(result.error().message().find("output path"), std::string::npos);
}
TEST(ExportCoordinatorValidate, RejectsAnOverlongPathNamingTheAcceptedRange) {
    ExportRequest2 r;
    r.outputPath = scratchRoot() / std::string(kMaxExportPathLength + 1, 'a');
    auto result = ExportCoordinator::validate(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
    EXPECT_NE(result.error().message().find("4096"), std::string::npos);
}
TEST(ExportCoordinatorValidate, RejectsAMissingParentDirectory) {
    ExportRequest2 r;
    r.outputPath = scratchRoot() / "no_such_directory" / "out.mp4";
    auto result = ExportCoordinator::validate(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    // No file, and no directory, was created by the rejection.
    EXPECT_FALSE(std::filesystem::exists(r.outputPath.parent_path()));
    EXPECT_FALSE(std::filesystem::exists(r.outputPath));
}
TEST(ExportCoordinatorValidate, RejectsAnUnwritableParentDirectory) {
    const std::filesystem::path dir = scratchRoot() / "readonly_parent";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::permissions(dir, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);
    if (::access(dir.c_str(), W_OK) == 0) {
        // Running as a user who bypasses the permission bits (root in a container):
        // the check itself cannot be exercised here.
        GTEST_SKIP() << "this user can write to a read-only directory; "
                        "the writability rejection is not observable";
    }

    ExportRequest2 r;
    r.outputPath = dir / "out.mp4";
    auto result = ExportCoordinator::validate(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    EXPECT_FALSE(std::filesystem::exists(r.outputPath));

    std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
}
TEST(ExportCoordinatorValidate, RejectsAnUnsupportedContainerNamingTheSupportedOnes) {
    ExportRequest2 r;
    r.outputPath = scratchPath("container");
    r.container = "avi";
    auto result = ExportCoordinator::validate(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(result.error().message().find("mp4"), std::string::npos);

    // The four supported containers are accepted, case-insensitively.
    for (const char* container : {"mp4", "MOV", "mkv", "WebM"}) {
        r.container = container;
        EXPECT_TRUE(ExportCoordinator::validate(r).isOk()) << container;
    }
}
TEST(ExportCoordinatorValidate, RejectsACodecTheSelectorDoesNotSupport) {
    ExportRequest2 r;
    r.outputPath = scratchPath("codec");
    for (gpu::CodecId codec : {gpu::CodecId::AV1, gpu::CodecId::MPEG2, gpu::CodecId::Unknown}) {
        r.codec = codec;
        auto result = ExportCoordinator::validate(r);
        ASSERT_TRUE(result.isError());
        EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
        EXPECT_NE(result.error().message().find("H.264"), std::string::npos);
    }
    for (gpu::CodecId codec : {gpu::CodecId::H264, gpu::CodecId::HEVC, gpu::CodecId::VP9}) {
        r.codec = codec;
        EXPECT_TRUE(ExportCoordinator::validate(r).isOk());
    }
}
TEST(ExportCoordinatorValidate, RejectsOutOfRangeGeometryCadenceAndBitRate) {
    const std::filesystem::path out = scratchPath("ranges");

    struct Case {
        const char*  what;
        Resolution   resolution;
        FrameRate    frameRate;
        std::int64_t bitrateKbps;
    };
    const Resolution ok{1920, 1080};
    const Case cases[] = {
        {"width below minimum", Resolution{127, 1080}, FrameRate::fps30(), 8'000},
        {"width above maximum", Resolution{3841, 1080}, FrameRate::fps30(), 8'000},
        {"height below minimum", Resolution{1920, 127}, FrameRate::fps30(), 8'000},
        {"height above maximum", Resolution{1920, 2161}, FrameRate::fps30(), 8'000},
        {"frame rate above maximum", ok, FrameRate{121, 1}, 8'000},
        {"bit rate below minimum", ok, FrameRate::fps30(), kMinExportBitrateKbps - 1},
        {"bit rate above maximum", ok, FrameRate::fps30(), kMaxExportBitrateKbps + 1},
    };

    for (const Case& c : cases) {
        ExportRequest2 r;
        r.outputPath = out;
        r.resolution = c.resolution;
        r.frameRate = c.frameRate;
        r.bitrateKbps = c.bitrateKbps;
        auto result = ExportCoordinator::validate(r);
        ASSERT_TRUE(result.isError()) << c.what;
        EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange) << c.what;
        EXPECT_FALSE(result.error().message().empty()) << c.what;
        EXPECT_FALSE(std::filesystem::exists(out)) << c.what;
    }

    // A degenerate frame rate is a malformed argument rather than a range error.
    ExportRequest2 degenerate;
    degenerate.outputPath = out;
    degenerate.frameRate = FrameRate{0, 0};
    auto result = ExportCoordinator::validate(degenerate);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}
TEST(ExportCoordinatorValidate, RejectsAnExistingDestinationAndPreservesItByteForByte) {
    const std::filesystem::path out = scratchPath("existing");
    const std::string original = "the pre-existing deliverable, byte for byte";
    {
        std::ofstream file(out, std::ios::binary);
        file << original;
    }
    const auto sizeBefore = std::filesystem::file_size(out);
    const auto writeTimeBefore = std::filesystem::last_write_time(out);

    ExportRequest2 r;
    r.outputPath = out;
    r.overwrite = false;
    auto result = ExportCoordinator::validate(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
    EXPECT_NE(result.error().message().find("already exists"), std::string::npos);

    // Requirement 7.11: byte-for-byte, not merely "still there".
    EXPECT_EQ(readAllBytes(out), original);
    EXPECT_EQ(std::filesystem::file_size(out), sizeBefore);
    EXPECT_EQ(std::filesystem::last_write_time(out), writeTimeBefore);

    // With the acknowledgement the same request is admitted.
    r.overwrite = true;
    EXPECT_TRUE(ExportCoordinator::validate(r).isOk());
    EXPECT_EQ(readAllBytes(out), original); // validate() still wrote nothing.
}
TEST(ExportCoordinatorValidate, RejectsAnEmptyTimeline) {
    Project project;
    project.id = Uuid::generateV4();
    project.timelineFps = FrameRate::fps30();
    project.canvas = kRes;
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    project.tracks = {track}; // a track, but zero media segments.

    auto result = ExportCoordinator::validateTimeline(project);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_NE(result.error().message().find("empty"), std::string::npos);
}
// ===========================================================================
// A successful export (Requirements 7.1, 7.2, 7.4, 6.5)
// ===========================================================================

TEST_F(ExportCoordinatorTest, RunsTheRequestedParametersAndReportsTheOutcome) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("success");
    script.outputPath = out;
    makeCoordinator(&script);

    ExportRequest2 r = request(out);
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome& outcome = *coordinator_->lastOutcome();
    EXPECT_FALSE(coordinator_->lastError().has_value());

    // Requirement 7.2: the tool's return values.
    EXPECT_EQ(outcome.outputPath, out);
    EXPECT_EQ(outcome.framesEncoded, 4u);
    EXPECT_EQ(outcome.plannedFrames, 4u);
    EXPECT_FALSE(outcome.encoderName.empty());
    EXPECT_FALSE(outcome.usedHardwareEncode);
    EXPECT_FALSE(outcome.cancelled);

    // Requirement 7.4: the planner and the encode agree, and the reported duration
    // is the planned frame intervals.
    EXPECT_EQ(outcome.framesEncoded, outcome.plannedFrames);
    EXPECT_EQ(outcome.duration, FrameRate::fps30().durationForFrames(4));

    // Requirement 6.5: audio was requested, so exactly one audio stream was
    // configured and audio really was written.
    EXPECT_TRUE(script.hadAudioSpec.load());
    EXPECT_TRUE(outcome.containsAudio);
    EXPECT_EQ(script.audioCalls.load(), 4);
    EXPECT_GT(outcome.audioFrames, 0u);

    // The deliverable survives, and it is not empty.
    ASSERT_TRUE(std::filesystem::exists(out));
    EXPECT_GT(std::filesystem::file_size(out), 0u);
    EXPECT_EQ(script.finishCalls.load(), 1);

    // Requirement 7.1: the project is untouched, and the outcome says so.
    EXPECT_FALSE(outcome.projectModified);
    EXPECT_EQ(session_.revision(), revisionBeforeExport_);
    EXPECT_EQ(outcome.projectRevisionAtStart, revisionBeforeExport_);
    EXPECT_FALSE(coordinator_->running());
}
TEST_F(ExportCoordinatorTest, UsesTheEncoderSelectorForAHardwareSelection) {
    // Capabilities and compiled-in vendor paths are supplied as VALUES, exactly as
    // media::EncoderSelector's own tests do, so the hardware lane is exercised on
    // this host, which has neither a GPU nor a vendor SDK. The probe is awaited
    // through an injected virtual-time awaiter, so nothing waits.
    BackendScript script;
    const std::filesystem::path out = scratchPath("selection_hw");
    script.outputPath = out;

    ExportCoordinator::Options opts = options(&script);
    opts.caps = nvidiaCaps();
    opts.availability = allVendorsCompiledIn();
    opts.selector.probe = [](gpu::CodecId, const gpu::GpuCaps&) { return true; };
    opts.selector.awaiter = [](const media::CapabilityProbe& probe, gpu::CodecId codec,
                               const gpu::GpuCaps& caps, std::chrono::milliseconds) {
        return probe(codec, caps) ? media::ProbeOutcome::Supported
                                  : media::ProbeOutcome::Unsupported;
    };
    coordinator_ =
        std::make_unique<ExportCoordinator>(session_, *context_, teardown_, std::move(opts));

    ExportRequest2 r = request(out);
    r.preferHardware = true;
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastSelection().has_value());
    const media::SelectionOutcome& selection = *coordinator_->lastSelection();
    ASSERT_TRUE(selection.selection.isHardware());
    EXPECT_EQ(selection.selection.encoderName(), "h264_nvenc");
    EXPECT_EQ(selection.probe, media::ProbeOutcome::Supported);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome& outcome = *coordinator_->lastOutcome();
    // Requirement 8.8: never both, whichever way the encoder ended up routed.
    EXPECT_FALSE(outcome.usedHardwareEncode && outcome.usedSoftwareFallback);
    if (outcome.usedHardwareEncode) {
        EXPECT_EQ(outcome.encoderName, "h264_nvenc");
        EXPECT_TRUE(outcome.fallbackReason.empty());
    } else {
        // The encoder's own hardware-init retry ended on software: the outcome must
        // report the SOFTWARE encoder and the fallback, never the hardware name
        // (Requirements 8.3, 8.8).
        EXPECT_EQ(outcome.encoderName, "libx264");
        EXPECT_TRUE(outcome.usedSoftwareFallback);
        EXPECT_FALSE(outcome.fallbackReason.empty());
    }
    EXPECT_TRUE(std::filesystem::exists(out));
}
TEST_F(ExportCoordinatorTest, TheProgressIntervalCeilingIsOneSecond) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("interval");
    script.outputPath = out;

    ExportCoordinator::Options opts = options(&script);
    opts.progressInterval = std::chrono::milliseconds{5'000}; // asked for 5 s.
    ExportCoordinator coordinator(session_, *context_, teardown_, std::move(opts));

    // Requirement 7.3 is a ceiling, so a longer configured interval is clamped
    // down rather than honoured: the guarantee cannot be configured away.
    EXPECT_EQ(coordinator.progressInterval(), std::chrono::milliseconds{1'000});
}
// ===========================================================================
// Cancellation (Requirement 7.7)
// ===========================================================================

TEST_F(ExportCoordinatorTest, CancellationStopsAtAKnownFrameAndLeavesNoFile) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("cancel");
    script.outputPath = out;
    makeCoordinator(&script);

    // Deterministic cancellation: the hook runs on the worker inside the second
    // frame's encode(), so the flag is set before the loop's third frame-boundary
    // check. The export always stops after exactly 2 encoded frames.
    script.hookOnFrame = 1;
    script.hook = [this]() { coordinator_->cancel(); };

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome& outcome = *coordinator_->lastOutcome();
    EXPECT_TRUE(outcome.cancelled);
    EXPECT_EQ(outcome.framesEncoded, 2u);
    EXPECT_EQ(script.encodeCalls.load(), 2);

    // Requirement 7.7: no file remains at the requested path, and the project is
    // unchanged. The backend DID create the file — that is the point.
    EXPECT_TRUE(script.created.load());
    EXPECT_FALSE(std::filesystem::exists(out));
    EXPECT_FALSE(outcome.projectModified);
    EXPECT_EQ(session_.revision(), revisionBeforeExport_);
    EXPECT_TRUE(coordinator_->cancelRequested());
    EXPECT_FALSE(coordinator_->running());
}
// ===========================================================================
// Failure after encoding begins (Requirements 6.10, 7.5, 8.11)
// ===========================================================================

TEST_F(ExportCoordinatorTest, AVideoFailureAfterEncodingBeginsLeavesNoFileAndNamesTheStage) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("video_failure");
    script.outputPath = out;
    makeCoordinator(&script);
    script.failVideoOnFrame = 2; // the third frame fails.

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastError().has_value());
    EXPECT_EQ(coordinator_->lastError()->code(), ErrorCode::Io);
    EXPECT_NE(coordinator_->lastError()->message().find("video encoding failed"),
              std::string::npos);

    // Requirement 7.5: no file remains, and the project is unmodified.
    EXPECT_TRUE(script.created.load());
    EXPECT_FALSE(std::filesystem::exists(out));
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_FALSE(coordinator_->lastOutcome()->projectModified);
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 2u);
    EXPECT_EQ(session_.revision(), revisionBeforeExport_);
}
TEST_F(ExportCoordinatorTest, AMidExportHardwareFailureIsReportedAsSuch) {
    // A hardware route whose encode fails AFTER a frame has been encoded is a hard
    // failure with its own name (Requirement 8.11) — distinct from an
    // initialization failure, which the selector turns into a software fallback.
    BackendScript script;
    const std::filesystem::path out = scratchPath("hw_mid_export");
    script.outputPath = out;

    ExportCoordinator::Options opts = options(&script);
    opts.caps = nvidiaCaps();
    opts.availability = allVendorsCompiledIn();
    opts.selector.probe = [](gpu::CodecId, const gpu::GpuCaps&) { return true; };
    opts.selector.awaiter = [](const media::CapabilityProbe&, gpu::CodecId,
                               const gpu::GpuCaps&, std::chrono::milliseconds) {
        return media::ProbeOutcome::Supported;
    };
    coordinator_ =
        std::make_unique<ExportCoordinator>(session_, *context_, teardown_, std::move(opts));

    script.failVideoOnFrame = 2; // two frames encoded, then the encoder fails.
    ExportRequest2 r = request(out);
    r.preferHardware = true;
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastSelection().has_value());
    ASSERT_TRUE(coordinator_->lastSelection()->selection.isHardware());
    ASSERT_TRUE(coordinator_->lastError().has_value());
    // Requirement 8.11: a hardware failure AFTER at least one frame is reported as
    // a mid-export hardware encode failure — a different fact from an
    // initialization failure, which the selector turns into a software fallback.
    EXPECT_NE(coordinator_->lastError()->message().find("mid-export hardware encode failure"),
              std::string::npos);
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 2u);
    // The incomplete output is gone and the project is unmodified.
    EXPECT_TRUE(script.created.load());
    EXPECT_FALSE(std::filesystem::exists(out));
    EXPECT_FALSE(coordinator_->lastOutcome()->projectModified);
    EXPECT_EQ(session_.revision(), revisionBeforeExport_);
}
TEST_F(ExportCoordinatorTest, AnEncoderThatNeverInitializesCreatesNoFile) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("init_failure");
    script.outputPath = out;

    std::atomic<int> attempts{0};
    ExportCoordinator::Options opts = options(&script);
    opts.encodeFactory = failingInitFactory(&attempts);
    coordinator_ = std::make_unique<ExportCoordinator>(session_, *context_, teardown_,
                                                       std::move(opts));

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastError().has_value());
    EXPECT_GE(attempts.load(), 1);
    EXPECT_FALSE(std::filesystem::exists(out));
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 0u);
    EXPECT_FALSE(coordinator_->lastOutcome()->projectModified);
}
TEST_F(ExportCoordinatorTest, BeginRejectsAnInvalidRequestWithoutStartingAWorker) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("begin_invalid");
    script.outputPath = out;
    makeCoordinator(&script);

    ExportRequest2 r = request(out);
    r.bitrateKbps = 1; // below the accepted range.
    auto result = coordinator_->begin(r);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);

    EXPECT_FALSE(coordinator_->running());
    EXPECT_FALSE(script.created.load());
    EXPECT_FALSE(std::filesystem::exists(out));
}
TEST_F(ExportCoordinatorTest, BeginRejectsAnEmptyTimelineWithoutStartingAWorker) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("begin_empty");
    script.outputPath = out;
    makeCoordinator(&script);

    // Empty the timeline: tracks remain, clips do not.
    Project project = session_.engine().snapshot();
    for (Track& track : project.tracks) track.clips.clear();
    ASSERT_TRUE(session_.engine().reset(project).isOk());

    auto result = coordinator_->begin(request(out));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_FALSE(coordinator_->running());
    EXPECT_FALSE(script.created.load());
    EXPECT_FALSE(std::filesystem::exists(out));
}
TEST_F(ExportCoordinatorTest, BeginRejectsAnExistingDestinationAndPreservesIt) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("begin_existing");
    script.outputPath = out;
    makeCoordinator(&script);

    const std::string original = "an earlier export nobody asked to replace";
    {
        std::ofstream file(out, std::ios::binary);
        file << original;
    }

    auto result = coordinator_->begin(request(out));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
    EXPECT_FALSE(coordinator_->running());
    EXPECT_FALSE(script.created.load());
    EXPECT_EQ(readAllBytes(out), original);
}
TEST_F(ExportCoordinatorTest, TheNotifierFiresSoAnOwnerKnowsWhenToPump) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("notifier");
    script.outputPath = out;
    makeCoordinator(&script);

    std::atomic<int> notifications{0};
    coordinator_->setNotifier([&notifications]() { notifications.fetch_add(1); });

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);
    EXPECT_GT(notifications.load(), 0);
}
TEST_F(ExportCoordinatorTest, ADestroyedCoordinatorCancelsAndLeavesNoPartialFile) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("destroyed");
    script.outputPath = out;
    makeCoordinator(&script);

    // Park the worker in the first frame, then destroy the coordinator: its
    // destructor cancels and joins, and the guard removes the partial output.
    //
    // "Park" has to mean BLOCK, not merely signal. An earlier version of this
    // test only set `reached` from the hook and returned immediately, which left
    // the worker free to finish all four frames and call guard.commit() before
    // the main thread got to reset() — and a committed guard deliberately keeps
    // its file, so the final assertion failed intermittently depending on
    // scheduling (seen on CI run 33417375130, against a docs-only commit that
    // could not have caused it). The hook therefore holds the worker inside
    // encode() until `release` is set, which is what actually guarantees an
    // export is still in flight when the destructor runs.
    std::promise<void>       reached;
    std::shared_future<void> hasReached(reached.get_future());
    std::promise<void>       release;
    std::shared_future<void> mayContinue(release.get_future());
    script.hookOnFrame = 0;
    script.hook = [&reached, mayContinue]() mutable {
        reached.set_value();
        // Bounded, so a mistake here fails the test rather than hanging the suite
        // (and so the destructor's join below can never deadlock on this hook).
        (void)mayContinue.wait_for(kWaitBudget);
    };

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_EQ(hasReached.wait_for(kWaitBudget), std::future_status::ready);
    ASSERT_TRUE(script.created.load()) << "the backend must have created the file to clean up";

    // Destroy on a helper thread: the destructor requests cancellation and then
    // joins the worker, and the worker is parked in the hook, so the release has
    // to come from outside both. Releasing only after the destructor has been
    // entered is what makes the worker observe cancellation at its next frame
    // boundary rather than racing to completion.
    std::thread destroyer([this]() { coordinator_.reset(); });
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    release.set_value();
    destroyer.join();  // must not hang

    EXPECT_FALSE(std::filesystem::exists(out));
    EXPECT_TRUE(script.created.load());
}
TEST_F(ExportCoordinatorTest, ExportToolDefaultsGeometryAndCadenceFromTheProject) {
    BackendScript               script;
    const std::filesystem::path out = scratchPath("tool_defaults");
    script.outputPath = out;
    makeCoordinator(&script);

    // Only the two arguments that were required before task 9.7 — exactly what a
    // pre-9.7 caller (and the offline interpreter's "export as mp4 to <path>"
    // phrase) sends.
    const Tool::Handler handler = makeExportToolHandler(*coordinator_, session_);
    Json                args = Json::object();
    args.set("outputPath", out.string());
    args.set("format", std::string("mp4"));

    const Result<Json> result = handler(args);
    ASSERT_TRUE(result.isOk()) << result.error().toString();

    // The omitted geometry and cadence came from the project, not from a constant.
    const Project project = session_.engine().snapshot();
    std::lock_guard<std::mutex> lock(script.mutex);
    EXPECT_EQ(script.spec.resolution.width, project.canvas.width);
    EXPECT_EQ(script.spec.resolution.height, project.canvas.height);
    EXPECT_EQ(script.spec.frameRate.numerator(), project.timelineFps.numerator());
    EXPECT_EQ(script.spec.frameRate.denominator(), project.timelineFps.denominator());
    // The container came from `format`, and the default codec for mp4 is H.264.
    EXPECT_EQ(script.spec.containerFormat, "mp4");
    EXPECT_EQ(script.spec.codec, gpu::CodecId::H264);
    // `includeAudio` defaults to true, so the encoder was given an audio stream.
    EXPECT_TRUE(script.spec.audio.has_value());
}
TEST_F(ExportCoordinatorTest, ExportToolForwardsAdmissionRejectionsAndCreatesNoFile) {
    BackendScript               script;
    const std::filesystem::path out = scratchPath("tool_rejected");
    script.outputPath = out;
    makeCoordinator(&script);
    const Tool::Handler handler = makeExportToolHandler(*coordinator_, session_);

    // A missing destination is refused by the translation, before the coordinator.
    Json noPath = Json::object();
    noPath.set("format", std::string("mp4"));
    const Result<Json> missing = handler(noPath);
    ASSERT_TRUE(missing.isError());
    EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);

    // An unsupported container is refused by ExportCoordinator::validate, whose
    // message names the supported set (Requirement 7.9) — the translation does not
    // duplicate that rule.
    Json badContainer = Json::object();
    badContainer.set("outputPath", out.string());
    badContainer.set("format", std::string("avi"));
    const Result<Json> unsupportedContainer = handler(badContainer);
    ASSERT_TRUE(unsupportedContainer.isError());
    EXPECT_EQ(unsupportedContainer.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(unsupportedContainer.error().message().find("mp4"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(out));

    // A parent directory that does not exist (Requirement 7.9).
    const std::filesystem::path absent = scratchRoot() / "no_such_dir" / "out.mp4";
    Json missingParent = Json::object();
    missingParent.set("outputPath", absent.string());
    missingParent.set("format", std::string("mp4"));
    const Result<Json> notFound = handler(missingParent);
    ASSERT_TRUE(notFound.isError());
    EXPECT_EQ(notFound.error().code(), ErrorCode::NotFound);
    EXPECT_FALSE(std::filesystem::exists(absent));

    // Requirement 7.11: an existing destination without acknowledgement is refused
    // and preserved byte-for-byte; the same request WITH `overwrite` is accepted.
    const std::filesystem::path existing = scratchPath("tool_existing");
    {
        std::ofstream seed(existing, std::ios::binary | std::ios::trunc);
        seed << "ORIGINAL-CONTENT";
    }
    const std::string before = readAllBytes(existing);
    Json              exists = Json::object();
    exists.set("outputPath", existing.string());
    exists.set("format", std::string("mp4"));
    const Result<Json> alreadyExists = handler(exists);
    ASSERT_TRUE(alreadyExists.isError());
    EXPECT_EQ(alreadyExists.error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(readAllBytes(existing), before);

    {
        std::lock_guard<std::mutex> lock(script.mutex);
        script.outputPath = existing;
    }
    exists.set("overwrite", true);
    exists.set("preferHardware", false);
    const Result<Json> acknowledged = handler(exists);
    ASSERT_TRUE(acknowledged.isOk()) << acknowledged.error().toString();
    EXPECT_NE(readAllBytes(existing), before);

    // Requirement 7.2 again: no rejection and no acceptance touched the project.
    EXPECT_FALSE(session_.modified());
    EXPECT_EQ(session_.revision(), revisionBeforeExport_);
}
TEST_F(ExportCoordinatorTest, ReportsNoSidecarPathWhenTheProjectHasNoCaptions) {
    // The fixture's own seeded timeline (video + audio, no captions).
    BackendScript script;
    const std::filesystem::path out = scratchPath("no_captions");
    script.outputPath = out;
    makeCoordinator(&script);

    ExportRequest2 r = request(out);
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome& outcome = *coordinator_->lastOutcome();
    EXPECT_TRUE(outcome.captionsSidecarPath.empty());

    std::filesystem::path wouldBeSidecar = out;
    wouldBeSidecar.replace_extension(".srt");
    EXPECT_FALSE(std::filesystem::exists(wouldBeSidecar));
}

}  // namespace
}  // namespace palmier::services
