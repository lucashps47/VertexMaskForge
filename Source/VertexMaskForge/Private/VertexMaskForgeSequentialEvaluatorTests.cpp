// M16-B: mathematical, deterministic automation tests for the new sequential evaluators
// (VertexMaskForgeSequentialEvaluator.h/.cpp) and for characterizing/protecting the legacy
// VertexMaskForgeMaskStackComposer::ComposeStack algorithm across the ApplyMaskBlendMode/
// BlendMaskValueUnclamped linkage change made in this same checkpoint. Every expected value below was
// computed by hand directly from the real, verbatim function bodies (see the M15/M15.1/M15.2/M15.2.1
// audits' own derivations) -- never assumed from a name or a comment.
//
// AUDITED: NaN/Infinity handling is deliberately NOT exercised or asserted here -- Lerp(a, NaN, 0) ==
// a is NOT a floating-point guarantee, and no such claim is made or tested (M15.2.1's own correction).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeMaskStackComposer.h"
#include "VertexMaskForgeSequentialEvaluator.h"

namespace
{
	constexpr float VMF_Tolerance = 1e-4f;
}

// =================================================================================================
// A. Mask Stack -- seed and empty stack
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaskStackSeedTest, "VertexMaskForge.SequentialEvaluator.MaskStack.SeedAndEmpty", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaskStackSeedTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	// 1. Empty array -> 1.0.
	{
		const float Result = EvaluateMaskStack(TConstArrayView<FVertexMaskForgeMaskEvaluationInput>());
		TestEqual(TEXT("Empty stack"), Result, 1.0f);
	}

	// 2. All entries disabled -> 1.0.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.75f, EVertexMaskForgeBlendMode::Copy, 1.0f, /*bEnabled=*/false });
		Inputs.Add({ 0.3f, EVertexMaskForgeBlendMode::Multiply, 1.0f, /*bEnabled=*/false });
		const float Result = EvaluateMaskStack(Inputs);
		TestEqual(TEXT("All-disabled stack"), Result, 1.0f);
	}

	// 3. Single Copy 0.75 -> 0.75.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.75f, EVertexMaskForgeBlendMode::Copy, 1.0f, true });
		TestNearlyEqual(TEXT("Single Copy 0.75"), EvaluateMaskStack(Inputs), 0.75f, VMF_Tolerance);
	}

	// 4. Single Multiply 0.75 on seed 1 -> 0.75 (Base=1 makes Multiply degenerate to MaskValue itself).
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.75f, EVertexMaskForgeBlendMode::Multiply, 1.0f, true });
		TestNearlyEqual(TEXT("Single Multiply 0.75 on seed 1"), EvaluateMaskStack(Inputs), 0.75f, VMF_Tolerance);
	}

	// 5. Single Subtract 0.75 on seed 1 -> 0.25.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.75f, EVertexMaskForgeBlendMode::Subtract, 1.0f, true });
		TestNearlyEqual(TEXT("Single Subtract 0.75 on seed 1"), EvaluateMaskStack(Inputs), 0.25f, VMF_Tolerance);
	}

	return true;
}

