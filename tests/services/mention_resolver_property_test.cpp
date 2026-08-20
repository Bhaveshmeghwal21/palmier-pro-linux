// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/mention_resolver_property_test.cpp — the @-mention resolution
// property (task 10.4; design.md Property 63; Requirements 11.6, 11.7).
//
// This file extends the `palmier_services_mention_resolver_tests` target, whose
// unit half (`mention_resolver_test.cpp`, task 16.2) pins parsing, matching and
// the canonical rewrite by example. What is added here is the quantified
// statement of the two arms:
//
//   * 11.6  "WHEN an agent utterance references media with an `@` mention that
//            names exactly one asset in the project media library, THE
//            Agent_Interpreter SHALL substitute that asset's identifier into the
//            tool arguments before the tool is executed."
//   * 11.7  "IF an `@` mention in an utterance matches no asset or more than one
//            asset in the project media library, THEN THE Agent_Interpreter SHALL
//            return an error that names the mention text and the number of
//            matching assets, SHALL invoke no tool, and SHALL leave the project
//            unchanged."
//
// Note which counts 11.7 covers: **zero as well as two or more**. The design's
// generator says the same ("mention texts matching zero, one or several
// entries"), so the zero-match case is a refusal arm of this property and not a
// separate concern.
//
// The carrier utterance, and why the substitution is observable at all
// -------------------------------------------------------------------
// "Substituted into the tool arguments" only means something if some tool's
// arguments actually carry it. The carrier is `import <path>` — a phrase from the
// offline interpreter's documented table (design.md D9) whose argument is
// precisely the rest of the utterance. So the message
//
//     import @intro.mp4
//
// is rewritten by the resolver to `import @<assetId>` and then interpreted by the
// REAL `OfflineIntentInterpreter` into `media.import` with
// `path = "@<assetId>"` — the asset's identifier, in the tool arguments, before
// the tool runs. The property reads the argument the tool was actually handed, not
// the rewritten message, so a resolver that rewrote the message but failed to get
// the identifier as far as the tool would be caught.
//
// The whole pipeline under test is the real one: `MentionResolver` through the
// real `MessagePreprocessor` seam, the real `AgentOrchestrator`, the real
// `OfflineIntentInterpreter`, the real `buildDefaultToolRegistry` surface, the
// real `McpToolExecutor` policy and a real `ProjectSession` with a real
// `MediaManager`. The one in-test hook is `media.import`, which is hook-backed in
// the real surface: it resolves an `@<assetId>` argument to the library entry that
// is already registered under that id and reports it with `duplicate = true`,
// which is exactly what Requirement 2.5 says a re-import of an already-registered
// location reports. That hook is also the counter: "SHALL invoke no tool" is
// checked by observing that it was never entered, not by inspecting a message.
//
// Generating the three arms honestly
// ----------------------------------
// The library is 0-30 assets whose source paths are built from a vocabulary of
// deliberately overlapping names — shared prefixes (`intro`, `intro_v2`,
// `introduction`), shared stems under different extensions (`clip.mp4`,
// `clip.mov`) and the same file name under different directories (`/a/clip.mp4`,
// `/b/clip.mp4`) — because an ambiguity has to be *constructible* for the refusal
// arm to exist at all.
//
// The arm is then chosen from a bucket index built over the documented name set an
// asset answers to (its full path, its file name, its stem and its canonical asset
// id, compared case-insensitively): a token whose bucket holds exactly one asset
// drives the substitution arm, a token whose bucket holds two or more drives the
// ambiguity arm, and a token that is in no bucket at all drives the zero arm. The
// bucket index is used only to CHOOSE the token and to know the expected count —
// every assertion is about the behaviour the pipeline produced.
//
// Every asset id comes from `Uuid::generateV4()`. Drawing UUID bytes would be
// actively destructive here: `inRange` shrinks towards duplicates, and two assets
// sharing an id would collapse the very uniqueness distinction this property is
// about (and `MediaManager::importAsset` rejects the duplicate anyway).

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/AgentOrchestrator.hpp"
#include "services/Json.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/MentionResolver.hpp"
#include "services/OfflineIntentInterpreter.hpp"
#include "services/ProjectSession.hpp"
#include "services/ProjectStore.hpp"
#include "services/ToolRegistry.hpp"

