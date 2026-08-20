// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/docs/suite_hygiene_property_test.cpp — the suite hygiene property
// (task 12.9; design.md Property 80; Requirement 15.6).
//
// Requirement 15.6, in full:
//
//   "THE Verification_Suite SHALL contain no placeholder test, and every test it
//    registers with CTest SHALL assert observable behaviour of at least one
//    component named in this document."
//
// Property 80, as design.md states it:
//
//   "For all test source files in tests/ and all targets registered with CTest,
//    no source matches the placeholder pattern (a property or test whose body
//    asserts only a tautology over generated values and links no `Palmier::`
//    library), and every registered target links at least one `Palmier::`
//    library and references at least one component named in this design."
//
// This file lands in the same commit that deletes
// `tests/palmier_placeholder_property_test.cpp` and its `palmier_placeholder_tests`
// target, so Requirement 15.6 is never transiently violated: the rule that forbids
// the placeholder arrives with its removal, not after it.
//
// What the check actually is
// ---------------------------------------------------------------------------
// Six defects, each a pure function of (the parsed `tests/CMakeLists.txt`, the
// enumerated test sources, the discovered component set):
//
//   1. RegisteredTargetIsNotDefined      — registered with CTest, no add_executable().
//   2. TargetCompilesNoProductCode       — the binary contains none of this
//                                          product: no `Palmier::` library other
//                                          than `Palmier::test_support`, and no
//                                          source under `src/`.
//   3. TargetReferencesNoNamedComponent  — none of the target's own test sources
//                                          references a documented component.
//   4. PlaceholderTestSource             — a source that declares test cases,
//                                          references no component of this product,
//                                          and whose every owning target compiles
//                                          no product code. This is design.md's
//                                          placeholder pattern, both conjuncts.
//   5. TestSourceIsNeverCompiled         — a test source no target compiles.
//   6. TestSourceIsNeverRegistered       — a source that declares test cases but is
//                                          compiled only into targets CTest never
//                                          registers, so those cases never run.
//
// Defect 2 is why `Palmier::test_support` is excluded from the "links a
// `Palmier::` library" clause. `Palmier::test_support` is the GoogleTest +
// RapidCheck bundle — the scaffolding, not the subject. The deleted placeholder
// target linked exactly that and nothing else, and would satisfy the clause read
// literally, which would make Property 80 unable to reject the very target it
// exists to reject. Compiling a `src/` source counts as well as linking a product
// library, because this tree's established pattern is to compile the specific
// service sources into a test binary rather than link the whole library (see the
// `palmier_services_*` targets).
//
// Defects 5 and 6 are the other half of "the suite has no dead weight": a test
// written and never compiled, or compiled and never registered, asserts nothing at
// all — the same defect as a placeholder wearing different clothes.
//
// "A component named in this document"
// ---------------------------------------------------------------------------
// Read as the INTERSECTION of two discovered sets, never a list written here:
//
//   * DECLARED — every name a header under `src/` declares (a class, struct, enum,
//     alias, or a function), with comments stripped so a name that only appears in
//     prose does not count as declared.
//   * DOCUMENTED — every word appearing in the documents this repository checks in
//     that describe the system: `docs/*.md` (the operator and agent-user set
//     Requirement 16 mandates) and every `requirements.md` / `design.md` under
//     `.kiro/specs/`.
//
// filtered to names of at least 4 characters carrying an interior case change
// (`camelCase` / `CamelCase`), which is what separates a component name from
// vocabulary such as `size`, `value` or `vector` that a header also declares. The
// result is ~225 names on this tree — `PreviewController`, `TimelineEngine`,
// `ExportCoordinator`, `checkPlatformCompatibility` and so on.
//
// Two readings are documented rather than assumed:
//
//  1. **The document set includes `docs/`, not only the specification.** Requirement
//     15.6 says "named in this document", meaning its own requirements document.
//     Taken alone that would condemn `tests/app/platform_compatibility_test.cpp`,
//     which exercises the launch-time compatibility gate — a real component, tested
//     for real, whose feature specification predates this one and is not checked
//     into this repository. Its component IS named in a checked-in document that
//     describes this system: `docs/BUILD.md` names `app::checkPlatformCompatibility`
//     and `CompatibilityRequirements::minGlibc` in the minimum-host table that
//     Requirement 16.1 requires. So the document set is every checked-in document
//     that describes the system, and it widens by itself as further specifications
//     land under `.kiro/specs/`. The clause the requirement is really making — a
//     test must bind to a named, real component of this product rather than to
//     nothing — is preserved exactly; what is not done is to fail an honest test
//     because the document naming its subject lives outside this repository.
//  2. **A reference must appear in CODE, not in a comment.** Component references
//     are counted after C and C++ comments are stripped, so a file cannot satisfy
//     the clause by mentioning `TimelineEngine` in its header comment. This is
//     load-bearing: with comments counted, the deleted placeholder file passes
//     (its own comment block names `GoogleTest`, `RapidCheck` and the property
//     conventions), and so would any future placeholder carrying a plausible
//     comment.
//
// Why this cannot be satisfied by enumeration
// ---------------------------------------------------------------------------
// Every set is discovered at run time from the source tree: the targets and their
// sources by parsing `tests/CMakeLists.txt`, the test sources by walking `tests/`,
// the component names by walking `src/` and the documents. A target registered
// tomorrow that links no product code, or a source added tomorrow that asserts
// nothing about this product, fails this property tomorrow with no edit here. No
// file, directory or target is exempt — including this file, which is subject to
// its own rule (it declares tests, it is compiled into `palmier_docs_tests`, and
// that target compiles product code, which is what places it outside the
// placeholder pattern; see `SuiteHygieneChecker.ThisFileIsSubjectToItsOwnRule`).
//
// Non-vacuity
// ---------------------------------------------------------------------------
// Property 80 passing is only meaningful if the checker can fail, so every rule is
// driven over synthetic models through the very same pure functions the scan uses:
// the deleted placeholder's own shape (reconstructed below) must be flagged, a
// registered target linking only the test-support bundle must be flagged, an
// orphan and an unregistered test source must be flagged, and each real-tree
// counterpart must not be. The CMake parser and the component extractor have their
// own cases, because a parser that silently found nothing would report a spotless
// suite.
//
// Cost
// ---------------------------------------------------------------------------
// The walk, the reads and the parse happen exactly ONCE per process in a
// function-local static; the property's >= 100 generated cases then read the cached
// model. The whole binary stays far inside the 600 s per-test limit.

#include <gtest/gtest.h>

#include <rapidcheck/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#ifndef PALMIER_SOURCE_DIR
#error "PALMIER_SOURCE_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif
#ifndef PALMIER_DOCS_DIR
#error "PALMIER_DOCS_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif
#ifndef PALMIER_SPEC_DIR
#error "PALMIER_SPEC_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

