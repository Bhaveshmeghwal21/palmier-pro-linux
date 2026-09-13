// SPDX-License-Identifier: GPL-3.0-or-later
//
// Part 2 of the export_coordinator_test cases, split out of export_coordinator_test.cpp.
//
// Suites here: ExportCoordinatorTest, ExportToolSchema.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "export_coordinator_test_support.hpp"

namespace palmier::services {
namespace {

using namespace palmier::services::testsupport_exportcoordinatortest;  // NOLINT(google-build-using-namespace)

TEST_F(ExportCoordinatorTest, AVideoOnlyRequestConfiguresNoAudioStream) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("video_only");
    script.outputPath = out;
    makeCoordinator(&script);

    ExportRequest2 r = request(out);
    r.includeAudio = false;
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_FALSE(coordinator_->lastOutcome()->containsAudio);
    EXPECT_EQ(coordinator_->lastOutcome()->audioFrames, 0u);
    EXPECT_FALSE(script.hadAudioSpec.load());
    EXPECT_EQ(script.audioCalls.load(), 0);
}
TEST_F(ExportCoordinatorTest, TheExportRunsOnASnapshotSoALaterEditCannotChangeIt) {
    // The snapshot is taken in begin(); the timeline is then lengthened. The
    // exported frame count must still be the snapshot's, which is what makes an
    // export reproducible while the user keeps working (Requirement 7.2).
    BackendScript script;
    const std::filesystem::path out = scratchPath("snapshot");
    script.outputPath = out;
    makeCoordinator(&script);

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    seedTimeline(40); // a much longer timeline, applied to the LIVE session.
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 4u);
    EXPECT_EQ(script.encodeCalls.load(), 4);
}
TEST_F(ExportCoordinatorTest, UsesTheEncoderSelectorForASoftwareSelection) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("selection_sw");
    script.outputPath = out;
    makeCoordinator(&script);

    ExportRequest2 r = request(out);
    r.preferHardware = false; // software was asked for: not a fallback.
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastSelection().has_value());
    const media::SelectionOutcome& selection = *coordinator_->lastSelection();
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome& outcome = *coordinator_->lastOutcome();

    // The reported name is the selector's, not a second policy's.
    EXPECT_EQ(outcome.encoderName, selection.selection.encoderName());
    EXPECT_EQ(outcome.encoderName, "libx264");
    EXPECT_FALSE(outcome.usedHardwareEncode);
    // Software BY REQUEST is not a fallback (Requirement 8.8).
    EXPECT_FALSE(outcome.usedSoftwareFallback);
    EXPECT_TRUE(outcome.fallbackReason.empty());
    // The requested parameters survive selection unchanged (Requirement 8.3).
    EXPECT_EQ(selection.selection.parameters().resolution.width, kRes.width);
    EXPECT_EQ(selection.selection.parameters().resolution.height, kRes.height);
    EXPECT_EQ(selection.selection.parameters().bitrateBitsPerSecond, r.bitrateKbps * 1000);
    EXPECT_EQ(selection.probe, media::ProbeOutcome::NotRun); // no probe cost with no hardware.
}
// ===========================================================================
// Progress: monotonic, bounded, and at most one second apart (Requirement 7.3)
// ===========================================================================

