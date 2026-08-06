// M16-K.4A/M16-K.4B: automation tests for the Dynamic Layers row tint's pure, Slate-free logic --
// ResolveDynamicLayerChannelTint (the channel-exclusivity DECISION: which of Default/Red/Green/Blue/White)
// and GetDynamicLayerChannelTintColor (the literal FLinearColor RGBA for the four exclusive-channel cases),
// both in VertexMaskForgeLayerTypes.h. Nothing here constructs SVertexMaskForgePanel or SNew()s any Slate
// widget.
//
// HONEST SCOPE STATEMENT (M16-K.4B root-cause fix): the M16-K.4A bug (row stayed gray for every channel
// combination) was NOT in the decision logic tested below -- ResolveDynamicLayerChannelTint always
// returned the correct kind, and these same tests already passed while the UI was visibly broken (this is
// exactly the gap the M16-K.4B corrective prompt called out). The actual defect was in
// SVertexMaskForgePanel::GetDynamicLayerChannelTint/BuildDynamicLayerRow: SBorder's DEFAULT BorderImage
// ("Border", a brush whose own TintColor is already FStyleColors::Panel, a near-black color) meant
// BorderBackgroundColor(1,0,0,0.15) multiplied against an already near-black color instead of a neutral
// white one, crushing the visible signal. That defect lived entirely in Slate widget construction/paint
// composition, which this plugin has no test harness for (no test anywhere constructs
// SVertexMaskForgePanel or performs a real Slate paint pass) -- it is NOT re-testable here even after the
// fix. What IS newly testable this round, extracted specifically because the M16-K.4B prompt required
// proving more than the untouched decision logic: GetDynamicLayerChannelTintColor's actual literal RGBA
// constants (Red/Green/Blue), and the FStyleColors::Panel-not-White invariant the fix depends on. The
// real, load-bearing proof that pixels on screen actually change color is -- and can only be -- the
// manual visual checklist in this checkpoint's own report.
//
// M19-B: extended throughout for the fourth channel, Alpha, and its White tint -- ResolveDynamicLayerChannelTint
// now takes bAffectAlpha as a fourth parameter everywhere it's called, and new tests below prove White
// resolves exclusively for Alpha-only ownership, reuses the identical TintAlpha as Red/Green/Blue, and that
// a fresh (Alpha-disabled) layer never receives it.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Styling/StyleColors.h"
#include "VertexMaskForgeDynamicLayerStack.h"

using ETint = EVertexMaskForgeDynamicLayerChannelTint;

// 1. RedOnlyUsesRedTint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintRedOnlyTest, "VertexMaskForge.DynamicLayerChannelTint.RedOnlyUsesRedTint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintRedOnlyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("R only -> Red"), ResolveDynamicLayerChannelTint(true, false, false, false) == ETint::Red);
	return true;
}

// 2. GreenOnlyUsesGreenTint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintGreenOnlyTest, "VertexMaskForge.DynamicLayerChannelTint.GreenOnlyUsesGreenTint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintGreenOnlyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("G only -> Green"), ResolveDynamicLayerChannelTint(false, true, false, false) == ETint::Green);
	return true;
}

// 3. BlueOnlyUsesBlueTint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintBlueOnlyTest, "VertexMaskForge.DynamicLayerChannelTint.BlueOnlyUsesBlueTint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintBlueOnlyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("B only -> Blue"), ResolveDynamicLayerChannelTint(false, false, true, false) == ETint::Blue);
	return true;
}

// 3b. M19-B. AlphaOnlyUsesWhiteTint: the new exclusive-channel case -- R=G=B=false, A=true -> White,
// exactly symmetric with R/G/B above, implementing the enum's own pre-existing FUTURE DECISION doc comment.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintAlphaOnlyTest, "VertexMaskForge.DynamicLayerChannelTint.AlphaOnlyUsesWhiteTint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintAlphaOnlyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("A only -> White"), ResolveDynamicLayerChannelTint(false, false, false, true) == ETint::White);
	return true;
}

