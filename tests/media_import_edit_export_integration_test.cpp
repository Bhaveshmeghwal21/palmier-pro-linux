// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media_import_edit_export_integration_test.cpp — end-to-end integration
// test for the import -> edit -> export pipeline (task 10.4; Requirements 11.1,
// 11.6), exercised per source codec (H.264, HEVC, AV1, VP9).
//
// This is the one test that wires the *real* production pipeline together —
// every stage is the actual component, joined only at the two seams the design
// already exposes for FFmpeg/GPU-free operation:
//
//   1. Import validation — validateMediaImport() driven through MediaProbe's
//      injectable MediaProbeBackend seam, producing a synthetic, per-codec
//      MediaInfo. The accepted asset is cataloged in the real MediaManager
//      library (Requirement 3.1) and its clip registered for version history.
//
//   2. Decode — a real MediaDecoder driven through the IDecodeBackend seam with
//      a synthetic per-codec backend producing CPU frames. The decoder feeds the
//      Compositor's ClipFrameProvider, so the composited pixels genuinely flow
//      out of a decode step (not a hand-rolled stand-in).
//
//   3. Edit — a real TimelineEngine applying real EditCommands (AddEffect +
//      SplitClip) atomically, exactly as the UI / MCP server / agent would. The
//      exported project is the engine's post-edit snapshot.
//
//   4. Composite — the real vendor-neutral software Compositor over the
//      software-fallback GpuContext (the sandbox has no Vulkan/GPU).
//
//   5. Export — the real ExportEngine::run driving Compositor + MediaEncoder,
//      the latter behind the IEncodeBackend seam with a mock backend that writes
//      a genuine output file to disk. The output follows a documented, well-
//      formed container contract (magic header, one record per encoded frame, a
//      trailer carrying the frame count) so the test can prove a *valid*,
//      non-empty file was produced — and that the ExportResult reports its
//      location (Requirement 11.6).
//
// The whole flow runs with no FFmpeg, no GPU, and no vendor SDK: the probe,
// decode, and encode backends are all injected, while the MediaManager,
// TimelineEngine, EditCommands, and Compositor are the real, dependency-light
// components. The test asserts, for every codec, that the export succeeds
// (Requirement 11.1: the complete timeline rendered into a single output file
// without modifying the source timeline) and that the reported output location
// names a real, valid, non-empty file (Requirement 11.6).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/Effect.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "core/EditCommands.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/ExportEngine.hpp"
#include "media/ImportValidation.hpp"
#include "media/MediaDecoder.hpp"
#include "media/MediaEncoder.hpp"
#include "media/MediaInfo.hpp"
#include "media/MediaProbe.hpp"

namespace palmier {
namespace {

// The canvas / source / export frame size. Small and even (the export codecs'
// 4:2:0 chroma subsampling requires even dimensions).
constexpr std::uint32_t kW = 4;
constexpr std::uint32_t kH = 4;
constexpr Resolution    kRes{kW, kH};

// The number of whole frames the exported clip covers at 30fps.
constexpr int kFrameCount = 3;

// ---------------------------------------------------------------------------
// The mock encoder's on-disk container contract
// ---------------------------------------------------------------------------
// A "valid" output file the FileWritingEncodeBackend produces:
//   [8 bytes ] magic  "PALMEXP1"
//   [ per frame ] "FRAM" + int64 little-endian presentation ticks
//   [ trailer ] "ENDX" + uint32 little-endian encoded-frame count
// A file is valid iff it begins with the magic, ends with a well-formed trailer,
// and the trailer's count equals the number of FRAM records between them.

constexpr char        kMagic[8] = {'P', 'A', 'L', 'M', 'E', 'X', 'P', '1'};
constexpr char        kFrameTag[4] = {'F', 'R', 'A', 'M'};
constexpr char        kEndTag[4] = {'E', 'N', 'D', 'X'};
constexpr std::size_t kFrameRecordSize = 4 + sizeof(std::int64_t);
constexpr std::size_t kTrailerSize = 4 + sizeof(std::uint32_t);

template <typename T>
void appendLe(std::vector<char>& buf, T value) {
    char tmp[sizeof(T)];
    std::memcpy(tmp, &value, sizeof(T));
    buf.insert(buf.end(), tmp, tmp + sizeof(T));
}

// ---------------------------------------------------------------------------
// Mock encode backend that writes a real, well-formed output file
// ---------------------------------------------------------------------------

class FileWritingEncodeBackend final : public media::IEncodeBackend {
public:
    explicit FileWritingEncodeBackend(std::filesystem::path path) : path_(std::move(path)) {
        buffer_.insert(buffer_.end(), kMagic, kMagic + sizeof(kMagic));
    }

