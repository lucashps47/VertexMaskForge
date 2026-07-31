// M16-K.4C: automation tests for ResolveDynamicLayerChannelToggle (VertexMaskForgeLayerTypes.h) -- the
// pure, Slate-free Channel Solo (Alt-click) decision behind the Dynamic Layers UI's R/G/B checkboxes.
// Nothing here constructs SVertexMaskForgePanel, SNew()s any Slate widget, or depends on real keyboard/
// modifier-key state -- bAltDown is passed explicitly as a bool, exactly mirroring how
// SVertexMaskForgePanel::BuildDynamicLayerRow's OnCheckStateChanged_Lambda handlers resolve it once (via
// FSlateApplication::Get().GetModifierKeys().IsAltDown()) and then pass it straight through as a plain
// value -- that one-line resolution is the only genuinely Slate-dependent part of this feature and has no
// legitimate test seam without a Slate harness (none exists in this plugin); everything else, including
// the full GUID/reorder/remove-safety workflow, is proven here directly against
// FVertexMaskForgeDynamicLayerStack (the same production API the panel calls).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeDynamicLayerStack.h"

namespace
{
	using EChannel = EVertexMaskForgeDynamicLayerChannel;

	struct FRGBState
	{
		bool bR = false;
		bool bG = false;
		bool bB = false;
	};

	// All 8 possible starting RGB combinations -- used to prove Alt-click ALWAYS produces a pure solo
	// regardless of what was active beforehand.
	const FRGBState AllEightStartingStates[8] = {
		{ false, false, false }, // none
		{ true,  false, false }, // R
		{ false, true,  false }, // G
		{ false, false, true  }, // B
		{ true,  true,  false }, // RG
		{ true,  false, true  }, // RB
		{ false, true,  true  }, // GB
		{ true,  true,  true  }, // RGB
	};

	const TCHAR* StateLabels[8] = { TEXT("none"), TEXT("R"), TEXT("G"), TEXT("B"), TEXT("RG"), TEXT("RB"), TEXT("GB"), TEXT("RGB") };

	// Mirrors real SCheckBox behavior: a click always requests the OPPOSITE of the channel's current
	// checked state.
	bool CurrentValueOf(const FRGBState& State, const EChannel Channel)
	{
		switch (Channel)
		{
		case EChannel::Red: return State.bR;
		case EChannel::Green: return State.bG;
		case EChannel::Blue: return State.bB;
		default: return false;
		}
	}
}

// 1/2/3. Normal click on each channel: alters only that channel, preserves the other two -- proven from
// a mixed starting state (R on, G off, B on) so "preserve" is a real, non-trivial assertion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelSoloNormalClickTest, "VertexMaskForge.DynamicLayerChannelSolo.NormalClickAltersOnlyClickedChannel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelSoloNormalClickTest::RunTest(const FString& Parameters)
{
	// Starting state: R=true, G=false, B=true.
	{
		// Normal click on G (request Checked=true): only G changes.
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
			true, false, true, EChannel::Green, /*bRequestedChecked=*/true, /*bAltDown=*/false);
		TestTrue(TEXT("Normal click G: R preserved"), Result.bAffectRed);
		TestTrue(TEXT("Normal click G: G becomes true"), Result.bAffectGreen);
		TestTrue(TEXT("Normal click G: B preserved"), Result.bAffectBlue);
	}
	{
		// Normal click on R (request Unchecked=false): only R changes.
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
			true, false, true, EChannel::Red, /*bRequestedChecked=*/false, /*bAltDown=*/false);
		TestFalse(TEXT("Normal click R: R becomes false"), Result.bAffectRed);
		TestFalse(TEXT("Normal click R: G preserved"), Result.bAffectGreen);
		TestTrue(TEXT("Normal click R: B preserved"), Result.bAffectBlue);
	}
	{
		// Normal click on B (request Unchecked=false): only B changes.
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
			true, false, true, EChannel::Blue, /*bRequestedChecked=*/false, /*bAltDown=*/false);
		TestTrue(TEXT("Normal click B: R preserved"), Result.bAffectRed);
		TestFalse(TEXT("Normal click B: G preserved"), Result.bAffectGreen);
		TestFalse(TEXT("Normal click B: B becomes false"), Result.bAffectBlue);
	}

	return true;
}