// 4. MultipleChannelsUseDefaultAppearance -- RG, RB, GB, RGB all resolve to Default, never a combined hue.
// M19-B: extended with every combination that pairs Alpha alongside an RGB channel (RA, GA, BA, RGA, RGBA)
// -- all must ALSO resolve to Default, exactly like today's RGB-only combinations, per the checkpoint's
// own instruction that Alpha alongside any RGB channel is never treated as exclusive.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintMultipleTest, "VertexMaskForge.DynamicLayerChannelTint.MultipleChannelsUseDefaultAppearance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintMultipleTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("RG -> Default (never yellow)"), ResolveDynamicLayerChannelTint(true, true, false, false) == ETint::Default);
	TestTrue(TEXT("RB -> Default (never magenta)"), ResolveDynamicLayerChannelTint(true, false, true, false) == ETint::Default);
	TestTrue(TEXT("GB -> Default (never cyan)"), ResolveDynamicLayerChannelTint(false, true, true, false) == ETint::Default);
	TestTrue(TEXT("RGB -> Default (never white)"), ResolveDynamicLayerChannelTint(true, true, true, false) == ETint::Default);
	TestTrue(TEXT("RA -> Default (never pink)"), ResolveDynamicLayerChannelTint(true, false, false, true) == ETint::Default);
	TestTrue(TEXT("GA -> Default"), ResolveDynamicLayerChannelTint(false, true, false, true) == ETint::Default);
	TestTrue(TEXT("BA -> Default"), ResolveDynamicLayerChannelTint(false, false, true, true) == ETint::Default);
	TestTrue(TEXT("RGA -> Default"), ResolveDynamicLayerChannelTint(true, true, false, true) == ETint::Default);
	TestTrue(TEXT("RBA -> Default"), ResolveDynamicLayerChannelTint(true, false, true, true) == ETint::Default);
	TestTrue(TEXT("GBA -> Default"), ResolveDynamicLayerChannelTint(false, true, true, true) == ETint::Default);
	TestTrue(TEXT("RGBA (all four) -> Default"), ResolveDynamicLayerChannelTint(true, true, true, true) == ETint::Default);
	return true;
}

// 5. NoChannelsUsesDefaultAppearance -- never black.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintNoneTest, "VertexMaskForge.DynamicLayerChannelTint.NoChannelsUsesDefaultAppearance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintNoneTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("No channels -> Default (never black)"), ResolveDynamicLayerChannelTint(false, false, false, false) == ETint::Default);
	return true;
}

// 6. TintFollowsGuidAfterReorder: the tint decision is a function of the LAYER's own current channel
// flags, resolved by GUID -- reordering two single-channel layers must not swap which one resolves Red
// vs. Green (proving the decision is never associated with a position/index).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintReorderTest, "VertexMaskForge.DynamicLayerChannelTint.TintFollowsGuidAfterReorder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintReorderTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid RedLayer = Stack.AddLayer(TEXT("RedLayer"));
	Stack.SetLayerChannelFilter(RedLayer, true, false, false, false);
	const FGuid GreenLayer = Stack.AddLayer(TEXT("GreenLayer"));
	Stack.SetLayerChannelFilter(GreenLayer, false, true, false, false);

	auto ResolveByGuid = [&Stack](const FGuid& Id) -> ETint
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		return Layer ? ResolveDynamicLayerChannelTint(Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue, Layer->bAffectAlpha) : ETint::Default;
	};

	TestTrue(TEXT("Before reorder: RedLayer resolves Red"), ResolveByGuid(RedLayer) == ETint::Red);
	TestTrue(TEXT("Before reorder: GreenLayer resolves Green"), ResolveByGuid(GreenLayer) == ETint::Green);

	TestTrue(TEXT("MoveLayerUp(GreenLayer) succeeds"), Stack.MoveLayerUp(GreenLayer));
	TestEqual(TEXT("GreenLayer now first"), Stack.GetLayers()[0].LayerId, GreenLayer);

	// After reorder, resolving BY GUID must still give each layer its OWN color -- never swapped because
	// of the position change.
	TestTrue(TEXT("After reorder: RedLayer STILL resolves Red"), ResolveByGuid(RedLayer) == ETint::Red);
	TestTrue(TEXT("After reorder: GreenLayer STILL resolves Green"), ResolveByGuid(GreenLayer) == ETint::Green);

	return true;
}