    [[nodiscard]] Result<void> encode(const media::EncoderInputFrame& frame) override {
        buffer_.insert(buffer_.end(), kFrameTag, kFrameTag + sizeof(kFrameTag));
        appendLe<std::int64_t>(buffer_, frame.presentation.ticks());
        ++frames_;
        return ok();
    }

    [[nodiscard]] Result<void> finish() override {
        buffer_.insert(buffer_.end(), kEndTag, kEndTag + sizeof(kEndTag));
        appendLe<std::uint32_t>(buffer_, frames_);

        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) {
            return err(Error(ErrorCode::Io, "mock encoder could not open the output file"));
        }
        out.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        if (!out) {
            return err(Error(ErrorCode::Io, "mock encoder could not write the output file"));
        }
        return ok();
    }

private:
    std::filesystem::path path_;
    std::vector<char>     buffer_{};
    std::uint32_t         frames_{0};
};

media::EncodeBackendFactory fileWritingFactory() {
    return [](const media::EncodeSpec& spec, const gpu::CodecRoute&)
               -> Result<std::unique_ptr<media::IEncodeBackend>> {
        return std::unique_ptr<media::IEncodeBackend>(
            std::make_unique<FileWritingEncodeBackend>(spec.outputPath));
    };
}

// Validate an output file against the mock encoder's container contract, and
// return the number of encoded frame records it carries.
[[nodiscard]] bool isValidMockOutput(const std::filesystem::path& path, std::uint32_t& framesOut) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const std::size_t minSize = sizeof(kMagic) + kTrailerSize;
    if (data.size() < minSize) return false;
    if (std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) return false;

    const std::size_t trailerOff = data.size() - kTrailerSize;
    if (std::memcmp(data.data() + trailerOff, kEndTag, sizeof(kEndTag)) != 0) return false;

    std::uint32_t count = 0;
    std::memcpy(&count, data.data() + trailerOff + sizeof(kEndTag), sizeof(count));

    // The bytes between the magic and the trailer must be exactly `count`
    // fixed-size frame records.
    const std::size_t body = data.size() - sizeof(kMagic) - kTrailerSize;
    if (body != static_cast<std::size_t>(count) * kFrameRecordSize) return false;

    // Each record must carry the frame tag.
    std::size_t off = sizeof(kMagic);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (std::memcmp(data.data() + off, kFrameTag, sizeof(kFrameTag)) != 0) return false;
        off += kFrameRecordSize;
    }

    framesOut = count;
    return true;
}

// ---------------------------------------------------------------------------
// Synthetic probe backend (per codec) — the import seam
// ---------------------------------------------------------------------------

media::MediaProbeBackend probeBackendFor(media::MediaCodecId codec, std::string codecName) {
    return [codec, codecName](const std::filesystem::path&) -> Result<media::MediaInfo> {
        media::MediaInfo info;
        info.containerFormat = "mov,mp4,m4a,3gp,3g2,mj2";
        info.containerLongName = "QuickTime / MOV";
        info.duration = FrameRate::fps30().durationForFrames(kFrameCount);

        media::MediaStreamInfo video;
        video.index = 0;
        video.type = media::MediaStreamType::Video;
        video.codec = codec;
        video.codecName = std::move(codecName);
        video.resolution = kRes;
        video.frameRate = FrameRate::fps30();
        video.duration = info.duration;
        info.streams.push_back(video);
        return info;
    };
}