TEST_F(ExportCoordinatorTest, ProgressIsMonotonicBoundedAndDeliveredOnTheCallingThread) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("progress");
    script.outputPath = out;

    // Every frame advances the injected clock past the interval, so every frame is
    // due — the cadence is exercised without a single sleep.
    ExportCoordinator::Options opts = options(&script);
    script.hookOnFrame = -1;
    coordinator_ = std::make_unique<ExportCoordinator>(session_, *context_, teardown_,
                                                       std::move(opts));

    std::vector<ExportProgressReport> seen;
    std::thread::id deliveringThread{};
    ASSERT_TRUE(coordinator_
                    ->begin(request(out),
                            [&seen, &deliveringThread](const ExportProgressReport& report) {
                                deliveringThread = std::this_thread::get_id();
                                seen.push_back(report);
                            })
                    .isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_FALSE(seen.empty());
    // Delivered on the thread that pumped — never from the worker (Requirement 7.3:
    // the shell keeps processing its own events).
    EXPECT_EQ(deliveringThread, std::this_thread::get_id());

    EXPECT_EQ(seen.front().percent, 0);
    EXPECT_EQ(seen.back().percent, 100);
    for (std::size_t i = 0; i < seen.size(); ++i) {
        EXPECT_GE(seen[i].percent, 0);
        EXPECT_LE(seen[i].percent, 100);
        if (i > 0) EXPECT_GE(seen[i].percent, seen[i - 1].percent);
    }
    EXPECT_EQ(seen.back().framesEncoded, 4u);
    EXPECT_EQ(seen.back().totalFrames, 4u);
}
TEST_F(ExportCoordinatorTest, AStoppedClockStillDeliversTheZeroAndHundredPercentReports) {
    // The clock never advances, so no intermediate report is ever due. The two
    // reports Requirement 7.3/7.4 mandate — the initial 0% and the final 100% —
    // are forced and therefore still arrive.
    BackendScript script;
    const std::filesystem::path out = scratchPath("stopped_clock");
    script.outputPath = out;
    makeCoordinator(&script);

    std::vector<int> percents;
    ASSERT_TRUE(coordinator_
                    ->begin(request(out),
                            [&percents](const ExportProgressReport& r) {
                                percents.push_back(r.percent);
                            })
                    .isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_GE(percents.size(), 2u);
    EXPECT_EQ(percents.front(), 0);
    EXPECT_EQ(percents.back(), 100);
}
TEST_F(ExportCoordinatorTest, CancellingBeforeTheFirstFrameEncodesNothingAndLeavesNoFile) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("cancel_first");
    script.outputPath = out;
    makeCoordinator(&script);

    script.hookOnFrame = 0;
    script.hook = [this]() { coordinator_->cancel(); };

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_TRUE(coordinator_->lastOutcome()->cancelled);
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 1u);
    EXPECT_FALSE(std::filesystem::exists(out));
}
TEST_F(ExportCoordinatorTest, CancelWithNoRunningExportIsHarmless) {
    BackendScript script;
    script.outputPath = scratchPath("cancel_idle");
    makeCoordinator(&script);
    coordinator_->cancel(); // no worker exists.
    EXPECT_FALSE(coordinator_->running());
    EXPECT_FALSE(coordinator_->lastOutcome().has_value());
}
TEST_F(ExportCoordinatorTest, AnAudioFailureIdentifiesTheAudioStageAndLeavesNoFile) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("audio_failure");
    script.outputPath = out;
    makeCoordinator(&script);
    script.failAudioOnBlock = 1; // the second interval's audio fails.

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    // Requirement 6.10: the error identifies audio encoding or muxing as the
    // failing stage, not merely "the export".
    ASSERT_TRUE(coordinator_->lastError().has_value());
    EXPECT_NE(coordinator_->lastError()->message().find("audio encoding or muxing failed"),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(out));
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_FALSE(coordinator_->lastOutcome()->projectModified);
}
TEST_F(ExportCoordinatorTest, AFinalizeFailureIdentifiesTheFinishStageAndLeavesNoFile) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("finish_failure");
    script.outputPath = out;
    makeCoordinator(&script);
    script.failFinish = true;

    ASSERT_TRUE(coordinator_->begin(request(out)).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastError().has_value());
    EXPECT_NE(coordinator_->lastError()->message().find("finalizing the output file failed"),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(out));
}
TEST_F(ExportCoordinatorTest, AFirstFrameHardwareFailureIsNotCalledMidExport) {
    // The distinction Requirement 8.11 draws is "after at least one frame". A
    // failure on the very first frame is the plain video-stage failure.
    BackendScript script;
    const std::filesystem::path out = scratchPath("hw_first_frame");
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

    script.failVideoOnFrame = 0;
    ExportRequest2 r = request(out);
    r.preferHardware = true;
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastError().has_value());
    const std::string message = coordinator_->lastError()->message();
    EXPECT_EQ(message.find("mid-export hardware encode failure"), std::string::npos);
    EXPECT_NE(message.find("video encoding failed"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(out));
}
// ===========================================================================
// Concurrency and admission at begin() (Requirements 7.6, 7.9, 7.10, 7.11)
// ===========================================================================

