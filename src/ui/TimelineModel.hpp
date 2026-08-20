// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/TimelineModel.hpp — the Qt 6 QAbstractItemModel binding for the timeline
// (task 19.2).
//
// This is the thin Qt half of the Timeline view. It exposes the project to QML
// as a two-level tree — top-level rows are tracks (1-50, Requirement 2.1), and
// each track's child rows are its clips in timelineStart order — and forwards
// the interactive gestures (drag-move, trim, split, reorder) to the Qt-free
// TimelineViewModel, which maps each one to the shared EditCommand path. All the
// editing logic, indication classification, and change reflection live in the
// (unit-tested, Qt-free) TimelineViewModel; this class only adapts it to
// QAbstractItemModel / Q_INVOKABLE so QML can bind to it.
//
// The entire translation unit is guarded by PALMIER_HAVE_QT (mirroring
// MainWindow), so the module tree still configures and builds where Qt 6 is not
// installed; the compiled model is produced only when Qt is found. The Qt-free
// TimelineViewModel it wraps is always built and always tested.

#ifndef PALMIER_UI_TIMELINEMODEL_HPP
#define PALMIER_UI_TIMELINEMODEL_HPP

#ifdef PALMIER_HAVE_QT

#include <QAbstractItemModel>
#include <QHash>
#include <QByteArray>
#include <QModelIndex>
#include <QString>
#include <QVariant>

#include "core/TimelineEngine.hpp"
#include "ui/GuiToolGateway.hpp"
#include "ui/TimelineViewModel.hpp"

namespace palmier::ui {

/// QAbstractItemModel over a TimelineEngine, backed by TimelineViewModel.
///
/// Tree layout:
///   * invalid parent  -> track rows      (rowCount == trackCount)
///   * a track index    -> its clip rows   (rowCount == clipCount(track))
///   * a clip index     -> no children
class TimelineModel : public QAbstractItemModel {
    Q_OBJECT
    Q_PROPERTY(int trackCount READ trackCount NOTIFY modelRefreshed)
    Q_PROPERTY(QString lastIndication READ lastIndication NOTIFY indicationChanged)
    Q_PROPERTY(QString lastMessage READ lastMessage NOTIFY indicationChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY modelRefreshed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY modelRefreshed)

public:
    /// Roles exposed to QML. Track roles apply to top-level rows; clip roles to
    /// child rows. IsTrackRole disambiguates the two levels in a delegate.
    enum Roles {
        IsTrackRole = Qt::UserRole + 1,
        // Track roles
        TrackIdRole,
        TrackKindRole,     ///< "video" or "audio"
        TrackMutedRole,
        TrackLockedRole,
        ClipCountRole,
        // Clip roles
        ClipIdRole,
        ClipStartMsRole,
        ClipDurationMsRole,
        ClipSourceInMsRole,
        ClipSourceOutMsRole,
        ClipOpacityRole,
        ClipGainRole,
        ClipHasTransitionRole,
        ClipEffectCountRole,
    };

    /// `engine` must outlive the model. When `gateway` is non-null (it must
    /// then outlive the model too), every gesture invoked from QML routes
    /// through it instead of calling TimelineEngine::apply directly (task 11.4).
    explicit TimelineModel(TimelineEngine& engine, QObject* parent = nullptr,
                           GuiToolGateway* gateway = nullptr);
    ~TimelineModel() override;

    // --- QAbstractItemModel ------------------------------------------------
    QModelIndex index(int row, int column, const QModelIndex& parent) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent) const override;
    int columnCount(const QModelIndex& parent) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // --- Property getters --------------------------------------------------
    [[nodiscard]] int trackCount() const { return static_cast<int>(vm_.trackCount()); }
    [[nodiscard]] QString lastIndication() const;
    [[nodiscard]] QString lastMessage() const { return QString::fromStdString(vm_.lastMessage()); }
    [[nodiscard]] bool canUndo() const { return vm_.canUndo(); }
    [[nodiscard]] bool canRedo() const { return vm_.canRedo(); }

    // --- Gestures invokable from QML ---------------------------------------
    // Positions/boundaries are milliseconds on the timeline / in source time; a
    // clip id is its canonical UUID string. Each returns true iff the gesture
    // changed the project; the indication (invalid-drop / nothing-to-split /
    // no-op) is published via the lastIndication property regardless.
    Q_INVOKABLE bool moveClip(const QString& clipId, qint64 newStartMs);
    Q_INVOKABLE bool trimClipStart(const QString& clipId, qint64 newSourceInMs,
                                   qint64 sourceDurationMs);
    Q_INVOKABLE bool trimClipEnd(const QString& clipId, qint64 newSourceOutMs,
                                 qint64 sourceDurationMs);
    Q_INVOKABLE bool splitClip(const QString& clipId, qint64 playheadMs);
    Q_INVOKABLE bool reorderClips(const QString& trackId, const QStringList& clipIdOrder);
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();

signals:
    void modelRefreshed();
    void indicationChanged();

private:
    // Sentinel internalId marking a top-level (track) index; clip indices store
    // their parent track row in internalId instead.
    static constexpr quintptr kTrackTag = ~static_cast<quintptr>(0);

    [[nodiscard]] bool isTrackIndex(const QModelIndex& index) const;

    TimelineViewModel vm_;
};

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT

#endif  // PALMIER_UI_TIMELINEMODEL_HPP
