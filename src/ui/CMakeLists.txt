# SPDX-License-Identifier: GPL-3.0-or-later
#
# src/ui — Qt 6 (QML) presentation layer.
#
# Timeline view, preview/player, inspector, media browser, and agent chat panel.
# Only built when PALMIER_BUILD_UI is ON. Concrete sources are added by later
# tasks (19.x).

# ---------------------------------------------------------------------------
# palmier_ui_preview_vm — the Qt-free preview/player playback engine
# (task 19.3; extended by task 7.5 of end-to-end-editor-integration).
#
# PreviewController owns the playhead clock and the whole playback transport
# (play/pause/stop/seek-with-clamp, the end-of-timeline halt, drop accounting, the
# playhead-indicator cadence, the decode-failure pause and the runtime fallback to
# software compositing) and drives gpu::Compositor at the playhead. It has NO Qt
# dependency, so it links only the domain core and the GPU layer.
#
# Defined ABOVE the PALMIER_BUILD_UI guard below — and therefore built in BOTH
# trees — because the composition root (Palmier::app_composition, src/app) owns
# the single instance of it as the Requirement 1.1 Playback_Engine, and the
# composition root is Qt-free and is built and tested with the UI off. Everything
# in this file after the guard needs Qt and stays behind it. The thin Qt surface
# (palmier_ui_preview_view, at the end of this file) owns one of these.
# ---------------------------------------------------------------------------
add_library(palmier_ui_preview_vm STATIC
    PreviewController.cpp)
add_library(Palmier::ui_preview_vm ALIAS palmier_ui_preview_vm)

target_include_directories(palmier_ui_preview_vm PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_ui_preview_vm PUBLIC cxx_std_20)

target_link_libraries(palmier_ui_preview_vm PUBLIC
    Palmier::core
    Palmier::gpu)

# ---------------------------------------------------------------------------
# palmier_ui_gateway — the Qt-free GUI-to-tool-surface gateway (task 11.1;
# Requirements 1.7, 9.4, 11.5).
#
# GuiToolGateway routes every GUI mutating gesture through
# services::McpToolExecutor::executeTool (tagged InvocationSource::Gui) — the
# SAME execution path the MCP endpoint and the in-app agent use. It carries no
# Qt dependency, so it is unit-testable without a Qt runtime and is defined
# ABOVE the PALMIER_BUILD_UI guard (like palmier_ui_preview_vm above) so both
# build trees compile and test it; the Qt panels below link it once
# PALMIER_BUILD_UI is ON.
# ---------------------------------------------------------------------------
add_library(palmier_ui_gateway STATIC
    GuiToolGateway.cpp)
add_library(Palmier::ui_gateway ALIAS palmier_ui_gateway)

target_include_directories(palmier_ui_gateway PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_ui_gateway PUBLIC cxx_std_20)

target_link_libraries(palmier_ui_gateway PUBLIC
    Palmier::core
    Palmier::services)

# ---------------------------------------------------------------------------
# palmier_ui_project_file_actions — the Qt-free File-menu action orchestration
# (task 11.5; Requirements 4.1, 4.2, 4.3, 4.5, 4.9, 4.10).
#
# ProjectFileActions drives the New/Open/Save/Save As gestures and the
# unsaved-changes PendingIntent prompt against a GuiToolGateway + ProjectSession.
# Every real QFileDialog/QMessageBox call is behind the injected UiPrompts seam
# (MainWindow supplies the real one), so this class carries no Qt dependency and
# is unit-testable without a Qt runtime — defined above the PALMIER_BUILD_UI
# guard for the same reason as palmier_ui_gateway above.
# ---------------------------------------------------------------------------
add_library(palmier_ui_project_file_actions STATIC
    ProjectFileActions.cpp)
add_library(Palmier::ui_project_file_actions ALIAS palmier_ui_project_file_actions)

target_include_directories(palmier_ui_project_file_actions PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_ui_project_file_actions PUBLIC cxx_std_20)

target_link_libraries(palmier_ui_project_file_actions PUBLIC
    Palmier::core
    Palmier::services
    Palmier::ui_gateway)

if(NOT PALMIER_BUILD_UI)
    return()
endif()

add_library(palmier_ui INTERFACE)
add_library(Palmier::ui ALIAS palmier_ui)

target_include_directories(palmier_ui INTERFACE
    "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(palmier_ui INTERFACE
    Palmier::core
    Palmier::services
    Palmier::gpu
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::Widgets)

