// SPDX-License-Identifier: GPL-3.0-or-later
//
// The agent_equivalence_property_test cases that stayed in this file after it was split.
//
// Suites here: AgentEquivalence, AgentEquivalenceFingerprint.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "agent_equivalence_property_test_support.hpp"

namespace palmier::services {
namespace {

using namespace palmier::services::testsupport_agentequivalencepropertytest;  // NOLINT(google-build-using-namespace)

// The fingerprint is the property's whole notion of "project state". A change the
// fingerprint cannot see is a change the property cannot catch, so this proves it
// sees an ordinary edit, an undo, a media import and a history depth.
TEST(AgentEquivalenceFingerprint, ChangesWithEveryPartOfTheStateItClaimsToCover) {
    const std::unique_ptr<World> world = makeWorld(Sabotage::None);
    Stack                        stack(*world);

    const std::string initial = stack.fingerprint();

    Json muteArgs = Json::object();
    muteArgs.set("trackId", world->trackIds[0].toString());
    muteArgs.set("muted", true);
    ASSERT_TRUE(stack.executor().executeTool("timeline.set_track_muted", muteArgs).isOk());
    const std::string afterEdit = stack.fingerprint();
    EXPECT_NE(afterEdit, initial) << "an applied edit must move the fingerprint";

    ASSERT_TRUE(stack.executor().executeTool("edit.undo", Json::object()).isOk());
    const std::string afterUndo = stack.fingerprint();
    EXPECT_NE(afterUndo, afterEdit);
    // The project value is back, but the history depths are not, so the
    // fingerprint must still differ from the pristine one.
    EXPECT_NE(afterUndo, initial) << "the fingerprint must carry the undo/redo depths";

    Json importArgs = Json::object();
    importArgs.set("path", world->importPath.string());
    ASSERT_TRUE(stack.executor().executeTool("media.import", importArgs).isOk());
    EXPECT_NE(stack.fingerprint(), afterUndo)
        << "the fingerprint must carry the media library";
}
// Every documented phrase, deterministically, on its own pair of stacks: the
// property draws from the phrase table and over 100 cases will very likely reach
// all twelve, but "very likely" is not "always".
TEST(AgentEquivalence, EveryDocumentedPhraseAgreesAcrossTheTwoPaths) {
    const std::vector<PhrasePattern>& table = OfflineIntentInterpreter::patterns();
    ASSERT_EQ(table.size(), 12u);

    for (const PhrasePattern& pattern : table) {
        const std::unique_ptr<World> world = makeWorld(Sabotage::None);
        Stack                        agentStack(*world);
        Stack                        directStack(*world);

        std::string utterance;
        switch (pattern.match) {
            case PhraseMatch::Exact:
                utterance = pattern.canonicalUtterance();
                break;
            case PhraseMatch::Ordinal:
                utterance = pattern.canonicalUtterance(2);
                break;
            case PhraseMatch::Suffix:
                utterance = pattern.canonicalUtterance(
                    1, (pattern.toolName == "media.import" ? world->importPath
                                                           : world->exportPath)
                           .string());
                break;
        }

        const std::string              before = agentStack.fingerprint();
        const std::vector<std::string> knownIds = uuidsIn(before);
        ASSERT_EQ(before, directStack.fingerprint()) << utterance;

        const OfflineIntentInterpreter interpreter = agentStack.interpreter();
        const Result<AgentIntent>      intent = interpreter.interpret(utterance);
        ASSERT_TRUE(intent.isOk()) << utterance << ": " << intent.error().toString();

        AlwaysAuthorized  gate;
        AgentOrchestrator agent(agentStack.executor(), gate, interpreter.asInterpreter());
        const Result<AgentTurn> viaAgent = agent.sendMessage(utterance);
        const Result<Json>      viaDirect = directStack.executor().executeTool(
            intent.value().toolName, intent.value().arguments, InvocationSource::Mcp);

        ASSERT_TRUE(viaDirect.isOk())
            << utterance << ": " << viaDirect.error().toString();
        ASSERT_TRUE(viaAgent.isOk()) << utterance << ": " << viaAgent.error().toString();

        EXPECT_EQ(agentStack.undoDepth(), directStack.undoDepth()) << utterance;
        EXPECT_EQ(agentStack.redoDepth(), directStack.redoDepth()) << utterance;
        EXPECT_EQ(canonicalizeFreshIds(agentStack.fingerprint(), knownIds),
                  canonicalizeFreshIds(directStack.fingerprint(), knownIds))
            << utterance;
    }
}
// Both halves of Property 62 are reachable by construction: every sabotage mode
// really does make its phrase fail on BOTH paths, and really does leave both
// projects exactly as they were. Without this the failure arm could be silently
// empty and the property would only ever test the success arm.
TEST(AgentEquivalence, EverySabotageModeFailsBothPathsAndRollsBothBack) {
    for (const FailableTool& failable : failableTools()) {
        const std::unique_ptr<World> world = makeWorld(failable.sabotage);
        Stack                        agentStack(*world);
        Stack                        directStack(*world);

        std::vector<PhrasePattern> candidates;
        for (const PhrasePattern& pattern : OfflineIntentInterpreter::patterns()) {
            if (pattern.toolName == failable.toolName) candidates.push_back(pattern);
        }
        ASSERT_FALSE(candidates.empty()) << failable.toolName;
        const PhrasePattern& pattern = candidates.front();

        std::string utterance;
        switch (pattern.match) {
            case PhraseMatch::Exact:
                utterance = pattern.canonicalUtterance();
                break;
            case PhraseMatch::Ordinal:
                utterance = pattern.canonicalUtterance(1);
                break;
            case PhraseMatch::Suffix:
                utterance = pattern.canonicalUtterance(
                    1, (pattern.toolName == "media.import" ? world->importPath
                                                           : world->exportPath)
                           .string());
                break;
        }

        const std::string before = agentStack.fingerprint();
        ASSERT_EQ(before, directStack.fingerprint()) << utterance;

        const OfflineIntentInterpreter interpreter = agentStack.interpreter();
        const Result<AgentIntent>      intent = interpreter.interpret(utterance);
        ASSERT_TRUE(intent.isOk())
            << utterance << " (sabotage must fail at EXECUTION, not at "
               "interpretation): "
            << intent.error().toString();

        AlwaysAuthorized  gate;
        AgentOrchestrator agent(agentStack.executor(), gate, interpreter.asInterpreter());
        const Result<AgentTurn> viaAgent = agent.sendMessage(utterance);
        const Result<Json>      viaDirect = directStack.executor().executeTool(
            intent.value().toolName, intent.value().arguments, InvocationSource::Mcp);

        EXPECT_TRUE(viaAgent.isError()) << utterance;
        EXPECT_TRUE(viaDirect.isError()) << utterance;
        ASSERT_TRUE(viaAgent.isError() && viaDirect.isError()) << utterance;
        EXPECT_EQ(viaAgent.error().code(), viaDirect.error().code()) << utterance;

        // Requirement 11.5: the pre-invocation state is restored on both paths.
        EXPECT_EQ(agentStack.fingerprint(), before) << utterance;
        EXPECT_EQ(directStack.fingerprint(), before) << utterance;
    }
}
// Requirement 11.5 calls the agent path and the MCP path "the same execution
// policy". The `InvocationSource` tag is documented as affecting logging only;
// this pins that, because if it did affect the outcome the property's direct arm
// (which uses `InvocationSource::Mcp`) would not be a fair comparator.
TEST(AgentEquivalence, TheInvocationSourceTagDoesNotChangeTheOutcome) {
    const InvocationSource sources[] = {InvocationSource::Gui, InvocationSource::Mcp,
                                        InvocationSource::Agent};
    // ONE world for all three sources: a fresh world would seed different project
    // identifiers and the three texts would differ for a reason that has nothing to
    // do with the source tag.
    const std::unique_ptr<World> world = makeWorld(Sabotage::None);
    std::string                  reference;
    for (const InvocationSource source : sources) {
        Stack                          stack(*world);
        const std::vector<std::string> knownIds = uuidsIn(stack.fingerprint());

        Json arguments = Json::object();
        arguments.set("kind", "video");
        const Result<Json> executed =
            stack.executor().executeTool("timeline.add_track", arguments, source);
        ASSERT_TRUE(executed.isOk()) << invocationSourceName(source);

        const std::string canonical = canonicalizeFreshIds(stack.fingerprint(), knownIds);
        if (reference.empty()) {
            reference = canonical;
        } else {
            EXPECT_EQ(canonical, reference) << invocationSourceName(source);
        }
    }
}

}  // namespace
}  // namespace palmier::services
