// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/docs/repository_hygiene_property_test.cpp — the repository hygiene
// property (task 10.8; design.md Property 68; Requirement 12.6).
//
// Requirement 12.6, in full:
//
//   "THE repository SHALL contain only GPLv3-licensed client code for generative
//    capability, with the hosted service implementation kept outside this
//    repository, SHALL contain no hosted-service credential values, and every
//    added source file SHALL carry the `SPDX-License-Identifier:
//    GPL-3.0-or-later` header."
//
// Two of the three clauses are mechanically checkable and are what this file
// checks: the SPDX header on every source file, and the absence of credential
// values. (The third clause — that the hosted service implementation lives
// outside this repository — is an architectural statement about what is *not*
// here, and has no finite witness set to quantify over.)
//
// What makes this property non-decorative
// ---------------------------------------
// 1. THE FILE SET IS DISCOVERED, NEVER HARD-CODED. `enumerateSourceFiles()`
//    walks `src/`, `tests/` and `cmake/` under PALMIER_SOURCE_DIR with
//    std::filesystem::recursive_directory_iterator. A source file added
//    tomorrow without a header therefore fails this property tomorrow, with no
//    edit to this file. A hard-coded list would make the property permanently
//    true of the list rather than of the repository.
//
// 2. THE CHECKER IS PROVEN FALSIFIABLE. Every check is a pure function of
//    (path, content), so the same code that scans the tree is driven over
//    synthetic content by the `RepositoryHygieneChecker*` unit tests below:
//    content with no SPDX line, content whose SPDX line sits below code, and
//    content carrying each credential shape. Without those, a checker with a
//    broken pattern would report a clean tree and nobody would know.
//
// 3. THE CREDENTIAL RULE IS ABOUT OPACITY, NOT VOCABULARY. This repository
//    legitimately says `token`, `secret`, `password` and `Bearer` all over
//    `RemoteAccessGate`, `SecretStore`, `ByokCredentialManager` and their
//    tests, and its test fixtures contain deliberately fake 32-64 character
//    tokens. Flagging the words would flag the repository. So a credential
//    *shape* only becomes a *finding* when its VALUE also looks opaque, i.e.
//    machine-generated. See `isPlaceholderValue` for the rule and for the
//    fixtures it was validated against — every one of this repository's
//    existing fixtures is excused by a named, value-scoped clause of that rule,
//    and no directory, file or line is excluded to make the property pass.
//
// Why PALMIER_SOURCE_DIR
// ----------------------
// ctest's working directory is the build tree, not the source root, so a
// relative path or a runtime guess would be wrong (and a test that cannot find
// the tree would trivially "pass"). The source root is injected as a compile
// definition by tests/CMakeLists.txt, the same way task 12.3 injects
// PALMIER_DOCS_DIR. `enumerateSourceFiles()` fails the test outright if a
// scanned directory is missing or if the walk finds no files at all, so a
// mis-wired definition is loud rather than vacuous.
//
// Cost
// ----
// The walk, the reads and the scan happen exactly ONCE per process, in a
// function-local static (`hygieneScan()`); the property's ≥100 generated cases
// then read the cached per-file findings. So the 3.9 MB tree is scanned once,
// not 100 times, and the whole binary runs in well under a second — far inside
// the 600 s per-test limit. Because the cached scan already covers every file,
// each case asserts BOTH the drawn file (which is what RapidCheck shrinks to
// and names on failure) AND the whole-tree total, so no file can escape
// coverage merely because sampling did not draw it.

#include <gtest/gtest.h>

#include <rapidcheck/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef PALMIER_SOURCE_DIR
#error "PALMIER_SOURCE_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

namespace {

// ===========================================================================
// Findings
// ===========================================================================

enum class DefectKind {
    MissingSpdxHeader,           ///< No SPDX-License-Identifier: GPL-3.0-or-later anywhere.
    SpdxOutsideLeadingComment,   ///< Present, but not in the file's leading comment block.
    WrongSpdxLicense,            ///< An SPDX identifier naming some other licence.
    CredentialLiteral            ///< A credential-shaped literal with an opaque value.
};

struct Defect {
    DefectKind kind{DefectKind::MissingSpdxHeader};
    std::size_t line{0};       ///< 1-based; 0 when the defect is about the whole file.
    std::string detail;        ///< Human-readable, never the raw secret in full.
};

std::string describe(DefectKind kind) {
    switch (kind) {
    case DefectKind::MissingSpdxHeader:
        return "missing SPDX-License-Identifier: GPL-3.0-or-later";
    case DefectKind::SpdxOutsideLeadingComment:
        return "SPDX identifier is not in the leading comment block";
    case DefectKind::WrongSpdxLicense:
        return "SPDX identifier names a licence other than GPL-3.0-or-later";
    case DefectKind::CredentialLiteral:
        return "credential-shaped literal with an opaque value";
    }
    return "unknown defect";
}

// ===========================================================================
// Clause 1 — the SPDX header
// ===========================================================================
//
// "Carries the header" is read as design.md Property 68 words it: the SPDX
// identifier appears in the file's LEADING COMMENT BLOCK. That block is the
// run of comment and blank lines from the top of the file, and it ends at the
// first line that is neither. Requiring the leading block (rather than
// "somewhere in the file") is what makes the check about licensing rather than
// about the string occurring in, say, a test fixture halfway down.

constexpr std::string_view kSpdxTag = "SPDX-License-Identifier:";
constexpr std::string_view kRequiredLicense = "GPL-3.0-or-later";

enum class CommentStyle { Cxx, Hash };

CommentStyle commentStyleFor(const std::string& fileName) {
    // CMake input is `#`-commented; every other file this property enumerates is
    // C or C++, which is `//` / `/* */`-commented.
    if (fileName == "CMakeLists.txt") {
        return CommentStyle::Hash;
    }
    const std::size_t dot = fileName.rfind('.');
    const std::string extension = dot == std::string::npos ? std::string{} : fileName.substr(dot);
    return extension == ".cmake" ? CommentStyle::Hash : CommentStyle::Cxx;
}

std::vector<std::string> splitLines(std::string_view content) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= content.size()) {
        const std::size_t end = content.find('\n', start);
        if (end == std::string_view::npos) {
            if (start < content.size()) {
                lines.emplace_back(content.substr(start));
            }
            break;
        }
        std::string line{content.substr(start, end - start)};
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

std::string_view trimLeft(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
        ++i;
    }
    return text.substr(i);
}

