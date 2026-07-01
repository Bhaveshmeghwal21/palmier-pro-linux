// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/KeyMomentMarkers.cpp — implementation of the key-moment marker
// overlay (Requirements 5.3, 5.4, 5.5). See KeyMomentMarkers.hpp for the contract.

#include "services/KeyMomentMarkers.hpp"

#include <utility>
#include <vector>

#include "core/TimelineEngine.hpp"

namespace palmier::services {

KeyMomentMarkerModel::KeyMomentMarkerModel()
    : observers_(std::make_shared<ObserverRegistry>()) {}

KeyMomentMarkerModel::~KeyMomentMarkerModel() = default;

Result<ClipMarkers> KeyMomentMarkerModel::classify(
    ClipId clipId, const Result<std::vector<KeyMoment>>& detection) {
    // 5.5: a detection failure carries through unchanged and yields no markers.
    if (detection.isError()) {
        return err<ClipMarkers>(detection.error());
    }

    const std::vector<KeyMoment>& moments = detection.value();

    ClipMarkers result;
    result.clipId = clipId;

    // 5.4: a completed detection with zero timestamps produces NO markers and a
    // distinct "no key moments" indication (a success, not an error).
    if (moments.empty()) {
        result.presence = MarkerPresence::NoKeyMoments;
        return result;
    }

    // 5.3: one marker per detected timestamp. The detector already returns them
    // in ascending, de-duplicated, in-range, ms-precision order (task 12.1), so
    // the markers inherit that ordering and bounding directly.
    result.presence = MarkerPresence::KeyMomentsFound;
    result.markers.reserve(moments.size());
    for (const KeyMoment& moment : moments) {
        result.markers.push_back(KeyMomentMarker{clipId, moment.timestamp});
    }
    return result;
}

Result<ClipMarkers> KeyMomentMarkerModel::record(
    ClipId clipId, const Result<std::vector<KeyMoment>>& detection) {
    Result<ClipMarkers> classified = classify(clipId, detection);
    // 5.5: on error, leave the clip's prior marker state untouched.
    if (classified.isError()) {
        return classified;
    }

    ClipMarkers markers = std::move(classified).value();
    byClip_[clipId] = markers;
    emit(markers);
    return markers;
}

Result<ClipMarkers> KeyMomentMarkerModel::detectAndRecord(KeyMomentDetector& detector,
                                                          const KeyMomentSource& source) {
    return record(source.clipId, detector.detect(source));
}

std::optional<ClipMarkers> KeyMomentMarkerModel::markersFor(ClipId clipId) const {
    const auto it = byClip_.find(clipId);
    if (it == byClip_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<MarkerPresence> KeyMomentMarkerModel::presenceFor(ClipId clipId) const {
    const auto it = byClip_.find(clipId);
    if (it == byClip_.end()) {
        return std::nullopt;
    }
    return it->second.presence;
}

bool KeyMomentMarkerModel::hasKeyMoments(ClipId clipId) const {
    const auto it = byClip_.find(clipId);
    return it != byClip_.end() && it->second.hasKeyMoments();
}

std::size_t KeyMomentMarkerModel::markerCount(ClipId clipId) const {
    const auto it = byClip_.find(clipId);
    return it == byClip_.end() ? 0u : it->second.count();
}

bool KeyMomentMarkerModel::clear(ClipId clipId) { return byClip_.erase(clipId) > 0; }

void KeyMomentMarkerModel::clearAll() noexcept { byClip_.clear(); }

void KeyMomentMarkerModel::pruneRemovedClips(const ChangeSet& change) {
    for (const ClipId& removed : change.removedClips) {
        byClip_.erase(removed);
    }
}

void KeyMomentMarkerModel::attachTimeline(TimelineEngine& engine) {
    timelineSubscriptions_.push_back(
        engine.observe([this](const ChangeSet& change) { pruneRemovedClips(change); }));
}

void KeyMomentMarkerModel::detachAll() noexcept { timelineSubscriptions_.clear(); }

Subscription KeyMomentMarkerModel::observe(std::function<void(const ClipMarkers&)> callback) {
    if (!callback) {
        return Subscription{};  // inactive handle; nothing registered
    }

    const std::uint64_t id = observers_->nextId++;
    observers_->callbacks.emplace(id, std::move(callback));

    // Capture a weak reference so the unsubscribe thunk stays safe even if the
    // model (and its registry) is destroyed before the Subscription.
    std::weak_ptr<ObserverRegistry> weak = observers_;
    return Subscription([weak, id]() noexcept {
        if (auto registry = weak.lock()) {
            registry->callbacks.erase(id);
        }
    });
}

void KeyMomentMarkerModel::emit(const ClipMarkers& markers) const {
    // Iterate over a snapshot so an observer that (un)subscribes from within its
    // callback cannot invalidate the iteration.
    std::vector<std::function<void(const ClipMarkers&)>> current;
    current.reserve(observers_->callbacks.size());
    for (const auto& [id, callback] : observers_->callbacks) {
        current.push_back(callback);
    }
    for (const auto& callback : current) {
        if (callback) {
            callback(markers);
        }
    }
}

}  // namespace palmier::services
