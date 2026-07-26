// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ClipGroup.hpp — a named set of clips that edit together.
//
// A ClipGroup names clips (by ClipId, across any number of tracks) that a future
// multicam ripple trim will keep synchronised. Schema 1.1 reserves the
// project-level `clipGroups` array so the grouping can be persisted now and the
// behaviour added later without another schema bump (design.md Data Models,
// "Project document (.palmier)"; upstream PR 397 in docs/PORT_BACKLOG.md).
//
// This feature deliberately implements NO ripple-trim behaviour: nothing reads
// clipGroups to decide how an edit propagates. The type exists so that a document
// carrying groups round-trips faithfully rather than losing them, and so a
// RippleTrimCommand can later be added without touching the store format.

#ifndef PALMIER_CORE_CLIPGROUP_HPP
#define PALMIER_CORE_CLIPGROUP_HPP

#include <vector>

#include "core/Clip.hpp"
#include "core/Uuid.hpp"

namespace palmier {

struct ClipGroup {
    Uuid                id;      ///< Stable per-group identity.
    std::vector<ClipId> clipIds; ///< The grouped clips, in document order.
};

} // namespace palmier

#endif // PALMIER_CORE_CLIPGROUP_HPP