namespace {

// ===========================================================================
// Small text utilities
// ===========================================================================

constexpr bool isIdentifierStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

constexpr bool isIdentifierChar(char c) {
    return isIdentifierStart(c) || (c >= '0' && c <= '9');
}

constexpr bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/// Every identifier in `text`, in order, reported with the first non-whitespace
/// character that follows it (`'\0'` at end of input). Both the component
/// extractor and the declaration scanner are written on top of this, so they
/// cannot disagree about what an identifier is.
void forEachIdentifier(std::string_view text,
                       const std::function<void(std::string_view, char)>& visit) {
    std::size_t i = 0;
    while (i < text.size()) {
        if (!isIdentifierStart(text[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && isIdentifierChar(text[j])) {
            ++j;
        }
        std::size_t k = j;
        while (k < text.size() && isSpace(text[k])) {
            ++k;
        }
        visit(text.substr(i, j - i), k < text.size() ? text[k] : '\0');
        i = j;
    }
}

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                                        : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

/// True when `text` contains `word` as a whole identifier.
bool referencesWord(std::string_view text, std::string_view word) {
    std::size_t from = 0;
    while (true) {
        const std::size_t at = text.find(word, from);
        if (at == std::string_view::npos) {
            return false;
        }
        const bool leftClean = at == 0 || !isIdentifierChar(text[at - 1]);
        const std::size_t after = at + word.size();
        const bool rightClean = after >= text.size() || !isIdentifierChar(text[after]);
        if (leftClean && rightClean) {
            return true;
        }
        from = at + 1;
    }
}

/// C and C++ comments replaced by spaces, string and character literals kept.
/// Keeping literals costs nothing here and avoids a second escaping rule; the
/// point of the pass is that PROSE must not satisfy a code clause.
std::string stripCxxComments(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    enum class State { Code, LineComment, BlockComment, StringLiteral, CharLiteral };
    State state = State::Code;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        switch (state) {
        case State::Code:
            if (c == '/' && next == '/') {
                state = State::LineComment;
                out.push_back(' ');
                ++i;
                out.push_back(' ');
            } else if (c == '/' && next == '*') {
                state = State::BlockComment;
                out.push_back(' ');
                ++i;
                out.push_back(' ');
            } else {
                if (c == '"') {
                    state = State::StringLiteral;
                } else if (c == '\'') {
                    state = State::CharLiteral;
                }
                out.push_back(c);
            }
            break;
        case State::LineComment:
            if (c == '\n') {
                state = State::Code;
                out.push_back('\n');
            } else {
                out.push_back(' ');
            }
            break;
        case State::BlockComment:
            if (c == '*' && next == '/') {
                state = State::Code;
                out.push_back(' ');
                ++i;
            }
            out.push_back(c == '\n' ? '\n' : ' ');
            break;
        case State::StringLiteral:
            out.push_back(c);
            if (c == '\\' && i + 1 < text.size()) {
                out.push_back(text[i + 1]);
                ++i;
            } else if (c == '"') {
                state = State::Code;
            }
            break;
        case State::CharLiteral:
            out.push_back(c);
            if (c == '\\' && i + 1 < text.size()) {
                out.push_back(text[i + 1]);
                ++i;
            } else if (c == '\'') {
                state = State::Code;
            }
            break;
        }
    }
    return out;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// ===========================================================================
// The component set — discovered, never listed
// ===========================================================================

/// A name a header DECLARES: preceded by a declarator keyword (so `class Foo`,
/// `struct Foo`, `enum class Foo`, `using Foo =`) or followed by `(` (so a
/// function or constructor declaration). Run over comment-stripped content, so a
/// name that only appears in a doc comment is not "declared".
std::set<std::string> declaredNames(std::string_view headerContent) {
    static const std::array<std::string_view, 5> declarators{"class", "struct", "enum", "using",
                                                             "typedef"};
    // Keywords that are followed by `(` in ordinary code and are not components.
    static const std::array<std::string_view, 18> keywords{
        "if",       "for",    "while",  "switch",   "return",    "sizeof",
        "catch",    "throw",  "delete", "new",      "noexcept",  "alignof",
        "operator", "static", "and",    "or",       "not",       "decltype"};

    std::set<std::string> names;
    std::string previous;
    const std::string stripped = stripCxxComments(headerContent);
    forEachIdentifier(stripped, [&names, &previous](std::string_view word, char next) {
        const bool afterDeclarator =
            std::find(declarators.begin(), declarators.end(), previous) != declarators.end();
        const bool isKeyword =
            std::find(keywords.begin(), keywords.end(), word) != keywords.end();
        if (!isKeyword && (afterDeclarator || next == '(')) {
            names.emplace(word);
        }
        previous = std::string{word};
    });
    return names;
}

/// Every word a document contains. Cheap and deliberately dumb: the filter that
/// makes the intersection meaningful is `looksLikeComponentName` plus the
/// requirement that a header actually declares the name.
std::set<std::string> documentedWords(std::string_view documentText) {
    std::set<std::string> words;
    forEachIdentifier(documentText, [&words](std::string_view word, char) {
        words.emplace(word);
    });
    return words;
}

/// At least 4 characters and carrying an interior case change, i.e. `camelCase`
/// or `CamelCase`. This is what keeps `size`, `value`, `path` and `vector` — all
/// declared by headers and all present in the documents — out of the component
/// set, so that referencing one does not satisfy Requirement 15.6.
bool looksLikeComponentName(std::string_view name) {
    if (name.size() < 4) {
        return false;
    }
    for (std::size_t i = 1; i < name.size(); ++i) {
        const bool lowerThenUpper = std::islower(static_cast<unsigned char>(name[i - 1])) != 0 &&
                                    std::isupper(static_cast<unsigned char>(name[i])) != 0;
        if (lowerThenUpper) {
            return true;
        }
    }
    return false;
}

std::set<std::string> componentNames(const std::set<std::string>& declared,
                                     const std::set<std::string>& documented) {
    std::set<std::string> components;
    for (const std::string& name : declared) {
        if (looksLikeComponentName(name) && documented.count(name) != 0) {
            components.insert(name);
        }
    }
    return components;
}

// ===========================================================================
// The build model — parsed from tests/CMakeLists.txt
// ===========================================================================

struct TargetInfo {
    std::vector<std::string> sources;          ///< Repository-relative, '/'-separated.
    std::vector<std::string> palmierLibraries; ///< e.g. "Palmier::core".
    bool registeredWithCTest{false};

    [[nodiscard]] bool compilesProductCode() const {
        const bool productLibrary =
            std::any_of(palmierLibraries.begin(), palmierLibraries.end(),
                        [](const std::string& library) {
                            return library != "Palmier::test_support";
                        });
        const bool productSource =
            std::any_of(sources.begin(), sources.end(), [](const std::string& source) {
                return source.rfind("src/", 0) == 0;
            });
        return productLibrary || productSource;
    }

    /// The target's OWN test sources — the files under `tests/` it compiles, as
    /// opposed to the product sources it pulls in.
    [[nodiscard]] std::vector<std::string> testSources() const {
        std::vector<std::string> own;
        for (const std::string& source : sources) {
            if (source.rfind("tests/", 0) == 0) {
                own.push_back(source);
            }
        }
        return own;
    }
};

struct CMakeModel {
    std::map<std::string, TargetInfo> targets;
    std::vector<std::string>          registrationsWithoutTarget;
    std::string                       error; ///< Non-empty when parsing gave up.
};

/// `#` to end of line removed, outside double-quoted strings. Quote state is
/// tracked across lines, because a CMake string may span them.
std::string stripCMakeComments(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool inQuotes = false;
    bool inComment = false;
    for (const char c : text) {
        if (c == '\n') {
            inComment = false;
            out.push_back(c);
            continue;
        }
        if (inComment) {
            out.push_back(' ');
            continue;
        }
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (c == '#' && !inQuotes) {
            inComment = true;
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

struct CMakeCall {
    std::string name;
    std::string arguments;
};

/// Every `name(...)` command in the (comment-stripped) text, with its argument
/// text. Parentheses inside double quotes do not nest.
std::vector<CMakeCall> cmakeCalls(std::string_view text) {
    std::vector<CMakeCall> calls;
    std::size_t i = 0;
    while (i < text.size()) {
        if (!isIdentifierStart(text[i])) {
            ++i;
            continue;
        }
        const bool atBoundary = i == 0 || !isIdentifierChar(text[i - 1]);
        std::size_t j = i;
        while (j < text.size() && isIdentifierChar(text[j])) {
            ++j;
        }
        std::size_t k = j;
        while (k < text.size() && isSpace(text[k])) {
            ++k;
        }
        if (!atBoundary || k >= text.size() || text[k] != '(') {
            i = j;
            continue;
        }
        // Read to the matching ')'.
        std::size_t depth = 0;
        bool inQuotes = false;
        std::size_t at = k;
        for (; at < text.size(); ++at) {
            const char c = text[at];
            if (c == '"') {
                inQuotes = !inQuotes;
            } else if (!inQuotes && c == '(') {
                ++depth;
            } else if (!inQuotes && c == ')') {
                --depth;
                if (depth == 0) {
                    break;
                }
            }
        }
        if (at >= text.size()) {
            break; // Unbalanced; the caller reports it via an empty model.
        }
        calls.push_back(CMakeCall{std::string{text.substr(i, j - i)},
                                  std::string{text.substr(k + 1, at - k - 1)}});
        i = at + 1;
    }
    return calls;
}

std::vector<std::string> cmakeArguments(std::string_view argumentText) {
    std::vector<std::string> arguments;
    std::size_t i = 0;
    while (i < argumentText.size()) {
        while (i < argumentText.size() && isSpace(argumentText[i])) {
            ++i;
        }
        if (i >= argumentText.size()) {
            break;
        }
        std::string token;
        bool inQuotes = false;
        for (; i < argumentText.size(); ++i) {
            const char c = argumentText[i];
            if (c == '"') {
                inQuotes = !inQuotes;
                continue;
            }
            if (!inQuotes && isSpace(c)) {
                break;
            }
            token.push_back(c);
        }
        if (!token.empty()) {
            arguments.push_back(std::move(token));
        }
    }
    return arguments;
}

bool isSourceFileName(std::string_view token) {
    static const std::array<std::string_view, 6> extensions{".cpp", ".cc", ".cxx", ".c", ".hpp",
                                                            ".h"};
    return std::any_of(extensions.begin(), extensions.end(),
                       [token](const std::string_view extension) {
                           return token.size() > extension.size() &&
                                  token.compare(token.size() - extension.size(), extension.size(),
                                                extension) == 0;
                       });
}

/// A source argument as a repository-relative path. `${PROJECT_SOURCE_DIR}/x`
/// is repository-rooted; anything else is relative to `tests/`, which is the
/// directory `tests/CMakeLists.txt` speaks in.
std::string normalizeSource(std::string_view token, std::string& error) {
    constexpr std::string_view kProjectRoot = "${PROJECT_SOURCE_DIR}/";
    if (token.rfind(kProjectRoot, 0) == 0) {
        const std::string_view rest = token.substr(kProjectRoot.size());
        if (rest.find('$') != std::string_view::npos) {
            error = "unresolvable source argument: " + std::string{token};
        }
        return std::string{rest};
    }
    if (token.find('$') != std::string_view::npos) {
        error = "unresolvable source argument: " + std::string{token};
        return std::string{token};
    }
    return "tests/" + std::string{token};
}

CMakeModel parseTestsCMake(std::string_view cmakeText) {
    static const std::array<std::string_view, 5> scopeKeywords{"PRIVATE", "PUBLIC", "INTERFACE",
                                                               "EXCLUDE_FROM_ALL", "WIN32"};
    CMakeModel model;
    const std::string stripped = stripCMakeComments(cmakeText);
    const std::vector<CMakeCall> calls = cmakeCalls(stripped);
    if (calls.empty()) {
        model.error = "no CMake command was parsed out of tests/CMakeLists.txt";
        return model;
    }

    const auto isScopeKeyword = [](const std::string& token) {
        return std::find(scopeKeywords.begin(), scopeKeywords.end(), token) != scopeKeywords.end();
    };

    for (const CMakeCall& call : calls) {
        const std::vector<std::string> arguments = cmakeArguments(call.arguments);
        if (arguments.empty()) {
            continue;
        }
        const std::string& first = arguments.front();

        if (call.name == "add_executable") {
            if (first.find('$') != std::string::npos) {
                model.error = "unresolvable target name: " + first;
                continue;
            }
            TargetInfo& target = model.targets[first];
            for (std::size_t i = 1; i < arguments.size(); ++i) {
                if (isScopeKeyword(arguments[i]) || !isSourceFileName(arguments[i])) {
                    continue;
                }
                target.sources.push_back(normalizeSource(arguments[i], model.error));
            }
        } else if (call.name == "target_sources") {
            const auto found = model.targets.find(first);
            if (found == model.targets.end()) {
                continue; // Not one of ours (or declared elsewhere).
            }
            for (std::size_t i = 1; i < arguments.size(); ++i) {
                if (isScopeKeyword(arguments[i]) || !isSourceFileName(arguments[i])) {
                    continue;
                }
                found->second.sources.push_back(normalizeSource(arguments[i], model.error));
            }
        } else if (call.name == "target_link_libraries") {
            const auto found = model.targets.find(first);
            if (found == model.targets.end()) {
                continue;
            }
            for (std::size_t i = 1; i < arguments.size(); ++i) {
                if (arguments[i].rfind("Palmier::", 0) == 0) {
                    found->second.palmierLibraries.push_back(arguments[i]);
                }
            }
        } else if (call.name == "palmier_register_test") {
            if (first.find('$') != std::string::npos) {
                continue; // The helper's own definition, `gtest_discover_tests(${target})`.
            }
            const auto found = model.targets.find(first);
            if (found == model.targets.end()) {
                model.registrationsWithoutTarget.push_back(first);
            } else {
                found->second.registeredWithCTest = true;
            }
        }
    }
    return model;
}

// ===========================================================================
// Per-source facts
// ===========================================================================

struct SourceFacts {
    bool                     declaresTestCases{false};
    bool                     includesProjectHeader{false};
    std::set<std::string>    componentReferences{};
};

/// True when a line (leading whitespace aside) opens a GoogleTest or RapidCheck
/// test case.
bool lineDeclaresTestCase(std::string_view line) {
    static const std::array<std::string_view, 8> macros{
        "TEST(",     "TEST_F(",   "TEST_P(",           "TYPED_TEST(",
        "TYPED_TEST_P(", "RC_GTEST_PROP(", "RC_GTEST_FIXTURE_PROP(", "INSTANTIATE_TEST_SUITE_P("};
    std::size_t i = 0;
    while (i < line.size() && isSpace(line[i])) {
        ++i;
    }
    const std::string_view rest = line.substr(i);
    return std::any_of(macros.begin(), macros.end(), [rest](const std::string_view macro) {
        return rest.rfind(macro, 0) == 0;
    });
}

/// `headerExists` answers "does this quoted include resolve to a header under
/// `src/`?" — injected so the unit tests can drive the analysis without a tree.
SourceFacts analyzeSource(std::string_view content,
                          const std::function<bool(const std::string&)>& headerExists,
                          const std::set<std::string>& components) {
    SourceFacts facts;
    const std::string code = stripCxxComments(content);
    const std::vector<std::string_view> lines = splitLines(code);

    for (const std::string_view line : lines) {
        if (lineDeclaresTestCase(line)) {
            facts.declaresTestCases = true;
            break;
        }
    }

    // Quoted includes that resolve under `src/`. Only a LINE that begins with
    // `#include` counts, and the quoted path may not leave that line: a test
    // fixture in this very suite passes the text `"#include <vector>\n..."` to a
    // checker as data, and treating that as a directive would make the scan read a
    // multi-line "path".
    for (const std::string_view line : lines) {
        std::size_t i = 0;
        while (i < line.size() && isSpace(line[i])) {
            ++i;
        }
        if (line.substr(i).rfind("#include", 0) != 0) {
            continue;
        }
        const std::size_t open = line.find('"', i);
        if (open == std::string_view::npos) {
            continue; // An angle-bracket include: a third-party or standard header.
        }
        const std::size_t close = line.find('"', open + 1);
        if (close == std::string_view::npos) {
            continue;
        }
        if (headerExists(std::string{line.substr(open + 1, close - open - 1)})) {
            facts.includesProjectHeader = true;
        }
    }

    for (const std::string& component : components) {
        if (referencesWord(code, component)) {
            facts.componentReferences.insert(component);
        }
    }
    return facts;
}

// ===========================================================================
// Defects
// ===========================================================================

enum class DefectKind {
    RegisteredTargetIsNotDefined,
    TargetCompilesNoProductCode,
    TargetReferencesNoNamedComponent,
    PlaceholderTestSource,
    TestSourceIsNeverCompiled,
    TestSourceIsNeverRegistered
};

std::string describe(DefectKind kind) {
    switch (kind) {
    case DefectKind::RegisteredTargetIsNotDefined:
        return "registered with CTest but no such target is defined";
    case DefectKind::TargetCompilesNoProductCode:
        return "registered target compiles no code of this product";
    case DefectKind::TargetReferencesNoNamedComponent:
        return "registered target references no component named in the documents";
    case DefectKind::PlaceholderTestSource:
        return "placeholder test source";
    case DefectKind::TestSourceIsNeverCompiled:
        return "test source no target compiles";
    case DefectKind::TestSourceIsNeverRegistered:
        return "test source whose cases CTest never registers";
    }
    return "unknown defect";
}

struct Defect {
    DefectKind  kind{DefectKind::PlaceholderTestSource};
    std::string subject; ///< A target name or a repository-relative source path.
    std::string detail;
};

/// A subject of Property 80: one registered target, or one test source. Carrying
/// them in one list is what lets the property quantify over exactly the domain
/// design.md names ("all test source files in tests/ and all targets registered
/// with CTest").
struct Subject {
    std::string         name;
    bool                isTarget{false};
    std::vector<Defect> defects{};
};

struct SuiteModel {
    CMakeModel                         cmake;
    std::map<std::string, SourceFacts> testSources; ///< Keyed "tests/...".
};

std::vector<Defect> evaluate(const SuiteModel& model) {
    std::vector<Defect> defects;

    for (const std::string& name : model.cmake.registrationsWithoutTarget) {
        defects.push_back({DefectKind::RegisteredTargetIsNotDefined, name,
                           "palmier_register_test(" + name + ") names no add_executable target"});
    }

    for (const auto& [name, target] : model.cmake.targets) {
        if (!target.registeredWithCTest) {
            continue;
        }
        if (!target.compilesProductCode()) {
            std::ostringstream detail;
            detail << "links no Palmier:: library other than Palmier::test_support and compiles "
                      "no source under src/ (linked:";
            for (const std::string& library : target.palmierLibraries) {
                detail << ' ' << library;
            }
            detail << ')';
            defects.push_back({DefectKind::TargetCompilesNoProductCode, name, detail.str()});
        }

        std::set<std::string> referenced;
        for (const std::string& source : target.testSources()) {
            const auto found = model.testSources.find(source);
            if (found == model.testSources.end()) {
                continue;
            }
            referenced.insert(found->second.componentReferences.begin(),
                              found->second.componentReferences.end());
        }
        if (referenced.empty()) {
            defects.push_back({DefectKind::TargetReferencesNoNamedComponent, name,
                               "none of its " + std::to_string(target.testSources().size()) +
                                   " test source(s) references a documented component"});
        }
    }

    for (const auto& [path, sourceFacts] : model.testSources) {
        std::vector<std::string> owners;
        bool anyOwnerHasProductCode = false;
        bool anyOwnerRegistered = false;
        for (const auto& [name, target] : model.cmake.targets) {
            if (std::find(target.sources.begin(), target.sources.end(), path) ==
                target.sources.end()) {
                continue;
            }
            owners.push_back(name);
            anyOwnerHasProductCode = anyOwnerHasProductCode || target.compilesProductCode();
            anyOwnerRegistered = anyOwnerRegistered || target.registeredWithCTest;
        }

        if (owners.empty()) {
            defects.push_back({DefectKind::TestSourceIsNeverCompiled, path,
                               "no target in tests/CMakeLists.txt compiles it"});
            continue;
        }
        if (!sourceFacts.declaresTestCases) {
            continue; // A support source (a helper, a fixture generator's main).
        }
        if (!anyOwnerRegistered) {
            defects.push_back({DefectKind::TestSourceIsNeverRegistered, path,
                               "declares test cases, but no target that compiles it is "
                               "registered with CTest"});
        }
        // design.md's placeholder pattern, both conjuncts: asserts nothing about
        // this product AND is in a binary that contains none of it.
        if (sourceFacts.componentReferences.empty() && !sourceFacts.includesProjectHeader &&
            !anyOwnerHasProductCode) {
            defects.push_back({DefectKind::PlaceholderTestSource, path,
                               "declares test cases, references no component of this product in "
                               "code, includes no header from src/, and every target that "
                               "compiles it links no product code"});
        }
    }

    std::sort(defects.begin(), defects.end(), [](const Defect& a, const Defect& b) {
        return std::tie(a.subject, a.detail) < std::tie(b.subject, b.detail);
    });
    return defects;
}

std::vector<Subject> subjectsOf(const SuiteModel& model, const std::vector<Defect>& defects) {
    std::map<std::string, Subject> byName;
    for (const auto& [name, target] : model.cmake.targets) {
        if (target.registeredWithCTest) {
            byName.emplace(name, Subject{name, true, {}});
        }
    }
    for (const auto& [path, sourceFacts] : model.testSources) {
        (void)sourceFacts;
        byName.emplace(path, Subject{path, false, {}});
    }
    // A defect whose subject is neither (a registration naming no target) still
    // needs a subject to hang off, so the property can draw and report it.
    for (const Defect& defect : defects) {
        Subject& subject =
            byName.emplace(defect.subject, Subject{defect.subject, true, {}}).first->second;
        subject.defects.push_back(defect);
    }
    std::vector<Subject> subjects;
    subjects.reserve(byName.size());
    for (auto& [name, subject] : byName) {
        (void)name;
        subjects.push_back(std::move(subject));
    }
    return subjects;
}

// ===========================================================================
// The scan of the real tree — performed once per process
// ===========================================================================

struct SuiteScan {
    SuiteModel            model;
    std::vector<Defect>   defects;
    std::vector<Subject>  subjects;
    std::set<std::string> components;
    std::size_t           headerCount{0};
    std::size_t           documentCount{0};
    std::string           enumerationError;
};

std::vector<std::filesystem::path> documentPaths() {
    std::vector<std::filesystem::path> documents;
    const std::filesystem::path docs{PALMIER_DOCS_DIR};
    if (std::filesystem::is_directory(docs)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator{docs}) {
            if (entry.is_regular_file() && entry.path().extension() == ".md") {
                documents.push_back(entry.path());
            }
        }
    }
    const std::filesystem::path specs{PALMIER_SPEC_DIR};
    if (std::filesystem::is_directory(specs)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator{specs}) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (name == "requirements.md" || name == "design.md") {
                documents.push_back(entry.path());
            }
        }
    }
    std::sort(documents.begin(), documents.end());
    return documents;
}

SuiteScan performScan() {
    SuiteScan scan;
    const std::filesystem::path root{PALMIER_SOURCE_DIR};
    const std::filesystem::path sourceRoot = root / "src";
    const std::filesystem::path testRoot = root / "tests";

    if (!std::filesystem::is_directory(sourceRoot) || !std::filesystem::is_directory(testRoot)) {
        scan.enumerationError = "src/ or tests/ is missing under " + root.string();
        return scan;
    }

    // --- the component set -------------------------------------------------
    std::set<std::string> declared;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator{sourceRoot}) {
        if (!entry.is_regular_file() || entry.path().extension() != ".hpp") {
            continue;
        }
        ++scan.headerCount;
        const std::set<std::string> names = declaredNames(readFile(entry.path()));
        declared.insert(names.begin(), names.end());
    }
    std::set<std::string> documented;
    for (const std::filesystem::path& document : documentPaths()) {
        ++scan.documentCount;
        const std::set<std::string> words = documentedWords(readFile(document));
        documented.insert(words.begin(), words.end());
    }
    if (scan.headerCount == 0 || scan.documentCount == 0) {
        scan.enumerationError = "no headers under src/ or no documents to read";
        return scan;
    }
    scan.components = componentNames(declared, documented);

    // --- the build model ---------------------------------------------------
    const std::filesystem::path cmakeFile = testRoot / "CMakeLists.txt";
    if (!std::filesystem::is_regular_file(cmakeFile)) {
        scan.enumerationError = "tests/CMakeLists.txt is missing";
        return scan;
    }
    scan.model.cmake = parseTestsCMake(readFile(cmakeFile));
    if (!scan.model.cmake.error.empty()) {
        scan.enumerationError = "tests/CMakeLists.txt: " + scan.model.cmake.error;
        return scan;
    }

    // Every source a target names must exist: a stale entry would otherwise make
    // the property quantify over a file that is not there.
    for (const auto& [name, target] : scan.model.cmake.targets) {
        for (const std::string& source : target.sources) {
            if (!std::filesystem::is_regular_file(root / source)) {
                scan.enumerationError =
                    "target " + name + " names a source that does not exist: " + source;
                return scan;
            }
        }
    }

    // --- the test sources --------------------------------------------------
    const auto headerExists = [&sourceRoot](const std::string& include) {
        return std::filesystem::is_regular_file(sourceRoot / include);
    };
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator{testRoot}) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
            continue;
        }
        const std::string relative =
            "tests/" + std::filesystem::relative(entry.path(), testRoot).generic_string();
        scan.model.testSources.emplace(
            relative, analyzeSource(readFile(entry.path()), headerExists, scan.components));
    }
    if (scan.model.testSources.empty()) {
        scan.enumerationError = "no test sources were found under " + testRoot.string();
        return scan;
    }

    scan.defects = evaluate(scan.model);
    scan.subjects = subjectsOf(scan.model, scan.defects);
    return scan;
}

