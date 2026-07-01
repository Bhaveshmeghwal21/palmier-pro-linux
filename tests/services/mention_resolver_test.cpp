// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/mention_resolver_test.cpp — tests for @-mention resolution in
// the in-app agent chat (task 16.2; Requirements 8.2, 8.3, 8.4).
//
// These exercise the MentionResolver directly (parsing, matching, rewrite) and
// through the MessagePreprocessor adapter that plugs into the AgentOrchestrator
// seam, plus an end-to-end check that a resolved mention flows into the shared
// tool executor while an unmatched / ambiguous mention rejects the message
// BEFORE it is submitted for processing.
//
// (Task 16.3 adds the dedicated mention-resolution + auth-gating unit-test
// suite; this file proves the 16.2 implementation and lives in a distinct test
// target.)

#include "services/MentionResolver.hpp"

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/AgentOrchestrator.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ToolRegistry.hpp"

namespace palmier::services {
namespace {

MediaAssetRef makeAsset(std::string path) {
    return MediaAssetRef(Uuid::generateV4(), std::move(path));
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST(MentionResolverParse, ExtractsBareMention) {
    const auto mentions = MentionResolver::parseMentions("please trim @intro now");
    ASSERT_EQ(mentions.size(), 1u);
    EXPECT_EQ(mentions[0].name, "intro");
}

TEST(MentionResolverParse, ExtractsFileNameWithExtension) {
    const auto mentions = MentionResolver::parseMentions("use @intro.mp4 here");
    ASSERT_EQ(mentions.size(), 1u);
    EXPECT_EQ(mentions[0].name, "intro.mp4");
}

TEST(MentionResolverParse, ExtractsBracketedAndQuotedMentionsWithSpaces) {
    const auto bracketed = MentionResolver::parseMentions("use @[my clip.mp4] please");
    ASSERT_EQ(bracketed.size(), 1u);
    EXPECT_EQ(bracketed[0].name, "my clip.mp4");

    const auto quoted = MentionResolver::parseMentions("use @\"my clip.mp4\" please");
    ASSERT_EQ(quoted.size(), 1u);
    EXPECT_EQ(quoted[0].name, "my clip.mp4");
}

TEST(MentionResolverParse, MentionAtStartAndAfterPunctuation) {
    const auto atStart = MentionResolver::parseMentions("@intro is first");
    ASSERT_EQ(atStart.size(), 1u);
    EXPECT_EQ(atStart[0].name, "intro");

    const auto afterParen = MentionResolver::parseMentions("(@intro)");
    ASSERT_EQ(afterParen.size(), 1u);
    EXPECT_EQ(afterParen[0].name, "intro");
}

TEST(MentionResolverParse, DoesNotTreatEmailAsMention) {
    // '@' embedded mid-token (not at a boundary) is not a mention.
    const auto mentions = MentionResolver::parseMentions("email ada@example.com now");
    EXPECT_TRUE(mentions.empty());
}

TEST(MentionResolverParse, LoneAtSignIsNotAMention) {
    const auto mentions = MentionResolver::parseMentions("price is @ 5 dollars");
    EXPECT_TRUE(mentions.empty());
}

TEST(MentionResolverParse, ExtractsMultipleMentionsInOrder) {
    const auto mentions = MentionResolver::parseMentions("splice @a before @b");
    ASSERT_EQ(mentions.size(), 2u);
    EXPECT_EQ(mentions[0].name, "a");
    EXPECT_EQ(mentions[1].name, "b");
}

// ---------------------------------------------------------------------------
// 8.2 — a unique match resolves and the token is rewritten canonically
// ---------------------------------------------------------------------------

TEST(MentionResolverResolve, NoMentionsPassesThroughUnchanged) {
    MentionResolver resolver({makeAsset("/media/intro.mp4")});
    const MentionResolution r = resolver.resolve("just do something");
    EXPECT_EQ(r.status, MentionStatus::Resolved);
    EXPECT_EQ(r.rewrittenMessage, "just do something");
    EXPECT_TRUE(r.resolved.empty());
}

TEST(MentionResolverResolve, ResolvesByStemAndRewritesToCanonicalId) {
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    MentionResolver resolver({intro});

    const MentionResolution r = resolver.resolve("trim @intro to 2s");
    ASSERT_EQ(r.status, MentionStatus::Resolved);
    ASSERT_EQ(r.resolved.size(), 1u);
    EXPECT_EQ(r.resolved[0].assetId, intro.assetId);
    // The mention token is replaced by the canonical @<assetId> reference.
    EXPECT_EQ(r.rewrittenMessage, "trim @" + intro.assetId.toString() + " to 2s");
}

TEST(MentionResolverResolve, ResolvesByFileName) {
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    MentionResolver resolver({intro});
    const MentionResolution r = resolver.resolve("place @intro.mp4");
    ASSERT_EQ(r.status, MentionStatus::Resolved);
    ASSERT_EQ(r.resolved.size(), 1u);
    EXPECT_EQ(r.resolved[0].assetId, intro.assetId);
}

TEST(MentionResolverResolve, ResolvesByAssetId) {
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    MentionResolver resolver({intro});
    const MentionResolution r = resolver.resolve("place @" + intro.assetId.toString());
    ASSERT_EQ(r.status, MentionStatus::Resolved);
    ASSERT_EQ(r.resolved.size(), 1u);
    EXPECT_EQ(r.resolved[0].assetId, intro.assetId);
}

TEST(MentionResolverResolve, MatchingIsCaseInsensitive) {
    MediaAssetRef intro = makeAsset("/media/Intro.mp4");
    MentionResolver resolver({intro});
    const MentionResolution r = resolver.resolve("use @INTRO");
    ASSERT_EQ(r.status, MentionStatus::Resolved);
    ASSERT_EQ(r.resolved.size(), 1u);
    EXPECT_EQ(r.resolved[0].assetId, intro.assetId);
}

TEST(MentionResolverResolve, ResolvesMultipleDistinctMentions) {
    MediaAssetRef a = makeAsset("/media/a.mp4");
    MediaAssetRef b = makeAsset("/media/b.mp4");
    MentionResolver resolver({a, b});
    const MentionResolution r = resolver.resolve("splice @a before @b");
    ASSERT_EQ(r.status, MentionStatus::Resolved);
    ASSERT_EQ(r.resolved.size(), 2u);
    EXPECT_EQ(r.rewrittenMessage,
              "splice @" + a.assetId.toString() + " before @" + b.assetId.toString());
}

// ---------------------------------------------------------------------------
// 8.3 — an unmatched mention is rejected (message not submitted)
// ---------------------------------------------------------------------------

TEST(MentionResolverResolve, UnmatchedMentionYieldsUnmatched) {
    MentionResolver resolver({makeAsset("/media/intro.mp4")});
    const MentionResolution r = resolver.resolve("use @ghost");
    EXPECT_EQ(r.status, MentionStatus::Unmatched);
    EXPECT_EQ(r.problemMention, "ghost");
    EXPECT_TRUE(r.rewrittenMessage.empty());
}

TEST(MentionResolverPreprocessor, UnmatchedRejectsWithNotFound) {
    MentionResolver resolver({makeAsset("/media/intro.mp4")});
    const Result<std::string> out = toPreprocessorResult(resolver.resolve("use @ghost"));
    ASSERT_TRUE(out.isError());
    EXPECT_EQ(out.error().code(), ErrorCode::NotFound);
    EXPECT_NE(out.error().message().find("not found"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 8.4 — a mention matching multiple items prompts for selection
// ---------------------------------------------------------------------------

TEST(MentionResolverResolve, AmbiguousMentionListsCandidates) {
    MediaAssetRef a = makeAsset("/a/clip.mp4");
    MediaAssetRef b = makeAsset("/b/clip.mp4");
    MentionResolver resolver({a, b});

    const MentionResolution r = resolver.resolve("trim @clip");
    ASSERT_EQ(r.status, MentionStatus::Ambiguous);
    EXPECT_EQ(r.problemMention, "clip");
    ASSERT_EQ(r.candidates.size(), 2u);
    // Both distinct media items are offered as candidates for selection.
    EXPECT_NE(r.candidates[0].assetId, r.candidates[1].assetId);
    EXPECT_TRUE(r.rewrittenMessage.empty());
}

TEST(MentionResolverPreprocessor, AmbiguousRejectsWithSelectionPrompt) {
    MentionResolver resolver({makeAsset("/a/clip.mp4"), makeAsset("/b/clip.mp4")});
    const Result<std::string> out = toPreprocessorResult(resolver.resolve("trim @clip"));
    ASSERT_TRUE(out.isError());
    EXPECT_EQ(out.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_NE(out.error().message().find("select"), std::string::npos);
}

TEST(MentionResolverResolve, SameItemUnderTwoNamesIsNotAmbiguous) {
    // A single item whose file name and stem both match the mention is one
    // candidate, not two — de-duplication guards the ambiguity count.
    MediaAssetRef clip = makeAsset("/media/clip");  // base name == stem == "clip"
    MentionResolver resolver({clip});
    const MentionResolution r = resolver.resolve("use @clip");
    EXPECT_EQ(r.status, MentionStatus::Resolved);
    ASSERT_EQ(r.resolved.size(), 1u);
    EXPECT_EQ(r.resolved[0].assetId, clip.assetId);
}

// ---------------------------------------------------------------------------
// Integration through the orchestrator seam
// ---------------------------------------------------------------------------

Project makeProject(Uuid& trackId, MediaAssetRef intro) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "Mention Resolver Test";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;

    project.assets.push_back(intro);
    project.tracks.push_back(track);
    trackId = track.id;
    return project;
}

class MockGate : public IAgentAuthGate {
public:
    Result<void> authorize() const override { return ok(); }
};

// Records the message it receives and always maps it to a harmless read tool.
struct RecordingInterpreter {
    std::string* seen;
    Result<AgentIntent> operator()(std::string_view message) const {
        *seen = std::string(message);
        return AgentIntent{"timeline.read", Json::object()};
    }
};

TEST(MentionResolverIntegration, ResolvedMentionReachesInterpreterRewritten) {
    Uuid trackId;
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    TimelineEngine engine(makeProject(trackId, intro));
    ToolRegistry registry = buildDefaultToolRegistry(engine);
    McpToolExecutor executor(registry, &engine);
    MockGate gate;

    std::string seenMessage;
    AgentOrchestrator agent(executor, gate, RecordingInterpreter{&seenMessage});
    agent.setPreprocessor(makeMentionPreprocessor(std::vector<MediaAssetRef>{intro}));

    const Result<AgentTurn> turn = agent.sendMessage("read @intro");
    ASSERT_TRUE(turn.isOk());
    // The interpreter saw the canonical rewrite, not the raw @intro.
    EXPECT_EQ(seenMessage, "read @" + intro.assetId.toString());
}

TEST(MentionResolverIntegration, UnmatchedMentionBlocksSubmission) {
    Uuid trackId;
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    TimelineEngine engine(makeProject(trackId, intro));
    ToolRegistry registry = buildDefaultToolRegistry(engine);
    McpToolExecutor executor(registry, &engine);
    MockGate gate;

    std::string seenMessage;
    AgentOrchestrator agent(executor, gate, RecordingInterpreter{&seenMessage});
    agent.setPreprocessor(makeMentionPreprocessor(std::vector<MediaAssetRef>{intro}));

    const Result<AgentTurn> turn = agent.sendMessage("read @ghost");
    ASSERT_TRUE(turn.isError());
    EXPECT_EQ(turn.error().code(), ErrorCode::NotFound);
    // Never submitted: the interpreter was not invoked.
    EXPECT_TRUE(seenMessage.empty());
}

TEST(MentionResolverIntegration, AmbiguousMentionBlocksSubmission) {
    Uuid trackId;
    MediaAssetRef introA = makeAsset("/a/clip.mp4");
    MediaAssetRef introB = makeAsset("/b/clip.mp4");
    TimelineEngine engine(makeProject(trackId, introA));
    ToolRegistry registry = buildDefaultToolRegistry(engine);
    McpToolExecutor executor(registry, &engine);
    MockGate gate;

    std::string seenMessage;
    AgentOrchestrator agent(executor, gate, RecordingInterpreter{&seenMessage});
    agent.setPreprocessor(
        makeMentionPreprocessor(std::vector<MediaAssetRef>{introA, introB}));

    const Result<AgentTurn> turn = agent.sendMessage("read @clip");
    ASSERT_TRUE(turn.isError());
    EXPECT_EQ(turn.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_TRUE(seenMessage.empty());
}

TEST(MentionResolverIntegration, LiveManagerPreprocessorTracksCurrentLibrary) {
    Uuid trackId;
    MediaAssetRef intro = makeAsset("/media/intro.mp4");
    TimelineEngine engine(makeProject(trackId, intro));
    ToolRegistry registry = buildDefaultToolRegistry(engine);
    McpToolExecutor executor(registry, &engine);
    MockGate gate;

    MediaManager manager;
    std::string seenMessage;
    AgentOrchestrator agent(executor, gate, RecordingInterpreter{&seenMessage});
    agent.setPreprocessor(makeMentionPreprocessor(manager));

    // Before import: the mention is unmatched.
    EXPECT_TRUE(agent.sendMessage("read @intro").isError());

    // After importing the asset, the same mention now resolves against the live
    // library snapshot.
    ASSERT_TRUE(manager.importAsset(intro).isOk());
    const Result<AgentTurn> turn = agent.sendMessage("read @intro");
    ASSERT_TRUE(turn.isOk());
    EXPECT_EQ(seenMessage, "read @" + intro.assetId.toString());
}

}  // namespace
}  // namespace palmier::services
