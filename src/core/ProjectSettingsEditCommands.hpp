// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ProjectSettingsEditCommands.hpp - project-wide settings.
//
// One of the six command-family headers behind core/EditCommands.hpp. The grouping mirrors
// ProjectSettingsEditCommands.cpp exactly, so a command's declaration and its
// implementation are always in the matching pair of files.
//
// The apply()/revert() contract these all share, and the invariants the engine enforces
// around them, are documented once in core/EditCommands.hpp.

#ifndef PALMIER_CORE_PROJECTSETTINGSEDITCOMMANDS_HPP
#define PALMIER_CORE_PROJECTSETTINGSEDITCOMMANDS_HPP

#include <optional>
#include <string>
#include <string_view>

#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/FrameRate.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"

namespace palmier {

struct Project;  // core/Project.hpp - the mutation target (forward-declared).

// ---------------------------------------------------------------------------
// SetProjectSettingsCommand â€” change frame rate, canvas and/or colour space.
// ---------------------------------------------------------------------------
//
// Requirement 7: a project's frame rate, canvas resolution and colour space can
// each be changed after creation, accepting the same ranges `project.create`
// accepts (services::kMinFramesPerSecond..kMaxFramesPerSecond,
// kMinCanvasWidth..kMaxCanvasWidth x kMinCanvasHeight..kMaxCanvasHeight; the
// range check itself is the Tool_Surface's, since core has no reason to depend
// on services:: â€” this command validates only that a supplied FrameRate/
// Resolution is internally well-formed via isValid()).
//
// Any subset of the three settings may change in one call; each argument left
// as std::nullopt is left exactly as it was. This is what makes "any setting
// changes... undoable in one Undo" (Requirement 7.4) hold even when several
// fields change together: one command, one history entry, one exact revert.
//
// Requirement 7.3: no clip is touched. Every clip's timeline position and
// source range is a Duration (an absolute nanosecond count with no embedded
// frame rate â€” see Duration.hpp), so changing timelineFps has nothing to
// migrate; the clips are simply interpreted against a different rate from the
// next read onward.
class SetProjectSettingsCommand final : public EditCommand {
public:
    SetProjectSettingsCommand(std::optional<FrameRate> fps, std::optional<Resolution> canvas,
                              std::optional<ColorSpace> colorSpace);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetProjectSettings"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    std::optional<FrameRate> fps_;
    std::optional<Resolution> canvas_;
    std::optional<ColorSpace> colorSpace_;

    // Captured on apply for an exact revert; only the fields this command
    // actually changes are meaningful, but all three are captured together since
    // Project's settings are a single unit to snapshot.
    FrameRate priorFps_;
    Resolution priorCanvas_;
    ColorSpace priorColorSpace_ = defaultColorSpace();
    bool captured_ = false;
};

}  // namespace palmier

#endif  // PALMIER_CORE_PROJECTSETTINGSEDITCOMMANDS_HPP