const SuiteScan& suiteScan() {
    static const SuiteScan scan = performScan();
    return scan;
}

std::string report(const std::vector<Defect>& defects) {
    std::ostringstream out;
    for (const Defect& defect : defects) {
        out << defect.subject << " \u2014 " << describe(defect.kind) << ": " << defect.detail
            << '\n';
    }
    return out.str();
}

std::size_t registeredTargetCount(const SuiteScan& scan) {
    return static_cast<std::size_t>(
        std::count_if(scan.model.cmake.targets.begin(), scan.model.cmake.targets.end(),
                      [](const auto& entry) { return entry.second.registeredWithCTest; }));
}

} // namespace

// ===========================================================================
// Property 80
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 80: Every registered test
// asserts a named component and none is a placeholder — for all test source files
// in tests/ and all targets registered with CTest, no source matches the
// placeholder pattern (a test whose body asserts only a tautology over generated
// values and which sits in a binary linking no Palmier:: product library), and
// every registered target compiles product code and references at least one
// component named in this repository's documents.
// Validates: Requirements 15.6
RC_GTEST_PROP(SuiteHygieneProperties, EveryRegisteredTestNamesAComponentAndNoneIsAPlaceholder, ()) {
    const SuiteScan& scan = suiteScan();

    // A domain that could not be enumerated must fail loudly: a property over an
    // empty domain is vacuously true, which is the failure mode this guards.
    RC_ASSERT(scan.enumerationError.empty());
    RC_ASSERT(registeredTargetCount(scan) >= 50u);
    RC_ASSERT(scan.model.testSources.size() >= 80u);
    RC_ASSERT(scan.components.size() >= 50u);

    // The universally quantified variable is the subject — a registered target or
    // a test source. Drawing one gives RapidCheck something to shrink to and to
    // name on failure.
    const auto index = *rc::gen::inRange<std::size_t>(0, scan.subjects.size());
    const Subject& drawn = scan.subjects[index];
    if (!drawn.defects.empty()) {
        RC_FAIL(report(drawn.defects));
    }

    // The scan is cached, so re-asserting the whole suite costs nothing and
    // guarantees every subject is covered in every case, rather than only the
    // subjects sampling happened to draw.
    if (!scan.defects.empty()) {
        RC_FAIL(report(scan.defects));
    }
}

