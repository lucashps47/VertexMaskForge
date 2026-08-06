// M16-K.4C: automation tests for ResolveDynamicLayerChannelToggle (VertexMaskForgeLayerTypes.h) -- the
// pure, Slate-free Channel Solo (Alt-click) decision behind the Dynamic Layers UI's R/G/B checkboxes.
// M19-A: extended to cover the fourth channel, Alpha, symmetrically -- the toggle helper is now
// four-channel-capable ahead of the artist-facing Alpha checkbox (deferred to M19-B). Nothing here
// constructs SVertexMaskForgePanel, SNew()s any Slate widget, or depends on real keyboard/modifier-key
// state -- bAltDown is passed explicitly as a bool, exactly mirroring how SVertexMaskForgePanel::
// BuildDynamicLayerRow's OnCheckStateChanged_Lambda handlers resolve it once (via
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

	struct FRGBAState
	{
		bool bR = false;
		bool bG = false;
		bool bB = false;
		bool bA = false;
	};

	// All 8 possible starting RGB combinations (Alpha always starts false, matching
	// FVertexMaskForgeLayer::bAffectAlpha's own default) -- used to prove Alt-click ALWAYS produces a pure
	// solo regardless of what was active beforehand.
	const FRGBAState AllEightStartingStates[8] = {
		{ false, false, false, false }, // none
		{ true,  false, false, false }, // R
		{ false, true,  false, false }, // G
		{ false, false, true,  false }, // B
		{ true,  true,  false, false }, // RG
		{ true,  false, true,  false }, // RB
		{ false, true,  true,  false }, // GB
		{ true,  true,  true,  false }, // RGB
	};

	const TCHAR* StateLabels[8] = { TEXT("none"), TEXT("R"), TEXT("G"), TEXT("B"), TEXT("RG"), TEXT("RB"), TEXT("GB"), TEXT("RGB") };

	// Mirrors real SCheckBox behavior: a click always requests the OPPOSITE of the channel's current
	// checked state.
	bool CurrentValueOf(const FRGBAState& State, const EChannel Channel)
	{
		switch (Channel)
		{
		case EChannel::Red: return State.bR;
		case EChannel::Green: return State.bG;
		case EChannel::Blue: return State.bB;
		case EChannel::Alpha: return State.bA;
		default: return false;
		}
	}
}

// 1/2/3/3b. Normal click on each channel: alters only that channel, preserves the other three -- proven
// from a mixed starting state (R on, G off, B on, A on) so "preserve" is a real, non-trivial assertion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelSoloNormalClickTest, "VertexMaskForge.DynamicLayerChannelSolo.NormalClickAltersOnlyClickedChannel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelSoloNormalClickTest::RunTest(const FString& Parameters)
{
	// Starting state: R=true, G=false, B=true, A=true.
	{
		// Normal click on G (request Checked=true): only G changes.
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
			true, false, true, true, EChannel::Green, /*bRequestedChecked=*/true, /*bAltDown=*/false);
		TestTrue(TEXT("Normal click G: R preserved"), Result.bAffectRed);
		TestTrue(TEXT("Normal click G: G becomes true"), Result.bAffectGreen);
		TestTrue(TEXT("Normal click G: B preserved"), Result.bAffectBlue);
		TestTrue(TEXT("Normal click G: A preserved"), Result.bAffectAlpha);
	}
	{
		// Normal click on R (request Unchecked=false): only R changes.
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
			true, false, true, true, EChannel::Red, /*bRequestedChecked=*/false, /*bAltDown=*/false);
		TestFalse(TEXT("Normal click R: R becomes false"), Result.bAffectRed);
		TestFalse(TEXT("Normal click R: G preserved"), Result.bAffectGreen);
		TestTrue(TEXT("Normal click R: B preserved"), Result.bAffectBlue);
		TestTrue(TEXT("Normal click R: A preserved"), Result.bAffectAlpha);
	}
	{
		// Normal click on B (request Unchecked=false): only B changes.
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
			true, false, true, true, EChannel::Blue, /*bRequestedChecked=*/false, /*bAltDown=*/false);
		TestTrue(TEXT("Normal click B: R preserved"), Result.bAffectRed);
		TestFalse(TEXT("Normal click B: G preserved"), Result.bAffectGreen);
		TestFalse(TEXT("Normal click B: B becomes false"), Result.bAffectBlue);
		TestTrue(TEXT("Normal click B: A preserved"), Result.bAffectAlpha);
	}
	{
		// Normal click on A (request Unchecked=false): only A changes. Symmetric with R/G/B above.
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
			true, false, true, true, EChannel::Alpha, /*bRequestedChecked=*/false, /*bAltDown=*/false);
		TestTrue(TEXT("Normal click A: R preserved"), Result.bAffectRed);
		TestFalse(TEXT("Normal click A: G preserved"), Result.bAffectGreen);
		TestTrue(TEXT("Normal click A: B preserved"), Result.bAffectBlue);
		TestFalse(TEXT("Normal click A: A becomes false"), Result.bAffectAlpha);
	}

	return true;
}

