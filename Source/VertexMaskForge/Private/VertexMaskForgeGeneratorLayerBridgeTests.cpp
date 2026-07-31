// M16-J final: automation tests for VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential
// -- the pure adapter between the panel's existing FVertexMaskForgeMaskLayerParams array and
// VertexMaskForgeSequentialEvaluator::EvaluateFillLayers. Every expected value below is derived by hand
// from the real, verbatim formulas already proven in VertexMaskForgeSequentialEvaluatorTests.cpp (this
// file adds no new blend math of its own -- it only proves the bridge resolves/forwards correctly).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeGeneratorLayerBridge.h"
#include "VertexMaskForgeMaskStackComposer.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	constexpr float GLB_Tolerance = 1e-4f;

	FVertexMaskForgeScalarMask MakeMask(const TArray<float>& Values)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.State = EVertexMaskForgeScalarMaskState::Ready;
		Mask.Values = Values;
		Mask.bHasValue.Init(true, Values.Num());
		Mask.NumValidValues = Values.Num();
		return Mask;
	}
}

// A. Empty Layers -> CommittedColor passthrough (R/G/B), Alpha == BaselineColor.W, bOutAnyLayerContributed
// == false. Mirrors the legacy ComposeStack's own empty-Layers contract.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorLayerBridgeEmptyTest, "VertexMaskForge.GeneratorLayerBridge.EmptyLayers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorLayerBridgeEmptyTest::RunTest(const FString& Parameters)
{
	const FVector4f Baseline(0.2f, 0.3f, 0.4f, 0.9f);
	const FVector4f Committed(0.5f, 0.6f, 0.7f, 1.0f);

	bool bAnyContributed = true;
	const FVector4f Result = VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential(
		Baseline, Committed, 0, TArrayView<const VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams>(),
		true, true, true, bAnyContributed);

	TestFalse(TEXT("bOutAnyLayerContributed false for empty Layers"), bAnyContributed);
	TestTrue(TEXT("R/G/B verbatim from CommittedColor"), FMath::IsNearlyEqual(Result.X, 0.5f, GLB_Tolerance)
		&& FMath::IsNearlyEqual(Result.Y, 0.6f, GLB_Tolerance) && FMath::IsNearlyEqual(Result.Z, 0.7f, GLB_Tolerance));
	TestNearlyEqual(TEXT("Alpha == BaselineColor.W"), Result.W, 0.9f, GLB_Tolerance);

	return true;
}

// B. A single layer whose Mask has no value at VertexIndex (TryGetValue fails) is skipped -- identical to
// an empty Layers array, never a hard failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorLayerBridgeUnresolvedSkipTest, "VertexMaskForge.GeneratorLayerBridge.UnresolvedLayerSkipped", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorLayerBridgeUnresolvedSkipTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeScalarMask SparseMask = MakeMask({}); // Empty -- TryGetValue(0, ...) fails.
	const FVertexMaskForgeScalarMask* NullMask = nullptr;

	TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
	Layers.Add({ &SparseMask, EVertexMaskForgeBlendMode::Copy, 1.0f, -1 });
	Layers.Add({ NullMask, EVertexMaskForgeBlendMode::Add, 1.0f, -1 });

	const FVector4f Baseline(0.2f, 0.2f, 0.2f, 1.0f);
	const FVector4f Committed(0.6f, 0.6f, 0.6f, 1.0f);

	bool bAnyContributed = true;
	const FVector4f Result = VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential(
		Baseline, Committed, 0, Layers, true, true, true, bAnyContributed);

	TestFalse(TEXT("bOutAnyLayerContributed false -- both layers unresolved"), bAnyContributed);
	TestNearlyEqual(TEXT("R passthrough from CommittedColor"), Result.X, 0.6f, GLB_Tolerance);

	return true;
}