// 12. Normal click still permits reaching every combination, including all-false and all-true --
// confirms M16-K.4C did not narrow the pre-existing normal-click contract.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelSoloNormalClickFullRangeTest, "VertexMaskForge.DynamicLayerChannelSolo.NormalClickStillReachesAllCombinations", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelSoloNormalClickFullRangeTest::RunTest(const FString& Parameters)
{
	// From RGB, normal-uncheck each channel in turn down to none.
	FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(true, true, true, EChannel::Red, false, false);
	TestTrue(TEXT("RGB -> uncheck R -> GB"), !Result.bAffectRed && Result.bAffectGreen && Result.bAffectBlue);

	Result = ResolveDynamicLayerChannelToggle(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, EChannel::Green, false, false);
	TestTrue(TEXT("GB -> uncheck G -> B"), !Result.bAffectRed && !Result.bAffectGreen && Result.bAffectBlue);

	Result = ResolveDynamicLayerChannelToggle(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, EChannel::Blue, false, false);
	TestTrue(TEXT("B -> uncheck B -> none"), !Result.bAffectRed && !Result.bAffectGreen && !Result.bAffectBlue);

	// From none, normal-check R and G to reach RG (multi-channel, never forced to solo without Alt).
	Result = ResolveDynamicLayerChannelToggle(false, false, false, EChannel::Red, true, false);
	Result = ResolveDynamicLayerChannelToggle(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, EChannel::Green, true, false);
	TestTrue(TEXT("none -> check R, check G -> RG"), Result.bAffectRed && Result.bAffectGreen && !Result.bAffectBlue);

	return true;
}

// 4/5/6. Alt-click matrix: for EVERY one of the 8 possible starting states, Alt-clicking R/G/B always
// results in exactly that one channel true and the other two false.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelSoloAltClickMatrixTest, "VertexMaskForge.DynamicLayerChannelSolo.AltClickAlwaysProducesPureSolo", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelSoloAltClickMatrixTest::RunTest(const FString& Parameters)
{
	const EChannel Channels[3] = { EChannel::Red, EChannel::Green, EChannel::Blue };
	const TCHAR* ChannelLabels[3] = { TEXT("R"), TEXT("G"), TEXT("B") };

	for (int32 ChannelIndex = 0; ChannelIndex < 3; ++ChannelIndex)
	{
		const EChannel Channel = Channels[ChannelIndex];

		for (int32 StateIndex = 0; StateIndex < 8; ++StateIndex)
		{
			const FRGBState& Start = AllEightStartingStates[StateIndex];
			// Mirror real SCheckBox semantics: clicking requests the OPPOSITE of the clicked channel's
			// CURRENT state (this matters most when the clicked channel is already the sole active one --
			// see the dedicated test below -- but is applied uniformly here for realism).
			const bool bRequestedChecked = !CurrentValueOf(Start, Channel);

			const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
				Start.bR, Start.bG, Start.bB, Channel, bRequestedChecked, /*bAltDown=*/true);

			const bool bExpectedRed = (Channel == EChannel::Red);
			const bool bExpectedGreen = (Channel == EChannel::Green);
			const bool bExpectedBlue = (Channel == EChannel::Blue);

			const FString Description = FString::Printf(TEXT("Alt-click %s from starting state '%s'"), ChannelLabels[ChannelIndex], StateLabels[StateIndex]);
			TestEqual(*FString::Printf(TEXT("%s: R"), *Description), Result.bAffectRed, bExpectedRed);
			TestEqual(*FString::Printf(TEXT("%s: G"), *Description), Result.bAffectGreen, bExpectedGreen);
			TestEqual(*FString::Printf(TEXT("%s: B"), *Description), Result.bAffectBlue, bExpectedBlue);
		}
	}

	return true;
}

// 7. Alt-click on a channel that is ALREADY the sole active one must not disable it, even though a real
// checkbox click would request Unchecked for it (since it's currently Checked).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelSoloAlreadySoloTest, "VertexMaskForge.DynamicLayerChannelSolo.AltClickOnAlreadySoloChannelStaysActive", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelSoloAlreadySoloTest::RunTest(const FString& Parameters)
{
	// R is already the sole active channel; a real click on it would request Unchecked (false).
	const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
		true, false, false, EChannel::Red, /*bRequestedChecked=*/false, /*bAltDown=*/true);

	TestTrue(TEXT("R stays active despite the checkbox requesting Unchecked"), Result.bAffectRed);
	TestFalse(TEXT("G stays inactive"), Result.bAffectGreen);
	TestFalse(TEXT("B stays inactive"), Result.bAffectBlue);
	TestTrue(TEXT("Never zero active channels from an Alt-click"), Result.bAffectRed || Result.bAffectGreen || Result.bAffectBlue);

	return true;
}