// The number of leading lines that form the leading comment block. Blank lines
// inside it are tolerated; the first line of code (or of anything that is not a
// comment) closes it.
std::size_t leadingCommentBlockLength(const std::vector<std::string>& lines, CommentStyle style) {
    bool inBlockComment = false;
    std::size_t count = 0;
    for (const std::string& raw : lines) {
        const std::string_view line = trimLeft(raw);
        if (inBlockComment) {
            ++count;
            if (line.find("*/") != std::string_view::npos) {
                inBlockComment = false;
            }
            continue;
        }
        if (line.empty()) {
            ++count;
            continue;
        }
        if (style == CommentStyle::Hash) {
            if (line.front() == '#' && line.substr(0, 7) != "#pragma") {
                ++count;
                continue;
            }
            break;
        }
        if (line.substr(0, 2) == "//") {
            ++count;
            continue;
        }
        if (line.substr(0, 2) == "/*") {
            ++count;
            if (line.find("*/", 2) == std::string_view::npos) {
                inBlockComment = true;
            }
            continue;
        }
        break;
    }
    return count;
}

void checkSpdxHeader(const std::vector<std::string>& lines,
                     CommentStyle style,
                     std::vector<Defect>& out) {
    const std::size_t blockLength = leadingCommentBlockLength(lines, style);

    std::size_t firstTagLine = 0;  // 1-based
    bool tagInBlock = false;
    bool correctLicenseInBlock = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(kSpdxTag) == std::string::npos) {
            continue;
        }
        if (firstTagLine == 0) {
            firstTagLine = i + 1;
        }
        if (i < blockLength) {
            tagInBlock = true;
            if (lines[i].find(kRequiredLicense) != std::string::npos) {
                correctLicenseInBlock = true;
                break;
            }
        }
    }

    if (correctLicenseInBlock) {
        return;
    }
    if (tagInBlock) {
        out.push_back({DefectKind::WrongSpdxLicense, firstTagLine,
                       "leading comment block declares an SPDX licence that is not "
                       + std::string{kRequiredLicense}});
        return;
    }
    if (firstTagLine != 0) {
        out.push_back({DefectKind::SpdxOutsideLeadingComment, firstTagLine,
                       "SPDX identifier appears at line " + std::to_string(firstTagLine)
                           + ", below the leading comment block of "
                           + std::to_string(blockLength) + " line(s)"});
        return;
    }
    out.push_back({DefectKind::MissingSpdxHeader, 0,
                   "no " + std::string{kSpdxTag} + " " + std::string{kRequiredLicense}
                       + " in the leading comment block"});
}

// ===========================================================================
// Clause 2 — no credential values
// ===========================================================================
//
// THE RULE, in two halves.
//
// (a) SHAPE. A candidate is produced only by a specific pattern:
//       * a PEM `BEGIN ... PRIVATE KEY` block THAT CARRIES BASE64 BODY
//         MATERIAL (a bare BEGIN marker is a header, not a secret: the key is
//         the body, and this repository's TLS fixtures write exactly the marker
//         and nothing else, which is why the body is what is required);
//       * a vendor-prefixed key id — AWS `AKIA/ASIA/ABIA/ACCA` + 16, Google
//         `AIza` + 35, OpenAI `sk-`, Stripe live `sk_live_`/`rk_live_`, GitHub
//         `ghp_/gho_/ghu_/ghs_/ghr_`, Slack `xox[abprs]-`;
//       * a JWT (`eyJ` header segment, three dot-separated base64url runs);
//       * an ASSIGNMENT whose left side names a credential — `api_key`,
//         `apikey`, `secret`, `token`, `password`, `passwd`, `credential`,
//         `access_key`, `client_secret`, `auth_token`, `authorization` — to a
//         double-quoted literal, via `=`, `:` or `=>` (so both C++ and
//         JSON/YAML spellings are covered);
//       * an `Authorization: Bearer <v>` / `Basic <v>` value.
//
// (b) VALUE. The candidate value must then look OPAQUE — machine-generated.
//     `isPlaceholderValue` excuses it otherwise, and the whole art of this
//     property is in that function, because a credential-shaped line with a
//     human-written value is the normal state of a test suite.
//
// The two halves are deliberately asymmetric: the shape half is loose (it will
// look at a great many lines of this repository) and the value half is what
// discriminates. Widening the shape half costs nothing; widening the value half
// would blind the check. No file, directory or line is exempt.