// C. Single-layer numeric verification, all 7 Blend Modes, mask/opacity 0/1/intermediate -- proves the
// bridge's implicit-white-FillValue contract: PaintValue == the generator's own resolved scalar,
// broadcast to RGB.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorLayerBridgeSingleLayerNumericTest, "VertexMaskForge.GeneratorLayerBridge.SingleLayerNumeric", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorLayerBridgeSingleLayerNumericTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeMaskStackComposer;

	const EVertexMaskForgeBlendMode AllModes[] = {
		EVertexMaskForgeBlendMode::Copy, EVertexMaskForgeBlendMode::Add, EVertexMaskForgeBlendMode::Subtract,
		EVertexMaskForgeBlendMode::Multiply, EVertexMaskForgeBlendMode::Overlay, EVertexMaskForgeBlendMode::Screen,
		EVertexMaskForgeBlendMode::Linear
	};
	const float OpacityValues[] = { 0.0f, 1.0f, 0.5f };
	const float MaskValues[] = { 0.0f, 1.0f, 0.6f };

	const FVector4f Baseline(0.35f, 0.35f, 0.35f, 1.0f);
	const FVector4f Committed(0.9f, 0.9f, 0.9f, 1.0f);

	for (const EVertexMaskForgeBlendMode Mode : AllModes)
	{
		for (const float Opacity : OpacityValues)
		{
			for (const float MaskValue : MaskValues)
			{
				const FVertexMaskForgeScalarMask Mask = MakeMask({ MaskValue });
				TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
				Layers.Add({ &Mask, Mode, Opacity, -1 });

				bool bAnyContributed = false;
				const FVector4f BridgeResult = VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential(
					Baseline, Committed, 0, Layers, true, true, true, bAnyContributed);
				TestTrue(TEXT("bOutAnyLayerContributed true"), bAnyContributed);

				// Oracle: BlendMaskValueUnclamped(Base, MaskValue, Mode, Opacity), then Clamp01 once --
				// exactly what the sequential evaluator does for a single white-fill Fill Layer (see
				// VertexMaskForgeSequentialEvaluatorTests.cpp Section J for the proof this also matches
				// the legacy ComposeStack for a single layer).
				const float Expected = FMath::Clamp(BlendMaskValueUnclamped(0.35f, MaskValue, Mode, Opacity), 0.0f, 1.0f);

				TestNearlyEqual(
					FString::Printf(TEXT("mode=%d Opacity=%.2f Mask=%.2f"), (int32)Mode, Opacity, MaskValue),
					BridgeResult.X, Expected, GLB_Tolerance);
			}
		}
	}

	return true;
}

// D. Strict array-order fold, cross-mode, no Blend-Mode regrouping -- two layers whose order changes the
// result (Copy then Multiply != Multiply then Copy), proving the bridge threads SortedLayers straight
// into the sequential fold without re-sorting or stage-grouping.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorLayerBridgeOrderTest, "VertexMaskForge.GeneratorLayerBridge.StrictArrayOrder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorLayerBridgeOrderTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeScalarMask CopyMask = MakeMask({ 0.4f });
	const FVertexMaskForgeScalarMask MultiplyMask = MakeMask({ 0.5f });

	const VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams CopyLayer = { &CopyMask, EVertexMaskForgeBlendMode::Copy, 1.0f, -1 };
	const VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams MultiplyLayer = { &MultiplyMask, EVertexMaskForgeBlendMode::Multiply, 1.0f, -1 };

	const FVector4f Baseline(0.2f, 0.2f, 0.2f, 1.0f);
	const FVector4f Committed(0.2f, 0.2f, 0.2f, 1.0f);

	// Order [Copy, Multiply]: Base=0.2 -> Copy(0.4) -> 0.4 (Copy ignores Base at Opacity=1) ->
	// Multiply(0.5) -> Lerp(0.4, 0.4*0.5=0.2, 1) = 0.2.
	{
		TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
		Layers.Add(CopyLayer);
		Layers.Add(MultiplyLayer);
		bool bAnyContributed = false;
		const FVector4f Result = VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential(
			Baseline, Committed, 0, Layers, true, true, true, bAnyContributed);
		TestNearlyEqual(TEXT("Order [Copy, Multiply] -> 0.2"), Result.X, 0.2f, GLB_Tolerance);
	}

	// Order [Multiply, Copy]: Base=0.2 -> Multiply(0.5) -> Lerp(0.2, 0.2*0.5=0.1, 1) = 0.1 -> Copy(0.4)
	// -> 0.4 (Copy ignores the 0.1 accumulator).
	{
		TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
		Layers.Add(MultiplyLayer);
		Layers.Add(CopyLayer);
		bool bAnyContributed = false;
		const FVector4f Result = VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential(
			Baseline, Committed, 0, Layers, true, true, true, bAnyContributed);
		TestNearlyEqual(TEXT("Order [Multiply, Copy] -> 0.4"), Result.X, 0.4f, GLB_Tolerance);
	}

	return true;
}