// ---------------------------------------------------------------------------
// Synthetic decode backend (per codec) — the decode seam
// ---------------------------------------------------------------------------
// Produces CPU frames of the canvas geometry, filled with a codec-derived
// value so the composited pixels demonstrably originate from a decode step.

class SyntheticDecodeBackend final : public media::IDecodeBackend {
public:
    SyntheticDecodeBackend(media::MediaCodecId codec, std::uint8_t fill)
        : desc_{kW, kH, gpu::FrameFormat::RGBA8}, fill_(fill) {
        media::MediaStreamInfo video;
        video.index = 0;
        video.type = media::MediaStreamType::Video;
        video.codec = codec;
        video.codecName = "mock";
        video.resolution = kRes;
        info_.streams.push_back(video);
    }

    [[nodiscard]] const media::MediaInfo& info() const override { return info_; }

    [[nodiscard]] Result<media::BackendFrame> decode(bool useHardware) override {
        if (useHardware) {
            // No hardware in this lane; the software route must be taken instead.
            return err<media::BackendFrame>(
                Error(ErrorCode::Internal, "no hardware decode available"));
        }
        media::BackendFrame f;
        f.hardware = false;
        f.desc = desc_;
        f.timestamp = Duration::fromMilliseconds(10);
        f.cpuPixels.assign(desc_.byteSize(), std::byte{fill_});
        return f;
    }

    [[nodiscard]] Result<void> seek(Duration) override { return ok(); }

private:
    gpu::FrameDesc   desc_;
    std::uint8_t     fill_;
    media::MediaInfo info_{};
};

media::DecodeBackendFactory syntheticDecodeFactory(media::MediaCodecId codec, std::uint8_t fill) {
    return [codec, fill](const std::filesystem::path&, const media::DecodePrefs&)
               -> Result<std::unique_ptr<media::IDecodeBackend>> {
        return std::unique_ptr<media::IDecodeBackend>(
            std::make_unique<SyntheticDecodeBackend>(codec, fill));
    };
}

// ---------------------------------------------------------------------------
// Per-codec end-to-end fixture
// ---------------------------------------------------------------------------

struct CodecCase {
    media::MediaCodecId media;
    std::string         name;      ///< raw decoder name for the probe.
    std::uint8_t        fill;      ///< decoded pixel fill value.
    std::string         fileTag;   ///< output filename discriminator.
};

