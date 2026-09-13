// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared fixtures and helpers for the project_tools_property_test cases, split out so that no single file
// exceeds this repository's 1000-line ceiling.
//
// The support namespace opens INSIDE the original enclosing namespace, because these
// helpers name the types they use unqualified. `inline` and named rather than an
// anonymous namespace: an anonymous namespace in a header gives every translation unit
// its own copy, and a helper only some partitions use would warn as unused.

#ifndef PALMIER_TESTS_SERVICES_PROJECT_TOOLS_PROPERTY_TEST_SUPPORT_HPP_
#define PALMIER_TESTS_SERVICES_PROJECT_TOOLS_PROPERTY_TEST_SUPPORT_HPP_

#include "services/ToolRegistry.hpp"
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <unistd.h>  // getpid, for a per-process scratch directory name
#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/SchemaVersion.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/ProjectSession.hpp"
#include "services/ProjectStore.hpp"
#include "services/ToolSchema.hpp"

namespace palmier::services {
namespace testsupport_projecttoolspropertytest {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Scratch directory
// ---------------------------------------------------------------------------

/// A per-case directory under the OS temp directory, removed when the case ends.
/// The name carries the process id as well as a counter because CTest runs this
/// binary once per discovered case, in parallel processes.
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<std::uint64_t> counter{0};
        root_ = fs::temp_directory_path() / ("palmier_project_tools_prop_" +
                                             std::to_string(static_cast<long long>(::getpid())) +
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

    /// A fresh ABSOLUTE path inside this directory. Tools that write are only ever
    /// given one of these, so nothing lands in the ctest working directory.
    [[nodiscard]] fs::path file(std::string_view tag) const {
        static std::atomic<std::uint64_t> counter{0};
        return root_ / (std::string(tag) + "_" + std::to_string(counter.fetch_add(1)) + ".palmier");
    }

    /// True iff nothing was created inside this directory.
    [[nodiscard]] bool empty() const {
        std::error_code ec;
        const bool isEmpty = fs::is_empty(root_, ec);
        return !ec && isEmpty;
    }

private:
    fs::path root_;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

[[nodiscard]] inline bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

[[nodiscard]] inline std::size_t clipCount(const Project& project) {
    std::size_t count = 0;
    for (const Track& track : project.tracks) count += track.clips.size();
    return count;
}

/// The document bytes of the session's current project: the "byte-identical"
/// yardstick Properties 13 and 14 compare against.
[[nodiscard]] inline std::string projectBytes(const ProjectSession& session) {
    return serializeProject(session.engine().snapshot());
}

/// Everything a failed call must leave untouched, in one comparable value.
struct SessionFingerprint {
    std::string bytes;
    std::optional<fs::path> documentPath;
    bool modified = false;
    std::size_t undoDepth = 0;

    [[nodiscard]] bool operator==(const SessionFingerprint& other) const = default;
};

[[nodiscard]] inline SessionFingerprint fingerprint(const ProjectSession& session) {
    return SessionFingerprint{projectBytes(session), session.documentPath(), session.modified(),
                              session.engine().undoDepth()};
}

[[nodiscard]] inline std::size_t drawIndex(std::size_t count) {
    return *rc::gen::inRange<std::size_t>(0, count);
}

[[nodiscard]] inline std::string drawAsciiText(std::size_t length) {
    static const std::string alphabet = "abcdeXYZ 0189_-.'";
    std::string text;
    text.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        text.push_back(alphabet[drawIndex(alphabet.size())]);
    }
    return text;
}

/// The seven colour spaces the domain core exposes, spelled the way the tool
/// surface publishes them (`ColorSpace::toStringView`).
inline const std::vector<ColorSpace>& selectableColorSpaces() {
    static const std::vector<ColorSpace> spaces = {
        ColorSpace::Srgb,       ColorSpace::Rec709,    ColorSpace::Rec2020,   ColorSpace::Rec2100Pq,
        ColorSpace::Rec2100Hlg, ColorSpace::DisplayP3, ColorSpace::LinearSrgb};
    return spaces;
}

// ---------------------------------------------------------------------------
// project.create arguments
// ---------------------------------------------------------------------------

/// One `project.create` argument tuple, plus the settings it asks for so the
/// resulting project can be compared against the request.
struct CreateRequest {
    std::string name;
    double fps = 30.0;
    std::int64_t width = 1920;
    std::int64_t height = 1080;
    std::optional<ColorSpace> colorSpace;  ///< std::nullopt ⇒ argument omitted.