namespace palmier::services {
namespace {

// ---------------------------------------------------------------------------
// The name vocabulary: overlapping on purpose.
// ---------------------------------------------------------------------------

/// File names sharing prefixes, stems and extensions, so a library drawn from
/// them produces buckets of size 1 and of size >= 2 without being told to.
const std::vector<std::string>& fileNames() {
    static const std::vector<std::string> names = {
        "intro.mp4",  "intro.mov",   "intro_v2.mp4", "introduction.mp4",
        "clip.mp4",   "clip.mov",    "clip2.mp4",    "b_roll.mp4",
        "b-roll.mp4", "outro.mp4",   "outro",        "take.1.mp4",
        "final.mp4",  "final_v2.mp4",
    };
    return names;
}

/// Directories, so the SAME file name can appear more than once in one library.
const std::vector<std::string>& directories() {
    static const std::vector<std::string> dirs = {"/media", "/media/a", "/media/b",
                                                 "/archive", "/archive/2024"};
    return dirs;
}

[[nodiscard]] std::size_t drawIndex(std::size_t count) {
    return *rc::gen::inRange<std::size_t>(0, count);
}

[[nodiscard]] std::string toLowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

/// The names an asset answers to, per `MentionResolver`'s documented contract:
/// its full source path, its file name, that name's stem, and its canonical asset
/// id — all lowercased, because matching is case-insensitive.
[[nodiscard]] std::vector<std::string> answersTo(const MediaAssetRef& asset) {
    std::vector<std::string> names;
    const std::string        path = asset.sourcePath;
    if (!path.empty()) names.push_back(toLowerAscii(path));

    const std::size_t slash = path.find_last_of('/');
    const std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (!base.empty()) {
        names.push_back(toLowerAscii(base));
        const std::size_t dot = base.find_last_of('.');
        const std::string stem = (dot == std::string::npos || dot == 0) ? base
                                                                       : base.substr(0, dot);
        if (!stem.empty()) names.push_back(toLowerAscii(stem));
    }
    names.push_back(asset.assetId.toString());
    return names;
}

// ---------------------------------------------------------------------------
// The library, and the bucket index over it.
// ---------------------------------------------------------------------------

/// token -> the DISTINCT assets that answer to it.
using Buckets = std::map<std::string, std::vector<Uuid>>;

[[nodiscard]] Buckets bucketsFor(const std::vector<MediaAssetRef>& assets) {
    Buckets buckets;
    for (const MediaAssetRef& asset : assets) {
        for (const std::string& name : answersTo(asset)) {
            std::vector<Uuid>& holders = buckets[name];
            bool               already = false;
            for (const Uuid& id : holders) {
                if (id == asset.assetId) {
                    already = true;
                    break;
                }
            }
            if (!already) holders.push_back(asset.assetId);
        }
    }
    return buckets;
}

/// A drawn media library: 0-30 assets over distinct source paths, every id from
/// `Uuid::generateV4()`.
[[nodiscard]] std::vector<MediaAssetRef> drawLibrary() {
    const std::size_t count = drawIndex(31);  // 0..30 inclusive
    std::vector<MediaAssetRef> assets;
    std::vector<std::string>   used;
    assets.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        // Distinct full paths: a repeated (directory, name) pair would be the same
        // location twice, which the media library rejects and which is not what
        // this property is about — ambiguity here means two DIFFERENT locations
        // answering to one name.
        std::string path;
        for (int attempt = 0; attempt < 12; ++attempt) {
            const std::string candidate = *rc::gen::elementOf(directories()) + "/" +
                                          *rc::gen::elementOf(fileNames());
            bool taken = false;
            for (const std::string& earlier : used) {
                if (earlier == candidate) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                path = candidate;
                break;
            }
        }
        if (path.empty()) continue;  // the vocabulary is exhausted; a shorter library
        used.push_back(path);
        assets.emplace_back(Uuid::generateV4(), path);
    }
    return assets;
}

// ---------------------------------------------------------------------------
// The pipeline under test: a real session, the real tool surface, the real
// executor and the real orchestrator, with `media.import` counted.
// ---------------------------------------------------------------------------

class Pipeline {
public:
    /// `assets` become both the project's asset references and the session's media
    /// library, which is what "the project media library" of Requirements 11.6 and
    /// 11.7 means.
    explicit Pipeline(std::vector<MediaAssetRef> assets) : assets_(std::move(assets)) {
        Project project;
        project.id = Uuid::generateV4();
        project.name = "Mention Resolution";
        project.timelineFps = FrameRate::fps30();
        project.canvas = Resolution::hd1080();
        project.assets = assets_;

        Track video;
        video.id = Uuid::generateV4();
        video.kind = TrackKind::Video;
        project.tracks.push_back(std::move(video));
        [[maybe_unused]] const bool seeded = session_.engine().reset(project).isOk();

        for (const MediaAssetRef& asset : assets_) {
            [[maybe_unused]] const bool imported = session_.mediaLibrary().importAsset(asset).isOk();
        }

        ToolRegistryHooks hooks;
        // The one hook, and the property's tool-invocation counter. An argument of
        // the form `@<assetId>` is the canonical reference the mention resolver
        // produces; resolving it against the library that already holds that id and
        // reporting `duplicate = true` is exactly what Requirement 2.5 specifies
        // for an already-registered location.
        hooks.importMedia =
            [this](const std::filesystem::path& path) -> Result<ImportedAsset> {
            ++importCalls_;
            lastImportArgument_ = path.string();

            std::string reference = path.string();
            if (!reference.empty() && reference.front() == '@') reference.erase(0, 1);
            const std::optional<Uuid> parsed = Uuid::parse(reference);
            if (!parsed.has_value()) {
                return err<ImportedAsset>(makeError(
                    ErrorCode::InvalidArgument,
                    "media.import: not an asset reference: " + path.string()));
            }
            const std::optional<MediaAssetRef> known =
                session_.mediaLibrary().asset(*parsed);
            if (!known.has_value()) {
                return err<ImportedAsset>(makeError(
                    ErrorCode::NotFound,
                    "media.import: no such asset: " + parsed->toString()));
            }

            ImportedAsset imported;
            imported.assetId = known->assetId;
            imported.sourcePath = known->sourcePath;
            imported.containerFormat = "mp4";
            imported.durationMs = 5'000;
            imported.hasVideo = true;
            imported.duplicate = true;  // Requirement 2.5 — already registered.
            return imported;
        };

        registry_ = buildDefaultToolRegistry(session_, std::move(hooks));
        executor_ = std::make_unique<McpToolExecutor>(registry_, &session_);
    }