# ---------------------------------------------------------------------------
# palmier_ui_shell — the compiled editor main window (task 19.1).
#
# MainWindow is the top-level editor window shown once the launch-time platform
# compatibility check passes; the entry point (src/app/main.cpp) links this and
# constructs it only on a supported host. It is built only when Qt 6 is present
# (reached only when PALMIER_BUILD_UI is ON, which requires Qt during dependency
# discovery); an extra TARGET guard keeps configuration safe if Qt is somehow
# absent. AUTOMOC processes the Q_OBJECT in MainWindow. The window carries no
# platform-decision logic, so it depends only on Qt and Palmier::core; later
# tasks (19.2-19.6) dock the timeline/preview/inspector/media/agent panels in.
# ---------------------------------------------------------------------------
if(TARGET Qt6::Widgets)
    add_library(palmier_ui_shell STATIC
        MainWindow.cpp
        TimelinePanel.cpp
        ExportDialog.cpp
        ProjectSettingsDialog.cpp)
    add_library(Palmier::ui_shell ALIAS palmier_ui_shell)

    set_target_properties(palmier_ui_shell PROPERTIES AUTOMOC ON)

    target_include_directories(palmier_ui_shell PUBLIC
        "${PROJECT_SOURCE_DIR}/src")

    target_compile_features(palmier_ui_shell PUBLIC cxx_std_20)

    target_compile_definitions(palmier_ui_shell PUBLIC PALMIER_HAVE_QT=1)

    # NOTE: MainWindow.hpp/.cpp reference app::ApplicationComposition (by
    # reference, forward-declared in the header), but this target deliberately
    # does NOT link Palmier::app_composition here: src/app (which defines that
    # target) is added AFTER src/ui in the top-level CMakeLists.txt, and
    # target_link_libraries cannot name a target that does not exist yet. The
    # link is completed from the OTHER side instead: src/app/CMakeLists.txt adds
    # `target_link_libraries(palmier_ui_shell PUBLIC Palmier::app_composition)`
    # once app_composition exists, which is equally valid CMake (a target's
    # link interface may be extended after its own add_library() call) and
    # keeps both subdirectories buildable in the existing top-level order.
    target_link_libraries(palmier_ui_shell PUBLIC
        Palmier::core
        Palmier::services
        Palmier::gpu
        Palmier::media
        Palmier::ui_gateway
        Palmier::ui_project_file_actions
        Palmier::ui_preview_vm
        Palmier::ui_inspector_vm
        Palmier::ui_media_browser_vm
        Palmier::ui_agent_chat_vm
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets)
endif()


# ---------------------------------------------------------------------------
# palmier_ui_media_browser_vm — Qt-free Media Browser presentation model
# (task 19.5; Requirements 3.1, 3.4, 5.3, 5.4).
#
# MediaBrowserViewModel wires the import UI to the Media Manager and exposes the
# project media library, a clip's retained (selectable) versions, and a clip's
# key-moment markers / "no key moments" indication. It depends only on the domain
# core (MediaManager + value types) and the Qt-free key-moment marker service, so
# it carries NO Qt dependency and is unit-testable on any platform. The thin
# QWidget surface (palmier_ui_media_browser, below) skins this model.
# ---------------------------------------------------------------------------
add_library(palmier_ui_media_browser_vm STATIC
    MediaBrowserViewModel.cpp)
add_library(Palmier::ui_media_browser_vm ALIAS palmier_ui_media_browser_vm)

target_include_directories(palmier_ui_media_browser_vm PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_ui_media_browser_vm PUBLIC cxx_std_20)

target_link_libraries(palmier_ui_media_browser_vm PUBLIC
    Palmier::core
    Palmier::services
    Palmier::ui_gateway)

# ---------------------------------------------------------------------------
# palmier_ui_media_browser — the compiled Qt 6 Media Browser panel (task 19.5).
#
# MediaBrowserPanel is a thin QWidget skin over the Qt-free view model above: it
# renders the library / version / key-moment output and forwards user actions
# back into the model. Built only when Qt 6 is present; AUTOMOC processes the
# Q_OBJECT in MediaBrowserPanel. Links the Qt-free view model plus the Qt widget
# libraries it draws with.
# ---------------------------------------------------------------------------
if(TARGET Qt6::Widgets)
    add_library(palmier_ui_media_browser STATIC
        MediaBrowserPanel.cpp)
    add_library(Palmier::ui_media_browser ALIAS palmier_ui_media_browser)

    set_target_properties(palmier_ui_media_browser PROPERTIES AUTOMOC ON)

    target_include_directories(palmier_ui_media_browser PUBLIC
        "${PROJECT_SOURCE_DIR}/src")

    target_compile_features(palmier_ui_media_browser PUBLIC cxx_std_20)

    target_compile_definitions(palmier_ui_media_browser PUBLIC PALMIER_HAVE_QT=1)

    target_link_libraries(palmier_ui_media_browser PUBLIC
        Palmier::ui_media_browser_vm
        Palmier::core
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets)
endif()


