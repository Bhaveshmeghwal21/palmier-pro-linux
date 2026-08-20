// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/ComponentConstructionError.hpp — the startup construction guard's error
// type (task 11.7; Requirements 1.1, 1.9).
//
// ApplicationComposition's constructor is designed, throughout, to degrade
// gracefully rather than fail outright: a missing GPU degrades to software
// compositing, an unconfigured generative backend degrades to the offline
// stub, an unset audio device degrades to the null sink, and so on
// (Requirement 10.4/13.3's graceful-degradation contract, exercised by the
// property tests in tests/gpu_graceful_degradation_property_test.cpp and
// tests/services/offline_mode_availability_test.cpp). None of the components
// Requirement 1.1 names are therefore EXPECTED to throw during construction on
// a supported host.
//
// This type exists for the residual case design.md's own error-handling table
// already accounts for at the layer below (e.g. a filesystem error opening the
// secret store, or a std::bad_alloc): if any exception escapes
// ApplicationComposition's constructor, the entry point (src/app/main.cpp)
// catches it, names the component that failed and the underlying reason, and
// exits WITHOUT constructing or showing the editor shell (Requirement 1.9) —
// exactly like the platform-incompatibility path already does for a missing
// runtime dependency.

#ifndef PALMIER_APP_COMPONENTCONSTRUCTIONERROR_HPP
#define PALMIER_APP_COMPONENTCONSTRUCTIONERROR_HPP

#include <exception>
#include <string>

namespace palmier::app {

/// Thrown-and-caught-at-the-boundary description of a failed component
/// construction. `componentName` should name the Requirement 1.1 component
/// (e.g. "GpuContext", "ProjectSession", "McpToolExecutor") whose construction
/// could not complete; `reason` is the human-readable cause.
class ComponentConstructionError : public std::exception {
public:
    ComponentConstructionError(std::string componentName, std::string reason)
        : componentName_(std::move(componentName)), reason_(std::move(reason)) {
        message_ = "failed to construct '" + componentName_ + "': " + reason_;
    }

    [[nodiscard]] const std::string& componentName() const noexcept { return componentName_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

    [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string componentName_;
    std::string reason_;
    std::string message_;
};

}  // namespace palmier::app

#endif  // PALMIER_APP_COMPONENTCONSTRUCTIONERROR_HPP