// A vocabulary of words that a real hosted-service credential does not contain
// and a hand-written stand-in almost always does. This is the crux clause: a
// secret is opaque, and a value that NAMES ITSELF ("...-token", "supersecret...",
// "not-the-configured-token", "AKIAIOSFODNN7EXAMPLE") is a description of a
// secret rather than one. Applied to the VALUE only — never to the surrounding
// identifier, the line, the file or the directory — so `RemoteAccessGate`'s
// thousands of legitimate mentions of `token` are untouched by it.
const std::array<std::string_view, 27> kSelfDescribingWords{
    "token",    "secret",   "password", "passwd",      "credential", "apikey", "api_key",
    "api-key",  "example",  "sample",   "specimen",    "placeholder", "dummy", "fake",
    "stub",     "mock",     "test",     "notreal",     "invalid",    "wrong",  "unused",
    "changeme", "redacted", "elided",   "yourkey",     "your-",      "mykey"};

std::string toLower(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char c : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

// Shannon entropy per character, in bits.
double entropyBitsPerChar(std::string_view value) {
    if (value.empty()) {
        return 0.0;
    }
    std::map<char, std::size_t> histogram;
    for (const char c : value) {
        ++histogram[c];
    }
    double bits = 0.0;
    for (const auto& [character, count] : histogram) {
        const double p = static_cast<double>(count) / static_cast<double>(value.size());
        bits -= p * std::log2(p);
    }
    return bits;
}

// True when the value is some prefix of length <= size/2 repeated to cover the
// whole string (a trailing partial repetition counts). Hand-written filler such
// as "0123456789abcdef0123456789abcdef01234567" is periodic; a generated secret
// is not.
bool isPeriodic(std::string_view value) {
    for (std::size_t period = 1; period * 2 <= value.size(); ++period) {
        bool matches = true;
        for (std::size_t i = period; i < value.size() && matches; ++i) {
            matches = value[i] == value[i % period];
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

// The fraction of adjacent character pairs that are consecutive in ASCII, in
// either direction. Keyboard-walked filler ("0123456789abcdef", "abcdefghij")
// scores near 1.0; a generated secret scores near 0.
double consecutiveRunFraction(std::string_view value) {
    if (value.size() < 2) {
        return 0.0;
    }
    std::size_t consecutive = 0;
    for (std::size_t i = 1; i < value.size(); ++i) {
        const int delta = static_cast<int>(static_cast<unsigned char>(value[i]))
                          - static_cast<int>(static_cast<unsigned char>(value[i - 1]));
        if (delta == 1 || delta == -1) {
            ++consecutive;
        }
    }
    return static_cast<double>(consecutive) / static_cast<double>(value.size() - 1);
}

std::size_t distinctCharacters(std::string_view value) {
    std::array<bool, 256> seen{};
    std::size_t count = 0;
    for (const char c : value) {
        auto& slot = seen[static_cast<unsigned char>(c)];
        if (!slot) {
            slot = true;
            ++count;
        }
    }
    return count;
}

bool looksLikeUuid(std::string_view value) {
    static const std::regex uuid{
        R"(^\{?[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}?$)"};
    return std::regex_match(value.begin(), value.end(), uuid);
}

// The reason a candidate value is NOT reported, or an empty string when it is
// reported. Returning the reason (rather than a bool) keeps the rule auditable:
// the property prints, for any excused candidate, which clause excused it.
std::string placeholderReason(std::string_view value) {
    // P1 — too short to be a hosted-service credential. Real API keys, bearer
    // tokens and secret access keys are 20+ characters; 16 is a conservative
    // floor that still admits every shape this property looks for.
    if (value.size() < 16) {
        return "shorter than 16 characters";
    }
    // P4 — a reference to a value rather than a value: an environment variable
    // name, a format string, a CMake/shell expansion, a CLI flag.
    const std::string lowered = toLower(value);
    if (value.find("${") != std::string_view::npos || value.find("$(") != std::string_view::npos
        || value.find("%s") != std::string_view::npos
        || value.find("{}") != std::string_view::npos
        || value.substr(0, 2) == "--") {
        return "a reference or format string, not a literal value";
    }
    // An environment-variable name: ALL_CAPS *with underscores*. The underscore
    // requirement is load-bearing and was found by the non-vacuity test below —
    // "all caps and digits" alone also describes an AWS access key id
    // (AKIA + 16 uppercase), so without it this clause silently swallowed the
    // single most recognisable credential shape there is.
    if (value.find('_') != std::string_view::npos
        && std::all_of(value.begin(), value.end(), [](const char c) {
               return (std::isupper(static_cast<unsigned char>(c)) != 0) || c == '_'
                      || (std::isdigit(static_cast<unsigned char>(c)) != 0);
           })) {
        return "an ALL_CAPS environment-variable style name";
    }
    // P5 — a canonical UUID. This repository's asset, clip, track and project
    // identifiers are UUIDs and appear in argument objects next to credential
    // words. (Accepted cost: a hosted service whose API key is literally a
    // canonical UUID would be excused by this clause.)
    if (looksLikeUuid(value)) {
        return "a canonical UUID (this repository's identifier shape)";
    }
    // P2 — self-describing. THE crux clause; see kSelfDescribingWords.
    for (const std::string_view word : kSelfDescribingWords) {
        if (lowered.find(word) != std::string::npos) {
            return "self-describing: the value contains \"" + std::string{word} + "\"";
        }
    }
    // P3 — structurally hand-written rather than generated.
    if (distinctCharacters(value) < 5) {
        return "fewer than 5 distinct characters";
    }
    if (isPeriodic(value)) {
        return "a repeated pattern";
    }
    if (consecutiveRunFraction(value) >= 0.6) {
        return "mostly runs of consecutive characters";
    }
    if (entropyBitsPerChar(value) < 2.0) {
        return "entropy below 2 bits per character";
    }
    // A path, not a secret: a value that is mostly '/'-separated words.
    if (value.find('/') != std::string_view::npos
        && value.find_first_of(" \t") == std::string_view::npos
        && std::count(value.begin(), value.end(), '/') >= 2) {
        return "a filesystem path";
    }
    return {};
}

bool isPlaceholderValue(std::string_view value) { return !placeholderReason(value).empty(); }

// Never echo a suspected secret in full into test output.
std::string redact(std::string_view value) {
    const std::size_t prefix = std::min<std::size_t>(4, value.size());
    std::ostringstream out;
    out << '"' << value.substr(0, prefix) << "\u2026\" (" << value.size() << " characters)";
    return out.str();
}

// --- the shape patterns ----------------------------------------------------

const std::regex& vendorKeyPattern() {
    static const std::regex pattern{
        R"(((?:AKIA|ASIA|ABIA|ACCA)[0-9A-Z]{16})"
        R"(|AIza[0-9A-Za-z_\-]{35})"
        R"(|sk-(?:proj-)?[A-Za-z0-9_\-]{20,})"
        R"(|(?:sk|rk)_live_[0-9A-Za-z]{16,})"
        R"(|gh[pousr]_[A-Za-z0-9]{36,})"
        R"(|xox[abprs]-[A-Za-z0-9\-]{12,})"
        R"(|eyJ[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}))"};
    return pattern;
}

const std::regex& credentialAssignmentPattern() {
    // <credential word><identifier tail><optional quote/bracket> <= | : | =>> "value"
    static const std::regex pattern{
        R"((?:api[_\-]?key|apikey|secret|token|password|passwd|credential|access[_\-]?key)"
        R"(|client[_\-]?secret|auth[_\-]?token|authorization)[A-Za-z0-9_]*)"
        R"([\"'\]]?\s*(?:=>|[=:])\s*\"([^\"]*)\")",
        std::regex::icase};
    return pattern;
}

const std::regex& authorizationHeaderPattern() {
    static const std::regex pattern{R"((?:Bearer|Basic)\s+([A-Za-z0-9._\-+/=]{8,}))",
                                    std::regex::icase};
    return pattern;
}

// A cheap pre-filter: only lines that mention something credential-ish are run
// through the regexes above, which keeps the whole-tree scan fast.
bool mightMentionCredential(const std::string& loweredLine) {
    static const std::array<std::string_view, 14> needles{
        "key", "secret", "token", "password", "passwd", "credential", "authoriz",
        "bearer", "basic ", "akia", "asia", "aiza", "eyj", "xox"};
    return std::any_of(needles.begin(), needles.end(), [&loweredLine](const std::string_view n) {
        return loweredLine.find(n) != std::string::npos;
    });
}

// A whole line of base64 — which is what a PEM body line is, and what an
// identifier or a quoted fragment of one is not. Requiring the WHOLE line
// matters: `[A-Za-z0-9]{40,}` alone also matches long C++ identifiers such as
// this file's own test names, which would make any PEM marker anywhere in a file
// contaminate the 64 lines after it.
bool isPemBodyLine(std::string_view line) {
    std::string_view trimmed = trimLeft(line);
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'
                               || trimmed.back() == '\r')) {
        trimmed.remove_suffix(1);
    }
    if (trimmed.size() < 40) {
        return false;
    }
    return std::all_of(trimmed.begin(), trimmed.end(), [](const char c) {
        return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '+' || c == '/'
               || c == '=';
    });
}

// A PEM private-key marker is only a finding when KEY MATERIAL follows it: a
// body line of 40+ base64 characters before the END marker. A bare
// `-----BEGIN PRIVATE KEY-----\n` — which is exactly what this repository's TLS
// fixtures write, so that a key *file* exists for the gate to open — carries no
// secret at all: the secret in a PEM file is its body.
void checkPrivateKeyBlocks(const std::vector<std::string>& lines, std::vector<Defect>& out) {
    static const std::regex begin{R"(-----BEGIN (?:[A-Z0-9 ]+ )?PRIVATE KEY-----)"};

    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!std::regex_search(lines[i], begin)) {
            continue;
        }
        bool hasBody = false;
        const std::size_t last = std::min(lines.size(), i + 64);
        for (std::size_t j = i + 1; j < last; ++j) {
            if (lines[j].find("-----END") != std::string::npos) {
                break;
            }
            if (isPemBodyLine(lines[j])) {
                hasBody = true;
                break;
            }
        }
        if (hasBody) {
            out.push_back({DefectKind::CredentialLiteral, i + 1,
                           "a PEM private key block with base64 key material"});
        }
    }
}