// ===========================================================================
// Non-vacuity: the checker is proven able to fail
// ===========================================================================

namespace {

/// The essential shape of the file this task deleted,
/// `tests/palmier_placeholder_property_test.cpp`: a plausible comment block, two
/// tautologies over generated values, one arithmetic unit test, no product header
/// and no product component. Kept here so the rule that forbade it is pinned to
/// the artefact it was written for — if a future edit made the checker blind to
/// this shape, this case fails rather than the tree silently going quiet.
constexpr std::string_view kDeletedPlaceholderSource = R"CPP(
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Placeholder property-based test. Demonstrates the GoogleTest + RapidCheck
// wiring, the TAG FORMAT and the >= 100-iteration configuration. It does NOT
// test any domain behaviour. Mentions TimelineEngine and PreviewController in
// prose only, the way a plausible placeholder would.
#include <algorithm>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
namespace {
RC_GTEST_PROP(PalmierPlaceholderProperties, ReverseTwiceIsIdentity,
              (const std::vector<int> &xs)) {
    std::vector<int> roundTrip = xs;
    std::reverse(roundTrip.begin(), roundTrip.end());
    std::reverse(roundTrip.begin(), roundTrip.end());
    RC_ASSERT(roundTrip == xs);
}
RC_GTEST_PROP(PalmierPlaceholderProperties, ConcatenationLengthIsAdditive,
              (const std::string &a, const std::string &b)) {
    RC_ASSERT((a + b).size() == a.size() + b.size());
}
TEST(PalmierPlaceholderUnit, FrameworksAreWired) {
    EXPECT_EQ(1 + 1, 2);
}
}  // namespace
)CPP";

