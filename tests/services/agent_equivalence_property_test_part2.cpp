// SPDX-License-Identifier: GPL-3.0-or-later
//
// Part 2 of the agent_equivalence_property_test cases, split out of agent_equivalence_property_test.cpp.
//
// Suites here: AgentEquivalenceCanonicalization, AgentEquivalenceProperties.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "agent_equivalence_property_test_support.hpp"

namespace palmier::services {
namespace {

using namespace palmier::services::
    testsupport_agentequivalencepropertytest;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 62: The agent path equals a
// direct tool invocation — for all utterances the offline interpreter maps
// successfully, executing the produced invocation through the agent yields the
// same project state and the same undo-history depth as invoking that tool
// directly with the same arguments; and for any invocation that fails, both paths
// leave the project byte-identical and the undo depth unchanged.
//
// Requirement 11.10: "FOR ALL utterances the offline interpreter maps
// successfully, executing the produced invocation SHALL yield the same project
// state and the same undo history depth as invoking that tool directly with the
// same arguments (equivalence property)."
//
// Requirement 11.5: "WHEN the Agent_Interpreter produces a tool invocation, THE
// Tool_Surface SHALL execute it through the same execution policy the
// MCP_Protocol_Handler uses, validating the arguments before execution, restoring
// the pre-invocation project state if execution fails, and recording exactly one
// undo entry when execution succeeds."
//
// The generator is the documented phrase table x starting contexts, with half the
// cases drawing a (phrase, context sabotage) pair that forces the invocation to
// fail, and an independently drawn history priming so `undo` and `redo` are seen
// over both an empty and a non-empty history.
//
// The comparison is over `serializeProject` — the canonical `.palmier` document
// text — plus both history depths, the session revision, the modified flag, the
// media library and the tool's own result payload, compared byte for byte after
// identifiers that did not exist before the invocation have been replaced by
// positional placeholders (see the file comment: `timeline.add_track` and
// `timeline.split_clip` mint a random UUID as part of succeeding, and no two
// executions can agree on its value).
//
// **Validates: Requirements 11.5, 11.10**
// ===========================================================================
RC_GTEST_PROP(AgentEquivalenceProperties, TheAgentPathEqualsADirectToolInvocation, ()) {
    const DrawnCase drawn = drawCase();
    const World& world = *drawn.world;
    // 0 = pristine history, 1 = one applied edit (so `undo` has something to do),
    // 2 = one applied edit then undone (so `redo` has something to do).
    const int priming = *rc::gen::element(0, 1, 2);

    // Two editors, seeded from ONE project value: identical identifier for
    // identifier, which is what makes a byte-level comparison meaningful.
    Stack agentStack(world);
    Stack directStack(world);
    primeHistory(agentStack, world, priming);
    primeHistory(directStack, world, priming);

    // They really did start identical.
    const std::string beforeAgent = agentStack.fingerprint();
    const std::string beforeDirect = directStack.fingerprint();
    RC_ASSERT(beforeAgent == beforeDirect);

    // The identifiers that existed BEFORE the invocation. Anything else in the
    // after-text was minted by the invocation.
    const std::vector<std::string> knownIds = uuidsIn(beforeAgent);

    // The one invocation both paths are given. Requirement 11.10 says "the same
    // arguments", so the arguments are produced once and handed to both sides; the
    // two interpreters are separately asserted to agree on them, which is why
    // sharing them is not hiding anything.
    const OfflineIntentInterpreter agentInterpreter = agentStack.interpreter();
    const Result<AgentIntent> interpreted = agentInterpreter.interpret(drawn.utterance);
    RC_PRE(interpreted.isOk());  // Property 62 quantifies over utterances that MAP
    const AgentIntent& intent = interpreted.value();

    const Result<AgentIntent> interpretedAgain =
        directStack.interpreter().interpret(drawn.utterance);
    RC_ASSERT(interpretedAgain.isOk());
    RC_ASSERT(interpretedAgain.value().toolName == intent.toolName);
    RC_ASSERT(interpretedAgain.value().arguments.dump() == intent.arguments.dump());

    // --- The agent path: interpreter -> orchestrator -> executor -----------
    AlwaysAuthorized gate;
    AgentOrchestrator agent(agentStack.executor(), gate, agentInterpreter.asInterpreter());
    const Result<AgentTurn> agentOutcome = agent.sendMessage(drawn.utterance);

    // --- The direct path: the same tool, the same arguments, the same policy the
    //     MCP protocol handler applies to a decoded `tools/call`.
    const Result<Json> directOutcome = directStack.executor().executeTool(
        intent.toolName, intent.arguments, InvocationSource::Mcp);

    RC_TAG(std::string(intent.toolName) + (directOutcome.isOk() ? " ok" : " failed"));

    // 1. The two paths agree on WHETHER the invocation succeeded.
    RC_ASSERT(agentOutcome.isOk() == directOutcome.isOk());

    // 2. And on why, when it failed.
    if (directOutcome.isError()) {
        RC_ASSERT(agentOutcome.error().code() == directOutcome.error().code());
        RC_ASSERT(agentOutcome.error().message() == directOutcome.error().message());
    } else {
        RC_ASSERT(agentOutcome.value().toolName == intent.toolName);
    }

    // 3. The undo and redo depths match exactly (Requirement 11.10's "the same
    //    undo history depth"), compared as numbers, not as text.
    RC_ASSERT(agentStack.undoDepth() == directStack.undoDepth());
    RC_ASSERT(agentStack.redoDepth() == directStack.redoDepth());

    // 4. And the project states are byte-identical, modulo identifiers that did
    //    not exist before the invocation. The tool's own result payload is folded
    //    into the compared text, so a divergence in what the two paths REPORTED
    //    fails here too.
    std::string afterAgent = agentStack.fingerprint();
    std::string afterDirect = directStack.fingerprint();
    if (agentOutcome.isOk()) {
        afterAgent += "\nresult=" + agentOutcome.value().result.dump();
        afterDirect += "\nresult=" + directOutcome.value().dump();
    }
    const std::string canonicalAgent = canonicalizeFreshIds(afterAgent, knownIds);
    const std::string canonicalDirect = canonicalizeFreshIds(afterDirect, knownIds);
    if (canonicalAgent != canonicalDirect) {
        RC_FAIL("the agent path and a direct invocation of '" + intent.toolName + "' with " +
                intent.arguments.dump() + " left different states.\nagent:\n" + canonicalAgent +
                "\ndirect:\n" + canonicalDirect);
    }

    // 5. Requirement 11.5's two halves, now that the paths are known to agree.
    if (directOutcome.isError()) {
        // "restoring the pre-invocation project state if execution fails" — each
        // side is byte-identical to ITS OWN pre-invocation fingerprint, so nothing
        // was left behind and no undo residue remains.
        RC_ASSERT(afterAgent == beforeAgent);
        RC_ASSERT(afterDirect == beforeDirect);
    } else if (intent.toolName != "timeline.read" && intent.toolName != "project.save" &&
               intent.toolName != "media.import" && intent.toolName != "timeline.export" &&
               intent.toolName != "edit.undo" && intent.toolName != "edit.redo") {
        // "recording exactly one undo entry when execution succeeds" — for the
        // tools that are edits. The excluded six are not edits: three are reads or
        // writes that apply no command, and undo/redo MOVE through the history
        // rather than extend it.
        const std::size_t beforeDepth = (priming == 0) ? 0u : (priming == 1 ? 1u : 0u);
        RC_ASSERT(agentStack.undoDepth() == beforeDepth + 1);
    }
}
// ---------------------------------------------------------------------------
// Unit tests: the specific examples the property quantifies over but does not pin
// by name, and the checks that keep the property from being vacuous.
// ---------------------------------------------------------------------------

// The property compares texts after canonicalizing fresh identifiers. If that
// canonicalization were too aggressive the whole property would be vacuous, so
// this pins exactly what it does and does not hide.
TEST(AgentEquivalenceCanonicalization, HidesFreshIdentifierValuesAndNothingElse) {
    const Uuid known = Uuid::generateV4();
    const Uuid freshA = Uuid::generateV4();
    const Uuid freshB = Uuid::generateV4();
    const std::vector<std::string> knownIds = {known.toString()};

    // A known id survives verbatim; a fresh one becomes a positional placeholder.
    EXPECT_EQ(canonicalizeFreshIds("track " + known.toString(), knownIds),
              "track " + known.toString());
    EXPECT_EQ(canonicalizeFreshIds("track " + freshA.toString(), knownIds), "track <fresh-uuid-0>");

    // Two different fresh ids get two different placeholders...
    EXPECT_EQ(canonicalizeFreshIds(freshA.toString() + " " + freshB.toString(), knownIds),
              "<fresh-uuid-0> <fresh-uuid-1>");
    // ...and the SAME fresh id repeated keeps one placeholder, so multiplicity and
    // cross-references are preserved.
    EXPECT_EQ(canonicalizeFreshIds(freshA.toString() + " " + freshA.toString(), knownIds),
              "<fresh-uuid-0> <fresh-uuid-0>");

    // Consistently relabelling two fresh ids IS invisible, and deliberately so:
    // which random value `Uuid::generateV4()` returned is not part of the project
    // state, so two paths that minted the same structure with different values are
    // equivalent.
    EXPECT_EQ(canonicalizeFreshIds(freshA.toString() + " x " + freshB.toString(), knownIds),
              canonicalizeFreshIds(freshB.toString() + " x " + freshA.toString(), knownIds));

    // What is NOT invisible is the pattern of repetition: a text in which the
    // first fresh id recurs does not canonicalize like one in which the second
    // does, so a path that cross-referenced the wrong object still fails.
    EXPECT_NE(canonicalizeFreshIds(
                  freshA.toString() + " " + freshB.toString() + " " + freshA.toString(), knownIds),
              canonicalizeFreshIds(
                  freshA.toString() + " " + freshB.toString() + " " + freshB.toString(), knownIds));

    // Nor is putting a fresh id where a pre-existing one belongs.
    EXPECT_NE(canonicalizeFreshIds("track " + known.toString(), knownIds),
              canonicalizeFreshIds("track " + freshA.toString(), knownIds));

    // A different NUMBER of fresh ids never canonicalizes alike.
    EXPECT_NE(canonicalizeFreshIds(freshA.toString(), knownIds),
              canonicalizeFreshIds(freshA.toString() + " " + freshB.toString(), knownIds));

    // Non-UUID text is untouched, including near-misses of the UUID shape.
    EXPECT_EQ(canonicalizeFreshIds("plain text, 1234-56, {}", knownIds), "plain text, 1234-56, {}");
}

}  // namespace
}  // namespace palmier::services