    [[nodiscard]] Json args() const {
        Json out = Json::object();
        out.set("name", Json(name));
        out.set("fps", Json(fps));
        out.set("width", Json(width));
        out.set("height", Json(height));
        if (colorSpace.has_value()) {
            out.set("colorSpace", Json(std::string(toStringView(*colorSpace))));
        }
        return out;
    }

    /// The colour space the created project must carry: the requested one, or the
    /// documented default when the argument was omitted.
    [[nodiscard]] ColorSpace expectedColorSpace() const {
        return colorSpace.value_or(kDefaultProjectColorSpace);
    }
};

/// A name across the accepted 1-128 character band, including both bounds and
/// non-ASCII text (the bound is on bytes, so the multi-byte case stays short).
[[nodiscard]] inline std::string drawProjectName() {
    switch (*rc::gen::inRange(0, 5)) {
        case 0:
            return drawAsciiText(kMinProjectNameLength);  // 1 character
        case 1:
            return drawAsciiText(kMaxProjectNameLength);  // 128 characters
        case 2: {
            static const std::vector<std::string> pieces = {"é",      "ü",  "日本語",
                                                            "Привет", "🎬", "λ"};
            std::string text;
            const int count = *rc::gen::inRange(1, 5);
            for (int i = 0; i < count; ++i) text += pieces[drawIndex(pieces.size())];
            return text;
        }
        default:
            return drawAsciiText(*rc::gen::inRange<std::size_t>(kMinProjectNameLength, 41));
    }
}

/// A frame rate inside the accepted 1-240 fps band: whole rates (including both
/// bounds), the three broadcast pull-down rates, and rates carrying a fraction.
[[nodiscard]] inline double drawFps() {
    switch (*rc::gen::inRange(0, 6)) {
        case 0:
            return static_cast<double>(kMinFramesPerSecond);  // 1 fps
        case 1:
            return static_cast<double>(kMaxFramesPerSecond);  // 240 fps
        case 2:
            return *rc::gen::element<double>(24000.0 / 1001.0, 30000.0 / 1001.0, 60000.0 / 1001.0);
        case 3:
            return static_cast<double>(*rc::gen::inRange<std::int64_t>(1'000, 240'001)) / 1'000.0;
        default:
            return static_cast<double>(
                *rc::gen::inRange<std::int64_t>(kMinFramesPerSecond, kMaxFramesPerSecond + 1));
    }
}

/// A canvas inside 16x16 … 7680x4320, including all four corners of that box.
[[nodiscard]] inline std::pair<std::int64_t, std::int64_t> drawCanvas() {
    switch (*rc::gen::inRange(0, 6)) {
        case 0:
            return {kMinCanvasWidth, kMinCanvasHeight};
        case 1:
            return {kMaxCanvasWidth, kMaxCanvasHeight};
        case 2:
            return {kMinCanvasWidth, kMaxCanvasHeight};
        case 3:
            return {kMaxCanvasWidth, kMinCanvasHeight};
        default:
            return {*rc::gen::inRange<std::int64_t>(kMinCanvasWidth, kMaxCanvasWidth + 1),
                    *rc::gen::inRange<std::int64_t>(kMinCanvasHeight, kMaxCanvasHeight + 1)};
    }
}

[[nodiscard]] inline CreateRequest drawCreateRequest() {
    CreateRequest request;
    request.name = drawProjectName();
    request.fps = drawFps();
    const auto [width, height] = drawCanvas();
    request.width = width;
    request.height = height;
    // Every colour-space key, plus the omitted case that must fall back to the
    // documented default.
    if (*rc::gen::inRange(0, 8) == 0) {
        request.colorSpace = std::nullopt;
    } else {
        request.colorSpace = selectableColorSpaces()[drawIndex(selectableColorSpaces().size())];
    }
    return request;
}

// ---------------------------------------------------------------------------
// Project generator (Property 10's subjects, and the fixtures Properties 12-14
// start from). Shapes stay inside the persistence bounds Requirement 4.7 states:
// 1-20 tracks, 0-200 clips per track, 0-200 registered assets.
// ---------------------------------------------------------------------------

struct Shape {
    int maxTracks = 5;
    int maxClipsPerTrack = 10;
    int clipBudget = 40;
    int maxAssets = 6;
};

/// Mostly small projects, with a deliberate minority at each stated bound.
[[nodiscard]] inline Shape drawShape() {
    Shape shape;
    switch (*rc::gen::inRange(0, 6)) {
        case 0:  // Maximal track count.
            shape = Shape{20, 3, 60, 4};
            break;
        case 1:  // One maximal-length track.
            shape = Shape{2, 200, 220, 3};
            break;
        case 2:  // Maximal asset table.
            shape = Shape{3, 6, 18, 200};
            break;
        default:
            break;
    }
    return shape;
}

/// A legal project: a supported schema version, a positive frame rate and canvas,
/// unique non-nil asset ids, and per-track clips ordered by timelineStart with no
/// overlap — exactly what `project.open` accepts.
[[nodiscard]] inline Project drawProject(const Shape& shape) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = drawAsciiText(*rc::gen::inRange<std::size_t>(1, 33));
    project.version = SchemaVersion::current();
    project.timelineFps =
        *rc::gen::element<FrameRate>(FrameRate::fps24(), FrameRate::fps25(), FrameRate::fps30(),
                                     FrameRate::fps29_97(), FrameRate::fps60());
    project.canvas = Resolution{static_cast<std::uint32_t>(*rc::gen::inRange(16, 3'841)),
                                static_cast<std::uint32_t>(*rc::gen::inRange(16, 2'161))};
    project.colorSpace = selectableColorSpaces()[drawIndex(selectableColorSpaces().size())];

    const int assetCount = *rc::gen::inRange(1, shape.maxAssets + 1);
    for (int i = 0; i < assetCount; ++i) {
        project.assets.emplace_back(
            Uuid::generateV4(), "/media/" + drawAsciiText(*rc::gen::inRange<std::size_t>(1, 12)));
    }

    int clipsRemaining = shape.clipBudget;
    const int trackCount = *rc::gen::inRange(1, shape.maxTracks + 1);
    for (int t = 0; t < trackCount; ++t) {
        Track track;
        track.id = Uuid::generateV4();
        track.kind = *rc::gen::element<TrackKind>(TrackKind::Video, TrackKind::Audio);

        const int wanted = *rc::gen::inRange(0, shape.maxClipsPerTrack + 1);
        const int count = std::min(wanted, std::max(clipsRemaining, 0));
        clipsRemaining -= count;

        std::int64_t cursorMs = *rc::gen::inRange<std::int64_t>(0, 1'001);
        for (int c = 0; c < count; ++c) {
            Clip clip;
            clip.id = Uuid::generateV4();
            clip.assetRef = project.assets[drawIndex(project.assets.size())];
            const std::int64_t inMs = *rc::gen::inRange<std::int64_t>(0, 10'001);
            const std::int64_t lengthMs = *rc::gen::inRange<std::int64_t>(20, 2'001);
            clip.sourceIn = Duration::fromMilliseconds(inMs);
            clip.sourceOut = Duration::fromMilliseconds(inMs + lengthMs);
            clip.timelineStart = Duration::fromMilliseconds(cursorMs);
            cursorMs += lengthMs + *rc::gen::inRange<std::int64_t>(1, 501);
            track.clips.push_back(std::move(clip));
        }
        project.tracks.push_back(std::move(track));
    }
    return project;
}

/// Make `project` the session's current project through the load path the tool
/// surface itself uses: a `.palmier` document plus `ProjectSession::openProject`.
[[nodiscard]] inline bool seedSession(ProjectSession& session, const Project& project,
                                      const fs::path& seedPath) {
    if (saveProjectToFile(project, seedPath).isError()) return false;
    return session.openProject(seedPath).isOk();
}

// ---------------------------------------------------------------------------
// Argument generation from a tool's own ToolSchema (Property 11). Iterating the
// registry rather than hard-coding tool names is deliberate: a tool added later
// is covered without touching this file.
// ---------------------------------------------------------------------------

[[nodiscard]] inline Json validValue(const ArgSpec& spec) {
    switch (spec.kind) {
        case JsonKind::Object: {
            Json object = Json::object();
            object.set("amount", Json(0.5));
            return object;
        }
        case JsonKind::Array: {
            Json items = Json::array();
            const std::size_t count =
                std::max<std::size_t>(spec.minLength.value_or(0), drawIndex(3));
            for (std::size_t i = 0; i < count; ++i) {
                items.push_back(Json(Uuid::generateV4().toString()));
            }
            return items;
        }
        case JsonKind::String: {
            if (!spec.enumValues.empty()) {
                return Json(spec.enumValues[drawIndex(spec.enumValues.size())]);
            }
            if (spec.uuid) return Json(Uuid::generateV4().toString());
            const std::size_t low = std::max<std::size_t>(spec.minLength.value_or(1), 1);
            return Json(drawAsciiText(low + drawIndex(6)));
        }
        case JsonKind::Integer: {
            const std::int64_t low = spec.minInt.value_or(0);
            const std::int64_t high = spec.maxInt.value_or(low + 1'000'000);
            return Json(low >= high ? low : *rc::gen::inRange<std::int64_t>(low, high));
        }
        case JsonKind::Number: {
            const double low = spec.minNum.value_or(static_cast<double>(spec.minInt.value_or(0)));
            const double high =
                spec.maxNum.value_or(static_cast<double>(spec.maxInt.value_or(0)) + 100.0);
            const double span = high > low ? high - low : 1.0;
            return Json(low + span * (static_cast<double>(drawIndex(101)) / 100.0));
        }
        case JsonKind::Bool:
            return Json(*rc::gen::arbitrary<bool>());
    }
    return Json(nullptr);
}

/// A value of a type the argument does not accept.
[[nodiscard]] inline Json wrongTypedValue(JsonKind kind) {
    if (kind == JsonKind::String) return Json(static_cast<std::int64_t>(7));
    if (kind == JsonKind::Bool) return Json("true");
    if (kind == JsonKind::Array || kind == JsonKind::Object) return Json("not-a-composite");
    return Json("not-a-number");
}

/// How a drawn argument object relates to the tool's declared schema. Every shape
/// is in scope: Requirement 3.5's refusal must not depend on the arguments.
enum class ArgMode {
    Empty,        ///< `{}` — every required argument missing.
    Valid,        ///< every required argument present and in range.
    WrongType,    ///< one argument of the wrong JSON type.
    OutOfBounds,  ///< one numeric/enum argument outside its declared bound.
    UnknownKey,   ///< a member the schema does not declare.
    Garbage,      ///< nothing the schema knows about at all.
};

/// Draw an argument object for `schema`. Any path-shaped argument is pointed at
/// an absolute path inside `scratch`, so a tool that ignored the refusal and
/// wrote something would leave observable evidence there and nowhere else.
[[nodiscard]] inline Json drawArgs(const ToolSchema& schema, ArgMode mode,
                                   const ScratchDir& scratch) {
    if (mode == ArgMode::Empty) return Json::object();
    if (mode == ArgMode::Garbage) {
        Json args = Json::object();
        args.set("nonsense", Json(drawAsciiText(1 + drawIndex(8))));
        args.set("count", Json(static_cast<std::int64_t>(*rc::gen::inRange(-10, 11))));
        return args;
    }

    Json args = Json::object();
    for (const ArgSpec& spec : schema.args()) {
        if (!spec.required && !*rc::gen::arbitrary<bool>()) continue;
        if (spec.kind == JsonKind::String && !spec.uuid && spec.enumValues.empty() &&
            (spec.name == "path" || spec.name == "outputPath")) {
            args.set(spec.name, Json(scratch.file("must_not_be_written").string()));
            continue;
        }
        args.set(spec.name, validValue(spec));
    }

    if (mode == ArgMode::UnknownKey) {
        args.set("definitelyNotAnArgument", Json(true));
        return args;
    }
    if (schema.size() == 0) return args;

    const ArgSpec& spec = schema.args()[drawIndex(schema.size())];
    if (mode == ArgMode::WrongType) {
        args.set(spec.name, wrongTypedValue(spec.kind));
    } else {  // OutOfBounds
        if (!spec.enumValues.empty()) {
            args.set(spec.name, Json("not-a-member-of-the-enum"));
        } else if (spec.uuid) {
            args.set(spec.name, Json("not-a-uuid"));
        } else if (spec.maxInt.has_value()) {
            args.set(spec.name, Json(*spec.maxInt + 1));
        } else if (spec.minInt.has_value()) {
            args.set(spec.name, Json(*spec.minInt - 1));
        } else if (spec.maxNum.has_value()) {
            args.set(spec.name, Json(*spec.maxNum + 1.0));
        } else if (spec.minNum.has_value()) {
            args.set(spec.name, Json(*spec.minNum - 1.0));
        } else if (spec.maxLength.has_value()) {
            args.set(spec.name, Json(drawAsciiText(*spec.maxLength + 1)));
        } else {
            args.set(spec.name, wrongTypedValue(spec.kind));
        }
    }
    return args;
}

}  // namespace testsupport_projecttoolspropertytest
}  // namespace palmier::services

#endif  // PALMIER_TESTS_SERVICES_PROJECT_TOOLS_PROPERTY_TEST_SUPPORT_HPP_