// =================================================================================================
// B. Mask Stack -- MaskOpacity semantics, all 7 Blend Modes
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaskStackOpacityTest, "VertexMaskForge.SequentialEvaluator.MaskStack.Opacity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaskStackOpacityTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	const EVertexMaskForgeBlendMode AllModes[] = {
		EVertexMaskForgeBlendMode::Copy, EVertexMaskForgeBlendMode::Add, EVertexMaskForgeBlendMode::Subtract,
		EVertexMaskForgeBlendMode::Multiply, EVertexMaskForgeBlendMode::Overlay, EVertexMaskForgeBlendMode::Screen,
		EVertexMaskForgeBlendMode::Linear
	};

	// Opacity=0 preserves the prior accumulator exactly, for every mode, from an arbitrary (non-seed)
	// starting Result.
	for (const EVertexMaskForgeBlendMode Mode : AllModes)
	{
		const float Result = EvaluateMaskStep(/*Result=*/0.5f, /*MaskValue=*/0.75f, Mode, /*Opacity=*/0.0f);
		TestEqual(FString::Printf(TEXT("Opacity=0 preserves accumulator, mode=%d"), (int32)Mode), Result, 0.5f);
	}

	// Single Mask Instance, seed=1, Opacity=0 -> exactly 1.0, for every mode.
	for (const EVertexMaskForgeBlendMode Mode : AllModes)
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.75f, Mode, 0.0f, true });
		TestEqual(FString::Printf(TEXT("Single instance seed=1 Opacity=0 exact 1.0, mode=%d"), (int32)Mode), EvaluateMaskStack(Inputs), 1.0f);
	}

	// Disabled produces the same result as removing the entry entirely.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> WithDisabled;
		WithDisabled.Add({ 0.6f, EVertexMaskForgeBlendMode::Copy, 1.0f, true });
		WithDisabled.Add({ 0.9f, EVertexMaskForgeBlendMode::Multiply, 0.7f, /*bEnabled=*/false });

		TArray<FVertexMaskForgeMaskEvaluationInput> WithoutEntry;
		WithoutEntry.Add({ 0.6f, EVertexMaskForgeBlendMode::Copy, 1.0f, true });

		TestNearlyEqual(TEXT("Disabled entry == removed entry"), EvaluateMaskStack(WithDisabled), EvaluateMaskStack(WithoutEntry), VMF_Tolerance);
	}

	// Opacity=1 corresponds fully to the raw operation, for every mode.
	for (const EVertexMaskForgeBlendMode Mode : AllModes)
	{
		const float Expected = FMath::Clamp(VertexMaskForgeMaskStackComposer::ApplyMaskBlendMode(0.4f, 0.6f, Mode), 0.0f, 1.0f);
		const float Actual = EvaluateMaskStep(/*Result=*/0.4f, /*MaskValue=*/0.6f, Mode, /*Opacity=*/1.0f);
		TestNearlyEqual(FString::Printf(TEXT("Opacity=1 full application, mode=%d"), (int32)Mode), Actual, Expected, VMF_Tolerance);
	}

	// Continuity near zero: seed=1, single Copy 0.25, Opacity=0.001 -> 0.99925 exactly.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.25f, EVertexMaskForgeBlendMode::Copy, 0.001f, true });
		TestNearlyEqual(TEXT("Continuity near Opacity=0"), EvaluateMaskStack(Inputs), 0.99925f, VMF_Tolerance);
	}

	// Explicit negative case: Enabled vs. Disabled are NOT required to match when the mask has real
	// effect -- Enable/Disable is a discrete change, not a continuous one (M15.2.1's own correction).
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Enabled;
		Enabled.Add({ 0.75f, EVertexMaskForgeBlendMode::Copy, 1.0f, true });
		TArray<FVertexMaskForgeMaskEvaluationInput> Disabled;
		Disabled.Add({ 0.75f, EVertexMaskForgeBlendMode::Copy, 1.0f, false });

		const float EnabledResult = EvaluateMaskStack(Enabled);
		const float DisabledResult = EvaluateMaskStack(Disabled);
		TestNearlyEqual(TEXT("Enabled Copy 0.75 result"), EnabledResult, 0.75f, VMF_Tolerance);
		TestEqual(TEXT("Disabled-only stack result"), DisabledResult, 1.0f);
		TestTrue(TEXT("Enabled != Disabled when mask has real effect"), !FMath::IsNearlyEqual(EnabledResult, DisabledResult, VMF_Tolerance));
	}

	return true;
}

// =================================================================================================
// C. Mask Stack -- reorder sensitivity
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaskStackReorderTest, "VertexMaskForge.SequentialEvaluator.MaskStack.Reorder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaskStackReorderTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	// Order A: Copy 0.25 then Add 0.50 -> 0.75.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.25f, EVertexMaskForgeBlendMode::Copy, 1.0f, true });
		Inputs.Add({ 0.50f, EVertexMaskForgeBlendMode::Add, 1.0f, true });
		TestNearlyEqual(TEXT("Copy then Add"), EvaluateMaskStack(Inputs), 0.75f, VMF_Tolerance);
	}

	// Order B: Add 0.50 then Copy 0.25 -> 0.25.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.50f, EVertexMaskForgeBlendMode::Add, 1.0f, true });
		Inputs.Add({ 0.25f, EVertexMaskForgeBlendMode::Copy, 1.0f, true });
		TestNearlyEqual(TEXT("Add then Copy"), EvaluateMaskStack(Inputs), 0.25f, VMF_Tolerance);
	}

	return true;
}

