// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/MentionResolver.cpp — @-mention resolution for the in-app agent chat
// (task 16.2). See MentionResolver.hpp for the behavioural contract
// (Requirements 8.2, 8.3, 8.4).

#include "services/MentionResolver.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "core/Error.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"

namespace palmier::services {
namespace {

// ASCII lowercase (locale-independent) so name matching is case-insensitive
// without dragging in locale surprises.
[[nodiscard]] std::string toLowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] bool isMentionStartBoundary(const std::string& message, std::size_t atPos) {
    // A mention's '@' must start the message or follow whitespace or an opening
    // bracket/quote — this keeps "user@host" style substrings from being read as
    // mentions while still allowing "(@clip)" and quoted forms.
    if (atPos == 0) return true;
    const char prev = message[atPos - 1];
    if (std::isspace(static_cast<unsigned char>(prev)) != 0) return true;
    return prev == '(' || prev == '[' || prev == '{' || prev == '"' || prev == '\'';
}

// True for characters permitted in a bare (unbracketed) mention name. File-name
// friendly: letters, digits, and the common path/name punctuation.
[[nodiscard]] bool isBareNameChar(char c) {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) != 0) return true;
    switch (c) {
        case '.':
        case '_':
        case '-':
        case '/':
        case ':':
            return true;
        default:
            return false;
    }
}

[[nodiscard]] char closingDelimiterFor(char open) {
    switch (open) {
        case '[': return ']';
        case '{': return '}';
        case '"': return '"';
        default:  return '\0';
    }
}

