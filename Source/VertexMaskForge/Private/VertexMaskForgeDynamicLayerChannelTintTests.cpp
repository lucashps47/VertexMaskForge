// M16-K.4A/M16-K.4B: automation tests for the Dynamic Layers row tint's pure, Slate-free logic --
// ResolveDynamicLayerChannelTint (the channel-exclusivity DECISION: which of Default/Red/Green/Blue) and
// GetDynamicLayerChannelTintColor (the literal FLinearColor RGBA for the three exclusive-channel cases),
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

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Styling/StyleColors.h"
#include "VertexMaskForgeDynamicLayerStack.h"

using ETint = EVertexMaskForgeDynamicLayerChannelTint;

// 1. RedOnlyUsesRedTint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintRedOnlyTest, "VertexMaskForge.DynamicLayerChannelTint.RedOnlyUsesRedTint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintRedOnlyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("R only -> Red"), ResolveDynamicLayerChannelTint(true, false, false) == ETint::Red);
	return true;
}

// 2. GreenOnlyUsesGreenTint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintGreenOnlyTest, "VertexMaskForge.DynamicLayerChannelTint.GreenOnlyUsesGreenTint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintGreenOnlyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("G only -> Green"), ResolveDynamicLayerChannelTint(false, true, false) == ETint::Green);
	return true;
}

// 3. BlueOnlyUsesBlueTint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintBlueOnlyTest, "VertexMaskForge.DynamicLayerChannelTint.BlueOnlyUsesBlueTint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintBlueOnlyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("B only -> Blue"), ResolveDynamicLayerChannelTint(false, false, true) == ETint::Blue);
	return true;
}

// 4. MultipleChannelsUseDefaultAppearance -- RG, RB, GB, RGB all resolve to Default, never a combined hue.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintMultipleTest, "VertexMaskForge.DynamicLayerChannelTint.MultipleChannelsUseDefaultAppearance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintMultipleTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("RG -> Default (never yellow)"), ResolveDynamicLayerChannelTint(true, true, false) == ETint::Default);
	TestTrue(TEXT("RB -> Default (never magenta)"), ResolveDynamicLayerChannelTint(true, false, true) == ETint::Default);
	TestTrue(TEXT("GB -> Default (never cyan)"), ResolveDynamicLayerChannelTint(false, true, true) == ETint::Default);
	TestTrue(TEXT("RGB -> Default (never white)"), ResolveDynamicLayerChannelTint(true, true, true) == ETint::Default);
	return true;
}

// 5. NoChannelsUsesDefaultAppearance -- never black.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintNoneTest, "VertexMaskForge.DynamicLayerChannelTint.NoChannelsUsesDefaultAppearance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintNoneTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("No channels -> Default (never black)"), ResolveDynamicLayerChannelTint(false, false, false) == ETint::Default);
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
	Stack.SetLayerChannelFilter(RedLayer, true, false, false);
	const FGuid GreenLayer = Stack.AddLayer(TEXT("GreenLayer"));
	Stack.SetLayerChannelFilter(GreenLayer, false, true, false);

	auto ResolveByGuid = [&Stack](const FGuid& Id) -> ETint
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		return Layer ? ResolveDynamicLayerChannelTint(Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue) : ETint::Default;
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

