# Requirements Document

## Introduction

Palmier Pro Linux is a Linux port of Palmier Pro, the open-source AI-native video editor originally built as a Swift application for macOS (Apple Silicon, macOS 26 Tahoe). The goal of this feature is to deliver an application that runs natively on Linux distributions and reproduces the full feature set of the original macOS product: a timeline-based video editor, built-in generative AI for video and image creation, a local Model Context Protocol (MCP) server for external AI agents, an in-app agent chat, audio transcription, key-moment detection, media management, and multi-language localization.

In addition to feature parity, the Linux version introduces GPU acceleration as a new first-class capability. GPU acceleration applies to video decoding, encoding, effect rendering, and timeline playback, with support for NVIDIA, AMD, and Intel graphics hardware and a graceful fallback to CPU processing when no compatible GPU is available.

The open-source/closed-source split of the original product is preserved: the editor and MCP server remain open source under GPLv3, while generative AI processing remains a closed, account-gated capability.

## Glossary

- **Palmier_Pro_Linux**: The complete Linux desktop application being specified, including the editor, MCP server, and agent integrations.
- **Timeline_Editor**: The component that displays and manipulates video, audio, and image clips arranged on a multi-track timeline.
- **Media_Manager**: The component responsible for importing, cataloging, storing, and versioning media assets within a project.
- **Project_Store**: The persistent representation of a project that acts as the single source of truth for clips, tracks, edits, and generated media.
- **Transcription_Service**: The component that converts audio in media clips into time-aligned text.
- **KeyMoment_Detector**: The component that analyzes footage to identify candidate key moments.
- **Generative_AI_Service**: The closed-source component that generates videos and images from prompts using supported generation models.
- **MCP_Server**: The local HTTP server that exposes editor tools to external AI agents via the Model Context Protocol.
- **Agent_Chat**: The in-app conversational assistant that operates on the current project using the same tools as the MCP_Server.
- **Authentication_Service**: The component that manages user login, sessions, subscription entitlement, and bring-your-own-key (BYOK) credentials.
- **GPU_Manager**: The component that detects available graphics hardware, selects an acceleration backend, and routes media operations to GPU or CPU.
- **Export_Engine**: The component that renders the timeline into an output media file.
- **Localization_Manager**: The component that presents the user interface in the user's selected language.
- **Installer**: The Linux distribution package or bundle used to install Palmier_Pro_Linux.
- **GPU_Backend**: A hardware acceleration interface such as NVDEC/NVENC (NVIDIA), VAAPI (Intel/AMD), or a Vulkan/CUDA compute path used for media operations.
- **BYOK**: Bring Your Own Key; user-supplied third-party model provider credentials.
- **SOTA_Model**: A supported state-of-the-art generative model (for example Seedance, Kling, Nano Banana Pro, Veo, Grok, GPT-image).

## Requirements

### Requirement 1: Linux Platform Compatibility

**User Story:** As a Linux user, I want Palmier Pro to run natively on my Linux distribution, so that I can edit AI videos without macOS hardware.

#### Acceptance Criteria

1. THE Palmier_Pro_Linux SHALL run on x86-64 (64-bit) Linux distributions that provide glibc version 2.31 or later.
2. THE Installer SHALL provide at least one distributable package in AppImage, Flatpak, or .deb format.
3. WHEN a user launches Palmier_Pro_Linux on a supported distribution, THE Palmier_Pro_Linux SHALL display the editor interface within 15 seconds without requiring a network connection.
4. IF Palmier_Pro_Linux is launched on a distribution that does not meet the minimum runtime dependencies, THEN THE Palmier_Pro_Linux SHALL display a message that names each missing dependency and exit without displaying the editor interface.
5. IF Palmier_Pro_Linux is launched on an unsupported CPU architecture or on a distribution providing glibc earlier than version 2.31, THEN THE Palmier_Pro_Linux SHALL display a message indicating the unsupported platform and exit without displaying the editor interface.
6. THE Palmier_Pro_Linux SHALL preserve feature parity with the macOS edition for every capability defined in Requirements 2 through 13.

### Requirement 2: Timeline Video Editor

**User Story:** As a video editor, I want a multi-track timeline, so that I can arrange and edit video, audio, and image clips.