// A candidate credential SHAPE whose VALUE was judged human-written, together
// with the clause that judged it. Recorded so the suite can show that the shape
// half of the rule really does fire on this repository — a scan that reported
// zero candidates would be passing because it matched nothing, which is a very
// different thing from passing because it found no secrets.
struct ExcusedCandidate {
    std::size_t line{0};
    std::string reason;
};

void checkCredentials(const std::vector<std::string>& lines,
                      std::vector<Defect>& out,
                      std::vector<ExcusedCandidate>* excused) {
    const auto judge = [&out, excused](const std::string& value, const std::size_t line,
                                       const std::string& shape) {
        const std::string reason = placeholderReason(value);
        if (reason.empty()) {
            out.push_back({DefectKind::CredentialLiteral, line, shape + " " + redact(value)});
        } else if (excused != nullptr) {
            excused->push_back({line, reason});
        }
    };

    checkPrivateKeyBlocks(lines, out);

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        const std::string lowered = toLower(line);
        if (!mightMentionCredential(lowered)) {
            continue;
        }

        for (std::sregex_iterator it{line.begin(), line.end(), vendorKeyPattern()}, end;
             it != end; ++it) {
            judge((*it)[1].str(), i + 1, "a vendor-prefixed credential");
        }
        for (std::sregex_iterator it{line.begin(), line.end(), credentialAssignmentPattern()}, end;
             it != end; ++it) {
            judge((*it)[1].str(), i + 1, "a credential-named assignment to");
        }
        for (std::sregex_iterator it{line.begin(), line.end(), authorizationHeaderPattern()}, end;
             it != end; ++it) {
            judge((*it)[1].str(), i + 1, "an Authorization header credential");
        }
    }
}

