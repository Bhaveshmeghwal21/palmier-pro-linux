// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/offline_interpreter_property_test.cpp — the offline
// interpreter's four properties (task 10.2; design.md Properties 59, 60, 61, 64;
// Requirements 11.2, 11.3, 11.4, 11.9).
//
// What each property is really checking, and why it is not a tautology
// -------------------------------------------------------------------
//   * Property 59 does not compare the interpreter against itself: it builds real
//     case and whitespace PERTURBATIONS of each documented phrase — upper, lower
//     and alternating case, leading and trailing spaces, tabs, newlines, carriage
//     returns and interior whitespace runs — and asserts every one of them
//     resolves to the same tool name and the same argument object as the canonical
//     form.
//   * Property 60 does not merely check that the produced JSON is well formed. It
//     drives the invocation through the REAL `buildDefaultToolRegistry` surface
//     and the REAL `McpToolExecutor` policy against a REAL `ProjectSession`, so a
//     produced invocation that the advertised schema would reject, or that the
//     engine would refuse, is a genuine counterexample. That is the only way to
//     read Requirement 11.2's "a tool name that exists in the Tool_Surface
//     together with an argument object that satisfies that tool's declared input
//     schema" as a checkable statement.
//   * Property 61 quantifies over generated strings that the interpreter does NOT
//     map, including deliberate near misses of every phrase, and asserts the
//     response quotes the request verbatim while the project's serialized state
//     and its undo depth are byte-identical before and after.
//   * Property 64 covers both bounds arms — whitespace-only strings over the whole
//     whitespace set, and lengths 0, 1, 1999, 2000, 2001 and 5000 — and asserts the
//     rejection names the permitted 1-2000 character range.
//
// The no-network guarantee (Requirement 11.3), proven two ways
// -----------------------------------------------------------
//   1. The component's declared network seam (`Options::network`) is installed
//      with a gate that fails the test if it is ever called. That catches an
//      implementation that reaches the network the sanctioned way.
//   2. The C library's socket entry points — `socket`, `connect`, `getaddrinfo`,
//      `gethostbyname`, `sendto` — are INTERPOSED for the duration of every
//      interpretation. A definition in the main executable takes precedence over
//      glibc's, so a call made from anywhere in the code under test lands here
//      first, is counted, and is forwarded to the real symbol. That catches an
//      implementation that reaches the network *around* the seam.
//
// Both counters are asserted to be zero after every interpretation, so "issues no
// network request" is a checked property rather than a claim about the source.
//
// No sleeping, no clock stubbing, no FFmpeg, no GPU
// ------------------------------------------------
// The 1-second bound of Requirement 11.3 is measured with a steady clock over the
// real call, and asserted at a small fraction of the budget. `media.import` and
// `timeline.export` are hook-backed in the real surface; this binary wires
// deliberately minimal hooks — the import hook registers an asset in the session's
// real `MediaManager`, the export hook writes the bytes to the requested path —
// because what Property 60 is about is whether the INTERPRETER's output is
// acceptable to the tool surface, not whether an encoder produces valid H.264.
// Every write-performing tool is only ever given an absolute path inside a
// per-case scratch directory whose name carries the process id.

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

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
#include "services/AgentInterpreterRegistry.hpp"
#include "services/AgentOrchestrator.hpp"
#include "services/Json.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/OfflineIntentInterpreter.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"

// ===========================================================================
// The socket interposers (see the file comment, point 2).
//
// These are ordinary strong definitions in the test executable, so the dynamic
// linker resolves calls from every object in this binary to them rather than to
// glibc's. Each records the call when arming is in effect and then forwards to the
// real implementation, so a legitimate socket user elsewhere in the process still
// works.
// ===========================================================================

namespace {

std::atomic<bool>        gNetworkArmed{false};
std::atomic<std::size_t> gSocketCalls{0};

/// Count a socket entry point if we are inside an interpretation.
void noteSocketCall() {
    if (gNetworkArmed.load(std::memory_order_relaxed)) {
        gSocketCalls.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Arms the interposers for one interpretation and reports what happened.
class NetworkWatch {
public:
    NetworkWatch() {
        gSocketCalls.store(0, std::memory_order_relaxed);
        gNetworkArmed.store(true, std::memory_order_relaxed);
    }
    ~NetworkWatch() { gNetworkArmed.store(false, std::memory_order_relaxed); }

    NetworkWatch(const NetworkWatch&) = delete;
    NetworkWatch& operator=(const NetworkWatch&) = delete;

    [[nodiscard]] std::size_t calls() const {
        return gSocketCalls.load(std::memory_order_relaxed);
    }
};

}  // namespace

extern "C" {

int socket(int domain, int type, int protocol) {
    noteSocketCall();
    using Fn = int (*)(int, int, int);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "socket"));
    return real ? real(domain, type, protocol) : -1;
}

int connect(int fd, const struct sockaddr* address, socklen_t length) {
    noteSocketCall();
    using Fn = int (*)(int, const struct sockaddr*, socklen_t);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "connect"));
    return real ? real(fd, address, length) : -1;
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                struct addrinfo** result) {
    noteSocketCall();
    using Fn = int (*)(const char*, const char*, const struct addrinfo*, struct addrinfo**);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "getaddrinfo"));
    return real ? real(node, service, hints, result) : EAI_FAIL;
}

struct hostent* gethostbyname(const char* name) {
    noteSocketCall();
    using Fn = struct hostent* (*)(const char*);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "gethostbyname"));
    return real ? real(name) : nullptr;
}

ssize_t sendto(int fd, const void* buffer, size_t length, int flags,
               const struct sockaddr* address, socklen_t addressLength) {
    noteSocketCall();
    using Fn = ssize_t (*)(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
    static Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "sendto"));
    return real ? real(fd, buffer, length, flags, address, addressLength) : -1;
}

}  // extern "C"