// =================================================================================================
// D. Mask Stack -- per-step clamp policy
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaskStackClampTest, "VertexMaskForge.SequentialEvaluator.MaskStack.Clamp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaskStackClampTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	// Add (saturates above 1) then Multiply -> 0.5 with per-step clamp (would be 0.75 without it).
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.5f, EVertexMaskForgeBlendMode::Add, 1.0f, true });
		Inputs.Add({ 0.5f, EVertexMaskForgeBlendMode::Multiply, 1.0f, true });
		TestNearlyEqual(TEXT("Add-saturate then Multiply"), EvaluateMaskStack(Inputs), 0.5f, VMF_Tolerance);
	}

	// Two Subtracts (drives below 0) then Screen -> 0.5 with per-step clamp (would be 0.4 without it).
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.6f, EVertexMaskForgeBlendMode::Subtract, 1.0f, true });
		Inputs.Add({ 0.6f, EVertexMaskForgeBlendMode::Subtract, 1.0f, true });
		Inputs.Add({ 0.5f, EVertexMaskForgeBlendMode::Screen, 1.0f, true });
		TestNearlyEqual(TEXT("Subtract-Subtract-below-zero then Screen"), EvaluateMaskStack(Inputs), 0.5f, VMF_Tolerance);
	}

	// Add (saturates to 1) then partial-opacity Copy -> 0.65 with per-step clamp (would be 1.0 without
	// it, since Copy at Opacity=0.5 would lerp toward 0.3 from an unclamped 2.0 instead of a clamped 1.0).
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 1.0f, EVertexMaskForgeBlendMode::Add, 1.0f, true });
		Inputs.Add({ 0.3f, EVertexMaskForgeBlendMode::Copy, 0.5f, true });
		TestNearlyEqual(TEXT("Add-saturate then partial Copy"), EvaluateMaskStack(Inputs), 0.65f, VMF_Tolerance);
	}

	return true;
}

// =================================================================================================
// E. Fill Layer -- passthrough guarantees, all 7 Blend Modes
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerPassthroughTest, "VertexMaskForge.SequentialEvaluator.FillLayer.Passthrough", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerPassthroughTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	const FVector3f Base(0.2f, 0.2f, 0.2f);
	const FVector3f Fill(1.0f, 1.0f, 1.0f);
	const EVertexMaskForgeBlendMode AllModes[] = {
		EVertexMaskForgeBlendMode::Copy, EVertexMaskForgeBlendMode::Add, EVertexMaskForgeBlendMode::Subtract,
		EVertexMaskForgeBlendMode::Multiply, EVertexMaskForgeBlendMode::Overlay, EVertexMaskForgeBlendMode::Screen,
		EVertexMaskForgeBlendMode::Linear
	};

	for (const EVertexMaskForgeBlendMode Mode : AllModes)
	{
		// EffectiveMask=0 -> passthrough.
		{
			TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
			Inputs.Add({ Fill, Mode, 1.0f, true, /*EffectiveMask=*/0.0f });
			const FVector3f Result = EvaluateFillLayers(Base, Inputs);
			TestTrue(FString::Printf(TEXT("EffectiveMask=0 passthrough, mode=%d"), (int32)Mode), Result.Equals(Base, VMF_Tolerance));
		}
		// LayerOpacity=0 -> passthrough.
		{
			TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
			Inputs.Add({ Fill, Mode, 0.0f, true, /*EffectiveMask=*/1.0f });
			const FVector3f Result = EvaluateFillLayers(Base, Inputs);
			TestTrue(FString::Printf(TEXT("LayerOpacity=0 passthrough, mode=%d"), (int32)Mode), Result.Equals(Base, VMF_Tolerance));
		}
		// bEnabled=false -> passthrough, identical to removing the layer.
		{
			TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
			Inputs.Add({ Fill, Mode, 1.0f, /*bEnabled=*/false, /*EffectiveMask=*/1.0f });
			const FVector3f Result = EvaluateFillLayers(Base, Inputs);
			const FVector3f ResultEmpty = EvaluateFillLayers(Base, TConstArrayView<FVertexMaskForgeLayerEvaluationInput>());
			TestTrue(FString::Printf(TEXT("Disabled layer passthrough, mode=%d"), (int32)Mode), Result.Equals(Base, VMF_Tolerance));
			TestTrue(FString::Printf(TEXT("Disabled layer == removed layer, mode=%d"), (int32)Mode), Result.Equals(ResultEmpty, VMF_Tolerance));
		}
	}

	return true;
}