// 6b. M19-B. TintFollowsGuidAfterReorderAlpha: same GUID-not-index proof as above, but for an Alpha-only
// (White) layer alongside an RGB one -- exercises the exact stale-widget/reorder concern the checkpoint
// calls out specifically for the new A control.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintReorderAlphaTest, "VertexMaskForge.DynamicLayerChannelTint.TintFollowsGuidAfterReorderAlpha", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintReorderAlphaTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid AlphaLayer = Stack.AddLayer(TEXT("AlphaLayer"));
	Stack.SetLayerChannelFilter(AlphaLayer, false, false, false, true);
	const FGuid RedLayer = Stack.AddLayer(TEXT("RedLayer"));
	Stack.SetLayerChannelFilter(RedLayer, true, false, false, false);

	auto ResolveByGuid = [&Stack](const FGuid& Id) -> ETint
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		return Layer ? ResolveDynamicLayerChannelTint(Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue, Layer->bAffectAlpha) : ETint::Default;
	};

	TestTrue(TEXT("Before reorder: AlphaLayer resolves White"), ResolveByGuid(AlphaLayer) == ETint::White);
	TestTrue(TEXT("Before reorder: RedLayer resolves Red"), ResolveByGuid(RedLayer) == ETint::Red);

	TestTrue(TEXT("MoveLayerUp(RedLayer) succeeds"), Stack.MoveLayerUp(RedLayer));
	TestEqual(TEXT("RedLayer now first"), Stack.GetLayers()[0].LayerId, RedLayer);

	TestTrue(TEXT("After reorder: AlphaLayer STILL resolves White"), ResolveByGuid(AlphaLayer) == ETint::White);
	TestTrue(TEXT("After reorder: RedLayer STILL resolves Red"), ResolveByGuid(RedLayer) == ETint::Red);

	return true;
}

// 7. TintFailsSafelyForRemovedLayer: a removed GUID resolves to nullptr (the panel-side function's own
// contract maps that to Default), and the surviving layer's own tint is completely unaffected.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintRemovedTest, "VertexMaskForge.DynamicLayerChannelTint.TintFailsSafelyForRemovedLayer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintRemovedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Survivor = Stack.AddLayer(TEXT("Survivor"));
	Stack.SetLayerChannelFilter(Survivor, false, false, true, false); // Blue-only.
	const FGuid Removed = Stack.AddLayer(TEXT("Removed"));
	Stack.SetLayerChannelFilter(Removed, false, false, false, true); // Alpha-only (White), before removal.

	Stack.RemoveLayer(Removed);

	TestNull(TEXT("Removed layer is gone"), Stack.FindLayerById(Removed));

	const FVertexMaskForgeLayer* SurvivorLayer = Stack.FindLayerById(Survivor);
	TestNotNull(TEXT("Survivor still present"), SurvivorLayer);
	if (SurvivorLayer)
	{
		TestTrue(TEXT("Survivor still resolves its own Blue tint, unaffected by the removal"),
			ResolveDynamicLayerChannelTint(SurvivorLayer->bAffectRed, SurvivorLayer->bAffectGreen, SurvivorLayer->bAffectBlue, SurvivorLayer->bAffectAlpha) == ETint::Blue);
	}

	return true;
}