#### Acceptance Criteria

1. THE Timeline_Editor SHALL display video, audio, and image clips on a multi-track timeline supporting between 1 and 50 tracks.
2. WHEN a user drags a clip to a new valid position on the timeline, THE Timeline_Editor SHALL move the clip to the dropped position and update the project state within 200 milliseconds.
3. IF a user drops a clip at a position that overlaps an existing clip on the same track, THEN THE Timeline_Editor SHALL reject the move, retain the clip at its original position, and display an indication that the drop position is invalid.
4. WHEN a user trims the start or end of a clip, THE Timeline_Editor SHALL update the clip duration to the trimmed boundary, constrained to a minimum clip duration of 1 frame and a maximum equal to the clip's original source duration.
5. WHEN a user splits a clip at a playhead positioned within that clip's start and end boundaries, THE Timeline_Editor SHALL replace the clip with two contiguous clips divided at the playhead position, preserving the combined duration of the original clip.
6. IF a user requests a split while the playhead is not positioned within any clip's boundaries, THEN THE Timeline_Editor SHALL leave all clips unchanged and display an indication that no clip is available to split.
7. WHEN a user reorders clips on a track, THE Timeline_Editor SHALL preserve the total count of clips on that track.
8. WHEN a user requests playback, THE Timeline_Editor SHALL render the timeline preview at the playhead position at a minimum of 24 frames per second.
9. IF a user performs an edit operation and then requests an undo, THEN THE Timeline_Editor SHALL restore the project state that preceded that edit operation, supporting at least 20 sequential undo operations.
10. IF a user requests an undo when no prior edit operation exists in the undo history, THEN THE Timeline_Editor SHALL leave the project state unchanged and display an indication that no operation is available to undo.

### Requirement 3: Media Import and Management

**User Story:** As a creator, I want to import and organize media in a project, so that the project is the single source of truth for my edit.

#### Acceptance Criteria

1. WHEN a user imports a media file in a supported format, THE Media_Manager SHALL add the media to the project library and make the imported media available for placement on the timeline.
2. IF a user imports a file in an unsupported format, THEN THE Media_Manager SHALL reject the import, leave the project library unchanged, and display an error message that names the unsupported format.
3. IF a user imports a file whose format is supported but whose contents cannot be read or decoded, THEN THE Media_Manager SHALL reject the import, leave the project library unchanged, and display an error message indicating that the file could not be read.
4. WHEN a generated clip replaces an existing clip, THE Media_Manager SHALL retain the prior version as a selectable version of that clip and SHALL preserve at least the 10 most recent versions of that clip.
5. THE Project_Store SHALL persist all clips, tracks, edits, and imported media references such that reopening the project restores the prior editing state, including clip positions, track order, and the selected version of each clip.
6. WHEN a user saves a project, THE Project_Store SHALL write the complete project state to a single project location on disk and indicate to the user that the save completed successfully.
7. IF a save operation fails due to insufficient disk space, insufficient permissions, or an inaccessible save location, THEN THE Project_Store SHALL preserve the last successfully saved project state and display an error message indicating that the save did not complete.

### Requirement 4: Audio Transcription

**User Story:** As an editor, I want audio transcribed into time-aligned text, so that I can navigate and edit based on spoken content.

#### Acceptance Criteria

1. WHEN a user requests transcription of an audio or video clip that contains a detectable audio track, THE Transcription_Service SHALL produce one or more text segments where each segment carries a start time and an end time expressed in milliseconds relative to the start of the source clip, with each segment's start time strictly less than its end time.
2. WHEN a user requests transcription and the produced segments are ordered, THE Transcription_Service SHALL arrange segments in non-decreasing start-time order such that no two segments overlap in time.
3. WHEN a transcription completes successfully, THE Transcription_Service SHALL associate the resulting text segments with the source clip so that each segment is retrievable via that source clip.
4. IF a clip contains no detectable audio track, THEN THE Transcription_Service SHALL return an empty transcript, leave the source clip unchanged, and present an indication to the user that no audio was found.
5. IF transcription fails after being requested for a clip that contains a detectable audio track, THEN THE Transcription_Service SHALL leave the source clip's existing segments unchanged and present an indication to the user that transcription did not complete.
6. WHEN a user requests transcription, THE Transcription_Service SHALL complete the transcription and return a result (transcript, empty transcript, or failure indication) within 60 seconds per minute of source clip audio duration.