namespace palmier::services {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Scratch directory — every write-performing tool is only ever given an absolute
// path inside one of these, and the name carries the process id because ctest
// runs this binary once per discovered case, in parallel processes.
// ---------------------------------------------------------------------------

class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<std::uint64_t> counter{0};
        root_ = fs::temp_directory_path() /
                ("palmier_offline_interp_" + std::to_string(static_cast<long long>(::getpid())) +
                 "_" + std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] const fs::path& root() const noexcept { return root_; }

    /// A fresh ABSOLUTE path inside this directory.
    [[nodiscard]] fs::path file(std::string_view tag, std::string_view extension) const {
        static std::atomic<std::uint64_t> counter{0};
        return root_ / (std::string(tag) + "_" + std::to_string(counter.fetch_add(1)) +
                        std::string(extension));
    }

    /// An existing readable file, so `media.import` has something real to register.
    [[nodiscard]] fs::path existingFile(std::string_view tag) const {
        const fs::path path = file(tag, ".mp4");
        std::ofstream out(path, std::ios::binary);
        out << "palmier scratch media";
        return path;
    }

private:
    fs::path root_;
};

// ---------------------------------------------------------------------------
// The fixture project: two video tracks, one audio track, clips on the first
// video track. Track ordinals 1..3 are therefore all resolvable, which is what
// "mute track N" needs.
//
// Every identifier comes from `Uuid::generateV4()`. Drawing UUID bytes with a
// generator would shrink towards the nil UUID and towards duplicates, both of
// which `MediaManager::importAsset` rejects outright — the failure would be the
// generator's, not the interpreter's.
// ---------------------------------------------------------------------------

struct Fixture {
    Project           project;
    std::vector<Uuid> trackIds;   ///< in project order, so index i is ordinal i+1
    Uuid              clipId;     ///< a clip with room on either side to split
    std::int64_t      clipStartNs = 0;
    std::int64_t      clipEndNs = 0;
};