// =================================================================================================
// F. Fill Layer -- numeric verification (Copy/Add/Multiply)
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerNumericTest, "VertexMaskForge.SequentialEvaluator.FillLayer.Numeric", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerNumericTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	const FVector3f Base(0.2f, 0.2f, 0.2f);
	const FVector3f Fill(1.0f, 1.0f, 1.0f);

	auto RunSingleLayer = [&](const EVertexMaskForgeBlendMode Mode) -> FVector3f
	{
		TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
		Inputs.Add({ Fill, Mode, /*Opacity=*/1.0f, true, /*EffectiveMask=*/0.5f });
		return EvaluateFillLayers(Base, Inputs);
	};

	TestTrue(TEXT("Copy: Base=0.2 Fill=1.0 Coverage=0.5 -> 0.6"), RunSingleLayer(EVertexMaskForgeBlendMode::Copy).Equals(FVector3f(0.6f, 0.6f, 0.6f), VMF_Tolerance));
	TestTrue(TEXT("Add: Base=0.2 Fill=1.0 Coverage=0.5 -> 0.7"), RunSingleLayer(EVertexMaskForgeBlendMode::Add).Equals(FVector3f(0.7f, 0.7f, 0.7f), VMF_Tolerance));
	TestTrue(TEXT("Multiply: Base=0.2 Fill=1.0 Coverage=0.5 -> 0.2"), RunSingleLayer(EVertexMaskForgeBlendMode::Multiply).Equals(FVector3f(0.2f, 0.2f, 0.2f), VMF_Tolerance));

	return true;
}

// =================================================================================================
// G. Fill Layer -- reorder sensitivity
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerReorderTest, "VertexMaskForge.SequentialEvaluator.FillLayer.Reorder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerReorderTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	const FVector3f Base(0.2f, 0.2f, 0.2f);
	const FVertexMaskForgeLayerEvaluationInput LayerA = { FVector3f(0.8f, 0.8f, 0.8f), EVertexMaskForgeBlendMode::Copy, 1.0f, true, 0.5f };
	const FVertexMaskForgeLayerEvaluationInput LayerB = { FVector3f(0.5f, 0.5f, 0.5f), EVertexMaskForgeBlendMode::Multiply, 1.0f, true, 1.0f };

	// Order A -> B: after A = 0.5, after B = 0.25.
	{
		TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
		Inputs.Add(LayerA);
		Inputs.Add(LayerB);
		TestTrue(TEXT("A then B -> 0.25"), EvaluateFillLayers(Base, Inputs).Equals(FVector3f(0.25f, 0.25f, 0.25f), VMF_Tolerance));
	}

	// Order B -> A: after B = 0.1, after A = 0.45.
	{
		TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
		Inputs.Add(LayerB);
		Inputs.Add(LayerA);
		TestTrue(TEXT("B then A -> 0.45"), EvaluateFillLayers(Base, Inputs).Equals(FVector3f(0.45f, 0.45f, 0.45f), VMF_Tolerance));
	}

	return true;
}

// =================================================================================================
// H. Fill Layer -- per-step clamp policy
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerClampTest, "VertexMaskForge.SequentialEvaluator.FillLayer.Clamp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerClampTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	// Base=0.8; Layer A: Add Fill=1, Coverage=1 -> clamps to 1.0; Layer B: Multiply Fill=0.5,
	// Coverage=1 -> 0.5 with per-step clamp (would be 0.9 without it).
	const FVector3f Base(0.8f, 0.8f, 0.8f);
	TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
	Inputs.Add({ FVector3f(1.0f, 1.0f, 1.0f), EVertexMaskForgeBlendMode::Add, 1.0f, true, 1.0f });
	Inputs.Add({ FVector3f(0.5f, 0.5f, 0.5f), EVertexMaskForgeBlendMode::Multiply, 1.0f, true, 1.0f });

	TestTrue(TEXT("Add-saturate then Multiply, per-layer clamp -> 0.5"), EvaluateFillLayers(Base, Inputs).Equals(FVector3f(0.5f, 0.5f, 0.5f), VMF_Tolerance));

	return true;
}