### Requirement 5: Key Moment Detection

**User Story:** As an editor working with long footage, I want the system to find key moments, so that I can quickly locate usable segments.

#### Acceptance Criteria

1. WHEN a user requests key-moment detection on a valid clip, THE KeyMoment_Detector SHALL return a list of 0 to 500 timestamps, each expressed to millisecond precision, within 10 seconds of the request for clips up to 60 minutes in duration.
2. THE KeyMoment_Detector SHALL ensure every returned timestamp is greater than or equal to 0 and less than or equal to the analyzed clip duration.
3. WHEN key-moment detection completes with one or more detected timestamps, THE Timeline_Editor SHALL display a visual marker at each detected timestamp on the clip.
4. WHEN key-moment detection completes with zero detected timestamps, THE Timeline_Editor SHALL display an indication that no key moments were found and SHALL add no markers to the clip.
5. IF key-moment detection fails or the requested clip is empty or has zero duration, THEN THE KeyMoment_Detector SHALL return an error indication describing the failure and SHALL NOT return any timestamps.

### Requirement 6: Built-in Generative AI

**User Story:** As a creator, I want to generate videos and images from prompts inside the timeline, so that I can iterate without leaving the editor.

#### Acceptance Criteria

1. WHEN a user submits a generation prompt of 1 to 2000 characters with a selected SOTA_Model, THE Generative_AI_Service SHALL return generated media that matches the requested media type of video or image within 300 seconds.
2. WHEN generated media is returned, THE Media_Manager SHALL add the generated media to the project library and place it on the timeline at the user-specified position, measured in frames from the timeline start (0 to the current timeline duration).
3. WHERE a user selects a supported video SOTA_Model, THE Generative_AI_Service SHALL accept the prompt and produce a video asset.
4. WHERE a user selects a supported image SOTA_Model, THE Generative_AI_Service SHALL accept the prompt and produce an image asset.
5. IF a generation request is submitted by a user without an active subscription or BYOK credentials, THEN THE Generative_AI_Service SHALL reject the request, leave the timeline unchanged, and display a message prompting the user to authenticate.
6. IF a generation request fails at the provider, THEN THE Generative_AI_Service SHALL return an error message indicating the failure reason and leave the timeline and project library unchanged.
7. IF a submitted prompt is empty or exceeds 2000 characters, THEN THE Generative_AI_Service SHALL reject the request, leave the timeline unchanged, and display an error message indicating the allowed prompt length.
8. IF a generation request does not complete within 300 seconds, THEN THE Generative_AI_Service SHALL cancel the request, leave the timeline and project library unchanged, and display an error message indicating the generation timed out.

### Requirement 7: MCP Server

**User Story:** As a developer, I want a local MCP server, so that external agents like Claude Code, Codex, and Cursor can edit my timeline.

#### Acceptance Criteria

1. WHILE Palmier_Pro_Linux is running, THE MCP_Server SHALL expose a Model Context Protocol endpoint over HTTP at `http://127.0.0.1:19789/mcp`.
2. WHEN Palmier_Pro_Linux starts, THE MCP_Server SHALL begin accepting connections on `http://127.0.0.1:19789/mcp` within 5 seconds.
3. IF the MCP_Server cannot bind to port 19789 on 127.0.0.1 because the address is already in use, THEN THE MCP_Server SHALL not start the endpoint, SHALL leave the current project unchanged, and SHALL surface an error indicating that the MCP endpoint port is unavailable.
4. WHEN an MCP client invokes an exposed editor tool, THE MCP_Server SHALL execute the corresponding operation on the current project and return the result to the client within 30 seconds.
5. IF an invoked tool name is not recognized as an exposed editor tool, THEN THE MCP_Server SHALL leave the current project unchanged and SHALL return an error indicating that the tool name is unknown.
6. IF execution of an invoked tool fails, THEN THE MCP_Server SHALL roll back the current project to its state prior to the invocation and SHALL return an error indicating that the tool execution failed.
7. IF an invoked tool does not complete within 30 seconds, THEN THE MCP_Server SHALL abort the operation, SHALL roll back the current project to its state prior to the invocation, and SHALL return an error indicating that the tool execution timed out.
8. THE MCP_Server SHALL expose the same set of editing and generation tools that are available to the Agent_Chat.
9. WHEN Palmier_Pro_Linux is closed, THE MCP_Server SHALL stop accepting connections on the endpoint within 5 seconds.
10. IF an MCP request targets a project operation while no project is open, THEN THE MCP_Server SHALL return an error that indicates no project is open.