    /// Send `message` the way the in-app agent does: the mention preprocessor,
    /// then the real offline interpreter, then the shared executor. `liveManager`
    /// selects the preprocessor adapter that snapshots the LIVE MediaManager over
    /// the one built from an explicit snapshot; both are production adapters and
    /// both are exercised.
    [[nodiscard]] Result<AgentTurn> send(const std::string& message, bool liveManager) {
        OfflineIntentInterpreter::Options options;
        // No context is needed: `import <path>` takes its whole argument from the
        // utterance. Leaving the network seam armed keeps the no-network guarantee
        // of Requirement 11.3 in force on this path too.
        options.network = [](std::string_view endpoint) -> Result<void> {
            ADD_FAILURE() << "the offline interpreter reached for the network: " << endpoint;
            return err<void>(makeError(ErrorCode::Unsupported, "no network in Offline_Mode"));
        };
        const OfflineIntentInterpreter interpreter(std::move(options));

        AlwaysAuthorized  gate;
        AgentOrchestrator agent(*executor_, gate, interpreter.asInterpreter());
        agent.setPreprocessor(liveManager
                                  ? makeMentionPreprocessor(session_.mediaLibrary())
                                  : makeMentionPreprocessor(assets_));
        return agent.sendMessage(message);
    }

    [[nodiscard]] std::size_t        importCalls() const noexcept { return importCalls_; }
    [[nodiscard]] const std::string& lastImportArgument() const noexcept {
        return lastImportArgument_;
    }
    [[nodiscard]] const std::vector<MediaAssetRef>& assets() const noexcept { return assets_; }

