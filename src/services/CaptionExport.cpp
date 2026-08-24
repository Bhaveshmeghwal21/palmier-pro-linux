// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/CaptionExport.cpp — see CaptionExport.hpp for the contract.

#include "services/CaptionExport.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <vector>

#include "core/Track.hpp"

namespace palmier::services {

namespace {

/// One cue gathered from the project, ready to be ordered and rendered.
struct SrtCue {
    Duration    start;
    Duration    end;
    std::size_t trackIndex;
    Uuid        clipId;
    std::string text;
};

/// Format `d` (assumed non-negative) as SRT's "HH:MM:SS,mmm".
[[nodiscard]] std::string formatSrtTimestamp(Duration d) {
    std::int64_t totalMs = d.milliseconds();
    if (totalMs < 0) totalMs = 0;  // defensive: SRT has no negative timestamps.

    const std::int64_t ms = totalMs % 1000;
    const std::int64_t totalSeconds = totalMs / 1000;
    const std::int64_t s = totalSeconds % 60;
    const std::int64_t totalMinutes = totalSeconds / 60;
    const std::int64_t m = totalMinutes % 60;
    const std::int64_t h = totalMinutes / 60;

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld,%03lld",
                 static_cast<long long>(h), static_cast<long long>(m),
                 static_cast<long long>(s), static_cast<long long>(ms));
    return std::string(buffer);
}

}  // namespace

bool projectHasCaptions(const Project& project) noexcept {
    for (const Track& track : project.tracks) {
        if (track.kind != TrackKind::Caption || track.muted) continue;
        for (const Clip& clip : track.clips) {
            if (clip.isCaptionCue()) return true;
        }
    }
    return false;
}

std::string renderSrt(const Project& project) {
    std::vector<SrtCue> cues;
    for (std::size_t trackIndex = 0; trackIndex < project.tracks.size(); ++trackIndex) {
        const Track& track = project.tracks[trackIndex];
        if (track.kind != TrackKind::Caption || track.muted) continue;
        for (const Clip& clip : track.clips) {
            if (!clip.isCaptionCue()) continue;  // see this file's own header doc comment.
            cues.push_back(SrtCue{clip.timelineStart, clip.timelineEnd(), trackIndex, clip.id,
                                  *clip.captionText});
        }
    }

    std::stable_sort(cues.begin(), cues.end(), [](const SrtCue& a, const SrtCue& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.trackIndex != b.trackIndex) return a.trackIndex < b.trackIndex;
        return a.clipId < b.clipId;
    });

    std::ostringstream out;
    int index = 1;
    for (const SrtCue& cue : cues) {
        out << index << "\r\n";
        out << formatSrtTimestamp(cue.start) << " --> " << formatSrtTimestamp(cue.end) << "\r\n";
        out << cue.text << "\r\n";
        out << "\r\n";
        ++index;
    }
    return out.str();
}

}  // namespace palmier::services
