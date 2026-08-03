// M16-K.6D-2 (correction): proves the pure, directly-testable core of the Source-Topology preview seam
// -- VertexMaskForgePanel::DeriveValidatedSourceTopologyPreviewColors (declared in
// VertexMaskForgeMaskTypes.h, defined in SVertexMaskForgePanel.cpp) -- the exact, unmodified function
// ApplySuppliedSourceTopologyPreviewColors itself calls for its own cardinality-gate-then-derive step.
//
// Scope note (Slate/render-resource boundary): ApplySuppliedSourceTopologyPreviewColors' own remaining
// responsibilities (EnsureSourceTopologyPreviewComponent, ApplySourceTopologyColorsToPreviewComponent,
// Actor-hide bookkeeping) require a real UDynamicMeshComponent/AActor/UWorld and are not covered by any
// automation test in this codebase (no test anywhere constructs SVertexMaskForgePanel or a live preview
// component -- confirmed by symbol search). These tests instead prove, directly and without any Slate/
// render dependency, the two behaviors the M16-K.6D-2 correction was specifically required to fix:
// (1) Preview Mode display derivation happens from a semantically-composed input, not a pre-reduced one;
// (2) cardinality mismatch returns an explicit failure with zero output, never padding/truncation.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeMaskTypes.h"

// 1. Valid cardinality, RGBVertexColor (falls to ApplyPreviewModeDisplay's default: passthrough):
// output must be set and byte-identical to the input (linear round-trip through /255 and *255+round is
// exact for integer FColor inputs -- see VertexMaskForgeColorConversion).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeSourceTopologyPreviewSeamRGBPassthroughTest, "VertexMaskForge.SourceTopologyPreviewSeam.ValidCardinalityRGBVertexColorPassthrough", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeSourceTopologyPreviewSeamRGBPassthroughTest::RunTest(const FString& Parameters)
{
	const TArray<FColor> SemanticComposedColors = {
		FColor(10, 20, 30, 255),
		FColor(200, 100, 50, 128),
		FColor(0, 255, 0, 64),
	};

	const TOptional<TArray<FColor>> Result = VertexMaskForgePanel::DeriveValidatedSourceTopologyPreviewColors(
		SemanticComposedColors, /*ExpectedCornerCount=*/3, EVertexMaskForgePreviewMode::RGBVertexColor);

	TestTrue(TEXT("Result is set for valid cardinality"), Result.IsSet());
	if (!Result.IsSet())
	{
		return false;
	}

	TestEqual(TEXT("Result.Num() == input.Num()"), Result.GetValue().Num(), SemanticComposedColors.Num());
	for (int32 Index = 0; Index < SemanticComposedColors.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("RGBVertexColor[%d] byte-identical to semantic input"), Index),
			Result.GetValue()[Index], SemanticComposedColors[Index]);
	}

	// AUDITED: input array itself is never mutated (taken by const&) -- re-check the original values.
	TestEqual(TEXT("Input[0] unchanged"), SemanticComposedColors[0], FColor(10, 20, 30, 255));
	TestEqual(TEXT("Input[1] unchanged"), SemanticComposedColors[1], FColor(200, 100, 50, 128));
	TestEqual(TEXT("Input[2] unchanged"), SemanticComposedColors[2], FColor(0, 255, 0, 64));

	return true;
}

// 2. Valid cardinality, RedChannel: proves Preview Mode derivation genuinely happens INSIDE this
// function from the semantic (pre-reduction) input -- the exact defect (Bloqueador 1) the M16-K.6D-2
// rejected attempt left uncorrected (it received an already-reduced array from its caller instead).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeSourceTopologyPreviewSeamRedChannelDerivationTest, "VertexMaskForge.SourceTopologyPreviewSeam.ValidCardinalityRedChannelIsolatesRedFromSemanticInput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeSourceTopologyPreviewSeamRedChannelDerivationTest::RunTest(const FString& Parameters)
{
	const TArray<FColor> SemanticComposedColors = { FColor(10, 20, 30, 255) };

	const TOptional<TArray<FColor>> Result = VertexMaskForgePanel::DeriveValidatedSourceTopologyPreviewColors(
		SemanticComposedColors, /*ExpectedCornerCount=*/1, EVertexMaskForgePreviewMode::RedChannel);

	TestTrue(TEXT("Result is set"), Result.IsSet());
	if (!Result.IsSet() || Result.GetValue().Num() != 1)
	{
		return false;
	}

	// AUDITED: RedChannel reduces to (R, R, R, 1) -- see ApplyPreviewModeDisplay. R=10 -> Alpha forced
	// to 255 (Alpha isolation is a SEPARATE mode, AlphaChannel, tested below).
	TestEqual(TEXT("RedChannel result"), Result.GetValue()[0], FColor(10, 10, 10, 255));

	return true;
}