// 8. ChannelMutationUpdatesTintWithoutProduction: toggling a channel via the real controlled API changes
// the resolved tint kind immediately (no rebuild needed, since the decision is pure/stateless); this test
// only proves the domain-level state transition -- the "no production call" half of this requirement is
// proven by code inspection of SVertexMaskForgePanel.cpp's M16-K.4/M16-K.4A blocks (see this checkpoint's
// own report), not a runtime assertion, since there is no seam to observe "was RecomposeWorkingColors
// called" without constructing the panel.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintMutationTest, "VertexMaskForge.DynamicLayerChannelTint.ChannelMutationUpdatesTint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintMutationTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	// Defaults: R=G=B=true, A=false -> Default. M19-B: also confirm a fresh layer's Alpha-disabled state
	// never resolves to the new White tint (only Alpha-only ownership does).
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		TestTrue(TEXT("Default (RGB all true, A false) -> Default tint"), ResolveDynamicLayerChannelTint(Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue, Layer->bAffectAlpha) == ETint::Default);
		TestFalse(TEXT("M19-B: fresh layer never resolves to White"), ResolveDynamicLayerChannelTint(Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue, Layer->bAffectAlpha) == ETint::White);
	}

	// Simulate unchecking G and B (as two separate checkbox clicks, each preserving the other channels).
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		Stack.SetLayerChannelFilter(Id, Layer->bAffectRed, /*NewGreen=*/false, Layer->bAffectBlue, Layer->bAffectAlpha);
	}
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		Stack.SetLayerChannelFilter(Id, Layer->bAffectRed, Layer->bAffectGreen, /*NewBlue=*/false, Layer->bAffectAlpha);
	}

	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		TestTrue(TEXT("After unchecking G and B: Red-only -> Red tint"), ResolveDynamicLayerChannelTint(Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue, Layer->bAffectAlpha) == ETint::Red);
	}

	// M19-B: continue the same real-API mutation sequence -- uncheck R, check A -- reaching Alpha-only via
	// MANUAL toggles only (never Alt-click/the toggle helper) and confirm it resolves to White, exactly
	// like the Alt-click-produced state does (see the ChannelSolo test file's own AltClickResultMatchesExpectedTint
	// and the composition test suite's Alt-vs-manual parity coverage).
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		Stack.SetLayerChannelFilter(Id, /*NewRed=*/false, Layer->bAffectGreen, Layer->bAffectBlue, Layer->bAffectAlpha);
	}
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		Stack.SetLayerChannelFilter(Id, Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue, /*NewAlpha=*/true);
	}
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		TestTrue(TEXT("M19-B: manual R-off + A-on reaches Alpha-only -> White tint"), ResolveDynamicLayerChannelTint(Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue, Layer->bAffectAlpha) == ETint::White);
	}

	return true;
}

