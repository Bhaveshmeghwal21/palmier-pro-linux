# SPDX-License-Identifier: GPL-3.0-or-later
#
# src/core — UI-agnostic domain core.
#
# Shared types, project data model, timeline engine, and command/undo-redo.
# Has no third-party runtime dependencies so it can be driven headlessly by the
# MCP server and the in-app agent.
#
# Task 1.3 introduces the foundational shared value types (Uuid, Duration,
# FrameRate, Resolution, ColorSpace, Result<T>/Error, SchemaVersion). Most are
# header-only, but Uuid (random generation/parse/format) and SchemaVersion
# (string parsing) carry translation units, so this target is now a compiled
# STATIC library instead of an INTERFACE library.
#
# Task 2.1 adds the project data model (Project, Track, Clip, Effect, Transition,
# MediaAssetRef — header-only value types) and its validation rules
# (ProjectValidation, which carries a translation unit).
#
# Task 3.1 adds the command / undo-redo primitives: the abstract EditCommand
# interface and the CommandResult type (both header-only), plus the bounded
# UndoRedoStack, which carries a translation unit.
#
# Task 3.2 adds the TimelineEngine — the authoritative project/timeline model
# that wraps a Project and the UndoRedoStack, applies EditCommands atomically
# (rolling back on failure or invariant violation), enforces the timeline
# invariants, and emits granular ChangeSet events to observers. ChangeSet and
# Subscription are header-only value/handle types; TimelineEngine carries a
# translation unit. The concrete editing commands are added by task 3.3.
#
# Task 6.1 adds the MediaManager — the project media library (imported/generated
# assets available for timeline placement, Requirement 3.1) and the per-clip
# generated-clip version history that retains at least the 10 most recent
# selectable versions when a generated clip replaces a clip (Requirement 3.4).
# ClipVersion is a header-only value type; MediaManager carries a translation unit.
# It is pure domain data (no FFmpeg/GPU/Qt), so it stays in the dependency-light core.
#
# Task 3.3 adds the concrete editing commands (EditCommands: AddClip, DeleteClip,
# MoveClip, TrimClip, SplitClip, ReorderClips, AddEffect) — the EditCommand
# subclasses that realize the interactive timeline editing operations
# (Requirement 2.2-2.7). They mutate a Project through the same apply()/revert()
# contract the TimelineEngine drives and carry a translation unit.

add_library(palmier_core STATIC
    Uuid.cpp
    SchemaVersion.cpp
    ProjectValidation.cpp
    UndoRedoStack.cpp
    TimelineEngine.cpp
    EditCommands.cpp
    MediaManager.cpp)
add_library(Palmier::core ALIAS palmier_core)

# Public headers are included as "core/<Header>.hpp" from the src/ root.
target_include_directories(palmier_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_core PUBLIC cxx_std_20)

if(PALMIER_WERROR)
    target_compile_options(palmier_core PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>)
endif()