TEST_F(ExportCoordinatorTest, ASecondConcurrentRequestIsRejectedWithoutDisturbingTheFirst) {
    BackendScript script;
    const std::filesystem::path out = scratchPath("concurrent_first");
    const std::filesystem::path second = scratchPath("concurrent_second");
    script.outputPath = out;
    makeCoordinator(&script);

    // Hold the worker inside the first frame until the test has issued the second
    // request, so the two really do overlap. The gate is released by this thread,
    // so there is no timing assumption: the worker cannot proceed until it is.
    std::promise<void> release;
    std::shared_future<void> released(release.get_future());
    std::promise<void> reached;
    std::shared_future<void> hasReached(reached.get_future());
    script.hookOnFrame = 0;
    script.hook = [&reached, released]() mutable {
        reached.set_value();
        released.wait();
    };

    std::vector<ExportProgressReport> firstProgress;
    ASSERT_TRUE(coordinator_
                    ->begin(request(out),
                            [&firstProgress](const ExportProgressReport& r) {
                                firstProgress.push_back(r);
                            })
                    .isOk());

    // The worker is now parked inside the first frame.
    ASSERT_EQ(hasReached.wait_for(kWaitBudget), std::future_status::ready);
    EXPECT_TRUE(coordinator_->running());

    // Requirement 7.10: the second request is refused and says why.
    auto rejected = coordinator_->begin(request(second));
    ASSERT_TRUE(rejected.isError());
    EXPECT_EQ(rejected.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_NE(rejected.error().message().find("already in progress"), std::string::npos);
    // ...and it created nothing of its own.
    EXPECT_FALSE(std::filesystem::exists(second));

    // The running export is untouched: still running, not cancelled.
    EXPECT_TRUE(coordinator_->running());
    EXPECT_FALSE(coordinator_->cancelRequested());

    release.set_value();
    ASSERT_GT(runToCompletion(), 0u);

    // The first export completed normally, with its own progress and its own file.
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_FALSE(coordinator_->lastOutcome()->cancelled);
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 4u);
    EXPECT_TRUE(std::filesystem::exists(out));
    ASSERT_FALSE(firstProgress.empty());
    EXPECT_EQ(firstProgress.back().percent, 100);
    EXPECT_FALSE(std::filesystem::exists(second));
}
TEST_F(ExportCoordinatorTest, TwoExportsCanRunOneAfterTheOther) {
    BackendScript first;
    const std::filesystem::path outA = scratchPath("sequential_a");
    first.outputPath = outA;
    makeCoordinator(&first);

    ASSERT_TRUE(coordinator_->begin(request(outA)).isOk());
    ASSERT_GT(runToCompletion(), 0u);
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 4u);
    EXPECT_TRUE(std::filesystem::exists(outA));

    // The second export reuses the same coordinator: running() is false again and
    // the per-export state (progress, outcome) starts clean.
    const std::filesystem::path outB = scratchPath("sequential_b");
    first.outputPath = outB;
    first.encodeCalls.store(0);
    first.audioCalls.store(0);
    first.finishCalls.store(0);
    EXPECT_FALSE(coordinator_->running());

    ASSERT_TRUE(coordinator_->begin(request(outB)).isOk());
    ASSERT_GT(runToCompletion(), 0u);
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    EXPECT_EQ(coordinator_->lastOutcome()->outputPath, outB);
    EXPECT_EQ(coordinator_->lastOutcome()->framesEncoded, 4u);
    EXPECT_TRUE(std::filesystem::exists(outB));
    EXPECT_EQ(coordinator_->deliveredProgress().front().percent, 0);
}
// ===========================================================================
// The `timeline.export` tool-surface adapter (task 9.7; Requirements 3.1, 7.2)
//
// Requirement 7.2 asks the tool to "perform the same export as criterion 1" and
// to return the output path, the encoded frame count, the selected encoder name
// and whether hardware encode was used. These cases check both halves:
//
//   * "the same export" — the tool enters through the same `begin()`, so the same
//     request yields outcome fields equal to the dialog's, and every admission
//     rejection reaches the caller unchanged with no file created.
//   * "and returns" — every field named above appears in the result, taken from
//     the ExportOutcome rather than recomputed.
//
// The encode backend is still the mock that writes real bytes: this host's
// libavcodec carries no H.264, HEVC or VP9 encoder, so the seam is the only way an
// export runs here at all.
// ===========================================================================