[[nodiscard]] Fixture makeFixture() {
    Fixture fixture;
    Project& project = fixture.project;
    project.id = Uuid::generateV4();
    project.name = "Offline Interpreter";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    const MediaAssetRef asset(Uuid::generateV4(), "/media/offline-fixture.mp4");
    project.assets.push_back(asset);

    Track video;
    video.id = Uuid::generateV4();
    video.kind = TrackKind::Video;
    for (int i = 0; i < 2; ++i) {
        Clip clip;
        clip.id = Uuid::generateV4();
        clip.assetRef = asset;
        clip.timelineStart = Duration::fromMilliseconds(i * 10'000);
        clip.sourceIn = Duration::fromMilliseconds(0);
        clip.sourceOut = Duration::fromMilliseconds(5'000);
        if (i == 0) {
            fixture.clipId = clip.id;
            fixture.clipStartNs = clip.timelineStart.nanoseconds();
            fixture.clipEndNs = clip.timelineEnd().nanoseconds();
        }
        video.clips.push_back(std::move(clip));
    }

    Track secondVideo;
    secondVideo.id = Uuid::generateV4();
    secondVideo.kind = TrackKind::Video;

    Track audio;
    audio.id = Uuid::generateV4();
    audio.kind = TrackKind::Audio;

    fixture.trackIds = {video.id, secondVideo.id, audio.id};
    project.tracks.push_back(std::move(video));
    project.tracks.push_back(std::move(secondVideo));
    project.tracks.push_back(std::move(audio));
    return fixture;
}

// ---------------------------------------------------------------------------
// The stack under test: a real ProjectSession, the real default tool surface with
// minimal hooks for the two hook-backed tools the phrase table reaches, and the
// real execution policy.
// ---------------------------------------------------------------------------

class Stack {
public:
    explicit Stack(const ScratchDir& scratch) : fixture_(makeFixture()) {
        [[maybe_unused]] const bool seeded = session_.engine().reset(fixture_.project).isOk();

        ToolRegistryHooks hooks;
        // `media.import`: register the file as one asset of the session's REAL media
        // library. This is the library the tool surface reports through `media.list`
        // and `project.info`, so the registration is observable exactly as a real
        // import would be. Probing the container needs FFmpeg, which this build does
        // not require, and probing is not what Property 60 is about.
        hooks.importMedia =
            [this](const fs::path& path) -> Result<ImportedAsset> {
            const MediaAssetRef asset(Uuid::generateV4(), path.string());
            if (Result<void> added = session_.mediaLibrary().importAsset(asset);
                added.isError()) {
                return err<ImportedAsset>(std::move(added).error());
            }
            ImportedAsset imported;
            imported.assetId = asset.assetId;
            imported.sourcePath = path;
            imported.containerFormat = "mp4";
            imported.durationMs = 5'000;
            imported.hasVideo = true;
            return imported;
        };
        // `timeline.export`: write the requested bytes to the requested path. The
        // interpreter's job is to produce an `outputPath` and a `format` the tool
        // accepts; encoding is stage 9's concern and its files are owned elsewhere.
        hooks.exportTimeline = [](const Json& in) -> Result<Json> {
            const std::string outputPath = in.stringOr("outputPath");
            if (outputPath.empty()) {
                return err<Json>(makeError(ErrorCode::InvalidArgument,
                                           "timeline.export: 'outputPath' is required"));
            }
            std::ofstream out(outputPath, std::ios::binary);
            if (!out) {
                return err<Json>(makeError(ErrorCode::Io,
                                           "timeline.export: cannot write " + outputPath));
            }
            out << "palmier export";
            Json result = Json::object();
            result.set("outputPath", outputPath);
            result.set("format", in.stringOr("format"));
            return result;
        };

        registry_ = buildDefaultToolRegistry(session_, std::move(hooks));
        executor_ = std::make_unique<McpToolExecutor>(registry_, &session_);
        savePath_ = scratch.file("project", ".palmier");
    }

    [[nodiscard]] const Fixture&    fixture() const noexcept { return fixture_; }
    [[nodiscard]] ProjectSession&   session() noexcept { return session_; }
    [[nodiscard]] const ToolRegistry& registry() const noexcept { return registry_; }
    [[nodiscard]] McpToolExecutor&  executor() noexcept { return *executor_; }

    /// The editor context the interpreter reads: the fixture's clip as the
    /// selection, an interior playhead, the fixture's tracks in order, and an
    /// absolute scratch destination so `save the project` writes inside the case's
    /// own directory and nowhere else.
    [[nodiscard]] EditorContext context() const {
        EditorContext context;
        context.selectedClipId = fixture_.clipId;
        context.playheadNs = fixture_.clipStartNs + (fixture_.clipEndNs - fixture_.clipStartNs) / 2;
        context.trackIds = fixture_.trackIds;
        context.documentPath = savePath_;
        return context;
    }

    /// The interpreter under test, with the network seam armed to fail the test.
    [[nodiscard]] OfflineIntentInterpreter interpreter() const {
        OfflineIntentInterpreter::Options options;
        options.context = [this] { return context(); };
        options.network = [](std::string_view endpoint) -> Result<void> {
            ADD_FAILURE() << "the offline interpreter reached for the network: " << endpoint;
            return err<void>(makeError(ErrorCode::Unsupported, "no network in Offline_Mode"));
        };
        return OfflineIntentInterpreter(std::move(options));
    }

    /// The project as the tool surface reports it, plus the undo depth: the pair
    /// Requirement 11.4 says an unmappable utterance must leave unchanged.
    [[nodiscard]] std::string stateFingerprint() {
        const Result<Json> read = registry_.invoke("timeline.read", Json::object());
        std::string out = read.isOk() ? read.value().dump() : std::string("<unreadable>");
        out += "|undo=" + std::to_string(session_.engine().undoDepth());
        out += "|redo=" + std::to_string(session_.engine().redoDepth());
        out += "|assets=" + std::to_string(session_.mediaLibrary().assetCount());
        return out;
    }

private:
    Fixture                          fixture_;
    ProjectSession                   session_;
    ToolRegistry                     registry_;
    std::unique_ptr<McpToolExecutor> executor_;
    fs::path                         savePath_;
};

// ---------------------------------------------------------------------------
// Interpretation under the two network guards, with the 1-second bound measured
// over the real call.
// ---------------------------------------------------------------------------

struct Interpretation {
    Result<AgentIntent>       outcome;
    std::chrono::microseconds elapsed{0};
    std::size_t               socketCalls = 0;
};

[[nodiscard]] Interpretation interpretGuarded(const OfflineIntentInterpreter& interpreter,
                                             std::string_view utterance) {
    NetworkWatch watch;
    const auto   started = std::chrono::steady_clock::now();
    Result<AgentIntent> outcome = interpreter.interpret(utterance);
    const auto   finished = std::chrono::steady_clock::now();
    return Interpretation{
        std::move(outcome),
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started),
        watch.calls()};
}

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

[[nodiscard]] std::size_t drawIndex(std::size_t count) {
    return *rc::gen::inRange<std::size_t>(0, count);
}

/// One documented pattern, and a concrete canonical utterance for it. Ordinal
/// patterns draw a track ordinal that the fixture can resolve (1..3); suffix
/// patterns draw an absolute scratch path.
struct DrawnPhrase {
    PhrasePattern pattern;
    std::string   canonical;   ///< the utterance in canonical (lowercase) form
    std::size_t   prefixWords = 0;  ///< words belonging to the phrase itself
};

[[nodiscard]] std::size_t wordsIn(std::string_view text) {
    std::size_t words = 0;
    bool        inWord = false;
    for (const char c : text) {
        const bool space = (c == ' ');
        if (!space && !inWord) ++words;
        inWord = !space;
    }
    return words;
}

[[nodiscard]] DrawnPhrase drawPhrase(const ScratchDir& scratch, const Fixture& fixture) {
    const std::vector<PhrasePattern>& table = OfflineIntentInterpreter::patterns();
    const PhrasePattern&              pattern = table[drawIndex(table.size())];

    DrawnPhrase drawn;
    drawn.pattern = pattern;
    drawn.prefixWords = wordsIn(pattern.text);

    switch (pattern.match) {
        case PhraseMatch::Exact:
            drawn.canonical = pattern.canonicalUtterance();
            break;
        case PhraseMatch::Ordinal: {
            // Only ordinals the fixture can resolve: the property is about the
            // mapping, and an out-of-range ordinal is the separate, documented
            // "track N does not exist" refusal.
            const std::size_t ordinal = 1 + drawIndex(fixture.trackIds.size());
            drawn.canonical = pattern.canonicalUtterance(ordinal);
            break;
        }
        case PhraseMatch::Suffix: {
            // An absolute path inside the case's scratch directory. `media.import`
            // needs the file to exist; `timeline.export` needs a writable parent.
            const fs::path path = pattern.toolName == "media.import"
                                      ? scratch.existingFile("import")
                                      : scratch.file("export", ".mp4");
            drawn.canonical = pattern.canonicalUtterance(1, path.string());
            break;
        }
    }
    return drawn;
}

/// Whitespace runs the perturbations insert. Every member of the whitespace set
/// the interpreter documents appears here.
const std::vector<std::string>& whitespaceRuns() {
    static const std::vector<std::string> runs = {
        "", " ", "  ", "\t", "\t\t", "\n", "\r\n", " \t ", "\v", "\f", " \t\n\r ",
    };
    return runs;
}

/// Apply a drawn case map to `text`. Only the letters of the PHRASE are touched;
/// a captured argument keeps its bytes, because Requirement 11.3 makes *phrase*
/// matching case-insensitive and a filesystem path is case-sensitive data, not
/// part of the phrase.
[[nodiscard]] std::string applyCaseMap(std::string_view text, int map) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        switch (map) {
            case 0: out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c)))); break;
            case 1: out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c)))); break;
            case 2:  // alternating
                out.push_back(i % 2 == 0
                                  ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                                  : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                break;
            default:  // randomly per character
                out.push_back(*rc::gen::arbitrary<bool>()
                                  ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                                  : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                break;
        }
    }
    return out;
}

