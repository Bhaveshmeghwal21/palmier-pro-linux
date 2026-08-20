// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/TimelineModel.cpp — implementation of the Qt QAbstractItemModel binding.
//
// Compiled only when Qt 6 is available (PALMIER_HAVE_QT). Every method here is a
// thin adapter over TimelineViewModel: the tree navigation projects the adapter's
// tracks x clips shape onto QModelIndex, data() reads the adapter's TrackRow /
// ClipView projections, and the Q_INVOKABLE gestures parse their string/ms
// arguments and delegate to the adapter (which routes them to the shared
// EditCommands). A ChangeSet from the engine triggers a model reset so the view
// re-reads the current state; because edits are infrequent relative to painting,
// a reset is simple and correct (a later optimization could translate the
// ChangeSet's added/removed/modified sets into granular begin/end row signals).

#include "ui/TimelineModel.hpp"

#ifdef PALMIER_HAVE_QT

#include <optional>
#include <vector>

#include "core/ChangeSet.hpp"
#include "core/Uuid.hpp"

namespace palmier::ui {

namespace {

QString kindToString(TrackKind kind) {
    return kind == TrackKind::Audio ? QStringLiteral("audio") : QStringLiteral("video");
}

std::optional<Uuid> parseId(const QString& text) {
    return Uuid::parse(text.toStdString());
}

}  // namespace

TimelineModel::TimelineModel(TimelineEngine& engine, QObject* parent, GuiToolGateway* gateway)
    : QAbstractItemModel(parent), vm_(engine, gateway) {
    // Reflect every engine change back into the view. beginResetModel/endResetModel
    // brackets keep attached views consistent across the snapshot swap.
    vm_.setChangeListener([this](const ChangeSet&) {
        beginResetModel();
        endResetModel();
        emit modelRefreshed();
    });
}

TimelineModel::~TimelineModel() = default;

bool TimelineModel::isTrackIndex(const QModelIndex& index) const {
    return index.isValid() && index.internalId() == kTrackTag;
}

QModelIndex TimelineModel::index(int row, int column, const QModelIndex& parent) const {
    if (row < 0 || column != 0) {
        return {};
    }
    if (!parent.isValid()) {
        // Top level: a track row.
        if (static_cast<std::size_t>(row) >= vm_.trackCount()) {
            return {};
        }
        return createIndex(row, column, kTrackTag);
    }
    // Child of a track: a clip row. Only tracks have children.
    if (!isTrackIndex(parent)) {
        return {};
    }
    const int trackRow = parent.row();
    if (static_cast<std::size_t>(row) >= vm_.clipCount(static_cast<std::size_t>(trackRow))) {
        return {};
    }
    return createIndex(row, column, static_cast<quintptr>(trackRow));
}

QModelIndex TimelineModel::parent(const QModelIndex& child) const {
    if (!child.isValid() || isTrackIndex(child)) {
        return {};  // tracks are top-level; invalid has no parent
    }
    const int trackRow = static_cast<int>(child.internalId());
    return createIndex(trackRow, 0, kTrackTag);
}

int TimelineModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid()) {
        return static_cast<int>(vm_.trackCount());
    }
    if (isTrackIndex(parent)) {
        return static_cast<int>(vm_.clipCount(static_cast<std::size_t>(parent.row())));
    }
    return 0;  // clips have no children
}

int TimelineModel::columnCount(const QModelIndex& /*parent*/) const { return 1; }

QVariant TimelineModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }

    if (isTrackIndex(index)) {
        const std::optional<TrackRow> track = vm_.trackAt(static_cast<std::size_t>(index.row()));
        if (!track) {
            return {};
        }
        switch (role) {
            case IsTrackRole:     return true;
            case TrackIdRole:     return QString::fromStdString(track->id.toString());
            case TrackKindRole:   return kindToString(track->kind);
            case TrackMutedRole:  return track->muted;
            case TrackLockedRole: return track->locked;
            case ClipCountRole:   return static_cast<qulonglong>(track->clipCount);
            default:              return {};
        }
    }

    const std::size_t trackRow = static_cast<std::size_t>(index.internalId());
    const std::optional<ClipView> clip =
        vm_.clipAt(trackRow, static_cast<std::size_t>(index.row()));
    if (!clip) {
        return {};
    }
    switch (role) {
        case IsTrackRole:            return false;
        case ClipIdRole:             return QString::fromStdString(clip->id.toString());
        case ClipStartMsRole:        return static_cast<qint64>(clip->timelineStart.milliseconds());
        case ClipDurationMsRole:     return static_cast<qint64>(clip->duration.milliseconds());
        case ClipSourceInMsRole:     return static_cast<qint64>(clip->sourceIn.milliseconds());
        case ClipSourceOutMsRole:    return static_cast<qint64>(clip->sourceOut.milliseconds());
        case ClipOpacityRole:        return clip->opacity;
        case ClipGainRole:           return clip->gain;
        case ClipHasTransitionRole:  return clip->hasTransitionIn;
        case ClipEffectCountRole:    return static_cast<qulonglong>(clip->effectCount);
        default:                     return {};
    }
}

