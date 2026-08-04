# SPDX-License-Identifier: GPL-3.0-or-later
#
# tests — GoogleTest (unit) + RapidCheck (property-based) test tree.
#
# This file wires up the two testing frameworks used across the project and
# registers the resulting test binaries with CTest:
#
#   * GoogleTest  — example-based unit tests (see design "Unit testing").
#   * RapidCheck  — property-based tests for correctness properties P1-P12
#                   (see design "Property-based testing"). RapidCheck's
#                   GoogleTest integration (RC_GTEST_PROP) lets property tests
#                   run inside the same GoogleTest binary and be discovered by
#                   CTest like any other test case.
#
# Both frameworks are pulled in with FetchContent so a checkout builds and
# tests without a system-wide install. Pins are exact (a release tag for
# GoogleTest, a commit for RapidCheck, which publishes no releases) for
# reproducible builds.
#
# Property-test conventions enforced project-wide (see design "Testing
# Strategy"):
#   * Each property test runs a MINIMUM of 100 iterations. RapidCheck's default
#     max_success is 100; we also set RC_PARAMS in the test environment so the
#     floor is explicit and cannot silently regress.
#   * Every registered test runs under a per-test time limit of 600 seconds;
#     CTest fails a test that exceeds it and names the limit in the report.
#   * Each property test is tagged, in the form:
#         Feature: palmier-pro-linux, Property {n}: {text}
#     and references the requirement clause(s) it validates.
#
# The placeholder property test (palmier_placeholder_property_test.cpp) exists
# only to demonstrate this wiring, the tag format, and the 100-iteration floor;
# real property tests for P1-P12 are added by later tasks and replace it.

include(FetchContent)

# ---------------------------------------------------------------------------
# Minimum number of successful cases every property test must exercise.
# Surfaced as a CMake cache variable so CI can raise (never lower) it, and
# injected into each test's environment via RC_PARAMS below.
# ---------------------------------------------------------------------------
set(PALMIER_PBT_MIN_SUCCESS 100 CACHE STRING
    "Minimum successful iterations per RapidCheck property test (>= 100)")

# The 100-case floor is a hard requirement (Requirement 15.2), so a lower value
# — however it was supplied — is raised back to the floor rather than honoured.
if(PALMIER_PBT_MIN_SUCCESS LESS 100)
    message(WARNING
        "PALMIER_PBT_MIN_SUCCESS=${PALMIER_PBT_MIN_SUCCESS} is below the required "
        "floor of 100 generated cases per property test; using 100 instead.")
    set(PALMIER_PBT_MIN_SUCCESS 100 CACHE STRING
        "Minimum successful iterations per RapidCheck property test (>= 100)" FORCE)
endif()

# ---------------------------------------------------------------------------
# Per-test wall-clock limit in seconds (Requirement 15.8). Applied to every
# test registered through palmier_register_test(); CTest reports a test that
# exceeds it as failed, naming the limit it exceeded.
# ---------------------------------------------------------------------------
set(PALMIER_TEST_TIMEOUT_SECONDS 600)

# ---------------------------------------------------------------------------
# GoogleTest — example-based unit tests.
# ---------------------------------------------------------------------------
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2  # b514bdc898e2951020cbdca1304b75f5950d1f59
    GIT_SHALLOW    TRUE)

# Do not install GoogleTest with our project, and reuse our shared CRT on MSVC.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# ---------------------------------------------------------------------------
# RapidCheck — property-based tests. Enable its GoogleTest integration so we
# can write RC_GTEST_PROP(...) properties that CTest discovers as test cases.
# RapidCheck has no tagged releases, so we pin an exact commit.
# ---------------------------------------------------------------------------
FetchContent_Declare(rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG        b2d9ed2dddefc4b84318d664b4f221eb792d89c7
    GIT_SHALLOW    FALSE)

set(RC_ENABLE_GTEST ON CACHE BOOL "Build the RapidCheck GoogleTest integration" FORCE)
set(RC_ENABLE_TESTS OFF CACHE BOOL "Do not build RapidCheck's own test suite" FORCE)
set(RC_ENABLE_EXAMPLES OFF CACHE BOOL "Do not build RapidCheck's examples" FORCE)

FetchContent_MakeAvailable(googletest rapidcheck)

# gtest_discover_tests() lives in the GoogleTest CMake module.
include(GoogleTest)

# ---------------------------------------------------------------------------
# palmier_test_support — INTERFACE target bundling everything a test binary
# needs: GoogleTest, RapidCheck, and RapidCheck's GoogleTest bridge. Test
# targets added by later tasks just link this one.
# ---------------------------------------------------------------------------
add_library(palmier_test_support INTERFACE)
add_library(Palmier::test_support ALIAS palmier_test_support)
target_link_libraries(palmier_test_support INTERFACE
    GTest::gtest
    GTest::gtest_main
    rapidcheck
    rapidcheck_gtest)
target_compile_features(palmier_test_support INTERFACE cxx_std_20)

# ---------------------------------------------------------------------------
# palmier_register_test(<target>)
#
# Registers a GoogleTest/RapidCheck binary with CTest via test discovery and
# pins the project-wide test conventions onto every discovered test case:
#
#   * ENVIRONMENT RC_PARAMS=max_success=${PALMIER_PBT_MIN_SUCCESS} — the
#     property-test iteration floor, so every RapidCheck property runs at least
#     PALMIER_PBT_MIN_SUCCESS (>= 100) generated cases (Requirement 15.2).
#   * TIMEOUT ${PALMIER_TEST_TIMEOUT_SECONDS} — the per-test time limit of 600
#     seconds; CTest terminates and fails any test that exceeds it, reporting
#     the timeout together with the limit that was exceeded (Requirement 15.8).
# ---------------------------------------------------------------------------
function(palmier_register_test target)
    gtest_discover_tests(${target}
        PROPERTIES
            LABELS "palmier"
            TIMEOUT ${PALMIER_TEST_TIMEOUT_SECONDS}
            ENVIRONMENT "RC_PARAMS=max_success=${PALMIER_PBT_MIN_SUCCESS}")
endfunction()

# ---------------------------------------------------------------------------
# palmier_test_enable_openssl(<target>)
#
# Attaches the optional TLS transport to a test binary that compiles
# src/services/TlsTransport.cpp directly (task 6.3): the PALMIER_HAVE_OPENSSL
# guard and the OpenSSL imported target are added ONLY when the option is ON and
# OpenSSL 3.x was found — the same ENABLE-and-FOUND form src/services uses. With
# the guard absent the same sources still compile and report the capability
# unavailable, so a host without OpenSSL builds and runs this suite unchanged and
# the TLS-specific cases skip with a recorded reason.
# ---------------------------------------------------------------------------
function(palmier_test_enable_openssl target)
    if(PALMIER_ENABLE_OPENSSL AND PALMIER_OPENSSL_AVAILABLE)
        target_compile_definitions(${target} PRIVATE PALMIER_HAVE_OPENSSL=1)
        target_link_libraries(${target} PRIVATE PkgConfig::OPENSSL)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Placeholder test binary — proves the GoogleTest + RapidCheck wiring, the
# required property tag format, and the 100-iteration floor. Replaced by the
# real P1-P12 property tests in later tasks.
# ---------------------------------------------------------------------------
add_executable(palmier_placeholder_tests
    palmier_placeholder_property_test.cpp)
target_link_libraries(palmier_placeholder_tests PRIVATE Palmier::test_support)

palmier_register_test(palmier_placeholder_tests)

# ---------------------------------------------------------------------------
# GPU abstraction layer — unit tests for device selection, capability probing,
# the software fallback, and persisted GPU selection (task 7.1). These drive
# GpuContext through a synthetic device enumerator so they run without a Vulkan
# loader or physical GPU.
# ---------------------------------------------------------------------------
add_executable(palmier_gpu_tests
    gpu_context_test.cpp
    gpu_capability_probe_test.cpp
    gpu_codec_bridge_test.cpp
    frame_pool_test.cpp
    gpu_compositor_test.cpp
    gpu_effect_kernels_test.cpp)
target_link_libraries(palmier_gpu_tests PRIVATE
    Palmier::test_support
    Palmier::gpu)

palmier_register_test(palmier_gpu_tests)

# ---------------------------------------------------------------------------
# Core domain tests — unit tests for the project data model and validation
# rules (task 2.2). Links the core library under test alongside the shared
# GoogleTest/RapidCheck support target.
# ---------------------------------------------------------------------------
add_executable(palmier_core_tests
    core/project_validation_test.cpp
    core/undo_redo_stack_test.cpp
    core/timeline_engine_test.cpp)
target_link_libraries(palmier_core_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_core_tests)