// 8/9/10. GUID association, reorder, and remove safety -- exercises the exact call sequence the panel's
// checkbox handlers perform (resolve -> ResolveDynamicLayerChannelToggle -> SetLayerChannelFilter)
// directly against the real stack, proving Alt-click targets only the correct layer by GUID, survives a
// reorder, and fails safely for a removed layer without affecting any other layer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelSoloWorkflowTest, "VertexMaskForge.DynamicLayerChannelSolo.GuidReorderRemoveWorkflow", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelSoloWorkflowTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));
	// Both default to R=G=B=true.

	// Alt-click Green on A only -- mirrors the panel's OnCheckStateChanged_Lambda for the Green checkbox.
	auto AltClick = [&Stack](const FGuid& LayerId, const EChannel Channel)
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(LayerId);
		if (!Layer)
		{
			return false;
		}
		const bool bRequestedChecked = !CurrentValueOf({ Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue }, Channel);
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
			Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue, Channel, bRequestedChecked, /*bAltDown=*/true);
		return Stack.SetLayerChannelFilter(LayerId, Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue);
	};

	TestTrue(TEXT("Alt-click Green on A succeeds"), AltClick(A, EChannel::Green));

	const FVertexMaskForgeLayer* LayerA = Stack.FindLayerById(A);
	const FVertexMaskForgeLayer* LayerB = Stack.FindLayerById(B);
	TestTrue(TEXT("A is now Green-solo"), !LayerA->bAffectRed && LayerA->bAffectGreen && !LayerA->bAffectBlue);
	TestTrue(TEXT("B is completely untouched (still RGB)"), LayerB->bAffectRed && LayerB->bAffectGreen && LayerB->bAffectBlue);

	// Reorder: move B before A.
	TestTrue(TEXT("MoveLayerUp(B)"), Stack.MoveLayerUp(B));

	// Alt-click Blue on A again, by GUID, after the reorder -- must still hit A, not whichever layer is
	// now at A's old index.
	TestTrue(TEXT("Alt-click Blue on A after reorder succeeds"), AltClick(A, EChannel::Blue));
	LayerA = Stack.FindLayerById(A);
	LayerB = Stack.FindLayerById(B);
	TestTrue(TEXT("A is now Blue-solo"), !LayerA->bAffectRed && !LayerA->bAffectGreen && LayerA->bAffectBlue);
	TestTrue(TEXT("B still unaffected by A's Alt-click"), LayerB->bAffectRed && LayerB->bAffectGreen && LayerB->bAffectBlue);

	// Remove A, then attempt a stale Alt-click on its old GUID.
	Stack.RemoveLayer(A);
	TestFalse(TEXT("Alt-click on removed A's GUID fails safely"), AltClick(A, EChannel::Red));
	LayerB = Stack.FindLayerById(B);
	TestNotNull(TEXT("B still present"), LayerB);
	TestTrue(TEXT("B still fully unaffected after A's removal and stale Alt-click"), LayerB->bAffectRed && LayerB->bAffectGreen && LayerB->bAffectBlue);
	TestTrue(TEXT("Stack remains valid"), Stack.IsValid());

	return true;
}

// 11. Tint follows an Alt-click solo immediately -- the same resolved RGB feeds both SetLayerChannelFilter
// (already proven above) and ResolveDynamicLayerChannelTint (M16-K.4A/K.4B), so the two are always
// consistent by construction; this test proves that consistency explicitly for all three channels.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelSoloTintConsistencyTest, "VertexMaskForge.DynamicLayerChannelSolo.AltClickResultMatchesExpectedTint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelSoloTintConsistencyTest::RunTest(const FString& Parameters)
{
	using ETint = EVertexMaskForgeDynamicLayerChannelTint;

	// Starting from RGB (all active), Alt-click each channel in turn and confirm the resulting tint.
	{
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(true, true, true, EChannel::Red, false, true);
		TestTrue(TEXT("Alt-click R from RGB -> Red tint"), ResolveDynamicLayerChannelTint(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue) == ETint::Red);
	}
	{
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(true, true, true, EChannel::Green, false, true);
		TestTrue(TEXT("Alt-click G from RGB -> Green tint"), ResolveDynamicLayerChannelTint(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue) == ETint::Green);
	}
	{
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(true, true, true, EChannel::Blue, false, true);
		TestTrue(TEXT("Alt-click B from RGB -> Blue tint"), ResolveDynamicLayerChannelTint(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue) == ETint::Blue);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
