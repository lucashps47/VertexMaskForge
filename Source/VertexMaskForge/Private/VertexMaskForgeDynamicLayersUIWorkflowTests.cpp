// M16-K.4: Dynamic Layers UI Prototype -- workflow-level tests.
//
// HONEST SCOPE STATEMENT (read before trusting these test names): NOTHING in this file constructs
// SVertexMaskForgePanel, SNew()s any Slate widget, or invokes any SVertexMaskForgePanel member function
// (RebuildDynamicLayersList/BuildDynamicLayerRow/OnAddDynamicLayerClicked/OnRemoveDynamicLayerClicked/
// OnMoveDynamicLayerUp/DownClicked/CanMoveDynamicLayerUp/Down are all private-to-the-panel and there is no
// legitimate seam to call them without constructing the full panel -- no such seam is added here, per this
// checkpoint's explicit "no private-access hacks, no test-only APIs" constraint). No Slate/panel test
// harness exists anywhere in this plugin (confirmed: none of the prior 5 prototype tests or any other
// automation test in this module construct SVertexMaskForgePanel either).
//
// What IS tested: every SVertexMaskForgePanel Dynamic Layers callback is, BY DESIGN, a thin 1:1 delegation
// to FVertexMaskForgeDynamicLayerStack's own controlled API plus (for structural changes only) a rebuild
// call -- see SVertexMaskForgePanel.cpp's own M16-K.4 module comment. The callbacks contain no business
// logic of their own EXCEPT the "read current Layer, preserve two channels, change one" pattern used by
// the R/G/B checkboxes' OnCheckStateChanged_Lambda handlers. This file exercises exactly that real usage
// pattern directly against FVertexMaskForgeDynamicLayerStack (the same production API the panel calls),
// and a couple of realistic end-to-end sequences (Add -> Rename -> Reorder -> Remove; a callback firing
// for an already-removed layer) mirroring what a sequence of real button clicks would do -- proving the
// WORKFLOW is sound. It does NOT and CANNOT prove Slate wiring (that a click on the real "Up" button
// actually reaches OnMoveDynamicLayerUpClicked, that the row layout renders correctly, that
// Text_Lambda/IsChecked_Lambda actually repaint after a mutation, etc.) -- those require the manual
// validation checklist in this checkpoint's own report.
//
// Every individual mutation (AddLayer/RemoveLayer/RenameLayer/MoveLayerUp/MoveLayerDown/SetLayerFill/
// SetLayerBlendMode/SetLayerOpacity/SetLayerEnabled/SetLayerChannelFilter, GUID-based tracking across
// reorder, boundary safety, unknown-id no-op, duplicate names) is already fully covered by
// VertexMaskForgeDynamicLayerStackTests.cpp (15 M16-K.3A + 4 M16-K.3B tests) and
// VertexMaskForgeDynamicLayerEvaluatorTests.cpp (24 M16-K.3B tests) -- this file does not re-test those in
// isolation; it composes them into the specific sequences the UI actually performs.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeDynamicLayerStack.h"

// A. Simulates the R/G/B checkbox pattern used by BuildDynamicLayerRow's OnCheckStateChanged_Lambda
// handlers: read the layer's CURRENT other two channels, then call SetLayerChannelFilter with the one
// changed value plus the two preserved values -- proving this exact read-preserve-write sequence leaves
// the other two channels untouched, for all three channels in turn.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayersUIChannelPreservationTest, "VertexMaskForge.DynamicLayersUIWorkflow.ChannelTogglePreservesOtherTwo", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayersUIChannelPreservationTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	// Defaults: R=G=B=true.

	// Toggle R off, exactly as the Red checkbox's handler would: read current G/B, write new R + old G/B.
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		TestNotNull(TEXT("Layer found"), Layer);
		const bool bResult = Stack.SetLayerChannelFilter(Id, /*NewRed=*/false, Layer->bAffectGreen, Layer->bAffectBlue);
		TestTrue(TEXT("SetLayerChannelFilter(R=false) succeeds"), bResult);
	}
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		TestFalse(TEXT("R is now false"), Layer->bAffectRed);
		TestTrue(TEXT("G preserved true"), Layer->bAffectGreen);
		TestTrue(TEXT("B preserved true"), Layer->bAffectBlue);
	}

	// Toggle G off the same way -- R (already false) and B (true) must both be preserved.
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		Stack.SetLayerChannelFilter(Id, Layer->bAffectRed, /*NewGreen=*/false, Layer->bAffectBlue);
	}
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		TestFalse(TEXT("R remains false"), Layer->bAffectRed);
		TestFalse(TEXT("G is now false"), Layer->bAffectGreen);
		TestTrue(TEXT("B still preserved true"), Layer->bAffectBlue);
	}

	// Toggle B off -- all three now false, which must be permitted (compositionally no-op layer, still valid).
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		Stack.SetLayerChannelFilter(Id, Layer->bAffectRed, Layer->bAffectGreen, /*NewBlue=*/false);
	}
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		TestFalse(TEXT("All three channels now false"), Layer->bAffectRed || Layer->bAffectGreen || Layer->bAffectBlue);
	}
	TestTrue(TEXT("Stack remains valid with all channels disabled"), Stack.IsValid());

	return true;
}