// 12. Normal click still permits reaching every combination, including all-false and all-true --
// confirms M16-K.4C did not narrow the pre-existing normal-click contract. M19-A: extended to also pass
// through the all-zero -> A-only -> all-zero transition, proving Alpha is reachable via manual clicks
// exactly like R/G/B, and that "manual A-only isolation" (R/G/B unchecked, A checked one at a time)
// produces the identical end state as Alt-click A (see AltClickAlwaysProducesPureSolo below).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelSoloNormalClickFullRangeTest, "VertexMaskForge.DynamicLayerChannelSolo.NormalClickStillReachesAllCombinations", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelSoloNormalClickFullRangeTest::RunTest(const FString& Parameters)
{
	// From RGB (Alpha starts false, the default), normal-uncheck each RGB channel in turn down to none.
	FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(true, true, true, false, EChannel::Red, false, false);
	TestTrue(TEXT("RGB -> uncheck R -> GB"), !Result.bAffectRed && Result.bAffectGreen && Result.bAffectBlue && !Result.bAffectAlpha);

	Result = ResolveDynamicLayerChannelToggle(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha, EChannel::Green, false, false);
	TestTrue(TEXT("GB -> uncheck G -> B"), !Result.bAffectRed && !Result.bAffectGreen && Result.bAffectBlue && !Result.bAffectAlpha);

	Result = ResolveDynamicLayerChannelToggle(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha, EChannel::Blue, false, false);
	TestTrue(TEXT("B -> uncheck B -> none"), !Result.bAffectRed && !Result.bAffectGreen && !Result.bAffectBlue && !Result.bAffectAlpha);

	// Manual A-only isolation: from all-zero, check only A -- must reach the identical state Alt-click A
	// reaches from any starting state (see AltClickAlwaysProducesPureSolo).
	Result = ResolveDynamicLayerChannelToggle(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha, EChannel::Alpha, true, false);
	TestTrue(TEXT("none -> check A -> A-only"), !Result.bAffectRed && !Result.bAffectGreen && !Result.bAffectBlue && Result.bAffectAlpha);

	// Manual return from A-only to all-zero -- uncheck A.
	Result = ResolveDynamicLayerChannelToggle(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha, EChannel::Alpha, false, false);
	TestTrue(TEXT("A-only -> uncheck A -> none"), !Result.bAffectRed && !Result.bAffectGreen && !Result.bAffectBlue && !Result.bAffectAlpha);

	// From none, normal-check R and G to reach RG (multi-channel, never forced to solo without Alt).
	Result = ResolveDynamicLayerChannelToggle(false, false, false, false, EChannel::Red, true, false);
	Result = ResolveDynamicLayerChannelToggle(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha, EChannel::Green, true, false);
	TestTrue(TEXT("none -> check R, check G -> RG"), Result.bAffectRed && Result.bAffectGreen && !Result.bAffectBlue && !Result.bAffectAlpha);

	// All four channels disabled must remain a reachable, valid toggle-helper output (mirrors
	// FVertexMaskForgeDynamicLayerStack::SetLayerChannelFilter's own "accepts all-false" contract).
	Result = ResolveDynamicLayerChannelToggle(true, true, true, true, EChannel::Red, false, false);
	Result = ResolveDynamicLayerChannelToggle(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha, EChannel::Green, false, false);
	Result = ResolveDynamicLayerChannelToggle(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha, EChannel::Blue, false, false);
	Result = ResolveDynamicLayerChannelToggle(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha, EChannel::Alpha, false, false);
	TestTrue(TEXT("RGBA -> uncheck all four -> none"), !Result.bAffectRed && !Result.bAffectGreen && !Result.bAffectBlue && !Result.bAffectAlpha);

	return true;
}

