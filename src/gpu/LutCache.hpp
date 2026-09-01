// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/LutCache.hpp — parsed `.cube` tables, keyed by path
// (monitoring-and-grading task 7; Requirement 7.8).
//
// Requirement 7.8 is the whole reason this type exists: when a referenced LUT file is
// missing or unreadable, the clip must render UN-GRADED, the effect must stay in the
// chain, and the failure must be reported naming the path — a missing LUT must not fail
// the open, drop the effect, or block editing.
//
// That set of demands is what makes a failure a CACHED ANSWER rather than an error return.
// If lutForPath() reported an error, every caller in the render loop would have to decide
// what to do about it, and the render loop's only correct answer is "carry on with no
// table". So a failure is remembered with its reason and returns an empty table, exactly
// as media::PeakEnvelopeCache remembers a failed envelope: the same shape, for the same
// reason, and re-reading a missing file once per frame would be the alternative.
//
// A LUT is small — a size-33 table is 36k entries — and a project uses a handful, so the
// cache is unbounded by path and never evicts. That is a deliberate difference from
// PeakEnvelopeCache, which is bounded because an asset's envelope scales with the media's
// duration and a library can hold hundreds.

#ifndef PALMIER_GPU_LUTCACHE_HPP
#define PALMIER_GPU_LUTCACHE_HPP

#include <cstddef>
#include <functional>
#include <map>
#include <string>

#include "core/Effect.hpp"
#include "gpu/CubeLut.hpp"

namespace palmier::gpu {

/// What a lookup found: a table, or the reason there is none.
struct LutLookup {
    /// Empty when the file was missing, unreadable or malformed.
    CubeLut     table;
    /// Empty when the lookup succeeded. Always names the path when it did not.
    std::string failure;

    [[nodiscard]] bool ok() const noexcept { return failure.empty(); }
};

/// Parses and remembers `.cube` tables by path.
///
/// Not thread-safe, and deliberately so: the compositor's software path is single-threaded
/// and adding a mutex would suggest otherwise.
class LutCache {
public:
    /// The table for `path`, parsing it on first use.
    ///
    /// An empty path is not a failure -- it is an effect with no LUT, which renders
    /// un-graded with nothing to report. Distinguishing the two matters, because "no LUT
    /// chosen yet" must not raise the same notice as "your LUT is missing".
    const LutLookup& lookup(const std::string& path);

    /// Whether a path has been looked up, without performing one.
    [[nodiscard]] bool contains(const std::string& path) const;
    /// Every remembered failure, for the shell to report once (Requirement 7.8).
    [[nodiscard]] std::map<std::string, std::string> failures() const;

    /// Forget one path, so a LUT replaced on disk can be re-read.
    void forget(const std::string& path);
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    /// Files actually read, for asserting that a cached failure is not retried per frame.
    [[nodiscard]] std::size_t readCount() const noexcept { return reads_; }

    /// Read a file's whole contents. Overridable so tests need no filesystem, and so the
    /// missing-file path is exercised without arranging for a missing file.
    using Reader = std::function<Result<std::string>(const std::string& path)>;
    void setReader(Reader reader);

private:
    std::map<std::string, LutLookup> entries_;
    Reader                           reader_;
    std::size_t                      reads_ = 0;
};

/// Read a file's whole contents, reporting NotFound with the path when it cannot be read.
[[nodiscard]] Result<std::string> readFileForLut(const std::string& path);

/// The table an effect refers to, or an empty one. The seam gpu::Compositor's software
/// path calls, kept out of line so that path needs no cache of its own.
[[nodiscard]] const CubeLut& lutForEffect(const Effect& effect);

/// The process-wide cache lutForEffect() consults. Exposed so the shell can report
/// failures and so a test can install a reader.
[[nodiscard]] LutCache& sharedLutCache();

}  // namespace palmier::gpu

#endif  // PALMIER_GPU_LUTCACHE_HPP