/// A real case- and whitespace-perturbed rendering of `drawn.canonical`: the first
/// `prefixWords` words (the phrase) get a drawn case map, every gap between those
/// words gets a drawn whitespace run of at least one character, and leading and
/// trailing whitespace runs are drawn independently. Any captured argument that
/// follows is appended verbatim.
[[nodiscard]] std::string perturb(const DrawnPhrase& drawn) {
    // Split the canonical utterance into words (it is single-spaced by
    // construction, so splitting on ' ' is exact).
    std::vector<std::string> words;
    std::string              current;
    for (const char c : drawn.canonical) {
        if (c == ' ') {
            words.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    words.push_back(current);

    const std::vector<std::string>& runs = whitespaceRuns();
    const int                       caseMap = static_cast<int>(drawIndex(4));

    // A non-empty interior separator: collapsing it must not join two words.
    const auto interior = [&runs]() -> std::string {
        std::string run = runs[1 + drawIndex(runs.size() - 1)];
        return run.empty() ? std::string(" ") : run;
    };

    std::string out = runs[drawIndex(runs.size())];  // leading run, possibly empty
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0) out += interior();
        // The phrase words are case-perturbed; a trailing argument is not.
        out += (i < drawn.prefixWords) ? applyCaseMap(words[i], caseMap) : words[i];
    }
    out += runs[drawIndex(runs.size())];  // trailing run, possibly empty
    return out;
}

/// True when `text` is one of the documented phrases (in any case/whitespace
/// rendering) — used to keep Property 61's generator strictly outside the mapping.
[[nodiscard]] bool mapsToSomething(const OfflineIntentInterpreter& interpreter,
                                  std::string_view text) {
    return interpreter.interpret(text).isOk();
}

/// Printable-ASCII text of `length` characters, biased towards words so a
/// generated string reads like a plausible request rather than line noise.
[[nodiscard]] std::string drawText(std::size_t length) {
    static const char kAlphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,/-_@";
    constexpr std::size_t kAlphabetSize = sizeof(kAlphabet) - 1;
    std::string           out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        out.push_back(kAlphabet[drawIndex(kAlphabetSize)]);
    }
    return out;
}

