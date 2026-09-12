// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/TextCaptionEditCommands.hpp - text clips and caption cues.
//
// One of the six command-family headers behind core/EditCommands.hpp. The grouping mirrors
// TextCaptionEditCommands.cpp exactly, so a command's declaration and its
// implementation are always in the matching pair of files.
//
// The apply()/revert() contract these all share, and the invariants the engine enforces
// around them, are documented once in core/EditCommands.hpp.

#ifndef PALMIER_CORE_TEXTCAPTIONEDITCOMMANDS_HPP
#define PALMIER_CORE_TEXTCAPTIONEDITCOMMANDS_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"

namespace palmier {

struct Project;  // core/Project.hpp - the mutation target (forward-declared).

// ===========================================================================
// Text and titles (usable-editor task 12; Requirement 9)
//
// Creating a text clip needs no new command: it is an ordinary AddClipCommand
// whose Clip carries a populated textStyle and an unset (nil, "invalid")
// assetRef â€” AddClipCommand's own asset-registration step already skips a
// clip whose assetRef.isValid() is false, so a text clip is placed, ordered
// and made undoable by the exact path every other clip already uses, with
// zero changes to that command. What text needs beyond placement is a way to
// change its content and its styling after creation â€” the two commands below.
// ===========================================================================

// ---------------------------------------------------------------------------
// SetTextContentCommand â€” change a text clip's displayed string.
// ---------------------------------------------------------------------------
//
// Refused, leaving the project unchanged, when the named clip does not exist
// or is not a text clip (Requirement 9.2's "each undoable in one Undo" only
// makes sense for a clip that actually carries a TextStyle to change).
class SetTextContentCommand final : public EditCommand {
public:
    SetTextContentCommand(ClipId clipId, std::string content);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetTextContent"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    std::string content_;
    std::string prior_;  // captured on apply for an exact revert
    bool captured_ = false;
};

// ---------------------------------------------------------------------------
// SetTextStyleCommand â€” change a text clip's font, size, colour, alignment
// and/or screen position.
// ---------------------------------------------------------------------------
//
// Every field is std::optional, mirroring SetProjectSettingsCommand: a field
// left std::nullopt is left exactly as it was, so any subset of font family,
// point size, colour, alignment and position changes in one undoable edit
// (Requirement 9.2). core has no services:: dependency, so this command
// validates only that the resulting TextStyle is internally well-formed
// (TextStyle::isValid()) â€” the same core/services split
// SetProjectSettingsCommand already established. Refused, leaving the project
// unchanged, when the named clip does not exist or is not a text clip, or when
// the requested change would make the style invalid.
class SetTextStyleCommand final : public EditCommand {
public:
    SetTextStyleCommand(ClipId clipId, std::optional<std::string> fontFamily,
                        std::optional<double> pointSize, std::optional<double> colorR,
                        std::optional<double> colorG, std::optional<double> colorB,
                        std::optional<double> colorA, std::optional<TextAlignment> alignment,
                        std::optional<double> x, std::optional<double> y);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetTextStyle"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    std::optional<std::string> fontFamily_;
    std::optional<double> pointSize_;
    std::optional<double> colorR_, colorG_, colorB_, colorA_;
    std::optional<TextAlignment> alignment_;
    std::optional<double> x_, y_;

    TextStyle prior_;  // captured on apply for an exact revert
    bool captured_ = false;
};

// ===========================================================================
// Captions and transcription (usable-editor task 13; Requirement 10)
//
// Creating a caption cue needs no new command either, for the identical reason
// creating a text clip did not (task 12): it is an ordinary AddClipCommand
// whose Clip carries a populated captionText and an unset (nil, "invalid")
// assetRef. What captions need beyond placement is a way to change a cue's
// text and to retime it â€” the two commands below.
// ===========================================================================

// ---------------------------------------------------------------------------
// SetCaptionTextCommand â€” change a caption cue's displayed string.
// ---------------------------------------------------------------------------
//
// Refused, leaving the project unchanged, when the named clip does not exist,
// is not a caption cue, or when the new text is empty (Requirement 10's own
// "a caption cue has text" invariant, checked by TextStyle-analogous
// well-formedness â€” see Clip::isCaptionCue()'s own doc comment).
class SetCaptionTextCommand final : public EditCommand {
public:
    SetCaptionTextCommand(ClipId clipId, std::string text);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetCaptionText"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    std::string text_;
    std::string prior_;  // captured on apply for an exact revert
    bool captured_ = false;
};

// ---------------------------------------------------------------------------
// RetimeCaptionCueCommand â€” change a caption cue's timing in one undoable edit.
// ---------------------------------------------------------------------------
//
// Requirement 10.2's "retime" operation is a change to when a cue starts and/or
// how long it lasts. Rather than requiring two separate calls â€” MoveClipCommand
// for the start, TrimClipCommand for the duration â€” retiming sets timelineStart
// and sourceOut (with sourceIn fixed at zero, exactly like a freshly-created
// cue) together, so any combination of "move it", "lengthen it", "shorten it",
// or both at once is the same single command and the same single Undo. Rejects
// a non-positive resulting duration or a negative timelineStart, leaving the
// project unchanged, and â€” like MoveClipCommand â€” rejects (and rolls back) a
// destination that would overlap another cue on the same track.
class RetimeCaptionCueCommand final : public EditCommand {
public:
    RetimeCaptionCueCommand(ClipId clipId, std::optional<Duration> newTimelineStart,
                            std::optional<Duration> newDuration);

    [[nodiscard]] std::string_view name() const noexcept override { return "RetimeCaptionCue"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    std::optional<Duration> newTimelineStart_;
    std::optional<Duration> newDuration_;

    Uuid trackId_;
    std::vector<Clip> priorClips_;  // captured on apply for an exact revert
    bool captured_ = false;
};

}  // namespace palmier

#endif  // PALMIER_CORE_TEXTCAPTIONEDITCOMMANDS_HPP