    /// The project, both history depths and the media library as one text — the
    /// "project unchanged" of Requirement 11.7. The project half is
    /// `serializeProject`, the canonical `.palmier` document, so a change anywhere
    /// in it shows up.
    [[nodiscard]] std::string fingerprint() {
        std::string out = serializeProject(session_.engine().snapshot());
        out += "\nundoDepth=" + std::to_string(session_.engine().undoDepth());
        out += "\nredoDepth=" + std::to_string(session_.engine().redoDepth());
        out += "\nrevision=" + std::to_string(session_.revision());
        out += "\nlibrary=";
        for (const MediaAssetRef& asset : session_.mediaLibrary().library()) {
            out += "\n  " + asset.assetId.toString() + " " + asset.sourcePath;
        }
        return out;
    }

private:
    class AlwaysAuthorized : public IAgentAuthGate {
    public:
        [[nodiscard]] Result<void> authorize() const override { return ok(); }
    };

    std::vector<MediaAssetRef>       assets_;
    ProjectSession                   session_;
    ToolRegistry                     registry_;
    std::unique_ptr<McpToolExecutor> executor_;
    std::size_t                      importCalls_ = 0;
    std::string                      lastImportArgument_;
};

// ---------------------------------------------------------------------------
// Arm selection
// ---------------------------------------------------------------------------

enum class Arm { Unique, Ambiguous, Absent };

/// A token that no asset in `buckets` answers to. Built from a fresh UUID so it
/// cannot collide with a generated file name, and checked against the index.
[[nodiscard]] std::string drawAbsentToken(const Buckets& buckets) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::string token = "absent-" + Uuid::generateV4().toString();
        if (buckets.find(toLowerAscii(token)) == buckets.end()) return token;
    }
    return "absent-" + Uuid::generateV4().toString();
}

/// Every token whose bucket holds exactly `1` asset, or at least `2`.
[[nodiscard]] std::vector<std::string> tokensWithBucketSize(const Buckets& buckets,
                                                            bool          unique) {
    std::vector<std::string> tokens;
    for (const auto& [token, holders] : buckets) {
        if (unique ? (holders.size() == 1) : (holders.size() >= 2)) tokens.push_back(token);
    }
    return tokens;
}