# ---------------------------------------------------------------------------
# Authentication service unit tests (Requirements 9.2-9.4). The auth source is
# compiled directly into the test binary so the login/lockout policy can be
# exercised in isolation, without pulling in the media/FFmpeg dependencies that
# the full Palmier::services library links. AuthenticationService references the
# BYOK manager (for its optional BYOK integration), so that source is compiled
# in too.
# ---------------------------------------------------------------------------
add_executable(palmier_services_auth_tests
    AuthenticationService_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp")
target_include_directories(palmier_services_auth_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_auth_tests PRIVATE Palmier::test_support)

palmier_register_test(palmier_services_auth_tests)

# ---------------------------------------------------------------------------
# BYOK credential unit tests (Requirements 9.5, 9.6). Exercise the validate ->
# persist -> authorize policy and the AuthenticationService BYOK integration
# with a scriptable provider-validator fake and the in-memory secret store.
# SecretStore.cpp is compiled WITHOUT the PALMIER_HAVE_LIBSECRET define, so the
# real libsecret path is not linked and makeSystemSecretStore() returns the
# Unsupported stub the test asserts on — keeping the binary free of libsecret /
# Secret Service dependencies.
# ---------------------------------------------------------------------------
add_executable(palmier_services_byok_tests
    ByokCredentialManager_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/SecretStore.cpp")
target_include_directories(palmier_services_byok_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_byok_tests PRIVATE Palmier::test_support)

palmier_register_test(palmier_services_byok_tests)

# ---------------------------------------------------------------------------
# Media engine tests — unit tests for MediaDecoder's open/nextFrame/seek surface
# and its HW-preferred / SW-fallback routing and zero-copy FramePool frames
# (task 8.2; Requirements 3.1, 10.2, 10.5). These drive the decoder through the
# IDecodeBackend seam with a mock backend and a host-memory FramePool, so they
# run without a GPU, FFmpeg, or any vendor SDK. Links Palmier::media (which
# brings in Palmier::gpu and Palmier::core).
# ---------------------------------------------------------------------------
add_executable(palmier_media_tests
    media_decoder_test.cpp)
target_link_libraries(palmier_media_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_tests)

# ---------------------------------------------------------------------------
# Project store unit tests (task 5.1; Requirements 3.5, 3.6). The .palmier
# serializer/deserializer is compiled directly into the test binary alongside
# the core library so the persistence round-trip can be exercised in isolation,
# without pulling in the media/FFmpeg, libsecret, or lcms2 dependencies that the
# full Palmier::services library links. ProjectStore depends only on Palmier::core.
# ---------------------------------------------------------------------------
add_executable(palmier_services_project_store_tests
    services/project_store_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp")
target_include_directories(palmier_services_project_store_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_project_store_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_project_store_tests)


# ---------------------------------------------------------------------------
# Localization manager unit tests (Requirements 12.1-12.5, task 17.2). The
# LocalizationManager source is compiled directly into the test binary so the
# language-set / fallback / default-selection / persistence policy is exercised
# in isolation, without pulling in the media/FFmpeg/libsecret dependencies that
# the full Palmier::services library links. The manager is Qt-free and
# dependency-free (standard library + core Result/Error only).
# ---------------------------------------------------------------------------
add_executable(palmier_services_localization_tests
    LocalizationManager_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/LocalizationManager.cpp")
target_include_directories(palmier_services_localization_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_localization_tests PRIVATE Palmier::test_support)

palmier_register_test(palmier_services_localization_tests)


# ---------------------------------------------------------------------------
# Media import validation unit tests (task 8.5; Requirements 3.2, 3.3). Drive
# validateMediaImport through MediaProbe's injectable backend seam to cover the
# accept path, the unsupported-format rejection (names the format), and the
# unreadable-file rejection (indicates unreadable) — all without FFmpeg or real
# media. Links Palmier::media (which brings in Palmier::gpu and Palmier::core).
# ---------------------------------------------------------------------------
add_executable(palmier_media_import_validation_tests
    media_import_validation_test.cpp)
target_link_libraries(palmier_media_import_validation_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_import_validation_tests)


# ---------------------------------------------------------------------------
# Media manager unit tests (task 6.1; Requirements 3.1, 3.4). Exercise the
# project media library (import/availability) and the per-clip generated-clip
# version history, including the "at least the 10 most recent versions retained
# and selectable" bound. MediaManager is pure domain data, so this links only
# Palmier::core alongside the shared GoogleTest/RapidCheck support target. (The
# dedicated version-retention property test P12 is added by task 6.2.)
# ---------------------------------------------------------------------------
add_executable(palmier_core_media_manager_tests
    core/media_manager_test.cpp)
target_link_libraries(palmier_core_media_manager_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_core_media_manager_tests)


# ---------------------------------------------------------------------------
# Transcription service unit tests (task 11.1; Requirements 4.1-4.6). The
# TranscriptionService source is compiled directly into the test binary
# alongside the core library so the ordering/validation, association,
# empty-audio, and failure policy is exercised in isolation via a mock
# ITranscriptionBackend, without pulling in the media/FFmpeg, libsecret, or
# lcms2 dependencies that the full Palmier::services library links.
# TranscriptionService depends only on Palmier::core.
# ---------------------------------------------------------------------------
add_executable(palmier_services_transcription_tests
    TranscriptionService_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/TranscriptionService.cpp")
target_include_directories(palmier_services_transcription_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_transcription_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_transcription_tests)


# ---------------------------------------------------------------------------
# Audio graph tests — unit tests for the audio graph's FFmpeg-free numeric core
# (task 8.4; Requirement 2.8): buffer/rate math, channel up/down-mix, the linear
# resampler, multi-source mixing (sum + clamp with gain), PCM pack/unpack, and
# the AudioGraph orchestration. These run without FFmpeg, a GPU, or any vendor
# SDK; the libswresample backend (when compiled in) satisfies the same IResampler
# seam. Links Palmier::media (which brings in Palmier::gpu and Palmier::core).
# ---------------------------------------------------------------------------
add_executable(palmier_media_audio_tests
    media_audio_graph_test.cpp)
target_link_libraries(palmier_media_audio_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_audio_tests)


# ---------------------------------------------------------------------------
# Media encoder tests — unit tests for MediaEncoder's create/submit/finish
# surface and its HW-preferred / SW-fallback-on-init routing, GPU-vs-CPU frame
# acceptance, and the resolution / presentation-order guards (task 8.3;
# Requirements 10.2, 10.5). These drive the encoder through the IEncodeBackend
# seam with a mock backend and a host-memory FramePool, so they run without a
# GPU, FFmpeg, or any vendor SDK. Links Palmier::media (which brings in
# Palmier::gpu and Palmier::core).
# ---------------------------------------------------------------------------
add_executable(palmier_media_encoder_tests
    media_encoder_test.cpp)
target_link_libraries(palmier_media_encoder_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_encoder_tests)



# ---------------------------------------------------------------------------
# Generative AI client unit tests (task 14.1; Requirements 6.1, 6.3, 6.4, 6.6,
# 6.8). The GenerativeClient source is compiled directly into the test binary
# alongside the core library so the submit/poll/fetch lifecycle, the 300-second
# timeout+cancel budget, and provider-failure handling are exercised in
# isolation via a mock IGenerativeBackend and an injectable clock — without a
# real network or the media/FFmpeg, libsecret, or lcms2 dependencies that the
# full Palmier::services library links. GenerativeClient depends only on
# Palmier::core.
# ---------------------------------------------------------------------------
add_executable(palmier_services_generative_tests
    GenerativeClient_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeClient.cpp")
target_include_directories(palmier_services_generative_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_generative_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_generative_tests)


# ---------------------------------------------------------------------------
# Project save-service unit tests (task 5.3; Requirements 3.6, 3.7). Exercise the
# save success/failure handling layered on the `.palmier` store: a completed save
# writes the complete state to the single target and reports success, while a save
# that fails (inaccessible location, insufficient permissions, insufficient disk
# space) preserves the last successfully saved file byte-for-byte via the atomic
# write-to-temp-then-rename sequence and leaves the recorded last-saved status
# unchanged. Disk-space / permission failures are injected through the
# RawFileWriter seam, so the tests need no real full disk or root privileges. The
# ProjectSaveService and ProjectStore sources are compiled directly into the test
# binary alongside Palmier::core, without the media/FFmpeg, libsecret, or lcms2
# dependencies that the full Palmier::services library links.
# ---------------------------------------------------------------------------
add_executable(palmier_services_project_save_tests
    services/project_save_service_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp")
target_include_directories(palmier_services_project_save_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_project_save_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_project_save_tests)



# ---------------------------------------------------------------------------
# Key-moment detector unit tests (task 12.1; Requirements 5.1, 5.2, 5.5). The
# KeyMomentDetector source is compiled directly into the test binary alongside
# the core library so the bounds enforcement, count cap, ordering/de-duplication,
# and empty/zero-duration/failure policy is exercised in isolation via a mock
# IKeyMomentBackend, without pulling in the media/FFmpeg, libsecret, or lcms2
# dependencies that the full Palmier::services library links. KeyMomentDetector
# depends only on Palmier::core.
# ---------------------------------------------------------------------------
add_executable(palmier_services_key_moment_tests
    KeyMomentDetector_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/KeyMomentDetector.cpp")
target_include_directories(palmier_services_key_moment_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_key_moment_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_key_moment_tests)



# ---------------------------------------------------------------------------
# Editing command unit tests (task 3.3; Requirements 2.2-2.7). Exercise the
# concrete EditCommand subclasses (AddClip, DeleteClip, MoveClip, TrimClip,
# SplitClip, ReorderClips, AddEffect): their edit semantics, the overlap /
# out-of-range rejection paths, and the apply()/revert() round-trip (driven both
# directly against a Project and through the TimelineEngine so invariant
# enforcement and atomic rollback are covered). Links Palmier::core alongside the
# shared GoogleTest/RapidCheck support target. (The dedicated property tests for
# these commands — P3, P8, P9, P10 — are added by tasks 3.5-3.8.)
# ---------------------------------------------------------------------------
add_executable(palmier_core_edit_commands_tests
    core/edit_commands_test.cpp)
target_link_libraries(palmier_core_edit_commands_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_core_edit_commands_tests)



# ---------------------------------------------------------------------------
# Generative media coordinator unit tests (task 14.2; Requirements 6.2, 6.5,
# 6.7, 9.7). Exercise prompt-length validation, entitlement gating (mock gate
# plus the real AuthServiceGenerationGate over AuthenticationService, including
# the BYOK path), and successful generate-and-place through the real
# TimelineEnginePlacer + MediaManager. The coordinator and its collaborators are
# compiled directly into the test binary so the policy runs in isolation, via a
# mock generation gate/runner and an in-memory secret store, without a real
# network, GPU, or the media/FFmpeg/libsecret dependencies that the full
# Palmier::services library links. SecretStore.cpp is intentionally omitted
# (InMemorySecretStore is header-only), keeping the binary free of libsecret.
# ---------------------------------------------------------------------------
add_executable(palmier_services_generative_coordinator_tests
    GenerativeMediaCoordinator_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeMediaCoordinator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeClient.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp")
target_include_directories(palmier_services_generative_coordinator_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_generative_coordinator_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_generative_coordinator_tests)



# ---------------------------------------------------------------------------
# Media manager version-retention property test (task 6.2; Requirement 3.4).
# The dedicated RapidCheck property P12 (generated-clip version retention): a
# clip replaced by a generated clip keeps its prior version retained and
# selectable, and at least the 10 most recent versions stay selectable. Drives
# arbitrary sequences of generated-clip replacements against the pure-domain
# MediaManager, so this links only Palmier::core alongside the shared
# GoogleTest/RapidCheck support target.
# ---------------------------------------------------------------------------
add_executable(palmier_core_media_manager_version_retention_tests
    core/media_manager_version_retention_property_test.cpp)
target_link_libraries(palmier_core_media_manager_version_retention_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_core_media_manager_version_retention_tests)



# ---------------------------------------------------------------------------
# Project store property test (task 5.2; Requirement 3.5). RapidCheck property
# P11: for any valid project, serialize -> deserialize yields an equivalent
# project (all clips, tracks, edits, and media references preserved). Like the
# project-store unit tests, the `.palmier` serializer/deserializer is compiled
# directly into the test binary alongside the core library so the round-trip is
# exercised in isolation, without pulling in the media/FFmpeg, libsecret, or
# lcms2 dependencies that the full Palmier::services library links. ProjectStore
# depends only on Palmier::core.
# ---------------------------------------------------------------------------
add_executable(palmier_services_project_store_property_tests
    services/project_store_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp")
target_include_directories(palmier_services_project_store_property_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_project_store_property_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_project_store_property_tests)



# ---------------------------------------------------------------------------
# Timeline ordering / non-overlap property test (task 3.5; Requirements 2.2,
# 2.3). RapidCheck property P3: after any sequence of editing commands
# (add/move/trim/split/reorder/delete) driven through a TimelineEngine, every
# track's clips remain ordered by timelineStart and non-overlapping outside an
# incoming clip's explicit transition region. Drives arbitrary command sequences
# against the engine's invariant enforcement and the concrete EditCommands, so
# this links only Palmier::core alongside the shared GoogleTest/RapidCheck
# support target.
# ---------------------------------------------------------------------------
add_executable(palmier_core_timeline_ordering_property_tests
    core/timeline_ordering_property_test.cpp)
target_link_libraries(palmier_core_timeline_ordering_property_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_core_timeline_ordering_property_tests)



# ---------------------------------------------------------------------------
# Timeline undo/redo round-trip property test (task 3.4; Requirement 2.9).
# RapidCheck property P1: for any command, apply() then undo() restores the
# exact prior project state, and redo() reproduces the post-apply state. Drives
# arbitrary sequences of the concrete EditCommands (add/delete/move/trim/split/
# reorder/add-effect) through a TimelineEngine, snapshotting before and after
# each applied command and asserting the undo/redo round-trip. Links only
# Palmier::core alongside the shared GoogleTest/RapidCheck support target.
# ---------------------------------------------------------------------------
add_executable(palmier_core_timeline_undo_redo_roundtrip_property_tests
    core/timeline_undo_redo_roundtrip_property_test.cpp)
target_link_libraries(palmier_core_timeline_undo_redo_roundtrip_property_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_core_timeline_undo_redo_roundtrip_property_tests)



# ---------------------------------------------------------------------------
# Key-moment marker overlay unit tests (task 12.2; Requirements 5.3, 5.4, 5.5).
# Exercise the mapping from a detection result to timeline marker data: one
# marker per detected timestamp (5.3), the distinct "no key moments" indication
# with zero markers (5.4), a detection error recording nothing (5.5), the query
# and observer surface, and the TimelineEngine integration (markers pruned when a
# clip is removed). The KeyMomentMarkers and KeyMomentDetector sources are
# compiled directly into the test binary alongside Palmier::core (which supplies
# the real TimelineEngine and EditCommands), without the media/FFmpeg, libsecret,
# or lcms2 dependencies that the full Palmier::services library links.
# ---------------------------------------------------------------------------
add_executable(palmier_services_key_moment_markers_tests
    KeyMomentMarkers_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/KeyMomentMarkers.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/KeyMomentDetector.cpp")
target_include_directories(palmier_services_key_moment_markers_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_key_moment_markers_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_key_moment_markers_tests)



# ---------------------------------------------------------------------------
# Export engine tests — unit tests for the export render loop + progress
# reporting (task 10.1; Requirements 11.1, 11.2, 10.3, 10.8). Drive
# ExportEngine::run with a real software Compositor (fed synthetic frames) and a
# MediaEncoder built behind the IEncodeBackend seam with a mock backend, so the
# full loop — frame stepping, strictly increasing presentation order, monotonic
# 0-100% progress, HW-preferred routing, and error propagation — runs without a
# GPU, FFmpeg, or any vendor SDK, and without mutating the source timeline. Links
# Palmier::media (which brings in Palmier::gpu and Palmier::core).
# ---------------------------------------------------------------------------
add_executable(palmier_media_export_tests
    media_export_engine_test.cpp)
target_link_libraries(palmier_media_export_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_export_tests)



# ---------------------------------------------------------------------------
# Timeline split contiguity / duration-conservation property test (task 3.6;
# Requirement 2.5). RapidCheck property P8: splitting a clip at any interior
# playhead yields two contiguous, non-overlapping clips whose combined timeline
# duration equals the original clip duration and whose combined source range
# equals the original source range. Drives the SplitClipCommand through a
# TimelineEngine over arbitrary clip geometries and interior playhead positions,
# so this links only Palmier::core alongside the shared GoogleTest/RapidCheck
# support target.
# ---------------------------------------------------------------------------
add_executable(palmier_core_timeline_split_property_tests
    core/timeline_split_property_test.cpp)
target_link_libraries(palmier_core_timeline_split_property_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_core_timeline_split_property_tests)



# ---------------------------------------------------------------------------
# Timeline reorder clip-count property test (task 3.7; Requirement 2.7).
# RapidCheck property P9 (reorder preserves clip count): for any track and any
# reordering of its clips — a valid permutation (accepted) or a malformed request
# such as a wrong-length list, an unknown id, or a duplicate (rejected and rolled
# back) — the track's total clip count is unchanged. Drives arbitrary sequences
# of ReorderClipsCommand attempts through a TimelineEngine and asserts the count
# after every attempt, so this links only Palmier::core alongside the shared
# GoogleTest/RapidCheck support target.
# ---------------------------------------------------------------------------
add_executable(palmier_core_timeline_reorder_property_tests
    core/timeline_reorder_clip_count_property_test.cpp)
target_link_libraries(palmier_core_timeline_reorder_property_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_core_timeline_reorder_property_tests)



# ---------------------------------------------------------------------------
# Timeline trim duration property test (task 3.8; Requirement 2.4). RapidCheck
# property P10: for any clip and any valid trim boundary, after trimming the
# clip's duration equals `sourceOut - sourceIn` for the new boundary, constrained
# to [1 frame, source duration]. Drives TrimClipCommand on both edges with
# whole-frame boundaries generated inside the permitted band, and independently
# asserts the moved edge lands on the boundary, the duration matches, and the
# retained content keeps its timeline position. Links only Palmier::core
# alongside the shared GoogleTest/RapidCheck support target.
# ---------------------------------------------------------------------------
add_executable(palmier_core_timeline_trim_property_tests
    core/timeline_trim_duration_property_test.cpp)
target_link_libraries(palmier_core_timeline_trim_property_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_core_timeline_trim_property_tests)



# ---------------------------------------------------------------------------
# Clip math edge case + undo-empty / split-miss unit tests (task 3.9;
# Requirements 2.6, 2.10). Cover the clip/duration/frame arithmetic underpinning
# the timeline (duration()/timelineEnd() identities, zero-length, single-tick,
# and one-frame clips, and the frame <-> duration round-trip at boundary counts),
# the undo/redo empty-history no-op-with-indication behaviour (2.10), and the
# split-when-the-playhead-misses-all-clips rejection that leaves the project
# unchanged (2.6). Drives the real TimelineEngine and SplitClipCommand, so this
# links only Palmier::core alongside the shared GoogleTest/RapidCheck support
# target.
# ---------------------------------------------------------------------------
add_executable(palmier_core_clip_math_tests
    core/clip_math_edge_cases_test.cpp)
target_link_libraries(palmier_core_clip_math_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_core_clip_math_tests)



# ---------------------------------------------------------------------------
# GPU/CPU parity property test (task 7.6; Requirement 10.7). RapidCheck property
# P5: for any source frame and effect parameters, GPU vs software compositing
# differ by no more than 1 per channel on a 0-255 scale (golden-image
# comparison). Because this sandbox has no Vulkan/GPU, the test drives two lanes
# through the available abstraction: the production software fallback
# (applyEffectSoftware / applyTransitionSoftware) as the CPU lane, and an
# independent normalized-space reference model of the SPIR-V effect kernels'
# semantics (mirroring the GLSL in EffectKernels.cpp) as the GPU lane, asserting
# per-channel parity within the tolerance. It is ready to compare against a real
# compute-shader dispatch when a device is present. Links Palmier::gpu (which
# supplies the software compositing reference and the kernel sources) alongside
# the shared GoogleTest/RapidCheck support target.
# ---------------------------------------------------------------------------
add_executable(palmier_gpu_parity_property_tests
    gpu_gpu_cpu_parity_property_test.cpp)
target_link_libraries(palmier_gpu_parity_property_tests PRIVATE
    Palmier::gpu
    Palmier::test_support)

palmier_register_test(palmier_gpu_parity_property_tests)



# ---------------------------------------------------------------------------
# Generation validation & auth-gating unit tests (task 14.4; Requirements 6.5,
# 6.7, 6.8). Targeted cases that complement the broader GenerativeClient (14.1)
# and GenerativeMediaCoordinator (14.2) suites without duplicating them: the
# prompt-length boundaries accepted through the full generate-and-place pipeline
# (6.7), timeout cancellation on the fetchResult path / through the
# GenerativeClientRunner adapter / surfaced by the coordinator (6.8), and the
# AuthServiceGenerationGate subscription/BYOK branches plus the real gate driving
# the coordinator end-to-end (6.5). Like the coordinator suite, the coordinator,
# client, auth, and BYOK sources are compiled directly into the test binary so
# the policy runs in isolation via mocks, an in-memory secret store, and an
# injectable clock — without a real network, GPU, or the media/FFmpeg/libsecret
# dependencies that the full Palmier::services library links. SecretStore.cpp is
# intentionally omitted (InMemorySecretStore is header-only), keeping the binary
# free of libsecret.
# ---------------------------------------------------------------------------
add_executable(palmier_services_generation_validation_tests
    GenerationValidationAndAuthGating_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeMediaCoordinator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeClient.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp")
target_include_directories(palmier_services_generation_validation_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_generation_validation_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_generation_validation_tests)



# ---------------------------------------------------------------------------
# No-partial-edits atomicity property test (task 14.3; Requirement 6.6).
# RapidCheck property P2: any command either fully applies or leaves the project
# unchanged — never a partial mutation. Exercises two axes: (1) arbitrary
# EditCommand sequences (mixing success and deliberately-failing commands, plus a
# null command every step) through a TimelineEngine, asserting a changed apply
# yields an invariant-satisfying project and any non-change leaves it byte-for-
# byte unchanged; and (2) the generative path — a GenerativeMediaCoordinator over
# the real GenerativeClientRunner/GenerativeClient whose backend fails the job at
# the provider — asserting the timeline and media library are left unchanged
# (Req 6.6). Following the generative-coordinator test's isolated-source compile
# pattern, the coordinator/client/auth/BYOK sources are compiled directly into
# the binary alongside Palmier::core, without the media/FFmpeg, libsecret, or
# lcms2 dependencies that the full Palmier::services library links.
# ---------------------------------------------------------------------------
# Task 10.7 extends the same binary with Property 67 (a failed or timed-out job
# leaves nothing behind; Requirements 12.7, 12.10). That property is stated about
# the whole `generation.generate` path rather than about the coordinator alone, so
# the target additionally compiles the tool surface (ToolRegistry/McpToolExecutor
# over a ProjectSession, whose `.palmier` serialization is how the property
# compares the post-failure project to the pre-submission one as a document) and
# task 10.5's backend registry with its hosted HTTPS client. The injected
# GenerativeHttpTransport is the only route from any of it to a socket, and it is
# also where the timeout is injected: the transport advances an injected virtual
# clock as it answers each poll, so Requirement 12.10's budget (10..3600 s) is
# crossed with no wall-clock wait. As with the other service-layer targets the
# sources are listed explicitly, so this binary needs neither libsecret, lcms2 nor
# Palmier::media.
add_executable(palmier_services_generative_atomicity_property_tests
    services/generative_atomicity_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeMediaCoordinator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeClient.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeBackendRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeHttpTransport.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/HostedGenerativeBackend.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokGenerativeBackend.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_generative_atomicity_property_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_generative_atomicity_property_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_generative_atomicity_property_tests)



# ---------------------------------------------------------------------------
# Graceful degradation property test (task 7.7; Requirements 10.4, 13.3).
# RapidCheck property P7 (software-only lane): with no supported GPU (the
# software-fallback context) or when ForceSoftware is selected, decode,
# composite (renderAt), and encode all remain fully functional on the software
# path for any source frame and effect parameters. The decode/encode lanes are
# driven through the IDecodeBackend / IEncodeBackend seams with software-only
# capabilities and BridgeAvailability::softwareOnly(), and compositing uses the
# real vendor-neutral software Compositor — so the whole property runs without a
# Vulkan loader, GPU, FFmpeg, or any vendor SDK, which is exactly the environment
# graceful degradation must cover. Links Palmier::media (which brings in
# Palmier::gpu and Palmier::core) alongside the shared GoogleTest/RapidCheck
# support target.
# ---------------------------------------------------------------------------
add_executable(palmier_gpu_graceful_degradation_property_tests
    gpu_graceful_degradation_property_test.cpp)
target_link_libraries(palmier_gpu_graceful_degradation_property_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_gpu_graceful_degradation_property_tests)



# ---------------------------------------------------------------------------
# Export frame ordering property test (task 10.3; Requirement 11.1). RapidCheck
# property P6: the frames rendered by an export are emitted in strictly
# increasing presentation time. Drives ExportEngine::run over arbitrary
# timelines, frame rates (integer and NTSC-style rational), and output
# resolutions with a real software Compositor (fed synthetic solid frames) and a
# MediaEncoder built behind the IEncodeBackend seam with a mock backend that
# records every submitted frame's presentation time, then asserts the recorded
# sequence is strictly increasing. Runs without a GPU, FFmpeg, or any vendor SDK.
# Links Palmier::media (which brings in Palmier::gpu and Palmier::core) alongside
# the shared GoogleTest/RapidCheck support target.
# ---------------------------------------------------------------------------
# Task 9.6 extends the same file with Property 38 (two successive exports are
# identical; Requirement 7.8): two exports of one generated timeline over all four
# containers, all three codecs and both encoder selections, compared trace against
# trace. The hardware selection is reachable here because gpu::GpuCaps and
# gpu::BridgeAvailability are values on EncodeSpec, so no GPU or vendor SDK is
# needed; the trace length is asserted against plannedFrameCount first, so two empty
# traces cannot compare equal into a pass.
add_executable(palmier_media_export_ordering_property_tests
    media_export_ordering_property_test.cpp)
target_link_libraries(palmier_media_export_ordering_property_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_export_ordering_property_tests)



# ---------------------------------------------------------------------------
# Key-moment bounds & error-handling unit tests (task 12.3; Requirements 5.2,
# 5.4, 5.5). Focused cases complementing the broader task-12.1 suite: the tight
# inclusive/just-past-edge timestamp bounds and whole-millisecond precision
# (5.2), empty-result handling for both an empty backend result and an all-out-
# of-range result that filters to empty (5.4), and the empty / zero-duration
# clip error path — descriptive error, no timestamps, engine never consulted
# (5.5). Following the task-12.1 isolated-source pattern, KeyMomentDetector.cpp
# is compiled directly into the test binary alongside Palmier::core, without the
# media/FFmpeg, libsecret, or lcms2 dependencies that the full Palmier::services
# library links. KeyMomentDetector depends only on Palmier::core.
# ---------------------------------------------------------------------------
add_executable(palmier_services_key_moment_bounds_tests
    KeyMomentBounds_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/KeyMomentDetector.cpp")
target_include_directories(palmier_services_key_moment_bounds_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_key_moment_bounds_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_key_moment_bounds_tests)



# ---------------------------------------------------------------------------
# MCP server transport + lifecycle unit tests (task 15.2; Requirements 7.1,
# 7.2, 7.3, 7.9). Exercise the pure request-routing (dispatch: /mcp-only, POST-
# only, JSON parse-error, handler delegation, missing-handler 503), the
# loopback-only admission check, and the real bind/listen/accept lifecycle over
# an ephemeral loopback port — start/stop within budget, idempotent stop, a live
# HTTP round-trip to an injected handler, and the port-in-use refusal that leaves
# the server stopped. The McpServer source (and the Json value it uses) are
# compiled directly into the test binary alongside Palmier::core, so the tests
# run without the media/FFmpeg, libsecret, or lcms2 dependencies that the full
# Palmier::services library links. McpServer uses only POSIX sockets + the
# standard library and depends only on Palmier::core.
# ---------------------------------------------------------------------------
add_executable(palmier_services_mcp_server_tests
    services/mcp_server_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/McpServer.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/RemoteAccessGate.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/TlsTransport.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_mcp_server_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_mcp_server_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_test_enable_openssl(palmier_services_mcp_server_tests)

palmier_register_test(palmier_services_mcp_server_tests)



# ---------------------------------------------------------------------------
# MCP tool-execution policy unit tests (task 15.3; Requirements 7.4-7.7, 7.10).
# Exercise McpToolExecutor over a real TimelineEngine and the shared ToolRegistry:
# recognized-tool success within budget (7.4), unknown-tool error leaving the
# project unchanged (7.5), execution-failure rollback (7.6), timeout abort +
# rollback driven through an injected clock (7.7), the no-project-open error
# (7.10), and schema validation rejecting malformed input before any command is
# created (7.10). The executor, tool surface, project session (with the save
# service and `.palmier` store it owns — task 3.4 retargeted both the executor and
# the tool surface at services::ProjectSession) and JSON value sources are compiled
# directly into the test binary alongside Palmier::core (which supplies the real
# TimelineEngine and EditCommands), without the media/FFmpeg, libsecret, or lcms2
# dependencies that the full Palmier::services library links.
# ---------------------------------------------------------------------------
add_executable(palmier_services_mcp_executor_tests
    services/mcp_tool_executor_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_mcp_executor_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_mcp_executor_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_mcp_executor_tests)



# ---------------------------------------------------------------------------
# Import -> edit -> export end-to-end integration test (task 10.4; Requirements
# 11.1, 11.6). Wires the real production pipeline together per source codec
# (H.264, HEVC, AV1, VP9): import validation through MediaProbe's injectable
# backend seam, cataloging in the real MediaManager, real edits through the
# TimelineEngine + EditCommands (AddEffect + SplitClip), the vendor-neutral
# software Compositor fed by a real MediaDecoder decoding synthetic per-codec
# frames, and ExportEngine::run driving the Compositor + MediaEncoder behind the
# IEncodeBackend seam with a mock backend that writes a genuine, well-formed
# output file to disk. Asserts that, for every codec, the complete timeline is
# rendered into a single output file (11.1) whose reported location names a real,
# valid, non-empty file (11.6). Runs without a GPU, FFmpeg, or any vendor SDK.
# Links Palmier::media (which brings in Palmier::gpu and Palmier::core) alongside
# the shared GoogleTest/RapidCheck support target.
# ---------------------------------------------------------------------------
add_executable(palmier_media_import_edit_export_integration_tests
    media_import_edit_export_integration_test.cpp)
target_link_libraries(palmier_media_import_edit_export_integration_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_import_edit_export_integration_tests)




# ---------------------------------------------------------------------------
# In-app agent orchestrator unit tests (task 16.1; Requirements 8.1, 8.5, 8.6,
# 8.7). Exercise the agent chat send pipeline over a real TimelineEngine, the
# shared ToolRegistry, and the real McpToolExecutor: a message translated into a
# tool call mutates the project through the same path as the MCP server and is
# reflected synchronously (8.1/8.6); a failed edit is rolled back and the project
# left unchanged with an error surfaced (8.7); and an unauthorized send is
# rejected (Unauthenticated) with the unsent message preserved for retry (8.5),
# covering both a mock gate and the real AuthServiceAgentGate over the
# AuthenticationService (subscription + BYOK branches). The orchestrator, tool
# surface, JSON value, auth, and BYOK sources are compiled directly into the test
# binary alongside Palmier::core (which supplies the real TimelineEngine and
# EditCommands), without the media/FFmpeg, libsecret, or lcms2 dependencies that
# the full Palmier::services library links.
# ---------------------------------------------------------------------------
add_executable(palmier_services_agent_orchestrator_tests
    services/agent_orchestrator_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/AgentOrchestrator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp")
target_include_directories(palmier_services_agent_orchestrator_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_agent_orchestrator_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_agent_orchestrator_tests)



# ---------------------------------------------------------------------------
# MCP HTTP protocol-conformance integration tests (task 15.5; Requirements 7.4,
# 7.5, 7.6, 7.10). Unlike the transport unit tests (15.2) and the execution-
# policy unit tests (15.3), which each cover one half of Component 2 in
# isolation, this suite wires the two halves TOGETHER as the composition root
# does — a real TimelineEngine + the default ToolRegistry behind an
# McpToolExecutor, plugged into the McpServer as its request handler — and drives
# real HTTP POST requests to /mcp over an ephemeral loopback port. It asserts a
# recognized edit tool changes the resulting project state (7.4), an unknown tool
# returns an error response and leaves the project unchanged (7.5), a failing tool
# rolls back and returns an error (7.6), and a no-project / schema-invalid request
# returns the appropriate error (7.10), plus HTTP-level conformance (unknown path
# 404, non-POST 405, malformed JSON 400). The McpServer, McpToolExecutor,
# ToolRegistry, and Json sources are compiled directly into the test binary
# alongside Palmier::core (which supplies the real TimelineEngine and
# EditCommands), so the tests run without the media/FFmpeg, libsecret, or lcms2
# dependencies that the full Palmier::services library links. The whole stack uses
# only POSIX sockets + the standard library and depends only on Palmier::core.
# ---------------------------------------------------------------------------
#
# Task 5.3 extends this suite with the JSON-RPC 2.0 handshake driven over the same
# real loopback socket — initialize -> notifications/initialized -> tools/list ->
# tools/call — asserting Requirement 15.3's envelope invariants on every response
# (valid JSON, "jsonrpc":"2.0", the request id echoed unchanged, exactly one of
# result/error) plus name/description/inputSchema on every tools/list entry, the
# Mcp-Session-Id emission, the 202-with-empty-body notification answer and the
# 1 MiB body cap yielding -32700. The protocol handler and the session registry
# join the compiled-in source list for that.
add_executable(palmier_services_mcp_http_integration_tests
    services/mcp_http_integration_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/McpServer.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/RemoteAccessGate.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/TlsTransport.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpProtocolHandler.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpSessionRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_mcp_http_integration_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_mcp_http_integration_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_test_enable_openssl(palmier_services_mcp_http_integration_tests)

palmier_register_test(palmier_services_mcp_http_integration_tests)




# ---------------------------------------------------------------------------
# UI / MCP / agent edit equivalence property test (task 15.4; Requirements 7.4,
# 8.1, 8.4). RapidCheck property P4: for any generated editing operation
# (add/delete/move/trim/split/reorder/add_effect) applied to three identical
# starting projects, issuing the edit via the UI path (a concrete EditCommand
# applied through TimelineEngine::apply), the MCP path (McpToolExecutor over the
# shared ToolRegistry), and the agent path (AgentOrchestrator driving the same
# executor+registry) all produce the same resulting project state. Freshly
# minted ids (a split's right-half id, an add_effect's effect id) are compared
# modulo a consistent id renaming; the add_clip id is pre-generated and passed
# explicitly through every path. Following the agent-orchestrator test's
# isolated-source compile pattern, the orchestrator, executor, tool surface,
# JSON value, auth, and BYOK sources are compiled directly into the test binary
# alongside Palmier::core (which supplies the real TimelineEngine and
# EditCommands), without the media/FFmpeg, libsecret, or lcms2 dependencies that
# the full Palmier::services library links.
# ---------------------------------------------------------------------------
add_executable(palmier_services_edit_equivalence_property_tests
    services/edit_equivalence_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/AgentOrchestrator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp")
target_include_directories(palmier_services_edit_equivalence_property_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_edit_equivalence_property_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_edit_equivalence_property_tests)




# ---------------------------------------------------------------------------
# @-mention resolution tests (task 16.2; Requirements 8.2, 8.3, 8.4). Exercise
# the MentionResolver's parsing, matching, and canonical rewrite, its
# MessagePreprocessor adapters, and an end-to-end check that a resolved mention
# reaches the shared tool executor while an unmatched (8.3) or ambiguous (8.4)
# mention rejects the message before it is submitted. Following the agent-
# orchestrator suite's isolated-source compile pattern, the resolver,
# orchestrator, executor, tool surface, JSON value, auth, and BYOK sources are
# compiled directly into the test binary alongside Palmier::core (which supplies
# the real TimelineEngine, EditCommands, and MediaManager), without the
# media/FFmpeg, libsecret, or lcms2 dependencies that the full Palmier::services
# library links. (Task 16.3 adds the dedicated mention-resolution + auth-gating
# suite under its own target.)
# ---------------------------------------------------------------------------
# Task 10.4 extends this target with `services/mention_resolver_property_test.cpp`
# — design.md Property 63 (Requirements 11.6, 11.7), the quantified form of the
# two arms the unit half pins by example. It drives the resolver through the real
# MessagePreprocessor seam, the real AgentOrchestrator, the REAL
# OfflineIntentInterpreter (hence the added source) and the real tool surface, so
# `OfflineIntentInterpreter.cpp` joins the isolated-source list. No new target: the
# design fixes Property 63 on this one.
add_executable(palmier_services_mention_resolver_tests
    services/mention_resolver_test.cpp
    services/mention_resolver_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/MentionResolver.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/OfflineIntentInterpreter.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AgentOrchestrator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp")
target_include_directories(palmier_services_mention_resolver_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_mention_resolver_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_mention_resolver_tests)



# ---------------------------------------------------------------------------
# Application shell — platform compatibility unit tests (task 19.1; Requirements
# 1.1, 1.3, 1.4, 1.5, 13.3, 13.4). Exercise the Qt-free PlatformCompatibility
# checker's pure verdict logic (evaluateCompatibility) with injected
# PlatformFacts: a supported host passes; an unsupported CPU architecture and a
# too-old / undetectable glibc are each reported as an unsupported platform;
# every missing runtime dependency is named individually; combined failures are
# all reported together; and the verdict is a deterministic, network-free pure
# function that completes far within the launch budget. Links only
# Palmier::app_platform (the always-built, Qt-free checker library) alongside the
# shared GoogleTest/RapidCheck support target, so it runs without Qt, FFmpeg,
# Vulkan, or a GPU.
# ---------------------------------------------------------------------------
add_executable(palmier_app_platform_compat_tests
    app/platform_compatibility_test.cpp)
target_link_libraries(palmier_app_platform_compat_tests PRIVATE
    Palmier::app_platform
    Palmier::test_support)

palmier_register_test(palmier_app_platform_compat_tests)




# ---------------------------------------------------------------------------
# Mention-resolution + auth-gating combined unit tests (task 16.3; Requirements
# 8.2, 8.3, 8.4, 8.5). Unlike the mention-resolver suite (16.2), which drives the
# MentionResolver in isolation with an always-authorizing gate, and the agent-
# orchestrator suite (16.1), which drives the auth gate with the mention seam
# stubbed, this suite exercises BOTH policies together END-TO-END through the
# AgentOrchestrator with the real @-mention preprocessor and the auth gate wired
# in. It covers the 16.3 checklist — single-match resolve (8.2), no-match
# rejection (8.3), multi-match prompt (8.4), unauthenticated gating (8.5) — plus
# the interaction the isolated suites cannot: the gate runs BEFORE mention
# resolution (an unauthorized send of an unmatched/ambiguous @mention is rejected
# as Unauthenticated with the RAW text preserved, and the preprocessor never
# runs), and a preserved @mention message then resolves after authenticating —
# via a mock gate and via the real AuthServiceAgentGate (subscription + BYOK
# branches). Following the mention-resolver suite's isolated-source compile
# pattern, the resolver, orchestrator, executor, tool surface, JSON value, auth,
# and BYOK sources are compiled directly into the test binary alongside
# Palmier::core (which supplies the real TimelineEngine, EditCommands, and
# MediaManager), without the media/FFmpeg, libsecret, or lcms2 dependencies that
# the full Palmier::services library links.
# ---------------------------------------------------------------------------
add_executable(palmier_services_mention_auth_gating_tests
    services/mention_auth_gating_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/MentionResolver.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AgentOrchestrator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp")
target_include_directories(palmier_services_mention_auth_gating_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_mention_auth_gating_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_mention_auth_gating_tests)




# ---------------------------------------------------------------------------
# Media Browser view-model unit tests (task 19.5; Requirements 3.1, 3.4, 5.3,
# 5.4). Exercise the Qt-free MediaBrowserViewModel: import wired to the Media
# Manager (adds to the library on success (3.1); leaves it unchanged and surfaces
# a naming message on an unsupported-format (3.2) / unreadable-file (3.3)
# rejection), the per-clip retained-version list (selectable, active version
# marked, at least the 10 most recent retained — 3.4), and the key-moment display
# (a marker per detected timestamp (5.3), the distinct "no key moments"
# indication with no markers (5.4), and NotAnalyzed before any detection). The
# view model and the KeyMomentMarkers/KeyMomentDetector service sources it reads
# are compiled directly into the test binary alongside Palmier::core (which
# supplies the real MediaManager and TimelineEngine), following the isolated-
# source pattern so the tests run without Qt or the media/FFmpeg, libsecret, or
# lcms2 dependencies that the full Palmier::services library links.
# ---------------------------------------------------------------------------
add_executable(palmier_ui_media_browser_viewmodel_tests
    ui/media_browser_viewmodel_test.cpp
    "${PROJECT_SOURCE_DIR}/src/ui/MediaBrowserViewModel.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/KeyMomentMarkers.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/KeyMomentDetector.cpp")
target_include_directories(palmier_ui_media_browser_viewmodel_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_ui_media_browser_viewmodel_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_ui_media_browser_viewmodel_tests)




# ---------------------------------------------------------------------------
# Inspector/Effects view-model unit tests (task 19.4; Requirement 2.4). Exercise
# the Qt-free InspectorViewModel end-to-end: the selected-clip projection
# (properties + name-sorted effect parameters), and the mapping of every
# Inspector edit onto the undoable EditCommand path — adding an effect
# (AddEffectCommand), editing an effect parameter (SetEffectParameterCommand),
# adjusting opacity/gain (SetClipPropertyCommand, including the engine's
# out-of-range rejection), and trimming an edge (TrimClipCommand, constrained to
# [1 frame, source duration]), plus the onChanged notification on selection,
# self-edits, and external edits. Following the services suites' isolated-source
# pattern, the view model source (which also defines the two Inspector-only
# commands) is compiled directly into the test binary alongside Palmier::core
# (which supplies the real TimelineEngine and EditCommands), so the tests run
# without Qt or the PALMIER_BUILD_UI dependency chain.
# ---------------------------------------------------------------------------
add_executable(palmier_ui_inspector_viewmodel_tests
    ui/inspector_viewmodel_test.cpp
    "${PROJECT_SOURCE_DIR}/src/ui/InspectorViewModel.cpp")
target_include_directories(palmier_ui_inspector_viewmodel_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_ui_inspector_viewmodel_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_ui_inspector_viewmodel_tests)




# ---------------------------------------------------------------------------
# Timeline view-model unit tests (task 19.2; Requirements 2.1-2.7). Exercise the
# Qt-free TimelineViewModel — the presentation adapter behind the Qt Timeline
# view — in isolation: the tracks x clips model-shape projection across the full
# 1-50 track range (2.1), and the gesture -> EditCommand mapping with its
# indication classification (drag-move + invalid-drop retention 2.2/2.3, trim to
# boundary 2.4, split contiguity + nothing-to-split 2.5/2.6, reorder preserving
# clip count 2.7, and undo/redo with the empty-history no-op 2.9/2.10). The
# adapter's source is compiled directly into the test binary alongside
# Palmier::core (which supplies the real TimelineEngine and EditCommands), so the
# tests run WITHOUT Qt, FFmpeg, Vulkan, or a GPU — independent of PALMIER_BUILD_UI.
# ---------------------------------------------------------------------------
add_executable(palmier_ui_timeline_viewmodel_tests
    ui/timeline_viewmodel_test.cpp
    "${PROJECT_SOURCE_DIR}/src/ui/TimelineViewModel.cpp")
target_include_directories(palmier_ui_timeline_viewmodel_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_ui_timeline_viewmodel_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_ui_timeline_viewmodel_tests)





# ---------------------------------------------------------------------------
# Agent Chat view-model unit tests (task 19.6; Requirements 8.1, 8.2, 8.3, 8.4,
# 8.5, 10.4, 13.4). Exercise the Qt-free AgentChatViewModel end-to-end: a sent
# message drives the SAME shared tool path as the MCP server and lands in the
# transcript (8.1/8.6); an unauthorized send is surfaced as
# AuthenticationRequired with the unsent text preserved (8.5); @-mention
# affordances report unique-resolve (8.2), unmatched (8.3), and ambiguous-with-
# candidates (8.4) both as a pre-send preview and on submit; the non-blocking
# GPU-unavailable notice is derived from a software-fallback GpuContext (10.4);
# and the generative-service-unavailable error leaves the open-source editor
# fully functional (13.4). Following the services / sibling view-model suites'
# isolated-source pattern, the view model and the agent send-stack sources it
# drives (AgentOrchestrator, MentionResolver, McpToolExecutor, ToolRegistry,
# Json, AuthenticationService, ByokCredentialManager) are compiled directly into
# the test binary alongside Palmier::core (which supplies the real TimelineEngine
# and EditCommands), so the tests run without Qt or the media/FFmpeg, libsecret,
# or lcms2 dependencies that the full Palmier::services library links.
# ---------------------------------------------------------------------------
add_executable(palmier_ui_agent_chat_viewmodel_tests
    ui/agent_chat_viewmodel_test.cpp
    "${PROJECT_SOURCE_DIR}/src/ui/AgentChatViewModel.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AgentOrchestrator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/MentionResolver.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp")
target_include_directories(palmier_ui_agent_chat_viewmodel_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_ui_agent_chat_viewmodel_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_ui_agent_chat_viewmodel_tests)




# ---------------------------------------------------------------------------
# Preview/player controller unit tests (task 19.3; Requirements 2.8, 10.7).
# Exercise the Qt-free PreviewController headlessly: the preview frame rate
# floored at a minimum of 24 fps (2.8), the >= 24-frames-per-wall-second
# presentation cadence driven by an injected manual clock, the GPU-active vs
# CPU-fallback render-path selection from the GpuContext capabilities and the
# retry-on-CPU degrade when a GPU render fails (10.7), the FrameSink delivery of
# composited frames, and the transport (play/pause/stop/seek + auto-stop at the
# end of a bounded timeline). The controller source is compiled directly into
# the test binary alongside Palmier::core and Palmier::gpu (which supply the real
# Project model and the vendor-neutral software Compositor / GpuContext), so the
# tests run without Qt, Vulkan, FFmpeg, or a GPU.
# ---------------------------------------------------------------------------
add_executable(palmier_ui_preview_controller_tests
    ui/preview_controller_test.cpp
    "${PROJECT_SOURCE_DIR}/src/ui/PreviewController.cpp")
target_include_directories(palmier_ui_preview_controller_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_ui_preview_controller_tests PRIVATE
    Palmier::core
    Palmier::gpu
    Palmier::test_support)

palmier_register_test(palmier_ui_preview_controller_tests)




# ---------------------------------------------------------------------------
# Application composition-root unit tests (task 21.1; Requirements 1.6, 7.1,
# 7.2, 7.9, 13.3). Exercise the Qt-free ApplicationComposition headlessly: every
# component is composed and cross-wired (the one ProjectSession owns the single
# TimelineEngine and media library the executor, the tool surface and the
# generative coordinator all act on, and the shared ToolRegistry backs both the
# MCP server and the agent); the MCP server starts on launch and stops on close within its
# lifecycle (idempotent stop, restart, and destructor-stops-a-running-server);
# a tools/call routed through the composed server reaches the same engine; the
# offline auth gate rejects an agent send while preserving the unsent text; and
# the GPU context degrades to the software fallback with no GPU. Following the
# services suites' isolated-source pattern, the composition root and the whole
# service send-stack it wires (project I/O, auth + BYOK, generative client +
# coordinator, ToolRegistry, executor, McpServer, agent orchestrator, mention
# resolver, localization) plus the Json value are compiled directly into the
# test binary alongside Palmier::core and Palmier::gpu (which supply the real
# TimelineEngine, MediaManager, and the software GpuContext), so the tests run
# WITHOUT Qt or the media/FFmpeg, libsecret, or lcms2 dependencies that the full
# Palmier::services library links. SecretStore.cpp is intentionally omitted
# (InMemorySecretStore is header-only), keeping the binary free of libsecret.
# ---------------------------------------------------------------------------
#
# Task 7.5 adds the Playback_Engine of Requirement 1.1 to the composition — the
# single gpu::Compositor, media::DecoderTeardownQueue, media::DecoderClipFrameProvider
# and ui::PreviewController — so the Qt-free PreviewController source joins the
# compiled-in list and the target links Palmier::media (which brings in
# Palmier::gpu and Palmier::core) for the decode side. Still no Qt, FFmpeg, GPU or
# vendor SDK: the decode backend factory degrades to its stub and the compositor
# runs the vendor-neutral software reference.
add_executable(palmier_app_composition_tests
    app/application_composition_test.cpp
    "${PROJECT_SOURCE_DIR}/src/app/ApplicationComposition.cpp"
    "${PROJECT_SOURCE_DIR}/src/ui/PreviewController.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeClient.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeMediaCoordinator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpServer.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/RemoteAccessGate.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/TlsTransport.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AgentOrchestrator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/MentionResolver.cpp"
    # Task 10.1: the composition root now selects its Agent_Interpreter through the
    # registry instead of defaulting to `makeUnconfiguredInterpreter()`.
    "${PROJECT_SOURCE_DIR}/src/services/AgentInterpreterRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/OfflineIntentInterpreter.cpp"
    # Task 10.5: the composition root now selects its Generative_Backend through
    # the registry instead of its own private offline stub, so all three backends
    # and the transport seam join the compiled-in list (Requirements 12.1, 12.2).
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeBackendRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeHttpTransport.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/HostedGenerativeBackend.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokGenerativeBackend.cpp"
    # Task 9.7: the composition root now constructs the one Export_Coordinator and
    # the one Media_Import_Service of Requirement 1.1 and binds the
    # `timeline.export` / `media.import` hooks to them, so both sources join the
    # compiled-in list. `makeExportToolHandler` lives in ExportCoordinator.cpp.
    # The decode/encode sides they call are already supplied by Palmier::media.
    "${PROJECT_SOURCE_DIR}/src/services/ExportCoordinator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/MediaImportService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/LocalizationManager.cpp")
target_include_directories(palmier_app_composition_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_app_composition_tests PRIVATE
    Palmier::core
    Palmier::gpu
    Palmier::media
    Palmier::test_support)

palmier_test_enable_openssl(palmier_app_composition_tests)

palmier_register_test(palmier_app_composition_tests)




# ---------------------------------------------------------------------------
# Invert-colors effect property tests (task 1.4; Requirements 14.4, 14.5).
# RapidCheck properties 73 and 74 for the ported invert-colors effect (upstream
# PR 408): the per-channel arithmetic over generated RGBA images (1x1 .. 256x256,
# with 0 / 255 / mid-range values in every channel) against the production
# software reference, and the playback-vs-export agreement within 1 of 255 per
# channel. The playback lane is driven through the real ui::PreviewController
# (whose source is compiled directly into the binary, as the preview-controller
# unit tests do) and the export lane through media::ExportEngine with a recording
# encode backend behind the IEncodeBackend seam — so both production compositing
# pipelines run headlessly, without Qt, FFmpeg, Vulkan, or a GPU. Links
# Palmier::media (which brings in Palmier::gpu and Palmier::core) alongside the
# shared GoogleTest/RapidCheck support target. The example-based unit tests for
# this effect live in gpu_effect_kernels_test.cpp (task 1.3).
# ---------------------------------------------------------------------------
add_executable(palmier_gpu_invert_colors_tests
    gpu/invert_colors_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/ui/PreviewController.cpp")
target_include_directories(palmier_gpu_invert_colors_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_gpu_invert_colors_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_gpu_invert_colors_tests)




# ---------------------------------------------------------------------------
# ToolSchema unit tests (task 3.1; Requirements 9.3, 9.9). Exercise the single
# tool argument specification from which BOTH the JSON Schema published by
# `tools/list` and the pre-execution validator are generated (design.md D3,
# "Schema/handler agreement"): every JsonKind's rendered "type" and accepted
# payload, required vs optional arguments, every bound (integer/number
# minimum-maximum, string minLength/maxLength, array minItems/maxItems), closed
# enum value sets, the canonical-UUID format, unknown-key rejection under
# "additionalProperties": false, and the stability of the rendered schema. The
# ToolSchema and Json sources are compiled directly into the test binary
# alongside Palmier::core (which supplies Result/Error and the real Uuid parser),
# as the other standalone service tests do, so these run without the
# media/FFmpeg, libsecret, or lcms2 dependencies the full Palmier::services
# library links.
# ---------------------------------------------------------------------------
add_executable(palmier_services_tool_schema_tests
    services/tool_schema_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_tool_schema_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_tool_schema_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_tool_schema_tests)




# ---------------------------------------------------------------------------
# Tool-surface argument declaration tests (task 3.2; Requirements 3.1, 9.3, 9.9,
# 9.12, 14.4). Every tool in the default surface now declares its arguments once
# as a ToolSchema, from which `Tool::inputSchema()` renders the schema
# `tools/list` publishes. These tests pin the conversion: each tool still
# advertises exactly the arguments, order, types and required set it advertised
# when the schemas were hand-written; `additionalProperties` is now false so an
# undeclared key is rejected instead of ignored; the acceptance rules that used
# to live privately inside the handlers (canonical UUIDs, the edge/kind/type
# closed value sets, non-negative positions and durations, opacity/gain bounds,
# the 1..2000-character generation prompt) are published and enforced; and
# `timeline.add_effect` carries the ported invert-colors effect end to end. The
# tool surface, ToolSchema and Json sources are compiled directly into the test
# binary alongside Palmier::core (which supplies the real TimelineEngine and
# EditCommands), as the other standalone service tests do.
# ---------------------------------------------------------------------------
add_executable(palmier_services_tool_registry_schema_tests
    services/tool_registry_schema_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_tool_registry_schema_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_tool_registry_schema_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_tool_registry_schema_tests)




# ---------------------------------------------------------------------------
# Schema/handler conformance property test (task 3.3; Requirement 9.12).
# Property 50: for every tool the registry contains, the schema `tools/list`
# publishes accepts an argument object exactly when the tool's own handler
# accepts it. The property builds argument objects from each tool's own ArgSpec
# list and perturbs them (omitted required keys, wrong JSON types, values just
# inside and just outside every declared bound, out-of-enum strings, malformed
# UUIDs, extra unknown keys, fractional integers, garbage array items,
# non-numeric object members), then compares ToolSchema::validate against the
# handler's outcome in both directions — excluding, by name and with a reason,
# the five rule classes task 3.2 documented as inexpressible in the ArgSpec
# vocabulary (cross-field relations, array item shape, object member value
# types, state-dependent rules, open backend-defined sets). It iterates whatever
# tools buildDefaultToolRegistry registers rather than a hard-coded list, so
# tools added by later tasks are covered automatically. Sources are compiled in
# directly, as for the neighbouring tool-surface suites, so the binary needs
# neither FFmpeg, libsecret nor lcms2.
# ---------------------------------------------------------------------------
add_executable(palmier_services_tool_schema_conformance_tests
    services/tool_schema_conformance_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_tool_schema_conformance_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_tool_schema_conformance_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_tool_schema_conformance_tests)




# ---------------------------------------------------------------------------
# Project_Session unit tests (tasks 2.1, 2.2; Requirements 1.10, 3.2, 3.4, 3.8,
# 3.9, 4.1, 4.4, 4.6, 4.10, 14.6, 14.7). Exercise services::ProjectSession over
# the real TimelineEngine, MediaManager and `.palmier` store: the default empty
# project at the documented defaults, createProject's exact-settings and
# out-of-range-argument behaviour, openProject replacing the current project and
# rebuilding the media library while a FAILED open leaves the previous project,
# its path, its modified flag and its undo history untouched, the session-level
# view of TimelineEngine::reset (one stable engine, both history stacks cleared,
# ChangeOrigin::Reset broadcast), the monotonic revision counter that the
# modified flag derives from, and the off-thread save with its revision guard —
# returning immediately while the write is still running, clearing the dirty flag
# only when the revision has not advanced, and leaving the project, its modified
# state and any previously saved file untouched on failure. Save failures and the
# in-flight window are injected through the services::RawFileWriter seam, so no
# test needs a full disk, elevated privileges or a sleep. Following the
# neighbouring service suites' isolated-source pattern, ProjectSession,
# ProjectSaveService and ProjectStore are compiled directly into the test binary
# alongside Palmier::core (which supplies the real TimelineEngine, EditCommands
# and MediaManager), without the media/FFmpeg, libsecret or lcms2 dependencies
# that the full Palmier::services library links. The persistence property tests
# 16-19 of tasks 2.3/2.4 join this target. Task 2.3's
# services/project_session_roundtrip_property_test.cpp adds Property 16 (a
# save/open round-trip preserves the project across the Requirement 4.7 bounds),
# Property 17 (saving a loaded project is idempotent) and Property 19 (a
# successful save reports the project unmodified until the next tool-applied
# edit); a generated project reaches the session the way `project.open` does,
# through a `.palmier` seed document, and each edit is issued as the concrete
# EditCommand the corresponding editing tool builds, applied through
# ProjectSession::engine(). Task 2.4's
# services/project_save_failure_property_test.cpp adds Property 18 — the
# universally quantified "a failed save preserves the previously saved file
# byte-for-byte, the in-memory project, and the modified flag" over generated
# projects x pre-existing destination bytes x the three injected failure kinds.
# ---------------------------------------------------------------------------
add_executable(palmier_services_project_session_tests
    services/project_session_test.cpp
    services/project_session_roundtrip_property_test.cpp
    services/project_save_failure_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp")
target_include_directories(palmier_services_project_session_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_project_session_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_project_session_tests)




# ---------------------------------------------------------------------------
# Media import service tests (task 4.1; Requirements 2.1-2.5, 2.7-2.9).
# Exercise services::MediaImportService end to end through the
# media::MediaProbeBackend seam: the accept path registering EXACTLY one asset in
# the current project's media library and reporting the asset id, resolved
# absolute path, container format and duration (2.1, 2.2); resolution and frame
# rate reported only for an asset carrying a decodable video stream (2.2); an
# unrecognised container and a wholly undecodable one rejected with the container
# format and the rejected codecs named, leaving the library unchanged (2.3); the
# empty / missing / unopenable / undecodable classifications each naming the path
# and the condition (2.4); duplicate convergence over path spellings and symlinks
# on one entry with the existing asset id returned (2.5); "no project is open"
# registering nothing (2.7); a probe that outruns the configured limit abandoned
# with the library untouched (2.8); and the path reported pending — with no
# library entry yet — for exactly the probe/validate/register window (2.9).
#
# Following the neighbouring service suites' isolated-source pattern,
# MediaImportService, ProjectSession, ProjectSaveService and ProjectStore are
# compiled directly into the test binary alongside Palmier::core (which supplies
# the real TimelineEngine and MediaManager). MediaProbe.cpp and
# ImportValidation.cpp are compiled in WITHOUT PALMIER_HAVE_FFMPEG, so the binary
# links no libav*, libsecret or lcms2: the default probe backend is the
# FailedPrecondition stub and every test injects its own backend instead.
#
# services/media_import_property_test.cpp (task 4.2) adds the universally
# quantified counterparts to the same target: Property 4 (result completeness plus
# the "resolution and frame rate exactly when a decodable video stream exists"
# rule, 2.2), Property 5 (format rejections name the container and every rejected
# codec and leave the library byte-identical, 2.3), Property 78 (each of the four
# rejection classes named, nothing changed, 2.4), Property 6 (idempotence over
# equivalent path spellings through both entry points, 2.5) and Property 7 (the
# entry-count invariant over arbitrary request sequences, 2.6).
# ---------------------------------------------------------------------------
add_executable(palmier_services_media_import_tests
    services/media_import_service_test.cpp
    services/media_import_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/MediaImportService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/media/MediaProbe.cpp"
    "${PROJECT_SOURCE_DIR}/src/media/ImportValidation.cpp")
target_include_directories(palmier_services_media_import_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_media_import_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_media_import_tests)



# ---------------------------------------------------------------------------
# Session, media-library and track tool unit tests (tasks 4.3, 4.4, 4.5;
# Requirements 2.2, 3.1-3.5, 3.7-3.10). Exercise the eight tools stage 4 adds to
# the shared tool surface against a real ProjectSession:
#
#   * `project.create` carrying exactly the requested name, frame rate (including
#     the exact NTSC rational a fractional rate denotes), canvas and colour space,
#     rejecting every out-of-range argument without touching the project, and
#     recording no undo history — a session operation is not an EditCommand;
#   * `project.save` / `project.open` round-tripping a document, `project.save`
#     defaulting to the recorded document path and refusing when there is none,
#     and a failed open leaving the previous project, its path, its modified flag
#     and its undo history untouched;
#   * `project.info` reporting the settings, the track/clip/asset counts, the
#     timeline duration, the modified flag, the document path and the undo depth;
#   * Requirement 3.5 — with no project current every tool other than
#     `project.create` and `project.open` refuses with "no project is open",
#     before any argument is parsed and before any hook can run;
#   * `media.import` reporting Requirement 2.2's fields through a REAL
#     MediaImportService over MediaProbe's injectable backend seam (resolution and
#     frame rate absent for an audio-only asset, the duplicate flag on a
#     re-import), and `media.list` agreeing with `project.info` on the asset count;
#   * `timeline.add_track` appending after the last track of its kind and
#     `timeline.remove_track` dropping its clips while preserving the remaining
#     order, both undoable through the ordinary engine path, both refusing bad
#     arguments and the 64-tracks-per-kind cap without changing anything.
#
# Following the neighbouring service suites' isolated-source pattern, the tool
# surface, ProjectSession, the project store and MediaImportService are compiled
# directly into the test binary alongside Palmier::core (which supplies the real
# TimelineEngine, EditCommands and MediaManager). MediaProbe.cpp and
# ImportValidation.cpp are compiled in WITHOUT PALMIER_HAVE_FFMPEG, so the binary
# links no libav*, libsecret or lcms2.
#
# services/timeline_track_tools_property_test.cpp (task 4.7) adds the universally
# quantified counterparts for the two track tools to the same target: Property 9
# (for any project and any sequence of `timeline.add_track` calls, each new track
# lands immediately after the last pre-existing track of its kind, every
# pre-existing track and clip compares equal before and after, and each returned
# identifier is unique within the project — Requirement 3.3) and Property 15 (for
# any project and any track identifier present in it, `timeline.remove_track`
# drops that track and its clips, preserves the relative order of the rest, and
# reports the resulting track and clip counts — Requirement 3.10). A generated
# project becomes current through TimelineEngine::reset, the same commit path
# `project.open` uses, so every generated project is validated and legal.
# ---------------------------------------------------------------------------
add_executable(palmier_services_session_tools_tests
    services/session_media_track_tools_test.cpp
    services/timeline_track_tools_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/MediaImportService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp"
    "${PROJECT_SOURCE_DIR}/src/media/MediaProbe.cpp"
    "${PROJECT_SOURCE_DIR}/src/media/ImportValidation.cpp")
target_include_directories(palmier_services_session_tools_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_session_tools_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_session_tools_tests)



# ---------------------------------------------------------------------------
# Project-tool property tests (task 4.6; Requirements 3.2, 3.4, 3.5, 3.7, 3.8,
# 3.9, 4.10). The universally quantified counterparts of the session-tool unit
# tests above, over a REAL ProjectSession and the shared tool surface:
#
#   * Property 8  — `project.create` carries exactly the requested name, frame
#                   rate, canvas and colour space, is reported unmodified with no
#                   document path, and returns a session-unique identifier (3.2);
#   * Property 10 — `project.save` then `project.open` makes the loaded project
#                   current and reports its identifier, track count, clip count,
#                   unmodified state and document path accurately (3.4);
#   * Property 11 — with no project current (the null-session overload), every
#                   tool the registry carries other than `project.create` and
#                   `project.open` refuses with "no project is open" for arguments
#                   generated from that tool's own ToolSchema, valid or not,
#                   running no hook and writing no file (3.5);
#   * Property 12 — `project.info`'s track and clip counts equal the number of
#                   SUCCESSFUL add_track / add_clip calls over interleaved
#                   sequences that deliberately include calls that fail (3.7);
#   * Property 13 — an object violating exactly one declared bound of
#                   `project.create` / `timeline.add_track` / `timeline.remove_track`
#                   is rejected naming the argument, leaving the project
#                   byte-identical with no edit command created (3.8);
#   * Property 14 — a failed `project.open` (missing, unreadable, not a `.palmier`
#                   document, unsupported schema version) preserves the previous
#                   project bytes, document path, modified flag and undo history,
#                   and names the path and the reason (3.9, 4.10).
#
# Following the neighbouring service suites' isolated-source pattern, the tool
# surface, ProjectSession and the project store are compiled directly into the
# test binary alongside Palmier::core (which supplies the real TimelineEngine,
# EditCommands and MediaManager), so the binary links no libav*, libsecret or
# lcms2.
# ---------------------------------------------------------------------------
add_executable(palmier_services_project_tools_tests
    services/project_tools_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_project_tools_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_project_tools_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_project_tools_tests)



# ---------------------------------------------------------------------------
# Application settings unit tests (task 6.1; Requirements 10.2, 16.3).
#
# Exercise app::AppSettings' four-layer resolution — built-in defaults, the
# key=value file at $XDG_CONFIG_HOME/palmier-pro/config, environment variables,
# command-line flags — with each layer shown overriding the one below it, plus the
# ignore-and-report behaviour for an absent file, a malformed line, an unknown key
# or flag and an out-of-range value, and the secure default that remote access
# stays off (and the endpoint on loopback) unless configuration opts in explicitly.
#
# The environment is driven entirely through the injected lookup seam, so the
# process environment is never mutated; where a file is needed, XDG_CONFIG_HOME is
# pointed at a pid-qualified scratch directory under the system temp path, so no
# real user configuration directory is touched.
#
# Following the neighbouring suites' isolated-source pattern, AppSettings.cpp is
# compiled directly into the test binary; the AppConfig it produces is a value
# type, so the binary needs no service library and links neither Qt nor
# FFmpeg/libsecret/lcms2.
# ---------------------------------------------------------------------------
add_executable(palmier_app_settings_tests
    app/app_settings_test.cpp
    "${PROJECT_SOURCE_DIR}/src/app/AppSettings.cpp")
target_include_directories(palmier_app_settings_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_app_settings_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_app_settings_tests)




# ---------------------------------------------------------------------------
# MCP session-registry tests (task 5.1; Requirements 9.10, 9.11, 9.14, 9.15,
# 10.9, 10.11). Exercise services::McpSessionRegistry — the MCP endpoint's
# identity store — in isolation: a minted identifier is 64 lowercase hex
# characters (256 bits from std::random_device, well above Requirement 9.11's
# 32-character floor); identifiers are never reissued even after their sessions
# are closed, because every issued id is retained in a never-pruned set while only
# live records expire; touch() reports NotFound for an unrecognised identifier and
# Timeout for one whose session went idle, in both cases telling the client to
# repeat initialize (9.15); markInitialized() is what makes tools/list and
# tools/call admissible (9.10, 9.14); expireIdle() closes exactly the idle
# sessions (10.11); the concurrent maximum refuses only the excess session while
# every established one stays active, and a slot freed by an expiry is immediately
# reusable (10.9); and out-of-range options are clamped to the permitted 1..32 /
# 30..3600 s bounds. The clock is injected, so the expiry boundary is asserted
# exactly — at the timeout the session is still live, past it the session is
# closed — without any test sleeping.
#
# The registry depends only on Palmier::core (Result/Error) and the standard
# library, so it is compiled directly into the test binary, following the
# neighbouring service suites' isolated-source pattern.
#
# services/mcp_session_property_test.cpp (task 5.5) joins the same target with the
# universally quantified halves of the same behaviour: Property 49 — every issued
# identifier is at least 32 opaque characters, no identifier is ever reissued for
# the process lifetime (asserted across sequences of up to 200 creations
# interleaved with closures, expiries and later requests, so distinctness holds for
# identifiers whose sessions are long gone), and a live identifier is accepted on
# every later request from the same client; and Property 51 — `tools/list` and
# `tools/call` presented on an unrecognised, expired or uninitialized session are
# refused with -32001 / -32002, create no edit command and leave the project
# byte-identical. Property 51 drives the real stack, so this target also compiles
# the protocol handler, the executor, the tool surface, the project session and the
# project store; Palmier::core supplies the real TimelineEngine, and the binary
# still links no libav*, libsecret or lcms2. Task 6.5 adds Properties 55 and 57
# (the session limit and idle expiry) to this same file.
# ---------------------------------------------------------------------------
add_executable(palmier_services_mcp_session_tests
    services/mcp_session_registry_test.cpp
    services/mcp_session_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/McpSessionRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpProtocolHandler.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_mcp_session_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_mcp_session_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_mcp_session_tests)


# ---------------------------------------------------------------------------
# MCP protocol-handler tests (task 5.2; Requirements 9.1-9.10, 9.14, 9.15, 9.16).
# Exercise services::McpProtocolHandler — the JSON-RPC 2.0 half of design.md D3 —
# directly, with no socket: initialize negotiating the client's requested version
# when supported and the highest supported version otherwise, reporting the server
# name, version and a capabilities object declaring `tools`, and minting the
# session identifier the transport emits (9.2, 9.11); notifications/initialized
# answering HTTP 202 with a zero-byte body and marking the session initialized
# (9.10); tools/list returning one entry per registered tool with name,
# description and an object inputSchema (9.3); tools/call returning a content
# array whose first entry is a text entry with isError false and the edit landing
# in the project (9.4), or isError true naming the failing tool and reason while
# leaving the project and the undo history exactly as they were (9.5); the fixed
# fault order — parse (-32700), envelope (-32600), method (-32601), session state,
# arguments (-32602) — with none of those paths creating an edit command (9.6-9.9,
# 9.14, 9.15); and a budget overrun reported as an isError result naming the tool
# and the limit, with the 60-second budget observably handed to the
# MainThreadInvoker (9.16). Both invoker directions are covered: the inline
# invoker headless builds and tests use, and a scripted one that reports a Timeout
# without running the work.
#
# Following the neighbouring service suites' isolated-source pattern, the handler,
# the session registry, the executor, the tool surface, the project session and
# the project store are compiled directly into the test binary alongside
# Palmier::core (which supplies the real TimelineEngine and EditCommands), so the
# binary links no libav*, libsecret or lcms2 — `tools/call` still runs the real
# production execution policy.
#
# Task 5.4 adds services/mcp_protocol_property_test.cpp to the same target: the six
# universally quantified protocol properties (43-48) — the JSON-RPC envelope
# round-trip and the zero-byte notification answer (9.1, 9.13), the version
# negotiation (9.2), the tools/list description of any registry contents (9.3), the
# tools/call result shape (9.4), the untouched project and undo history behind a
# failing tool (9.5), and the fault-code map with no edit command created (9.6-9.9)
# — driven through the handler with no socket, over the same real session, tool
# surface, executor and session registry.
# ---------------------------------------------------------------------------
add_executable(palmier_services_mcp_protocol_tests
    services/mcp_protocol_handler_test.cpp
    services/mcp_protocol_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/McpProtocolHandler.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpSessionRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_mcp_protocol_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_mcp_protocol_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_mcp_protocol_tests)



# ---------------------------------------------------------------------------
# Remote-access gate tests (tasks 6.2, 6.3; Requirements 10.1-10.5, 10.7-10.10,
# 10.12, 10.13).
#
# Exercise services::RemoteAccessGate directly, with no socket, keeping the gate's
# two responsibilities apart the way design.md D4 does:
#
#   * bind-time — absent configuration binds loopback 127.0.0.1:19789 (10.1); a
#     non-loopback bind is the conjunction of a valid IPv4/IPv6 literal, a 32-512
#     printable-ASCII bearer token and an explicit acknowledgement (10.2); any
#     unmet prerequisite falls back to loopback while NAMING every prerequisite it
#     missed and leaking no fragment of the token (10.3); configured TLS material
#     must be readable, parseable and a matching pair, with the three conditions
#     distinguished (10.12); and enabling without TLS still binds while offering
#     exactly one plaintext warning (10.7).
#   * per-request — a loopback-only binding admits everything, so the current
#     default developer experience is preserved and no 401/403 is producible
#     (10.10); a non-loopback binding refuses an absent, malformed or non-matching
#     bearer token with 401 (10.4), an Origin outside the allow-list extended with
#     the bound address and loopback hosts with 403 (10.5), and only the excess
#     session-initiating request when the ceiling is reached (10.9); five
#     consecutive 401s from one source inside a 60-second window block that source
#     alone for 60 seconds (10.13); and every refusal is recorded with a UTC
#     millisecond timestamp, the source address and a reason code, with no
#     substring of the presented credential anywhere in the record (10.8).
#
# The clock is injected, so the window and block boundaries are driven by advancing
# a variable rather than by sleeping. The TLS cases generate a real P-256 key and a
# matching self-signed certificate when the TLS transport is compiled in, and
# otherwise assert the documented degradation (configuring TLS is an unmet
# prerequisite and the gate binds loopback) — the invariant that keeps a host
# without OpenSSL configuring, building and testing unchanged.
#
# Following the neighbouring service suites' isolated-source pattern, the gate and
# the TLS transport are compiled directly into the test binary alongside
# Palmier::core, so it links no libav*, libsecret or lcms2.
#
# Task 6.4 adds services/remote_access_gate_property_test.cpp — Properties 52
# (the bind decision is the conjunction of every prerequisite), 53 (non-loopback
# admission is the conjunction of its checks), 54 (every rejection is logged
# without the credential), 56 (loopback-only preserves the current developer
# experience) and 58 (the 401 rate limiter blocks the offending source alone), all
# under an injected clock.
#
# Task 6.6 adds services/remote_access_http_integration_test.cpp, the only
# remote-access test with a real socket in front of it: a non-loopback-configured
# endpoint answering three genuine HTTP requests (no token, wrong token, the
# configured token -> 401, 401, 200) with a byte-identical project after each
# rejection, plus the HTTPS-succeeds / plaintext-rejected case of Requirement 10.6,
# which skips with a recorded reason when PALMIER_HAVE_OPENSSL is absent. Both of
# those exercise the gate through the transport and the JSON-RPC layer, so the MCP
# server, protocol handler, session registry, tool surface and project session join
# the compiled-in source list — the same set services/mcp_http_integration_test.cpp
# uses.
# ---------------------------------------------------------------------------
add_executable(palmier_services_remote_access_tests
    services/remote_access_gate_test.cpp
    services/remote_access_gate_property_test.cpp
    services/remote_access_http_integration_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/RemoteAccessGate.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/TlsTransport.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpServer.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpProtocolHandler.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpSessionRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_remote_access_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_remote_access_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_test_enable_openssl(palmier_services_remote_access_tests)

palmier_register_test(palmier_services_remote_access_tests)



# ---------------------------------------------------------------------------
# Decoder teardown tests (tasks 7.1, 7.2; Requirement 14.8) — Property 75 plus
# the unit tests for media::DecoderTeardownQueue.
#
# The queue exists so that closing a decoder — which blocks inside FFmpeg for
# hundreds of milliseconds — never runs on the playback thread, the audio
# callback thread or the Qt main thread (upstream PR 405). Property 75 asserts
# the hand-over never waits on a destructor, that every retired decoder is
# destroyed exactly once, and that destroying the queue terminates, over
# generated retirement counts, arrival patterns (burst versus spread),
# concurrent producer counts and close costs.
#
# The property's fake decoder blocks on a gate the test controls rather than on
# a sleep, so the assertions are about ordering and completion, never about
# elapsed wall-clock time; every wait in the suite is bounded, so a regression
# that reintroduces synchronous teardown fails with a counterexample instead of
# hanging under the 600-second per-test limit.
#
# Links Palmier::media (which brings in Palmier::gpu and Palmier::core) so the
# real MediaDecoder overload is exercised through MediaDecoder's existing
# IDecodeBackend seam; no FFmpeg, GPU, vendor SDK or media fixture is needed.
# ---------------------------------------------------------------------------
add_executable(palmier_media_decoder_teardown_tests
    media/decoder_teardown_property_test.cpp)
target_link_libraries(palmier_media_decoder_teardown_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_decoder_teardown_tests)



# ---------------------------------------------------------------------------
# Audio decode tests (tasks 8.1, 8.2; Requirement 6.1) — Property 26 plus the
# unit tests for the MediaDecoder audio surface.
#
# Task 8.1 gave MediaDecoder an audio surface (AudioFrame, openAudioStream,
# nextAudioFrame, seekAudio, hasAudio) yielding the interleaved-float AudioBuffer
# the existing AudioGraph consumes. Property 26 asserts what Requirement 6.1
# fixes: every buffer the decoder emits declares a sample rate in
# 8 000-192 000 Hz, a channel count in 1-8, and a presentation timestamp
# non-decreasing across consecutive buffers of that stream — and hasAudio()
# agrees with the source's stream layout.
#
# Everything is driven through MediaDecoder's existing IDecodeBackend /
# DecodeBackendFactory seam with a synthetic audio backend, so the generator can
# explore the whole rate x channel x block-size space, plus layouts with audio at
# a non-zero index and assets with no audio stream at all, and out-of-range
# formats that must be refused rather than emitted. No FFmpeg, GPU, vendor SDK,
# sound card or media fixture is needed. The unit tests in the same file cover the
# example-based edges, including the default "no audio" IDecodeBackend
# implementation that keeps every pre-audio backend and test double compiling.
#
# Links Palmier::media (which brings in Palmier::gpu and Palmier::core).
# ---------------------------------------------------------------------------
add_executable(palmier_media_audio_decode_tests
    media/audio_decode_property_test.cpp)
target_link_libraries(palmier_media_audio_decode_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_audio_decode_tests)



# ---------------------------------------------------------------------------
# Playback frame-fidelity tests (tasks 7.3, 7.4; Requirements 5.1, 5.5, 14.8) —
# Property 20 plus the unit tests for media::DecoderClipFrameProvider and
# media::DecodeWorkerPool.
#
# Property 20 asserts that the frame presented for a timeline position is exactly
# the source frame the clip's (sourceIn, timelineStart) mapping selects. The
# ground truth comes from MediaDecoder's existing DecodeBackendFactory /
# IDecodeBackend seam: a synthetic backend emits frames whose pixel values are a
# deterministic function of the frame index, so a presented frame identifies which
# source frame reached the canvas. Generated cases cover sequential playback,
# forward and backward seeks, interleaved access, non-zero sourceIn, several clips
# sharing one asset, and more distinct assets than the LRU decoder cache holds.
#
# The pipeline runs for real: the provider owns a DecodeWorkerPool with the
# design's N = 2 workers and bounded per-clip queues, and the composited output is
# produced by gpu::Compositor on gpu::GpuContext::softwareFallback(). Nothing here
# needs FFmpeg, a GPU, a media fixture or a temporary file — the asset "paths" are
# opaque keys the synthetic factory dispatches on. Every wait in the file is
# bounded, so a concurrency regression fails with a counterexample rather than
# hanging under the harness's 600-second per-test limit.
#
# Links Palmier::media (which brings in Palmier::gpu and Palmier::core).
# ---------------------------------------------------------------------------
add_executable(palmier_media_playback_tests
    media/playback_frame_fidelity_property_test.cpp)
target_link_libraries(palmier_media_playback_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_playback_tests)



# ---------------------------------------------------------------------------
# Audio engine tests (tasks 8.3, 8.4, 8.5; Requirements 6.2, 6.3, 6.4, 6.6, 6.7,
# 6.8, 6.9, 6.11) — Properties 27-32 plus the unit tests for media::IAudioSink /
# media::NullAudioSink and media::AudioEngine.
#
#   * Property 27 — mixing honours mute and gain and delivers without dropout.
#   * Property 28 — audio and video stay within 40 milliseconds.
#   * Property 29 — mute and gain changes take effect within 200 milliseconds.
#   * Property 30 — an asset without audio contributes exactly silence.
#   * Property 31 — export mix duration and sample range invariant.
#   * Property 32 — an audio decode failure yields silence for the rest of the clip.
#
# Everything runs through IAudioSink fakes and an INJECTED steady clock: the
# dropout bound (100 ms), the A/V skew bound (40 ms), the mute/gain latency bound
# (200 ms) and the audio-unavailable notice budget (2 s) are all driven by a clock
# the property advances by hand, so no test sleeps and no test needs a sound card
# — which is precisely why NullAudioSink lands before the PipeWire/ALSA sinks
# (design.md D7). Source audio comes from a synthetic DecodeBackendFactory emitting
# constant-level blocks, so the correct mixed value is knowable in closed form; no
# FFmpeg, GPU, media fixture or temporary file is involved.
#
# Links Palmier::media (which brings in Palmier::gpu and Palmier::core).
# ---------------------------------------------------------------------------
add_executable(palmier_media_audio_engine_tests
    media/audio_engine_property_test.cpp)
target_link_libraries(palmier_media_audio_engine_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_audio_engine_tests)



# ---------------------------------------------------------------------------
# Preview playback transport + pacing property tests (task 7.6; Requirements
# 5.2, 5.3, 5.4, 5.5, 5.7, 5.8, 5.9, 5.10) — Properties 21, 22, 23, 24, 25, 79
# for the ui::PreviewController transport that task 7.5 completed and the
# composition root now owns as the Requirement 1.1 Playback_Engine.
#
#   * Property 21 — presentation rate stays within bounds and drops stay under 5%.
#   * Property 22 — playhead indicator cadence (>= 10 Hz, within 100 ms).
#   * Property 23 — a decode failure pauses and retains the last good frame.
#   * Property 24 — playback frame accounting matches the export planner.
#   * Property 79 — each transport command reaches its specified resting state.
#   * Property 25 — seek clamps to the timeline bounds.
#
# Every one of those is a TIMING property, and every one of them is driven by the
# INJECTED ui::PlaybackClock: the test advances a manual clock, and composite
# latency is injected by advancing that same clock from inside the render seam,
# which is exactly what a slow GPU or decoder looks like to the transport. Nothing
# in the file sleeps, so the 100 ms / 500 ms / 1-second-window bounds are asserted
# exactly rather than measured against a loaded host's wall clock.
#
# The frame-accounting property compares against the REAL export planner
# (media::ExportEngine::plannedFrameCount) rather than a reimplementation of the
# arithmetic, which is what makes Requirement 5.7 a cross-check between the two
# pipelines; that is why this target links Palmier::media (which brings in
# Palmier::gpu and Palmier::core). The Qt-free PreviewController source is compiled
# directly into the binary, as the preview-controller unit tests do, and the
# compositor runs the vendor-neutral software reference on
# gpu::GpuContext::softwareFallback() — so no Qt, Vulkan, GPU, FFmpeg, sound card,
# media fixture or temporary file is involved.
# ---------------------------------------------------------------------------
add_executable(palmier_ui_preview_playback_tests
    ui/preview_playback_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/ui/PreviewController.cpp")
target_include_directories(palmier_ui_preview_playback_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_ui_preview_playback_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_ui_preview_playback_tests)



# ---------------------------------------------------------------------------
# Encoder-selection property tests (task 9.2; Requirements 8.2, 8.3, 8.4, 8.8)
# — Properties 40, 41 and 42 for the media::EncoderSelector that task 9.1 added,
# plus the unit companions for the 3000 ms probe deadline, the real detached-thread
# awaiter, the exhaustive 3-codecs x 8-compiled-in-states enumeration design.md D8
# calls for, and the tests/support/HardwareSkip.hpp reason text.
#
#   * Property 40 — exactly one encoder selection with consistent flags.
#   * Property 41 — hardware init failure retries once then falls back with the
#                   requested resolution, frame rate and bit rate intact.
#   * Property 42 — software selection uses the documented encoder per codec.
#
# These tests ALWAYS RUN — they never skip. The selector takes the device
# capabilities and the compiled-in vendor paths as VALUES (gpu::GpuCaps,
# gpu::BridgeAvailability), so the generator synthesizes all three codecs, all
# eight compiled-in states and arbitrary capability sets, and every hardware
# branch is exercised on a host with no GPU and no vendor SDK (this sandbox, and
# the SDK-free CI configure job). Nothing consults fromBuildConfig() or a live
# device for the properties.
#
# No test sleeps and none waits out the 3000 ms deadline: the properties inject a
# virtual-time awaiter that compares a generated probe delay against the deadline
# arithmetically, and the two unit tests that drive the REAL detached-thread
# awaiter use a short injected deadline with a probe blocked on a gate the test
# owns, so a wedged probe surfaces as a bounded failure rather than a hang.
#
# The include path adds ${PROJECT_SOURCE_DIR}/tests so the shared
# support/HardwareSkip.hpp helper resolves; links Palmier::media (which brings in
# Palmier::gpu and Palmier::core). No FFmpeg, GPU, vendor SDK, media fixture or
# temporary file is involved.
# ---------------------------------------------------------------------------
add_executable(palmier_media_encoder_selector_tests
    media/encoder_selector_property_test.cpp)
target_include_directories(palmier_media_encoder_selector_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(palmier_media_encoder_selector_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_encoder_selector_tests)




# ---------------------------------------------------------------------------
# Audio output sink set + startup selection tests (task 8.6; Requirements 6.2,
# 6.7) — the PipeWire and ALSA sinks and the design's selection order
# PipeWire -> ALSA -> Null.
#
# The order is asserted through INJECTED candidate factories, so every branch —
# PipeWire chosen, PipeWire unopenable so ALSA chosen, neither openable so the
# null sink chosen with the Requirement 6.7 notice, and a backend simply not
# compiled into this build — is exercised identically on a developer desktop with
# a sound server and on a CI runner with no sound card. Two further tests drive
# the REAL PipeWireAudioSink / AlsaAudioSink on whatever host is running and
# assert the host-independent contract: start() either opens the device or reports
# an error naming the backend, and selection always ends with a usable sink. On a
# machine with no audio device — this project's CI, and every container — that is
# exactly the fall-through to NullAudioSink.
#
# No sound card, no audio daemon, no FFmpeg, no GPU and no sleeping are required:
# the null fallback's position comes from an injected steady clock the test
# advances by hand. Links Palmier::media (which brings in Palmier::gpu and
# Palmier::core).
# ---------------------------------------------------------------------------
add_executable(palmier_media_audio_sink_tests
    media/audio_sink_selection_test.cpp)
target_link_libraries(palmier_media_audio_sink_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_audio_sink_tests)




# ---------------------------------------------------------------------------
# Offline interpreter tests (task 10.2; design.md Properties 59, 60, 61, 64;
# Requirements 11.2, 11.3, 11.4, 11.9) — the built-in agent interpreter and the
# interpreter registry that replaced `makeUnconfiguredInterpreter()`.
#
# The four properties:
#
#   * 59 — case- and whitespace-insensitivity, over the documented phrase table x
#     case maps (lower, upper, alternating, per-character random) x leading,
#     interior and trailing whitespace runs covering the whole whitespace set
#     (spaces, tabs, newlines, carriage returns, vertical tabs, form feeds). Every
#     perturbation must resolve to the SAME invocation as the canonical form.
#   * 60 — "the interpreter's output is always executable", taken literally: each
#     produced invocation is looked up in the REAL surface built by
#     `buildDefaultToolRegistry`, validated against the tool's ADVERTISED
#     `ToolSchema`, and then actually RUN through `McpToolExecutor` against a real
#     `ProjectSession`. An invocation the schema would reject, or that the engine
#     would refuse, is therefore a genuine counterexample rather than a formatting
#     nit.
#   * 61 — unmappable utterances (random strings plus deliberate near misses)
#     quote the request and leave the project, both history depths and the media
#     library byte-identical.
#   * 64 — the 1-2000 character bounds, both arms, with the in-range lengths as the
#     control so the boundary is pinned rather than just its far side.
#
# The no-network guarantee of Requirement 11.3 is PROVEN, not assumed, two ways:
# the component's declared network seam is installed with a gate that fails the
# test if called, and the C library's socket entry points (`socket`, `connect`,
# `getaddrinfo`, `gethostbyname`, `sendto`) are interposed for the duration of
# every interpretation and asserted never to be reached. The interposers forward to
# the real symbols through dlsym(RTLD_NEXT), so nothing else in the process is
# disturbed; the platform's dl library is linked for that reason.
#
# No sleeping, no injected clock, no FFmpeg, no GPU, no Qt and no sound device: the
# 1-second bound is measured with a steady clock over the real call and asserted at
# a small fraction of it. The two hook-backed tools the phrase table reaches
# (`media.import`, `timeline.export`) are wired to minimal in-test hooks — the
# import hook registers an asset in the session's real MediaManager, the export
# hook writes the requested bytes — because the property is about the
# INTERPRETER's output being acceptable to the tool surface, not about encoding.
# Every write-performing tool is only ever handed an absolute path inside a
# per-case scratch directory whose name carries the process id.
#
# Sources are listed explicitly (rather than linking Palmier::services) so this
# binary needs neither libsecret, lcms2 nor Palmier::media, exactly as the other
# service-layer test targets do.
# ---------------------------------------------------------------------------
add_executable(palmier_services_agent_offline_tests
    services/offline_interpreter_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/OfflineIntentInterpreter.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AgentInterpreterRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_agent_offline_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_agent_offline_tests PRIVATE
    Palmier::core
    Palmier::test_support)

# dlsym(RTLD_NEXT, ...) for the socket interposers.
if(CMAKE_DL_LIBS)
    target_link_libraries(palmier_services_agent_offline_tests PRIVATE ${CMAKE_DL_LIBS})
endif()

palmier_register_test(palmier_services_agent_offline_tests)


# ---------------------------------------------------------------------------
# Generative backend registry + HTTPS clients (task 10.5; Requirements 12.1,
# 12.2, 12.4, 12.6, 12.8).
#
# Everything above the network seam: id selection and its fallbacks, the offline
# stub's immediate precondition-naming rejection, request construction, the
# read-at-request-time credential policy, and the HTTP status -> ErrorCode mapping.
# No endpoint, no TLS and no socket are involved — which is the point of the seam.
#
# The binary links ${CMAKE_DL_LIBS} for the same reason the offline interpreter
# suite does: it interposes the C library's socket entry points via
# dlsym(RTLD_NEXT, ...) so the "no network" claims are proven about the process
# rather than asserted about a mock.
#
# Sources are listed explicitly (rather than linking Palmier::services) so this
# binary needs neither libsecret, lcms2 nor Palmier::media, exactly as the other
# service-layer test targets do.
# ---------------------------------------------------------------------------
add_executable(palmier_services_generative_backend_tests
    services/generative_backend_registry_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeBackendRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeHttpTransport.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/HostedGenerativeBackend.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokGenerativeBackend.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp"
    # For the canonical `toStringView(GenerationMediaType)`: the wire body spells
    # the media kind exactly the way the rest of the system does, rather than
    # keeping a second copy of the vocabulary that could drift from it.
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeClient.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_generative_backend_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_generative_backend_tests PRIVATE
    Palmier::core
    Palmier::test_support)

# dlsym(RTLD_NEXT, ...) for the socket interposers.
if(CMAKE_DL_LIBS)
    target_link_libraries(palmier_services_generative_backend_tests PRIVATE ${CMAKE_DL_LIBS})
endif()

palmier_register_test(palmier_services_generative_backend_tests)



# ---------------------------------------------------------------------------
# Generation lifecycle property tests (task 10.6; Requirements 12.3, 12.9).
# RapidCheck Property 65 (a successful generation is one undoable edit) and
# Property 66 (invalid generation requests never reach the network).
#
# Both properties are stated about the ONE path a `generation.generate` call
# takes, so the binary assembles that whole path out of product sources: the
# McpToolExecutor's schema validation and rollback policy, the shared
# ToolRegistry's declared `generation.generate` schema, the
# GenerativeMediaCoordinator over the real AuthServiceGenerationGate and
# GenerativeClientRunner, and task 10.5's backend registry with its hosted HTTPS
# client. The ProjectSession supplies the one TimelineEngine (undo history) and
# the one MediaManager (media library); the `.palmier` store supplies
# serializeProject, which is how Property 65 compares the post-undo project to
# the pre-generation one as a document rather than as spot checks.
#
# The injected GenerativeHttpTransport is the only route from any of it to a
# socket, which is what makes "never reached the network" checkable: Property 66
# installs a transport that fails the test when asked to send, and ${CMAKE_DL_LIBS}
# is linked so the same dlsym(RTLD_NEXT, ...) socket interposers task 10.5's suite
# uses can count socket/connect/getaddrinfo/sendto for the process rather than for
# a mock. A non-vacuity case arms those counters and opens a real socket.
#
# Sources are listed explicitly (rather than linking Palmier::services) so this
# binary needs neither libsecret, lcms2 nor Palmier::media, exactly as the other
# service-layer test targets do.
# ---------------------------------------------------------------------------
add_executable(palmier_services_generative_lifecycle_tests
    services/generative_lifecycle_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeBackendRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeHttpTransport.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/HostedGenerativeBackend.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokGenerativeBackend.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeMediaCoordinator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/GenerativeClient.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp")
target_include_directories(palmier_services_generative_lifecycle_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_generative_lifecycle_tests PRIVATE
    Palmier::core
    Palmier::test_support)

# dlsym(RTLD_NEXT, ...) for the socket interposers.
if(CMAKE_DL_LIBS)
    target_link_libraries(palmier_services_generative_lifecycle_tests PRIVATE ${CMAKE_DL_LIBS})
endif()

palmier_register_test(palmier_services_generative_lifecycle_tests)




# ---------------------------------------------------------------------------
# Export audio stream tests (task 9.3; Requirements 6.5, 6.11) — the encoder's
# second stream and the export engine's per-frame audio interleave.
#
# Two surfaces, tested separately because they fail for different reasons:
#
#   * media::MediaEncoder — EncodeSpec::audio, submitAudio()'s format and ordering
#     guards, the refusal to accept audio on a video-only encoder, the inherited
#     IEncodeBackend::encodeAudio default that reports Unsupported rather than
#     dropping audio, and finish() flushing BOTH streams before the trailer.
#   * media::ExportEngine — one audio block per video frame interval, the two
#     streams' presentation times marching together, and the per-frame intervals
#     tiling the timeline with no gap and no overlap so the audio stream spans the
#     whole duration within one frame interval.
#
# Requirement 6.11 ("no clip on an unmuted audio-bearing track still gets one
# silent stream spanning the full timeline duration") is asserted twice: once
# through the fallback silent renderer, and once through the PRODUCTION path,
# media::AudioEngine::renderRange, mixing a project whose only audio track is
# muted. That second case is the point of the seam — there is one export mixer, the
# AudioGraph behind renderRange, not a second one written for export.
#
# Nothing here sleeps, opens a device or needs a media fixture: the encode backend
# is a scriptable mock, the compositor runs the software reference on
# gpu::GpuContext::softwareFallback(), and the audio engine mixes through the
# always-available linear resampler with a NullAudioSink it never starts. Links
# Palmier::media (which brings in Palmier::gpu and Palmier::core).
# ---------------------------------------------------------------------------
add_executable(palmier_media_export_audio_tests
    media/export_audio_stream_test.cpp)
target_link_libraries(palmier_media_export_audio_tests PRIVATE
    Palmier::media
    Palmier::test_support)

palmier_register_test(palmier_media_export_audio_tests)




# ---------------------------------------------------------------------------
# Export coordinator tests (task 9.4; Requirements 6.5, 6.10, 6.11, 7.1-7.7,
# 7.9, 7.10, 7.11, 8.11) — services::ExportCoordinator's admission, isolation,
# reporting and cleanup.
#
# The coordinator's contract is mostly about what does NOT happen, so this target
# is deliberately set up to make those absences checkable:
#
#   * The mock encode backend WRITES A REAL FILE the moment it is built, exactly as
#     the FFmpeg backend does when it opens the muxer. That is what gives "no file
#     remains after a failure or a cancellation" (Requirements 7.5, 7.7, 8.11)
#     something real to assert, and it is why every write-performing case uses an
#     ABSOLUTE path inside a per-process scratch directory whose name contains
#     getpid(): gtest_discover_tests runs one process per case and ctest runs those
#     in parallel.
#   * The overwrite case compares the destination's BYTES before and after, which
#     is the only way "preserved byte-for-byte" (Requirement 7.11) can be checked.
#   * Cancellation and mid-export failure are DETERMINISTIC: the mock backend runs
#     on the export worker, so calling cancel() from inside its Nth encode() sets
#     the flag before the render loop's next frame-boundary check and the export
#     always stops at that exact frame. Nothing sleeps and nothing polls.
#   * The concurrent-request case parks the worker on a promise the TEST releases,
#     so the two requests genuinely overlap rather than probably overlapping.
#   * The <=1 s progress cadence is driven by an INJECTED clock the test advances by
#     hand, and every wait is bounded (awaitCompletion with a budget) with its
#     result asserted — so a coordinator that fails to finish makes these tests
#     FAIL rather than hang.
#
# No GPU, no vendor SDK, no FFmpeg and no media fixture: the compositor runs the
# software reference on gpu::GpuContext::softwareFallback(), clip pixels come from
# an injected provider, and encoder choice runs through media::EncoderSelector with
# an injected virtual-time probe awaiter. Links Palmier::services (which brings in
# Palmier::media, Palmier::gpu and Palmier::core).
# ---------------------------------------------------------------------------
# Task 9.5 adds services/export_coordinator_property_test.cpp to the SAME target
# (Properties 33, 34, 35, 36, 37 and 39; Requirements 6.10, 7.1-7.7, 7.9, 7.11,
# 8.11). The properties reuse the whole arrangement above — real files at absolute
# per-CASE paths containing getpid(), deterministic cancellation and failure driven
# from inside the backend, and the injected clock for the cadence — and add the
# anti-vacuity checks that matter for properties about absence: the backend records
# that the file really existed on disk before cleanup, Property 35 recovers the
# frame count from the finished file's own bytes, Property 39 compares a
# pre-existing destination's bytes, and Property 33 compares a SERIALIZED project
# snapshot across an edit made while the export runs.
add_executable(palmier_services_export_coordinator_tests
    services/export_coordinator_test.cpp
    services/export_coordinator_property_test.cpp)
target_link_libraries(palmier_services_export_coordinator_tests PRIVATE
    Palmier::services
    Palmier::test_support)

palmier_register_test(palmier_services_export_coordinator_tests)





# ---------------------------------------------------------------------------
# Agent-equivalence property test (task 10.3; design.md Property 62;
# Requirements 11.5, 11.10).
#
# One property, on its own target because it needs a collaborator no other agent
# suite links: `AgentOrchestrator` (and therefore `AuthenticationService` and
# `ByokCredentialManager`, which its `AuthServiceAgentGate` pulls in) on top of the
# offline interpreter, the tool surface, the executor and the session. It builds
# TWO real `ProjectSession`s per case from one `Project` value, drives one through
# interpreter -> orchestrator -> executor and the other straight into
# `McpToolExecutor::executeTool` with the same arguments, and compares the results
# byte for byte over `serializeProject` — hence `ProjectStore.cpp`, whose
# `serializeProject` is the canonical `.palmier` text the comparison is made on.
#
# Half the generated cases sabotage the EDITOR CONTEXT (a selection naming no
# clip, a playhead past the clip end, track ordinals resolving to unknown
# identifiers, an import of an absent file, an export or a save into a directory
# that does not exist) so the failure arm of Property 62 — "both paths leave the
# project byte-identical and the undo depth unchanged" — is reached about as often
# as the success arm.
#
# No sleeping, no injected clock, no FFmpeg, no GPU, no Qt and no sound device.
# `media.import` and `timeline.export` are hook-backed in the real surface and get
# minimal in-test hooks; the import hook draws its asset ids from a per-case map
# shared by both sessions so the same import yields the same asset id on both
# sides. Every write-performing tool is only ever handed an absolute path inside a
# per-case scratch directory whose name carries the process id.
#
# Sources are listed explicitly (rather than linking Palmier::services) so this
# binary needs neither libsecret, lcms2 nor Palmier::media, exactly as the other
# service-layer test targets do.
# ---------------------------------------------------------------------------
add_executable(palmier_services_agent_equivalence_tests
    services/agent_equivalence_property_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/OfflineIntentInterpreter.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AgentOrchestrator.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/McpToolExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ToolSchema.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSession.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectSaveService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/Json.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/AuthenticationService.cpp"
    "${PROJECT_SOURCE_DIR}/src/services/ByokCredentialManager.cpp")
target_include_directories(palmier_services_agent_equivalence_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(palmier_services_agent_equivalence_tests PRIVATE
    Palmier::core
    Palmier::test_support)

palmier_register_test(palmier_services_agent_equivalence_tests)




# ---------------------------------------------------------------------------
# Documentation, report and repository-hygiene checkers — `palmier_docs_tests`
# (design.md "test targets": Properties 68-72, 76, 77, 80).
#
# Created by task 10.8 with the repository hygiene property (Property 68,
# Requirement 12.6) as its first source. THIS TARGET IS SHARED: task 12.3 owns
# the same target name for the Parity_Report / Port_Backlog checkers, so it is
# deliberately set up to be EXTENDED rather than re-declared. To add sources to
# it, append with target_sources(), e.g.
#
#     target_sources(palmier_docs_tests PRIVATE
#         docs/parity_report_property_test.cpp
#         support/ReportParser.cpp)
#
# and, if a checker needs core's Result/Error, add to it additively with
# target_link_libraries(palmier_docs_tests PRIVATE Palmier::core). Do NOT add a
# second add_executable() with this name, and do NOT call palmier_register_test()
# on it again — it is registered once, below, and discovery picks up whatever
# test cases the accumulated sources define.
#
# Both source-tree paths these checkers need are injected as compile definitions
# here, because ctest's working directory is the BUILD tree, so neither a
# relative path nor a runtime guess would find the sources — and a checker that
# cannot find its input would pass vacuously:
#
#   * PALMIER_SOURCE_DIR — the repository root, walked by the hygiene property
#     (task 10.8) to discover src/, tests/ and cmake/.
#   * PALMIER_DOCS_DIR   — ${PROJECT_SOURCE_DIR}/docs, read by the report
#     checkers (task 12.3). Defined here so 12.3 needs no edit to this block.
#
# The hygiene checker is standard-library only (std::filesystem + std::regex), so
# the target links just the shared GoogleTest/RapidCheck support target; the
# tests/ include directory is on the path so `support/...` headers resolve the
# way the other test targets have them.
# ---------------------------------------------------------------------------
add_executable(palmier_docs_tests
    docs/repository_hygiene_property_test.cpp)
target_include_directories(palmier_docs_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
target_compile_definitions(palmier_docs_tests PRIVATE
    PALMIER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    PALMIER_DOCS_DIR="${PROJECT_SOURCE_DIR}/docs")
target_link_libraries(palmier_docs_tests PRIVATE
    Palmier::test_support)

palmier_register_test(palmier_docs_tests)



# ---------------------------------------------------------------------------
# Hardware-versus-software export comparison (task 9.8; Requirements 8.6, 15.5).
#
# On its own target rather than appended to
# `palmier_services_export_coordinator_tests`, because it is the opposite kind of
# test: that binary is documented as needing "no GPU, no vendor SDK, no FFmpeg and
# no media fixture" and drives a mock encode backend, whereas this one requires the
# REAL FFmpeg encode and decode paths and a real vendor encoder — a mocked backend
# cannot produce the decodable output Requirement 8.6 compares. Keeping them apart
# keeps that binary's premise true and makes this one's dependency on the host
# obvious.
#
# Links Palmier::services (which carries ExportCoordinator.cpp and pulls in
# Palmier::media for the encode/decode/probe surface) plus the shared test support
# target, which is where `support/HardwareSkip.hpp` — task 9.1's
# PALMIER_SKIP_WITHOUT_HW — lives.
#
# This test SKIPS, with a recorded reason naming what is absent, on any host that
# lacks either a vendor hardware encoder or a software H.264 encoder to compare
# against (Requirement 15.5). It skips on the current sandbox for BOTH reasons.
# ---------------------------------------------------------------------------
add_executable(palmier_services_export_hw_sw_comparison_tests
    services/export_hardware_software_comparison_test.cpp)
target_include_directories(palmier_services_export_hw_sw_comparison_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(palmier_services_export_hw_sw_comparison_tests PRIVATE
    Palmier::services
    Palmier::test_support)

palmier_register_test(palmier_services_export_hw_sw_comparison_tests)