// B. End-to-end workflow: Add two layers (mirrors two "+ Add Layer" clicks), rename one (mirrors an
// OnTextCommitted), reorder via MoveLayerUp (mirrors an "Up" click), remove one (mirrors a "Remove"
// click) -- the exact sequence of stack calls SVertexMaskForgePanel's Dynamic Layers callbacks perform,
// composed together, proving identity/order/data integrity survives a realistic interaction sequence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayersUIAddRenameReorderRemoveTest, "VertexMaskForge.DynamicLayersUIWorkflow.AddRenameReorderRemoveSequence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayersUIAddRenameReorderRemoveTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack = FVertexMaskForgeDynamicLayerStack::MakeInitialStack();
	TestEqual(TEXT("Starts with one Base Layer"), Stack.Num(), 1);

	// Two "+ Add Layer" clicks -- OnAddDynamicLayerClicked's own naming convention (Num()+1 at click time).
	const FGuid Second = Stack.AddLayer(FString::Printf(TEXT("Layer %d"), Stack.Num() + 1)); // "Layer 2"
	const FGuid Third = Stack.AddLayer(FString::Printf(TEXT("Layer %d"), Stack.Num() + 1));  // "Layer 3"
	TestEqual(TEXT("Three layers now"), Stack.Num(), 3);
	TestEqual(TEXT("Second layer auto-named"), Stack.FindLayerById(Second)->Name, FString(TEXT("Layer 2")));
	TestEqual(TEXT("Third layer auto-named"), Stack.FindLayerById(Third)->Name, FString(TEXT("Layer 3")));

	// Rename Third -- OnTextCommitted's own call shape.
	TestTrue(TEXT("Rename Third"), Stack.RenameLayer(Third, TEXT("Dirt")));
	TestEqual(TEXT("Third renamed"), Stack.FindLayerById(Third)->Name, FString(TEXT("Dirt")));

	// Move Third up twice ("Up" clicked twice) -- from index 2 to index 0.
	TestTrue(TEXT("MoveLayerUp(Third) #1"), Stack.MoveLayerUp(Third));
	TestTrue(TEXT("MoveLayerUp(Third) #2"), Stack.MoveLayerUp(Third));
	TestEqual(TEXT("Third (renamed Dirt) now first"), Stack.GetLayers()[0].LayerId, Third);
	TestEqual(TEXT("Third's data survived the moves"), Stack.GetLayers()[0].Name, FString(TEXT("Dirt")));

	// Remove Second ("Remove" clicked on Second's row).
	const FVertexMaskForgeDynamicLayerStack::FRemoveResult RemoveResult = Stack.RemoveLayer(Second);
	TestTrue(TEXT("Second removed"), RemoveResult.bRemoved);
	TestEqual(TEXT("Two layers remain"), Stack.Num(), 2);
	TestNull(TEXT("Second is gone"), Stack.FindLayerById(Second));
	TestNotNull(TEXT("Third (Dirt) still present"), Stack.FindLayerById(Third));

	TestTrue(TEXT("Stack remains valid throughout"), Stack.IsValid());

	return true;
}

// C. A callback firing for a layer that was already removed by an earlier click (the "callback tardio"
// case) -- every Set*/Move* call the UI could issue must fail safely (false, no mutation, no crash), and
// the rest of the stack must be completely unaffected.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayersUIStaleCallbackTest, "VertexMaskForge.DynamicLayersUIWorkflow.StaleCallbackAfterRemovalIsSafe", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayersUIStaleCallbackTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Survivor = Stack.AddLayer(TEXT("Survivor"));
	const FGuid Removed = Stack.AddLayer(TEXT("Removed"));

	Stack.RemoveLayer(Removed);
	TestNull(TEXT("Removed layer is gone"), Stack.FindLayerById(Removed));

	// Simulate every kind of callback that could still be queued for the now-gone row's widgets.
	TestFalse(TEXT("SetLayerFill on removed id"), Stack.SetLayerFill(Removed, EVertexMaskForgeLayerFill::White));
	TestFalse(TEXT("SetLayerBlendMode on removed id"), Stack.SetLayerBlendMode(Removed, EVertexMaskForgeBlendMode::Add));
	TestFalse(TEXT("SetLayerOpacity on removed id"), Stack.SetLayerOpacity(Removed, 0.5f));
	TestFalse(TEXT("SetLayerEnabled on removed id"), Stack.SetLayerEnabled(Removed, false));
	TestFalse(TEXT("SetLayerChannelFilter on removed id"), Stack.SetLayerChannelFilter(Removed, false, false, false));
	TestFalse(TEXT("RenameLayer on removed id"), Stack.RenameLayer(Removed, TEXT("Ghost")));
	TestFalse(TEXT("MoveLayerUp on removed id"), Stack.MoveLayerUp(Removed));
	TestFalse(TEXT("MoveLayerDown on removed id"), Stack.MoveLayerDown(Removed));
	TestFalse(TEXT("MoveLayer on removed id"), Stack.MoveLayer(Removed, 0));
	const FVertexMaskForgeDynamicLayerStack::FRemoveResult SecondRemove = Stack.RemoveLayer(Removed);
	TestFalse(TEXT("RemoveLayer on already-removed id is a no-op"), SecondRemove.bRemoved);

	// The survivor must be completely unaffected by all of the above.
	const FVertexMaskForgeLayer* SurvivorLayer = Stack.FindLayerById(Survivor);
	TestNotNull(TEXT("Survivor still present"), SurvivorLayer);
	if (SurvivorLayer)
	{
		TestEqual(TEXT("Survivor name unchanged"), SurvivorLayer->Name, FString(TEXT("Survivor")));
		TestTrue(TEXT("Survivor Fill unchanged"), SurvivorLayer->Fill == EVertexMaskForgeLayerFill::None);
		TestTrue(TEXT("Survivor Enabled unchanged"), SurvivorLayer->bEnabled);
	}
	TestEqual(TEXT("Exactly one layer remains"), Stack.Num(), 1);
	TestTrue(TEXT("Stack remains valid"), Stack.IsValid());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