// E. Channel Filter -- a disabled channel is read verbatim from CommittedColor, untouched by any layer,
// even when that layer resolves and contributes for the OTHER (enabled) channels.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorLayerBridgeChannelFilterTest, "VertexMaskForge.GeneratorLayerBridge.ChannelFilter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorLayerBridgeChannelFilterTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeScalarMask Mask = MakeMask({ 1.0f }); // Copy(1.0) -> fully replaces with 1.0.
	TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
	Layers.Add({ &Mask, EVertexMaskForgeBlendMode::Copy, 1.0f, -1 });

	const FVector4f Baseline(0.2f, 0.2f, 0.2f, 1.0f);
	const FVector4f Committed(0.5f, 0.6f, 0.7f, 1.0f);

	// Only G enabled -- R and B must stay exactly at CommittedColor's own values; G becomes the computed 1.0.
	bool bAnyContributed = false;
	const FVector4f Result = VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential(
		Baseline, Committed, 0, Layers, /*bFilterR=*/false, /*bFilterG=*/true, /*bFilterB=*/false, bAnyContributed);

	TestTrue(TEXT("bOutAnyLayerContributed true (independent of Channel Filter)"), bAnyContributed);
	TestNearlyEqual(TEXT("R untouched (CommittedColor.X)"), Result.X, 0.5f, GLB_Tolerance);
	TestNearlyEqual(TEXT("G computed (Copy 1.0)"), Result.Y, 1.0f, GLB_Tolerance);
	TestNearlyEqual(TEXT("B untouched (CommittedColor.Z)"), Result.Z, 0.7f, GLB_Tolerance);

	return true;
}

// F. IndexOverride resolution -- a layer with IndexOverride >= 0 is looked up at that index instead of
// the shared VertexIndex, exactly like the legacy ComposeMaskStack.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorLayerBridgeIndexOverrideTest, "VertexMaskForge.GeneratorLayerBridge.IndexOverride", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorLayerBridgeIndexOverrideTest::RunTest(const FString& Parameters)
{
	// Index 0 -> 0.2, Index 5 -> 0.9 -- deliberately distinguishable.
	const FVertexMaskForgeScalarMask Mask = MakeMask({ 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.9f });

	const FVector4f Baseline(0.1f, 0.1f, 0.1f, 1.0f);
	const FVector4f Committed(0.1f, 0.1f, 0.1f, 1.0f);

	// VertexIndex=0, but IndexOverride=5 -- must read 0.9, not 0.2.
	{
		TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
		Layers.Add({ &Mask, EVertexMaskForgeBlendMode::Copy, 1.0f, /*IndexOverride=*/5 });
		bool bAnyContributed = false;
		const FVector4f Result = VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential(
			Baseline, Committed, /*VertexIndex=*/0, Layers, true, true, true, bAnyContributed);
		TestNearlyEqual(TEXT("IndexOverride=5 reads Values[5]=0.9, ignoring VertexIndex=0"), Result.X, 0.9f, GLB_Tolerance);
	}

	// No IndexOverride (-1) -- falls back to the shared VertexIndex=5.
	{
		TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
		Layers.Add({ &Mask, EVertexMaskForgeBlendMode::Copy, 1.0f, /*IndexOverride=*/-1 });
		bool bAnyContributed = false;
		const FVector4f Result = VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential(
			Baseline, Committed, /*VertexIndex=*/5, Layers, true, true, true, bAnyContributed);
		TestNearlyEqual(TEXT("No IndexOverride falls back to VertexIndex=5, reads Values[5]=0.9"), Result.X, 0.9f, GLB_Tolerance);
	}

	return true;
}

// G. Alpha is never touched by the bridge -- always exactly BaselineColor.W, regardless of layers,
// Channel Filter, or CommittedColor's own Alpha.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorLayerBridgeAlphaTest, "VertexMaskForge.GeneratorLayerBridge.AlphaUntouched", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorLayerBridgeAlphaTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeScalarMask Mask = MakeMask({ 0.7f });
	TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
	Layers.Add({ &Mask, EVertexMaskForgeBlendMode::Add, 1.0f, -1 });

	const FVector4f Baseline(0.2f, 0.2f, 0.2f, 0.42f);
	const FVector4f Committed(0.2f, 0.2f, 0.2f, 0.99f); // Deliberately different Alpha -- must be ignored.

	bool bAnyContributed = false;
	const FVector4f Result = VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential(
		Baseline, Committed, 0, Layers, true, true, true, bAnyContributed);

	TestNearlyEqual(TEXT("Alpha == BaselineColor.W, never CommittedColor.W"), Result.W, 0.42f, GLB_Tolerance);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