# ---------------------------------------------------------------------------
# palmier_ui_inspector_vm — the Qt-free Inspector/Effects view model (task 19.4).
#
# InspectorViewModel holds the selected-clip state, projects the clip's
# properties and effect chain from the TimelineEngine snapshot, and maps every
# Inspector edit (add effect, remove/reorder an effect, set an effect
# parameter, adjust opacity/gain, trim an edge) onto the undoable EditCommand
# path on the engine. It carries NO Qt dependency, so it builds and is
# unit-tested everywhere — including hosts where Qt 6 is not installed. Only
# SetClipPropertyCommand is Inspector-only and lives in the same translation
# unit; RemoveEffectCommand, ReorderEffectsCommand and SetEffectParameterCommand
# (task 9) are core::EditCommands, since each has its own published tool.
# Depends only on Palmier::core.
# ---------------------------------------------------------------------------
add_library(palmier_ui_inspector_vm STATIC
    InspectorViewModel.cpp)
add_library(Palmier::ui_inspector_vm ALIAS palmier_ui_inspector_vm)

target_include_directories(palmier_ui_inspector_vm PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_ui_inspector_vm PUBLIC cxx_std_20)

target_link_libraries(palmier_ui_inspector_vm PUBLIC
    Palmier::core
    Palmier::ui_gateway)

# ---------------------------------------------------------------------------
# palmier_ui_inspector_panel — the thin Qt 6 widget over the view model
# (task 19.4). Built only when Qt 6 is present; AUTOMOC processes the Q_OBJECT in
# InspectorPanel. All editing logic lives in palmier_ui_inspector_vm, so this
# target is a pure presentation surface.
# ---------------------------------------------------------------------------
if(TARGET Qt6::Widgets)
    add_library(palmier_ui_inspector_panel STATIC
        InspectorPanel.cpp)
    add_library(Palmier::ui_inspector_panel ALIAS palmier_ui_inspector_panel)

    set_target_properties(palmier_ui_inspector_panel PROPERTIES AUTOMOC ON)

    target_include_directories(palmier_ui_inspector_panel PUBLIC
        "${PROJECT_SOURCE_DIR}/src")

    target_compile_features(palmier_ui_inspector_panel PUBLIC cxx_std_20)

    target_compile_definitions(palmier_ui_inspector_panel PUBLIC PALMIER_HAVE_QT=1)

    target_link_libraries(palmier_ui_inspector_panel PUBLIC
        Palmier::ui_inspector_vm
        Palmier::core
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets)
endif()


# ---------------------------------------------------------------------------
# palmier_ui_timeline_vm — the Qt-free Timeline presentation adapter (task 19.2).
#
# TimelineViewModel maps the TimelineEngine's Project into a tracks x clips model
# shape and translates timeline gestures (drag-move, trim, split, reorder) into
# the SAME concrete EditCommands the MCP server and agent use, driving them
# through TimelineEngine::apply so undo/redo, atomicity, and observers behave
# identically. Because it carries no Qt dependency it is ALWAYS built (even where
# Qt 6 is absent) so its unit tests run without a Qt runtime; the Qt
# QAbstractItemModel binding (TimelineModel, added to palmier_ui_shell below)
# only forwards to it.
#
# Declared with a UNIQUE target name and appended here so it does not disturb the
# palmier_ui / palmier_ui_shell targets defined above (task 19.1) or the sibling
# UI panels (tasks 19.3-19.6) that also append to this shared file.
# ---------------------------------------------------------------------------
add_library(palmier_ui_timeline_vm STATIC
    TimelineViewModel.cpp)
add_library(Palmier::ui_timeline_vm ALIAS palmier_ui_timeline_vm)

target_include_directories(palmier_ui_timeline_vm PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_ui_timeline_vm PUBLIC cxx_std_20)

target_link_libraries(palmier_ui_timeline_vm PUBLIC
    Palmier::core
    Palmier::ui_gateway)

# The Qt QAbstractItemModel binding is compiled into the editor shell only when
# Qt 6 is present; it wraps the always-built Qt-free adapter above.
if(TARGET palmier_ui_shell)
    target_sources(palmier_ui_shell PRIVATE
        TimelineModel.cpp)
    target_link_libraries(palmier_ui_shell PUBLIC
        Palmier::ui_timeline_vm
        Palmier::ui_gateway)
endif()