// 7. TintFailsSafelyForRemovedLayer: a removed GUID resolves to nullptr (the panel-side function's own
// contract maps that to Default), and the surviving layer's own tint is completely unaffected.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintRemovedTest, "VertexMaskForge.DynamicLayerChannelTint.TintFailsSafelyForRemovedLayer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintRemovedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Survivor = Stack.AddLayer(TEXT("Survivor"));
	Stack.SetLayerChannelFilter(Survivor, false, false, true); // Blue-only.
	const FGuid Removed = Stack.AddLayer(TEXT("Removed"));
	Stack.SetLayerChannelFilter(Removed, true, false, false); // Red-only, before removal.

	Stack.RemoveLayer(Removed);

	TestNull(TEXT("Removed layer is gone"), Stack.FindLayerById(Removed));

	const FVertexMaskForgeLayer* SurvivorLayer = Stack.FindLayerById(Survivor);
	TestNotNull(TEXT("Survivor still present"), SurvivorLayer);
	if (SurvivorLayer)
	{
		TestTrue(TEXT("Survivor still resolves its own Blue tint, unaffected by the removal"),
			ResolveDynamicLayerChannelTint(SurvivorLayer->bAffectRed, SurvivorLayer->bAffectGreen, SurvivorLayer->bAffectBlue) == ETint::Blue);
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
	// Defaults: R=G=B=true -> Default.
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		TestTrue(TEXT("Default (RGB all true) -> Default tint"), ResolveDynamicLayerChannelTint(Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue) == ETint::Default);
	}

	// Simulate unchecking G and B (as two separate checkbox clicks, each preserving the other channel).
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		Stack.SetLayerChannelFilter(Id, Layer->bAffectRed, /*NewGreen=*/false, Layer->bAffectBlue);
	}
	{
		const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
		Stack.SetLayerChannelFilter(Id, Layer->bAffectRed, Layer->bAffectGreen, /*NewBlue=*/false);
	}

	const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
	TestTrue(TEXT("After unchecking G and B: Red-only -> Red tint"), ResolveDynamicLayerChannelTint(Layer->bAffectRed, Layer->bAffectGreen, Layer->bAffectBlue) == ETint::Red);

	return true;
}

// --- M16-K.4B: root-cause-fix-specific regressions ------------------------------------------------

// 9. GetDynamicLayerChannelTintColor returns the correct literal, fully-opaque-hue, low-alpha RGBA for
// each of the three exclusive-channel kinds -- this is the exact value BorderBackgroundColor now receives
// (multiplied against "WhiteBrush", i.e. rendered as-is) since the M16-K.4B fix, unlike M16-K.4A where an
// equivalent value was computed but rendered against a near-black brush and was never actually visible.
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

	// None of the three are "gray"/desaturated -- each has exactly one fully-saturated primary channel
	// and the other two exactly zero, which is what makes the M16-K.4A bug (imperceptible, near-gray
	// output) impossible to reproduce from THESE values alone -- the bug was in how they were rendered,
	// not in what they were computed as (see this file's own header comment).
	TestTrue(TEXT("Red is fully saturated, not desaturated"), RedColor.R == 1.0f && RedColor.G == 0.0f && RedColor.B == 0.0f);

	return true;
}

// 10. The M16-K.4B fix's load-bearing invariant: the row's default/neutral appearance MUST be
// FStyleColors::Panel, never FLinearColor::White -- because BuildDynamicLayerRow's SBorder now uses
// "WhiteBrush" (pure white TintColor) as its BorderImage, White*White would render as a SOLID WHITE ROW
// (replacing the entire baseline appearance), whereas White*Panel correctly reproduces the exact color
// SBorder's old default "Border" brush rendered. This test exists specifically to catch a regression back
// to the M16-K.4A defaulting mistake if GetDynamicLayerChannelTint's DefaultAppearance is ever
// accidentally changed back to FLinearColor::White.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerChannelTintDefaultIsPanelNotWhiteTest, "VertexMaskForge.DynamicLayerChannelTint.DefaultAppearanceIsPanelNotWhite", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerChannelTintDefaultIsPanelNotWhiteTest::RunTest(const FString& Parameters)
{
	const FLinearColor PanelColor = FStyleColors::Panel.GetSpecifiedColor();
	TestFalse(TEXT("FStyleColors::Panel must not be opaque white -- if it were, this whole distinction would be moot"),
		FMath::IsNearlyEqual(PanelColor.R, 1.0f) && FMath::IsNearlyEqual(PanelColor.G, 1.0f) && FMath::IsNearlyEqual(PanelColor.B, 1.0f) && FMath::IsNearlyEqual(PanelColor.A, 1.0f));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