/// A genuine test source, for contrast: it includes a product header and asserts
/// on a named component.
constexpr std::string_view kRealSource = R"CPP(
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/TimelineEngine.hpp"
#include <gtest/gtest.h>
TEST(TimelineEngineTest, RejectsAnOverlappingClip) {
    palmier::core::TimelineEngine engine;
    EXPECT_FALSE(engine.apply(nullptr).isOk());
}
)CPP";

const std::set<std::string>& fixtureComponents() {
    static const std::set<std::string> components{"TimelineEngine", "PreviewController",
                                                  "ExportCoordinator",
                                                  "checkPlatformCompatibility"};
    return components;
}

/// `core/TimelineEngine.hpp` resolves; nothing else does.
bool fixtureHeaderExists(const std::string& include) {
    return include == "core/TimelineEngine.hpp";
}

SourceFacts facts(std::string_view content) {
    return analyzeSource(content, fixtureHeaderExists, fixtureComponents());
}

bool hasKind(const std::vector<Defect>& defects, DefectKind kind) {
    return std::any_of(defects.begin(), defects.end(),
                       [kind](const Defect& defect) { return defect.kind == kind; });
}

std::vector<Defect> defectsFor(std::string_view cmakeText,
                               const std::map<std::string, std::string_view>& sources) {
    SuiteModel model;
    model.cmake = parseTestsCMake(cmakeText);
    for (const auto& [path, content] : sources) {
        model.testSources.emplace(path, facts(content));
    }
    return evaluate(model);
}

} // namespace