# ---------------------------------------------------------------------------
# palmier_ui_agent_chat_vm — the Qt-free Agent Chat view model (task 19.6).
#
# AgentChatViewModel drives the in-app agent through the AgentOrchestrator,
# exposes the chat transcript and @-mention affordances, and holds the two
# editor status notices this task owns: the non-blocking "GPU acceleration
# unavailable" notification (Req 10.4) and the "Generative AI service
# unavailable" error state that leaves the open-source editor functional (Req
# 13.4). It contains NO Qt types, so it builds and is unit-tested without a Qt
# install; the QWidget surface (palmier_ui_agent_chat_panel, below) is a thin
# renderer over it. Depends only on the service layer + core.
# ---------------------------------------------------------------------------
add_library(palmier_ui_agent_chat_vm STATIC
    AgentChatViewModel.cpp)
add_library(Palmier::ui_agent_chat_vm ALIAS palmier_ui_agent_chat_vm)

target_include_directories(palmier_ui_agent_chat_vm PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_ui_agent_chat_vm PUBLIC cxx_std_20)

target_link_libraries(palmier_ui_agent_chat_vm PUBLIC
    Palmier::core
    Palmier::services)

# ---------------------------------------------------------------------------
# palmier_ui_agent_chat_panel — the Qt 6 Agent Chat panel widget (task 19.6).
#
# The thin QWidget rendering surface over palmier_ui_agent_chat_vm. Built only
# when Qt 6 is present (an extra TARGET guard keeps configuration safe if Qt is
# somehow absent); AUTOMOC processes the Q_OBJECT in AgentChatPanel.
# ---------------------------------------------------------------------------
if(TARGET Qt6::Widgets)
    add_library(palmier_ui_agent_chat_panel STATIC
        AgentChatPanel.cpp)
    add_library(Palmier::ui_agent_chat_panel ALIAS palmier_ui_agent_chat_panel)

    set_target_properties(palmier_ui_agent_chat_panel PROPERTIES AUTOMOC ON)

    target_include_directories(palmier_ui_agent_chat_panel PUBLIC
        "${PROJECT_SOURCE_DIR}/src")

    target_compile_features(palmier_ui_agent_chat_panel PUBLIC cxx_std_20)

    target_compile_definitions(palmier_ui_agent_chat_panel PUBLIC PALMIER_HAVE_QT=1)

    target_link_libraries(palmier_ui_agent_chat_panel PUBLIC
        Palmier::ui_agent_chat_vm
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets)
endif()


# (palmier_ui_preview_vm — the Qt-free preview/player playback engine, task 19.3 —
# is defined at the TOP of this file, above the PALMIER_BUILD_UI guard, because the
# Qt-free composition root owns the single instance of it in both build trees.)

# ---------------------------------------------------------------------------
# palmier_ui_preview_view — the thin Qt 6 preview surface (task 19.3).
#
# A QWidget that owns a PreviewController and paints the frames it produces; all
# playback/path logic lives in the Qt-free view model above. Built only when
# Qt 6 Widgets is present (the TARGET guard keeps configuration safe if Qt is
# somehow absent even with PALMIER_BUILD_UI ON). AUTOMOC processes the Q_OBJECT
# in PreviewView.
# ---------------------------------------------------------------------------
if(TARGET Qt6::Widgets)
    add_library(palmier_ui_preview_view STATIC
        PreviewView.cpp)
    add_library(Palmier::ui_preview_view ALIAS palmier_ui_preview_view)

    set_target_properties(palmier_ui_preview_view PROPERTIES AUTOMOC ON)

    target_include_directories(palmier_ui_preview_view PUBLIC
        "${PROJECT_SOURCE_DIR}/src")

    target_compile_features(palmier_ui_preview_view PUBLIC cxx_std_20)

    target_compile_definitions(palmier_ui_preview_view PUBLIC PALMIER_HAVE_QT=1)

    target_link_libraries(palmier_ui_preview_view PUBLIC
        Palmier::ui_preview_vm
        Palmier::core
        Palmier::gpu
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets)
endif()

# palmier_ui_shell (MainWindow, TimelinePanel, ExportDialog — defined earlier in
# this file) uses PreviewView, MediaBrowserPanel, InspectorPanel and
# AgentChatPanel directly (task 11.2 docks each of the five panels), so its link
# interface is extended here, after each of those Qt panel targets has been
# defined, for the same forward-reference reason app_composition is linked from
# src/app/CMakeLists.txt instead of from palmier_ui_shell's own definition above.
if(TARGET palmier_ui_shell)
    target_link_libraries(palmier_ui_shell PUBLIC
        Palmier::ui_preview_view
        Palmier::ui_media_browser
        Palmier::ui_inspector_panel
        Palmier::ui_agent_chat_panel)
endif()