// 4/5/6/6b. Alt-click matrix: for EVERY one of the 8 possible starting states, Alt-clicking R/G/B/A always
// results in exactly that one channel true and the other three false.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelSoloAltClickMatrixTest, "VertexMaskForge.DynamicLayerChannelSolo.AltClickAlwaysProducesPureSolo", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelSoloAltClickMatrixTest::RunTest(const FString& Parameters)
{
	const EChannel Channels[4] = { EChannel::Red, EChannel::Green, EChannel::Blue, EChannel::Alpha };
	const TCHAR* ChannelLabels[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };

	for (int32 ChannelIndex = 0; ChannelIndex < 4; ++ChannelIndex)
	{
		const EChannel Channel = Channels[ChannelIndex];

		for (int32 StateIndex = 0; StateIndex < 8; ++StateIndex)
		{
			const FRGBAState& Start = AllEightStartingStates[StateIndex];
			// Mirror real SCheckBox semantics: clicking requests the OPPOSITE of the clicked channel's
			// CURRENT state (this matters most when the clicked channel is already the sole active one --
			// see the dedicated test below -- but is applied uniformly here for realism).
			const bool bRequestedChecked = !CurrentValueOf(Start, Channel);

			const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
				Start.bR, Start.bG, Start.bB, Start.bA, Channel, bRequestedChecked, /*bAltDown=*/true);

			const bool bExpectedRed = (Channel == EChannel::Red);
			const bool bExpectedGreen = (Channel == EChannel::Green);
			const bool bExpectedBlue = (Channel == EChannel::Blue);
			const bool bExpectedAlpha = (Channel == EChannel::Alpha);

			const FString Description = FString::Printf(TEXT("Alt-click %s from starting state '%s'"), ChannelLabels[ChannelIndex], StateLabels[StateIndex]);
			TestEqual(*FString::Printf(TEXT("%s: R"), *Description), Result.bAffectRed, bExpectedRed);
			TestEqual(*FString::Printf(TEXT("%s: G"), *Description), Result.bAffectGreen, bExpectedGreen);
			TestEqual(*FString::Printf(TEXT("%s: B"), *Description), Result.bAffectBlue, bExpectedBlue);
			TestEqual(*FString::Printf(TEXT("%s: A"), *Description), Result.bAffectAlpha, bExpectedAlpha);
		}
	}

	return true;
}