// 3. Valid cardinality, GreenChannel/BlueChannel/AlphaChannel: each isolates its own channel, proving no
// channel confusion in the derivation this seam now performs internally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeSourceTopologyPreviewSeamOtherChannelsDerivationTest, "VertexMaskForge.SourceTopologyPreviewSeam.ValidCardinalityGreenBlueAlphaChannelsIsolateCorrectly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeSourceTopologyPreviewSeamOtherChannelsDerivationTest::RunTest(const FString& Parameters)
{
	const TArray<FColor> SemanticComposedColors = { FColor(10, 20, 30, 200) };

	const TOptional<TArray<FColor>> GreenResult = VertexMaskForgePanel::DeriveValidatedSourceTopologyPreviewColors(
		SemanticComposedColors, 1, EVertexMaskForgePreviewMode::GreenChannel);
	const TOptional<TArray<FColor>> BlueResult = VertexMaskForgePanel::DeriveValidatedSourceTopologyPreviewColors(
		SemanticComposedColors, 1, EVertexMaskForgePreviewMode::BlueChannel);
	const TOptional<TArray<FColor>> AlphaResult = VertexMaskForgePanel::DeriveValidatedSourceTopologyPreviewColors(
		SemanticComposedColors, 1, EVertexMaskForgePreviewMode::AlphaChannel);

	if (!TestTrue(TEXT("GreenResult set"), GreenResult.IsSet() && GreenResult.GetValue().Num() == 1)
		|| !TestTrue(TEXT("BlueResult set"), BlueResult.IsSet() && BlueResult.GetValue().Num() == 1)
		|| !TestTrue(TEXT("AlphaResult set"), AlphaResult.IsSet() && AlphaResult.GetValue().Num() == 1))
	{
		return false;
	}

	TestEqual(TEXT("GreenChannel result"), GreenResult.GetValue()[0], FColor(20, 20, 20, 255));
	TestEqual(TEXT("BlueChannel result"), BlueResult.GetValue()[0], FColor(30, 30, 30, 255));
	TestEqual(TEXT("AlphaChannel result"), AlphaResult.GetValue()[0], FColor(200, 200, 200, 255));

	return true;
}

// 4. Cardinality shorter than required: explicit failure, no padding, no partial output. This is the
// exact Bloqueador 2 defect -- the M16-K.6D-2 rejected attempt's lower-level helper silently padded a
// short array with FColor::White; this function (the seam's own validated entry point) must not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeSourceTopologyPreviewSeamShortCardinalityFailsTest, "VertexMaskForge.SourceTopologyPreviewSeam.CardinalityTooShortReturnsUnsetNoPadding", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeSourceTopologyPreviewSeamShortCardinalityFailsTest::RunTest(const FString& Parameters)
{
	// One triangle == 3 corners expected; only 2 colors supplied.
	const TArray<FColor> SemanticComposedColors = { FColor(10, 20, 30, 255), FColor(40, 50, 60, 255) };

	const TOptional<TArray<FColor>> Result = VertexMaskForgePanel::DeriveValidatedSourceTopologyPreviewColors(
		SemanticComposedColors, /*ExpectedCornerCount=*/3, EVertexMaskForgePreviewMode::RGBVertexColor);

	TestFalse(TEXT("Result is unset for a too-short input -- no derivation attempted, no padding"), Result.IsSet());

	return true;
}

// 5. Cardinality longer than required: also an explicit failure, never silently truncated.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeSourceTopologyPreviewSeamLongCardinalityFailsTest, "VertexMaskForge.SourceTopologyPreviewSeam.CardinalityTooLongReturnsUnsetNoTruncation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeSourceTopologyPreviewSeamLongCardinalityFailsTest::RunTest(const FString& Parameters)
{
	// One triangle == 3 corners expected; 4 colors supplied.
	const TArray<FColor> SemanticComposedColors = {
		FColor(10, 20, 30, 255), FColor(40, 50, 60, 255), FColor(70, 80, 90, 255), FColor(1, 2, 3, 255),
	};

	const TOptional<TArray<FColor>> Result = VertexMaskForgePanel::DeriveValidatedSourceTopologyPreviewColors(
		SemanticComposedColors, /*ExpectedCornerCount=*/3, EVertexMaskForgePreviewMode::RGBVertexColor);

	TestFalse(TEXT("Result is unset for a too-long input -- no derivation attempted, no truncation"), Result.IsSet());

	return true;
}

// 6. Zero expected corners (degenerate but well-defined: an empty mesh) with an empty input succeeds
// with an empty (but SET) result -- proves the all-or-nothing gate is an exact equality check, not an
// off-by-one/">="-style check that could silently accept a short array.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeSourceTopologyPreviewSeamZeroCardinalityTest, "VertexMaskForge.SourceTopologyPreviewSeam.ZeroExpectedCornersWithEmptyInputSucceeds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeSourceTopologyPreviewSeamZeroCardinalityTest::RunTest(const FString& Parameters)
{
	const TArray<FColor> SemanticComposedColors;

	const TOptional<TArray<FColor>> Result = VertexMaskForgePanel::DeriveValidatedSourceTopologyPreviewColors(
		SemanticComposedColors, /*ExpectedCornerCount=*/0, EVertexMaskForgePreviewMode::RGBVertexColor);

	TestTrue(TEXT("Result is set for exact zero/zero match"), Result.IsSet());
	if (Result.IsSet())
	{
		TestEqual(TEXT("Result is empty"), Result.GetValue().Num(), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