// ===========================================================================
// The checker, as one pure function
// ===========================================================================

std::vector<Defect> scanContent(const std::string& fileName,
                                std::string_view content,
                                std::vector<ExcusedCandidate>* excused = nullptr) {
    std::vector<Defect> defects;
    const std::vector<std::string> lines = splitLines(content);
    checkSpdxHeader(lines, commentStyleFor(fileName), defects);
    checkCredentials(lines, defects, excused);
    return defects;
}

// ===========================================================================
// The file set — discovered, not listed
// ===========================================================================
//
// Scanned: every regular file under `src/`, `tests/` and `cmake/` whose name is
// `CMakeLists.txt` or whose extension is one of .cpp .cc .cxx .hpp .h .hh .cmake
// — i.e. the C++ sources and the build inputs, the files Requirement 12.6's
// "every added source file" is about.
//
// NOT scanned, and why:
//   * anything outside those three directories. `build-*/` in particular holds
//     FetchContent's GoogleTest and RapidCheck checkouts, which are third-party
//     code under their own licences and generated CMake output — neither is an
//     "added source file" of this repository, and neither is committed (they are
//     in .gitignore). Because the three scanned directories are named
//     positively, no exclusion list is needed to keep them out, and none can
//     silently grow to cover a real source file.
//   * non-source files (docs, .json, .qrc, images, .in templates): they carry no
//     compiled code and the requirement speaks of source files. Note this is a
//     scope boundary, not a hole in the credential half — see the note below.
//
// Two deliberate non-exclusions worth naming: `tests/` is scanned exactly as
// `src/` is (test fixtures are where credential-shaped strings actually live, so
// exempting tests would gut the check), and no individual file is exempt.
//
// Known scope boundary: a credential pasted into a non-source file (a README, a
// packaging script) is outside this property, because the enumerated set is the
// source set. Extending the enumeration to the whole working tree is a
// requirements question, not something to decide inside a test.

const std::array<std::string_view, 3> kScannedDirectories{"src", "tests", "cmake"};

bool isScannedSource(const std::filesystem::path& path) {
    if (path.filename() == "CMakeLists.txt") {
        return true;
    }
    static const std::array<std::string_view, 7> extensions{".cpp", ".cc",  ".cxx", ".hpp",
                                                            ".h",   ".hh", ".cmake"};
    const std::string extension = path.extension().string();
    return std::any_of(extensions.begin(), extensions.end(),
                       [&extension](const std::string_view candidate) {
                           return extension == candidate;
                       });
}

struct ScannedFile {
    std::string relativePath;
    std::vector<Defect> defects;
};