// 7/7b. Alt-click on a channel that is ALREADY the sole active one must not disable it, even though a real
// checkbox click would request Unchecked for it (since it's currently Checked). Covers both an RGB channel
// (R) and Alpha, symmetrically.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelSoloAlreadySoloTest, "VertexMaskForge.DynamicLayerChannelSolo.AltClickOnAlreadySoloChannelStaysActive", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelSoloAlreadySoloTest::RunTest(const FString& Parameters)
{
	// R is already the sole active channel; a real click on it would request Unchecked (false).
	{
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
			true, false, false, false, EChannel::Red, /*bRequestedChecked=*/false, /*bAltDown=*/true);

		TestTrue(TEXT("R stays active despite the checkbox requesting Unchecked"), Result.bAffectRed);
		TestFalse(TEXT("G stays inactive"), Result.bAffectGreen);
		TestFalse(TEXT("B stays inactive"), Result.bAffectBlue);
		TestFalse(TEXT("A stays inactive"), Result.bAffectAlpha);
		TestTrue(TEXT("Never zero active channels from an Alt-click"), Result.bAffectRed || Result.bAffectGreen || Result.bAffectBlue || Result.bAffectAlpha);
	}
	// A is already the sole active channel; a real click on it would request Unchecked (false).
	{
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
			false, false, false, true, EChannel::Alpha, /*bRequestedChecked=*/false, /*bAltDown=*/true);

		TestFalse(TEXT("R stays inactive"), Result.bAffectRed);
		TestFalse(TEXT("G stays inactive"), Result.bAffectGreen);
		TestFalse(TEXT("B stays inactive"), Result.bAffectBlue);
		TestTrue(TEXT("A stays active despite the checkbox requesting Unchecked"), Result.bAffectAlpha);
		TestTrue(TEXT("Never zero active channels from an Alt-click"), Result.bAffectRed || Result.bAffectGreen || Result.bAffectBlue || Result.bAffectAlpha);
	}

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
	// Both default to R=G=B=true, Alpha=false.

	// Alt-click a channel on the given layer only -- mirrors the panel's OnCheckStateChanged_Lambda.
	auto AltClick = [&Stack](const FGuid& LayerId, const EChannel Channel)
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(LayerId);
		if (!Layer)
		{
			return false;
		}
		const bool bRequestedChecked = !CurrentValueOf({ Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue, Layer->bAffectAlpha }, Channel);
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
			Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue, Layer->bAffectAlpha, Channel, bRequestedChecked, /*bAltDown=*/true);
		return Stack.SetLayerChannelFilter(LayerId, Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha);
	};

	TestTrue(TEXT("Alt-click Green on A succeeds"), AltClick(A, EChannel::Green));

	const FVertexMaskForgeLayer* LayerA = Stack.FindLayerById(A);
	const FVertexMaskForgeLayer* LayerB = Stack.FindLayerById(B);
	TestTrue(TEXT("A is now Green-solo"), !LayerA->bAffectRed && LayerA->bAffectGreen && !LayerA->bAffectBlue && !LayerA->bAffectAlpha);
	TestTrue(TEXT("B is completely untouched (still RGB, no Alpha)"), LayerB->bAffectRed && LayerB->bAffectGreen && LayerB->bAffectBlue && !LayerB->bAffectAlpha);

	// Reorder: move B before A.
	TestTrue(TEXT("MoveLayerUp(B)"), Stack.MoveLayerUp(B));

	// Alt-click Alpha on A again, by GUID, after the reorder -- must still hit A, not whichever layer is
	// now at A's old index.
	TestTrue(TEXT("Alt-click Alpha on A after reorder succeeds"), AltClick(A, EChannel::Alpha));
	LayerA = Stack.FindLayerById(A);
	LayerB = Stack.FindLayerById(B);
	TestTrue(TEXT("A is now Alpha-solo"), !LayerA->bAffectRed && !LayerA->bAffectGreen && !LayerA->bAffectBlue && LayerA->bAffectAlpha);
	TestTrue(TEXT("B still unaffected by A's Alt-click"), LayerB->bAffectRed && LayerB->bAffectGreen && LayerB->bAffectBlue && !LayerB->bAffectAlpha);

	// Remove A, then attempt a stale Alt-click on its old GUID.
	Stack.RemoveLayer(A);
	TestFalse(TEXT("Alt-click on removed A's GUID fails safely"), AltClick(A, EChannel::Red));
	LayerB = Stack.FindLayerById(B);
	TestNotNull(TEXT("B still present"), LayerB);
	TestTrue(TEXT("B still fully unaffected after A's removal and stale Alt-click"), LayerB->bAffectRed && LayerB->bAffectGreen && LayerB->bAffectBlue && !LayerB->bAffectAlpha);
	TestTrue(TEXT("Stack remains valid"), Stack.IsValid());

	return true;
}

// 11. Tint follows an Alt-click solo immediately -- the same resolved RGB feeds both SetLayerChannelFilter
// (already proven above) and ResolveDynamicLayerChannelTint (M16-K.4A/K.4B), so the two are always
// consistent by construction; this test proves that consistency explicitly for all three RGB channels.
// M19-A note: ResolveDynamicLayerChannelTint itself remains three-channel (R/G/B) -- the white Alpha-only
// row tint is deferred to M19-B (see EVertexMaskForgeDynamicLayerChannelTint's own FUTURE DECISION doc
// comment in VertexMaskForgeLayerTypes.h) and is deliberately NOT exercised here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelSoloTintConsistencyTest, "VertexMaskForge.DynamicLayerChannelSolo.AltClickResultMatchesExpectedTint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelSoloTintConsistencyTest::RunTest(const FString& Parameters)
{
	using ETint = EVertexMaskForgeDynamicLayerChannelTint;

	// Starting from RGB (all active), Alt-click each channel in turn and confirm the resulting tint.
	{
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(true, true, true, false, EChannel::Red, false, true);
		TestTrue(TEXT("Alt-click R from RGB -> Red tint"), ResolveDynamicLayerChannelTint(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha) == ETint::Red);
	}
	{
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(true, true, true, false, EChannel::Green, false, true);
		TestTrue(TEXT("Alt-click G from RGB -> Green tint"), ResolveDynamicLayerChannelTint(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha) == ETint::Green);
	}
	{
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(true, true, true, false, EChannel::Blue, false, true);
		TestTrue(TEXT("Alt-click B from RGB -> Blue tint"), ResolveDynamicLayerChannelTint(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha) == ETint::Blue);
	}
	// M19-B: Alt-click A from RGB -> White tint (Alpha-only), the exact symmetry this test proves for R/G/B.
	{
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(true, true, true, false, EChannel::Alpha, false, true);
		TestTrue(TEXT("Alt-click A from RGB -> White tint"), ResolveDynamicLayerChannelTint(Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha) == ETint::White);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