// 8b. M19-B. ManualAndAltClickAlphaOnlyResolveToSameTint: state-derived proof that the two DIFFERENT paths
// to Alpha-only ownership (Alt+click A, vs. manually disabling R/G/B then enabling A) produce IDENTICAL
// tint results -- since ResolveDynamicLayerChannelTint depends only on the four booleans, not on how they
// were reached, this also proves the checkpoint's own "these two paths must be visually identical" contract.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintManualVsAltAlphaParityTest, "VertexMaskForge.DynamicLayerChannelTint.ManualAndAltClickAlphaOnlyResolveToSameTint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintManualVsAltAlphaParityTest::RunTest(const FString& Parameters)
{
	using EChannel = EVertexMaskForgeDynamicLayerChannel;

	// Path 1: Alt+click A (via the real toggle helper) from a fresh RGB-enabled/Alpha-disabled layer.
	FVertexMaskForgeDynamicLayerStack AltStack;
	const FGuid AltId = AltStack.AddLayer(TEXT("AltLayer"));
	{
		const FVertexMaskForgeLayer* Layer = AltStack.FindLayerById(AltId);
		const FVertexMaskForgeDynamicLayerChannelToggleResult Result = ResolveDynamicLayerChannelToggle(
			Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue, Layer->bAffectAlpha,
			EChannel::Alpha, /*bRequestedChecked=*/true, /*bAltDown=*/true);
		AltStack.SetLayerChannelFilter(AltId, Result.bAffectRed, Result.bAffectGreen, Result.bAffectBlue, Result.bAffectAlpha);
	}

	// Path 2: manual isolation -- three separate normal-click-equivalent mutations (uncheck R, uncheck G,
	// uncheck B, check A), never Alt-click.
	FVertexMaskForgeDynamicLayerStack ManualStack;
	const FGuid ManualId = ManualStack.AddLayer(TEXT("ManualLayer"));
	ManualStack.SetLayerChannelFilter(ManualId, false, false, false, false); // R/G/B off in one call, mirrors three sequential unchecks' end state.
	ManualStack.SetLayerChannelFilter(ManualId, false, false, false, true); // then check A.

	const FVertexMaskForgeLayer* AltLayer = AltStack.FindLayerById(AltId);
	const FVertexMaskForgeLayer* ManualLayer = ManualStack.FindLayerById(ManualId);
	TestNotNull(TEXT("Alt-produced layer found"), AltLayer);
	TestNotNull(TEXT("Manually-produced layer found"), ManualLayer);
	if (AltLayer && ManualLayer)
	{
		TestEqual(TEXT("Both paths reach the identical channel state"), AltLayer->bAffectRed, ManualLayer->bAffectRed);
		TestEqual(TEXT("Both paths reach the identical channel state (G)"), AltLayer->bAffectGreen, ManualLayer->bAffectGreen);
		TestEqual(TEXT("Both paths reach the identical channel state (B)"), AltLayer->bAffectBlue, ManualLayer->bAffectBlue);
		TestEqual(TEXT("Both paths reach the identical channel state (A)"), AltLayer->bAffectAlpha, ManualLayer->bAffectAlpha);

		const ETint AltTint = ResolveDynamicLayerChannelTint(AltLayer->bAffectRed, AltLayer->bAffectGreen, AltLayer->bAffectBlue, AltLayer->bAffectAlpha);
		const ETint ManualTint = ResolveDynamicLayerChannelTint(ManualLayer->bAffectRed, ManualLayer->bAffectGreen, ManualLayer->bAffectBlue, ManualLayer->bAffectAlpha);
		TestTrue(TEXT("Alt-click A resolves to White"), AltTint == ETint::White);
		TestEqual(TEXT("Manual isolation resolves to the SAME tint as Alt-click"), static_cast<uint8>(ManualTint), static_cast<uint8>(AltTint));
	}

	return true;
}

// --- M16-K.4B: root-cause-fix-specific regressions ------------------------------------------------