struct HygieneScan {
    std::vector<ScannedFile> files;
    std::size_t defectCount{0};
    std::size_t excusedCount{0};                    ///< Credential shapes judged human-written.
    std::map<std::string, std::size_t> excusedByReason;
    std::string enumerationError;   ///< Non-empty when the tree could not be walked.
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

HygieneScan performScan() {
    HygieneScan scan;
    const std::filesystem::path root{PALMIER_SOURCE_DIR};

    if (!std::filesystem::is_directory(root)) {
        scan.enumerationError = "PALMIER_SOURCE_DIR is not a directory: " + root.string();
        return scan;
    }
    for (const std::string_view directory : kScannedDirectories) {
        const std::filesystem::path base = root / directory;
        if (!std::filesystem::is_directory(base)) {
            scan.enumerationError =
                "expected source directory is missing: " + base.string();
            return scan;
        }
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator{base}) {
            if (!entry.is_regular_file() || !isScannedSource(entry.path())) {
                continue;
            }
            ScannedFile file;
            file.relativePath = std::filesystem::relative(entry.path(), root).string();
            std::vector<ExcusedCandidate> excused;
            file.defects =
                scanContent(entry.path().filename().string(), readFile(entry.path()), &excused);
            scan.defectCount += file.defects.size();
            scan.excusedCount += excused.size();
            for (const ExcusedCandidate& candidate : excused) {
                // Keep only the clause, not the word that matched, so the
                // histogram stays readable.
                const std::size_t quote = candidate.reason.find(": the value");
                ++scan.excusedByReason[quote == std::string::npos ? candidate.reason
                                                                 : candidate.reason.substr(0,
                                                                                           quote)];
            }
            scan.files.push_back(std::move(file));
        }
    }
    std::sort(scan.files.begin(), scan.files.end(),
              [](const ScannedFile& a, const ScannedFile& b) {
                  return a.relativePath < b.relativePath;
              });
    if (scan.files.empty()) {
        scan.enumerationError = "no source files were found under " + root.string();
    }
    return scan;
}

// Walked, read and scanned once per process; every generated case reads this.
const HygieneScan& hygieneScan() {
    static const HygieneScan scan = performScan();
    return scan;
}

std::string report(const ScannedFile& file) {
    std::ostringstream out;
    for (const Defect& defect : file.defects) {
        out << file.relativePath;
        if (defect.line != 0) {
            out << ':' << defect.line;
        }
        out << " — " << describe(defect.kind) << ": " << defect.detail << '\n';
    }
    return out.str();
}

std::string reportAll(const HygieneScan& scan) {
    std::ostringstream out;
    for (const ScannedFile& file : scan.files) {
        out << report(file);
    }
    return out.str();
}

}  // namespace

// ===========================================================================
// Property 68
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 68: Every source file
// carries the GPLv3 SPDX header and no credentials — for all source files under
// src/ and tests/, the file's leading comment block contains
// SPDX-License-Identifier: GPL-3.0-or-later, and no file contains a literal
// matching the hosted-service credential patterns.
// Validates: Requirements 12.6
RC_GTEST_PROP(RepositoryHygieneProperties,
              EverySourceFileCarriesTheGplHeaderAndNoCredentialLiteral,
              ()) {
    const HygieneScan& scan = hygieneScan();

    // A tree that could not be enumerated must fail loudly: a property over an
    // empty domain is vacuously true, which is the failure mode this guards.
    RC_ASSERT(scan.enumerationError.empty());
    RC_ASSERT(scan.files.size() >= 100u);

    // The universally quantified variable of Property 68 is the file. Drawing
    // one gives RapidCheck something to shrink to and to name on failure.
    const auto index = *rc::gen::inRange<std::size_t>(0, scan.files.size());
    const ScannedFile& drawn = scan.files[index];
    if (!drawn.defects.empty()) {
        RC_FAIL(report(drawn));
    }

    // The scan is cached, so re-asserting the whole tree costs nothing and
    // guarantees the property covers every file in every case, rather than only
    // the files sampling happened to draw.
    if (scan.defectCount != 0) {
        RC_FAIL(reportAll(scan));
    }
}

// ===========================================================================
// Non-vacuity: the checker is proven able to fail
// ===========================================================================
//
// Property 68 above passes. That is only meaningful if the checker CAN fail, so
// each check is driven here over synthetic content through the very same
// `scanContent` the property uses. The negative controls at the end are this
// repository's own real fixtures, which pin the false-positive rule: were the
// value half of the credential rule to be tightened into flagging them, these
// cases would fail rather than the tree turning red.

namespace {

bool hasKind(const std::vector<Defect>& defects, DefectKind kind) {
    return std::any_of(defects.begin(), defects.end(),
                       [kind](const Defect& d) { return d.kind == kind; });
}

constexpr std::string_view kGoodHeader = "// SPDX-License-Identifier: GPL-3.0-or-later\n//\n";

// Synthetic credentials are ASSEMBLED AT RUN TIME from two fragments, so that no
// single line of THIS file ever contains a complete credential shape. That
// matters because this file lives under `tests/` and is therefore scanned by its
// own property: the alternative would be an inline "ignore this line" escape
// hatch, i.e. exactly the kind of exclusion mechanism that could later be
// pointed at real code. Each fragment on its own is below every length floor the
// checker uses, and the assembled value is a full credential shape.
std::string join(std::string_view head, std::string_view tail) {
    return std::string{head} + std::string{tail};
}

std::string withHeader(const std::string& body) { return std::string{kGoodHeader} + body; }

}  // namespace

