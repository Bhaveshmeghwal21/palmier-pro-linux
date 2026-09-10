// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ProjectSettingsEditCommands.cpp — the project-wide settings edit.
//
// Split out of core/EditCommands.cpp, which held all 24 command implementations in
// one 1583-line translation unit. Every definition below was moved rather than
// rewritten. The commands are declared in the public core/EditCommands.hpp, which
// is unchanged; the helpers they share are in core/EditCommandSupport.hpp.

#include "core/EditCommands.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/EditCommandSupport.hpp"
#include "core/Error.hpp"
#include "core/Project.hpp"
#include "core/Track.hpp"

namespace palmier {

using namespace palmier::detail;  // NOLINT(google-build-using-namespace)

const char* curvePointOperationToolName(EditCurvePointCommand::Operation operation) noexcept {
    switch (operation) {
        case EditCurvePointCommand::Operation::Add:
            return "add";
        case EditCurvePointCommand::Operation::Move:
            return "move";
        case EditCurvePointCommand::Operation::Remove:
            return "remove";
    }
    return "add";
}

std::optional<EditCurvePointCommand::Operation> parseCurvePointOperationToolName(
    std::string_view name) {
    // Derived from the name function, so the two directions cannot disagree.
    for (const auto operation :
         {EditCurvePointCommand::Operation::Add, EditCurvePointCommand::Operation::Move,
          EditCurvePointCommand::Operation::Remove}) {
        if (name == curvePointOperationToolName(operation)) {
            return operation;
        }
    }
    return std::nullopt;
}

const std::vector<std::string>& curvePointOperationToolNames() {
    static const std::vector<std::string> names = {
        curvePointOperationToolName(EditCurvePointCommand::Operation::Add),
        curvePointOperationToolName(EditCurvePointCommand::Operation::Move),
        curvePointOperationToolName(EditCurvePointCommand::Operation::Remove)};
    return names;
}

// ===========================================================================
// SetProjectSettingsCommand
// ===========================================================================

SetProjectSettingsCommand::SetProjectSettingsCommand(std::optional<FrameRate> fps,
                                                     std::optional<Resolution> canvas,
                                                     std::optional<ColorSpace> colorSpace)
    : fps_(fps), canvas_(canvas), colorSpace_(colorSpace) {}

Result<void> SetProjectSettingsCommand::apply(Project& project) {
    // core has no dependency on services::, so the declared numeric ranges
    // (Requirement 7.1's "same ranges project.create accepts") are the
    // Tool_Surface's to enforce; this only rejects a value that is not even
    // internally well-formed, matching every other command's own-invariant check.
    if (fps_ && !fps_->isValid()) {
        return err(invalidArgument("SetProjectSettingsCommand: fps is not a valid frame rate"));
    }
    if (canvas_ && !canvas_->isValid()) {
        return err(invalidArgument("SetProjectSettingsCommand: canvas is not a valid resolution"));
    }

    priorFps_ = project.timelineFps;
    priorCanvas_ = project.canvas;
    priorColorSpace_ = project.colorSpace;
    captured_ = true;

    if (fps_) project.timelineFps = *fps_;
    if (canvas_) project.canvas = *canvas_;
    if (colorSpace_) project.colorSpace = *colorSpace_;
    return ok();
}

Result<void> SetProjectSettingsCommand::revert(Project& project) {
    if (!captured_) {
        return err(
            failedPrecondition("SetProjectSettingsCommand: revert before a successful apply"));
    }
    project.timelineFps = priorFps_;
    project.canvas = priorCanvas_;
    project.colorSpace = priorColorSpace_;
    return ok();
}

}  // namespace palmier
