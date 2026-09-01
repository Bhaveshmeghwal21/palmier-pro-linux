// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/LutCache.cpp — see the header for why a failure is a cached answer rather than an
// error return.

#include "gpu/LutCache.hpp"

#include <fstream>
#include <ios>
#include <sstream>
#include <utility>

#include "core/Error.hpp"

namespace palmier::gpu {

namespace {

/// The empty table lutForEffect() returns when there is nothing to apply. A function-local
/// static rather than a temporary, because the return type is a reference.
const CubeLut& noTable() {
    static const CubeLut empty;
    return empty;
}

}  // namespace

Result<std::string> readFileForLut(const std::string& path) {
    if (path.empty()) {
        return err<std::string>(invalidArgument("LUT path is empty"));
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        // NotFound names the path, which is what Requirement 7.8 asks the report to carry.
        // "Unreadable" and "missing" are deliberately one case: from the render loop's
        // point of view they are the same, and a colourist needs the path either way.
        return err<std::string>(notFound("LUT file cannot be read: " + path));
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad()) {
        return err<std::string>(notFound("LUT file could not be read to the end: " + path));
    }
    return ok(buffer.str());
}

void LutCache::setReader(Reader reader) {
    reader_ = std::move(reader);
    // A new reader invalidates everything remembered: the point of installing one is to
    // read something different, so keeping the old answers would silently ignore it.
    clear();
}

const LutLookup& LutCache::lookup(const std::string& path) {
    const auto existing = entries_.find(path);
    if (existing != entries_.end()) {
        return existing->second;  // including a remembered failure: never re-read per frame
    }

    LutLookup result;
    if (path.empty()) {
        // Not a failure. An effect with no LUT chosen renders un-graded with nothing to
        // report, and conflating it with a missing file would raise a notice about a
        // problem the user does not have.
        return entries_.emplace(path, std::move(result)).first->second;
    }

    ++reads_;
    // Called through the injected reader when one is installed, and directly otherwise --
    // rather than defaulting reader_ to readFileForLut in the constructor, so that
    // "no reader installed" and "the real filesystem" stay the same observable thing.
    Result<std::string> text = reader_ ? reader_(path) : readFileForLut(path);
    if (text.isError()) {
        result.failure = text.error().message();
        return entries_.emplace(path, std::move(result)).first->second;
    }
    Result<CubeLut> parsed = parseCubeLut(text.value());
    if (parsed.isError()) {
        // The parser's message names the fault; the path is added here, because the parser
        // is a pure function of text and does not know where the text came from.
        result.failure = parsed.error().message() + " (" + path + ")";
        return entries_.emplace(path, std::move(result)).first->second;
    }
    result.table = std::move(parsed).value();
    return entries_.emplace(path, std::move(result)).first->second;
}

bool LutCache::contains(const std::string& path) const {
    return entries_.find(path) != entries_.end();
}

std::map<std::string, std::string> LutCache::failures() const {
    std::map<std::string, std::string> out;
    for (const auto& [path, lookup] : entries_) {
        if (!lookup.ok()) {
            out.emplace(path, lookup.failure);
        }
    }
    return out;
}

void LutCache::forget(const std::string& path) { entries_.erase(path); }

void LutCache::clear() noexcept {
    entries_.clear();
    reads_ = 0;
}

LutCache& sharedLutCache() {
    static LutCache cache;
    return cache;
}

const CubeLut& lutForEffect(const Effect& effect) {
    if (effect.resourcePath.empty()) {
        return noTable();
    }
    return sharedLutCache().lookup(effect.resourcePath).table;
}

}  // namespace palmier::gpu
