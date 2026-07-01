// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/ImportValidation.hpp — decide whether a media file may be imported.
//
// This is the gate the Media Manager (task 6.1) applies before a file becomes
// part of a project library. It sits directly on top of MediaProbe: it probes
// the file, then classifies the outcome into exactly one of three cases that
// Requirements 3.2 and 3.3 call out:
//
//   * ACCEPT   — the container was read and carries at least one stream whose
//                codec the engine can decode. A normalized MediaInfo is
//                returned for placement on the timeline (Requirement 3.1).
//   * REJECT (unsupported format) — the file was read, but nothing in it is
//                decodable (all streams use unsupported codecs) or the
//                container itself is unrecognized. The error NAMES the format
//                so the UI can tell the user which format was rejected
//                (Requirement 3.2). ErrorCode::Unsupported.
//   * REJECT (unreadable)         — the format may well be supported, but the
//                file's contents could not be opened, read, or decoded. The
//                error indicates the file could not be read (Requirement 3.3).
//                ErrorCode::Io.
//
// The distinction between the two rejections is deliberate and machine-
// inspectable via the ErrorCode, so the Media Manager and UI can present the
// two required, different messages ("names the format" vs. "could not be read")
// from a single call.
//
// Purity / "library unchanged" guarantee: validation only READS. It never
// touches a project or library, so a caller that adds media to the library only
// when the returned Result is Ok trivially satisfies the "leave the library
// unchanged on rejection" clauses of both 3.2 and 3.3. See validateMediaImport.
//
// Like MediaProbe, this layer is FFmpeg-independent at the seam: it takes an
// injectable MediaProbeBackend, so both rejection paths and the accept path are
// unit-testable on machines with no FFmpeg or real media files.

#ifndef PALMIER_MEDIA_IMPORTVALIDATION_HPP
#define PALMIER_MEDIA_IMPORTVALIDATION_HPP

#include <filesystem>
#include <string>

#include "core/Result.hpp"
#include "media/MediaInfo.hpp"
#include "media/MediaProbe.hpp"

namespace palmier::media {

/// Produce a short, human-readable name of a probed file's format, suitable for
/// telling the user which format was rejected (Requirement 3.2). Combines the
/// container's name (long name preferred) with the distinct codecs it carries,
/// e.g. "Matroska / WebM (Theora, Vorbis)" or, when the container is unknown,
/// just the codec list. Falls back to "unknown format" when nothing is known.
[[nodiscard]] std::string describeMediaFormat(const MediaInfo& info);

/// Validate `path` for import using an injected probe backend, then classify:
///   * Ok(MediaInfo)              — accepted; carries >= 1 decodable stream.
///   * Error(Unsupported)         — rejected; message NAMES the format (3.2).
///   * Error(Io)                  — rejected; contents unreadable/undecodable (3.3).
///   * Error(InvalidArgument)     — the path was empty (propagated from probe).
///   * Error(FailedPrecondition)  — this build has no FFmpeg (propagated).
/// Performs no mutation: callers add to the library only on Ok, which keeps the
/// library unchanged on either rejection.
[[nodiscard]] Result<MediaInfo> validateMediaImport(const std::filesystem::path& path,
                                                    const MediaProbeBackend& backend);

/// Validate `path` for import using the default (FFmpeg) probe backend.
[[nodiscard]] Result<MediaInfo> validateMediaImport(const std::filesystem::path& path);

} // namespace palmier::media

#endif // PALMIER_MEDIA_IMPORTVALIDATION_HPP