// The file name (last path component) of a source path.
[[nodiscard]] std::string baseName(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The stem (file name without its final extension), or the whole base name when
// there is no extension / it is a dotfile.
[[nodiscard]] std::string stemOf(const std::string& base) {
    const std::size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return base;
    return base.substr(0, dot);
}

// The lowercased set of names an asset answers to for mention matching.
[[nodiscard]] std::vector<std::string> matchNamesFor(const MediaAssetRef& asset) {
    std::vector<std::string> names;
    const std::string base = baseName(asset.sourcePath);
    if (!asset.sourcePath.empty()) names.push_back(toLowerAscii(asset.sourcePath));
    if (!base.empty()) {
        names.push_back(toLowerAscii(base));
        const std::string stem = stemOf(base);
        if (!stem.empty()) names.push_back(toLowerAscii(stem));
    }
    // Also answer to the canonical asset-id string (already lowercase hex).
    names.push_back(asset.assetId.toString());
    // De-duplicate to keep candidate counting honest (an item is one candidate
    // even if several of its names coincide with the mention).
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

}  // namespace

// ---------------------------------------------------------------------------
// MentionResolver
// ---------------------------------------------------------------------------

MentionResolver::MentionResolver(std::vector<MediaAssetRef> mediaItems)
    : mediaItems_(std::move(mediaItems)) {}

MentionResolver MentionResolver::fromMediaManager(const MediaManager& manager) {
    return MentionResolver(manager.library());
}

MentionResolver MentionResolver::fromProject(const Project& project) {
    return MentionResolver(project.assets);
}

std::vector<MentionResolver::Mention> MentionResolver::parseMentions(
    const std::string& message) {
    std::vector<Mention> mentions;
    const std::size_t n = message.size();
    std::size_t i = 0;
    while (i < n) {
        if (message[i] != '@' || !isMentionStartBoundary(message, i)) {
            ++i;
            continue;
        }
        const std::size_t at = i;
        std::size_t j = i + 1;
        if (j < n) {
            const char delimClose = closingDelimiterFor(message[j]);
            if (delimClose != '\0') {
                // Bracketed / quoted form: read until the matching close.
                const std::size_t nameBegin = j + 1;
                std::size_t k = nameBegin;
                while (k < n && message[k] != delimClose) ++k;
                if (k < n) {
                    // Found a closing delimiter: a well-formed bracketed mention.
                    Mention m;
                    m.name = message.substr(nameBegin, k - nameBegin);
                    m.begin = at;
                    m.end = k + 1;  // consume the closing delimiter
                    if (!m.name.empty()) mentions.push_back(std::move(m));
                    i = k + 1;
                    continue;
                }
                // No closing delimiter — fall through and treat the '@' as text.
                i = at + 1;
                continue;
            }
        }
        // Bare form: read a run of name characters.
        std::size_t k = j;
        while (k < n && isBareNameChar(message[k])) ++k;
        if (k > j) {
            Mention m;
            m.name = message.substr(j, k - j);
            m.begin = at;
            m.end = k;
            mentions.push_back(std::move(m));
            i = k;
            continue;
        }
        // A lone '@' with no name — not a mention; advance past it.
        i = at + 1;
    }
    return mentions;
}

MentionResolution MentionResolver::resolve(const std::string& message) const {
    const std::vector<Mention> mentions = parseMentions(message);

    MentionResolution resolution;
    resolution.status = MentionStatus::Resolved;

    // No mentions: the message passes through unchanged (identity), preserving
    // the orchestrator's default-preprocessor behaviour for plain messages.
    if (mentions.empty()) {
        resolution.rewrittenMessage = message;
        return resolution;
    }

    // Build the rewritten message incrementally, splicing canonical references
    // in place of each resolved mention token.
    std::string rewritten;
    rewritten.reserve(message.size());
    std::size_t cursor = 0;

    for (const Mention& mention : mentions) {
        const std::string needle = toLowerAscii(mention.name);

        // Collect distinct matching media items (by assetId) for this mention.
        std::vector<const MediaAssetRef*> matches;
        for (const MediaAssetRef& asset : mediaItems_) {
            const std::vector<std::string> names = matchNamesFor(asset);
            if (std::find(names.begin(), names.end(), needle) != names.end()) {
                const bool already = std::any_of(
                    matches.begin(), matches.end(),
                    [&](const MediaAssetRef* m) { return m->assetId == asset.assetId; });
                if (!already) matches.push_back(&asset);
            }
        }

        if (matches.empty()) {
            // 8.3 — no media item matches; reject without submitting.
            resolution.status = MentionStatus::Unmatched;
            resolution.problemMention = mention.name;
            resolution.resolved.clear();
            resolution.candidates.clear();
            resolution.rewrittenMessage.clear();
            return resolution;
        }
        if (matches.size() > 1) {
            // 8.4 — ambiguous; surface the candidates for a selection prompt.
            resolution.status = MentionStatus::Ambiguous;
            resolution.problemMention = mention.name;
            resolution.resolved.clear();
            resolution.rewrittenMessage.clear();
            resolution.candidates.clear();
            resolution.candidates.reserve(matches.size());
            for (const MediaAssetRef* m : matches) {
                resolution.candidates.push_back(
                    MentionCandidate{m->assetId, m->sourcePath, baseName(m->sourcePath)});
            }
            return resolution;
        }

        // 8.2 — unique match; rewrite the token to a canonical @<assetId>.
        const MediaAssetRef& asset = *matches.front();
        rewritten.append(message, cursor, mention.begin - cursor);
        rewritten.push_back('@');
        rewritten.append(asset.assetId.toString());
        cursor = mention.end;

        resolution.resolved.push_back(
            ResolvedMention{mention.name, asset.assetId, asset.sourcePath});
    }

    // Append the remainder after the last rewritten mention.
    rewritten.append(message, cursor, message.size() - cursor);
    resolution.rewrittenMessage = std::move(rewritten);
    return resolution;
}

// ---------------------------------------------------------------------------
// MessagePreprocessor adapters
// ---------------------------------------------------------------------------

Result<std::string> toPreprocessorResult(const MentionResolution& resolution) {
    switch (resolution.status) {
        case MentionStatus::Resolved:
            return resolution.rewrittenMessage;

        case MentionStatus::Unmatched:
            // 8.3 — indicate the referenced media item was not found. Requirement
            // 11.7 additionally asks the error to state the NUMBER of matching
            // assets, which for this branch is zero; it is spelled out rather than
            // implied so the count is machine-readable on both refusal branches.
            return err<std::string>(makeError(
                ErrorCode::NotFound,
                "The referenced media item was not found: @" + resolution.problemMention +
                    " (0 matching assets in the project media library)."
                    " Check the name against the project's media library."));

        case MentionStatus::Ambiguous: {
            // 8.4 — prompt the user to select one of the matching candidates. The
            // count is stated as a number as well as implied by the list, because
            // Requirement 11.7 asks for "the mention text and the number of
            // matching assets".
            std::string message = "The mention @" + resolution.problemMention +
                                  " matches more than one media item (" +
                                  std::to_string(resolution.candidates.size()) +
                                  " matching assets); please select one: ";
            for (std::size_t idx = 0; idx < resolution.candidates.size(); ++idx) {
                const MentionCandidate& c = resolution.candidates[idx];
                if (idx != 0) message += ", ";
                message += c.displayName.empty() ? c.assetId.toString() : c.displayName;
                message += " (" + c.assetId.toString() + ")";
            }
            return err<std::string>(makeError(ErrorCode::FailedPrecondition, std::move(message)));
        }
    }
    // Unreachable; treat as an internal error rather than silently submitting.
    return err<std::string>(makeError(ErrorCode::Internal, "unknown mention-resolution status"));
}

MessagePreprocessor makeMentionPreprocessor(std::vector<MediaAssetRef> mediaItems) {
    MentionResolver resolver(std::move(mediaItems));
    return [resolver = std::move(resolver)](std::string message) -> Result<std::string> {
        return toPreprocessorResult(resolver.resolve(message));
    };
}

MessagePreprocessor makeMentionPreprocessor(const MediaManager& manager) {
    // Capture the manager by reference and snapshot its library per message so
    // mentions resolve against the CURRENT project state.
    const MediaManager* managerPtr = &manager;
    return [managerPtr](std::string message) -> Result<std::string> {
        const MentionResolver resolver = MentionResolver::fromMediaManager(*managerPtr);
        return toPreprocessorResult(resolver.resolve(message));
    };
}

}  // namespace palmier::services