### Requirement 8: In-App Agent Chat

**User Story:** As a creator, I want an in-app agent chat, so that I can direct edits and generation from within the editor.

#### Acceptance Criteria

1. THE Agent_Chat SHALL operate on the current project using the same tools exposed by the MCP_Server.
2. WHEN a user references a media item using an @ mention in the Agent_Chat, THE Agent_Chat SHALL resolve the mention to the referenced media item that exists in the current project.
3. IF a user references an @ mention that matches no media item in the current project, THEN THE Agent_Chat SHALL reject the reference and display an error message indicating that the referenced media item was not found, without submitting the message for processing.
4. IF a user references an @ mention that matches more than one media item in the current project, THEN THE Agent_Chat SHALL prompt the user to select a single media item from the matching candidates before submitting the message for processing.
5. IF a user sends an Agent_Chat message without an active subscription or BYOK credentials, THEN THE Agent_Chat SHALL reject the message and prompt the user to authenticate, and SHALL preserve the unsent message content.
6. WHEN the Agent_Chat performs an edit operation, THE Timeline_Editor SHALL reflect the resulting change in the project state within 2 seconds of the operation completing.
7. IF an Agent_Chat edit operation fails to complete, THEN THE Agent_Chat SHALL display an error message indicating the operation failed and SHALL leave the project state unchanged from before the operation.

### Requirement 9: Authentication and Subscription

**User Story:** As a user, I want account and subscription management, so that I can access generative AI features.

#### Acceptance Criteria

1. THE Palmier_Pro_Linux SHALL allow a user to open and use all Timeline_Editor and MCP_Server functions that do not invoke generative AI features without requiring the user to log in.
2. WHEN a user submits login credentials that match a valid account, THE Authentication_Service SHALL establish an authenticated session within 5 seconds and return the user's subscription entitlement status as one of: active, expired, or none.
3. IF a user submits login credentials that do not match a valid account, THEN THE Authentication_Service SHALL reject the login within 5 seconds, leave the current session unauthenticated, and display an error message indicating that the credentials are invalid.
4. IF a user submits invalid login credentials 5 consecutive times, THEN THE Authentication_Service SHALL block further login attempts for that account for 15 minutes and display an error message indicating that the account is temporarily locked.
5. WHERE a user supplies BYOK (Bring Your Own Key) credentials, WHEN the user saves those credentials, THE Authentication_Service SHALL persist the credentials for the current user and authorize subsequent generative requests that use those credentials.
6. IF a user submits BYOK credentials that fail validation with the associated provider, THEN THE Authentication_Service SHALL reject the credentials, discard them without persisting, and display an error message indicating that the BYOK credentials are invalid.
7. IF a user without an active subscription entitlement or valid BYOK credentials attempts to invoke a generative AI feature, THEN THE Palmier_Pro_Linux SHALL block the request and display a message indicating that an active subscription or BYOK credentials are required.

### Requirement 10: GPU Acceleration

**User Story:** As a Linux user with a dedicated graphics card, I want GPU acceleration, so that decoding, encoding, rendering, and playback are faster.

#### Acceptance Criteria