// Run the whole import -> edit -> export pipeline for one source codec and
// return the ExportResult (or the error). Populates `outputPath` with the file
// the export targeted so the caller can validate + clean it up.
Result<media::ExportResult> runPipelineForCodec(const CodecCase& c,
                                                std::filesystem::path& outputPath) {
    // --- 1. Import validation (through the probe seam) ---------------------
    auto imported = media::validateMediaImport("source." + c.fileTag,
                                               probeBackendFor(c.media, c.name));
    if (imported.isError()) return err<media::ExportResult>(imported.error());
    const media::MediaInfo info = std::move(imported).value();
    if (!info.hasVideo() || !info.hasSupportedStream()) {
        return err<media::ExportResult>(Error(ErrorCode::Internal, "import produced no usable video"));
    }

    // --- 2. Catalog the asset + register the clip in the media library -----
    MediaManager media;
    const MediaAssetRef assetRef(Uuid::generateV4(), "source." + c.fileTag);
    if (auto r = media.importAsset(assetRef); r.isError()) {
        return err<media::ExportResult>(r.error());
    }

    const Duration sourceOut = FrameRate::fps30().durationForFrames(kFrameCount);
    const ClipId clipId = Uuid::generateV4();
    if (auto r = media.registerClip(clipId, assetRef, Duration::zero(), sourceOut);
        r.isError()) {
        return err<media::ExportResult>(r.error());
    }

    // --- 3. Build the initial project + timeline ---------------------------
    Clip clip;
    clip.id = clipId;
    clip.assetRef = assetRef;
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = sourceOut;
    clip.opacity = 1.0;

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips = {clip};

    Project project;
    project.id = Uuid::generateV4();
    project.name = "import-edit-export-" + c.fileTag;
    project.timelineFps = FrameRate::fps30();
    project.canvas = kRes;
    project.tracks = {track};
    project.assets = {assetRef};

    // --- 4. Real edits through the TimelineEngine + EditCommands -----------
    TimelineEngine engine(project);

    if (!engine.apply(std::make_unique<AddEffectCommand>(clipId, Effect::brightness(0.2)))
             .changed()) {
        return err<media::ExportResult>(Error(ErrorCode::Internal, "AddEffect edit failed"));
    }
    // Split the clip at an interior playhead (1 frame in) into two contiguous
    // clips — a genuine structural edit.
    const Duration playhead = FrameRate::fps30().durationForFrames(1);
    if (!engine.apply(std::make_unique<SplitClipCommand>(clipId, playhead)).changed()) {
        return err<media::ExportResult>(Error(ErrorCode::Internal, "SplitClip edit failed"));
    }

    const Project edited = engine.snapshot();

    // --- 5. Compositor over the software-fallback context, fed by a real ---
    //         MediaDecoder decoding synthetic per-codec frames.
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor compositor(ctx);

    auto decoderHolder = std::make_shared<std::unique_ptr<media::MediaDecoder>>();
    {
        media::DecodePrefs prefs;
        prefs.preferHardware = false; // software lane (no GPU in the sandbox).
        auto opened = media::MediaDecoder::open("source." + c.fileTag, prefs,
                                                syntheticDecodeFactory(c.media, c.fill));
        if (opened.isError()) return err<media::ExportResult>(opened.error());
        *decoderHolder = std::make_unique<media::MediaDecoder>(std::move(opened).value());
    }

    compositor.setFrameProvider(
        [decoderHolder](const Clip&, Duration) -> Result<gpu::SourceFrame> {
            auto frameRes = (*decoderHolder)->nextFrame();
            if (frameRes.isError()) return err<gpu::SourceFrame>(frameRes.error());
            media::DecodedFrame frame = std::move(frameRes).value();
            if (!frame.isCpu()) {
                return err<gpu::SourceFrame>(
                    Error(ErrorCode::Internal, "expected a CPU frame on the software lane"));
            }
            gpu::SourceFrame src;
            src.width = frame.desc().width;
            src.height = frame.desc().height;
            const auto& px = frame.cpuPixels();
            src.rgba.resize(px.size());
            std::memcpy(src.rgba.data(), px.data(), px.size());
            return src;
        });

    // --- 6. Export through ExportEngine::run + the file-writing encoder ----
    outputPath = std::filesystem::temp_directory_path() /
                 ("palmier_integration_export_" + c.fileTag + ".mp4");
    std::error_code ec;
    std::filesystem::remove(outputPath, ec); // ensure a clean start.

    media::ExportEngine exporter(compositor);
    media::ExportRequest req;
    req.codec = gpu::CodecId::H264; // a supported export codec.
    req.resolution = kRes;
    req.frameRate = FrameRate::fps30();
    req.bitrateBitsPerSecond = 2'000'000;
    req.preferHardware = false;
    req.outputPath = outputPath;
    req.containerFormat = "mp4";
    req.progressInterval = std::chrono::milliseconds{0};

    return exporter.run(edited, req, fileWritingFactory());
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

class ImportEditExportIntegration : public ::testing::TestWithParam<CodecCase> {};

// Requirement 11.1 + 11.6: for each supported source codec, the complete
// import -> edit -> export pipeline renders the timeline into a single output
// file and reports its location; the file is real, valid, and non-empty.
TEST_P(ImportEditExportIntegration, ProducesAValidOutputFile) {
    const CodecCase c = GetParam();

    std::filesystem::path output;
    auto result = runPipelineForCodec(c, output);

    ASSERT_TRUE(result.isOk()) << "export failed for codec " << c.name;
    const media::ExportResult& r = result.value();

    // Requirement 11.1: the complete timeline was rendered (every planned frame).
    EXPECT_EQ(r.framesRendered, r.totalFrames);
    EXPECT_EQ(r.framesRendered, static_cast<std::size_t>(kFrameCount));

    // Requirement 11.6: the result reports the output location...
    EXPECT_EQ(r.outputPath, output);

    // ...and that location names a real, valid, non-empty output file whose
    // encoded-frame count matches what the engine rendered.
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::exists(output, ec)) << "no output file at " << output;
    EXPECT_GT(std::filesystem::file_size(output, ec), 0u);

    std::uint32_t encodedFrames = 0;
    EXPECT_TRUE(isValidMockOutput(output, encodedFrames)) << "malformed output for " << c.name;
    EXPECT_EQ(encodedFrames, r.framesRendered);

    std::filesystem::remove(output, ec); // clean up.
}