/// Near misses of the documented phrases: the shapes most likely to be wrongly
/// accepted, so the generator does not spend all its cases on obvious noise.
[[nodiscard]] std::string drawNearMiss() {
    const std::vector<PhrasePattern>& table = OfflineIntentInterpreter::patterns();
    const PhrasePattern&              pattern = table[drawIndex(table.size())];
    const std::string                 phrase = pattern.canonicalUtterance(2, "/tmp/x.mp4");

    switch (drawIndex(7)) {
        case 0: return phrase + " please";                       // trailing extra word
        case 1: return "please " + phrase;                       // leading extra word
        case 2: return phrase.substr(0, phrase.size() / 2);      // truncated
        case 3: return phrase + "s";                             // pluralised
        case 4: return std::string(pattern.text) + " zzz";       // wrong argument shape
        case 5: return "do not " + phrase;                       // negated
        default: return phrase + " and " + phrase;               // doubled
    }
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 59: The offline interpreter is
// case- and whitespace-insensitive — for any documented command phrase and any
// combination of letter-case changes and leading or trailing whitespace applied to
// it, the offline interpreter returns the same tool invocation, within 1 second,
// and issues no network request.
//
// Requirement 11.3: "... matching phrases without regard to letter case or leading
// and trailing whitespace, returning a mapping within 1 second, and issuing no
// network request."
//
// The generator is the documented phrase table x case maps (lower, upper,
// alternating, per-character random) x leading, interior and trailing whitespace
// runs drawn from the whole whitespace set including tabs, newlines, carriage
// returns, vertical tabs and form feeds. The canonical form is interpreted once as
// the reference, and every perturbation must agree with it exactly — same tool
// name, same argument object.
//
// **Validates: Requirements 11.3**
// ===========================================================================
RC_GTEST_PROP(OfflineInterpreterProperties, CaseAndWhitespaceInsensitive, ()) {
    ScratchDir scratch;
    Stack      stack(scratch);
    const OfflineIntentInterpreter interpreter = stack.interpreter();

    const DrawnPhrase drawn = drawPhrase(scratch, stack.fixture());

    // The reference: the canonical form, which by construction is already lowercase
    // and single-spaced.
    const Interpretation reference = interpretGuarded(interpreter, drawn.canonical);
    RC_ASSERT(reference.outcome.isOk());
    RC_ASSERT(reference.socketCalls == 0u);
    RC_ASSERT(reference.outcome.value().toolName == std::string(drawn.pattern.toolName));

    // Several perturbations per case, so one case covers a spread of renderings.
    const int perturbations = 1 + static_cast<int>(drawIndex(4));
    for (int i = 0; i < perturbations; ++i) {
        const std::string    variant = perturb(drawn);
        const Interpretation observed = interpretGuarded(interpreter, variant);

        RC_ASSERT_FALSE(observed.outcome.isError());

        // Same invocation, argument for argument.
        RC_ASSERT(observed.outcome.value().toolName == reference.outcome.value().toolName);
        RC_ASSERT(observed.outcome.value().arguments.dump() ==
                  reference.outcome.value().arguments.dump());

        // Well inside the 1-second bound, and no socket was touched.
        RC_ASSERT(observed.elapsed < 100ms);
        RC_ASSERT(observed.socketCalls == 0u);

        // And the normalization really did change the input: a perturbation that
        // happened to reproduce the canonical form proves nothing, so at least
        // assert the two are related by normalization.
        RC_ASSERT(OfflineIntentInterpreter::normalize(variant) ==
                  OfflineIntentInterpreter::normalize(drawn.canonical) ||
                  drawn.pattern.match == PhraseMatch::Suffix);
    }
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 60: Interpreter output is
// always executable — for any utterance the active interpreter maps successfully,
// the returned tool name exists in the Tool_Surface and the returned argument
// object satisfies that tool's declared input schema.
//
// Requirement 11.2: "... SHALL return ... exactly one tool name that exists in the
// Tool_Surface together with an argument object that satisfies that tool's
// declared input schema."
//
// "Executable" is taken at its word, in three escalating steps against the REAL
// surface built by `buildDefaultToolRegistry` over a REAL `ProjectSession`:
//
//   1. the tool is registered — `ToolRegistry::find` returns it;
//   2. the arguments satisfy the ADVERTISED schema — `ToolSchema::validate`, the
//      same declaration `tools/list` publishes and the executor enforces;
//   3. the invocation actually RUNS — `McpToolExecutor::executeTool` under the full
//      policy (validate, execute, roll back on failure) returns a success payload.
//
// Step 3 is what makes the property bite: an invocation whose arguments are
// well-formed JSON but semantically inapplicable (a playhead outside the clip, an
// unknown track id, a relative export path) fails here.
//
// **Validates: Requirements 11.2**
// ===========================================================================
RC_GTEST_PROP(OfflineInterpreterProperties, InterpreterOutputIsAlwaysExecutable, ()) {
    ScratchDir scratch;
    Stack      stack(scratch);
    const OfflineIntentInterpreter interpreter = stack.interpreter();

    // Utterances drawn from the documented phrase space with generated numeric and
    // path arguments, in a drawn case/whitespace rendering — so this property is
    // checked over the same input space Property 59 covers.
    const DrawnPhrase drawn = drawPhrase(scratch, stack.fixture());
    const std::string utterance =
        *rc::gen::arbitrary<bool>() ? perturb(drawn) : drawn.canonical;

    const Interpretation interpreted = interpretGuarded(interpreter, utterance);
    RC_ASSERT(interpreted.socketCalls == 0u);
    RC_ASSERT(interpreted.elapsed < 100ms);
    RC_ASSERT(interpreted.outcome.isOk());

    const AgentIntent& intent = interpreted.outcome.value();

    // 1. The tool name exists in the Tool_Surface.
    const Tool* tool = stack.registry().find(intent.toolName);
    RC_ASSERT(tool != nullptr);

    // 2. The argument object satisfies that tool's DECLARED input schema — the very
    //    one `tools/list` advertises.
    const Result<void> validated = tool->schema.validate(intent.arguments);
    if (validated.isError()) {
        RC_FAIL("the interpreter produced arguments its own tool's schema rejects: tool '" +
                intent.toolName + "' args " + intent.arguments.dump() + " -> " +
                validated.error().toString());
    }

    // 3. And it executes, through the same policy the MCP endpoint and the GUI use.
    const Result<Json> executed =
        stack.executor().executeTool(intent.toolName, intent.arguments,
                                     InvocationSource::Agent);
    if (executed.isError()) {
        RC_FAIL("the interpreter produced an invocation the tool surface could not "
                "execute: utterance \"" + utterance + "\" -> tool '" + intent.toolName +
                "' args " + intent.arguments.dump() + " -> " + executed.error().toString());
    }
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 61: Unmappable utterances
// change nothing — for any utterance outside the interpreter's mapping, the
// response quotes the unrecognised request, no tool is invoked, and the project
// state and undo-history depth are unchanged.
//
// Requirement 11.4: "IF an utterance cannot be mapped to a tool in the
// Tool_Surface, THEN THE Agent_Interpreter SHALL return a message that quotes the
// unrecognised request, SHALL invoke no tool, and SHALL leave the project state
// and the undo history depth unchanged."
//
// The generator is random strings of length 1-2000 filtered to exclude anything the
// table maps, plus deliberate near misses (extra words, truncations, pluralisations,
// negations, doubled phrases) — the shapes a prefix-matching table is most likely
// to accept by accident. "No tool is invoked" is checked structurally: the project
// as `timeline.read` renders it, the undo and redo depths and the media-library
// count are captured before and compared after, so any tool that had run would show
// up.
//
// **Validates: Requirements 11.4**
// ===========================================================================
RC_GTEST_PROP(OfflineInterpreterProperties, UnmappableUtterancesChangeNothing, ()) {
    ScratchDir scratch;
    Stack      stack(scratch);
    const OfflineIntentInterpreter interpreter = stack.interpreter();

    // Half the cases are near misses, half are free-form strings across the whole
    // permitted length range.
    std::string utterance;
    if (*rc::gen::arbitrary<bool>()) {
        utterance = drawNearMiss();
    } else {
        const std::size_t length = 1 + drawIndex(kMaxUtteranceChars);
        utterance = drawText(length);
    }

    // Strictly outside the mapping, and inside the length bounds (so the refusal
    // under test is the unmappable one, not the length one).
    RC_PRE(!utterance.empty() && utterance.size() <= kMaxUtteranceChars);
    RC_PRE(!OfflineIntentInterpreter::normalize(utterance).empty());
    RC_PRE(!mapsToSomething(interpreter, utterance));

    const std::string before = stack.stateFingerprint();

    const Interpretation observed = interpretGuarded(interpreter, utterance);
    RC_ASSERT(observed.socketCalls == 0u);
    RC_ASSERT(observed.elapsed < 100ms);

    // Refused, and the message QUOTES the unrecognised request verbatim.
    RC_ASSERT(observed.outcome.isError());
    const std::string message = observed.outcome.error().message();
    RC_ASSERT(message.find("\"" + utterance + "\"") != std::string::npos);

    // No tool ran: the project, both history depths and the media library are
    // exactly as they were.
    RC_ASSERT(stack.stateFingerprint() == before);
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 64: Utterance length bounds are
// enforced — for any utterance that is empty after whitespace removal or longer
// than 2000 characters, the interpreter rejects it with an error stating the
// permitted 1-2000 character range and invokes no tool.
//
// Requirement 11.9: "IF a submitted utterance is empty after whitespace removal or
// exceeds 2000 characters, THEN THE Agent_Interpreter SHALL reject it with an error
// stating the permitted length range and SHALL invoke no tool."
//
// The generator covers both arms exactly as design.md specifies: whitespace-only
// strings over the whitespace character set, and lengths 0, 1, 1999, 2000, 2001 and
// 5000. The two in-range lengths are included as the CONTROL arm — they must not be
// rejected for length — so the property pins the boundary rather than just the
// far side of it.
//
// **Validates: Requirements 11.9**
// ===========================================================================
RC_GTEST_PROP(OfflineInterpreterProperties, UtteranceLengthBoundsAreEnforced, ()) {
    ScratchDir scratch;
    Stack      stack(scratch);
    const OfflineIntentInterpreter interpreter = stack.interpreter();

    const std::string before = stack.stateFingerprint();
    const std::string range = utteranceRangeText();

    // --- Arm 1: empty after whitespace removal -----------------------------
    if (*rc::gen::arbitrary<bool>()) {
        static const char kWhitespace[] = {' ', '\t', '\n', '\r', '\v', '\f'};
        const std::size_t length = drawIndex(24);  // 0 (truly empty) up to 23
        std::string       blank;
        blank.reserve(length);
        for (std::size_t i = 0; i < length; ++i) {
            blank.push_back(kWhitespace[drawIndex(sizeof(kWhitespace))]);
        }

        const Interpretation observed = interpretGuarded(interpreter, blank);
        RC_ASSERT(observed.socketCalls == 0u);
        RC_ASSERT(observed.outcome.isError());
        RC_ASSERT(observed.outcome.error().code() == ErrorCode::InvalidArgument);
        // The message states the permitted range.
        RC_ASSERT(observed.outcome.error().message().find(range) != std::string::npos);
        RC_ASSERT(stack.stateFingerprint() == before);
        return;
    }

    // --- Arm 2: the boundary lengths ---------------------------------------
    static constexpr std::size_t kLengths[] = {0, 1, 1999, 2000, 2001, 5000};
    const std::size_t            length = kLengths[drawIndex(std::size(kLengths))];

    // Non-whitespace filler, so length is the ONLY thing under test on this arm.
    const std::string utterance(length, 'x');

    const Interpretation observed = interpretGuarded(interpreter, utterance);
    RC_ASSERT(observed.socketCalls == 0u);
    RC_ASSERT(observed.elapsed < 100ms);
    RC_ASSERT(observed.outcome.isError());  // 'xxxx' maps to nothing either way

    if (length == 0 || length > kMaxUtteranceChars) {
        // Rejected FOR LENGTH: the message names the permitted range.
        RC_ASSERT(observed.outcome.error().code() == ErrorCode::InvalidArgument);
        RC_ASSERT(observed.outcome.error().message().find(range) != std::string::npos);
    } else {
        // In range (1, 1999, 2000): the refusal is the unmappable one, which quotes
        // the request instead of naming a length range. This is the control that
        // keeps the boundary exact — a bound off by one would show up here.
        RC_ASSERT(observed.outcome.error().message().find("does not match") !=
                  std::string::npos);
    }

    // Either way no tool ran.
    RC_ASSERT(stack.stateFingerprint() == before);
}

// ---------------------------------------------------------------------------
// Unit tests: the specific examples and edge cases the properties quantify over
// but do not pin by name.
// ---------------------------------------------------------------------------

TEST(OfflineInterpreter, TheDocumentedTableHasTwelvePatternsResolvingToRealTools) {
    ScratchDir scratch;
    Stack      stack(scratch);

    const std::vector<PhrasePattern>& table = OfflineIntentInterpreter::patterns();
    ASSERT_GE(table.size(), 12u) << "Requirement 11.3 asks for at least 10; design.md D9 "
                                   "fixes the set at 12";
    EXPECT_EQ(table.size(), 12u);

    for (const PhrasePattern& pattern : table) {
        EXPECT_NE(stack.registry().find(pattern.toolName), nullptr)
            << "phrase '" << pattern.display() << "' resolves to tool '" << pattern.toolName
            << "', which is not in the Tool_Surface";
        EXPECT_FALSE(pattern.summary.empty()) << pattern.display();
    }
}

TEST(OfflineInterpreter, EachDocumentedPhraseMapsToItsSpecifiedTool) {
    ScratchDir scratch;
    Stack      stack(scratch);
    const OfflineIntentInterpreter interpreter = stack.interpreter();

    struct Expectation {
        const char* utterance;
        const char* tool;
    };
    const Expectation expected[] = {
        {"split the clip at the playhead", "timeline.split_clip"},
        {"mute track 2", "timeline.set_track_muted"},
        {"unmute track 2", "timeline.set_track_muted"},
        {"add a video track", "timeline.add_track"},
        {"add an audio track", "timeline.add_track"},
        {"delete the selected clip", "timeline.delete_clip"},
        {"import /tmp/palmier-example.mp4", "media.import"},
        {"export as mp4 to /tmp/palmier-example-out.mp4", "timeline.export"},
        {"save the project", "project.save"},
        {"undo", "edit.undo"},
        {"redo", "edit.redo"},
        {"show the timeline", "timeline.read"},
    };

    for (const Expectation& e : expected) {
        const Result<AgentIntent> intent = interpreter.interpret(e.utterance);
        ASSERT_TRUE(intent.isOk()) << e.utterance << ": " << intent.error().toString();
        EXPECT_EQ(intent.value().toolName, e.tool) << e.utterance;
    }

    // The two track phrases differ only in the flag they set, and the ordinal is
    // resolved to the identifier of the Nth track in project order.
    const Result<AgentIntent> muted = interpreter.interpret("MUTE  Track\t3");
    ASSERT_TRUE(muted.isOk());
    EXPECT_TRUE(muted.value().arguments.boolOr("muted"));
    EXPECT_EQ(muted.value().arguments.stringOr("trackId"),
              stack.fixture().trackIds[2].toString());

    const Result<AgentIntent> unmuted = interpreter.interpret("unmute track 1");
    ASSERT_TRUE(unmuted.isOk());
    EXPECT_FALSE(unmuted.value().arguments.boolOr("muted", true));
    EXPECT_EQ(unmuted.value().arguments.stringOr("trackId"),
              stack.fixture().trackIds[0].toString());

    // The two add-track phrases differ only in the kind they request.
    EXPECT_EQ(interpreter.interpret("add a video track").value().arguments.stringOr("kind"),
              "video");
    EXPECT_EQ(interpreter.interpret("add an audio track").value().arguments.stringOr("kind"),
              "audio");

    // A captured path keeps its letter case even though the phrase is matched
    // case-insensitively.
    const Result<AgentIntent> imported = interpreter.interpret("IMPORT /tmp/MixedCase.MP4");
    ASSERT_TRUE(imported.isOk());
    EXPECT_EQ(imported.value().arguments.stringOr("path"), "/tmp/MixedCase.MP4");

    // `export as mp4 to` fixes the container.
    const Result<AgentIntent> exported =
        interpreter.interpret("export as mp4 to /tmp/Out.MP4");
    ASSERT_TRUE(exported.isOk());
    EXPECT_EQ(exported.value().arguments.stringOr("outputPath"), "/tmp/Out.MP4");
    EXPECT_EQ(exported.value().arguments.stringOr("format"), "mp4");
}

// Property 60 draws from the phrase space, so over 100 cases it is very likely to
// reach every pattern but not certain to. This is the deterministic companion: all
// twelve patterns, each on its OWN fresh stack (so `delete the selected clip` in
// one case cannot make `split the clip at the playhead` inapplicable in the next),
// each executed through the real executor against a real session.
TEST(OfflineInterpreter, EveryDocumentedPhraseExecutesAgainstTheRealToolSurface) {
    const std::vector<PhrasePattern>& table = OfflineIntentInterpreter::patterns();
    ASSERT_EQ(table.size(), 12u);

    for (const PhrasePattern& pattern : table) {
        ScratchDir scratch;
        Stack      stack(scratch);

        std::string utterance;
        switch (pattern.match) {
            case PhraseMatch::Exact:
                utterance = pattern.canonicalUtterance();
                break;
            case PhraseMatch::Ordinal:
                utterance = pattern.canonicalUtterance(2);
                break;
            case PhraseMatch::Suffix:
                utterance = pattern.canonicalUtterance(
                    1, (pattern.toolName == "media.import"
                            ? scratch.existingFile("import")
                            : scratch.file("export", ".mp4"))
                           .string());
                break;
        }

        const Result<AgentIntent> intent = stack.interpreter().interpret(utterance);
        ASSERT_TRUE(intent.isOk()) << utterance << ": " << intent.error().toString();
        EXPECT_EQ(intent.value().toolName, pattern.toolName) << utterance;

        const Tool* tool = stack.registry().find(intent.value().toolName);
        ASSERT_NE(tool, nullptr) << intent.value().toolName;
        ASSERT_TRUE(tool->schema.validate(intent.value().arguments).isOk())
            << utterance << " -> " << intent.value().arguments.dump();

        const Result<Json> executed = stack.executor().executeTool(
            intent.value().toolName, intent.value().arguments, InvocationSource::Agent);
        EXPECT_TRUE(executed.isOk())
            << utterance << " -> " << intent.value().toolName << " "
            << intent.value().arguments.dump() << ": " << executed.error().toString();
    }
}

TEST(OfflineInterpreter, AContextThatCannotAnswerNamesTheMissingFact) {
    // No provider at all: nothing is selected, there are no tracks and there is no
    // document path. Each phrase that needs a fact says which one is missing, and
    // none of them produces an invocation.
    const OfflineIntentInterpreter bare;

    const Result<AgentIntent> split = bare.interpret("split the clip at the playhead");
    ASSERT_TRUE(split.isError());
    EXPECT_EQ(split.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_NE(split.error().message().find("no clip is selected"), std::string::npos);

    const Result<AgentIntent> deleted = bare.interpret("delete the selected clip");
    ASSERT_TRUE(deleted.isError());
    EXPECT_NE(deleted.error().message().find("no clip is selected"), std::string::npos);

    const Result<AgentIntent> muted = bare.interpret("mute track 4");
    ASSERT_TRUE(muted.isError());
    EXPECT_EQ(muted.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_NE(muted.error().message().find("track 4"), std::string::npos);
    EXPECT_NE(muted.error().message().find("0 track"), std::string::npos);

    // `save the project` still maps: omitting `path` is what tells `project.save`
    // to use the project's own recorded document path.
    const Result<AgentIntent> saved = bare.interpret("save the project");
    ASSERT_TRUE(saved.isOk());
    EXPECT_FALSE(saved.value().arguments.contains("path"));
}

// The no-network assertions above are only worth anything if the guard can
// actually see a socket call. This proves it is not vacuous: inside the same
// armed window the properties use, a real socket() lands in the interposer and is
// counted, and outside it nothing is.
TEST(OfflineInterpreter, TheNetworkGuardDetectsARealSocketCall) {
    {
        NetworkWatch watch;
        const int    fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_EQ(watch.calls(), 1u) << "the socket interposer was bypassed, so every "
                                       "no-network assertion in this file would be vacuous";
        if (fd >= 0) ::close(fd);
    }

    // Disarmed: the interposer is still in the path but records nothing, so an
    // unrelated socket user elsewhere in the process is unaffected.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0) << "the interposer must forward to the real socket()";
    if (fd >= 0) ::close(fd);
    {
        NetworkWatch watch;
        EXPECT_EQ(watch.calls(), 0u);
    }
}

TEST(OfflineInterpreter, NormalizationIsTrimLowercaseAndCollapse) {
    EXPECT_EQ(OfflineIntentInterpreter::normalize("  UNDO  "), "undo");
    EXPECT_EQ(OfflineIntentInterpreter::normalize("\t\nMuTe\r\v  TrAcK\f 2 \t"), "mute track 2");
    EXPECT_EQ(OfflineIntentInterpreter::normalize("   "), "");
    EXPECT_EQ(OfflineIntentInterpreter::normalize(""), "");
}

// ---------------------------------------------------------------------------
// The interpreter registry (task 10.1; Requirements 11.1, 11.8).
// ---------------------------------------------------------------------------

TEST(AgentInterpreterRegistry, OfflineIsTheDefaultAndTheFallback) {
    EXPECT_EQ(defaultAgentInterpreterId(), "offline");
    ASSERT_EQ(agentInterpreterIds().size(), 3u);
    EXPECT_EQ(agentInterpreterIds()[0], "offline");
    EXPECT_TRUE(isAgentInterpreterId("offline"));
    EXPECT_TRUE(isAgentInterpreterId("hosted"));
    EXPECT_TRUE(isAgentInterpreterId("byok"));
    EXPECT_FALSE(isAgentInterpreterId("gpt-9"));

    // An empty id means the default, and it is installed without a diagnostic.
    const AgentInterpreterSelection defaulted = selectAgentInterpreter({});
    EXPECT_EQ(defaulted.id, "offline");
    EXPECT_TRUE(defaulted.startupError.empty());
    ASSERT_TRUE(static_cast<bool>(defaulted.interpreter));
    EXPECT_TRUE(defaulted.interpreter("undo").isOk());
}

TEST(AgentInterpreterRegistry, AnUnknownIdInstallsOfflineAndNamesTheRejectedId) {
    AgentInterpreterRequest request;
    request.id = "totally-made-up";

    const AgentInterpreterSelection selection = selectAgentInterpreter(request);
    EXPECT_EQ(selection.id, "offline");
    EXPECT_TRUE(selection.fellBack());
    EXPECT_NE(selection.startupError.find("totally-made-up"), std::string::npos);
    EXPECT_NE(selection.startupError.find("no interpreter in the registry"),
              std::string::npos);
    // And the installed interpreter is a working one, not an inert stub.
    EXPECT_TRUE(selection.interpreter("show the timeline").isOk());
}

TEST(AgentInterpreterRegistry, CredentiallessHostedAndByokFallBackNamingTheUnmetRequirement) {
    for (const std::string id : {std::string("hosted"), std::string("byok")}) {
        AgentInterpreterRequest request;
        request.id = id;
        // No credentials probe at all: the Offline_Mode state.
        const AgentInterpreterSelection selection = selectAgentInterpreter(request);
        EXPECT_EQ(selection.id, "offline") << id;
        EXPECT_NE(selection.startupError.find(id), std::string::npos) << id;
        EXPECT_NE(selection.startupError.find("requires"), std::string::npos) << id;
        EXPECT_TRUE(selection.interpreter("redo").isOk()) << id;
    }
}

TEST(AgentInterpreterRegistry, AnAuthorizedBackendReportsItsFailureWithoutTouchingTheProject) {
    ScratchDir scratch;
    Stack      stack(scratch);
    const std::string before = stack.stateFingerprint();

    AgentInterpreterRequest request;
    request.id = "hosted";
    request.credentials = [](std::string_view id) { return id == "hosted"; };

    const AgentInterpreterSelection selection = selectAgentInterpreter(request);
    EXPECT_EQ(selection.id, "hosted");
    EXPECT_TRUE(selection.startupError.empty());

    // Requirement 11.8: a backend failure is reported by name, no tool is invoked
    // and the project is unchanged.
    const Result<AgentIntent> answered = selection.interpreter("undo");
    ASSERT_TRUE(answered.isError());
    EXPECT_NE(answered.error().message().find("hosted"), std::string::npos);
    EXPECT_NE(answered.error().message().find("backend failure"), std::string::npos);
    EXPECT_NE(answered.error().message().find("no tool was invoked"), std::string::npos);
    EXPECT_EQ(stack.stateFingerprint(), before);
}

}  // namespace
}  // namespace palmier::services