TEST(RepositoryHygieneChecker, TheRealTreeIsCleanAndWasActuallyEnumerated) {
    const HygieneScan& scan = hygieneScan();
    ASSERT_EQ(scan.enumerationError, "") << "the source tree could not be walked";
    EXPECT_GE(scan.files.size(), 100u) << "suspiciously few source files were enumerated";
    EXPECT_EQ(scan.defectCount, 0u) << '\n' << reportAll(scan);
}

TEST(RepositoryHygieneChecker, TheCredentialRuleIsExercisedByTheRealTreeNotMerelySilent) {
    // Property 68 passing is only good news if the credential half of the rule
    // actually LOOKED at something. This repository is full of credential shapes
    // — fake bearer tokens, PEM markers, `authToken` assignments — so the shape
    // patterns must produce candidates, every one of which must then have been
    // excused by a named value clause. A zero here would mean the property is
    // green because nothing matched, which no amount of tree-walking would fix.
    const HygieneScan& scan = hygieneScan();
    ASSERT_EQ(scan.enumerationError, "");

    std::ostringstream histogram;
    for (const auto& [reason, count] : scan.excusedByReason) {
        histogram << "  " << count << " x " << reason << '\n';
    }
    EXPECT_GT(scan.excusedCount, 0u)
        << "no credential shape was found anywhere in the tree — the patterns are not matching";
    EXPECT_GE(scan.excusedByReason.size(), 2u)
        << "only one excuse clause ever fired; the rule may be excusing by accident:\n"
        << histogram.str();
    std::cout << "credential shapes found and excused by value: " << scan.excusedCount << '\n'
              << histogram.str();
}

TEST(RepositoryHygieneChecker, DetectsAMissingSpdxHeader) {
    const std::vector<Defect> defects =
        scanContent("Widget.cpp", "#include <vector>\n\nint main() { return 0; }\n");
    EXPECT_TRUE(hasKind(defects, DefectKind::MissingSpdxHeader));
}

TEST(RepositoryHygieneChecker, DetectsAnSpdxHeaderBelowTheLeadingCommentBlock) {
    const std::vector<Defect> defects =
        scanContent("Widget.cpp",
                    "#include <vector>\n\n// SPDX-License-Identifier: GPL-3.0-or-later\n");
    EXPECT_TRUE(hasKind(defects, DefectKind::SpdxOutsideLeadingComment));
    EXPECT_FALSE(hasKind(defects, DefectKind::MissingSpdxHeader));
}

TEST(RepositoryHygieneChecker, DetectsTheWrongLicenceInTheHeader) {
    const std::vector<Defect> defects =
        scanContent("Widget.cpp", "// SPDX-License-Identifier: MIT\n\nint main() { return 0; }\n");
    EXPECT_TRUE(hasKind(defects, DefectKind::WrongSpdxLicense));
}

TEST(RepositoryHygieneChecker, AcceptsTheHeaderInABlockCommentAndInAHashComment) {
    EXPECT_TRUE(scanContent("Widget.cpp",
                            "/*\n * SPDX-License-Identifier: GPL-3.0-or-later\n */\nint x = 0;\n")
                    .empty());
    EXPECT_TRUE(scanContent("CMakeLists.txt",
                            "# SPDX-License-Identifier: GPL-3.0-or-later\nadd_library(a b.cpp)\n")
                    .empty());
}

TEST(RepositoryHygieneChecker, DetectsAnOpaqueCredentialAssignment) {
    const std::string content =
        withHeader("const char* kApiKey = \"" + join("9f83Ba2QmZ7v", "LpR4tYwXcE1dNhUoS6")
                   + "\";\n");
    const std::vector<Defect> defects = scanContent("Widget.cpp", content);
    EXPECT_TRUE(hasKind(defects, DefectKind::CredentialLiteral)) << content;
}

TEST(RepositoryHygieneChecker, DetectsAnOpaqueCredentialInJsonAndYamlSpellings) {
    const std::string json =
        withHeader("{\"client_secret\": \"" + join("Qh7Zm2Wv9Lx4Rb", "8Tn3Ck6Yp1Dg5Fs0Ju")
                   + "\"}\n");
    EXPECT_TRUE(hasKind(scanContent("Widget.cpp", json), DefectKind::CredentialLiteral)) << json;

    const std::string yaml =
        withHeader("auth_token: \"" + join("Vr4Nq8Zj2Hm6Bx", "1Ld9Tc5Ky3Ws7Pf") + "\"\n");
    EXPECT_TRUE(hasKind(scanContent("Widget.cpp", yaml), DefectKind::CredentialLiteral)) << yaml;
}

TEST(RepositoryHygieneChecker, DetectsAnAwsAccessKeyId) {
    const std::string content =
        withHeader("const char* id = \"" + join("AKIA", "3XQ7ZR2VB6WNTJ5C") + "\";\n");
    EXPECT_TRUE(hasKind(scanContent("Widget.cpp", content), DefectKind::CredentialLiteral))
        << content;
}