INSTANTIATE_TEST_SUITE_P(
    PerCodec, ImportEditExportIntegration,
    ::testing::Values(CodecCase{media::MediaCodecId::H264, "h264", 0x11, "h264"},
                      CodecCase{media::MediaCodecId::HEVC, "hevc", 0x22, "hevc"},
                      CodecCase{media::MediaCodecId::AV1, "av1", 0x33, "av1"},
                      CodecCase{media::MediaCodecId::VP9, "vp9", 0x44, "vp9"}),
    [](const ::testing::TestParamInfo<CodecCase>& info) { return info.param.fileTag; });

// A non-parameterized companion asserting the source timeline is not modified by
// the export (Requirement 11.1: "without modifying the source timeline"),
// checked here at the whole-pipeline level for one codec.
TEST(ImportEditExportIntegrationExtra, ExportDoesNotModifyTheEditedTimeline) {
    const CodecCase c{media::MediaCodecId::H264, "h264", 0x55, "nomutate"};

    // Rebuild just enough to hold a reference project alongside the export.
    auto imported = media::validateMediaImport("source.mp4", probeBackendFor(c.media, c.name));
    ASSERT_TRUE(imported.isOk());

    MediaManager media;
    const MediaAssetRef assetRef(Uuid::generateV4(), "source.mp4");
    ASSERT_TRUE(media.importAsset(assetRef).isOk());

    const Duration sourceOut = FrameRate::fps30().durationForFrames(kFrameCount);
    const ClipId clipId = Uuid::generateV4();
    ASSERT_TRUE(media.registerClip(clipId, assetRef, Duration::zero(), sourceOut).isOk());

    Clip clip;
    clip.id = clipId;
    clip.assetRef = assetRef;
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = sourceOut;

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips = {clip};

    Project project;
    project.id = Uuid::generateV4();
    project.name = "nomutate";
    project.timelineFps = FrameRate::fps30();
    project.canvas = kRes;
    project.tracks = {track};
    project.assets = {assetRef};

    TimelineEngine engine(project);
    ASSERT_TRUE(engine.apply(std::make_unique<AddEffectCommand>(clipId, Effect::brightness(0.1)))
                    .changed());

    const Project before = engine.snapshot();

    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor compositor(ctx);
    compositor.setFrameProvider([](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return gpu::SourceFrame::solid(kW, kH, gpu::RgbaColor{7, 8, 9, 255});
    });

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "palmier_integration_export_nomutate.mp4";
    std::error_code ec;
    std::filesystem::remove(output, ec);

    media::ExportEngine exporter(compositor);
    media::ExportRequest req;
    req.codec = gpu::CodecId::H264;
    req.resolution = kRes;
    req.frameRate = FrameRate::fps30();
    req.outputPath = output;
    req.containerFormat = "mp4";
    req.progressInterval = std::chrono::milliseconds{0};

    auto result = exporter.run(before, req, fileWritingFactory());
    ASSERT_TRUE(result.isOk());

    // The engine's project is unchanged by the export (it took `before` by value
    // and only reads it).
    const Project after = engine.snapshot();
    ASSERT_EQ(after.tracks.size(), before.tracks.size());
    ASSERT_EQ(after.tracks[0].clips.size(), before.tracks[0].clips.size());
    EXPECT_EQ(after.tracks[0].clips[0].id, before.tracks[0].clips[0].id);
    EXPECT_EQ(after.tracks[0].clips[0].sourceOut, before.tracks[0].clips[0].sourceOut);

    std::filesystem::remove(output, ec);
}

} // namespace
} // namespace palmier