// 9. GetDynamicLayerChannelTintColor returns the correct literal, fully-opaque-hue, low-alpha RGBA for
// each of the four exclusive-channel kinds -- this is the exact value BorderBackgroundColor now receives
// (multiplied against "WhiteBrush", i.e. rendered as-is) since the M16-K.4B fix, unlike M16-K.4A where an
// equivalent value was computed but rendered against a near-black brush and was never actually visible.
// M19-B: extended with White (Alpha-only), proving it shares the EXACT SAME TintAlpha constant as
// Red/Green/Blue -- no separate Alpha-specific opacity value exists anywhere in this call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintColorValuesTest, "VertexMaskForge.DynamicLayerChannelTint.TintColorLiteralValues", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintColorValuesTest::RunTest(const FString& Parameters)
{
	constexpr float TintAlpha = 0.15f;

	const FLinearColor RedColor = GetDynamicLayerChannelTintColor(ETint::Red, TintAlpha);
	TestEqual(TEXT("Red tint R"), RedColor.R, 1.0f);
	TestEqual(TEXT("Red tint G"), RedColor.G, 0.0f);
	TestEqual(TEXT("Red tint B"), RedColor.B, 0.0f);
	TestEqual(TEXT("Red tint A"), RedColor.A, TintAlpha);

	const FLinearColor GreenColor = GetDynamicLayerChannelTintColor(ETint::Green, TintAlpha);
	TestEqual(TEXT("Green tint R"), GreenColor.R, 0.0f);
	TestEqual(TEXT("Green tint G"), GreenColor.G, 1.0f);
	TestEqual(TEXT("Green tint B"), GreenColor.B, 0.0f);
	TestEqual(TEXT("Green tint A"), GreenColor.A, TintAlpha);

	const FLinearColor BlueColor = GetDynamicLayerChannelTintColor(ETint::Blue, TintAlpha);
	TestEqual(TEXT("Blue tint R"), BlueColor.R, 0.0f);
	TestEqual(TEXT("Blue tint G"), BlueColor.G, 0.0f);
	TestEqual(TEXT("Blue tint B"), BlueColor.B, 1.0f);
	TestEqual(TEXT("Blue tint A"), BlueColor.A, TintAlpha);

	const FLinearColor WhiteColor = GetDynamicLayerChannelTintColor(ETint::White, TintAlpha);
	TestEqual(TEXT("White tint R"), WhiteColor.R, 1.0f);
	TestEqual(TEXT("White tint G"), WhiteColor.G, 1.0f);
	TestEqual(TEXT("White tint B"), WhiteColor.B, 1.0f);
	TestEqual(TEXT("M19-B: White tint A uses the EXACT SAME TintAlpha as Red/Green/Blue -- no separate Alpha opacity constant"), WhiteColor.A, TintAlpha);
	TestEqual(TEXT("M19-B: White tint alpha byte-identical to Red tint alpha (shared opacity path)"), WhiteColor.A, RedColor.A);

	// None of the four are "gray"/desaturated -- Red/Green/Blue each have exactly one fully-saturated
	// primary channel and the other two exactly zero; White has all three fully saturated -- which is what
	// makes the M16-K.4A bug (imperceptible, near-gray output) impossible to reproduce from THESE values
	// alone -- the bug was in how they were rendered, not in what they were computed as (see this file's
	// own header comment).
	TestTrue(TEXT("Red is fully saturated, not desaturated"), RedColor.R == 1.0f && RedColor.G == 0.0f && RedColor.B == 0.0f);
	TestTrue(TEXT("White is fully saturated on all three RGB components"), WhiteColor.R == 1.0f && WhiteColor.G == 1.0f && WhiteColor.B == 1.0f);

	return true;
}

// 10. The M16-K.4B fix's load-bearing invariant: the row's default/neutral appearance MUST be
// FStyleColors::Panel, never FLinearColor::White -- because BuildDynamicLayerRow's SBorder now uses
// "WhiteBrush" (pure white TintColor) as its BorderImage, White*White would render as a SOLID WHITE ROW
// (replacing the entire baseline appearance), whereas White*Panel correctly reproduces the exact color
// SBorder's old default "Border" brush rendered. This test exists specifically to catch a regression back
// to the M16-K.4A defaulting mistake if GetDynamicLayerChannelTint's DefaultAppearance is ever
// accidentally changed back to FLinearColor::White. M19-B note: this invariant is exactly why an
// Alpha-only layer's white tint (a REAL, non-default ETint::White case) is visually distinguishable from
// the untinted/Default row -- Default never uses White*White either, so there is no ambiguity between "no
// channel isolated" and "Alpha isolated" at the rendering layer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintDefaultIsPanelNotWhiteTest, "VertexMaskForge.DynamicLayerChannelTint.DefaultAppearanceIsPanelNotWhite", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintDefaultIsPanelNotWhiteTest::RunTest(const FString& Parameters)
{
	const FLinearColor PanelColor = FStyleColors::Panel.GetSpecifiedColor();
	TestFalse(TEXT("FStyleColors::Panel must not be opaque white -- if it were, this whole distinction would be moot"),
		FMath::IsNearlyEqual(PanelColor.R, 1.0f) && FMath::IsNearlyEqual(PanelColor.G, 1.0f) && FMath::IsNearlyEqual(PanelColor.B, 1.0f) && FMath::IsNearlyEqual(PanelColor.A, 1.0f));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