TEST(RepositoryHygieneChecker, DetectsAJsonWebTokenAndAnOpaqueBearerHeader) {
    const std::string jwt =
        withHeader("// " + join("eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiI5OTk5OTk5OTk5In0.",
                                "Rq7Zn2Vm8Lx4Bb1Tk6Yc3Dg5Fs")
                   + "\n");
    EXPECT_TRUE(hasKind(scanContent("Widget.cpp", jwt), DefectKind::CredentialLiteral)) << jwt;

    const std::string bearer =
        withHeader("headers.emplace_back(\"authorization\", \"Bearer "
                   + join("Zt6Wq2Nm9Vx4Lb", "8Rk3Cy7Dp1Hs5Gf") + "\");\n");
    EXPECT_TRUE(hasKind(scanContent("Widget.cpp", bearer), DefectKind::CredentialLiteral))
        << bearer;
}

TEST(RepositoryHygieneChecker, DetectsAPemPrivateKeyWithBodyButNotABareMarker) {
    const std::string withBody =
        withHeader("-----BEGIN PRIVATE KEY-----\n"
                   + join("MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcw", "ggSjAgEAAoIBAQC7VJTUt9Us8cKj")
                   + "\n-----END PRIVATE KEY-----\n");
    EXPECT_TRUE(hasKind(scanContent("Widget.cpp", withBody), DefectKind::CredentialLiteral))
        << withBody;

    // The shape this repository's TLS fixtures actually write: a marker, no key.
    const std::string markerOnly = withHeader(
        "writeText(directory_ / \"valid.key\", \"-----BEGIN PRIVATE KEY-----\\n\");\n");
    EXPECT_FALSE(hasKind(scanContent("Widget.cpp", markerOnly), DefectKind::CredentialLiteral));
}

TEST(RepositoryHygieneChecker, DoesNotFlagThisRepositorysLegitimateCredentialVocabulary) {
    // Every line below is copied from this repository. Each must be excused by a
    // named clause of placeholderReason(), not by any file or path exemption.
    const std::array<std::string_view, 9> realLines{
        R"(constexpr const char* kToken = "0123456789abcdef0123456789abcdef01234567";)",
        R"(constexpr const char* kToken = "0123456789abcdef0123456789abcdef0123456789abcdef";)",
        R"(const std::string token = "s3cret-token-with-32-plus-characters";)",
        R"(const std::string presented = "supersecretpresentedtokenvalue0123456789";)",
        R"(failing.authorization = "Bearer definitely-not-the-configured-token";)",
        R"(request.headers.emplace_back("authorization", "Bearer token-value");)",
        R"(hostile.authorization = "Bearer not-the-token";)",
        R"(std::string authToken; ///< Non-empty bearer authorizing the hosted request.)",
        R"(return "the Authorization header is not a Bearer credential";)"};

    for (const std::string_view line : realLines) {
        const std::vector<Defect> defects =
            scanContent("Widget.cpp", std::string{kGoodHeader} + std::string{line} + "\n");
        EXPECT_FALSE(hasKind(defects, DefectKind::CredentialLiteral))
            << "false positive on a legitimate line: " << line << '\n'
            << (defects.empty() ? std::string{} : defects.front().detail);
    }
}

TEST(RepositoryHygieneChecker, EveryPlaceholderClauseIsReachableAndNamed) {
    // The excuse clauses are the part of the rule that could hide a real leak,
    // so each is pinned to the shape it is meant to excuse — and an opaque
    // value is pinned to being excused by none of them.
    EXPECT_NE(placeholderReason("shortsecret").find("shorter than"), std::string::npos);
    EXPECT_NE(placeholderReason("${PALMIER_REMOTE_AUTH}").find("reference"), std::string::npos);
    EXPECT_NE(placeholderReason("PALMIER_REMOTE_AUTH_TOKEN_VALUE").find("ALL_CAPS"),
              std::string::npos);
    EXPECT_NE(placeholderReason("3f2504e0-4f89-41d3-9a0c-0305e82c3301").find("UUID"),
              std::string::npos);
    EXPECT_NE(placeholderReason("Xq7Wm2Zn9-my-token-here").find("self-describing"),
              std::string::npos);
    // This repository's 40- and 48-character hex fixtures are hand-written
    // repetitions of "0123456789abcdef", which is what excuses them.
    EXPECT_NE(placeholderReason("0123456789abcdef0123456789abcdef01234567").find("repeated"),
              std::string::npos);
    EXPECT_NE(placeholderReason(join("Zq3", "defghijklmnopqrstuvw")).find("consecutive"),
              std::string::npos);
    EXPECT_NE(placeholderReason("aaaaaaaaaaaaaaaaaaaab").find("distinct"), std::string::npos);
    EXPECT_NE(placeholderReason("/usr/local/lib/pkgconfig/libavcodec").find("path"),
              std::string::npos);
    // ...and an opaque value is excused by none of the clauses.
    EXPECT_EQ(placeholderReason(join("9f83Ba2QmZ7v", "LpR4tYwXcE1dNhUoS6")), "");
}
