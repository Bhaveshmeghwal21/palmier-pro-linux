// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectStore.hpp — persistence of a Project to/from the documented
// `.palmier` store (design.md Architecture "Project I/O .palmier"; Requirements
// 3.5, 3.6).
//
// The Project is the single source of truth for the editor (design.md Data
// Models). This component captures the COMPLETE project state so that reopening a
// project restores the prior editing state — clip positions, track order, per-clip
// source ranges, effects, transitions, the referenced media (and hence the
// selected version of each clip), the timeline settings, and the schema version
// (Requirement 3.5). The store round-trips losslessly: for any valid project,
// deserialize(serialize(p)) yields an equivalent project (design property P11,
// validated by task 5.2).
//
// The `.palmier` file format
// --------------------------
// A `.palmier` store is a single UTF-8 JSON document. We define our own documented
// format rather than depend on the (unspecified) macOS layout — see design.md
// "Assumptions to validate": we define our own documented `.palmier` format. The
// top-level object is:
//
//   {
//     "format": "palmier-project",   // magic string; identifies the document kind
//     "version": "1.1",              // SchemaVersion (major.minor) the doc was written at
//     "project": { ...Project... }   // the serialized Project (see below)
//   }
//
// The nested "project" object mirrors the Project data model exactly:
//
//   id          : string  (canonical UUID)
//   name        : string
//   timelineFps : { "num": int, "den": int }              (exact rational frame rate)
//   canvas      : { "width": int, "height": int }
//   colorSpace  : string  (stable key, e.g. "rec709")
//   tracks      : [ Track, ... ]        (array order == track order / z-order)
//   assets      : [ MediaAssetRef, ... ]
//   clipGroups  : [ ClipGroup, ... ]    (1.1; optional on read, default [])
//
//   Track       : { id, kind ("video"|"audio"), name, muted, locked, clips: [Clip,...] }
//                 (name is 1.1; optional on read, default "")
//   Clip        : { id, assetRef, timelineStart, sourceIn, sourceOut,
//                   effects: [Effect,...], transitionIn: Transition|null,
//                   gain, opacity }
//   Effect      : { id, type (stable key), parameters: { name: number, ... } }
//   Transition  : { id, kind (stable key), duration }
//   MediaAssetRef : { assetId: string, sourcePath: string }
//   ClipGroup   : { id: string, clipIds: [string, ...] }
//
// Schema 1.1 additions
// --------------------
// 1.1 adds exactly three things, each of them additive and optional on read with a
// documented default, so a 1.0 document deserializes unchanged:
//
//   * the effect type key "invert_colors" (EffectType::InvertColors);
//   * "tracks[].name"  — string, default "" when absent;
//   * "clipGroups"     — array of { id, clipIds[] }, default [] when absent.
//
// A writer at 1.1 always emits the latter two (as "" and [] for a project that
// carries neither), so the round-trip and idempotence properties hold verbatim.
// `clipGroups` is *reserved*: it is persisted faithfully but no edit interprets it
// — the multicam ripple trim that will consume it is deliberately out of scope, and
// reserving the field now means adding that behaviour needs no further schema bump.
//
// Because 1.1 documents declare version "1.1", a 1.0-era build correctly rejects
// them as unsupported (same major, older reader minor) rather than loading them
// with fields silently dropped.
//
// All Duration fields (timelineStart, sourceIn, sourceOut, transition duration)
// are written as integer nanosecond tick counts, matching Duration's internal,
// drift-free representation, so timing survives the round-trip exactly.
//
// Schema version handling on read: the reader honours SchemaVersion compatibility
// (same major, reader minor >= stored minor). An unknown/unsupported version is
// rejected with an ErrorCode::Unsupported Result rather than being loaded
// (Requirement 3.5 "reopening restores the prior state" only for stores this build
// understands). Any malformed document yields an ErrorCode::InvalidArgument Result.
//
// This header intentionally exposes only the pure (de)serialization surface plus a
// pair of thin file read/write helpers. The save success/failure UX
// (disk-space/permission handling and "last good state" preservation, Requirement
// 3.7) is layered on top in task 5.3; this component provides the mechanism, not
// that policy.

#ifndef PALMIER_SERVICES_PROJECTSTORE_HPP
#define PALMIER_SERVICES_PROJECTSTORE_HPP

#include <filesystem>
#include <string>
#include <string_view>

#include "core/Project.hpp"
#include "core/Result.hpp"

namespace palmier::services {

/// Serialize a complete project to the documented `.palmier` (JSON) text.
///
/// The output captures every field of the project data model (tracks and their
/// order, clips and their positions/source ranges, effects, transitions, the
/// referenced media assets, timeline settings, and the schema version) so that a
/// subsequent deserialize reconstructs an equivalent project. The result is a
/// human-readable, pretty-printed UTF-8 string. This function never fails: any
/// project value can be rendered (validity is a separate concern checked by
/// ProjectValidation / on load).
[[nodiscard]] std::string serializeProject(const Project& project);

/// Parse a `.palmier` (JSON) document previously produced by serializeProject
/// (or a compatible writer) back into a Project.
///
/// Returns:
///   * an ok Result holding the reconstructed Project on success;
///   * ErrorCode::InvalidArgument if the text is not a well-formed `.palmier`
///     document (malformed JSON, wrong "format" magic, missing/ill-typed fields);
///   * ErrorCode::Unsupported if the document's schema version is not loadable by
///     this build (see SchemaVersion::isCompatible).
///
/// Parsing is purely structural: it does not additionally enforce the data-model
/// validation rules (use validateProject for that), except that it always rejects
/// an unsupported schema version.
[[nodiscard]] Result<Project> deserializeProject(std::string_view text);

/// Write a project to `path` as a `.palmier` store (basic helper: serialize then
/// write the bytes). Returns ErrorCode::Io if the file could not be opened or
/// written. This is the low-level mechanism; the user-facing save success/failure
/// handling (Requirement 3.7) is built on top in task 5.3.
[[nodiscard]] Result<void> saveProjectToFile(const Project& project,
                                             const std::filesystem::path& path);

/// Read and parse a `.palmier` store from `path`. Returns ErrorCode::Io if the
/// file could not be opened/read, or the same errors as deserializeProject for a
/// malformed/unsupported document.
[[nodiscard]] Result<Project> loadProjectFromFile(const std::filesystem::path& path);

} // namespace palmier::services

#endif // PALMIER_SERVICES_PROJECTSTORE_HPP