/// The three codec spellings the schema publishes must be exactly the three the
/// translation parses, and the numeric ranges it publishes must be the
/// coordinator's own constants — the declarations are in two files (the tool
/// surface depends only on Palmier::core) and this is what stops them drifting.
TEST(ExportToolSchema, MatchesTheCoordinatorRanges) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    const Tool*        tool = registry.find("timeline.export");
    ASSERT_NE(tool, nullptr);

    const ArgSpec* width = tool->schema.find("width");
    ASSERT_NE(width, nullptr);
    EXPECT_EQ(width->minInt, static_cast<std::int64_t>(kMinExportWidth));
    EXPECT_EQ(width->maxInt, static_cast<std::int64_t>(kMaxExportWidth));

    const ArgSpec* height = tool->schema.find("height");
    ASSERT_NE(height, nullptr);
    EXPECT_EQ(height->minInt, static_cast<std::int64_t>(kMinExportHeight));
    EXPECT_EQ(height->maxInt, static_cast<std::int64_t>(kMaxExportHeight));

    const ArgSpec* fps = tool->schema.find("fps");
    ASSERT_NE(fps, nullptr);
    EXPECT_EQ(fps->minNum, kMinExportFps);
    EXPECT_EQ(fps->maxNum, kMaxExportFps);

    const ArgSpec* bitrate = tool->schema.find("bitrateKbps");
    ASSERT_NE(bitrate, nullptr);
    EXPECT_EQ(bitrate->minInt, kMinExportBitrateKbps);
    EXPECT_EQ(bitrate->maxInt, kMaxExportBitrateKbps);

    // Every published codec spelling translates to a codec the coordinator
    // supports, and the set is exactly the supported three.
    const ArgSpec* codec = tool->schema.find("codec");
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->enumValues.size(), 3u);
    Project probe;
    for (const std::string& value : codec->enumValues) {
        Json args = Json::object();
        args.set("outputPath", std::string("/tmp/palmier-codec-probe.mp4"));
        args.set("format", std::string("mp4"));
        args.set("codec", value);
        const Result<ExportRequest2> translated = exportRequestFromToolArguments(args, probe);
        ASSERT_TRUE(translated.isOk()) << value;
        EXPECT_TRUE(isSupportedExportCodec(translated.value().codec)) << value;
    }
}
TEST_F(ExportCoordinatorTest, ExportToolReportsEveryFieldRequirement72Names) {
    BackendScript               script;
    const std::filesystem::path out = scratchPath("tool_success");
    script.outputPath = out;
    makeCoordinator(&script);

    const Tool::Handler handler = makeExportToolHandler(*coordinator_, session_);
    Json                args = Json::object();
    args.set("outputPath", out.string());
    args.set("format", std::string("mp4"));
    args.set("width", static_cast<std::int64_t>(kRes.width));
    args.set("height", static_cast<std::int64_t>(kRes.height));
    args.set("codec", std::string("h264"));
    args.set("fps", 30.0);
    args.set("bitrateKbps", static_cast<std::int64_t>(4'000));
    args.set("preferHardware", false);

    const Result<Json> result = handler(args);
    ASSERT_TRUE(result.isOk()) << result.error().toString();
    const Json& out_json = result.value();

    EXPECT_EQ(out_json.stringOr("outputPath"), out.string());
    // The seeded timeline is four frames at 30 fps, and the count the tool reports
    // is the count the planner predicted.
    EXPECT_EQ(out_json.intOr("framesEncoded"), 4);
    EXPECT_EQ(out_json.intOr("plannedFrames"), 4);
    EXPECT_EQ(out_json.intOr("framesEncoded"), out_json.intOr("plannedFrames"));
    // One encoder name, and it is the software encoder this host selects.
    EXPECT_EQ(out_json.stringOr("encoderName"), "libx264");
    EXPECT_FALSE(out_json.boolOr("usedHardwareEncode", true));
    // preferHardware was false, so software is what was asked for — not a fallback.
    EXPECT_FALSE(out_json.boolOr("usedSoftwareFallback", true));
    EXPECT_TRUE(out_json.boolOr("containsAudio"));
    EXPECT_EQ(out_json.intOr("durationNs"),
              static_cast<std::int64_t>(FrameRate::fps30().durationForFrames(4).nanoseconds()));
    // Requirement 7.2's "leaves the project state unchanged", reported so it can be
    // asserted rather than assumed — and checked here against the session too.
    EXPECT_FALSE(out_json.boolOr("projectModified", true));
    EXPECT_FALSE(session_.modified());
    EXPECT_EQ(session_.revision(), revisionBeforeExport_);

    // The file the export produced is really there, with the bytes the backend wrote.
    ASSERT_TRUE(std::filesystem::exists(out));
    EXPECT_FALSE(readAllBytes(out).empty());
}
TEST_F(ExportCoordinatorTest, ExportToolMatchesTheDialogForTheSameRequest) {
    BackendScript               script;
    const std::filesystem::path viaDialog = scratchPath("tool_vs_dialog_a");
    script.outputPath = viaDialog;
    makeCoordinator(&script);

    // (1) The dialog path: begin() directly.
    ASSERT_TRUE(coordinator_->begin(request(viaDialog)).isOk());
    ASSERT_GT(runToCompletion(), 0u);
    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome dialogOutcome = *coordinator_->lastOutcome();

    // (2) The tool path: the same parameters, expressed as tool arguments.
    const std::filesystem::path viaTool = scratchPath("tool_vs_dialog_b");
    {
        std::lock_guard<std::mutex> lock(script.mutex);
        script.outputPath = viaTool;
    }
    const Tool::Handler handler = makeExportToolHandler(*coordinator_, session_);
    Json                args = Json::object();
    args.set("outputPath", viaTool.string());
    args.set("format", std::string("mp4"));
    args.set("width", static_cast<std::int64_t>(kRes.width));
    args.set("height", static_cast<std::int64_t>(kRes.height));
    args.set("codec", std::string("h264"));
    args.set("fps", 30.0);
    args.set("bitrateKbps", static_cast<std::int64_t>(4'000));
    args.set("includeAudio", true);
    args.set("preferHardware", false);

    const Result<Json> toolResult = handler(args);
    ASSERT_TRUE(toolResult.isOk()) << toolResult.error().toString();

    // Every outcome field but the path is equal: the two callers really did run the
    // same export through the same coordinator.
    const Json expected = exportOutcomeToJson(dialogOutcome);
    for (const char* field : {"framesEncoded", "plannedFrames", "encoderName",
                              "usedHardwareEncode", "usedSoftwareFallback", "containsAudio",
                              "audioFrames", "durationNs", "projectModified"}) {
        ASSERT_EQ(expected.contains(field), toolResult.value().contains(field)) << field;
        if (expected.contains(field)) {
            EXPECT_EQ(expected.find(field)->dump(), toolResult.value().find(field)->dump())
                << field;
        }
    }
    EXPECT_NE(expected.stringOr("outputPath"), toolResult.value().stringOr("outputPath"));
}
TEST_F(ExportCoordinatorTest, ExportToolRunsThroughTheSharedToolRegistry) {
    BackendScript               script;
    const std::filesystem::path out = scratchPath("tool_registry");
    script.outputPath = out;
    makeCoordinator(&script);

    // The composition root wires exactly this hook, so this is the path the MCP
    // endpoint, the GUI and the in-app agent all take (Requirement 3.1).
    ToolRegistryHooks hooks;
    hooks.exportTimeline = makeExportToolHandler(*coordinator_, session_);
    const ToolRegistry registry = buildDefaultToolRegistry(session_, std::move(hooks));

    const Tool* tool = registry.find("timeline.export");
    ASSERT_NE(tool, nullptr);

    Json args = Json::object();
    args.set("outputPath", out.string());
    args.set("format", std::string("mp4"));
    args.set("preferHardware", false);
    // The arguments validate against the very schema `tools/list` publishes.
    ASSERT_TRUE(tool->schema.validate(args).isOk());

    const Result<Json> result = registry.invoke("timeline.export", args);
    ASSERT_TRUE(result.isOk()) << result.error().toString();
    EXPECT_EQ(result.value().stringOr("outputPath"), out.string());
    EXPECT_EQ(result.value().intOr("framesEncoded"), 4);
    EXPECT_TRUE(std::filesystem::exists(out));
}
// ---------------------------------------------------------------------------
// Captions sidecar export (usable-editor task 13; Requirement 10.3's second
// export mode, alongside burn-in). A caption cue on the project's timeline
// also needs a text rasterizer installed on the export-local Compositor
// (Requirement 9.5/10.3's shared seam), which the shared fixture's own
// options() does not set for every OTHER test — set directly here instead of
// changing that shared default.
// ---------------------------------------------------------------------------

TEST_F(ExportCoordinatorTest, WritesAnSrtSidecarNextToTheVideoWhenTheProjectHasACaptionCue) {
    // Add a caption track/cue to the already-seeded video+audio timeline.
    Project project = session_.engine().snapshot();
    Clip cue;
    cue.id = Uuid::generateV4();
    cue.timelineStart = Duration::zero();
    cue.sourceIn = Duration::zero();
    cue.sourceOut = FrameRate::fps30().durationForFrames(4);
    cue.captionText = "Hello, captions!";
    Track captionTrack;
    captionTrack.id = Uuid::generateV4();
    captionTrack.kind = TrackKind::Caption;
    captionTrack.clips = {cue};
    project.tracks.push_back(captionTrack);
    ASSERT_TRUE(session_.engine().reset(project).isOk());

    BackendScript script;
    const std::filesystem::path out = scratchPath("captions_sidecar");
    script.outputPath = out;

    ExportCoordinator::Options opts = options(&script);
    // Requirement 9.5/10.3: the export-local Compositor needs a rasterizer to
    // burn the caption cue in at all; a fixed solid frame stands in for a real
    // glyph render, exactly like the video frameProvider above stands in for a
    // real decode.
    opts.textRasterizer = [](const TextStyle&, std::uint32_t w,
                            std::uint32_t h) -> Result<gpu::SourceFrame> {
        return gpu::SourceFrame::solid(w, h, gpu::RgbaColor{255, 255, 255, 255});
    };
    coordinator_ = std::make_unique<ExportCoordinator>(session_, *context_, teardown_, opts);

    ExportRequest2 r = request(out);
    ASSERT_TRUE(coordinator_->begin(r).isOk());
    ASSERT_GT(runToCompletion(), 0u);

    ASSERT_TRUE(coordinator_->lastOutcome().has_value());
    const ExportOutcome& outcome = *coordinator_->lastOutcome();
    EXPECT_FALSE(coordinator_->lastError().has_value());

    // The video output itself succeeded, independent of the sidecar.
    ASSERT_TRUE(std::filesystem::exists(out));

    // The sidecar sits next to it with the same base name and a .srt extension.
    ASSERT_FALSE(outcome.captionsSidecarPath.empty());
    std::filesystem::path expectedSidecar = out;
    expectedSidecar.replace_extension(".srt");
    EXPECT_EQ(outcome.captionsSidecarPath, expectedSidecar);
    ASSERT_TRUE(std::filesystem::exists(outcome.captionsSidecarPath));

    std::ifstream sidecar(outcome.captionsSidecarPath, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(sidecar)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("Hello, captions!"), std::string::npos);
    EXPECT_NE(contents.find("00:00:00,000 -->"), std::string::npos);
}

}  // namespace
}  // namespace palmier::services
