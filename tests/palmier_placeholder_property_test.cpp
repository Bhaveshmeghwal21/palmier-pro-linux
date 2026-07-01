// SPDX-License-Identifier: GPL-3.0-or-later
//
// Placeholder property-based test.
//
// Purpose: demonstrate the GoogleTest + RapidCheck wiring, the project-wide
// property-test TAG FORMAT, and the >= 100-iteration configuration. It does
// NOT test any domain behaviour (no domain code exists yet) and is replaced by
// the real correctness-property tests (P1-P12) in later tasks.
//
// ---------------------------------------------------------------------------
// Property-test conventions every real property test in this project follows
// (see design.md "Testing Strategy"):
//
//   1. Written with RapidCheck via its GoogleTest bridge: RC_GTEST_PROP(...).
//   2. Runs a MINIMUM of 100 iterations. RapidCheck's default max_success is
//      100; the CTest wiring also exports RC_PARAMS=max_success=100 so the
//      floor is explicit. Raise it per-test with RC_PARAMS if desired.
//   3. Carries a tag naming the design property it exercises, in the form:
//
//          Feature: palmier-pro-linux, Property {n}: {text}
//
//      plus a "Validates: Requirements X.Y" reference. For real properties,
//      {n} is the design property number (P1-P12) and {text} is its statement.
//      This file uses Property 0 purely as a wiring placeholder.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

namespace {

// Feature: palmier-pro-linux, Property 0: reversing a sequence twice yields the
// original sequence (placeholder demonstrating RapidCheck + GoogleTest wiring).
// Validates: Requirements 1.6 (test-framework wiring only; no domain behaviour).
RC_GTEST_PROP(PalmierPlaceholderProperties,
              ReverseTwiceIsIdentity,
              (const std::vector<int> &xs)) {
    std::vector<int> roundTrip = xs;
    std::reverse(roundTrip.begin(), roundTrip.end());
    std::reverse(roundTrip.begin(), roundTrip.end());
    RC_ASSERT(roundTrip == xs);
}

// Feature: palmier-pro-linux, Property 0: concatenation length is additive
// (second placeholder confirming RapidCheck generates varied inputs).
// Validates: Requirements 1.6 (test-framework wiring only; no domain behaviour).
RC_GTEST_PROP(PalmierPlaceholderProperties,
              ConcatenationLengthIsAdditive,
              (const std::string &a, const std::string &b)) {
    RC_ASSERT((a + b).size() == a.size() + b.size());
}

// A plain GoogleTest case, confirming example-based unit tests build and run in
// the same binary as the RapidCheck properties.
TEST(PalmierPlaceholderUnit, FrameworksAreWired) {
    EXPECT_EQ(1 + 1, 2);
}

}  // namespace