/// Make sure `assets` offers the drawn arm, so all three arms are reached at
/// roughly equal rates rather than at whatever rate a blindly drawn library
/// happens to produce (RapidCheck biases `inRange` towards small values, and a
/// one-asset library can offer no ambiguity at all).
///
/// Repair is additive and minimal: the ambiguity arm appends two assets sharing a
/// file name under two directories reserved for the purpose — so the paths are
/// guaranteed free and the injected ambiguity is a genuine "two different
/// locations answering to one name" — and only when the library does not already
/// contain a natural ambiguity, which the larger drawn libraries usually do.
void ensureArmIsAvailable(std::vector<MediaAssetRef>& assets, Arm arm) {
    switch (arm) {
        case Arm::Absent:
            return;  // always available, including for the empty library
        case Arm::Unique:
            // Any non-empty library offers unique tokens: an asset always answers
            // to its own canonical id, and no two assets share one.
            if (assets.empty()) {
                assets.emplace_back(Uuid::generateV4(), "/media/only.mp4");
            }
            return;
        case Arm::Ambiguous: {
            if (!tokensWithBucketSize(bucketsFor(assets), /*unique=*/false).empty()) return;
            const std::string name = *rc::gen::elementOf(fileNames());
            assets.emplace_back(Uuid::generateV4(), "/ambiguous/left/" + name);
            assets.emplace_back(Uuid::generateV4(), "/ambiguous/right/" + name);
            return;
        }
    }
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 63: A unique @mention is
// substituted; a non-unique one is refused — for any media library and any mention
// text, if the mention matches exactly one asset its identifier is substituted
// into the tool arguments before execution; if it matches zero or more than one
// asset, an error naming the mention text and the number of matching assets is
// returned, no tool is invoked, and the project is unchanged.
//
// Requirement 11.6: "WHEN an agent utterance references media with an `@` mention
// that names exactly one asset in the project media library, THE Agent_Interpreter
// SHALL substitute that asset's identifier into the tool arguments before the tool
// is executed."
//
// Requirement 11.7: "IF an `@` mention in an utterance matches no asset or more
// than one asset in the project media library, THEN THE Agent_Interpreter SHALL
// return an error that names the mention text and the number of matching assets,
// SHALL invoke no tool, and SHALL leave the project unchanged."
//
// The generator is libraries of 0-30 assets over a deliberately overlapping name
// vocabulary (shared prefixes, shared stems, the same file name under different
// directories) x mention texts drawn to match exactly zero, exactly one, or two or
// more of them, x both production preprocessor adapters (the live MediaManager one
// and the explicit-snapshot one).
//
// **Validates: Requirements 11.6, 11.7**
// ===========================================================================
RC_GTEST_PROP(MentionResolutionProperties, AUniqueMentionIsSubstitutedANonUniqueOneIsRefused,
              ()) {
    // The arm is drawn FIRST and the library is then made able to offer it, so all
    // three arms are exercised at roughly equal rates. Drawing the library first
    // and taking whichever arms it happened to offer left the ambiguity arm at a
    // few per cent, because RapidCheck biases `inRange` towards small values and a
    // one-asset library cannot be ambiguous about anything.
    // `rc::gen::element` is the uniform choice; `inRange` is biased towards its low
    // end, which would over-sample whichever arm happened to be listed first.
    const Arm arm = *rc::gen::element(Arm::Unique, Arm::Ambiguous, Arm::Absent);

    std::vector<MediaAssetRef> assets = drawLibrary();
    ensureArmIsAvailable(assets, arm);
    const Buckets buckets = bucketsFor(assets);

    const std::vector<std::string> uniqueTokens = tokensWithBucketSize(buckets, true);
    const std::vector<std::string> ambiguousTokens = tokensWithBucketSize(buckets, false);
    RC_ASSERT(arm != Arm::Unique || !uniqueTokens.empty());
    RC_ASSERT(arm != Arm::Ambiguous || !ambiguousTokens.empty());

    std::string token;
    std::size_t expectedMatches = 0;
    switch (arm) {
        case Arm::Unique:
            token = *rc::gen::elementOf(uniqueTokens);
            expectedMatches = 1;
            break;
        case Arm::Ambiguous:
            token = *rc::gen::elementOf(ambiguousTokens);
            expectedMatches = buckets.at(token).size();
            break;
        case Arm::Absent:
            token = drawAbsentToken(buckets);
            expectedMatches = 0;
            break;
    }
    RC_TAG(arm == Arm::Unique ? "unique" : (arm == Arm::Ambiguous ? "ambiguous" : "absent"));

    const bool liveManager = *rc::gen::arbitrary<bool>();
    Pipeline   pipeline(assets);

    // `import @<token>` is a documented phrase whose argument is exactly the
    // mention, so a substituted identifier lands in the tool's `path` argument.
    const std::string utterance = "import @" + token;
    const std::string before = pipeline.fingerprint();

    const Result<AgentTurn> outcome = pipeline.send(utterance, liveManager);

    if (arm == Arm::Unique) {
        // --- Requirement 11.6 ---------------------------------------------
        if (outcome.isError()) {
            RC_FAIL("a mention matching exactly one asset was refused: \"" + utterance +
                    "\" -> " + outcome.error().toString());
        }
        RC_ASSERT(outcome.value().toolName == "media.import");

        // A tool ran, exactly once...
        RC_ASSERT(pipeline.importCalls() == 1u);

        // ...and the identifier of the ONE matching asset is what it was handed,
        // read from the tool's own argument rather than from the rewritten message.
        const Uuid expected = buckets.at(token).front();
        RC_ASSERT(pipeline.lastImportArgument() == "@" + expected.toString());

        // The tool's result reports that same asset, so the substitution reached
        // the end of the pipeline and not merely its start.
        RC_ASSERT(outcome.value().result.stringOr("assetId") == expected.toString());
        return;
    }

    // --- Requirement 11.7 (both counts: zero, and two or more) --------------
    RC_ASSERT(outcome.isError());

    // The error names the mention text...
    const std::string message = outcome.error().message();
    RC_ASSERT(message.find(token) != std::string::npos);

    // ...and the number of matching assets, as a number.
    RC_ASSERT(message.find(std::to_string(expectedMatches) + " matching assets") !=
              std::string::npos);

    // The two counts are distinguished by code, so a caller can tell "not in the
    // library" from "say which one".
    RC_ASSERT(outcome.error().code() ==
              (expectedMatches == 0 ? ErrorCode::NotFound : ErrorCode::FailedPrecondition));

    // No tool was invoked: the hook was never entered, and the message never
    // reached the interpreter, so nothing could have been executed.
    RC_ASSERT(pipeline.importCalls() == 0u);

    // And the project is unchanged, byte for byte.
    RC_ASSERT(pipeline.fingerprint() == before);
}

// ---------------------------------------------------------------------------
// Unit tests: the examples the property quantifies over but does not pin by name,
// and the checks that keep it from being vacuous.
// ---------------------------------------------------------------------------

// The property counts hook entries to decide "no tool was invoked". If the hook
// could not be reached at all, every refusal assertion would pass for the wrong
// reason.
TEST(MentionResolutionPipeline, TheImportHookIsReachedOnAUniqueMention) {
    const MediaAssetRef intro(Uuid::generateV4(), "/media/intro.mp4");
    Pipeline            pipeline({intro});

    ASSERT_EQ(pipeline.importCalls(), 0u);
    const Result<AgentTurn> turn = pipeline.send("import @intro", /*liveManager=*/false);
    ASSERT_TRUE(turn.isOk()) << turn.error().toString();
    EXPECT_EQ(pipeline.importCalls(), 1u);
    EXPECT_EQ(pipeline.lastImportArgument(), "@" + intro.assetId.toString());
}

// Requirement 11.7's exact wording, on both counts, spelled out by example: the
// message names the mention text and the number of matching assets.
TEST(MentionResolutionRefusal, NamesTheMentionTextAndTheNumberOfMatchingAssets) {
    const MediaAssetRef a(Uuid::generateV4(), "/a/clip.mp4");
    const MediaAssetRef b(Uuid::generateV4(), "/b/clip.mp4");
    const MediaAssetRef c(Uuid::generateV4(), "/c/clip.mp4");

    // Zero matches.
    {
        Pipeline                pipeline({a});
        const Result<AgentTurn> turn = pipeline.send("import @ghost", false);
        ASSERT_TRUE(turn.isError());
        EXPECT_EQ(turn.error().code(), ErrorCode::NotFound);
        EXPECT_NE(turn.error().message().find("ghost"), std::string::npos);
        EXPECT_NE(turn.error().message().find("0 matching assets"), std::string::npos);
        EXPECT_EQ(pipeline.importCalls(), 0u);
    }

    // Exactly two matches.
    {
        Pipeline                pipeline({a, b});
        const Result<AgentTurn> turn = pipeline.send("import @clip", false);
        ASSERT_TRUE(turn.isError());
        EXPECT_EQ(turn.error().code(), ErrorCode::FailedPrecondition);
        EXPECT_NE(turn.error().message().find("clip"), std::string::npos);
        EXPECT_NE(turn.error().message().find("2 matching assets"), std::string::npos);
        EXPECT_EQ(pipeline.importCalls(), 0u);
    }

    // Three matches — the count is the real count, not the word "several".
    {
        Pipeline                pipeline({a, b, c});
        const Result<AgentTurn> turn = pipeline.send("import @clip.mp4", false);
        ASSERT_TRUE(turn.isError());
        EXPECT_NE(turn.error().message().find("3 matching assets"), std::string::npos);
        EXPECT_EQ(pipeline.importCalls(), 0u);
    }
}

// The vocabulary has to be able to produce both bucket sizes, or the property's
// arms would not all be reachable. This pins that the overlap is real.
TEST(MentionResolutionGenerator, TheVocabularyProducesUniqueAndAmbiguousBuckets) {
    const MediaAssetRef introA(Uuid::generateV4(), "/media/intro.mp4");
    const MediaAssetRef introB(Uuid::generateV4(), "/media/a/intro.mp4");
    const MediaAssetRef outro(Uuid::generateV4(), "/media/outro.mp4");
    const Buckets       buckets = bucketsFor({introA, introB, outro});

    // The same file name under two directories is one ambiguous bucket...
    ASSERT_NE(buckets.find("intro.mp4"), buckets.end());
    EXPECT_EQ(buckets.at("intro.mp4").size(), 2u);
    EXPECT_EQ(buckets.at("intro").size(), 2u);
    // ...while the full paths and the asset ids stay unique.
    EXPECT_EQ(buckets.at("/media/intro.mp4").size(), 1u);
    EXPECT_EQ(buckets.at(introA.assetId.toString()).size(), 1u);
    EXPECT_EQ(buckets.at("outro.mp4").size(), 1u);

    // And an asset answering to one token under two of its names is still one
    // holder, so the count the refusal reports is a count of ASSETS.
    const MediaAssetRef stemless(Uuid::generateV4(), "/media/solo");
    const Buckets       single = bucketsFor({stemless});
    EXPECT_EQ(single.at("solo").size(), 1u);
}

// The empty library is the degenerate case of the zero arm: every mention matches
// nothing, and nothing can be executed.
TEST(MentionResolutionRefusal, AnEmptyLibraryRefusesEveryMention) {
    Pipeline          pipeline({});
    const std::string before = pipeline.fingerprint();

    for (const std::string mention : {std::string("intro"), std::string("clip.mp4"),
                                      Uuid::generateV4().toString()}) {
        const Result<AgentTurn> turn = pipeline.send("import @" + mention, true);
        ASSERT_TRUE(turn.isError()) << mention;
        EXPECT_EQ(turn.error().code(), ErrorCode::NotFound) << mention;
        EXPECT_NE(turn.error().message().find("0 matching assets"), std::string::npos)
            << mention;
    }
    EXPECT_EQ(pipeline.importCalls(), 0u);
    EXPECT_EQ(pipeline.fingerprint(), before);
}

// A mention that names the asset id directly is the canonical form the resolver
// itself produces, so resolving an already-canonical message must be idempotent
// rather than a second substitution.
TEST(MentionResolutionSubstitution, ACanonicalAssetIdMentionResolvesToItself) {
    const MediaAssetRef intro(Uuid::generateV4(), "/media/intro.mp4");
    Pipeline            pipeline({intro});

    const Result<AgentTurn> turn =
        pipeline.send("import @" + intro.assetId.toString(), false);
    ASSERT_TRUE(turn.isOk()) << turn.error().toString();
    EXPECT_EQ(pipeline.lastImportArgument(), "@" + intro.assetId.toString());
}

// Both preprocessor adapters are production code and the property draws between
// them; this pins that they agree on all three arms for one library.
TEST(MentionResolutionAdapters, TheLiveAndSnapshotPreprocessorsAgree) {
    const MediaAssetRef a(Uuid::generateV4(), "/a/clip.mp4");
    const MediaAssetRef b(Uuid::generateV4(), "/b/clip.mp4");
    const MediaAssetRef solo(Uuid::generateV4(), "/media/solo.mp4");

    for (const bool live : {false, true}) {
        Pipeline pipeline({a, b, solo});

        const Result<AgentTurn> unique = pipeline.send("import @solo", live);
        ASSERT_TRUE(unique.isOk()) << live << ": " << unique.error().toString();
        EXPECT_EQ(pipeline.lastImportArgument(), "@" + solo.assetId.toString()) << live;

        const Result<AgentTurn> ambiguous = pipeline.send("import @clip", live);
        ASSERT_TRUE(ambiguous.isError()) << live;
        EXPECT_EQ(ambiguous.error().code(), ErrorCode::FailedPrecondition) << live;
        EXPECT_NE(ambiguous.error().message().find("2 matching assets"), std::string::npos)
            << live;

        const Result<AgentTurn> absent = pipeline.send("import @ghost", live);
        ASSERT_TRUE(absent.isError()) << live;
        EXPECT_EQ(absent.error().code(), ErrorCode::NotFound) << live;

        // Only the unique arm ever reached a tool.
        EXPECT_EQ(pipeline.importCalls(), 1u) << live;
    }
}

}  // namespace
}  // namespace palmier::services