TEST(SuiteHygieneChecker, TheRealSuiteWasActuallyEnumeratedAndIsClean) {
    const SuiteScan& scan = suiteScan();
    ASSERT_EQ(scan.enumerationError, "") << "the suite could not be enumerated";
    EXPECT_GE(registeredTargetCount(scan), 50u) << "suspiciously few registered targets";
    EXPECT_GE(scan.model.testSources.size(), 80u) << "suspiciously few test sources";
    EXPECT_GE(scan.components.size(), 50u) << "suspiciously few component names discovered";
    EXPECT_TRUE(scan.defects.empty()) << '\n' << report(scan.defects);

    std::cout << "registered targets: " << registeredTargetCount(scan)
              << ", test sources: " << scan.model.testSources.size()
              << ", documented components: " << scan.components.size() << " (from "
              << scan.headerCount << " headers and " << scan.documentCount << " documents)\n";
}

TEST(SuiteHygieneChecker, EveryRegisteredTargetIsProvenToNameItsOwnComponents) {
    // The "names a component" clause is only good news if it is DISCRIMINATING —
    // if every target had matched some universally-present name, the clause would
    // be decoration. So: every registered target must reference a component, and
    // across the suite the referenced names must be many and varied rather than
    // one name matching everything.
    const SuiteScan& scan = suiteScan();
    ASSERT_EQ(scan.enumerationError, "");

    std::map<std::string, std::size_t> referenceCounts;
    for (const auto& [path, sourceFacts] : scan.model.testSources) {
        (void)path;
        for (const std::string& component : sourceFacts.componentReferences) {
            ++referenceCounts[component];
        }
    }
    EXPECT_GE(referenceCounts.size(), 50u)
        << "the suite as a whole references only " << referenceCounts.size()
        << " distinct components";

    const std::size_t sources = scan.model.testSources.size();
    for (const auto& [component, count] : referenceCounts) {
        EXPECT_LT(count, sources)
            << '"' << component << "\" is referenced by every test source, so referencing it "
            << "cannot evidence anything";
    }
}

TEST(SuiteHygieneChecker, ThisFileIsSubjectToItsOwnRule) {
    // This file declares tests and is compiled into `palmier_docs_tests`. It must
    // therefore appear in the enumerated domain — an exemption for the checker
    // itself is exactly the hole this case forbids.
    const SuiteScan& scan = suiteScan();
    ASSERT_EQ(scan.enumerationError, "");
    const auto found = scan.model.testSources.find("tests/docs/suite_hygiene_property_test.cpp");
    ASSERT_NE(found, scan.model.testSources.end())
        << "this file is not in the enumerated test-source set";
    EXPECT_TRUE(found->second.declaresTestCases);

    const auto target = scan.model.cmake.targets.find("palmier_docs_tests");
    ASSERT_NE(target, scan.model.cmake.targets.end());
    EXPECT_TRUE(target->second.registeredWithCTest);
    EXPECT_TRUE(target->second.compilesProductCode());
    const std::vector<std::string>& sources = target->second.sources;
    EXPECT_NE(std::find(sources.begin(), sources.end(),
                        "tests/docs/suite_hygiene_property_test.cpp"),
              sources.end());
}

TEST(SuiteHygieneChecker, TheDeletedPlaceholderShapeIsFlaggedInATestSupportOnlyTarget) {
    constexpr std::string_view cmakeText = R"CMAKE(
add_executable(palmier_placeholder_tests
    palmier_placeholder_property_test.cpp)
target_link_libraries(palmier_placeholder_tests PRIVATE Palmier::test_support)
palmier_register_test(palmier_placeholder_tests)
)CMAKE";
    const std::vector<Defect> defects =
        defectsFor(cmakeText,
                   {{"tests/palmier_placeholder_property_test.cpp", kDeletedPlaceholderSource}});

    EXPECT_TRUE(hasKind(defects, DefectKind::PlaceholderTestSource)) << report(defects);
    EXPECT_TRUE(hasKind(defects, DefectKind::TargetCompilesNoProductCode)) << report(defects);
    EXPECT_TRUE(hasKind(defects, DefectKind::TargetReferencesNoNamedComponent)) << report(defects);
}

TEST(SuiteHygieneChecker, AGenuineTestInAProductLinkingTargetIsClean) {
    constexpr std::string_view cmakeText = R"CMAKE(
add_executable(palmier_core_tests
    core/timeline_engine_test.cpp)
target_link_libraries(palmier_core_tests PRIVATE
    Palmier::core
    Palmier::test_support)
palmier_register_test(palmier_core_tests)
)CMAKE";
    const std::vector<Defect> defects =
        defectsFor(cmakeText, {{"tests/core/timeline_engine_test.cpp", kRealSource}});
    EXPECT_TRUE(defects.empty()) << report(defects);
}

TEST(SuiteHygieneChecker, ATautologySourceInAProductTargetStillFailsTheNamedComponentClause) {
    // design.md's placeholder pattern is a CONJUNCTION, so dropping a tautology
    // source into a target that links product code is not "a placeholder" — but it
    // is still a target asserting nothing about a named component, which is the
    // other half of Requirement 15.6. This case pins that the two clauses together
    // leave no way in.
    constexpr std::string_view cmakeText = R"CMAKE(
add_executable(palmier_smuggled_tests
    smuggled_property_test.cpp)
target_link_libraries(palmier_smuggled_tests PRIVATE
    Palmier::core
    Palmier::test_support)
palmier_register_test(palmier_smuggled_tests)
)CMAKE";
    const std::vector<Defect> defects =
        defectsFor(cmakeText, {{"tests/smuggled_property_test.cpp", kDeletedPlaceholderSource}});
    EXPECT_FALSE(hasKind(defects, DefectKind::PlaceholderTestSource)) << report(defects);
    EXPECT_TRUE(hasKind(defects, DefectKind::TargetReferencesNoNamedComponent)) << report(defects);
}

TEST(SuiteHygieneChecker, ProductCodeCompiledInDirectlyCountsAsProductCode) {
    // The tree's established pattern: no product library, but the specific service
    // source compiled into the binary. That must satisfy the clause, or two thirds
    // of this suite's targets would be false positives.
    constexpr std::string_view cmakeText = R"CMAKE(
add_executable(palmier_services_tests
    services/project_store_test.cpp
    "${PROJECT_SOURCE_DIR}/src/services/ProjectStore.cpp")
target_link_libraries(palmier_services_tests PRIVATE Palmier::test_support)
palmier_register_test(palmier_services_tests)
)CMAKE";
    const CMakeModel model = parseTestsCMake(cmakeText);
    ASSERT_EQ(model.error, "");
    const auto found = model.targets.find("palmier_services_tests");
    ASSERT_NE(found, model.targets.end());
    EXPECT_TRUE(found->second.compilesProductCode());
    EXPECT_NE(std::find(found->second.sources.begin(), found->second.sources.end(),
                        "src/services/ProjectStore.cpp"),
              found->second.sources.end());
}

TEST(SuiteHygieneChecker, DetectsARegistrationOfANonExistentTarget) {
    constexpr std::string_view cmakeText = R"CMAKE(
palmier_register_test(palmier_ghost_tests)
)CMAKE";
    const std::vector<Defect> defects = defectsFor(cmakeText, {});
    EXPECT_TRUE(hasKind(defects, DefectKind::RegisteredTargetIsNotDefined)) << report(defects);
}

TEST(SuiteHygieneChecker, DetectsATestSourceNoTargetCompiles) {
    constexpr std::string_view cmakeText = R"CMAKE(
add_executable(palmier_core_tests
    core/timeline_engine_test.cpp)
target_link_libraries(palmier_core_tests PRIVATE Palmier::core)
palmier_register_test(palmier_core_tests)
)CMAKE";
    const std::vector<Defect> defects =
        defectsFor(cmakeText, {{"tests/core/timeline_engine_test.cpp", kRealSource},
                               {"tests/core/forgotten_test.cpp", kRealSource}});
    EXPECT_TRUE(hasKind(defects, DefectKind::TestSourceIsNeverCompiled)) << report(defects);
}

TEST(SuiteHygieneChecker, DetectsTestCasesThatCTestNeverRegisters) {
    // Compiled, but into a target nobody registered: the cases never run, so they
    // assert nothing — the same defect as a placeholder in a different guise.
    constexpr std::string_view cmakeText = R"CMAKE(
add_executable(palmier_unregistered_tests
    core/timeline_engine_test.cpp)
target_link_libraries(palmier_unregistered_tests PRIVATE Palmier::core)
)CMAKE";
    const std::vector<Defect> defects =
        defectsFor(cmakeText, {{"tests/core/timeline_engine_test.cpp", kRealSource}});
    EXPECT_TRUE(hasKind(defects, DefectKind::TestSourceIsNeverRegistered)) << report(defects);
}

TEST(SuiteHygieneChecker, ASupportSourceWithNoTestCasesIsNotADefect) {
    // `support/SyntheticMedia.cpp` and `e2e/fixture_generator_main.cpp` declare no
    // test cases; neither the placeholder rule nor the registration rule applies.
    constexpr std::string_view cmakeText = R"CMAKE(
add_executable(palmier_e2e_fixture_generator
    e2e/fixture_generator_main.cpp)
target_link_libraries(palmier_e2e_fixture_generator PRIVATE Palmier::media)
)CMAKE";
    constexpr std::string_view generatorSource = R"CPP(
// SPDX-License-Identifier: GPL-3.0-or-later
int main(int argc, char** argv) { return argc > 1 ? 0 : 1; }
)CPP";
    const std::vector<Defect> defects =
        defectsFor(cmakeText, {{"tests/e2e/fixture_generator_main.cpp", generatorSource}});
    EXPECT_TRUE(defects.empty()) << report(defects);
}

TEST(SuiteHygieneChecker, ComponentReferencesAreCountedInCodeAndNotInProse) {
    // The crux of reading 2 in the file header: the deleted placeholder's comment
    // block names components, and that must not count.
    const SourceFacts placeholder = facts(kDeletedPlaceholderSource);
    EXPECT_TRUE(placeholder.declaresTestCases);
    EXPECT_FALSE(placeholder.includesProjectHeader);
    EXPECT_TRUE(placeholder.componentReferences.empty())
        << "prose mentions were counted as references";

    const SourceFacts real = facts(kRealSource);
    EXPECT_TRUE(real.declaresTestCases);
    EXPECT_TRUE(real.includesProjectHeader);
    EXPECT_EQ(real.componentReferences.count("TimelineEngine"), 1u);
}

TEST(SuiteHygieneChecker, EveryTestMacroFormIsRecognised) {
    // A macro form the scanner did not know would make every file using it look
    // like a support source, i.e. exempt from the placeholder rule.
    EXPECT_TRUE(lineDeclaresTestCase("TEST(Suite, Case) {"));
    EXPECT_TRUE(lineDeclaresTestCase("    TEST_F(Fixture, Case) {"));
    EXPECT_TRUE(lineDeclaresTestCase("TEST_P(Fixture, Case) {"));
    EXPECT_TRUE(lineDeclaresTestCase("TYPED_TEST(Fixture, Case) {"));
    EXPECT_TRUE(lineDeclaresTestCase("RC_GTEST_PROP(Suite, Property, ()) {"));
    EXPECT_TRUE(lineDeclaresTestCase("RC_GTEST_FIXTURE_PROP(Fixture, Property, ()) {"));
    EXPECT_FALSE(lineDeclaresTestCase("// TEST(Suite, Case) in a comment"));
    EXPECT_FALSE(lineDeclaresTestCase("void helperForTESTs() {"));
}

TEST(SuiteHygieneChecker, TheCMakeParserReadsMultiLineCallsCommentsAndQuotedPaths) {
    constexpr std::string_view cmakeText = R"CMAKE(
# add_executable(palmier_commented_out_tests commented.cpp)
add_executable(palmier_parsed_tests
    core/a_test.cpp   # a trailing comment
    "${PROJECT_SOURCE_DIR}/src/core/Thing.cpp")
target_sources(palmier_parsed_tests PRIVATE
    core/b_test.cpp)
target_link_libraries(palmier_parsed_tests PRIVATE
    Palmier::core
    Palmier::test_support
    ${CMAKE_DL_LIBS})
palmier_register_test(palmier_parsed_tests)
)CMAKE";
    const CMakeModel model = parseTestsCMake(cmakeText);
    ASSERT_EQ(model.error, "");
    EXPECT_EQ(model.targets.count("palmier_commented_out_tests"), 0u)
        << "a commented-out target was parsed";
    const auto found = model.targets.find("palmier_parsed_tests");
    ASSERT_NE(found, model.targets.end());
    EXPECT_TRUE(found->second.registeredWithCTest);
    EXPECT_EQ(found->second.sources,
              (std::vector<std::string>{"tests/core/a_test.cpp", "src/core/Thing.cpp",
                                        "tests/core/b_test.cpp"}));
    EXPECT_EQ(found->second.palmierLibraries,
              (std::vector<std::string>{"Palmier::core", "Palmier::test_support"}));
    EXPECT_EQ(found->second.testSources(),
              (std::vector<std::string>{"tests/core/a_test.cpp", "tests/core/b_test.cpp"}));
}

TEST(SuiteHygieneChecker, TheComponentSetIsTheIntersectionOfDeclaredAndDocumented) {
    constexpr std::string_view header = R"CPP(
// SPDX-License-Identifier: GPL-3.0-or-later
// Mentions UndocumentedInProseOnly in a comment, which does not declare it.
namespace palmier::ui {
class PreviewController {
public:
    void setAudioMasterClock();
};
struct NeverDocumented {};
[[nodiscard]] bool checkPlatformCompatibility();
} // namespace palmier::ui
)CPP";
    const std::set<std::string> declared = declaredNames(header);
    EXPECT_EQ(declared.count("PreviewController"), 1u);
    EXPECT_EQ(declared.count("setAudioMasterClock"), 1u);
    EXPECT_EQ(declared.count("checkPlatformCompatibility"), 1u);
    EXPECT_EQ(declared.count("UndocumentedInProseOnly"), 0u)
        << "a name from a comment was treated as declared";

    const std::set<std::string> documented = documentedWords(
        "The Playback_Engine is ui::PreviewController; the launch gate is "
        "app::checkPlatformCompatibility. Prose also names UndocumentedInProseOnly and size.");
    const std::set<std::string> components = componentNames(declared, documented);
    EXPECT_EQ(components.count("PreviewController"), 1u);
    EXPECT_EQ(components.count("checkPlatformCompatibility"), 1u);
    EXPECT_EQ(components.count("NeverDocumented"), 0u) << "an undocumented name became a component";
    EXPECT_EQ(components.count("setAudioMasterClock"), 0u)
        << "a declared-but-undocumented member became a component";
    EXPECT_EQ(components.count("UndocumentedInProseOnly"), 0u);
}

TEST(SuiteHygieneChecker, VocabularyThatIsNotAComponentNameIsRejected) {
    // Without this filter, `size`, `value` and `path` — declared by headers and
    // present in every document — would let any file satisfy the clause.
    EXPECT_FALSE(looksLikeComponentName("size"));
    EXPECT_FALSE(looksLikeComponentName("value"));
    EXPECT_FALSE(looksLikeComponentName("path"));
    EXPECT_FALSE(looksLikeComponentName("Frame"));
    EXPECT_FALSE(looksLikeComponentName("id"));
    EXPECT_TRUE(looksLikeComponentName("TimelineEngine"));
    EXPECT_TRUE(looksLikeComponentName("checkPlatformCompatibility"));
    EXPECT_TRUE(looksLikeComponentName("GpuContext"));
}