QHash<int, QByteArray> TimelineModel::roleNames() const {
    return {
        {IsTrackRole, "isTrack"},
        {TrackIdRole, "trackId"},
        {TrackKindRole, "trackKind"},
        {TrackMutedRole, "muted"},
        {TrackLockedRole, "locked"},
        {ClipCountRole, "clipCount"},
        {ClipIdRole, "clipId"},
        {ClipStartMsRole, "startMs"},
        {ClipDurationMsRole, "durationMs"},
        {ClipSourceInMsRole, "sourceInMs"},
        {ClipSourceOutMsRole, "sourceOutMs"},
        {ClipOpacityRole, "opacity"},
        {ClipGainRole, "gain"},
        {ClipHasTransitionRole, "hasTransition"},
        {ClipEffectCountRole, "effectCount"},
    };
}

QString TimelineModel::lastIndication() const {
    return QString::fromUtf8(toStringView(vm_.lastIndication()).data(),
                             static_cast<int>(toStringView(vm_.lastIndication()).size()));
}

// --- Gestures --------------------------------------------------------------

bool TimelineModel::moveClip(const QString& clipId, qint64 newStartMs) {
    const std::optional<Uuid> id = parseId(clipId);
    if (!id) {
        return false;
    }
    const GestureResult r = vm_.moveClip(*id, Duration::fromMilliseconds(newStartMs));
    emit indicationChanged();
    return r.changed();
}

bool TimelineModel::trimClipStart(const QString& clipId, qint64 newSourceInMs,
                                  qint64 sourceDurationMs) {
    const std::optional<Uuid> id = parseId(clipId);
    if (!id) {
        return false;
    }
    const GestureResult r = vm_.trimClipStart(*id, Duration::fromMilliseconds(newSourceInMs),
                                              vm_.project().timelineFps,
                                              Duration::fromMilliseconds(sourceDurationMs));
    emit indicationChanged();
    return r.changed();
}

bool TimelineModel::trimClipEnd(const QString& clipId, qint64 newSourceOutMs,
                                qint64 sourceDurationMs) {
    const std::optional<Uuid> id = parseId(clipId);
    if (!id) {
        return false;
    }
    const GestureResult r = vm_.trimClipEnd(*id, Duration::fromMilliseconds(newSourceOutMs),
                                            vm_.project().timelineFps,
                                            Duration::fromMilliseconds(sourceDurationMs));
    emit indicationChanged();
    return r.changed();
}

bool TimelineModel::splitClip(const QString& clipId, qint64 playheadMs) {
    const std::optional<Uuid> id = parseId(clipId);
    if (!id) {
        return false;
    }
    const GestureResult r = vm_.splitClip(*id, Duration::fromMilliseconds(playheadMs));
    emit indicationChanged();
    return r.changed();
}

bool TimelineModel::reorderClips(const QString& trackId, const QStringList& clipIdOrder) {
    const std::optional<Uuid> track = parseId(trackId);
    if (!track) {
        return false;
    }
    std::vector<ClipId> order;
    order.reserve(static_cast<std::size_t>(clipIdOrder.size()));
    for (const QString& s : clipIdOrder) {
        const std::optional<Uuid> cid = parseId(s);
        if (!cid) {
            return false;
        }
        order.push_back(*cid);
    }
    const GestureResult r = vm_.reorderClips(*track, std::move(order));
    emit indicationChanged();
    return r.changed();
}

bool TimelineModel::undo() {
    const GestureResult r = vm_.undo();
    emit indicationChanged();
    return r.changed();
}

bool TimelineModel::redo() {
    const GestureResult r = vm_.redo();
    emit indicationChanged();
    return r.changed();
}

bool TimelineModel::addTrack(const QString& kind) {
    TrackKind parsed;
    if (kind == QStringLiteral("video")) {
        parsed = TrackKind::Video;
    } else if (kind == QStringLiteral("audio")) {
        parsed = TrackKind::Audio;
    } else {
        return false;
    }
    const GestureResult r = vm_.addTrack(parsed);
    emit indicationChanged();
    return r.changed();
}

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT
