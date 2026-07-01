// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Types.hpp — convenience aggregator for the shared domain primitives.
//
// Pulls in the foundational value types used throughout the timeline model,
// media engine, GPU layer, and services. Downstream code may include this single
// header or the individual headers as preferred.

#ifndef PALMIER_CORE_TYPES_HPP
#define PALMIER_CORE_TYPES_HPP

#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/SchemaVersion.hpp"
#include "core/Uuid.hpp"

// Project data model (task 2.1) and its validation rules.
#include "core/Clip.hpp"
#include "core/Effect.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/ProjectValidation.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"

// Command / undo-redo primitives (task 3.1).
#include "core/CommandResult.hpp"
#include "core/EditCommand.hpp"
#include "core/UndoRedoStack.hpp"

#endif // PALMIER_CORE_TYPES_HPP