// =================================================================================================
// I. Legacy ComposeStack characterization -- protects byte/behavior parity across this checkpoint's
//    linkage-only change to ApplyMaskBlendMode/BlendMaskValueUnclamped. Values computed directly from
//    the unmodified, still stage-grouped ComposeStack algorithm -- never the new sequential contract.
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLegacyComposeStackCharacterizationTest, "VertexMaskForge.SequentialEvaluator.Legacy.ComposeStackCharacterization", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLegacyComposeStackCharacterizationTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeMaskStackComposer;

	const FVector4f Baseline(0.2f, 0.2f, 0.2f, 1.0f);
	const FVector4f Committed(0.2f, 0.2f, 0.2f, 1.0f);

	auto RunSingleLayer = [&](const EVertexMaskForgeBlendMode Mode, const float MaskValue, const float Opacity) -> float
	{
		TArray<FResolvedMaskLayer, TInlineAllocator<4>> Layers;
		Layers.Add({ Mode, Opacity, MaskValue });
		bool bAnyContributed = false;
		const FVector4f Result = ComposeStack(Baseline, Committed, Layers, true, true, true, bAnyContributed);
		return Result.X;
	};

	// Single mode, Base=0.2, Mask=0.75, Opacity=1, each of the 7 real Blend Modes.
	TestNearlyEqual(TEXT("Legacy Copy"), RunSingleLayer(EVertexMaskForgeBlendMode::Copy, 0.75f, 1.0f), 0.75f, VMF_Tolerance);
	TestNearlyEqual(TEXT("Legacy Add"), RunSingleLayer(EVertexMaskForgeBlendMode::Add, 0.75f, 1.0f), 0.95f, VMF_Tolerance);
	// Subtract's raw result (0.2-0.75=-0.55) is clamped to 0 by ComposeStack's own unconditional
	// post-Multiply-stage boundary clamp -- not -0.55.
	TestNearlyEqual(TEXT("Legacy Subtract (boundary-clamped)"), RunSingleLayer(EVertexMaskForgeBlendMode::Subtract, 0.75f, 1.0f), 0.0f, VMF_Tolerance);
	TestNearlyEqual(TEXT("Legacy Multiply"), RunSingleLayer(EVertexMaskForgeBlendMode::Multiply, 0.75f, 1.0f), 0.15f, VMF_Tolerance);
	TestNearlyEqual(TEXT("Legacy Overlay"), RunSingleLayer(EVertexMaskForgeBlendMode::Overlay, 0.75f, 1.0f), 0.3f, VMF_Tolerance);
	TestNearlyEqual(TEXT("Legacy Screen"), RunSingleLayer(EVertexMaskForgeBlendMode::Screen, 0.75f, 1.0f), 0.8f, VMF_Tolerance);
	TestNearlyEqual(TEXT("Legacy Linear"), RunSingleLayer(EVertexMaskForgeBlendMode::Linear, 0.75f, 1.0f), 0.6125f, VMF_Tolerance);

	// Partial opacity, Copy, Base=0.2, Mask=0.75, Opacity=0.5 -> 0.475.
	TestNearlyEqual(TEXT("Legacy Copy partial opacity"), RunSingleLayer(EVertexMaskForgeBlendMode::Copy, 0.75f, 0.5f), 0.475f, VMF_Tolerance);

	// Crosses more than one stage: Add(0.3,1.0) + Multiply(0.5,1.0), Base=0.2 -> 0.25 (Add stage
	// resolves to 0.5, then Multiply stage resolves 0.5*0.5=0.25 -- fixed stage order, list order
	// irrelevant here, which is exactly the legacy behavior this test protects).
	{
		TArray<FResolvedMaskLayer, TInlineAllocator<4>> Layers;
		Layers.Add({ EVertexMaskForgeBlendMode::Add, 1.0f, 0.3f });
		Layers.Add({ EVertexMaskForgeBlendMode::Multiply, 1.0f, 0.5f });
		bool bAnyContributed = false;
		const FVector4f Result = ComposeStack(Baseline, Committed, Layers, true, true, true, bAnyContributed);
		TestNearlyEqual(TEXT("Legacy multi-stage Add+Multiply"), Result.X, 0.25f, VMF_Tolerance);
		TestTrue(TEXT("Legacy multi-stage bAnyContributed"), bAnyContributed);
	}

	// Empty Layers -> passthrough (CommittedColor for RGB, BaselineColor.W for Alpha).
	{
		bool bAnyContributed = true;
		const FVector4f Result = ComposeStack(Baseline, Committed, TArrayView<const FResolvedMaskLayer>(), true, true, true, bAnyContributed);
		TestFalse(TEXT("Legacy empty Layers bAnyContributed"), bAnyContributed);
		TestTrue(TEXT("Legacy empty Layers passthrough"), Result.Equals(FVector4f(0.2f, 0.2f, 0.2f, 1.0f), VMF_Tolerance));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