1. WHEN Palmier_Pro_Linux starts, THE GPU_Manager SHALL complete detection of available graphics hardware within 10 seconds and identify a compatible GPU_Backend for NVIDIA, AMD, or Intel hardware, where a compatible GPU_Backend is one whose required driver is present and initializes successfully.
2. WHERE a compatible GPU_Backend is available, THE GPU_Manager SHALL route video decoding, video encoding, and effect rendering to the GPU_Backend.
3. WHERE a compatible GPU_Backend is available, THE Export_Engine SHALL use the GPU_Backend for hardware-accelerated encoding during export.
4. IF no compatible GPU_Backend is available or GPU detection does not complete within 10 seconds, THEN THE GPU_Manager SHALL route all media operations to the CPU processing path and display a non-blocking notification that GPU acceleration is unavailable.
5. IF a GPU operation fails during a media operation, THEN THE GPU_Manager SHALL retry the operation on the CPU processing path at most once, preserve the operation's input and project state so that no edit data is lost, and record the failure in the application log.
6. WHERE a system reports more than one compatible GPU, THE GPU_Manager SHALL select one compatible GPU by default, allow the user to select which GPU is used for acceleration, and persist the selection across restarts.
7. WHEN GPU acceleration is active for timeline playback, THE Timeline_Editor SHALL produce output frames whose per-channel pixel values differ from the CPU path output, for the same source frame and effect parameters, by no more than 1 on a 0-to-255 per-channel scale.
8. WHERE a compatible GPU_Backend is active, THE Export_Engine SHALL complete hardware-accelerated encoding of a given timeline in no more than the wall-clock time required by the CPU processing path for the same timeline, output format, and resolution.

### Requirement 11: Video Export

**User Story:** As a creator, I want to export my finished timeline, so that I can deliver a final video file.

#### Acceptance Criteria

1. WHEN a user requests an export with a selected output format and resolution that are both supported, THE Export_Engine SHALL render the complete timeline into a single output file in the selected format at the selected resolution without modifying the source timeline.
2. WHILE an export is in progress, THE Export_Engine SHALL report export progress as a monotonically non-decreasing percentage from 0 to 100, updated at least once per second.
3. IF an export fails before completion, THEN THE Export_Engine SHALL remove the incomplete output file, preserve the source timeline unchanged, and report an error message indicating the failure reason to the user.
4. IF a user requests an export with an output format or resolution that is not supported, THEN THE Export_Engine SHALL reject the export request before rendering begins and report an error message indicating that the selected format or resolution is unsupported.
5. IF a user requests an export of an empty timeline containing zero media segments, THEN THE Export_Engine SHALL reject the export request before rendering begins and report an error message indicating that the timeline is empty.
6. WHEN an export completes successfully, THE Export_Engine SHALL notify the user of successful completion and indicate the location of the output file.

### Requirement 12: Localization

**User Story:** As a non-English speaker, I want the interface in my language, so that I can use the editor comfortably.

#### Acceptance Criteria

1. THE Localization_Manager SHALL make available for selection the same set of interface languages that is supported by the macOS edition, and SHALL expose each such language as a selectable option in the interface language settings.
2. WHEN a user selects a supported interface language, THE Localization_Manager SHALL update all currently visible interface text to the selected language within 2 seconds, without requiring the user to restart the application.
3. WHERE a user interface string has no translation for the selected language, THE Localization_Manager SHALL display the English text for that string.
4. WHEN a user selects a supported interface language, THE Localization_Manager SHALL persist the selection and reapply the selected language automatically on all subsequent application launches.
5. WHEN the application launches and no interface language has been previously selected by the user, THE Localization_Manager SHALL default to the system language if that language is in the supported set, and otherwise SHALL default to English.

### Requirement 13: Open Source Licensing Parity

**User Story:** As a community contributor, I want the Linux editor to keep the original licensing model, so that the open-source guarantees are preserved.

#### Acceptance Criteria

1. THE Palmier_Pro_Linux SHALL distribute the complete Timeline_Editor and MCP_Server source code under the GPLv3 license, including the full unmodified GPLv3 license text.
2. WHERE a capability depends on the Generative_AI_Service, THE Palmier_Pro_Linux SHALL provide that capability as a closed-source service that requires a valid authenticated user account before granting access.
3. THE Palmier_Pro_Linux SHALL enable all open-source editor capabilities (Timeline_Editor and MCP_Server) to operate with full functionality while no network connection to the Generative_AI_Service is present.
4. IF the Generative_AI_Service is unreachable or user account authentication fails, THEN THE Palmier_Pro_Linux SHALL continue to provide all open-source editor capabilities without functional degradation and SHALL display an error message indicating that the Generative_AI_Service is unavailable, while preserving any in-progress open-source editing state.
